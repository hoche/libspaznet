// HTTP/2 (h2c, prior-knowledge cleartext) reactor dispatcher: the
// coroutine-free counterpart of dispatcher.cpp's serve(). Same protocol
// coverage (connection preface, SETTINGS exchange, multiplexed streams,
// HEADERS/CONTINUATION/DATA + HPACK, flow control, PING, GOAWAY,
// RST_STREAM — see that file's header comment for the full RFC 9113
// section list), same Frame/Settings/HPACK/Parser codec (codec.cpp,
// unchanged), same Handler interface — only the execution model differs.
//
// Concurrent multiplexing (RFC 9113 §5), reactor-style:
//   Where the coroutine dispatcher spawns a detached per-stream
//   coroutine and serializes writes through a writer_loop + out_queue,
//   Http2Connection calls Handler::handle_request() synchronously, right
//   from the frame-reading loop, for every stream as soon as its
//   HEADERS(+DATA) fully arrive. A handler that answers before returning
//   (writer.complete() called inline — the common case) writes its
//   frames immediately and costs the frame loop nothing but that one
//   synchronous call; a handler that defers just stores the
//   ResponseWriter and returns, and the frame loop keeps consuming
//   bytes — SETTINGS/PING/WINDOW_UPDATE and other streams' HEADERS/DATA
//   have zero dependency on any outstanding response. Every wire write
//   funnels through BufferedConnection::write(), which is what subsumes
//   the coroutine dispatcher's writer_loop/out_queue (see
//   buffered_connection.hpp's OutputBuffer comment) — no mutex, no
//   separately-scheduled writer, per-frame atomicity for free because
//   nothing here is ever suspended mid-write.
//
// Frame loop state machine:
//   Preface      -> waiting for the 24-byte client connection preface.
//   FrameHeader  -> waiting for the next 9-byte frame header.
//   FramePayload -> waiting for that frame's `length`-byte payload.
// All three just accumulate bytes in `buffer_` (mirrors
// example/http's Http1Connection's "insert new bytes, reparse" loop) and
// are driven from on_data(); nothing here suspends anything, so a frame
// that straddles several recv()s is handled the same way as one that
// arrives in a single read.
//
// Threading: unlike example/http's Http1Connection (at most one dispatch
// in flight per connection), HTTP/2 multiplexing means several
// ResponseWriters can be outstanding at once, and their completions can
// arrive concurrently, on arbitrary threads (a background thread, a
// timer, ...), while the frame-reading loop is itself mid-frame — the
// exact scenario that produced a real double-free crash under
// http2_showcase --reactor's concurrent /slow requests during this
// dispatcher's initial development (see CHANGELOG.md's "HTTP/2 reactor
// dispatcher" entry). The fix ended up NOT being a per-connection lock:
// only the IOContext's IO thread ever calls PlatformIO::wait() and
// invokes on_readable()/on_writable() (see IOContext::run()), so on_data()
// and on_closed() below are already single-threaded by construction.
// dispatch_request()'s ResponseWriter deliver callback is the one path
// that could otherwise land on a different thread; it routes the actual
// mutation through ctx_.post_to_io_thread(), which is a no-op queue hop
// when already on the IO thread (the overwhelmingly common
// completes-before-handle_request()-returns case) and marshals onto it
// otherwise. With every mutating entry point guaranteed single-threaded
// this way, there is no second thread left to race against and no lock
// is needed — see docs/concurrency-and-coroutines.md's threading
// section for the general primitive. assert(ctx_.is_io_thread()) below
// documents (and, in debug builds, enforces) that invariant rather than
// silently relying on it.
//
// One deliberate divergence from the coroutine dispatcher: several
// malformed-frame paths there (the WINDOW_UPDATE-length and
// frame-too-large checks) send a GOAWAY but don't explicitly close the
// socket, relying on the serve() coroutine unwinding to drop the last
// shared_ptr<ConnState> and close the fd via its destructor — which
// races the not-yet-scheduled writer_loop coroutine and can truncate
// that very GOAWAY. Http2Connection always calls close_after_flush() on
// every fatal path instead, guaranteeing delivery (the same fix
// example/http's reactor dispatcher applies to its own error responses).
// Not believed to be observable by any existing test — neither
// dispatcher's differential suite sends a malformed WINDOW_UPDATE or an
// oversized frame — but noted here in case that ever changes.

