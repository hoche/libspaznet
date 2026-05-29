#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/utils/header_utils.hpp>
#include <libspaznet/utils/number_utils.hpp>
#include <libspaznet/utils/string_utils.hpp>
#include <limits>
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
    // RFC 9112 §6.1: chunked is recognized only when it is the final coding in
    // the Transfer-Encoding list. Anything else (e.g. "chunked, gzip") means
    // the message body length cannot be determined and the recipient must
    // close the connection — callers should treat it as not chunked here and
    // reject the request at the parser level (see parse_request).
    auto te = get_header("Transfer-Encoding");
    if (!te) {
        return false;
    }
    std::string te_lower = HTTPParser::to_lower(*te);
    std::string last_token;
    std::istringstream iss(te_lower);
    std::string part;
    while (std::getline(iss, part, ',')) {
        part = HTTPParser::trim_ows(part);
        if (!part.empty()) {
            last_token = part;
        }
    }
    return last_token == "chunked";
}

namespace {

// RFC 9112 §5.6.2: field-name = token. Reject anything else so a caller
// can't smuggle CR/LF/SP/etc into the name and cause response splitting.
bool is_valid_field_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        if (!HTTPParser::is_token_char(c)) {
            return false;
        }
    }
    return true;
}

// RFC 9110 §5.5: field-value = *( field-content / obs-fold )
// field-content = field-vchar [ 1*( SP / HTAB / field-vchar ) field-vchar ]
// We forbid CR, LF, and NUL outright (response-splitting / header
// injection). Permit SP and HTAB so existing valid headers keep working.
bool is_valid_field_value(const std::string& value) {
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

} // namespace

// HTTPResponse helper methods
std::optional<std::string> HTTPResponse::get_header(const std::string& name) const {
    return HeaderUtils::get_header_case_insensitive(headers, name);
}

void HTTPResponse::set_content_length(size_t length) {
    headers["Content-Length"] = std::to_string(length);
}

void HTTPResponse::set_chunked() {
    headers["Transfer-Encoding"] = "chunked";
}

