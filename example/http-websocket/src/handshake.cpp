#include "handshake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <vector>

namespace spaznet::websocket::detail {

namespace {

inline uint32_t rotl(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::array<uint8_t, 20> sha1(const uint8_t* data, std::size_t len) {
    uint64_t total_bits = static_cast<uint64_t>(len) * 8;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() + 8) % 64 != 0) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF));
    }
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) |
                   (msg[chunk + i * 4 + 2] << 8) | msg[chunk + i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::array<uint8_t, 20> digest{};
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
    return digest;
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t triple = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t triple = data[i] << 16;
        if (i + 1 < data.size()) triple |= data[i + 1] << 8;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        if (i + 1 < data.size()) {
            out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool header_has_token(const std::string& value, const std::string& token) {
    std::string lower = to_lower(value);
    std::string lower_token = to_lower(token);
    std::istringstream iss(lower);
    std::string part;
    while (std::getline(iss, part, ',')) {
        part.erase(part.begin(),
                   std::find_if(part.begin(), part.end(),
                                [](unsigned char ch) { return !std::isspace(ch); }));
        part.erase(std::find_if(part.rbegin(), part.rend(),
                                [](unsigned char ch) { return !std::isspace(ch); })
                       .base(),
                   part.end());
        if (part == lower_token) return true;
    }
    return false;
}

// RFC 6455 §4.1: Sec-WebSocket-Key is a base64-encoded 16-byte nonce,
// i.e. a 24-character base64 string ending in "==". Reject anything else
// rather than computing an Accept over a malformed key.
bool is_valid_ws_key(const std::string& key) {
    if (key.size() != 24) return false;
    auto is_b64 = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '+' || c == '/';
    };
    for (std::size_t i = 0; i < 22; ++i) {
        if (!is_b64(key[i])) return false;
    }
    return key[22] == '=' && key[23] == '=';
}

} // namespace

auto parse_handshake(const std::string& request) -> std::optional<HandshakeRequest> {
    auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) return std::nullopt;
    std::istringstream iss(request.substr(0, header_end));
    std::string line;
    HandshakeRequest req;
    if (!std::getline(iss, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream start_line(line);
    start_line >> req.method;
    if (req.method.empty()) return std::nullopt;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = to_lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(),
                                 [](unsigned char ch) { return !std::isspace(ch); }));
        req.headers[name] = value;
    }
    return req;
}

auto is_upgrade(const HandshakeRequest& req) -> bool {
    const auto& hdrs = req.headers;
    auto upgrade_it = hdrs.find("upgrade");
    auto conn_it = hdrs.find("connection");
    auto key_it = hdrs.find("sec-websocket-key");
    auto version_it = hdrs.find("sec-websocket-version");
    return upgrade_it != hdrs.end() && conn_it != hdrs.end() && key_it != hdrs.end() &&
           version_it != hdrs.end() && req.method == "GET" &&
           header_has_token(upgrade_it->second, "websocket") &&
           header_has_token(conn_it->second, "upgrade") &&
           to_lower(version_it->second) == "13" && is_valid_ws_key(key_it->second);
}

auto compute_accept(const std::string& key) -> std::string {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = key + kGuid;
    auto digest = sha1(reinterpret_cast<const uint8_t*>(concat.data()), concat.size());
    return base64_encode(std::vector<uint8_t>(digest.begin(), digest.end()));
}

} // namespace spaznet::websocket::detail