#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/reactor/buffered_connection.hpp>
#include <libspaznet/server.hpp>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spaznet::http2 {

namespace {

constexpr const char* kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kPrefaceLen = 24;
constexpr std::uint32_t kInitialWindow = 65535;
constexpr std::uint32_t kMaxFrameSizeDefault = 16384;

// Resource caps — see dispatcher.cpp's identical constants for the
// attacker-controlled-accumulation rationale (CONTINUATION flood /
// unbounded body buffering).
constexpr std::size_t kMaxHeaderListBytes = 64ULL * 1024;
constexpr std::size_t kMaxBodyBytes = 64ULL * 1024 * 1024;
constexpr std::uint32_t kMaxConcurrentStreams = 100;
constexpr std::int64_t kMaxFlowControlWindow = 0x7FFFFFFF;

// Per-stream state, scoped to the connection's streams_ map. Once
// END_STREAM is seen we extract the request data and erase the stream;
// dispatch_request() owns the Request by value from then on.
struct Stream {
    std::uint32_t id{};
    bool headers_received{false};
    bool end_stream_received{false};
    std::int64_t recv_window{kInitialWindow};
    std::int64_t send_window{kInitialWindow};
    std::unordered_map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
    // Accumulates a HEADERS payload across CONTINUATION frames before we
    // hand it to HPACK::decode_headers.
    std::vector<std::uint8_t> pending_hpack;
};

class Http2Connection : public std::enable_shared_from_this<Http2Connection> {
  public:
    Http2Connection(::spaznet::IOContext& ctx, std::weak_ptr<::spaznet::BufferedConnection> conn,
                    std::shared_ptr<Handler> handler)
        : ctx_(ctx), conn_(std::move(conn)), handler_(std::move(handler)) {}

    // Wires this dispatcher onto `conn` (captured weak here, captured
    // strong in conn's own callbacks — same reference-cycle-avoidance
    // direction as example/http's Http1Connection) and starts the read
    // loop. Call exactly once, immediately after construction.
    void start(std::function<void()> on_closed) {
        notify_closed_ = std::move(on_closed);
        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        auto self = shared_from_this();
        conn->set_on_data([self]() { self->on_data(); });
        conn->set_on_closed([self]() { self->on_closed(); });
        conn->start();
    }

  private:
    enum class Phase : std::uint8_t { Preface, FrameHeader, FramePayload };

    // BufferedConnection::on_data: fires once per successful recv(), so
    // it may run several times back-to-back for one readable event.
    // Just accumulate; try_process() below reparses buffer_ from
    // wherever the state machine currently is.
    void on_data() {
        assert(ctx_.is_io_thread());
        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        auto data = conn->input().data();
        buffer_.insert(buffer_.end(), data.begin(), data.end());
        conn->input().consume(data.size());

        if (closing_) {
            return;
        }
        try_process();
    }

    // Fires exactly once, whenever BufferedConnection is done (orderly
    // EOF, hard error, or our own close()/close_after_flush() call
    // completing). Unlike a fatal protocol error we detect ourselves,
    // there's no point sending a GOAWAY here — the peer is already gone.
    void on_closed() {
        assert(ctx_.is_io_thread());
        closing_ = true;
        if (notify_closed_) {
            auto cb = std::move(notify_closed_);
            notify_closed_ = nullptr;
            cb();
        }
    }

