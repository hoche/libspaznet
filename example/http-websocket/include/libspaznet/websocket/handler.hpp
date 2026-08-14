#pragma once

// Shared WebSocket wire types (Opcode / Frame / Message). Model-specific
// Handler/Connection live in coroutine_handler.hpp and reactor_handler.hpp.

#include <cstdint>
#include <string>
#include <vector>

namespace spaznet::websocket {

enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

struct Frame {
    // RFC 6455 §5.2 payload-length field is up to 63 bits, but a server has
    // no reason to honor anything close to that — a single bad client could
    // request a 16-EiB allocation. Cap at a sane application limit.
    static constexpr uint64_t kMaxPayloadBytes = 16ULL * 1024 * 1024;

    bool fin{};
    bool rsv1{};
    bool rsv2{};
    bool rsv3{};
    Opcode opcode{};
    bool masked{};
    uint64_t payload_length{};
    uint32_t masking_key{};
    std::vector<uint8_t> payload;

    [[nodiscard]] auto serialize() const -> std::vector<uint8_t>;
    // Throws std::runtime_error on a protocol violation or short input.
    // The server hot path in example/http-websocket/src/dispatcher_coroutine.cpp
    // does not use this — it parses inline so it can distinguish "need
    // more bytes" from "kill the connection". Callers using parse()
    // must catch and close the connection with code 1002 (protocol
    // error) or 1009 (message too big) depending on the cause.
    static auto parse(const std::vector<uint8_t>& data) -> Frame;
};

struct Message {
    Opcode opcode;
    std::vector<uint8_t> data;
};

} // namespace spaznet::websocket
