#pragma once

#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <libspaznet/reactor/response_writer.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spaznet::http2 {

inline constexpr int DEFAULT_HTTP_STATUS_CODE = 200;

// HTTP/2 Frame Types per RFC 9113 Section 4.1
enum class FrameType : uint8_t {
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
namespace Flags {
constexpr uint8_t END_STREAM = 0x1;
constexpr uint8_t END_HEADERS = 0x4;
constexpr uint8_t PADDED = 0x8;
constexpr uint8_t PRIORITY = 0x20;
constexpr uint8_t ACK = 0x1; // For SETTINGS and PING
} // namespace Flags

// HTTP/2 Frame per RFC 9113 Section 4.1
struct Frame {
    uint32_t length{};     // Frame payload length (24 bits, max 16384)
    FrameType type{}; // Frame type
    uint8_t flags{};       // Frame flags
    uint32_t stream_id{};  // Stream identifier (31 bits, 0 for connection-level)
    std::vector<uint8_t> payload;

    // Serialize frame to binary format per RFC 9113 Section 4.1
    [[nodiscard]] auto serialize() const -> std::vector<uint8_t>;

    // Parse frame from binary format
    static auto parse(const std::vector<uint8_t>& data,
                      size_t& offset) -> std::optional<Frame>;
};

// HTTP/2 Settings per RFC 9113 Section 6.5.2
struct Settings {
    uint32_t header_table_size = 4096;    // SETTINGS_HEADER_TABLE_SIZE
    bool enable_push = true;              // SETTINGS_ENABLE_PUSH
    uint32_t max_concurrent_streams = 0;  // SETTINGS_MAX_CONCURRENT_STREAMS (0 = unlimited)
    uint32_t initial_window_size = 65535; // SETTINGS_INITIAL_WINDOW_SIZE
    uint32_t max_frame_size = 16384;      // SETTINGS_MAX_FRAME_SIZE
    uint32_t max_header_list_size = 0;    // SETTINGS_MAX_HEADER_LIST_SIZE (0 = unlimited)

    // Serialize to SETTINGS frame payload
    [[nodiscard]] auto serialize() const -> std::vector<uint8_t>;

    // Parse from SETTINGS frame payload (starts from RFC defaults).
    static auto parse(const std::vector<uint8_t>& payload) -> Settings;

    // Apply a SETTINGS frame payload onto an existing Settings, updating
    // only the parameters actually present. RFC 9113 §6.5 defines SETTINGS
    // as a cumulative update: absent parameters retain their prior value,
    // so `parse` (which resets to defaults) is wrong for incremental frames.
    static auto parse_into(const std::vector<uint8_t>& payload, Settings& into) -> void;
};

// HTTP/2 Stream State per RFC 9113 Section 5.1
enum class StreamState : uint8_t {
    IDLE,
    RESERVED_LOCAL,
    RESERVED_REMOTE,
    OPEN,
    HALF_CLOSED_LOCAL,
    HALF_CLOSED_REMOTE,
    CLOSED
};

// HTTP/2 Request per RFC 9113 Section 8.1
struct Request {
    uint32_t stream_id;
    std::string method;
    std::string path;                                     // Request path (from :path pseudo-header)
    std::unordered_map<std::string, std::string> headers; // Includes pseudo-headers
    std::vector<uint8_t> body;

    // Extract pseudo-headers
    auto get_pseudo_header(const std::string& name) const -> std::optional<std::string>;

    // Get regular headers (non-pseudo)
    auto get_regular_headers() const -> std::unordered_map<std::string, std::string>;
};

inline constexpr uint32_t DEFAULT_MAX_FRAME_SIZE = 16384;

// HTTP/2 Response per RFC 9113 Section 8.1
struct Response {
    uint32_t stream_id;
    int status_code = DEFAULT_HTTP_STATUS_CODE; // Defined in http_handler.hpp
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    // Convert to Frame(s) - may require multiple frames for large responses
    auto to_frames(uint32_t max_frame_size = DEFAULT_MAX_FRAME_SIZE) const
        -> std::vector<Frame>;

    // Backward compatibility: convert to single frame (HEADERS only, no body)
    auto to_frame() const -> Frame {
        auto frames = to_frames();
        if (!frames.empty()) {
            return frames[0];
        }
        Frame empty;
        empty.type = FrameType::HEADERS;
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
    static auto encode_headers(const std::unordered_map<std::string, std::string>& headers)
        -> std::vector<uint8_t>;

    // Decode headers from HPACK format
    static auto decode_headers(const std::vector<uint8_t>& data)
        -> std::unordered_map<std::string, std::string>;

    // Get static header table entry
    static auto get_static_table_entry(size_t index) -> const std::pair<std::string, std::string>&;
    static auto get_static_table_size() -> size_t;
};

// HTTP/2 Parser per RFC 9113
class Parser {
  public:
    enum class ParseResult : uint8_t { Success, Incomplete, Error, NeedMoreData };

