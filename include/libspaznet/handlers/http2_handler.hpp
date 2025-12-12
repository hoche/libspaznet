#pragma once

#include <cstdint>
#include <libspaznet/handlers/http_handler.hpp> // Reuse HTTP structures where possible
#include <libspaznet/io_context.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spaznet {

// Forward declaration
class Socket;

// HTTP/2 Frame Types per RFC 9113 Section 4.1
enum class HTTP2FrameType : uint8_t {
    DATA = 0x0,
    HEADERS = 0x1,
    PRIORITY = 0x2,
    RST_STREAM = 0x3,
    SETTINGS = 0x4,
    PUSH_PROMISE = 0x5,
    PING = 0x6,
    GOAWAY = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION = 0x9
};

// HTTP/2 Frame Flags per RFC 9113 Section 4.2
namespace HTTP2Flags {
constexpr uint8_t END_STREAM = 0x1;
constexpr uint8_t END_HEADERS = 0x4;
constexpr uint8_t PADDED = 0x8;
constexpr uint8_t PRIORITY = 0x20;
constexpr uint8_t ACK = 0x1; // For SETTINGS and PING
} // namespace HTTP2Flags

// HTTP/2 Frame per RFC 9113 Section 4.1
struct HTTP2Frame {
    uint32_t length;     // Frame payload length (24 bits, max 16384)
    HTTP2FrameType type; // Frame type
    uint8_t flags;       // Frame flags
    uint32_t stream_id;  // Stream identifier (31 bits, 0 for connection-level)
    std::vector<uint8_t> payload;

    // Serialize frame to binary format per RFC 9113 Section 4.1
    std::vector<uint8_t> serialize() const;

    // Parse frame from binary format
    static std::optional<HTTP2Frame> parse(const std::vector<uint8_t>& data, size_t& offset);
};

// HTTP/2 Settings per RFC 9113 Section 6.5.2
struct HTTP2Settings {
    uint32_t header_table_size = 4096;    // SETTINGS_HEADER_TABLE_SIZE
    bool enable_push = true;              // SETTINGS_ENABLE_PUSH
    uint32_t max_concurrent_streams = 0;  // SETTINGS_MAX_CONCURRENT_STREAMS (0 = unlimited)
    uint32_t initial_window_size = 65535; // SETTINGS_INITIAL_WINDOW_SIZE
    uint32_t max_frame_size = 16384;      // SETTINGS_MAX_FRAME_SIZE
    uint32_t max_header_list_size = 0;    // SETTINGS_MAX_HEADER_LIST_SIZE (0 = unlimited)

    // Serialize to SETTINGS frame payload
    std::vector<uint8_t> serialize() const;

    // Parse from SETTINGS frame payload
    static HTTP2Settings parse(const std::vector<uint8_t>& payload);
};

// HTTP/2 Stream State per RFC 9113 Section 5.1
enum class HTTP2StreamState {
    IDLE,
    RESERVED_LOCAL,
    RESERVED_REMOTE,
    OPEN,
    HALF_CLOSED_LOCAL,
    HALF_CLOSED_REMOTE,
    CLOSED
};

// HTTP/2 Request per RFC 9113 Section 8.1
struct HTTP2Request {
    uint32_t stream_id;
    std::string method;
    std::string path;                                     // Request path (from :path pseudo-header)
    std::unordered_map<std::string, std::string> headers; // Includes pseudo-headers
    std::vector<uint8_t> body;

    // Extract pseudo-headers
    std::optional<std::string> get_pseudo_header(const std::string& name) const;

    // Get regular headers (non-pseudo)
    std::unordered_map<std::string, std::string> get_regular_headers() const;
};

// HTTP/2 Response per RFC 9113 Section 8.1
struct HTTP2Response {
    uint32_t stream_id;
    int status_code = 200;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    // Convert to HTTP2Frame(s) - may require multiple frames for large responses
    std::vector<HTTP2Frame> to_frames(uint32_t max_frame_size = 16384) const;

    // Backward compatibility: convert to single frame (HEADERS only, no body)
    HTTP2Frame to_frame() const {
        auto frames = to_frames();
        if (!frames.empty()) {
            return frames[0];
        }
        HTTP2Frame empty;
        empty.type = HTTP2FrameType::HEADERS;
        empty.stream_id = stream_id;
        return empty;
    }

