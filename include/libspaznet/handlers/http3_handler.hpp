#pragma once

#include <cstdint>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/handlers/quic_handler.hpp>
#include <libspaznet/io_context.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spaznet {

// Forward declaration
class Socket;

// HTTP/3 Frame Types (RFC9114 Section 7.2)
enum class HTTP3FrameType : uint8_t {
    Data = 0x00,
    Headers = 0x01,
    CancelPush = 0x03,
    Settings = 0x04,
    PushPromise = 0x05,
    GoAway = 0x07,
    MaxPushId = 0x0D,
};

// HTTP/3 Request (RFC9114)
struct HTTP3Request {
    std::string method;
    std::string request_target;
    std::string scheme;
    std::string authority;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

// HTTP/3 Response (RFC9114)
struct HTTP3Response {
    int status_code = DEFAULT_HTTP_STATUS_CODE; // Defined in http_handler.hpp
    std::string reason_phrase = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    void set_header(const std::string& key, const std::string& value) {
        headers[key] = value;
    }

    auto get_header(const std::string& name) const -> std::optional<std::string> {
        auto iterator = headers.find(name);
        if (iterator != headers.end()) {
            return iterator->second;
        }
        return std::nullopt;
    }
};

// HTTP/3 Handler interface
class HTTP3Handler {
  public:
    HTTP3Handler() = default;
    virtual ~HTTP3Handler() = default;

    // Delete copy and move operations
    HTTP3Handler(const HTTP3Handler&) = delete;
    auto operator=(const HTTP3Handler&) -> HTTP3Handler& = delete;
    HTTP3Handler(HTTP3Handler&&) = delete;
    auto operator=(HTTP3Handler&&) -> HTTP3Handler& = delete;

    // Handle HTTP/3 request
    virtual auto handle_request(const HTTP3Request& request, HTTP3Response& response,
                                std::shared_ptr<QUICStream> stream) -> Task = 0;
};

} // namespace spaznet
