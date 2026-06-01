#include <algorithm>
#include <cassert>
#include <cstring>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/codec/huffman.hpp>
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

namespace spaznet::http2 {

// HTTP/2 Connection Preface (RFC 9113 Section 3.5)
constexpr const char* CONNECTION_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr size_t CONNECTION_PREFACE_LEN = 24;

// HTTP/2 Frame serialization per RFC 9113 Section 4.1
std::vector<uint8_t> Frame::serialize() const {
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
std::optional<Frame> Frame::parse(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 9 > data.size()) {
        return std::nullopt; // Not enough data for frame header
    }

    Frame frame;

    // Parse length (24 bits, big-endian)
    frame.length = BinaryUtils::decode_uint24_be(&data[offset]);
    offset += 3;

    // Validate length (max 16384 per RFC 9113 Section 4.1)
    if (frame.length > 16384) {
        return std::nullopt;
    }

    // Parse type
    frame.type = static_cast<FrameType>(data[offset++]);

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

// Settings serialization
std::vector<uint8_t> Settings::serialize() const {
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

// Settings parsing
Settings Settings::parse(const std::vector<uint8_t>& payload) {
    Settings settings;

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

// Request helper methods
std::optional<std::string> Request::get_pseudo_header(const std::string& name) const {
    auto it = headers.find(name);
    if (it != headers.end() && it->first[0] == ':') {
        return it->second;
    }
    return std::nullopt;
}

std::unordered_map<std::string, std::string> Request::get_regular_headers() const {
    std::unordered_map<std::string, std::string> regular;
    for (const auto& [key, value] : headers) {
        if (key[0] != ':') {
            regular[key] = value;
        }
    }
    return regular;
}

// Response helper methods
void Response::set_status(int code, const std::string& reason) {
    status_code = code;
    headers[":status"] = std::to_string(code);
    if (!reason.empty()) {
        headers["reason"] = reason;
    }
}

std::vector<Frame> Response::to_frames(uint32_t max_frame_size) const {
    std::vector<Frame> frames;

    // Build headers frame
    Frame headers_frame =
        Parser::build_headers_frame(*this, stream_id, true, body.empty());
    frames.push_back(headers_frame);

    // Build data frames if body is not empty
    if (!body.empty()) {
        size_t offset = 0;
        while (offset < body.size()) {
            size_t chunk_size = std::min(static_cast<size_t>(max_frame_size), body.size() - offset);
            bool end_stream = (offset + chunk_size >= body.size());

            std::vector<uint8_t> chunk(body.begin() + offset, body.begin() + offset + chunk_size);
            Frame data_frame = Parser::build_data_frame(stream_id, chunk, end_stream);
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

// ---- HPACK (RFC 7541) — proper implementation -----------------------
//
// Supports:
//   - Indexed header field (1-bit prefix)
//   - Literal with incremental indexing (2-bit prefix)
//   - Literal without indexing (4-bit prefix)
//   - Literal never indexed (4-bit prefix)
//   - Dynamic table size update (3-bit prefix) — accepted + ignored
//   - Static table lookup
//   - Variable-length prefix-N integers (RFC 7541 §5.1)
//   - Huffman-coded strings on decode (RFC 7541 §5.2 / Appendix B),
//     using the codec from libspaznet/http3/huffman.hpp
//
// We do not maintain a dynamic table on receive — we advertise
// SETTINGS_HEADER_TABLE_SIZE=0 to peers in the dispatcher, which tells
// them not to index against our table.  We accept (and ignore)
// table-size-update frames if a peer sends them anyway.
//
// On encode we emit literal-without-Huffman for everything that isn't
// a pure static-table hit.  That's RFC-conformant and trivial to
// produce; the savings from Huffman-encoding outbound headers are
// small compared with the size of typical response bodies.

namespace {

// Decode a prefix-N integer.  `prefix_bits` is the number of low bits
// of the current byte that hold the integer (1..8).  Returns false on
// truncation; advances `offset` past the encoded integer on success.
bool decode_prefix_int(const std::vector<uint8_t>& data, size_t& offset,
                       uint8_t prefix_bits, uint32_t& out) {
    if (offset >= data.size() || prefix_bits < 1 || prefix_bits > 8) {
        return false;
    }
    const uint32_t prefix_max = (1U << prefix_bits) - 1U;
    uint32_t v = data[offset++] & prefix_max;
    if (v < prefix_max) {
        out = v;
        return true;
    }
    // Multi-byte form.
    uint32_t m = 0;
    while (offset < data.size()) {
        const uint8_t b = data[offset++];
        v += (b & 0x7FU) << m;
        if ((b & 0x80U) == 0) {
            out = v;
            return true;
        }
        m += 7;
        if (m > 28) {
            return false; // overflow guard
        }
    }
    return false; // truncated
}

// Encode a prefix-N integer.  Caller must have already stuffed the
// high bits of the first byte (the pattern that identifies the
// representation); we OR the low `prefix_bits` in.
void encode_prefix_int(std::vector<uint8_t>& out, uint8_t high_bits, uint8_t prefix_bits,
                       uint32_t value) {
    const uint32_t prefix_max = (1U << prefix_bits) - 1U;
    if (value < prefix_max) {
        out.push_back(static_cast<uint8_t>(high_bits | static_cast<uint8_t>(value)));
        return;
    }
    out.push_back(static_cast<uint8_t>(high_bits | static_cast<uint8_t>(prefix_max)));
    value -= prefix_max;
    while (value >= 128) {
        out.push_back(static_cast<uint8_t>((value & 0x7FU) | 0x80U));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

// Decode an HPACK string (RFC 7541 §5.2).  Sets `out` and advances
// `offset`.  Handles Huffman strings via spaznet::codec::huffman_decode.
bool decode_string(const std::vector<uint8_t>& data, size_t& offset, std::string& out) {
    if (offset >= data.size()) {
        return false;
    }
    const bool huffman = (data[offset] & 0x80U) != 0;
    uint32_t length = 0;
    if (!decode_prefix_int(data, offset, 7, length)) {
        return false;
    }
    if (offset + length > data.size()) {
        return false;
    }
    if (huffman) {
        std::string decoded;
        if (!::spaznet::codec::huffman_decode(
                std::span<const uint8_t>(data.data() + offset, length), decoded)) {
            return false;
        }
        out = std::move(decoded);
    } else {
        out.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                   data.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    offset += length;
    return true;
}

// Encode an HPACK string literal-without-Huffman (RFC 7541 §5.2).
// The high bit of the length byte is the Huffman flag — we set it to 0.
void encode_string(std::vector<uint8_t>& out, const std::string& s) {
    encode_prefix_int(out, /*high_bits=*/0x00, /*prefix_bits=*/7,
                      static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

} // namespace

std::vector<uint8_t> HPACK::encode_headers(
    const std::unordered_map<std::string, std::string>& headers) {
    std::vector<uint8_t> result;
    const auto& static_table = get_static_table();

    // Emit pseudo-headers first (RFC 9113 §8.1.2.1: pseudo-headers
    // MUST appear before regular headers).  Sort regular headers
    // alphabetically for deterministic output (tests are easier).
    std::vector<std::pair<std::string, std::string>> pseudo;
    std::vector<std::pair<std::string, std::string>> regular;
    for (const auto& [k, v] : headers) {
        if (!k.empty() && k.front() == ':') {
            pseudo.emplace_back(k, v);
        } else {
            regular.emplace_back(k, v);
        }
    }
    std::sort(pseudo.begin(), pseudo.end());
    std::sort(regular.begin(), regular.end());

    auto emit = [&](const std::string& name, const std::string& value) {
        // First try for a (name, value) exact hit in the static table.
        for (size_t i = 1; i < static_table.size(); ++i) {
            if (static_table[i].first == name && static_table[i].second == value) {
                encode_prefix_int(result, /*high_bits=*/0x80, /*prefix_bits=*/7,
                                  static_cast<uint32_t>(i));
                return;
            }
        }
        // Try a name-only hit: emit literal-without-indexing with the
        // indexed name (4-bit prefix, pattern 0000).
        for (size_t i = 1; i < static_table.size(); ++i) {
            if (static_table[i].first == name) {
                encode_prefix_int(result, /*high_bits=*/0x00, /*prefix_bits=*/4,
                                  static_cast<uint32_t>(i));
                encode_string(result, value);
                return;
            }
        }
        // No name match: literal-without-indexing with new name
        // (4-bit prefix 0000, name-index = 0 -> next byte starts the
        // name string).
        result.push_back(0x00);
        encode_string(result, name);
        encode_string(result, value);
    };

    for (const auto& [k, v] : pseudo) {
        emit(k, v);
    }
    for (const auto& [k, v] : regular) {
        emit(k, v);
    }
    return result;
}

std::unordered_map<std::string, std::string> HPACK::decode_headers(
    const std::vector<uint8_t>& data) {
    std::unordered_map<std::string, std::string> headers;
    const auto& static_table = get_static_table();
    size_t offset = 0;

    while (offset < data.size()) {
        const uint8_t b = data[offset];

        if ((b & 0x80U) != 0) {
            // Indexed header field (1-bit prefix).
            uint32_t index = 0;
            if (!decode_prefix_int(data, offset, 7, index)) break;
            if (index == 0 || index >= static_table.size()) {
                // Out of range — RFC says protocol error; we just drop.
                continue;
            }
            const auto& [n, v] = static_table[index];
            headers[n] = v;

        } else if ((b & 0xC0U) == 0x40U) {
            // Literal with incremental indexing (2-bit prefix 01).
            uint32_t name_index = 0;
            if (!decode_prefix_int(data, offset, 6, name_index)) break;
            std::string name;
            std::string value;
            if (name_index == 0) {
                if (!decode_string(data, offset, name)) break;
            } else if (name_index < static_table.size()) {
                name = static_table[name_index].first;
            }
            if (!decode_string(data, offset, value)) break;
            if (!name.empty()) headers[name] = value;
            // (We don't maintain a dynamic table — indexing the entry
            //  is intentionally a no-op.)

        } else if ((b & 0xE0U) == 0x20U) {
            // Dynamic table size update (3-bit prefix 001).
            uint32_t new_size = 0;
            if (!decode_prefix_int(data, offset, 5, new_size)) break;
            // We advertise capacity 0; peer-issued updates within that
            // bound are fine, anything else would be a protocol error.
            // Either way: ignore.

        } else if ((b & 0xF0U) == 0x00U || (b & 0xF0U) == 0x10U) {
            // Literal without indexing (0000xxxx) or never-indexed
            // (0001xxxx).  Both use a 4-bit prefix for the name index.
            uint32_t name_index = 0;
            if (!decode_prefix_int(data, offset, 4, name_index)) break;
            std::string name;
            std::string value;
            if (name_index == 0) {
                if (!decode_string(data, offset, name)) break;
            } else if (name_index < static_table.size()) {
                name = static_table[name_index].first;
            }
            if (!decode_string(data, offset, value)) break;
            if (!name.empty()) headers[name] = value;

        } else {
            // Unknown prefix — bail to keep partial progress.
            break;
        }
    }
    return headers;
}

// Parser implementation
bool Parser::parse_connection_preface(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + CONNECTION_PREFACE_LEN > data.size()) {
        return false;
    }

    std::string preface(data.begin() + offset,
                        data.begin() + offset + CONNECTION_PREFACE_LEN);
    if (preface == CONNECTION_PREFACE) {
        offset += CONNECTION_PREFACE_LEN;
        return true;
    }

    return false;
}

Parser::ParseResult Parser::parse_frame(const std::vector<uint8_t>& data, size_t& offset,
                                                  Frame& frame) {
    auto parsed = Frame::parse(data, offset);
    if (!parsed) {
        return ParseResult::Incomplete;
    }

    frame = *parsed;
    return ParseResult::Success;
}

Parser::ParseResult Parser::parse_headers_frame(const Frame& frame,
                                                          Request& request) {
    if (frame.type != FrameType::HEADERS) {
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

Parser::ParseResult Parser::parse_headers_frame(const Frame& frame,
                                                          Response& response) {
    if (frame.type != FrameType::HEADERS) {
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

namespace {

// RFC 9113 §8.2.1: HTTP/2 field names must be lowercase ASCII tokens.
// Pseudo-header names begin with ':' (which is otherwise not a tchar).
// We forbid CR/LF/NUL in any name or value to keep us aligned with the
// HTTP/1.x sanitization done in HTTPResponse::serialize.
bool is_valid_http2_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    size_t start = 0;
    if (name[0] == ':') {
        if (name.size() < 2) {
            return false;
        }
        start = 1;
    }
    for (size_t i = start; i < name.size(); ++i) {
        char c = name[i];
        // tchar minus uppercase letters; lowercase only per §8.2.1.
        const bool is_lower_alpha = c >= 'a' && c <= 'z';
        const bool is_digit = c >= '0' && c <= '9';
        const bool is_other_tchar = c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
                                    c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
                                    c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
        if (!(is_lower_alpha || is_digit || is_other_tchar)) {
            return false;
        }
    }
    return true;
}

bool is_valid_http2_value(const std::string& value) {
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

std::unordered_map<std::string, std::string>
sanitize_http2_headers(const std::unordered_map<std::string, std::string>& in) {
    std::unordered_map<std::string, std::string> out;
    out.reserve(in.size());
    for (const auto& [k, v] : in) {
        if (is_valid_http2_name(k) && is_valid_http2_value(v)) {
            out.emplace(k, v);
        }
    }
    return out;
}

} // namespace

Frame Parser::build_headers_frame(const Request& request, uint32_t stream_id,
                                            bool end_headers, bool end_stream) {
    Frame frame;
    frame.type = FrameType::HEADERS;
    frame.flags = 0;
    if (end_headers)
        frame.flags |= Flags::END_HEADERS;
    if (end_stream)
        frame.flags |= Flags::END_STREAM;
    frame.stream_id = stream_id;

    // Sanitize before HPACK encoding so a caller cannot smuggle CR/LF
    // (which would corrupt downstream HTTP/1.x framing if this server
    // were ever proxied) or invalid characters in field names.
    frame.payload = HPACK::encode_headers(sanitize_http2_headers(request.headers));
    frame.length = frame.payload.size();

    return frame;
}

Frame Parser::build_headers_frame(const Response& response, uint32_t stream_id,
                                            bool end_headers, bool end_stream) {
    Frame frame;
    frame.type = FrameType::HEADERS;
    frame.flags = 0;
    if (end_headers)
        frame.flags |= Flags::END_HEADERS;
    if (end_stream)
        frame.flags |= Flags::END_STREAM;
    frame.stream_id = stream_id;

    // Sanitize before adding the mandatory :status pseudo-header; status
    // is integer-formatted by us, so it's known-good.
    std::unordered_map<std::string, std::string> headers = sanitize_http2_headers(response.headers);
    if (headers.find(":status") == headers.end()) {
        headers[":status"] = std::to_string(response.status_code);
    }

    frame.payload = HPACK::encode_headers(headers);
    frame.length = frame.payload.size();

    return frame;
}

Frame Parser::build_data_frame(uint32_t stream_id, const std::vector<uint8_t>& data,
                                         bool end_stream) {
    Frame frame;
    frame.type = FrameType::DATA;
    frame.flags = end_stream ? Flags::END_STREAM : 0;
    frame.stream_id = stream_id;
    frame.payload = data;
    frame.length = data.size();

    return frame;
}

Frame Parser::build_settings_frame(const Settings& settings, bool ack) {
    Frame frame;
    frame.type = FrameType::SETTINGS;
    frame.flags = ack ? Flags::ACK : 0;
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

Frame Parser::build_goaway_frame(uint32_t last_stream_id, uint32_t error_code) {
    Frame frame;
    frame.type = FrameType::GOAWAY;
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

Frame Parser::build_rst_stream_frame(uint32_t stream_id, uint32_t error_code) {
    Frame frame;
    frame.type = FrameType::RST_STREAM;
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

Frame Parser::build_window_update_frame(uint32_t stream_id,
                                                  uint32_t window_size_increment) {
    Frame frame;
    frame.type = FrameType::WINDOW_UPDATE;
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

Frame Parser::build_ping_frame(const std::vector<uint8_t>& opaque_data, bool ack) {
    Frame frame;
    frame.type = FrameType::PING;
    frame.flags = ack ? Flags::ACK : 0;
    frame.stream_id = 0; // Connection-level frame

    if (opaque_data.size() == 8) {
        frame.payload = opaque_data;
    } else {
        frame.payload.resize(8, 0);
    }

    frame.length = frame.payload.size();

    return frame;
}

// Connection implementation
Connection::Connection() : next_stream_id_(1), client_preface_received_(false) {}

Parser::ParseResult Connection::process_frame(const Frame& frame) {
    // Validate stream ID
    if (frame.stream_id != 0 && !is_valid_stream(frame.stream_id)) {
        // Invalid stream - should send RST_STREAM
        return Parser::ParseResult::Error;
    }

    // Update stream state based on frame type
    if (frame.stream_id != 0) {
        switch (frame.type) {
            case FrameType::HEADERS:
                if (get_stream_state(frame.stream_id) == StreamState::IDLE) {
                    initialize_stream(frame.stream_id);
                }
                break;
            case FrameType::DATA:
                // Stream must be OPEN or HALF_CLOSED_REMOTE
                break;
            default:
                break;
        }

        // Check END_STREAM flag
        if ((frame.flags & Flags::END_STREAM) != 0) {
            close_stream(frame.stream_id);
        }
    }

    return Parser::ParseResult::Success;
}

void Connection::update_settings(const Settings& settings) {
    settings_ = settings;
}

StreamState Connection::get_stream_state(uint32_t stream_id) const {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        return it->second;
    }
    return StreamState::IDLE;
}

bool Connection::is_valid_stream(uint32_t stream_id) const {
    if (stream_id == 0) {
        return false; // Stream 0 is reserved for connection-level frames
    }

    // Client-initiated streams must be odd (RFC 9113 Section 5.1.1)
    // Server-initiated streams must be even
    // For now, accept both
    return true;
}

void Connection::initialize_stream(uint32_t stream_id) {
    streams_[stream_id] = StreamState::OPEN;
}

void Connection::close_stream(uint32_t stream_id) {
    streams_[stream_id] = StreamState::CLOSED;
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

} // namespace spaznet::http2
