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

// Coroutine-free counterpart of make_dispatcher: identical wire behavior
// (preface, SETTINGS, multiplexed streams, HPACK, flow control, PING,
// GOAWAY, RST_STREAM — see dispatcher_reactor.cpp's header comment) built
// on Http2Connection, an explicit {Preface, FrameHeader, FramePayload}
// state machine driven by BufferedConnection's callbacks instead of a
// suspended coroutine frame per connection plus a detached one per
// stream. Same Handler interface as make_dispatcher above — a single
// Handler implementation works unchanged under either runtime. Hand the
// result to Server::set_connection_factory instead of
// set_connection_handler.
auto make_reactor_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::ConnectionFactory;

} // namespace spaznet::http2
