#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/utils/header_utils.hpp>
#include <libspaznet/utils/number_utils.hpp>
#include <libspaznet/utils/string_utils.hpp>
#include <sstream>
#include <stdexcept>

// HTTP/1.x parser/serializer code uses many protocol constants and iterator math; keep tidy output
// actionable by suppressing a few noisy style checks in this translation unit.
// NOLINTBEGIN(
//   modernize-use-trailing-return-type,
//   cppcoreguidelines-narrowing-conversions,
//   readability-identifier-length,
//   readability-function-cognitive-complexity,
//   cppcoreguidelines-avoid-magic-numbers,
//   readability-magic-numbers
// )

namespace spaznet {

// HTTPRequest helper methods
std::optional<std::string> HTTPRequest::get_header(const std::string& name) const {
    return HeaderUtils::get_header_case_insensitive(headers, name);
}

bool HTTPRequest::should_keep_alive() const {
    const bool is_http_11 = (version == "1.1");
    const bool is_http_10 = (version == "1.0");

    auto connection = get_header("Connection");
    if (connection) {
        // RFC 9110: Connection is a list of tokens.
        std::string conn = HTTPParser::to_lower(*connection);
        bool has_close = false;
        bool has_keep_alive = false;

        std::istringstream iss(conn);
        std::string part;
        while (std::getline(iss, part, ',')) {
            part = HTTPParser::trim_ows(part);
            if (part == "close") {
                has_close = true;
            } else if (part == "keep-alive") {
                has_keep_alive = true;
            }
        }

        if (has_close) {
            return false;
        }
        if (is_http_10) {
            // HTTP/1.0 defaults to closing unless explicitly kept alive.
            return has_keep_alive;
        }
        if (is_http_11) {
            // HTTP/1.1 defaults to keep-alive unless close is present.
            return true;
        }

        // Unknown HTTP version: be conservative and only keep-alive if explicitly requested.
        return has_keep_alive;
    }

    // Defaults by version.
    if (is_http_11) {
        return true;
    }
    return false;
}

std::optional<size_t> HTTPRequest::get_content_length() const {
    auto cl = get_header("Content-Length");
    if (!cl) {
        return std::nullopt;
    }
    auto parsed = NumberUtils::parse_uint64(*cl);
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

bool HTTPRequest::is_chunked() const {
    auto te = get_header("Transfer-Encoding");
    if (te) {
        std::string te_lower = HTTPParser::to_lower(*te);
        return te_lower.find("chunked") != std::string::npos;
    }
    return false;
}

// HTTPResponse helper methods
std::optional<std::string> HTTPResponse::get_header(const std::string& name) const {
    return HeaderUtils::get_header_case_insensitive(headers, name);
}

void HTTPResponse::set_content_length(size_t length) {
    headers["Content-Length"] = std::format("{}", length);
}

void HTTPResponse::set_chunked() {
    headers["Transfer-Encoding"] = "chunked";
}

std::vector<uint8_t> HTTPResponse::serialize() const {
    std::ostringstream oss;

    // Status line per RFC 9112 Section 6.1
    oss << "HTTP/" << version << " " << status_code << " " << reason_phrase << "\r\n";

    // Headers per RFC 9112 Section 6.3
    for (const auto& [key, value] : headers) {
        // Field name must be a token
        oss << key << ": " << value << "\r\n";
    }

    // Automatically add Content-Length if body is present and not already set
    // and not using chunked transfer encoding
    if (!body.empty() && headers.find("Content-Length") == headers.end() &&
        headers.find("Transfer-Encoding") == headers.end()) {
        oss << "Content-Length: " << body.size() << "\r\n";
    }

    // Empty line separates headers from body
    oss << "\r\n";

    std::string header_str = oss.str();
    std::vector<uint8_t> result;
    result.reserve(header_str.size() + body.size());

    result.insert(result.end(), header_str.begin(), header_str.end());
    result.insert(result.end(), body.begin(), body.end());

    return result;
}

std::vector<uint8_t> HTTPResponse::serialize_chunked() const {
    std::ostringstream oss;

    // Status line
    oss << "HTTP/" << version << " " << status_code << " " << reason_phrase << "\r\n";

    // Headers (ensure Transfer-Encoding: chunked is set)
    bool has_transfer_encoding = false;
    for (const auto& [key, value] : headers) {
        if (HTTPParser::to_lower(key) == "transfer-encoding") {
            has_transfer_encoding = true;
        }
        oss << key << ": " << value << "\r\n";
    }
    if (!has_transfer_encoding) {
        oss << "Transfer-Encoding: chunked\r\n";
    }

    oss << "\r\n";

    std::string header_str = oss.str();
    std::vector<uint8_t> result;
    result.reserve(header_str.size() + body.size() * 1.1); // Estimate for chunk overhead

    result.insert(result.end(), header_str.begin(), header_str.end());

    // Chunked encoding per RFC 9112 Section 7.1
    if (!body.empty()) {
        // Write chunk size in hex
        std::ostringstream chunk_header;
        chunk_header << std::hex << body.size() << "\r\n";
        std::string chunk_hdr = chunk_header.str();
        result.insert(result.end(), chunk_hdr.begin(), chunk_hdr.end());

        // Write chunk data
        result.insert(result.end(), body.begin(), body.end());
        result.insert(result.end(), {'\r', '\n'});
    }

    // Final chunk (size 0)
    result.insert(result.end(), {'0', '\r', '\n', '\r', '\n'});

    return result;
}

// HTTPParser implementation
bool HTTPParser::is_token_char(char c) {
    // Token characters per RFC 9112 Section 5.6.2
    // token = 1*tchar
    // tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
    //         "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
    return std::isalnum(c) || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
           c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
           c == '`' || c == '|' || c == '~';
}

bool HTTPParser::is_field_vchar(char c) {
    // Field value characters per RFC 9112 Section 5.5
    // field-value = *field-content / *( obs-fold CRLF ) field-content
    // field-content = field-vchar [ 1*( SP / HTAB / field-vchar ) field-vchar ]
    // field-vchar = VCHAR / obs-text
    // VCHAR = %x21-7E (visible characters)
    return c >= 0x21 && c <= 0x7E;
}

std::string HTTPParser::to_lower(const std::string& str) {
    return StringUtils::to_lower(str);
}

std::string HTTPParser::trim_ows(const std::string& str) {
    return StringUtils::trim_ows(str);
}

bool HTTPParser::parse_request_line(const std::string& line, HTTPRequest& request) {
    // Request line per RFC 9112 Section 3.1.1
    // request-line = method SP request-target SP HTTP-version CRLF

    size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        return false;
    }

    request.method = line.substr(0, first_space);

    // Validate method is a token
    for (char c : request.method) {
        if (!is_token_char(c)) {
            return false;
        }
    }

    size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos) {
        return false;
    }

