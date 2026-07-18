// HTTP/2 (h2c, prior-knowledge cleartext) ConnectionHandler.
//
// Implements RFC 9113 §3.4 "HTTP/2 over cleartext TCP" plus enough of
// the rest of RFC 9113 to interop with `curl --http2-prior-knowledge`
// and `nghttp -nv` against a single-port server:
//
//   - Connection preface (§3.4)
//   - SETTINGS exchange + ACK (§6.5)
//   - Multiplexed streams keyed by 31-bit stream ID (§5.1)
//   - HEADERS / CONTINUATION (§6.2, §6.10) → user's handle_request
//   - DATA frame request body assembly (§6.1)
//   - HEADERS + DATA response emission, chunked by peer's
//     SETTINGS_MAX_FRAME_SIZE (§4.2)
//   - Per-stream + connection-level flow control windows (§5.2)
//     and WINDOW_UPDATE both inbound and outbound (§6.9)
//   - PING / PING-ACK (§6.7)
//   - GOAWAY emission on shutdown (§6.8)
//   - RST_STREAM on stream-level errors (§6.4)
//
// Concurrent multiplexing (RFC 9113 §5):
//   Each fully-arrived request is dispatched as a *detached*
//   coroutine via `IOContext::schedule`.  The frame-reading loop
//   keeps making progress regardless of how long a handler runs, so
//   a slow request on stream A doesn't stall PING-ACK / WINDOW_UPDATE
//   traffic or hold up handlers for streams B, C, D.  All wire writes
//   funnel through a single per-connection writer coroutine: producers
//   (the frame loop, every handler) append a fully-built frame's
//   bytes to the connection's write queue under a `std::mutex`, and
//   the writer drains the queue one frame at a time.  That gives us
//   per-frame atomicity (no two `send()` calls from different
//   coroutines can interleave mid-frame) while still allowing
//   frame-level interleaving across streams — which is exactly what
//   §5 allows once HEADERS / CONTINUATION sequences carry
//   END_HEADERS in a single HEADERS frame, as ours always do.
//
// Not implemented:
//   - PUSH_PROMISE / server push
//   - Priority frames (deprecated in 9113)
//   - Dynamic HPACK table (we advertise SETTINGS_HEADER_TABLE_SIZE = 0
//     so peers don't compress against their own dynamic table)
//   - Trailers
//   - h2 over TLS (run a TLS terminator in front)

#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/server.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace spaznet::http2 {

namespace {

constexpr const char* kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kPrefaceLen = 24;
constexpr std::uint32_t kInitialWindow = 65535;
constexpr std::uint32_t kMaxFrameSizeDefault = 16384;

// Resource caps on attacker-controlled accumulation. Without these, a peer
// can stream CONTINUATION frames that never set END_HEADERS (unbounded
// `pending_hpack`) or DATA frames that never set END_STREAM (unbounded
// `body`) and exhaust server memory — the receive window is replenished on
// every DATA frame, so flow control alone provides no backpressure.
constexpr std::size_t kMaxHeaderListBytes = 64ULL * 1024;      // compressed HEADERS+CONTINUATION block
constexpr std::size_t kMaxBodyBytes = 64ULL * 1024 * 1024;     // total buffered request body

// Per-stream state, scoped to the frame loop coroutine.  Once
// END_STREAM is seen we extract the request data and erase the
// stream; the spawned dispatch_request coroutine owns the Request by
// value from then on.
struct Stream {
    std::uint32_t id{};
    bool headers_received{false};
    bool end_stream_received{false};
    bool reset{false};
    std::int64_t recv_window{kInitialWindow};
    std::int64_t send_window{kInitialWindow};
    std::unordered_map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
    // Accumulates a HEADERS payload across CONTINUATION frames before
    // we hand it to HPACK::decode_headers.
    std::vector<std::uint8_t> pending_hpack;
};

// Per-connection state, heap-allocated and shared between the
// frame-reading coroutine, every dispatched handler, and the writer.
// Lifetime is governed by std::shared_ptr: when the frame loop ends
// AND every detached handler is done AND the writer has drained, the
// last shared_ptr drops and the embedded Socket closes the fd.
struct ConnState {
    ::spaznet::Socket socket;
    Handler& handler;

