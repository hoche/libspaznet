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
enum class HTTP3FrameType : uint64_t {
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
    int status_code = 200;
    std::string reason_phrase = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    void set_header(const std::string& key, const std::string& value) {
        headers[key] = value;
    }

    std::optional<std::string> get_header(const std::string& name) const {
        auto it = headers.find(name);
        if (it != headers.end()) {
            return it->second;
        }
        return std::nullopt;
    }
};

// HTTP/3 Handler interface
class HTTP3Handler {
  public:
    virtual ~HTTP3Handler() = default;

    // Handle HTTP/3 request
    virtual Task handle_request(const HTTP3Request& request, HTTP3Response& response,
                                std::shared_ptr<QUICStream> stream) = 0;
};

} // namespace spaznet
