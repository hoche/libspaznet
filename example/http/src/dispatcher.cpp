// HTTP/1.1 ConnectionHandler implementation.
//
// Adapts a user-provided HTTPHandler into a std::function<Task(Socket)>
// that the Server can dispatch directly via set_connection_handler.
// The dispatch loop is a straight HTTP/1.1 keep-alive read/parse/serve
// cycle — there's no WebSocket upgrade detection here; for that, see
// example/http-websocket/.

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/server.hpp>

#include <algorithm>
#include <cctype>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace spaznet::http {

namespace {

// Bridges a ResponseWriter's completion into this coroutine's resumption.
// HTTPHandler::handle_request() is called as a plain synchronous function
// below — no co_await at the call site — so in the (currently universal)
// case where a handler completes the writer before returning,
// await_ready() is already true and this awaiter suspends nothing at all.
// It only matters for a handler that defers: it parks this coroutine and
// resumes it, via ctx->post() (so always off whichever call stack actually
// completes the writer, never inline), once complete() eventually runs.
struct AwaitResponseReady {
    ResponseWriter writer;
    ::spaznet::IOContext* ctx;

    [[nodiscard]] auto await_ready() const -> bool {
        return writer.is_completed();
    }
    void await_suspend(std::coroutine_handle<> handle) const {
        auto* ctx_ptr = ctx;
        writer.on_ready([ctx_ptr, handle]() mutable { ctx_ptr->post([handle]() mutable { handle.resume(); }); });
    }
    void await_resume() const {}
};

} // namespace

auto serve_keep_alive(::spaznet::Socket socket, HTTPHandler& handler,
                      std::vector<std::uint8_t> initial_buffer) -> ::spaznet::Task {
    constexpr std::size_t kReadChunk = 8192;
    constexpr std::size_t kMaxRequestBytes = 1024 * 1024; // 1 MiB safety cap

    std::vector<uint8_t> buffer = std::move(initial_buffer);
    if (buffer.empty()) {
        // No prior sniff — prime from the socket.
        co_await socket.async_read(buffer, 2048);
        if (buffer.empty()) {
            socket.close();
            co_return;
        }
    }

    while (true) {
        spaznet::http::HTTPRequest request;
        size_t bytes_consumed = 0;
        spaznet::http::HTTPParser::ParseResult parse_result =
            spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);

        // Read until we have a full request (headers + body) or hit the cap.
        while (parse_result == spaznet::http::HTTPParser::ParseResult::Incomplete &&
               buffer.size() < kMaxRequestBytes) {
            std::vector<uint8_t> more_data;
            co_await socket.async_read(more_data, kReadChunk);
            if (more_data.empty()) {
                socket.close();
                co_return;
            }
            buffer.insert(buffer.end(), more_data.begin(), more_data.end());
            parse_result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
        }

        if (parse_result != spaznet::http::HTTPParser::ParseResult::Success) {
            spaznet::http::HTTPResponse error_response;
            error_response.version = "1.1";
            error_response.status_code = 400;
            error_response.reason_phrase = "Bad Request";
            error_response.set_header("Connection", "close");
            error_response.set_header("Content-Length", "0");
            co_await socket.async_write(error_response.serialize());
            socket.close();
            co_return;
        }

        if (bytes_consumed > buffer.size()) {
            socket.close();
            co_return;
        }

        buffer.erase(buffer.begin(), buffer.begin() + bytes_consumed);

        socket.context()->increment_active_requests();
        try {
            // Filled in by ResponseWriter's deliver callback below (which
            // replaces this wholesale); HTTPResponse::version already
            // defaults to "1.1", matching what used to be set explicitly
            // here.
            spaznet::http::HTTPResponse response;

            // No co_await here: handle_request() is now a plain synchronous
            // call. ResponseWriter's deliver callback below fills `response`
            // in place, so a handler that completes before returning (the
            // common case) needs no suspension at all.
            ResponseWriter writer([&response](HTTPResponse r) { response = std::move(r); });
            handler.handle_request(request, writer);
            co_await AwaitResponseReady{writer, socket.context()};

            const bool keep_alive = request.should_keep_alive();
            response.set_header("Connection", keep_alive ? "keep-alive" : "close");

            std::vector<uint8_t> response_data;
            auto te = response.get_header("Transfer-Encoding");
            if (te) {
                std::string te_lower = *te;
                std::transform(te_lower.begin(), te_lower.end(), te_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (te_lower.find("chunked") != std::string::npos) {
                    response_data = response.serialize_chunked();
                } else {
                    response_data = response.serialize();
                }
            } else {
                response_data = response.serialize();
            }

            co_await socket.async_write(std::move(response_data));
            socket.context()->decrement_active_requests();

            if (!keep_alive) {
                socket.close();
                co_return;
            }
        } catch (...) {
            socket.context()->decrement_active_requests();
            throw;
        }
    }
}

auto make_dispatcher(std::unique_ptr<HTTPHandler> handler) -> ::spaznet::ConnectionHandler {
    // std::function requires its callable to be copyable; std::unique_ptr
    // isn't.  Wrap the handler in a shared_ptr so the std::function
    // payload is copyable while still owning a single handler instance
    // shared across all connections.
    std::shared_ptr<HTTPHandler> shared(handler.release());
    return [shared](::spaznet::Socket sock) -> ::spaznet::Task {
        co_await serve_keep_alive(std::move(sock), *shared, {});
    };
}

} // namespace spaznet::http
