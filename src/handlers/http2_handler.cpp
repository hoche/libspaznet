#include <algorithm>
#include <cassert>
#include <cstring>
#include <format>
#include <libspaznet/handlers/http2_handler.hpp>
#include <libspaznet/utils/binary_utils.hpp>
#include <libspaznet/utils/number_utils.hpp>
#include <sstream>
#include <stdexcept>

// This file implements HTTP/2 framing/parsing and contains many protocol-defined constants.
// We suppress a set of noisy style checks to keep clang-tidy actionable.
// NOLINTBEGIN(
//   cppcoreguidelines-avoid-magic-numbers,
//   readability-magic-numbers,
//   modernize-use-trailing-return-type,
//   cppcoreguidelines-narrowing-conversions,
//   readability-braces-around-statements,
//   readability-identifier-length,
//   readability-function-cognitive-complexity,
//   readability-convert-member-functions-to-static,
//   readability-simplify-boolean-expr,
//   cppcoreguidelines-use-default-member-init,
//   modernize-use-default-member-init
// )

namespace spaznet {

// HTTP/2 Connection Preface (RFC 9113 Section 3.5)
constexpr const char* HTTP2_CONNECTION_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr size_t HTTP2_CONNECTION_PREFACE_LEN = 24;

// HTTP/2 Frame serialization per RFC 9113 Section 4.1
std::vector<uint8_t> HTTP2Frame::serialize() const {
    std::vector<uint8_t> result;
    result.reserve(9 + payload.size()); // 9-byte header + payload

    // Length (24 bits, big-endian)
    auto len_bytes = BinaryUtils::encode_uint24_be(length);
    result.insert(result.end(), len_bytes.begin(), len_bytes.end());

    // Type (8 bits)
    result.push_back(static_cast<uint8_t>(type));

    // Flags (8 bits)
    result.push_back(flags);

    // Stream ID (31 bits, big-endian, R bit = 0)
    uint32_t sid = stream_id & 0x7FFFFFFF;
    auto sid_bytes = BinaryUtils::encode_uint32_be(sid);
    result.insert(result.end(), sid_bytes.begin(), sid_bytes.end());

    // Payload
    result.insert(result.end(), payload.begin(), payload.end());

    return result;
}

// HTTP/2 Frame parsing per RFC 9113 Section 4.1
std::optional<HTTP2Frame> HTTP2Frame::parse(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 9 > data.size()) {
        return std::nullopt; // Not enough data for frame header
    }

    HTTP2Frame frame;

    // Parse length (24 bits, big-endian)
    frame.length = BinaryUtils::decode_uint24_be(&data[offset]);
    offset += 3;

    // Validate length (max 16384 per RFC 9113 Section 4.1)
    if (frame.length > 16384) {
        return std::nullopt;
    }

    // Parse type
    frame.type = static_cast<HTTP2FrameType>(data[offset++]);

    // Parse flags
    frame.flags = data[offset++];

    // Parse stream ID (31 bits, big-endian)
    frame.stream_id = BinaryUtils::decode_uint32_be(&data[offset]) & 0x7FFFFFFF;
    offset += 4;

    // Parse payload
    if (offset + frame.length > data.size()) {
        return std::nullopt; // Not enough data for payload
    }

    frame.payload.assign(data.begin() + offset, data.begin() + offset + frame.length);
    offset += frame.length;

    return frame;
}

