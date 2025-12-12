#pragma once

#include <cctype>
#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spaznet {

// Forward declaration
class Socket;

// HTTP/1.1 Request structure per RFC 9112
struct HTTPRequest {
    std::string method;         // HTTP method (GET, POST, etc.) per RFC 9112 Section 9
    std::string request_target; // Request target (origin-form, absolute-form, etc.) per RFC 9112
                                // Section 3.2
    std::string version;        // HTTP version (e.g., "1.1") per RFC 9112 Section 2.5
    std::unordered_map<std::string, std::string>
        headers;               // Header fields (field-name: field-value) per RFC 9112 Section 5.5
    std::vector<uint8_t> body; // Message body per RFC 9112 Section 6

    // Helper to get header (case-insensitive per RFC 9112 Section 5.1)
    auto get_header(const std::string& name) const -> std::optional<std::string>;

    // Check if connection should be kept alive per RFC 9112 Section 9.6
    auto should_keep_alive() const -> bool;

    // Get Content-Length if present per RFC 9112 Section 8.6
    auto get_content_length() const -> std::optional<size_t>;

    // Check if request has chunked transfer encoding per RFC 9112 Section 7.1
    auto is_chunked() const -> bool;
};

namespace {
constexpr int DEFAULT_HTTP_STATUS_CODE = 200;
}

// HTTP/1.1 Response structure per RFC 9112
struct HTTPResponse {
    std::string version = "1.1";                          // HTTP version
    int status_code = DEFAULT_HTTP_STATUS_CODE;           // Status code
    std::string reason_phrase = "OK";                     // Reason phrase
    std::unordered_map<std::string, std::string> headers; // Header fields
    std::vector<uint8_t> body;                            // Message body

    void set_header(const std::string& key, const std::string& value) {
        headers[key] = value;
    }

    // Helper to get header (case-insensitive)
    auto get_header(const std::string& name) const -> std::optional<std::string>;

    // Set Content-Length header
    auto set_content_length(size_t length) -> void;

    // Set chunked transfer encoding
    auto set_chunked() -> void;

    // Serialize response per RFC 9112
    auto serialize() const -> std::vector<uint8_t>;

    // Serialize with chunked encoding
    auto serialize_chunked() const -> std::vector<uint8_t>;
};

// HTTP/1.1 Parser per RFC 9112
class HTTPParser {
  public:
    enum class ParseResult : uint8_t {
        Success,
        Incomplete, // Need more data
        Error       // Parse error
    };

    // Parse HTTP request from buffer
    static auto parse_request(const std::vector<uint8_t>& buffer, HTTPRequest& request,
                              size_t& bytes_consumed) -> ParseResult;

    // Parse HTTP response from buffer
    static auto parse_response(const std::vector<uint8_t>& buffer, HTTPResponse& response,
                               size_t& bytes_consumed) -> ParseResult;

    // Parse chunked message body
    static auto parse_chunked_body(const std::vector<uint8_t>& buffer, std::vector<uint8_t>& body,
                                   size_t& bytes_consumed) -> ParseResult;

    // Helper functions for parsing (public for testing)
    static auto is_token_char(char character) -> bool;
    static auto is_field_vchar(char character) -> bool;
    static auto to_lower(const std::string& str) -> std::string;
    static auto trim_ows(const std::string& str) -> std::string; // Trim obs-fold and OWS
    static auto parse_request_line(const std::string& line, HTTPRequest& request) -> bool;
    static auto parse_status_line(const std::string& line, HTTPResponse& response) -> bool;
    static auto parse_header_field(const std::string& line,
                                   std::unordered_map<std::string, std::string>& headers) -> bool;
    static auto parse_chunk_size(const std::string& line) -> size_t;
};

class HTTPHandler {
  public:
    HTTPHandler() = default;
    virtual ~HTTPHandler() = default;

    // Delete copy and move operations
    HTTPHandler(const HTTPHandler&) = delete;
    auto operator=(const HTTPHandler&) -> HTTPHandler& = delete;
    HTTPHandler(HTTPHandler&&) = delete;
    auto operator=(HTTPHandler&&) -> HTTPHandler& = delete;

    // Handle HTTP request
    virtual auto handle_request(const HTTPRequest& request, HTTPResponse& response,
                                Socket& socket) -> Task = 0;
};

} // namespace spaznet
