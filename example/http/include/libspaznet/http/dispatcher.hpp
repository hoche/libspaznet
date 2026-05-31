#pragma once

// HTTP/1.1-only ConnectionHandler factory.
//
// Hand `make_dispatcher` an HTTPHandler implementation and pass the
// result to `Server::set_connection_handler`.  Each accepted TCP
// connection runs an HTTP/1.1 keep-alive loop that calls into the
// user's handler.  No WebSocket upgrade detection — for that, see
// example/http-websocket/.

#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace spaznet::http {

// Build a ConnectionHandler that speaks HTTP/1.1 on the accepted
// connection.  Ownership of `handler` is transferred into the
// dispatcher; the same handler instance is shared across all
// connections (see docs/http.md for thread-safety notes).
auto make_dispatcher(std::unique_ptr<HTTPHandler> handler) -> ::spaznet::ConnectionHandler;

// Lower-level entry point: serve an HTTP/1.1 keep-alive session on
// `socket` with `initial_buffer` already consumed from the wire.
// Used by example/http-websocket: it reads the first ~2 KiB of the
// connection to sniff a WebSocket upgrade; if the request turns out
// to be plain HTTP, it hands the already-read buffer here instead of
// asking the kernel for those same bytes a second time.
auto serve_keep_alive(::spaznet::Socket socket, HTTPHandler& handler,
                      std::vector<std::uint8_t> initial_buffer) -> ::spaznet::Task;

} // namespace spaznet::http