    // Consumes as much of buffer_ as the current phase allows, advancing
    // through Preface -> FrameHeader -> FramePayload -> FrameHeader -> ...
    // Returns (waiting for more bytes via the next on_data()) whenever
    // buffer_ doesn't yet hold enough for the current phase, and stops
    // immediately if a fatal condition sets closing_.
    void try_process() {
        while (!closing_) {
            auto conn = conn_.lock();
            if (!conn || conn->closed()) {
                return;
            }
            switch (phase_) {
                case Phase::Preface: {
                    if (buffer_.size() < kPrefaceLen) {
                        return;
                    }
                    if (std::memcmp(buffer_.data(), kPreface, kPrefaceLen) != 0) {
                        closing_ = true;
                        conn->close();
                        return;
                    }
                    buffer_.erase(buffer_.begin(),
                                  buffer_.begin() + static_cast<std::ptrdiff_t>(kPrefaceLen));

                    // Header-table-size = 0 keeps the peer from indexing
                    // against a dynamic table on their side (we don't
                    // maintain one).
                    our_settings_.header_table_size = 0;
                    our_settings_.enable_push = false;
                    our_settings_.initial_window_size = kInitialWindow;
                    our_settings_.max_frame_size = kMaxFrameSizeDefault;
                    our_settings_.max_concurrent_streams = kMaxConcurrentStreams;
                    write_frame(Parser::build_settings_frame(our_settings_, false));

                    phase_ = Phase::FrameHeader;
                    break;
                }
                case Phase::FrameHeader: {
                    if (buffer_.size() < 9) {
                        return;
                    }
                    const std::uint32_t length = (static_cast<std::uint32_t>(buffer_[0]) << 16) |
                                                  (static_cast<std::uint32_t>(buffer_[1]) << 8) |
                                                  static_cast<std::uint32_t>(buffer_[2]);
                    const FrameType type = static_cast<FrameType>(buffer_[3]);
                    const std::uint8_t flags = buffer_[4];
                    const std::uint32_t stream_id =
                        ((static_cast<std::uint32_t>(buffer_[5]) << 24) |
                         (static_cast<std::uint32_t>(buffer_[6]) << 16) |
                         (static_cast<std::uint32_t>(buffer_[7]) << 8) |
                         static_cast<std::uint32_t>(buffer_[8])) &
                        0x7FFFFFFFU;
                    buffer_.erase(buffer_.begin(), buffer_.begin() + 9);

                    if (length > our_settings_.max_frame_size) {
                        fatal(/*FRAME_SIZE_ERROR*/ 0x6);
                        return;
                    }
                    if (length == 0) {
                        handle_frame(type, flags, stream_id, {});
                        if (closing_) {
                            return;
                        }
                        break; // still FrameHeader — nothing to read for this one.
                    }
                    cur_length_ = length;
                    cur_type_ = type;
                    cur_flags_ = flags;
                    cur_stream_id_ = stream_id;
                    phase_ = Phase::FramePayload;
                    break;
                }
                case Phase::FramePayload: {
                    if (buffer_.size() < cur_length_) {
                        return;
                    }
                    std::vector<std::uint8_t> payload(
                        buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(cur_length_));
                    buffer_.erase(buffer_.begin(),
                                  buffer_.begin() + static_cast<std::ptrdiff_t>(cur_length_));

                    // Connection-level flow-control accounting on DATA
                    // frames, mirroring dispatcher.cpp exactly.
                    if (cur_type_ == FrameType::DATA) {
                        conn_recv_window_ -= static_cast<std::int64_t>(cur_length_);
                    }

                    phase_ = Phase::FrameHeader;
                    handle_frame(cur_type_, cur_flags_, cur_stream_id_, std::move(payload));
                    if (closing_) {
                        return;
                    }
                    break;
                }
            }
        }
    }