    request.request_target = line.substr(first_space + 1, second_space - first_space - 1);

    size_t version_start = second_space + 1;
    if (line.substr(version_start, 5) != "HTTP/") {
        return false;
    }

    request.version = line.substr(version_start + 5);

    return true;
}

bool HTTPParser::parse_status_line(const std::string& line, HTTPResponse& response) {
    // Status line per RFC 9112 Section 6.1
    // status-line = HTTP-version SP status-code SP [ reason-phrase ] CRLF

    if (line.substr(0, 5) != "HTTP/") {
        return false;
    }

    size_t first_space = line.find(' ', 5);
    if (first_space == std::string::npos) {
        return false;
    }

    response.version = line.substr(5, first_space - 5);

    size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos) {
        // No reason phrase
        try {
            response.status_code = std::stoi(line.substr(first_space + 1));
            response.reason_phrase = "";
        } catch (...) {
            return false;
        }
    } else {
        auto status_opt =
            NumberUtils::parse_int(line.substr(first_space + 1, second_space - first_space - 1));
        if (!status_opt) {
            return false;
        }
        response.status_code = *status_opt;
        response.reason_phrase = line.substr(second_space + 1);
    }

    return true;
}

bool HTTPParser::parse_header_field(const std::string& line,
                                    std::unordered_map<std::string, std::string>& headers) {
    // Header field per RFC 9112 Section 5.5
    // header-field = field-name ":" OWS field-value OWS

    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }

    std::string field_name = line.substr(0, colon);
    std::string field_value = line.substr(colon + 1);

    // Trim OWS from field name
    field_name = trim_ows(field_name);

    // Validate field name is a token
    for (char c : field_name) {
        if (!is_token_char(c)) {
            return false;
        }
    }

    // Trim OWS from field value (and handle obs-fold)
    field_value = trim_ows(field_value);

    // Store header (preserve original case for field name, but allow case-insensitive lookup)
    headers[field_name] = field_value;

    return true;
}