// HTTP2Settings serialization
std::vector<uint8_t> HTTP2Settings::serialize() const {
    std::vector<uint8_t> result;

    // SETTINGS_HEADER_TABLE_SIZE (0x1)
    result.push_back(0x00);
    result.push_back(0x01);
    result.push_back((header_table_size >> 24) & 0xFF);
    result.push_back((header_table_size >> 16) & 0xFF);
    result.push_back((header_table_size >> 8) & 0xFF);
    result.push_back(header_table_size & 0xFF);

    // SETTINGS_ENABLE_PUSH (0x2)
    result.push_back(0x00);
    result.push_back(0x02);
    result.push_back(0x00);
    result.push_back(0x00);
    result.push_back(0x00);
    result.push_back(enable_push ? 0x01 : 0x00);

    // SETTINGS_MAX_CONCURRENT_STREAMS (0x3)
    result.push_back(0x00);
    result.push_back(0x03);
    result.push_back((max_concurrent_streams >> 24) & 0xFF);
    result.push_back((max_concurrent_streams >> 16) & 0xFF);
    result.push_back((max_concurrent_streams >> 8) & 0xFF);
    result.push_back(max_concurrent_streams & 0xFF);

    // SETTINGS_INITIAL_WINDOW_SIZE (0x4)
    result.push_back(0x00);
    result.push_back(0x04);
    result.push_back((initial_window_size >> 24) & 0xFF);
    result.push_back((initial_window_size >> 16) & 0xFF);
    result.push_back((initial_window_size >> 8) & 0xFF);
    result.push_back(initial_window_size & 0xFF);

    // SETTINGS_MAX_FRAME_SIZE (0x5)
    result.push_back(0x00);
    result.push_back(0x05);
    result.push_back((max_frame_size >> 24) & 0xFF);
    result.push_back((max_frame_size >> 16) & 0xFF);
    result.push_back((max_frame_size >> 8) & 0xFF);
    result.push_back(max_frame_size & 0xFF);

    // SETTINGS_MAX_HEADER_LIST_SIZE (0x6)
    result.push_back(0x00);
    result.push_back(0x06);
    result.push_back((max_header_list_size >> 24) & 0xFF);
    result.push_back((max_header_list_size >> 16) & 0xFF);
    result.push_back((max_header_list_size >> 8) & 0xFF);
    result.push_back(max_header_list_size & 0xFF);

    return result;
}

// HTTP2Settings parsing
HTTP2Settings HTTP2Settings::parse(const std::vector<uint8_t>& payload) {
    HTTP2Settings settings;

    for (size_t i = 0; i + 6 <= payload.size(); i += 6) {
        uint16_t id = (static_cast<uint16_t>(payload[i]) << 8) | payload[i + 1];
        uint32_t value = (static_cast<uint32_t>(payload[i + 2]) << 24) |
                         (static_cast<uint32_t>(payload[i + 3]) << 16) |
                         (static_cast<uint32_t>(payload[i + 4]) << 8) |
                         static_cast<uint32_t>(payload[i + 5]);

        switch (id) {
            case 0x1: // SETTINGS_HEADER_TABLE_SIZE
                settings.header_table_size = value;
                break;
            case 0x2: // SETTINGS_ENABLE_PUSH
                settings.enable_push = (value != 0);
                break;
            case 0x3: // SETTINGS_MAX_CONCURRENT_STREAMS
                settings.max_concurrent_streams = value;
                break;
            case 0x4: // SETTINGS_INITIAL_WINDOW_SIZE
                settings.initial_window_size = value;
                break;
            case 0x5: // SETTINGS_MAX_FRAME_SIZE
                if (value >= 16384 && value <= 16777215) {
                    settings.max_frame_size = value;
                }
                break;
            case 0x6: // SETTINGS_MAX_HEADER_LIST_SIZE
                settings.max_header_list_size = value;
                break;
        }
    }

    return settings;
}

// HTTP2Request helper methods
std::optional<std::string> HTTP2Request::get_pseudo_header(const std::string& name) const {
    auto it = headers.find(name);
    if (it != headers.end() && it->first[0] == ':') {
        return it->second;
    }
    return std::nullopt;
}

std::unordered_map<std::string, std::string> HTTP2Request::get_regular_headers() const {
    std::unordered_map<std::string, std::string> regular;
    for (const auto& [key, value] : headers) {
        if (key[0] != ':') {
            regular[key] = value;
        }
    }
    return regular;
}

// HTTP2Response helper methods
void HTTP2Response::set_status(int code, const std::string& reason) {
    status_code = code;
    headers[":status"] = std::format("{}", code);
    if (!reason.empty()) {
        headers["reason"] = reason;
    }
}

