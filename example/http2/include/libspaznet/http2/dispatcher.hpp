#pragma once

// HTTP/2 (h2c, prior-knowledge cleartext) ConnectionHandler factory.
//
// Hand `make_dispatcher` an http2::Handler implementation and pass
// the result to `Server::set_connection_handler`.  Each accepted TCP
// connection runs the full h2c serve loop: connection preface,
// SETTINGS exchange, multiplexed stream handling, HPACK header
// decode/encode, flow-control window tracking, and dispatch through
// the user's Handler.
//
// This is RFC 9113 §3.4 "HTTP/2 over cleartext TCP".  HTTP/2 over
// TLS (the `h2` ALPN) needs a TLS terminator in front; not provided
// here.

#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>

#include <memory>

namespace spaznet::http2 {

auto make_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::ConnectionHandler;

} // namespace spaznet::http2
