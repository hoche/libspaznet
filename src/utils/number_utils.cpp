#include <libspaznet/utils/number_utils.hpp>

#include <climits>
#include <cstdint>

namespace spaznet {

// Strict decimal parse. Unlike std::stoull/std::stoi, this rejects a
// leading sign, surrounding whitespace, alternate bases, and trailing
// garbage — it accepts only a non-empty run of ASCII digits. That
// matters for security: parse_uint64 backs HTTP Content-Length, and
// std::stoull would silently accept " 5", "+5", "0x10", "5abc", and
// even wrap "-1" to a huge value — all request-smuggling / length-
// confusion vectors.
auto NumberUtils::parse_uint64(const std::string& value) -> std::optional<uint64_t> {
    if (value.empty()) {
        return std::nullopt;
    }
    uint64_t result = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<uint64_t>(c - '0');
        if (result > (UINT64_MAX - digit) / 10) {
            return std::nullopt; // would overflow uint64_t
        }
        result = result * 10 + digit;
    }
    return result;
}

// Strict signed decimal parse: an optional leading '+'/'-' followed by a
// non-empty run of digits, nothing else. Rejects whitespace, trailing
// garbage, and out-of-range values.
auto NumberUtils::parse_int(const std::string& value) -> std::optional<int> {
    if (value.empty()) {
        return std::nullopt;
    }
    std::size_t i = 0;
    bool negative = false;
    if (value[0] == '+' || value[0] == '-') {
        negative = value[0] == '-';
        i = 1;
    }
    if (i >= value.size()) {
        return std::nullopt; // sign with no digits
    }
    long long result = 0; // wider accumulator for range checking
    for (; i < value.size(); ++i) {
        const char c = value[i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        result = result * 10 + (c - '0');
        // Magnitude can't exceed |INT_MIN| (2147483648); bail early to
        // keep the accumulator from overflowing.
        if (result > -static_cast<long long>(INT_MIN)) {
            return std::nullopt;
        }
    }
    if (negative) {
        result = -result;
    }
    if (result < INT_MIN || result > INT_MAX) {
        return std::nullopt;
    }
    return static_cast<int>(result);
}

} // namespace spaznet
