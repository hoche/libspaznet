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

#include <memory>

namespace spaznet::http {

// Build a ConnectionHandler that speaks HTTP/1.1 on the accepted
// connection.  Ownership of `handler` is transferred into the
// dispatcher; the same handler instance is shared across all
// connections (see docs/http.md for thread-safety notes).
auto make_dispatcher(std::unique_ptr<HTTPHandler> handler) -> ::spaznet::ConnectionHandler;

} // namespace spaznet::http
