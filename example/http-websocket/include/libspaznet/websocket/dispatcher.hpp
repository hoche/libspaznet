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
#include <libspaznet/websocket/reactor_handler.hpp>

#include <memory>

namespace spaznet::websocket {

#ifdef SPAZNET_HAS_COROUTINES
auto make_dispatcher(std::unique_ptr<::spaznet::http::HTTPHandler> http_handler,
                     std::unique_ptr<Handler> ws_handler) -> ::spaznet::ConnectionHandler;
#endif

// Coroutine-free counterpart of make_dispatcher: same upgrade-sniffing
// behavior (a null http_handler still means "404 to every non-WS
// request", a null ws_handler still means an upgrade attempt just falls
// through to the HTTP handler, same as above), same on-the-wire framing
// (handler.cpp's Frame::serialize/parse, unchanged), but the connection
// is a WsConnection state machine built on BufferedConnection instead of
// a suspended coroutine frame — see reactor_handler.hpp for the
// synchronous Handler/Connection this one is built against, and
// dispatcher_reactor.cpp for the state machine itself. Hand the result to
// Server::set_connection_factory instead of set_connection_handler.
auto make_reactor_dispatcher(std::unique_ptr<::spaznet::http::HTTPHandler> http_handler,
                             std::unique_ptr<reactor::Handler> ws_handler)
    -> ::spaznet::ConnectionFactory;

} // namespace spaznet::websocket
