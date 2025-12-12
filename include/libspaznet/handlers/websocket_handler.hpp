#pragma once

#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <string>
#include <vector>

namespace spaznet {

// Forward declaration
class Socket;

enum class WebSocketOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

struct WebSocketFrame {
    bool fin;
    bool rsv1;
    bool rsv2;
    bool rsv3;
    WebSocketOpcode opcode;
    bool masked;
    uint64_t payload_length;
    uint32_t masking_key;
    std::vector<uint8_t> payload;

    std::vector<uint8_t> serialize() const;
    static WebSocketFrame parse(const std::vector<uint8_t>& data);
};

struct WebSocketMessage {
    WebSocketOpcode opcode;
    std::vector<uint8_t> data;
};

class WebSocketHandler {
  public:
    virtual ~WebSocketHandler() = default;

    // Handle WebSocket message
    virtual Task handle_message(const WebSocketMessage& message, Socket& socket) = 0;

    // Handle WebSocket connection open
    virtual Task on_open(Socket& socket) = 0;

    // Handle WebSocket connection close
    virtual Task on_close(Socket& socket) = 0;
};

} // namespace spaznet