std::vector<HTTP2Frame> HTTP2Response::to_frames(uint32_t max_frame_size) const {
    std::vector<HTTP2Frame> frames;

    // Build headers frame
    HTTP2Frame headers_frame =
        HTTP2Parser::build_headers_frame(*this, stream_id, true, body.empty());
    frames.push_back(headers_frame);

    // Build data frames if body is not empty
    if (!body.empty()) {
        size_t offset = 0;
        while (offset < body.size()) {
            size_t chunk_size = std::min(static_cast<size_t>(max_frame_size), body.size() - offset);
            bool end_stream = (offset + chunk_size >= body.size());

            std::vector<uint8_t> chunk(body.begin() + offset, body.begin() + offset + chunk_size);
            HTTP2Frame data_frame = HTTP2Parser::build_data_frame(stream_id, chunk, end_stream);
            frames.push_back(data_frame);

            offset += chunk_size;
        }
    }

    return frames;
}

// HPACK Static Table (RFC 7541 Appendix B) - helper functions
namespace {
const std::vector<std::pair<std::string, std::string>>& get_static_table() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"", ""}, // Index 0 unused
        {":authority", ""},
        {":method", "GET"},
        {":method", "POST"},
        {":path", "/"},
        {":path", "/index.html"},
        {":scheme", "http"},
        {":scheme", "https"},
        {":status", "200"},
        {":status", "204"},
        {":status", "206"},
        {":status", "304"},
        {":status", "400"},
        {":status", "404"},
        {":status", "500"},
        {"accept-charset", ""},
        {"accept-encoding", "gzip, deflate"},
        {"accept-language", ""},
        {"accept-ranges", ""},
        {"accept", ""},
        {"access-control-allow-origin", ""},
        {"age", ""},
        {"allow", ""},
        {"authorization", ""},
        {"cache-control", ""},
        {"content-disposition", ""},
        {"content-encoding", ""},
        {"content-language", ""},
        {"content-length", ""},
        {"content-location", ""},
        {"content-range", ""},
        {"content-type", ""},
        {"cookie", ""},
        {"date", ""},
        {"etag", ""},
        {"expect", ""},
        {"expires", ""},
        {"from", ""},
        {"host", ""},
        {"if-match", ""},
        {"if-modified-since", ""},
        {"if-none-match", ""},
        {"if-range", ""},
        {"if-unmodified-since", ""},
        {"last-modified", ""},
        {"link", ""},
        {"location", ""},
        {"max-forwards", ""},
        {"proxy-authenticate", ""},
        {"proxy-authorization", ""},
        {"range", ""},
        {"referer", ""},
        {"refresh", ""},
        {"retry-after", ""},
        {"server", ""},
        {"set-cookie", ""},
        {"strict-transport-security", ""},
        {"transfer-encoding", ""},
        {"user-agent", ""},
        {"vary", ""},
        {"via", ""},
        {"www-authenticate", ""}};
    return table;
}
} // namespace

const std::pair<std::string, std::string>& HPACK::get_static_table_entry(size_t index) {
    const auto& table = get_static_table();
    if (index < table.size()) {
        return table[index];
    }
    static const std::pair<std::string, std::string> empty = {"", ""};
    return empty;
}

size_t HPACK::get_static_table_size() {
    return get_static_table().size();
}

// Simplified HPACK encoding (basic implementation)
std::vector<uint8_t> HPACK::encode_headers(
    const std::unordered_map<std::string, std::string>& headers) {
    std::vector<uint8_t> result;

    // Simple encoding: for each header, try to find in static table
    // If found, use index; otherwise, encode as literal
    const auto& static_table = get_static_table();
    for (const auto& [name, value] : headers) {
        // Try to find in static table
        bool found = false;
        for (size_t i = 1; i < static_table.size(); ++i) {
            if (static_table[i].first == name &&
                (value.empty() || static_table[i].second == value)) {
                // Found in static table - use index (6-bit prefix with 1-bit flag)
                if (i < 64) {
                    result.push_back(0x80 | static_cast<uint8_t>(i));
                } else {
                    // Indexed header field (6-bit prefix)
                    result.push_back(0x80 | 0x3F);
                    result.push_back(static_cast<uint8_t>(i - 64));
                }
                found = true;
                break;
            }
        }

        if (!found) {
            // Literal header field - never indexed (4-bit prefix)
            result.push_back(0x10);

            // Encode name length
            if (name.size() < 31) {
                result.push_back(static_cast<uint8_t>(name.size()));
            } else {
                result.push_back(0x1F);
                // Would need to encode larger lengths here
            }

            // Encode name
            result.insert(result.end(), name.begin(), name.end());

            // Encode value length
            if (value.size() < 31) {
                result.push_back(static_cast<uint8_t>(value.size()));
            } else {
                result.push_back(0x1F);
            }

            // Encode value
            result.insert(result.end(), value.begin(), value.end());
        }
    }

    return result;
}

