#pragma once

#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace spaznet {

// Forward declaration
class Socket;

struct HTTP2Frame {
    uint32_t length;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
    std::vector<uint8_t> payload;
};

struct HTTP2Request {
    uint32_t stream_id;
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

struct HTTP2Response {
    uint32_t stream_id;
    int status_code = 200;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    HTTP2Frame to_frame() const;
};

class HTTP2Handler {
  public:
    virtual ~HTTP2Handler() = default;

    // Handle HTTP/2 request
    virtual Task handle_request(const HTTP2Request& request, HTTP2Response& response,
                                Socket& socket) = 0;

    // Handle HTTP/2 frame
    virtual Task handle_frame(const HTTP2Frame& frame, Socket& socket) = 0;
};

} // namespace spaznet

