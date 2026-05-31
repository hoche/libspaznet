#pragma once

#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <string>
#include <vector>

namespace spaznet {
class Socket;
}

namespace spaznet::websocket {

using ::spaznet::Socket;
using ::spaznet::Task;

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
    // The server hot path in example/http-websocket/src/dispatcher.cpp
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

class Handler {
  public:
    Handler() = default;
    virtual ~Handler() = default;

    // Delete copy and move operations
    Handler(const Handler&) = delete;
    auto operator=(const Handler&) -> Handler& = delete;
    Handler(Handler&&) = delete;
    auto operator=(Handler&&) -> Handler& = delete;

    // Handle a WebSocket message.
    //
    // The dispatch site always calls the rvalue overload first, so a
    // handler that wants to *consume* the payload (move it into a
    // parser, into a response body, etc.) can override that overload
    // and avoid copying the data vector.
    //
    // The default implementation of the rvalue overload forwards to the
    // const& overload, which keeps existing handlers working unchanged
    // (they continue to see a const reference and copy as before).
    // Handlers MUST override exactly one — typically the const& form
    // for read-only use, or the rvalue form for move-consume — leaving
    // the other to its default forwarder.
    virtual auto handle_message(const Message& message, Socket& socket) -> Task = 0;
    virtual auto handle_message(Message&& message, Socket& socket) -> Task {
        // Default: forward to the const& overload. Override this in your
        // handler to take ownership of `message.data` via std::move.
        return handle_message(static_cast<const Message&>(message), socket);
    }

    // Handle WebSocket connection open
    virtual auto on_open(Socket& socket) -> Task = 0;

    // Handle WebSocket connection close
    virtual auto on_close(Socket& socket) -> Task = 0;
};

} // namespace spaznet::websocket