    // Handles one fully-buffered frame. Mirrors dispatcher.cpp's switch
    // statement case-for-case; see there for the RFC 9113 citations this
    // enforces.
    void handle_frame(FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                      std::vector<std::uint8_t> payload) {
        switch (type) {
            case FrameType::SETTINGS: {
                if (stream_id != 0) {
                    fatal(/*PROTOCOL_ERROR*/ 0x1);
                    return;
                }
                if ((flags & Flags::ACK) != 0) {
                    if (!payload.empty()) {
                        fatal(/*FRAME_SIZE_ERROR*/ 0x6);
                    }
                    return;
                }
                if (payload.size() % 6 != 0) {
                    fatal(/*FRAME_SIZE_ERROR*/ 0x6);
                    return;
                }
                // Cumulative update: only the parameters present in this
                // frame change; the rest keep their prior values.
                Settings::parse_into(payload, peer_settings_);
                write_frame(Parser::build_settings_frame(our_settings_, /*ack=*/true));
                return;
            }
            case FrameType::PING: {
                if (stream_id != 0) {
                    fatal(/*PROTOCOL_ERROR*/ 0x1);
                    return;
                }
                if (payload.size() != 8) {
                    fatal(/*FRAME_SIZE_ERROR*/ 0x6);
                    return;
                }
                if ((flags & Flags::ACK) != 0) {
                    return; // pong — done.
                }
                write_frame(Parser::build_ping_frame(payload, /*ack=*/true));
                return;
            }
            case FrameType::WINDOW_UPDATE: {
                if (payload.size() != 4) {
                    fatal(/*FRAME_SIZE_ERROR*/ 0x6);
                    return;
                }
                const std::uint32_t inc = ((static_cast<std::uint32_t>(payload[0]) << 24) |
                                           (static_cast<std::uint32_t>(payload[1]) << 16) |
                                           (static_cast<std::uint32_t>(payload[2]) << 8) |
                                           static_cast<std::uint32_t>(payload[3])) &
                                          0x7FFFFFFFU;
                // RFC 9113 §6.9: a 0 increment is a PROTOCOL_ERROR
                // (connection error on stream 0, stream error otherwise).
                if (inc == 0) {
                    if (stream_id == 0) {
                        fatal(/*PROTOCOL_ERROR*/ 0x1);
                        return;
                    }
                    rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                    return;
                }
                if (stream_id == 0) {
                    if (conn_send_window_ + static_cast<std::int64_t>(inc) > kMaxFlowControlWindow) {
                        fatal(/*FLOW_CONTROL_ERROR*/ 0x3);
                        return;
                    }
                    conn_send_window_ += static_cast<std::int64_t>(inc);
                } else {
                    // Only adjust an existing stream; a WINDOW_UPDATE for
                    // an unknown id must not spawn a phantom stream
                    // (map-growth DoS). Idle/closed streams are simply
                    // ignored here.
                    auto it = streams_.find(stream_id);
                    if (it == streams_.end()) {
                        return;
                    }
                    if (it->second.send_window + static_cast<std::int64_t>(inc) >
                        kMaxFlowControlWindow) {
                        rst_stream(stream_id, /*FLOW_CONTROL_ERROR*/ 0x3);
                        return;
                    }
                    it->second.send_window += static_cast<std::int64_t>(inc);
                }
                return;
            }
            case FrameType::RST_STREAM: {
                if (stream_id == 0) {
                    fatal(/*PROTOCOL_ERROR*/ 0x1);
                    return;
                }
                if (payload.size() != 4) {
                    fatal(/*FRAME_SIZE_ERROR*/ 0x6);
                    return;
                }
                // Peer reset the stream: reap it to bound memory.
                streams_.erase(stream_id);
                return;
            }
            case FrameType::GOAWAY: {
                // Peer is closing — drain whatever's already queued, then
                // follow suit.
                send_goaway(0);
                closing_ = true;
                {
                    auto conn = conn_.lock();
                    if (conn) {
                        conn->close_after_flush();
                    }
                }
                return;
            }
            case FrameType::HEADERS: {
                if (stream_id == 0) {
                    fatal(/*PROTOCOL_ERROR*/ 0x1);
                    return;
                }
                // RFC 9113 §5.1.2: refuse a new stream beyond the limit
                // we advertised, bounding concurrent stream state.
                if (streams_.find(stream_id) == streams_.end() &&
                    streams_.size() >= kMaxConcurrentStreams) {
                    rst_stream(stream_id, /*REFUSED_STREAM*/ 0x7);
                    return;
                }
                auto& s = streams_[stream_id];
                s.id = stream_id;
                s.headers_received = true;
                std::size_t off = 0;
                std::size_t end_off = payload.size();
                if ((flags & Flags::PADDED) != 0 && off < end_off) {
                    const std::uint8_t pad_len = payload[off++];
                    if (pad_len >= end_off - off) {
                        rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        return;
                    }
                    end_off -= pad_len;
                }
                if ((flags & Flags::PRIORITY) != 0) {
                    if (off + 5 > end_off) {
                        rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        return;
                    }
                    off += 5;
                }
                s.pending_hpack.assign(payload.begin() + static_cast<std::ptrdiff_t>(off),
                                       payload.begin() + static_cast<std::ptrdiff_t>(end_off));

                const bool end_headers = (flags & Flags::END_HEADERS) != 0;
                const bool end_stream = (flags & Flags::END_STREAM) != 0;
                if (end_stream) {
                    s.end_stream_received = true;
                }

                if (end_headers) {
                    s.headers = HPACK::decode_headers(s.pending_hpack);
                    s.pending_hpack.clear();
                    if (end_stream) {
                        handoff_request(s);
                        streams_.erase(stream_id);
                    }
                }
                return;
            }
            case FrameType::CONTINUATION: {
                auto it = streams_.find(stream_id);
                if (it == streams_.end() || !it->second.headers_received) {
                    fatal(/*PROTOCOL_ERROR*/ 0x1);
                    return;
                }
                auto& s = it->second;
                // Bound the header block: a CONTINUATION flood that never
                // sets END_HEADERS would otherwise grow pending_hpack
                // without limit (RFC 9113 CONTINUATION-flood DoS,
                // CVE-2024-27316 class).
                if (s.pending_hpack.size() + payload.size() > kMaxHeaderListBytes) {
                    fatal(/*ENHANCE_YOUR_CALM*/ 0xb);
                    return;
                }
                s.pending_hpack.insert(s.pending_hpack.end(), payload.begin(), payload.end());
                if ((flags & Flags::END_HEADERS) != 0) {
                    s.headers = HPACK::decode_headers(s.pending_hpack);
                    s.pending_hpack.clear();
                    if (s.end_stream_received) {
                        handoff_request(s);
                        streams_.erase(stream_id);
                    }
                }
                return;
            }
            case FrameType::DATA: {
                auto it = streams_.find(stream_id);
                if (it == streams_.end() || !it->second.headers_received) {
                    fatal(/*PROTOCOL_ERROR*/ 0x1);
                    return;
                }
                auto& s = it->second;
                std::size_t off = 0;
                std::size_t end_off = payload.size();
                if ((flags & Flags::PADDED) != 0 && off < end_off) {
                    const std::uint8_t pad_len = payload[off++];
                    if (pad_len >= end_off - off) {
                        rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        return;
                    }
                    end_off -= pad_len;
                }
                // Bound the buffered body — see dispatcher.cpp's
                // identical check for why flow control alone doesn't
                // provide this backpressure.
                if (s.body.size() + (end_off - off) > kMaxBodyBytes) {
                    rst_stream(stream_id, /*ENHANCE_YOUR_CALM*/ 0xb);
                    streams_.erase(stream_id);
                    return;
                }
                s.body.insert(s.body.end(), payload.begin() + static_cast<std::ptrdiff_t>(off),
                              payload.begin() + static_cast<std::ptrdiff_t>(end_off));
                s.recv_window -= static_cast<std::int64_t>(end_off - off);

                const auto frame_length = static_cast<std::uint32_t>(payload.size());
                if (frame_length > 0) {
                    write_frame(Parser::build_window_update_frame(0, frame_length));
                    write_frame(Parser::build_window_update_frame(stream_id, frame_length));
                }

                if ((flags & Flags::END_STREAM) != 0) {
                    s.end_stream_received = true;
                    handoff_request(s);
                    streams_.erase(stream_id);
                }
                return;
            }
            case FrameType::PRIORITY:
            case FrameType::PUSH_PROMISE:
            default:
                // Ignore unknown / unsupported frame types per RFC 9113
                // §4.1: "implementations MUST discard frames that have
                // unknown or unsupported types."
                return;
        }
    }

