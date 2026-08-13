// HTTP/1.1 reactor dispatcher: the coroutine-free counterpart of
// dispatcher.cpp's serve_keep_alive. Same protocol, same HTTPParser, same
// HTTPRequest/HTTPResponse, same HTTPHandler interface — only the
// execution model differs. Where serve_keep_alive keeps its buffer and
// request/response state implicitly in a coroutine frame across
// suspension points, Http1Connection keeps the same state in explicit
// members and is re-entered from the top by BufferedConnection's
// callbacks.
//
// Phase state machine:
//   ReadingRequest -> accumulating bytes via on_data(), reparsing the
//     whole buffered prefix each time (mirrors serve_keep_alive's
//     "insert new bytes, reparse" loop exactly; HTTPParser is stateless
//     across calls either way).
//   Dispatching -> a request was parsed and handed to the handler; we're
//     waiting for its ResponseWriter to complete (synchronously, almost
//     always today, or later).
// There's no separate "Writing" or "Closing" phase in the enum: writing
// is just a BufferedConnection::write() call (it manages its own
// backpressure via OutputBuffer), and closing is a terminal action
// (close() / close_after_flush()) rather than a state we wait in.
//
// Threading: on_data() only ever runs on the IOContext's IO thread (only
// that thread calls PlatformIO::wait()/process_io_events() — see
// IOContext::run()), so buffer_/phase_/closing_ are safe there without a
// lock. But a deferred ResponseWriter can complete from ANY thread (a
// background thread, a timer, another connection's callback), and
// on_response_ready() mutates the exact same fields. The ResponseWriter's
// deliver callback below routes the actual work through
// ctx_.post_to_io_thread(), which is a no-op queue hop when already on
// the IO thread (the synchronous-completion case) and marshals onto it
// otherwise — so on_response_ready() itself never needs to worry about
// which thread it's running on; it's always the IO thread. See
// docs/concurrency-and-coroutines.md's threading section.

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/reactor/buffered_connection.hpp>
#include <libspaznet/server.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace spaznet::http {