// Simplified HPACK decoding
std::unordered_map<std::string, std::string> HPACK::decode_headers(
    const std::vector<uint8_t>& data) {
    std::unordered_map<std::string, std::string> headers;
    size_t offset = 0;

    while (offset < data.size()) {
        uint8_t first_byte = data[offset++];

        const auto& static_table = get_static_table();
        if ((first_byte & 0x80) != 0) {
            // Indexed header field
            uint32_t index = first_byte & 0x7F;
            if (index < static_table.size()) {
                const auto& entry = static_table[index];
                if (!entry.second.empty()) {
                    headers[entry.first] = entry.second;
                } else {
                    // Header name only - would need to read value
                }
            }
        } else if ((first_byte & 0xE0) == 0x20) {
            // Dynamic table size update - skip
            continue;
        } else if ((first_byte & 0xC0) == 0x40) {
            // Literal header field - indexed name
            uint32_t name_index = first_byte & 0x3F;
            // Read value length and value
            if (offset < data.size()) {
                uint8_t value_len = data[offset++];
                if (offset + value_len <= data.size()) {
                    std::string value(data.begin() + offset, data.begin() + offset + value_len);
                    offset += value_len;
                    if (name_index < static_table.size()) {
                        headers[static_table[name_index].first] = value;
                    }
                }
            }
        } else if ((first_byte & 0xF0) == 0x10) {
            // Literal header field - never indexed
            // Read name length
            if (offset < data.size()) {
                uint8_t name_len = first_byte & 0x0F;
                if (name_len == 0x0F) {
                    // Extended length - simplified, assume next byte
                    if (offset < data.size()) {
                        name_len = data[offset++];
                    }
                }

                // Read name
                if (offset + name_len <= data.size()) {
                    std::string name(data.begin() + offset, data.begin() + offset + name_len);
                    offset += name_len;

                    // Read value length
                    if (offset < data.size()) {
                        uint8_t value_len = data[offset++];
                        if (value_len == 0x0F) {
                            if (offset < data.size()) {
                                value_len = data[offset++];
                            }
                        }

                        // Read value
                        if (offset + value_len <= data.size()) {
                            std::string value(data.begin() + offset,
                                              data.begin() + offset + value_len);
                            offset += value_len;
                            headers[name] = value;
                        }
                    }
                }
            }
        }
    }

    return headers;
}

// HTTP2Parser implementation
bool HTTP2Parser::parse_connection_preface(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + HTTP2_CONNECTION_PREFACE_LEN > data.size()) {
        return false;
    }

    std::string preface(data.begin() + offset,
                        data.begin() + offset + HTTP2_CONNECTION_PREFACE_LEN);
    if (preface == HTTP2_CONNECTION_PREFACE) {
        offset += HTTP2_CONNECTION_PREFACE_LEN;
        return true;
    }

    return false;
}

HTTP2Parser::ParseResult HTTP2Parser::parse_frame(const std::vector<uint8_t>& data, size_t& offset,
                                                  HTTP2Frame& frame) {
    auto parsed = HTTP2Frame::parse(data, offset);
    if (!parsed) {
        return ParseResult::Incomplete;
    }

    frame = *parsed;
    return ParseResult::Success;
}

HTTP2Parser::ParseResult HTTP2Parser::parse_headers_frame(const HTTP2Frame& frame,
                                                          HTTP2Request& request) {
    if (frame.type != HTTP2FrameType::HEADERS) {
        return ParseResult::Error;
    }

    request.stream_id = frame.stream_id;

    // Decode HPACK headers
    auto headers = HPACK::decode_headers(frame.payload);
    request.headers = headers;

    // Extract pseudo-headers and method/path
    auto method = headers.find(":method");
    auto path = headers.find(":path");

    if (method != headers.end()) {
        request.method = method->second;
    }
    if (path != headers.end()) {
        request.path = path->second;
    }

    return ParseResult::Success;
}