    // Set pseudo-header :status
    void set_status(int code, const std::string& reason = "");
};

// Simplified HPACK implementation (RFC 7541) - basic version
class HPACK {
  public:
    // Encode headers to HPACK format
    static std::vector<uint8_t> encode_headers(
        const std::unordered_map<std::string, std::string>& headers);

    // Decode headers from HPACK format
    static std::unordered_map<std::string, std::string> decode_headers(
        const std::vector<uint8_t>& data);

    // Get static header table entry
    static const std::pair<std::string, std::string>& get_static_table_entry(size_t index);
    static size_t get_static_table_size();
};

// HTTP/2 Parser per RFC 9113
class HTTP2Parser {
  public:
    enum class ParseResult { Success, Incomplete, Error, NeedMoreData };

    // Parse HTTP/2 connection preface (RFC 9113 Section 3.5)
    static bool parse_connection_preface(const std::vector<uint8_t>& data, size_t& offset);

    // Parse HTTP/2 frame
    static ParseResult parse_frame(const std::vector<uint8_t>& data, size_t& offset,
                                   HTTP2Frame& frame);

    // Parse HEADERS frame payload into request
    static ParseResult parse_headers_frame(const HTTP2Frame& frame, HTTP2Request& request);

    // Parse HEADERS frame payload into response
    static ParseResult parse_headers_frame(const HTTP2Frame& frame, HTTP2Response& response);

    // Build HEADERS frame from request
    static HTTP2Frame build_headers_frame(const HTTP2Request& request, uint32_t stream_id,
                                          bool end_headers = true, bool end_stream = false);

    // Build HEADERS frame from response
    static HTTP2Frame build_headers_frame(const HTTP2Response& response, uint32_t stream_id,
                                          bool end_headers = true, bool end_stream = false);

    // Build DATA frame
    static HTTP2Frame build_data_frame(uint32_t stream_id, const std::vector<uint8_t>& data,
                                       bool end_stream = false);

    // Build SETTINGS frame
    static HTTP2Frame build_settings_frame(const HTTP2Settings& settings, bool ack = false);

    // Build GOAWAY frame
    static HTTP2Frame build_goaway_frame(uint32_t last_stream_id, uint32_t error_code);

    // Build RST_STREAM frame
    static HTTP2Frame build_rst_stream_frame(uint32_t stream_id, uint32_t error_code);

    // Build WINDOW_UPDATE frame
    static HTTP2Frame build_window_update_frame(uint32_t stream_id, uint32_t window_size_increment);

    // Build PING frame
    static HTTP2Frame build_ping_frame(const std::vector<uint8_t>& opaque_data, bool ack = false);
};

// HTTP/2 Connection Manager
class HTTP2Connection {
  public:
    HTTP2Connection();

    // Process incoming frame
    HTTP2Parser::ParseResult process_frame(const HTTP2Frame& frame);

    // Get current settings
    const HTTP2Settings& get_settings() const {
        return settings_;
    }

    // Update settings
    void update_settings(const HTTP2Settings& settings);

    // Get stream state
    HTTP2StreamState get_stream_state(uint32_t stream_id) const;

    // Check if stream is valid
    bool is_valid_stream(uint32_t stream_id) const;

  private:
    HTTP2Settings settings_;
    std::unordered_map<uint32_t, HTTP2StreamState> streams_;
    uint32_t next_stream_id_;
    bool client_preface_received_;

    void initialize_stream(uint32_t stream_id);
    void close_stream(uint32_t stream_id);
};

class HTTP2Handler {
  public:
    virtual ~HTTP2Handler() = default;

    // Handle HTTP/2 request
    virtual Task handle_request(const HTTP2Request& request, HTTP2Response& response,
                                Socket& socket) = 0;

    // Handle HTTP/2 frame (for advanced frame handling)
    virtual Task handle_frame(const HTTP2Frame& frame, Socket& socket) = 0;

    // Handle connection-level frames (SETTINGS, PING, GOAWAY, etc.)
    virtual Task handle_connection_frame(const HTTP2Frame& frame, Socket& socket) {
        co_return; // Default: no-op
    }
};

} // namespace spaznet
