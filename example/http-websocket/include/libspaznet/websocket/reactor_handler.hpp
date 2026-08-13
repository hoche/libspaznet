#pragma once

// Coroutine-free counterpart of handler.hpp's Handler/Connection, used by
// websocket::make_reactor_dispatcher (see dispatcher.hpp). Reuses Opcode,
// Message, and Frame from handler.hpp unchanged -- only the
// connection/handler shape differs, because sending here is a direct,
// synchronous write into a BufferedConnection's OutputBuffer instead of a
// co_await'd socket write behind a WriteGate. The OutputBuffer already
// serializes writes by construction (see reactor/buffered_connection.hpp),
// which is what lets the WriteGate disappear entirely on this side.
//
// Unlike the coroutine Connection (deliberately non-copyable: it aliases
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
    // connection's OutputBuffer. Safe to call from any thread the caller
    // has arranged to have exclusive access to this connection from (see
    // docs/websocket.md's threading note) — a no-op if the underlying
    // connection has already gone away. `fin = false` sends a non-final
    // fragment (rarely needed).
    void send(Opcode opcode, std::span<const std::uint8_t> payload, bool fin = true) const {
        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        Frame frame;
        frame.fin = fin;
        frame.opcode = opcode;
        frame.masked = false;
        frame.payload.assign(payload.begin(), payload.end());
        frame.payload_length = frame.payload.size();
        conn->write(frame.serialize());
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

    // Same rvalue-first dispatch contract as the coroutine Handler (see
    // handler.hpp): override exactly one of the two handle_message
    // overloads.
    virtual void handle_message(const Message& message, Connection& conn) = 0;
    virtual void handle_message(Message&& message, Connection& conn) {
        handle_message(static_cast<const Message&>(message), conn);
    }

    virtual void on_open(Connection& conn) = 0;
    virtual void on_close(Connection& conn) = 0;
};

} // namespace spaznet::websocket::reactor