HTTP2Parser::ParseResult HTTP2Parser::parse_headers_frame(const HTTP2Frame& frame,
                                                          HTTP2Response& response) {
    if (frame.type != HTTP2FrameType::HEADERS) {
        return ParseResult::Error;
    }

    response.stream_id = frame.stream_id;

    // Decode HPACK headers
    auto headers = HPACK::decode_headers(frame.payload);
    response.headers = headers;

    // Extract :status pseudo-header
    auto status = headers.find(":status");
    if (status != headers.end()) {
        auto parsed_status = NumberUtils::parse_int(status->second);
        response.status_code = parsed_status.value_or(200);
    }

    return ParseResult::Success;
}

HTTP2Frame HTTP2Parser::build_headers_frame(const HTTP2Request& request, uint32_t stream_id,
                                            bool end_headers, bool end_stream) {
    HTTP2Frame frame;
    frame.type = HTTP2FrameType::HEADERS;
    frame.flags = 0;
    if (end_headers)
        frame.flags |= HTTP2Flags::END_HEADERS;
    if (end_stream)
        frame.flags |= HTTP2Flags::END_STREAM;
    frame.stream_id = stream_id;

    // Encode headers with HPACK
    frame.payload = HPACK::encode_headers(request.headers);
    frame.length = frame.payload.size();

    return frame;
}

HTTP2Frame HTTP2Parser::build_headers_frame(const HTTP2Response& response, uint32_t stream_id,
                                            bool end_headers, bool end_stream) {
    HTTP2Frame frame;
    frame.type = HTTP2FrameType::HEADERS;
    frame.flags = 0;
    if (end_headers)
        frame.flags |= HTTP2Flags::END_HEADERS;
    if (end_stream)
        frame.flags |= HTTP2Flags::END_STREAM;
    frame.stream_id = stream_id;

    // Ensure :status is set
    std::unordered_map<std::string, std::string> headers = response.headers;
    if (headers.find(":status") == headers.end()) {
        headers[":status"] = std::format("{}", response.status_code);
    }

    // Encode headers with HPACK
    frame.payload = HPACK::encode_headers(headers);
    frame.length = frame.payload.size();

    return frame;
}

HTTP2Frame HTTP2Parser::build_data_frame(uint32_t stream_id, const std::vector<uint8_t>& data,
                                         bool end_stream) {
    HTTP2Frame frame;
    frame.type = HTTP2FrameType::DATA;
    frame.flags = end_stream ? HTTP2Flags::END_STREAM : 0;
    frame.stream_id = stream_id;
    frame.payload = data;
    frame.length = data.size();

    return frame;
}

HTTP2Frame HTTP2Parser::build_settings_frame(const HTTP2Settings& settings, bool ack) {
    HTTP2Frame frame;
    frame.type = HTTP2FrameType::SETTINGS;
    frame.flags = ack ? HTTP2Flags::ACK : 0;
    frame.stream_id = 0; // Connection-level frame
    // ACK frames must have no payload per RFC 9113 Section 6.5
    if (ack) {
        frame.payload.clear();
        frame.length = 0;
    } else {
        frame.payload = settings.serialize();
        frame.length = frame.payload.size();
    }

    return frame;
}

HTTP2Frame HTTP2Parser::build_goaway_frame(uint32_t last_stream_id, uint32_t error_code) {
    HTTP2Frame frame;
    frame.type = HTTP2FrameType::GOAWAY;
    frame.flags = 0;
    frame.stream_id = 0; // Connection-level frame

    frame.payload.resize(8);
    frame.payload[0] = (last_stream_id >> 24) & 0xFF;
    frame.payload[1] = (last_stream_id >> 16) & 0xFF;
    frame.payload[2] = (last_stream_id >> 8) & 0xFF;
    frame.payload[3] = last_stream_id & 0xFF;
    frame.payload[4] = (error_code >> 24) & 0xFF;
    frame.payload[5] = (error_code >> 16) & 0xFF;
    frame.payload[6] = (error_code >> 8) & 0xFF;
    frame.payload[7] = error_code & 0xFF;

    frame.length = frame.payload.size();

    return frame;
}

