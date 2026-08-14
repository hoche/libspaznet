#pragma once

// Coroutine-free counterpart of coroutine_handler.hpp's Handler/Connection,
// used by websocket::make_reactor_dispatcher (see dispatcher.hpp). Reuses
// Opcode, Message, and Frame from handler.hpp unchanged — only the
// connection/handler shape differs, because sending here is a direct,
// synchronous write into a BufferedConnection's OutputBuffer instead of a
// co_await'd socket write behind a WriteGate. The OutputBuffer already
// serializes writes by construction (see reactor/buffered_connection.hpp),
// which is what lets the WriteGate disappear entirely on this side.
//
// Unlike coroutine::Connection (deliberately non-copyable: it aliases
// state living in a suspended coroutine frame that dies with the
// connection), this Connection is a small, cheap value type — just a
// weak_ptr and a stable fd — safe to copy and store anywhere (e.g. in a
// broadcast handler's session table) for as long as you like. It simply
// stops doing anything once the underlying connection is gone.

#include <libspaznet/reactor/buffered_connection.hpp>
#include <libspaznet/websocket/handler.hpp>

#include <memory>
#include <span>

namespace spaznet::websocket::reactor {

class Connection {
  public:
    Connection(std::weak_ptr<::spaznet::BufferedConnection> conn, int fd,
              ::spaznet::IOContext& ctx)
        : conn_(std::move(conn)), fd_(fd), ctx_(&ctx) {}

    // Stable identifier for this connection (the underlying socket fd),
    // valid even after the connection has gone away.
    [[nodiscard]] auto id() const -> int {
        return fd_;
    }
    [[nodiscard]] auto context() const -> ::spaznet::IOContext* {
        return ctx_;
    }

    // Build a server-origin (unmasked) frame and write it directly to the
    // connection's OutputBuffer. Genuinely safe to call from ANY thread —
    // not just the one driving this connection's own on_readable() — via
    // IOContext::post_to_io_thread(): the actual write happens inline,
    // synchronously, if the calling thread already is this connection's
    // IO thread (so a handler calling send() on its own Connection from
    // inside handle_message() costs nothing extra), and is marshaled
    // onto that thread otherwise (e.g. a broadcast into a *different*
    // connection from inside another connection's callback — see
    // demo/chat.cpp's ChatRoomReactor). A no-op if the underlying
    // connection has already gone away by the time it runs. `fin = false`
    // sends a non-final fragment (rarely needed).
    void send(Opcode opcode, std::span<const std::uint8_t> payload, bool fin = true) const {
        std::vector<std::uint8_t> payload_copy(payload.begin(), payload.end());
        auto conn_weak = conn_;
        auto do_send = [conn_weak, opcode, fin, payload_copy = std::move(payload_copy)]() mutable {
            auto conn = conn_weak.lock();
            if (!conn) {
                return;
            }
            Frame frame;
            frame.fin = fin;
            frame.opcode = opcode;
            frame.masked = false;
            frame.payload = std::move(payload_copy);
            frame.payload_length = frame.payload.size();
            conn->write(frame.serialize());
        };
        if (ctx_) {
            ctx_->post_to_io_thread(std::move(do_send));
        } else {
            do_send();
        }
    }

  private:
    std::weak_ptr<::spaznet::BufferedConnection> conn_;
    int fd_;
    ::spaznet::IOContext* ctx_;
};

class Handler {
  public:
    Handler() = default;
    virtual ~Handler() = default;

    Handler(const Handler&) = delete;
    auto operator=(const Handler&) -> Handler& = delete;
    Handler(Handler&&) = delete;
    auto operator=(Handler&&) -> Handler& = delete;

    // Same rvalue-first dispatch contract as coroutine::Handler (see
    // coroutine_handler.hpp): override exactly one of the two handle_message
    // overloads.
    virtual void handle_message(const Message& message, Connection& conn) = 0;
    virtual void handle_message(Message&& message, Connection& conn) {
        handle_message(static_cast<const Message&>(message), conn);
    }

    virtual void on_open(Connection& conn) = 0;
    virtual void on_close(Connection& conn) = 0;
};

} // namespace spaznet::websocket::reactor