    // `mu_` covers everything below it, including the writer's queue.
    // We keep it coarse on purpose: the frame loop, handlers, and the
    // writer never spend wall-clock time inside the critical section
    // (just bookkeeping), so contention is negligible.
    std::mutex mu;

    Settings peer_settings;          // defaults from RFC 9113 §6.5.2
    Settings our_settings;           // advertised at the start
    std::int64_t conn_send_window{kInitialWindow};
    std::int64_t conn_recv_window{kInitialWindow};
    std::uint32_t last_stream_id_handled{0};
    bool goaway_sent{false};
    bool closed{false};

    // Serialized outbound queue.  Producers append serialized frame
    // bytes; the lone writer coroutine drains.  A writer is spawned
    // lazily — `writer_running` ensures at most one writer exists at
    // a time, so async_write yield-points don't interleave with each
    // other.
    std::deque<std::vector<std::uint8_t>> out_queue;
    bool writer_running{false};

    ConnState(::spaznet::Socket s, Handler& h)
        : socket(std::move(s)), handler(h) {}
};

// Read exactly `n` bytes; returns false on EOF/error.
auto read_exact(::spaznet::Socket& socket, std::size_t n, std::vector<std::uint8_t>& out)
    -> ::spaznet::Task {
    out.clear();
    out.reserve(n);
    std::vector<std::uint8_t> chunk;
    while (out.size() < n) {
        co_await socket.async_read(chunk, n - out.size());
        if (chunk.empty()) co_return;
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
}

// Compose a goaway frame body: last_stream_id (4) + error_code (4).
auto goaway_payload(std::uint32_t last_stream, std::uint32_t error_code)
    -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> p(8);
    p[0] = static_cast<std::uint8_t>((last_stream >> 24) & 0x7F);
    p[1] = static_cast<std::uint8_t>((last_stream >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((last_stream >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(last_stream & 0xFF);
    p[4] = static_cast<std::uint8_t>((error_code >> 24) & 0xFF);
    p[5] = static_cast<std::uint8_t>((error_code >> 16) & 0xFF);
    p[6] = static_cast<std::uint8_t>((error_code >> 8) & 0xFF);
    p[7] = static_cast<std::uint8_t>(error_code & 0xFF);
    return p;
}

// Forward decl — defined after enqueue_frame so it can reference back.
auto writer_loop(std::shared_ptr<ConnState> state) -> ::spaznet::Task;

// Append a fully-serialized frame to the connection's write queue,
// spawning a writer coroutine if one isn't already draining.  Safe to
// call from any coroutine on the connection (frame loop and every
// detached handler share this path).
auto enqueue_frame(const std::shared_ptr<ConnState>& state, Frame frame) -> void {
    frame.length = static_cast<std::uint32_t>(frame.payload.size());
    std::vector<std::uint8_t> bytes = frame.serialize();
    bool need_spawn = false;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        if (state->closed) return;
        state->out_queue.push_back(std::move(bytes));
        if (!state->writer_running) {
            state->writer_running = true;
            need_spawn = true;
        }
    }
    if (need_spawn) {
        state->socket.context()->schedule(writer_loop(state));
    }
}

auto writer_loop(std::shared_ptr<ConnState> state) -> ::spaznet::Task {
    while (true) {
        std::vector<std::uint8_t> bytes;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            if (state->out_queue.empty()) {
                // Nothing left to do — clear `writer_running` so the
                // next producer spawns a fresh writer.  Inside the
                // lock to avoid the racy "queue looked empty, then
                // someone enqueued, then we returned without
                // spawning a new writer" window.
                state->writer_running = false;
                co_return;
            }
            bytes = std::move(state->out_queue.front());
            state->out_queue.pop_front();
        }
        // Critically — `mu` is NOT held across this co_await.  The
        // writer is the only producer-of-bytes-to-the-socket, so
        // even though async_write yields internally we know no other
        // coroutine is calling async_write concurrently on this
        // socket (`writer_running` is the per-connection mutex
        // ensuring at most one writer).
        co_await state->socket.async_write(std::move(bytes));
    }
}

// Single dispatched request — runs as a detached coroutine on the
// IOContext.  Owns its `Request` by value; never touches the
// per-connection streams map.
auto dispatch_request(std::shared_ptr<ConnState> state, Request req) -> ::spaznet::Task {
    state->socket.context()->increment_active_requests();

    Response resp;
    resp.stream_id = req.stream_id;
    resp.status_code = DEFAULT_HTTP_STATUS_CODE;

    bool errored = false;
    try {
        co_await state->handler.handle_request(req, resp, state->socket);
    } catch (...) {
        errored = true;
    }

    if (errored) {
        Frame rst = Parser::build_rst_stream_frame(req.stream_id, /*INTERNAL_ERROR*/ 0x2);
        enqueue_frame(state, std::move(rst));
        state->socket.context()->decrement_active_requests();
        co_return;
    }

    // Build HEADERS — RFC 9113 §8.1.2.1 wants :status before regular
    // headers; build_headers_frame already handles ordering.  We
    // also fill in content-length for non-empty bodies.
    resp.headers[":status"] = std::to_string(resp.status_code);
    if (resp.headers.find("content-length") == resp.headers.end() && !resp.body.empty()) {
        resp.headers["content-length"] = std::to_string(resp.body.size());
    }
    const bool empty_body = resp.body.empty();
    Frame headers_frame = Parser::build_headers_frame(resp, req.stream_id,
                                                       /*end_headers=*/true,
                                                       /*end_stream=*/empty_body);
    enqueue_frame(state, std::move(headers_frame));

    if (!empty_body) {
        std::size_t max_chunk;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            max_chunk = state->peer_settings.max_frame_size > 0
                            ? state->peer_settings.max_frame_size
                            : kMaxFrameSizeDefault;
        }
        std::size_t off = 0;
        while (off < resp.body.size()) {
            const std::size_t take = std::min(max_chunk, resp.body.size() - off);
            std::vector<std::uint8_t> chunk(resp.body.begin() + off,
                                            resp.body.begin() + off + take);
            const bool end = (off + take == resp.body.size());
            Frame f = Parser::build_data_frame(req.stream_id, chunk, end);
            enqueue_frame(state, std::move(f));
            off += take;
        }
    }

    state->socket.context()->decrement_active_requests();
}

auto serve(::spaznet::Socket socket, Handler& handler) -> ::spaznet::Task {
    auto state = std::make_shared<ConnState>(std::move(socket), handler);

    // 1. Connection preface (§3.4).
    std::vector<std::uint8_t> buffer;
    co_await read_exact(state->socket, kPrefaceLen, buffer);
    if (buffer.size() != kPrefaceLen ||
        std::memcmp(buffer.data(), kPreface, kPrefaceLen) != 0) {
        state->socket.close();
        co_return;
    }

    // 2. Send our SETTINGS.  Header-table-size = 0 keeps the peer from
    // indexing against a dynamic table on their side (we don't
    // maintain one).
    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->our_settings.header_table_size = 0;
        state->our_settings.enable_push = false;
        state->our_settings.initial_window_size = kInitialWindow;
        state->our_settings.max_frame_size = kMaxFrameSizeDefault;
        state->our_settings.max_concurrent_streams = 100;
    }
    enqueue_frame(state, Parser::build_settings_frame(state->our_settings, false));

    // 3. Frame loop.
    std::map<std::uint32_t, Stream> streams;

    auto rst_stream = [&](std::uint32_t sid, std::uint32_t error_code) {
        enqueue_frame(state, Parser::build_rst_stream_frame(sid, error_code));
        auto it = streams.find(sid);
        if (it != streams.end()) it->second.reset = true;
    };

    auto send_goaway = [&](std::uint32_t error_code) {
        std::uint32_t last;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            if (state->goaway_sent) return;
            state->goaway_sent = true;
            last = state->last_stream_id_handled;
        }
        Frame f;
        f.type = FrameType::GOAWAY;
        f.stream_id = 0;
        f.payload = goaway_payload(last, error_code);
        enqueue_frame(state, std::move(f));
    };