HTTPParser::ParseResult HTTPParser::parse_request(const std::vector<uint8_t>& buffer,
                                                  HTTPRequest& request, size_t& bytes_consumed) {
    bytes_consumed = 0;

    // Find end of headers (CRLF CRLF)
    size_t header_end = 0;
    bool found_crlf = false;
    for (size_t i = 0; i + 1 < buffer.size(); ++i) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
            if (found_crlf) {
                // Found second CRLF - end of headers
                header_end = i + 2;
                break;
            }
            found_crlf = true;
            i++; // Skip LF
        } else if (buffer[i] != '\r' && buffer[i] != '\n') {
            found_crlf = false;
        }
    }

    if (header_end == 0) {
        return ParseResult::Incomplete;
    }

    // Parse request line and headers
    std::string request_str(buffer.begin(), buffer.begin() + header_end);
    std::istringstream iss(request_str);
    std::string line;

    // Parse request line
    if (!std::getline(iss, line)) {
        return ParseResult::Error;
    }

    // Remove trailing CR
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    if (!parse_request_line(line, request)) {
        return ParseResult::Error;
    }

    // Parse headers
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            break; // End of headers
        }

        if (!parse_header_field(line, request.headers)) {
            return ParseResult::Error;
        }
    }

    bytes_consumed = header_end;

    // Parse body if present
    auto content_length = request.get_content_length();
    if (content_length) {
        size_t body_size = *content_length;
        if (buffer.size() < header_end + body_size) {
            return ParseResult::Incomplete;
        }
        const auto header_off = static_cast<std::ptrdiff_t>(header_end);
        const auto body_off = static_cast<std::ptrdiff_t>(body_size);
        request.body.assign(buffer.begin() + header_off, buffer.begin() + header_off + body_off);
        bytes_consumed += body_size;
    } else if (request.is_chunked()) {
        // Parse chunked body
        size_t chunk_bytes = 0;
        ParseResult chunk_result = parse_chunked_body(
            std::vector<uint8_t>(buffer.begin() + static_cast<std::ptrdiff_t>(header_end),
                                 buffer.end()),
            request.body, chunk_bytes);
        if (chunk_result != ParseResult::Success) {
            return chunk_result;
        }
        bytes_consumed += chunk_bytes;
    }

    return ParseResult::Success;
}