    void handoff_request(Stream& s) {
        Request req;
        req.stream_id = s.id;
        req.headers = std::move(s.headers);
        req.body = std::move(s.body);
        auto m = req.headers.find(":method");
        auto p = req.headers.find(":path");
        if (m != req.headers.end()) {
            req.method = m->second;
        }
        if (p != req.headers.end()) {
            req.path = p->second;
        }
        last_stream_id_handled_ = s.id;
        dispatch_request(std::move(req));
    }

    // Calls the user's handler synchronously for one fully-arrived
    // request. Unlike example/http's Http1Connection (one dispatch in
    // flight per connection, HTTP/1.1 responses must be ordered), any
    // number of these can be outstanding at once — that's HTTP/2
    // multiplexing — each tracked only by the ResponseWriter capturing
    // its own stream_id and a shared_ptr back to this connection.
    void dispatch_request(Request req) {
        ctx_.increment_active_requests();
        const std::uint32_t stream_id = req.stream_id;
        auto self = shared_from_this();
        // deliver() may run on any thread — see the file header comment.
        // post_to_io_thread() guarantees on_response_ready() always runs
        // on this connection's IO thread, inline immediately if we're
        // already there (the common synchronous-completion case).
        ResponseWriter writer([self, stream_id](Response response) {
            self->ctx_.post_to_io_thread([self, stream_id, response = std::move(response)]() mutable {
                self->on_response_ready(stream_id, std::move(response));
            });
        });

        try {
            handler_->handle_request(req, writer);
        } catch (...) {
            // Match dispatch_request's coroutine counterpart: discard
            // whatever the handler built and reset the stream instead.
            // Guard against a handler that both completed *and* threw
            // (contract violation, but let's not double-account or
            // double-respond if it happens).
            if (!writer.is_completed()) {
                ctx_.decrement_active_requests();
                write_frame(Parser::build_rst_stream_frame(stream_id, /*INTERNAL_ERROR*/ 0x2));
            }
        }
    }