namespace {

// Same Transfer-Encoding sniff serve_keep_alive uses, factored out so
// both dispatchers stay in sync if the rule ever changes.
auto serialize_response(const HTTPResponse& response) -> std::vector<std::uint8_t> {
    auto te = response.get_header("Transfer-Encoding");
    if (te) {
        std::string te_lower = *te;
        std::transform(te_lower.begin(), te_lower.end(), te_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (te_lower.find("chunked") != std::string::npos) {
            return response.serialize_chunked();
        }
    }
    return response.serialize();
}

class Http1Connection : public std::enable_shared_from_this<Http1Connection> {
  public:
    Http1Connection(::spaznet::IOContext& ctx, std::shared_ptr<::spaznet::BufferedConnection> conn,
                    std::shared_ptr<HTTPHandler> handler)
        : ctx_(ctx), conn_(conn), handler_(std::move(handler)) {}

    // Wires this dispatcher onto `conn` (captured weak in this object,
    // captured strong in conn's own callbacks — see the class comment
    // below on why that direction avoids a reference cycle) and starts
    // the read loop. `initial_buffer` mirrors serve_keep_alive's
    // parameter of the same name: bytes already read off the wire by a
    // caller that peeked before handing off (e.g. a future WebSocket
    // upgrade sniff). Call exactly once, immediately after construction.
    void start(std::vector<std::uint8_t> initial_buffer, std::function<void()> on_closed) {
        notify_closed_ = std::move(on_closed);
        buffer_ = std::move(initial_buffer);

        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        auto self = shared_from_this();
        conn->set_on_data([self]() { self->on_data(); });
        conn->set_on_closed([self]() { self->on_closed(); });

        if (!buffer_.empty()) {
            try_process();
        }
        // Always call this, even if try_process() above already decided
        // to close: BufferedConnection::start() is a no-op once closed_
        // is set, and if it instead only queued a close_after_flush(),
        // this is what arms the write interest that will actually drain
        // it.
        conn->start();
    }

  private:
    // Protocol bounds mirroring serve_keep_alive's exactly (see there for
    // rationale); kept in sync deliberately rather than shared via a
    // constant so each dispatcher can diverge later without surprising
    // the other.
    static constexpr std::size_t kMaxRequestBytes = 1024 * 1024; // 1 MiB safety cap

    enum class Phase : std::uint8_t { ReadingRequest, Dispatching };

    // BufferedConnection::on_data: fires once per successful recv(), so
    // it may run several times back-to-back for one readable event.
    // Just accumulate; only actually parse/dispatch when we're not
    // already waiting on a previous request's response (HTTP/1.1
    // responses must be sent in request order, so a second request
    // pipelined behind one that's still dispatching has to wait its
    // turn — its bytes simply stay buffered in `buffer_` until then).
    void on_data() {
        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        auto data = conn->input().data();
        buffer_.insert(buffer_.end(), data.begin(), data.end());
        conn->input().consume(data.size());

        if (closing_ || phase_ != Phase::ReadingRequest) {
            return;
        }
        try_process();
    }

    // Fires exactly once, whenever BufferedConnection is done (orderly
    // EOF, hard error, or our own close()/close_after_flush() call
    // completing). If a request was mid-dispatch when the peer vanished,
    // account for it so Statistics::active_requests doesn't leak.
    void on_closed() {
        if (phase_ == Phase::Dispatching) {
            ctx_.decrement_active_requests();
            phase_ = Phase::ReadingRequest;
        }
        closing_ = true;
        if (notify_closed_) {
            auto cb = std::move(notify_closed_);
            notify_closed_ = nullptr;
            cb();
        }
    }

    // Parses and dispatches as many complete, buffered requests as
    // possible without recursing: a request whose handler completes the
    // ResponseWriter synchronously (the common case) is answered and the
    // loop continues in place to check for another one already
    // pipelined behind it. A request whose handler defers exits the
    // loop; on_response_ready() restarts a fresh call to this function
    // itself once that eventually completes, since by then this call's
    // own stack frame is long gone.
    void try_process() {
        while (phase_ == Phase::ReadingRequest && !closing_) {
            auto conn = conn_.lock();
            if (!conn || conn->closed()) {
                return;
            }

            HTTPRequest request;
            std::size_t bytes_consumed = 0;
            auto parse_result = HTTPParser::parse_request(buffer_, request, bytes_consumed);

            if (parse_result == HTTPParser::ParseResult::Incomplete) {
                if (buffer_.size() >= kMaxRequestBytes) {
                    send_error_and_close(400, "Bad Request");
                }
                return; // wait for more bytes via the next on_data().
            }
            if (parse_result != HTTPParser::ParseResult::Success) {
                send_error_and_close(400, "Bad Request");
                return;
            }
            if (bytes_consumed > buffer_.size()) {
                closing_ = true;
                conn->close();
                return;
            }
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(bytes_consumed));

            const bool keep_alive = request.should_keep_alive();
            phase_ = Phase::Dispatching;
            ctx_.increment_active_requests();

            auto self = shared_from_this();
            ResponseWriter writer([self, keep_alive](HTTPResponse response) {
                // deliver() itself may run on any thread (see the class
                // comment); post_to_io_thread() guarantees
                // on_response_ready() always runs on this connection's
                // IO thread, inline immediately if we're already there.
                self->ctx_.post_to_io_thread(
                    [self, keep_alive, response = std::move(response)]() mutable {
                        self->on_response_ready(std::move(response), keep_alive);
                    });
            });

            dispatch_call_active_ = true;
            try {
                handler_->handle_request(request, writer);
            } catch (...) {
                dispatch_call_active_ = false;
                // Match serve_keep_alive's bookkeeping (it decrements
                // then rethrows into the Task machinery); here there's
                // nowhere sensible to rethrow to, so just tear the
                // connection down rather than leave it stuck
                // mid-dispatch forever.
                if (phase_ == Phase::Dispatching) {
                    ctx_.decrement_active_requests();
                    phase_ = Phase::ReadingRequest;
                }
                closing_ = true;
                conn->close();
                return;
            }
            dispatch_call_active_ = false;
            // If on_response_ready ran synchronously above (writer
            // completed before handle_request returned — the universal
            // case today), phase_ is ReadingRequest again and the while
            // loop naturally checks for another pipelined request.
            // Otherwise it's still Dispatching and the loop exits here.
        }
    }

    // Always runs on the IO thread (see try_process()'s ResponseWriter
    // construction and the class comment above) — but still either
    // synchronously (nested inside handle_request(), itself nested inside
    // try_process()'s while loop above) or asynchronously, arbitrarily
    // later. dispatch_call_active_ is how it tells those two cases apart,
    // since phase_ alone can't (both start from Dispatching).
    void on_response_ready(HTTPResponse response, bool keep_alive) {
        ctx_.decrement_active_requests();
        // Flip back to ReadingRequest before touching conn_ below: if
        // write() synchronously fails, it re-enters on_closed() (via
        // fail()->close()) before returning, and on_closed() must see
        // "not dispatching" here to avoid double-decrementing
        // active_requests.
        phase_ = Phase::ReadingRequest;

        auto conn = conn_.lock();
        if (!conn || closing_) {
            return; // Connection is already gone; nothing left to send.
        }

        response.set_header("Connection", keep_alive ? "keep-alive" : "close");
        conn->write(serialize_response(response));

        if (!keep_alive) {
            closing_ = true;
            conn->close_after_flush();
            return;
        }
        if (!dispatch_call_active_) {
            // Asynchronous completion: try_process()'s loop that
            // dispatched this request already returned; nothing else
            // will pick up any bytes pipelined behind it unless we do.
            try_process();
        }
        // Else: still inside try_process()'s while loop (synchronous
        // completion) — it owns continuing/exiting based on phase_.
    }

    void send_error_and_close(int status_code, const char* reason) {
        closing_ = true;
        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        HTTPResponse error_response;
        error_response.version = "1.1";
        error_response.status_code = status_code;
        error_response.reason_phrase = reason;
        error_response.set_header("Connection", "close");
        error_response.set_header("Content-Length", "0");
        conn->write(error_response.serialize());
        conn->close_after_flush();
    }

    ::spaznet::IOContext& ctx_;
    std::weak_ptr<::spaznet::BufferedConnection> conn_;
    std::shared_ptr<HTTPHandler> handler_;
    std::vector<std::uint8_t> buffer_;
    Phase phase_ = Phase::ReadingRequest;
    bool closing_ = false;
    bool dispatch_call_active_ = false;
    std::function<void()> notify_closed_;
};

} // namespace

auto attach_reactor_dispatcher(::spaznet::IOContext& ctx,
                               std::shared_ptr<::spaznet::BufferedConnection> conn,
                               std::shared_ptr<HTTPHandler> handler,
                               std::vector<std::uint8_t> initial_buffer,
                               std::function<void()> on_closed) -> void {
    auto dispatcher = std::make_shared<Http1Connection>(ctx, conn, std::move(handler));
    dispatcher->start(std::move(initial_buffer), std::move(on_closed));
}

auto make_reactor_dispatcher(std::unique_ptr<HTTPHandler> handler) -> ::spaznet::ConnectionFactory {
    std::shared_ptr<HTTPHandler> shared(handler.release());
    return [shared](int fd, ::spaznet::IOContext& ctx,
                    std::function<void()> on_closed) -> std::shared_ptr<::spaznet::IoHandler> {
        auto conn = std::make_shared<::spaznet::BufferedConnection>(ctx, fd);
        attach_reactor_dispatcher(ctx, conn, shared, {}, std::move(on_closed));
        return conn;
    };
}

} // namespace spaznet::http