std::vector<uint8_t> HTTPResponse::serialize() const {
    std::ostringstream oss;

    // Status line per RFC 9112 Section 6.1
    oss << "HTTP/" << version << " " << status_code << " " << reason_phrase << "\r\n";

    // Headers per RFC 9112 Section 6.3. Validate field name and value at
    // emission time so a caller-supplied CR/LF/NUL (or a non-token name)
    // can't split the response into a smuggled second message.
    for (const auto& [key, value] : headers) {
        if (!is_valid_field_name(key) || !is_valid_field_value(value)) {
            continue; // drop the offending entry rather than emit it
        }
        oss << key << ": " << value << "\r\n";
    }

    // Auto-emit Content-Length whenever the response uses Content-Length
    // framing (no Transfer-Encoding) and the status code permits a body.
    // RFC 9112 §6.1: under a persistent connection, recipients need
    // explicit framing on every response — omitting Content-Length on an
    // empty body was an interop bug (clients would block waiting for a
    // body byte that never came, or read into the next pipelined
    // response). 1xx/204/304 responses MUST NOT include body content per
    // RFC 9110 §15, so we skip the header there.
    const bool status_forbids_body = (status_code >= 100 && status_code < 200) ||
                                     status_code == 204 || status_code == 304;
    if (!status_forbids_body && headers.find("Content-Length") == headers.end() &&
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

    // Headers (ensure Transfer-Encoding: chunked is set). Same validation
    // as serialize() — drop entries whose name isn't a token or whose
    // value contains CR/LF/NUL.
    bool has_transfer_encoding = false;
    for (const auto& [key, value] : headers) {
        if (!is_valid_field_name(key) || !is_valid_field_value(value)) {
            continue;
        }
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

    // RFC 9112 §5.1: "A server MUST reject ... any received request message
    // that contains whitespace between a header field name and colon." This
    // closes a request-smuggling primitive where front-ends and back-ends
    // disagree on field-name normalization.
    const char before_colon = line[colon - 1];
    if (before_colon == ' ' || before_colon == '\t') {
        return false;
    }

    std::string field_name = line.substr(0, colon);
    std::string field_value = line.substr(colon + 1);

    // Validate field name is a token (no trimming — OWS before ':' is now an
    // error above, and OWS inside the name would be a non-token character).
    if (field_name.empty()) {
        return false;
    }
    for (char c : field_name) {
        if (!is_token_char(c)) {
            return false;
        }
    }

    // Trim OWS from field value (and handle obs-fold)
    field_value = trim_ows(field_value);

    // Store header (preserve original case for field name, but allow case-insensitive lookup).
    // Duplicate-header detection (e.g. two Content-Length fields) is handled
    // by parse_request after all headers are in, since the map collapses
    // same-case duplicates.
    auto existing = headers.find(field_name);
    if (existing != headers.end()) {
        // Multiple entries under the same exact-case key: RFC 9110 §5.3 allows
        // combining only if the field is list-valued. We don't know the field
        // semantics here, so we keep the first value and let parse_request
        // reject critical headers (Content-Length) at validation time. For
        // any other field we tolerate the duplicate by concatenating per
        // RFC 9110 §5.3 list rules.
        existing->second.append(", ");
        existing->second.append(field_value);
        return true;
    }
    headers[field_name] = field_value;

    return true;
}

namespace {

// Case-insensitive equality (ASCII) — local helper to avoid pulling the
// HeaderUtils-internal predicate out of header_utils.cpp.
bool ci_equal(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

// Locate end of header block (the byte AFTER the terminating CRLF CRLF), or
// 0 if not yet present. Returns Error if the search exceeds max_bytes (the
// peer is shipping an oversize header block).
enum class HeaderEndState : uint8_t { Found, Incomplete, TooLarge };
HeaderEndState find_header_end(const std::vector<uint8_t>& buffer, size_t max_bytes,
                               size_t& header_end_out) {
    header_end_out = 0;
    bool found_crlf = false;
    const size_t scan_limit = std::min(buffer.size(), max_bytes);
    for (size_t i = 0; i + 1 < scan_limit; ++i) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
            if (found_crlf) {
                header_end_out = i + 2;
                return HeaderEndState::Found;
            }
            found_crlf = true;
            ++i; // skip LF
        } else if (buffer[i] != '\r' && buffer[i] != '\n') {
            found_crlf = false;
        }
    }
    if (buffer.size() >= max_bytes) {
        return HeaderEndState::TooLarge;
    }
    return HeaderEndState::Incomplete;
}

// Validate critical message-framing headers per RFC 9112 §6 to close
// request-smuggling vectors:
//   - Content-Length and Transfer-Encoding must not both be present.
//   - At most one Content-Length value (and any case-folded duplicates must
//     hold the same numeric value).
//   - Transfer-Encoding, if present, must have "chunked" as the final coding
//     (other codings imply the recipient cannot determine framing).
bool validate_framing_headers(std::unordered_map<std::string, std::string>& headers) {
    std::string* cl_value = nullptr;
    const std::string* te_value = nullptr;
    for (auto& [key, value] : headers) {
        if (ci_equal(key, "Content-Length")) {
            if (cl_value != nullptr && *cl_value != value) {
                return false;
            }
            cl_value = &value;
        } else if (ci_equal(key, "Transfer-Encoding")) {
            te_value = &value;
        }
    }

    if (cl_value != nullptr) {
        // RFC 9112 §6.3 explicitly allows repeated Content-Length headers
        // when every value is identical (and §8.6 lets a single header
        // carry a comma-separated list of identical values). Anything
        // else is a framing ambiguity and must be rejected.
        //
        // parse_header_field merges identical-cased duplicates into the
        // same map entry with ", " between values, so we may see either
        // a bare "10" (one header) or "10, 10" (two headers that
        // merged). Validate by splitting on commas, trimming, and
        // requiring all tokens to be the same well-formed non-negative
        // integer.
        std::string first_token;
        bool seen_any = false;
        size_t pos = 0;
        while (pos <= cl_value->size()) {
            const size_t comma = cl_value->find(',', pos);
            const size_t end = (comma == std::string::npos) ? cl_value->size() : comma;
            size_t b = pos;
            while (b < end && (cl_value->at(b) == ' ' || cl_value->at(b) == '\t')) {
                ++b;
            }
            size_t e = end;
            while (e > b && (cl_value->at(e - 1) == ' ' || cl_value->at(e - 1) == '\t')) {
                --e;
            }
            if (b == e) {
                return false; // empty token (e.g. "10,, 10")
            }
            for (size_t i = b; i < e; ++i) {
                const char c = cl_value->at(i);
                if (c < '0' || c > '9') {
                    return false;
                }
            }
            std::string token = cl_value->substr(b, e - b);
            if (!seen_any) {
                first_token = std::move(token);
                seen_any = true;
            } else if (token != first_token) {
                return false; // distinct CL values — framing is ambiguous
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
        if (!seen_any) {
            return false;
        }
        // Normalize the merged "10, 10" form back to a single value so
        // downstream get_content_length() (which uses std::stoull, not
        // a strict parser) sees the canonical integer rather than a
        // list whose tokens it would silently truncate at the comma.
        *cl_value = first_token;
    }

    if (cl_value != nullptr && te_value != nullptr) {
        // RFC 9112 §6.1: if both fields appear the message must be rejected.
        return false;
    }

    if (te_value != nullptr) {
        // RFC 9112 §6.1: a recipient that receives a Transfer-Encoding whose
        // final coding is not chunked cannot determine framing — must close
        // the connection (we reject the message at parse time).
        std::string te_lower;
        te_lower.reserve(te_value->size());
        for (char c : *te_value) {
            te_lower.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        std::string last_token;
        std::istringstream te_iss(te_lower);
        std::string part;
        while (std::getline(te_iss, part, ',')) {
            // Inline trim (avoid pulling in trim_ows here, which lives on the
            // outer class and would force this helper to friend it).
            size_t b = part.find_first_not_of(" \t");
            size_t e = part.find_last_not_of(" \t");
            if (b == std::string::npos) {
                continue;
            }
            last_token = part.substr(b, e - b + 1);
        }
        if (last_token != "chunked") {
            return false;
        }
    }
    return true;
}

} // namespace

HTTPParser::ParseResult HTTPParser::parse_request(const std::vector<uint8_t>& buffer,
                                                  HTTPRequest& request, size_t& bytes_consumed) {
    bytes_consumed = 0;

    // Reset the output so this function is idempotent: a caller that
    // re-invokes us with an extended buffer (because the first call
    // returned Incomplete) must observe the same parsed result as
    // calling us once with the full buffer. Without this, the
    // per-field "merge duplicates by concatenating" path in
    // parse_header_field would append every header again on every
    // retry — turning "Content-Length: 10000" into
    // "Content-Length: 10000, 10000" and tripping the framing
    // validator.
    request.method.clear();
    request.request_target.clear();
    request.version.clear();
    request.headers.clear();
    request.body.clear();

    // Bounded search for end of headers (CRLF CRLF). Refusing to scan past
    // kMaxHeaderBytes defeats Slowloris-style header drips.
    size_t header_end = 0;
    switch (find_header_end(buffer, kMaxHeaderBytes, header_end)) {
        case HeaderEndState::Found:
            break;
        case HeaderEndState::Incomplete:
            return ParseResult::Incomplete;
        case HeaderEndState::TooLarge:
            return ParseResult::Error;
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

    // Parse headers, with an upper bound on the count.
    size_t header_count = 0;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            break; // End of headers
        }

        if (++header_count > kMaxHeaders) {
            return ParseResult::Error;
        }

        if (!parse_header_field(line, request.headers)) {
            return ParseResult::Error;
        }
    }

    if (!validate_framing_headers(request.headers)) {
        return ParseResult::Error;
    }

    bytes_consumed = header_end;

    // Parse body if present
    auto content_length = request.get_content_length();
    if (content_length) {
        size_t body_size = *content_length;
        if (body_size > kMaxBodySize) {
            return ParseResult::Error;
        }
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
    } else {
        // Transfer-Encoding present but not chunked (rejected above), or
        // neither header — request has no body. Nothing more to do.
    }

    return ParseResult::Success;
}

auto HTTPParser::parse_response(const std::vector<uint8_t>& buffer, HTTPResponse& response,
                                size_t& bytes_consumed) -> HTTPParser::ParseResult {
    bytes_consumed = 0;

    // Same idempotence story as parse_request — clear the output so a
    // retry on an extended buffer doesn't accumulate duplicate
    // headers via parse_header_field's list-merge path.
    response.version.clear();
    response.status_code = 0;
    response.reason_phrase.clear();
    response.headers.clear();
    response.body.clear();

    size_t header_end = 0;
    switch (find_header_end(buffer, kMaxHeaderBytes, header_end)) {
        case HeaderEndState::Found:
            break;
        case HeaderEndState::Incomplete:
            return ParseResult::Incomplete;
        case HeaderEndState::TooLarge:
            return ParseResult::Error;
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
    size_t header_count = 0;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            break;
        }

        if (++header_count > kMaxHeaders) {
            return ParseResult::Error;
        }

        if (!parse_header_field(line, response.headers)) {
            return ParseResult::Error;
        }
    }

    // Same framing-header validation as on the request path: defense in depth
    // even though responses are typically generated by our own upstream.
    if (!validate_framing_headers(response.headers)) {
        return ParseResult::Error;
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
        if (body_size > kMaxBodySize) {
            return ParseResult::Error;
        }
        if (buffer.size() < header_end + body_size) {
            return ParseResult::Incomplete;
        }
        const auto header_off = static_cast<std::ptrdiff_t>(header_end);
        const auto body_off = static_cast<std::ptrdiff_t>(body_size);
        response.body.assign(buffer.begin() + header_off, buffer.begin() + header_off + body_off);
        bytes_consumed += body_size;
    } else {
        auto transfer_encoding_header = response.get_header("Transfer-Encoding");
        if (transfer_encoding_header) {
            // Mirror HTTPRequest::is_chunked: only honor chunked when it is
            // the final coding.
            std::string te_lower = to_lower(*transfer_encoding_header);
            std::string last_token;
            std::istringstream te_iss(te_lower);
            std::string part;
            while (std::getline(te_iss, part, ',')) {
                part = trim_ows(part);
                if (!part.empty()) {
                    last_token = part;
                }
            }
            if (last_token != "chunked") {
                return ParseResult::Error;
            }
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

auto HTTPParser::parse_chunk_size(const std::string& line) -> std::optional<size_t> {
    // RFC 9112 §7.1:  chunk-size = 1*HEXDIG
    //                 chunk      = chunk-size [ chunk-ext ] CRLF chunk-data CRLF
    // Be strict: no leading sign, no whitespace, at least one hex digit; stop
    // at ';' (chunk-ext) or end-of-string. Returning an optional lets callers
    // distinguish a real last-chunk (value 0) from a parse failure — the
    // previous size_t-with-0-on-error contract was a request-smuggling
    // primitive (malformed chunk-size was silently treated as end-of-body).
    size_t value = 0;
    bool saw_digit = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == ';') {
            // chunk-extensions — ignored, but require ≥1 hex digit before.
            break;
        }
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            digit = 10 + (c - 'A');
        } else {
            return std::nullopt;
        }
        // Overflow guard before the shift: value*16 would exceed size_t.
        if (value > (std::numeric_limits<size_t>::max() - static_cast<size_t>(digit)) / 16) {
            return std::nullopt;
        }
        value = value * 16 + static_cast<size_t>(digit);
        saw_digit = true;
    }
    if (!saw_digit) {
        return std::nullopt;
    }
    return value;
}

auto HTTPParser::parse_chunked_body(const std::vector<uint8_t>& buffer, std::vector<uint8_t>& body,
                                    size_t& bytes_consumed) -> HTTPParser::ParseResult {
    bytes_consumed = 0;
    body.clear();

    size_t pos = 0;

    while (pos < buffer.size()) {
        // Cap the chunk-size line length so a peer cannot make us scan an
        // unbounded buffer looking for CRLF.
        const size_t chunk_line_max = 64;
        size_t crlf_pos = pos;
        while (crlf_pos + 1 < buffer.size() &&
               (buffer[crlf_pos] != '\r' || buffer[crlf_pos + 1] != '\n')) {
            if (crlf_pos - pos > chunk_line_max) {
                return ParseResult::Error;
            }
            crlf_pos++;
        }

        if (crlf_pos + 1 >= buffer.size()) {
            return ParseResult::Incomplete;
        }

        std::string chunk_size_line(buffer.begin() + static_cast<std::ptrdiff_t>(pos),
                                    buffer.begin() + static_cast<std::ptrdiff_t>(crlf_pos));
        auto chunk_size_opt = parse_chunk_size(chunk_size_line);
        if (!chunk_size_opt) {
            return ParseResult::Error;
        }
        size_t chunk_size = *chunk_size_opt;
        if (chunk_size > kMaxChunkSize) {
            return ParseResult::Error;
        }

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

        if (body.size() + chunk_size > kMaxBodySize) {
            return ParseResult::Error;
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
