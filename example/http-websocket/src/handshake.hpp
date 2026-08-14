#pragma once

// WebSocket handshake (RFC 6455 §4.2) parsing and Sec-WebSocket-Accept
// computation. Shared by both dispatchers -- dispatcher_coroutine.cpp's coroutine
// serve_websocket/make_coroutine_dispatcher and dispatcher_reactor.cpp's
// WsConnection -- so the upgrade-detection rules can't drift between
// them. Originally lived inline in dispatcher_coroutine.cpp; moved here verbatim
// (no behavior change) when the reactor dispatcher needed the same
// logic.

#include <map>
#include <optional>
#include <string>

namespace spaznet::websocket::detail {

struct HandshakeRequest {
    std::string method;
    std::map<std::string, std::string> headers;
};

// Parses the request line + headers out of the leading portion of a
// buffered TCP stream, up to and including the blank line that
// terminates an HTTP header block. Returns std::nullopt if `request`
// doesn't yet contain a full "\r\n\r\n"-terminated header block.
auto parse_handshake(const std::string& request) -> std::optional<HandshakeRequest>;

// RFC 6455 §4.2.1: GET, Upgrade: websocket, Connection: Upgrade,
// Sec-WebSocket-Version: 13, and a well-formed Sec-WebSocket-Key.
auto is_upgrade(const HandshakeRequest& req) -> bool;

// Sec-WebSocket-Accept = base64(SHA-1(key + RFC6455 magic GUID)).
auto compute_accept(const std::string& key) -> std::string;

} // namespace spaznet::websocket::detail
