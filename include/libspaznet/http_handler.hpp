#pragma once

#include <libspaznet/io_context.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace spaznet {

// Forward declaration
class Socket;

struct HTTPRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

struct HTTPResponse {
    int status_code = 200;
    std::string status_message = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    
    void set_header(const std::string& key, const std::string& value) {
        headers[key] = value;
    }
    
    std::vector<uint8_t> serialize() const;
};

class HTTPHandler {
public:
    virtual ~HTTPHandler() = default;
    
    // Handle HTTP request
    virtual Task handle_request(const HTTPRequest& request, HTTPResponse& response, Socket& socket) = 0;
};

} // namespace spaznet

