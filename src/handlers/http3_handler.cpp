#include <algorithm>
#include <cstring>
#include <libspaznet/handlers/http3_handler.hpp>
#include <libspaznet/handlers/quic_handler.hpp>
#include <libspaznet/server.hpp>
#include <sstream>
#include <stdexcept>

// HTTP/3 reference implementation: suppress noisy style checks (many protocol constants).
// NOLINTBEGIN(
//   cppcoreguidelines-avoid-magic-numbers,
//   readability-magic-numbers,
//   modernize-use-trailing-return-type,
//   cppcoreguidelines-narrowing-conversions,
//   readability-identifier-length,
//   readability-isolate-declaration,
//   readability-implicit-bool-conversion
// )

namespace spaznet {

namespace {

// Variable-length integer encoding (RFC9000 Section 16)
uint64_t read_varint(const std::vector<uint8_t>& data, size_t& offset, int prefix_bits) {
    uint64_t mask = (1ULL << prefix_bits) - 1;
    uint64_t value = data[offset] & mask;
    offset++;

    if (value < (1ULL << (prefix_bits - 2))) {
        return value;
    }

    int bytes = 1 << (value - (1ULL << (prefix_bits - 2)));
    for (int i = 0; i < bytes; ++i) {
        if (offset >= data.size()) {
            return 0;
        }
        value = (value << 8) | data[offset++];
    }

    return value;
}

void write_varint(std::vector<uint8_t>& out, uint64_t value, int prefix_bits) {
    uint64_t max_prefix = (1ULL << (prefix_bits - 2)) - 1;
    if (value < max_prefix) {
        out.push_back(static_cast<uint8_t>(value));
        return;
    }

    int bytes = 1;
    uint64_t v = value;
    while (v > 0) {
        bytes++;
        v >>= 8;
    }

    uint64_t prefix = (1ULL << (prefix_bits - 2)) + (bytes - 1);
    out.push_back(static_cast<uint8_t>((prefix << (8 - prefix_bits)) |
                                       (value >> (bytes * 8 - 8 + prefix_bits))));

    for (int i = bytes - 2; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

// Parse HTTP/3 HEADERS frame (RFC9114 Section 7.2.2)
bool parse_headers_frame(const std::vector<uint8_t>& data, HTTP3Request& request) {
    if (data.empty()) {
        return false;
    }

    size_t offset = 0;
    uint64_t frame_type = read_varint(data, offset, 8);

    if (frame_type != static_cast<uint64_t>(HTTP3FrameType::Headers)) {
        return false;
    }

    // Parse QPACK-encoded headers (simplified - just parse as text for now)
    // In a full implementation, we'd decode QPACK (RFC9204)
    std::string headers_str(data.begin() + offset, data.end());
    std::istringstream iss(headers_str);
    std::string line;

    bool first_line = true;
    while (std::getline(iss, line)) {
        if (line.empty() || line.back() == '\r') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
        }

        if (line.empty()) {
            continue;
        }

        if (first_line) {
            // Parse ":method :path :scheme :authority" pseudo-headers
            std::istringstream first_iss(line);
            std::string method, path, scheme, authority;
            first_iss >> method >> path >> scheme >> authority;
            request.method = method;
            request.request_target = path;
            request.scheme = scheme;
            request.authority = authority;
            first_line = false;
        } else {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                // Trim whitespace
                value.erase(value.begin(),
                            std::find_if(value.begin(), value.end(),
                                         [](unsigned char ch) { return !std::isspace(ch); }));
                request.headers[name] = value;
            }
        }
    }

    return true;
}

// Serialize HTTP/3 HEADERS frame
std::vector<uint8_t> serialize_headers_frame(const HTTP3Response& response) {
    std::vector<uint8_t> frame;

    // Frame type
    write_varint(frame, static_cast<uint64_t>(HTTP3FrameType::Headers), 8);

    // Serialize headers (simplified - just text format)
    // In a full implementation, we'd use QPACK encoding
    std::ostringstream oss;
    oss << ":status " << response.status_code << " " << response.reason_phrase << "\r\n";
    for (const auto& [name, value] : response.headers) {
        oss << name << ": " << value << "\r\n";
    }
    oss << "\r\n";

    std::string headers_str = oss.str();
    frame.insert(frame.end(), headers_str.begin(), headers_str.end());

    return frame;
}

// Serialize HTTP/3 DATA frame
std::vector<uint8_t> serialize_data_frame(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame;

    // Frame type
    write_varint(frame, static_cast<uint64_t>(HTTP3FrameType::Data), 8);

    // Length (simplified - just append data)
    frame.insert(frame.end(), data.begin(), data.end());

    return frame;
}

} // namespace

// Note: HTTP3ServerHandler would be implemented separately if needed
// This is a reference implementation showing how QUIC and HTTP3 integrate

// NOLINTEND(
//   cppcoreguidelines-avoid-magic-numbers,
//   readability-magic-numbers,
//   modernize-use-trailing-return-type,
//   cppcoreguidelines-narrowing-conversions,
//   readability-identifier-length,
//   readability-isolate-declaration,
//   readability-implicit-bool-conversion
// )

} // namespace spaznet
