#pragma once

// Build and send a server-origin (unmasked) WebSocket frame in one
// allocation, skipping the Frame value type entirely.  The replacement
// for the old Socket::send_websocket_message method — moved out of
// Socket because the core library has no business knowing about WS
// framing.
//
// Pass `fin = false` to send a non-final fragment (rare; only useful
// for streaming a huge payload).

#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <libspaznet/websocket/handler.hpp>

#include <span>

namespace spaznet {
class Socket;
}

namespace spaznet::websocket {

auto send_message(::spaznet::Socket& socket, Opcode opcode,
                  std::span<const std::uint8_t> payload, bool fin = true)
    -> ::spaznet::Task;

} // namespace spaznet::websocket
