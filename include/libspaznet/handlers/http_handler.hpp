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
    std::optional<std::string> get_header(const std::string& name) const;

    // Check if connection should be kept alive per RFC 9112 Section 9.6
    bool should_keep_alive() const;

    // Get Content-Length if present per RFC 9112 Section 8.6
    std::optional<size_t> get_content_length() const;

    // Check if request has chunked transfer encoding per RFC 9112 Section 7.1
    bool is_chunked() const;
};

// HTTP/1.1 Response structure per RFC 9112
struct HTTPResponse {
    std::string version = "1.1";                          // HTTP version
    int status_code = 200;                                // Status code
    std::string reason_phrase = "OK";                     // Reason phrase
    std::unordered_map<std::string, std::string> headers; // Header fields
    std::vector<uint8_t> body;                            // Message body

    void set_header(const std::string& key, const std::string& value) {
        headers[key] = value;
    }

    // Helper to get header (case-insensitive)
    std::optional<std::string> get_header(const std::string& name) const;

    // Set Content-Length header
    void set_content_length(size_t length);

    // Set chunked transfer encoding
    void set_chunked();

    // Serialize response per RFC 9112
    std::vector<uint8_t> serialize() const;

    // Serialize with chunked encoding
    std::vector<uint8_t> serialize_chunked() const;
};

// HTTP/1.1 Parser per RFC 9112
class HTTPParser {
  public:
    enum class ParseResult {
        Success,
        Incomplete, // Need more data
        Error       // Parse error
    };

    // Parse HTTP request from buffer
    static ParseResult parse_request(const std::vector<uint8_t>& buffer, HTTPRequest& request,
                                     size_t& bytes_consumed);

    // Parse HTTP response from buffer
    static ParseResult parse_response(const std::vector<uint8_t>& buffer, HTTPResponse& response,
                                      size_t& bytes_consumed);

    // Parse chunked message body
    static ParseResult parse_chunked_body(const std::vector<uint8_t>& buffer,
                                          std::vector<uint8_t>& body, size_t& bytes_consumed);

    // Helper functions for parsing (public for testing)
    static bool is_token_char(char c);
    static bool is_field_vchar(char c);
    static std::string to_lower(const std::string& str);
    static std::string trim_ows(const std::string& str); // Trim obs-fold and OWS
    static bool parse_request_line(const std::string& line, HTTPRequest& request);
    static bool parse_status_line(const std::string& line, HTTPResponse& response);
    static bool parse_header_field(const std::string& line,
                                   std::unordered_map<std::string, std::string>& headers);
    static size_t parse_chunk_size(const std::string& line);
};

class HTTPHandler {
  public:
    virtual ~HTTPHandler() = default;

    // Handle HTTP request
    virtual Task handle_request(const HTTPRequest& request, HTTPResponse& response,
                                Socket& socket) = 0;
};

} // namespace spaznet