    // Parse HTTP/2 connection preface (RFC 9113 Section 3.5)
    static auto parse_connection_preface(const std::vector<uint8_t>& data, size_t& offset) -> bool;

    // Parse HTTP/2 frame
    static auto parse_frame(const std::vector<uint8_t>& data, size_t& offset,
                            Frame& frame) -> ParseResult;

    // Parse HEADERS frame payload into request
    static auto parse_headers_frame(const Frame& frame, Request& request) -> ParseResult;

    // Parse HEADERS frame payload into response
    static auto parse_headers_frame(const Frame& frame,
                                    Response& response) -> ParseResult;

    // Build HEADERS frame from request
    static auto build_headers_frame(const Request& request, uint32_t stream_id,
                                    bool end_headers = true, bool end_stream = false) -> Frame;

    // Build HEADERS frame from response
    static auto build_headers_frame(const Response& response, uint32_t stream_id,
                                    bool end_headers = true, bool end_stream = false) -> Frame;

    // Build DATA frame
    static auto build_data_frame(uint32_t stream_id, const std::vector<uint8_t>& data,
                                 bool end_stream = false) -> Frame;

    // Build SETTINGS frame
    static auto build_settings_frame(const Settings& settings, bool ack = false) -> Frame;

    // Build GOAWAY frame
    static auto build_goaway_frame(uint32_t last_stream_id, uint32_t error_code) -> Frame;

    // Build RST_STREAM frame
    static auto build_rst_stream_frame(uint32_t stream_id, uint32_t error_code) -> Frame;

    // Build WINDOW_UPDATE frame
    static auto build_window_update_frame(uint32_t stream_id,
                                          uint32_t window_size_increment) -> Frame;

    // Build PING frame
    static auto build_ping_frame(const std::vector<uint8_t>& opaque_data,
                                 bool ack = false) -> Frame;
};

// HTTP/2 Connection Manager
class Connection {
  public:
    Connection();

    // Process incoming frame
    auto process_frame(const Frame&) -> Parser::ParseResult;

    // Get current settings
    auto get_settings() const -> const Settings& {
        return settings_;
    }

    // Update settings
    auto update_settings(const Settings& settings) -> void;

    // Get stream state
    auto get_stream_state(uint32_t stream_id) const -> StreamState;

    // Check if stream is valid
    auto is_valid_stream(uint32_t stream_id) const -> bool;

  private:
    Settings settings_;
    std::unordered_map<uint32_t, StreamState> streams_;
    // Placeholders for the unfinished HTTP/2 connection state machine
    // (server-initiated stream id allocation, client preface tracking).
    // The audit flagged them as unused; keep them so the wiring is in
    // place for the rewrite, and silence the warning.
    [[maybe_unused]] uint32_t next_stream_id_{};
    [[maybe_unused]] bool client_preface_received_{};

    void initialize_stream(uint32_t stream_id);
    void close_stream(uint32_t stream_id);
};

// Runtime-neutral: no Task, no co_await, no Socket. Implementations that
// can answer immediately just build a Response and call
// `writer.complete(std::move(response))` before returning — that's the
// entire handler, indistinguishable from a plain synchronous function.
// Implementations that must defer (issue background work, wait on another
// service, etc.) instead move/copy `writer` somewhere durable and call
// `.complete()` from wherever the answer eventually becomes available —
// a callback, a different thread, a timer, or a coroutine suspended on it
// by the dispatcher. Calling `writer.complete()` more than once (including
// via a stashed copy) is safe; only the first call has any effect. See
// include/libspaznet/reactor/response_writer.hpp.
//
// Unlike HTTP/1.1, HTTP/2 multiplexes several requests concurrently on one
// connection: both dispatchers call handle_request() once per stream as
// soon as its HEADERS(+DATA) fully arrive, independent of whether earlier
// streams' ResponseWriters have completed yet, so a slow stream never
// blocks the frame-reading loop or any other stream's handler.
using ResponseWriter = ::spaznet::ResponseWriter<Response>;

class Handler {
  public:
    Handler() = default;
    virtual ~Handler() = default;

    Handler(const Handler&) = delete;
    auto operator=(const Handler&) -> Handler& = delete;
    Handler(Handler&&) = delete;
    auto operator=(Handler&&) -> Handler& = delete;

    // Handle a single HTTP/2 request. The dispatcher decodes HEADERS +
    // DATA frames into `request` (including reassembled body) and, once
    // `writer` completes, serializes the populated Response back as
    // HEADERS + DATA frames on the same stream. See ResponseWriter above
    // for the synchronous vs. deferred contract.
    virtual void handle_request(const Request& request, ResponseWriter writer) = 0;
};

} // namespace spaznet::http2
