#pragma once

// HTTP/1.1-only CoroutineConnectionHandler factory.
//
// Hand `make_coroutine_dispatcher` an HTTPHandler implementation and pass the
// result to `Server::set_coroutine_connection_handler`.  Each accepted TCP
// connection runs an HTTP/1.1 keep-alive loop that calls into the
// user's handler.  No WebSocket upgrade detection — for that, see
// example/http-websocket/.

#include <libspaznet/http/handler.hpp>
#include <libspaznet/reactor/buffered_connection.hpp>
#include <libspaznet/server.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace spaznet::http {

#ifdef SPAZNET_HAS_COROUTINES
// Build a CoroutineConnectionHandler that speaks HTTP/1.1 on the accepted
// connection.  Ownership of `handler` is transferred into the
// dispatcher; the same handler instance is shared across all
// connections (see docs/http.md for thread-safety notes).
auto make_coroutine_dispatcher(std::unique_ptr<HTTPHandler> handler) -> ::spaznet::CoroutineConnectionHandler;

// Lower-level entry point: serve an HTTP/1.1 keep-alive session on
// `socket` with `initial_buffer` already consumed from the wire.
// Used by example/http-websocket: it reads the first ~2 KiB of the
// connection to sniff a WebSocket upgrade; if the request turns out
// to be plain HTTP, it hands the already-read buffer here instead of
// asking the kernel for those same bytes a second time.
auto serve_coroutine_keep_alive(::spaznet::Socket socket, HTTPHandler& handler,
                      std::vector<std::uint8_t> initial_buffer) -> ::spaznet::Task;
#endif // SPAZNET_HAS_COROUTINES

// Reactor-side counterpart of make_coroutine_dispatcher: no Task, no co_await,
// anywhere. Hand the result to Server::set_reactor_connection_factory instead of
// set_coroutine_connection_handler. Speaks the exact same HTTP/1.1 keep-alive
// protocol against the exact same HTTPHandler interface (handler.cpp,
// HTTPParser, HTTPRequest/HTTPResponse are all shared, unmodified, with
// the coroutine dispatcher above) — the two are meant to be
// interchangeable from a client's point of view; see
// tests/integration/test_dispatcher_differential.cpp for the harness that
// checks exactly that.
auto make_reactor_dispatcher(std::unique_ptr<HTTPHandler> handler) -> ::spaznet::ReactorConnectionFactory;

// Lower-level building block make_reactor_dispatcher() is built on:
// attach the reactor HTTP/1.1 keep-alive loop to an already-constructed
// BufferedConnection, optionally seeded with bytes already read off the
// wire. Mirrors serve_coroutine_keep_alive's role for the coroutine runtime above —
// example/http-websocket's reactor dispatcher uses this exactly the way
// its coroutine counterpart uses serve_coroutine_keep_alive: peek at the
// connection to decide HTTP vs. WebSocket, then hand off here (with
// whatever was already read) if it turns out to be plain HTTP.
// `on_closed` fires exactly once, whenever the connection this attaches
// to is done. Call at most once per `conn`.
auto attach_reactor_dispatcher(::spaznet::IOContext& ctx,
                               std::shared_ptr<::spaznet::BufferedConnection> conn,
                               std::shared_ptr<HTTPHandler> handler,
                               std::vector<std::uint8_t> initial_buffer,
                               std::function<void()> on_closed) -> void;

} // namespace spaznet::http