    auto handoff_request = [&](Stream& s) {
        Request req;
        req.stream_id = s.id;
        req.headers = std::move(s.headers);
        req.body = std::move(s.body);
        auto m = req.headers.find(":method");
        auto p = req.headers.find(":path");
        if (m != req.headers.end()) req.method = m->second;
        if (p != req.headers.end()) req.path = p->second;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_stream_id_handled = s.id;
        }
        state->socket.context()->schedule(dispatch_request(state, std::move(req)));
    };

    std::uint32_t our_max_frame_size;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        our_max_frame_size = state->our_settings.max_frame_size;
    }

    while (true) {
        // Read 9-byte frame header.
        std::vector<std::uint8_t> hdr;
        co_await read_exact(state->socket, 9, hdr);
        if (hdr.size() != 9) break;
        const std::uint32_t length =
            (static_cast<std::uint32_t>(hdr[0]) << 16) |
            (static_cast<std::uint32_t>(hdr[1]) << 8) | static_cast<std::uint32_t>(hdr[2]);
        const FrameType type = static_cast<FrameType>(hdr[3]);
        const std::uint8_t flags = hdr[4];
        const std::uint32_t stream_id =
            ((static_cast<std::uint32_t>(hdr[5]) << 24) |
             (static_cast<std::uint32_t>(hdr[6]) << 16) |
             (static_cast<std::uint32_t>(hdr[7]) << 8) |
             static_cast<std::uint32_t>(hdr[8])) &
            0x7FFFFFFFU;

        if (length > our_max_frame_size) {
            send_goaway(/*FRAME_SIZE_ERROR*/ 0x6);
            break;
        }
        std::vector<std::uint8_t> payload;
        if (length > 0) {
            co_await read_exact(state->socket, length, payload);
            if (payload.size() != length) break;
        }

        // Connection-level flow-control accounting on DATA frames.
        if (type == FrameType::DATA) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->conn_recv_window -= static_cast<std::int64_t>(length);
        }

        switch (type) {
            case FrameType::SETTINGS: {
                if ((flags & Flags::ACK) != 0) break;
                Settings parsed = Settings::parse(payload);
                {
                    std::lock_guard<std::mutex> lk(state->mu);
                    state->peer_settings = parsed;
                }
                Frame ack;
                ack.type = FrameType::SETTINGS;
                ack.flags = Flags::ACK;
                ack.stream_id = 0;
                enqueue_frame(state, std::move(ack));
                break;
            }
            case FrameType::PING: {
                if ((flags & Flags::ACK) != 0) break; // pong — done
                Frame pong;
                pong.type = FrameType::PING;
                pong.flags = Flags::ACK;
                pong.stream_id = 0;
                pong.payload = std::move(payload);
                enqueue_frame(state, std::move(pong));
                break;
            }
            case FrameType::WINDOW_UPDATE: {
                if (payload.size() != 4) {
                    send_goaway(/*FRAME_SIZE_ERROR*/ 0x6);
                    goto loop_end;
                }
                const std::uint32_t inc =
                    ((static_cast<std::uint32_t>(payload[0]) << 24) |
                     (static_cast<std::uint32_t>(payload[1]) << 16) |
                     (static_cast<std::uint32_t>(payload[2]) << 8) |
                     static_cast<std::uint32_t>(payload[3])) &
                    0x7FFFFFFFU;
                if (stream_id == 0) {
                    std::lock_guard<std::mutex> lk(state->mu);
                    state->conn_send_window += static_cast<std::int64_t>(inc);
                } else {
                    auto& s = streams[stream_id];
                    s.id = stream_id;
                    s.send_window += static_cast<std::int64_t>(inc);
                }
                break;
            }
            case FrameType::RST_STREAM: {
                auto it = streams.find(stream_id);
                if (it != streams.end()) it->second.reset = true;
                break;
            }
            case FrameType::GOAWAY: {
                // Peer is closing — drain.
                send_goaway(0);
                state->socket.close();
                goto loop_end;
            }
            case FrameType::HEADERS: {
                if (stream_id == 0) {
                    send_goaway(/*PROTOCOL_ERROR*/ 0x1);
                    state->socket.close();
                    goto loop_end;
                }
                auto& s = streams[stream_id];
                s.id = stream_id;
                s.headers_received = true;
                std::size_t off = 0;
                std::size_t end_off = payload.size();
                if ((flags & Flags::PADDED) != 0 && off < end_off) {
                    const std::uint8_t pad_len = payload[off++];
                    if (pad_len >= end_off - off) {
                        rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        break;
                    }
                    end_off -= pad_len;
                }
                if ((flags & Flags::PRIORITY) != 0) {
                    if (off + 5 > end_off) {
                        rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        break;
                    }
                    off += 5;
                }
                s.pending_hpack.assign(payload.begin() + off, payload.begin() + end_off);

                const bool end_headers = (flags & Flags::END_HEADERS) != 0;
                const bool end_stream = (flags & Flags::END_STREAM) != 0;
                if (end_stream) s.end_stream_received = true;

                if (end_headers) {
                    s.headers = HPACK::decode_headers(s.pending_hpack);
                    s.pending_hpack.clear();
                    if (end_stream) {
                        handoff_request(s);
                        streams.erase(stream_id);
                    }
                }
                break;
            }
            case FrameType::CONTINUATION: {
                auto it = streams.find(stream_id);
                if (it == streams.end() || !it->second.headers_received) {
                    send_goaway(/*PROTOCOL_ERROR*/ 0x1);
                    state->socket.close();
                    goto loop_end;
                }
                auto& s = it->second;
                // Bound the header block: a CONTINUATION flood that never
                // sets END_HEADERS would otherwise grow pending_hpack without
                // limit (RFC 9113 CONTINUATION-flood DoS, CVE-2024-27316
                // class). Treat an oversized block as a connection error.
                if (s.pending_hpack.size() + payload.size() > kMaxHeaderListBytes) {
                    send_goaway(/*ENHANCE_YOUR_CALM*/ 0xb);
                    state->socket.close();
                    goto loop_end;
                }
                s.pending_hpack.insert(s.pending_hpack.end(), payload.begin(),
                                       payload.end());
                if ((flags & Flags::END_HEADERS) != 0) {
                    s.headers = HPACK::decode_headers(s.pending_hpack);
                    s.pending_hpack.clear();
                    if (s.end_stream_received) {
                        handoff_request(s);
                        streams.erase(stream_id);
                    }
                }
                break;
            }
            case FrameType::DATA: {
                auto it = streams.find(stream_id);
                if (it == streams.end() || !it->second.headers_received) {
                    send_goaway(/*PROTOCOL_ERROR*/ 0x1);
                    state->socket.close();
                    goto loop_end;
                }
                auto& s = it->second;
                std::size_t off = 0;
                std::size_t end_off = payload.size();
                if ((flags & Flags::PADDED) != 0 && off < end_off) {
                    const std::uint8_t pad_len = payload[off++];
                    if (pad_len >= end_off - off) {
                        rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        break;
                    }
                    end_off -= pad_len;
                }
                // Bound the buffered body. The receive window is refilled on
                // every DATA frame below, so flow control provides no memory
                // backpressure; without this cap a single stream that never
                // sets END_STREAM can exhaust memory. Reset the stream once
                // it exceeds the limit rather than buffering further.
                if (s.body.size() + (end_off - off) > kMaxBodyBytes) {
                    rst_stream(stream_id, /*ENHANCE_YOUR_CALM*/ 0xb);
                    streams.erase(stream_id);
                    break;
                }
                s.body.insert(s.body.end(), payload.begin() + off,
                              payload.begin() + end_off);
                s.recv_window -= static_cast<std::int64_t>(end_off - off);

                if (length > 0) {
                    enqueue_frame(state, Parser::build_window_update_frame(0, length));
                    enqueue_frame(state,
                                  Parser::build_window_update_frame(stream_id, length));
                }

                if ((flags & Flags::END_STREAM) != 0) {
                    s.end_stream_received = true;
                    handoff_request(s);
                    streams.erase(stream_id);
                }
                break;
            }
            case FrameType::PRIORITY:
            case FrameType::PUSH_PROMISE:
            default:
                // Ignore unknown / unsupported frame types per
                // RFC 9113 §4.1: "implementations MUST discard frames
                // that have unknown or unsupported types."
                break;
        }
    }
loop_end:
    send_goaway(0);
    // Don't socket.close() here: dispatched handlers may still be
    // mid-response.  When the last shared_ptr<ConnState> drops, the
    // embedded Socket destructor closes the fd.
}

} // namespace

auto make_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::ConnectionHandler {
    std::shared_ptr<Handler> shared(handler.release());
    return [shared](::spaznet::Socket sock) -> ::spaznet::Task {
        co_await serve(std::move(sock), *shared);
    };
}

} // namespace spaznet::http2
