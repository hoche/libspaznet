#pragma once

// HTTP/1.1 + WebSocket ConnectionHandler factory.
//
// Combined dispatcher: the same TCP connection serves plain HTTP/1.1
// requests until a client asks for a WebSocket upgrade (RFC 6455
// §4.2), at which point control flips to the WS frame loop.  Provide
// both handlers up front — either may be null if you only care about
// one half (a null http_handler responds 404 to every non-WS request;
// a null ws_handler responds 400 to upgrade attempts).

#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/websocket/handler.hpp>

#include <memory>

namespace spaznet::websocket {

auto make_dispatcher(std::unique_ptr<::spaznet::http::HTTPHandler> http_handler,
                     std::unique_ptr<Handler> ws_handler) -> ::spaznet::ConnectionHandler;

} // namespace spaznet::websocket
