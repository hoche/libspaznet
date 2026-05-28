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
    // RFC 6455 §5.2 payload-length field is up to 63 bits, but a server has
    // no reason to honor anything close to that — a single bad client could
    // request a 16-EiB allocation. Cap at a sane application limit.
    static constexpr uint64_t kMaxPayloadBytes = 16ULL * 1024 * 1024;

    bool fin{};
    bool rsv1{};
    bool rsv2{};
    bool rsv3{};
    WebSocketOpcode opcode{};
    bool masked{};
    uint64_t payload_length{};
    uint32_t masking_key{};
    std::vector<uint8_t> payload;

    [[nodiscard]] auto serialize() const -> std::vector<uint8_t>;
    // Throws std::runtime_error on a protocol violation or short input. The
    // server hot path in server_impl.cpp does not use this — it parses
    // inline so it can distinguish "need more bytes" from "kill the
    // connection". Callers using parse() must catch and close the
    // connection with code 1002 (protocol error) or 1009 (message too big)
    // depending on the cause.
    static auto parse(const std::vector<uint8_t>& data) -> WebSocketFrame;
};

struct WebSocketMessage {
    WebSocketOpcode opcode;
    std::vector<uint8_t> data;
};

class WebSocketHandler {
  public:
    WebSocketHandler() = default;
    virtual ~WebSocketHandler() = default;

    // Delete copy and move operations
    WebSocketHandler(const WebSocketHandler&) = delete;
    auto operator=(const WebSocketHandler&) -> WebSocketHandler& = delete;
    WebSocketHandler(WebSocketHandler&&) = delete;
    auto operator=(WebSocketHandler&&) -> WebSocketHandler& = delete;

    // Handle WebSocket message
    virtual auto handle_message(const WebSocketMessage& message, Socket& socket) -> Task = 0;

    // Handle WebSocket connection open
    virtual auto on_open(Socket& socket) -> Task = 0;

    // Handle WebSocket connection close
    virtual auto on_close(Socket& socket) -> Task = 0;
};

} // namespace spaznet