auto HTTPParser::parse_response(const std::vector<uint8_t>& buffer, HTTPResponse& response,
                                size_t& bytes_consumed) -> HTTPParser::ParseResult {
    bytes_consumed = 0;

    // Find end of headers
    size_t header_end = 0;
    bool found_crlf = false;
    for (size_t i = 0; i + 1 < buffer.size(); ++i) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
            if (found_crlf) {
                header_end = i + 2;
                break;
            }
            found_crlf = true;
            i++;
        } else if (buffer[i] != '\r' && buffer[i] != '\n') {
            found_crlf = false;
        }
    }

    if (header_end == 0) {
        return ParseResult::Incomplete;
    }

    // Parse status line and headers
    std::string response_str(buffer.begin(), buffer.begin() + header_end);
    std::istringstream iss(response_str);
    std::string line;

    // Parse status line
    if (!std::getline(iss, line)) {
        return ParseResult::Error;
    }

    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    if (!parse_status_line(line, response)) {
        return ParseResult::Error;
    }

    // Parse headers
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            break;
        }

        if (!parse_header_field(line, response.headers)) {
            return ParseResult::Error;
        }
    }

    bytes_consumed = header_end;

    // Parse body
    auto content_length_header = response.get_header("Content-Length");
    if (content_length_header) {
        auto body_size_opt = NumberUtils::parse_uint64(*content_length_header);
        if (!body_size_opt) {
            return ParseResult::Error;
        }
        size_t body_size = *body_size_opt;
        if (buffer.size() < header_end + body_size) {
            return ParseResult::Incomplete;
        }
        const auto header_off = static_cast<std::ptrdiff_t>(header_end);
        const auto body_off = static_cast<std::ptrdiff_t>(body_size);
        response.body.assign(buffer.begin() + header_off, buffer.begin() + header_off + body_off);
        bytes_consumed += body_size;
    } else {
        auto transfer_encoding_header = response.get_header("Transfer-Encoding");
        if (transfer_encoding_header &&
            to_lower(*transfer_encoding_header).find("chunked") != std::string::npos) {
            size_t chunk_bytes = 0;
            ParseResult chunk_result = parse_chunked_body(
                std::vector<uint8_t>(buffer.begin() + static_cast<std::ptrdiff_t>(header_end),
                                     buffer.end()),
                response.body, chunk_bytes);
            if (chunk_result != ParseResult::Success) {
                return chunk_result;
            }
            bytes_consumed += chunk_bytes;
        }
    }

    return ParseResult::Success;
}

auto HTTPParser::parse_chunk_size(const std::string& line) -> size_t {
    // Chunk size is hex number per RFC 9112 Section 7.1
    try {
        constexpr int kHexBase = 16;
        return std::stoull(line, nullptr, kHexBase);
    } catch (...) {
        return 0;
    }
}

auto HTTPParser::parse_chunked_body(const std::vector<uint8_t>& buffer, std::vector<uint8_t>& body,
                                    size_t& bytes_consumed) -> HTTPParser::ParseResult {
    bytes_consumed = 0;
    body.clear();

    size_t pos = 0;

    while (pos < buffer.size()) {
        // Find chunk size line (ends with CRLF)
        size_t crlf_pos = pos;
        while (crlf_pos + 1 < buffer.size() &&
               (buffer[crlf_pos] != '\r' || buffer[crlf_pos + 1] != '\n')) {
            crlf_pos++;
        }

        if (crlf_pos + 1 >= buffer.size()) {
            return ParseResult::Incomplete;
        }

        std::string chunk_size_line(buffer.begin() + static_cast<std::ptrdiff_t>(pos),
                                    buffer.begin() + static_cast<std::ptrdiff_t>(crlf_pos));
        size_t chunk_size = parse_chunk_size(chunk_size_line);

        pos = crlf_pos + 2; // Skip CRLF

        if (chunk_size == 0) {
            // Last chunk - check for trailing CRLF
            if (pos + 1 < buffer.size() && buffer[pos] == '\r' && buffer[pos + 1] == '\n') {
                bytes_consumed = pos + 2;
                return ParseResult::Success;
            }
            return ParseResult::Incomplete;
        }

        // Read chunk data
        if (pos + chunk_size + 2 > buffer.size()) {
            return ParseResult::Incomplete;
        }

        body.insert(body.end(), buffer.begin() + static_cast<std::ptrdiff_t>(pos),
                    buffer.begin() + static_cast<std::ptrdiff_t>(pos + chunk_size));
        pos += chunk_size;

        // Skip CRLF after chunk data
        if (pos + 1 >= buffer.size() || buffer[pos] != '\r' || buffer[pos + 1] != '\n') {
            return ParseResult::Error;
        }
        pos += 2;
    }

    return ParseResult::Incomplete;
}

// NOLINTEND(
//   modernize-use-trailing-return-type,
//   cppcoreguidelines-narrowing-conversions,
//   readability-identifier-length,
//   readability-function-cognitive-complexity,
//   cppcoreguidelines-avoid-magic-numbers,
//   readability-magic-numbers
// )

} // namespace spaznet