HTTP2Frame HTTP2Parser::build_rst_stream_frame(uint32_t stream_id, uint32_t error_code) {
    HTTP2Frame frame;
    frame.type = HTTP2FrameType::RST_STREAM;
    frame.flags = 0;
    frame.stream_id = stream_id;

    frame.payload.resize(4);
    frame.payload[0] = (error_code >> 24) & 0xFF;
    frame.payload[1] = (error_code >> 16) & 0xFF;
    frame.payload[2] = (error_code >> 8) & 0xFF;
    frame.payload[3] = error_code & 0xFF;

    frame.length = frame.payload.size();

    return frame;
}

HTTP2Frame HTTP2Parser::build_window_update_frame(uint32_t stream_id,
                                                  uint32_t window_size_increment) {
    HTTP2Frame frame;
    frame.type = HTTP2FrameType::WINDOW_UPDATE;
    frame.flags = 0;
    frame.stream_id = stream_id;

    frame.payload.resize(4);
    frame.payload[0] = (window_size_increment >> 24) & 0xFF;
    frame.payload[1] = (window_size_increment >> 16) & 0xFF;
    frame.payload[2] = (window_size_increment >> 8) & 0xFF;
    frame.payload[3] = window_size_increment & 0xFF;

    frame.length = frame.payload.size();

    return frame;
}

HTTP2Frame HTTP2Parser::build_ping_frame(const std::vector<uint8_t>& opaque_data, bool ack) {
    HTTP2Frame frame;
    frame.type = HTTP2FrameType::PING;
    frame.flags = ack ? HTTP2Flags::ACK : 0;
    frame.stream_id = 0; // Connection-level frame

    if (opaque_data.size() == 8) {
        frame.payload = opaque_data;
    } else {
        frame.payload.resize(8, 0);
    }

    frame.length = frame.payload.size();

    return frame;
}

// HTTP2Connection implementation
HTTP2Connection::HTTP2Connection() : next_stream_id_(1), client_preface_received_(false) {}

HTTP2Parser::ParseResult HTTP2Connection::process_frame(const HTTP2Frame& frame) {
    // Validate stream ID
    if (frame.stream_id != 0 && !is_valid_stream(frame.stream_id)) {
        // Invalid stream - should send RST_STREAM
        return HTTP2Parser::ParseResult::Error;
    }

    // Update stream state based on frame type
    if (frame.stream_id != 0) {
        switch (frame.type) {
            case HTTP2FrameType::HEADERS:
                if (get_stream_state(frame.stream_id) == HTTP2StreamState::IDLE) {
                    initialize_stream(frame.stream_id);
                }
                break;
            case HTTP2FrameType::DATA:
                // Stream must be OPEN or HALF_CLOSED_REMOTE
                break;
            default:
                break;
        }

        // Check END_STREAM flag
        if ((frame.flags & HTTP2Flags::END_STREAM) != 0) {
            close_stream(frame.stream_id);
        }
    }

    return HTTP2Parser::ParseResult::Success;
}

void HTTP2Connection::update_settings(const HTTP2Settings& settings) {
    settings_ = settings;
}

HTTP2StreamState HTTP2Connection::get_stream_state(uint32_t stream_id) const {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        return it->second;
    }
    return HTTP2StreamState::IDLE;
}

bool HTTP2Connection::is_valid_stream(uint32_t stream_id) const {
    if (stream_id == 0) {
        return false; // Stream 0 is reserved for connection-level frames
    }

    // Client-initiated streams must be odd (RFC 9113 Section 5.1.1)
    // Server-initiated streams must be even
    // For now, accept both
    return true;
}

void HTTP2Connection::initialize_stream(uint32_t stream_id) {
    streams_[stream_id] = HTTP2StreamState::OPEN;
}

void HTTP2Connection::close_stream(uint32_t stream_id) {
    streams_[stream_id] = HTTP2StreamState::CLOSED;
}

// NOLINTEND(
//   cppcoreguidelines-avoid-magic-numbers,
//   readability-magic-numbers,
//   modernize-use-trailing-return-type,
//   cppcoreguidelines-narrowing-conversions,
//   readability-braces-around-statements,
//   readability-identifier-length,
//   readability-function-cognitive-complexity,
//   readability-convert-member-functions-to-static,
//   readability-simplify-boolean-expr,
//   cppcoreguidelines-use-default-member-init,
//   modernize-use-default-member-init
// )

} // namespace spaznet