    // Always runs on the IO thread (see dispatch_request()'s
    // ResponseWriter construction) — but still either synchronously
    // (nested inside handle_request(), itself nested inside
    // handle_frame()'s HEADERS/CONTINUATION/DATA case, itself nested
    // inside try_process()'s while loop) or asynchronously, arbitrarily
    // later.
    void on_response_ready(std::uint32_t stream_id, Response response) {
        assert(ctx_.is_io_thread());
        ctx_.decrement_active_requests();
        auto conn = conn_.lock();
        if (!conn || closing_) {
            return; // Connection is already gone; nothing left to send.
        }

        response.stream_id = stream_id;
        response.headers[":status"] = std::to_string(response.status_code);
        if (response.headers.find("content-length") == response.headers.end() &&
            !response.body.empty()) {
            response.headers["content-length"] = std::to_string(response.body.size());
        }
        const std::uint32_t max_chunk =
            peer_settings_.max_frame_size > 0 ? peer_settings_.max_frame_size : kMaxFrameSizeDefault;
        for (const auto& frame : response.to_frames(max_chunk)) {
            conn->write(frame.serialize());
        }
    }

    void write_frame(const Frame& frame) {
        auto conn = conn_.lock();
        if (conn) {
            conn->write(frame.serialize());
        }
    }

    void send_goaway(std::uint32_t error_code) {
        if (goaway_sent_) {
            return;
        }
        goaway_sent_ = true;
        write_frame(Parser::build_goaway_frame(last_stream_id_handled_, error_code));
    }

    // Connection-level protocol error: send GOAWAY and tear down once
    // it's flushed. See the file header comment for why this always
    // closes, unlike a couple of the coroutine dispatcher's equivalent
    // paths.
    void fatal(std::uint32_t error_code) {
        send_goaway(error_code);
        closing_ = true;
        auto conn = conn_.lock();
        if (conn) {
            conn->close_after_flush();
        }
    }

    // Stream-level error: reset just this stream, connection stays up.
    void rst_stream(std::uint32_t sid, std::uint32_t error_code) {
        write_frame(Parser::build_rst_stream_frame(sid, error_code));
        // Reap the stream: once we've reset it we won't process it
        // further, so drop it from the live map to bound memory (a peer
        // that opens and resets streams in a loop would otherwise grow
        // it unboundedly).
        streams_.erase(sid);
    }

    ::spaznet::IOContext& ctx_;
    std::weak_ptr<::spaznet::BufferedConnection> conn_;
    std::shared_ptr<Handler> handler_;
    std::vector<std::uint8_t> buffer_;
    Phase phase_ = Phase::Preface;
    bool closing_ = false;

    // Parsed frame header awaiting its payload (Phase::FramePayload).
    std::uint32_t cur_length_{};
    FrameType cur_type_{};
    std::uint8_t cur_flags_{};
    std::uint32_t cur_stream_id_{};

    Settings peer_settings_; // defaults from RFC 9113 §6.5.2
    Settings our_settings_;  // advertised once the preface checks out
    std::int64_t conn_send_window_{kInitialWindow};
    std::int64_t conn_recv_window_{kInitialWindow};
    std::uint32_t last_stream_id_handled_{0};
    bool goaway_sent_{false};
    std::map<std::uint32_t, Stream> streams_;

    std::function<void()> notify_closed_;
};

} // namespace

auto make_reactor_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::ConnectionFactory {
    std::shared_ptr<Handler> shared(handler.release());
    return [shared](int fd, ::spaznet::IOContext& ctx,
                    std::function<void()> on_closed) -> std::shared_ptr<::spaznet::IoHandler> {
        auto conn = std::make_shared<::spaznet::BufferedConnection>(ctx, fd);
        auto dispatcher = std::make_shared<Http2Connection>(ctx, conn, shared);
        dispatcher->start(std::move(on_closed));
        return conn;
    };
}

} // namespace spaznet::http2
