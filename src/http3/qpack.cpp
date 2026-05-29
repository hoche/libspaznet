#include <libspaznet/http3/qpack.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include <libspaznet/http3/huffman.hpp>

namespace spaznet {
namespace http3 {

namespace {

// RFC 9204 Appendix A — QPACK static table (99 entries, index 0..98).
constexpr std::array<StaticEntry, 99> kStatic = {{
    {":authority", ""},
    {":path", "/"},
    {"age", "0"},
    {"content-disposition", ""},
    {"content-length", "0"},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"referer", ""},
    {"set-cookie", ""},
    {":method", "CONNECT"},
    {":method", "DELETE"},
    {":method", "GET"},
    {":method", "HEAD"},
    {":method", "OPTIONS"},
    {":method", "POST"},
    {":method", "PUT"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "103"},
    {":status", "200"},
    {":status", "304"},
    {":status", "404"},
    {":status", "503"},
    {"accept", "*/*"},
    {"accept", "application/dns-message"},
    {"accept-encoding", "gzip, deflate, br"},
    {"accept-ranges", "bytes"},
    {"access-control-allow-headers", "cache-control"},
    {"access-control-allow-headers", "content-type"},
    {"access-control-allow-origin", "*"},
    {"cache-control", "max-age=0"},
    {"cache-control", "max-age=2592000"},
    {"cache-control", "max-age=604800"},
    {"cache-control", "no-cache"},
    {"cache-control", "no-store"},
    {"cache-control", "public, max-age=31536000"},
    {"content-encoding", "br"},
    {"content-encoding", "gzip"},
    {"content-type", "application/dns-message"},
    {"content-type", "application/javascript"},
    {"content-type", "application/json"},
    {"content-type", "application/x-www-form-urlencoded"},
    {"content-type", "image/gif"},
    {"content-type", "image/jpeg"},
    {"content-type", "image/png"},
    {"content-type", "text/css"},
    {"content-type", "text/html; charset=utf-8"},
    {"content-type", "text/plain"},
    {"content-type", "text/plain;charset=utf-8"},
    {"range", "bytes=0-"},
    {"strict-transport-security", "max-age=31536000"},
    {"strict-transport-security", "max-age=31536000; includesubdomains"},
    {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
    {"vary", "accept-encoding"},
    {"vary", "origin"},
    {"x-content-type-options", "nosniff"},
    {"x-xss-protection", "1; mode=block"},
    {":status", "100"},
    {":status", "204"},
    {":status", "206"},
    {":status", "302"},
    {":status", "400"},
    {":status", "403"},
    {":status", "421"},
    {":status", "425"},
    {":status", "500"},
    {"accept-language", ""},
    {"access-control-allow-credentials", "FALSE"},
    {"access-control-allow-credentials", "TRUE"},
    {"access-control-allow-headers", "*"},
    {"access-control-allow-methods", "get"},
    {"access-control-allow-methods", "get, post, options"},
    {"access-control-allow-methods", "options"},
    {"access-control-expose-headers", "content-length"},
    {"access-control-request-headers", "content-type"},
    {"access-control-request-method", "get"},
    {"access-control-request-method", "post"},
    {"alt-svc", "clear"},
    {"authorization", ""},
    {"content-security-policy",
     "script-src 'none'; object-src 'none'; base-uri 'none'"},
    {"early-data", "1"},
    {"expect-ct", ""},
    {"forwarded", ""},
    {"if-range", ""},
    {"origin", ""},
    {"purpose", "prefetch"},
    {"server", ""},
    {"timing-allow-origin", "*"},
    {"upgrade-insecure-requests", "1"},
    {"user-agent", ""},
    {"x-forwarded-for", ""},
    {"x-frame-options", "deny"},
    {"x-frame-options", "sameorigin"},
}};

// Encode a QPACK integer with N-bit prefix (RFC 9204 §4.1.1).
auto write_int(std::vector<uint8_t>& out, uint64_t value, uint8_t prefix, int n) -> void {
    const uint64_t max_prefix = (1ULL << n) - 1;
    if (value < max_prefix) {
        out.push_back(static_cast<uint8_t>(prefix | value));
        return;
    }
    out.push_back(static_cast<uint8_t>(prefix | max_prefix));
    value -= max_prefix;
    while (value >= 128) {
        out.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

auto read_int(std::span<const uint8_t> buf, std::size_t& off, int n, uint64_t& out)
    -> bool {
    if (off >= buf.size()) return false;
    const uint64_t max_prefix = (1ULL << n) - 1;
    out = buf[off++] & max_prefix;
    if (out < max_prefix) return true;
    uint64_t shift = 0;
    while (true) {
        if (off >= buf.size()) return false;
        uint8_t b = buf[off++];
        out += static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
        if (shift > 63) return false;
    }
}

// Encode a literal string with H-bit prefix at position `bits`. We
// always Huffman-encode literals on emit.
auto write_string(std::vector<uint8_t>& out, std::string_view s, uint8_t prefix_bits,
                  int n) -> void {
    std::vector<uint8_t> huff;
    huffman_encode(s, huff);
    const uint8_t huff_bit = static_cast<uint8_t>(1U << n); // H=1 set
    write_int(out, huff.size(), prefix_bits | huff_bit, n);
    out.insert(out.end(), huff.begin(), huff.end());
}

auto read_string(std::span<const uint8_t> buf, std::size_t& off, int n, std::string& out)
    -> bool {
    if (off >= buf.size()) return false;
    const bool huff = (buf[off] & (1U << n)) != 0;
    uint64_t len = 0;
    if (!read_int(buf, off, n, len)) return false;
    if (off + len > buf.size()) return false;
    std::span<const uint8_t> raw{buf.data() + off, static_cast<std::size_t>(len)};
    off += static_cast<std::size_t>(len);
    if (huff) {
        return huffman_decode(raw, out);
    }
    out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    return true;
}

// Build name -> [indices] and (name,value) -> index lookups once.
struct StaticIndex {
    std::unordered_map<std::string, std::vector<uint64_t>> by_name;
    std::unordered_map<std::string, uint64_t> by_pair;

    StaticIndex() {
        for (std::size_t i = 0; i < kStatic.size(); ++i) {
            by_name[kStatic[i].name].push_back(i);
            by_pair[std::string(kStatic[i].name) + std::string("\x1f", 1) + kStatic[i].value] =
                i;
        }
    }
};

auto static_index() -> const StaticIndex& {
    static const StaticIndex idx;
    return idx;
}

} // namespace

auto qpack_static_table() -> std::span<const StaticEntry> {
    return {kStatic.data(), kStatic.size()};
}

auto qpack_encode(const HeaderList& headers, std::vector<uint8_t>& out) -> void {
    // Encoded Field Section Prefix: Required Insert Count = 0, Sign = 0,
    // Delta Base = 0 -> two zero varints, both with the appropriate
    // prefix forms ("varint with 8-bit prefix" and "varint with 7-bit
    // prefix"). Per RFC 9204 §4.5.1, the encoder must encode
    // Required Insert Count = 0 as the literal byte 0x00, and the
    // Delta Base byte as 0x00 (Sign=0, Delta=0). Two zero bytes.
    out.push_back(0x00);
    out.push_back(0x00);

    const auto& idx = static_index();
    for (const auto& [name, value] : headers) {
        // Try (name, value) full match.
        std::string key = name + std::string("\x1f", 1) + value;
        auto pit = idx.by_pair.find(key);
        if (pit != idx.by_pair.end()) {
            // 4.5.2 Indexed Field Line — Static: pattern 1 1 + 6-bit index.
            write_int(out, pit->second, 0b1100'0000, 6);
            continue;
        }
        // Try name match → Literal with Name Reference (4.5.4).
        auto nit = idx.by_name.find(name);
        if (nit != idx.by_name.end() && !nit->second.empty()) {
            // Pattern: 0 1 N T (Static=1) + 4-bit index prefix.
            // N=0 (allow indexing — not used with capacity 0 but legal),
            // T=1 (Static).
            uint8_t prefix = 0b0101'0000;
            write_int(out, nit->second.front(), prefix, 4);
            // Literal value with 7-bit prefix and H flag at bit 7.
            write_string(out, value, 0x00, 7);
            continue;
        }
        // Literal with Literal Name (4.5.6). Pattern: 0 0 1 N H bits +
        // 3-bit name length prefix.
        uint8_t prefix = 0b0010'0000; // 001-prefix; N=0; H bit set by write_string
        write_string(out, name, prefix, 3);
        write_string(out, value, 0x00, 7);
    }
}

auto qpack_decode(std::span<const uint8_t> input, HeaderList& out) -> bool {
    out.clear();
    std::size_t off = 0;
    // Field section prefix: Required Insert Count (8-bit prefix) and
    // Delta Base (Sign + 7-bit varint).
    uint64_t enc_insert_count = 0;
    if (!read_int(input, off, 8, enc_insert_count)) return false;
    if (off >= input.size()) return false;
    // The next byte is Sign in bit 7 and Delta Base in low 7 bits.
    uint64_t delta_base = 0;
    if (!read_int(input, off, 7, delta_base)) return false;
    // We don't support dynamic table — anything non-zero is an error.
    if (enc_insert_count != 0 || delta_base != 0) return false;

    const auto& tbl = kStatic;
    while (off < input.size()) {
        uint8_t b = input[off];
        if ((b & 0b1000'0000) != 0) {
            // 1 T + 6-bit index (Indexed Field Line, §4.5.2).
            const bool is_static = (b & 0b0100'0000) != 0;
            uint64_t index = 0;
            if (!read_int(input, off, 6, index)) return false;
            if (!is_static || index >= tbl.size()) return false;
            out.emplace_back(tbl[index].name, tbl[index].value);
        } else if ((b & 0b0100'0000) != 0) {
            // 0 1 N T + 4-bit index (Literal with Name Reference, §4.5.4).
            const bool is_static = (b & 0b0001'0000) != 0;
            uint64_t index = 0;
            if (!read_int(input, off, 4, index)) return false;
            std::string value;
            if (!read_string(input, off, 7, value)) return false;
            if (!is_static || index >= tbl.size()) return false;
            out.emplace_back(tbl[index].name, std::move(value));
        } else if ((b & 0b0010'0000) != 0) {
            // 0 0 1 N H + 3-bit name length (Literal with Literal Name, §4.5.6).
            std::string name;
            std::string value;
            if (!read_string(input, off, 3, name)) return false;
            if (!read_string(input, off, 7, value)) return false;
            out.emplace_back(std::move(name), std::move(value));
        } else if ((b & 0b0001'0000) != 0) {
            // 0 0 0 1 + 4-bit index (Indexed Field Line w/ Post-Base
            // Index, §4.5.3). We don't support dynamic tables.
            return false;
        } else {
            // 0 0 0 0 + bits — Literal with Post-Base Name Reference
            // (§4.5.5). Also dynamic; reject.
            return false;
        }
    }
    return true;
}

} // namespace http3
} // namespace spaznet
