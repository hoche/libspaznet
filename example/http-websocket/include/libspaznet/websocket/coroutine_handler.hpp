#pragma once

// Coroutine counterpart of reactor_handler.hpp's Handler/Connection, used by
// websocket::make_coroutine_dispatcher (see dispatcher.hpp). Reuses Opcode,
// Message, and Frame from handler.hpp unchanged — only the connection/handler
// shape differs: sending is a co_await'd socket write behind a WriteGate.
//
// Connection is deliberately non-copyable: it aliases state living in a
// suspended coroutine frame that dies with the connection. Contrast
// reactor::Connection, which is a cheap copyable weak_ptr + fd value.

#ifndef SPAZNET_HAS_COROUTINES
#error "websocket/coroutine_handler.hpp requires SPAZNET_HAS_COROUTINES"
#endif

#include <libspaznet/io_context.hpp>
#include <libspaznet/websocket/handler.hpp>

#include <span>

namespace spaznet {
class Socket;
}

namespace spaznet::websocket::coroutine {

using ::spaznet::Socket;
using ::spaznet::Task;

// Per-connection async write gate. Opaque here; defined in
// dispatcher_coroutine.cpp. Serializes every write to a single connection so
// the dispatcher's control frames never interleave with (or race the fd's
// write registration against) an application coroutine's writes.
struct WriteGate;

// Handle to one live WebSocket connection, handed to every Handler callback.
//
// A Handler must NOT write to the socket directly: the dispatcher's reader
// coroutine writes control frames (Pong, Close) on its own, and a second
// uncoordinated writer would interleave frames on the wire and clobber the
// I/O layer's per-fd write registration. Instead, all application writes go
// through send(), which funnels through the same per-connection gate the
// dispatcher uses, so writes are serialized no matter which coroutine or
// worker thread they originate from (e.g. a chat broadcaster writing to this
// connection from a *different* connection's context).
class Connection {
  public:
    Connection(::spaznet::Socket& socket, WriteGate& gate) : socket_(&socket), gate_(&gate) {}

    // Delete copy/move: a Connection refers to state owned by the dispatcher's
    // per-connection coroutine frame and must not outlive or be duplicated.
    Connection(const Connection&) = delete;
    auto operator=(const Connection&) -> Connection& = delete;
    Connection(Connection&&) = delete;
    auto operator=(Connection&&) -> Connection& = delete;
    ~Connection() = default;

    // Stable identifier for this connection (the underlying socket fd).
    [[nodiscard]] auto id() const -> int;
    [[nodiscard]] auto context() const -> ::spaznet::IOContext*;

    // Build a server-origin (unmasked) frame and write it under the
    // connection's write gate. Safe to call from any coroutine / worker
    // thread. `fin = false` sends a non-final fragment (rarely needed).
    auto send(Opcode opcode, std::span<const std::uint8_t> payload, bool fin = true)
        -> ::spaznet::Task;

  private:
    ::spaznet::Socket* socket_;
    WriteGate* gate_;
};

class Handler {
  public:
    Handler() = default;
    virtual ~Handler() = default;

    Handler(const Handler&) = delete;
    auto operator=(const Handler&) -> Handler& = delete;
    Handler(Handler&&) = delete;
    auto operator=(Handler&&) -> Handler& = delete;

    // Handle a WebSocket message.
    //
    // The dispatch site always calls the rvalue overload first, so a
    // handler that wants to *consume* the payload (move it into a
    // parser, into a response body, etc.) can override that overload
    // and avoid copying the data vector.
    //
    // The default implementation of the rvalue overload forwards to the
    // const& overload, which keeps existing handlers working unchanged
    // (they continue to see a const reference and copy as before).
    // Handlers MUST override exactly one — typically the const& form
    // for read-only use, or the rvalue form for move-consume — leaving
    // the other to its default forwarder.
    virtual auto handle_message(const Message& message, Connection& conn) -> Task = 0;
    virtual auto handle_message(Message&& message, Connection& conn) -> Task {
        return handle_message(static_cast<const Message&>(message), conn);
    }

    virtual auto on_open(Connection& conn) -> Task = 0;
    virtual auto on_close(Connection& conn) -> Task = 0;
};

} // namespace spaznet::websocket::coroutine
