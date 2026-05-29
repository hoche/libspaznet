#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spaznet {
namespace quic {

// RFC 9000 §16 variable-length integer encoding (QUIC v1).
//
// The two most significant bits of the first byte encode the length:
//   00 -> 1 byte,  6-bit value
//   01 -> 2 byte, 14-bit value
//   10 -> 4 byte, 30-bit value
//   11 -> 8 byte, 62-bit value
// All values are big-endian; the length prefix occupies the top of the
// first byte and is masked off when reconstructing the value.
struct VarInt {
    static constexpr uint64_t kMax = (1ULL << 62) - 1;

    // Number of bytes a value will consume on the wire.
    [[nodiscard]] static auto encoded_size(uint64_t value) -> std::size_t;

    // Encode `value` into a freshly returned buffer.
    [[nodiscard]] static auto encode(uint64_t value) -> std::vector<uint8_t>;

    // Append `value`'s wire encoding to `out`.
    static auto append(std::vector<uint8_t>& out, uint64_t value) -> void;

    // Write `value`'s encoding to `out` (must have at least encoded_size(value)
    // bytes available). Returns the number of bytes written.
    static auto write(uint8_t* out, uint64_t value) -> std::size_t;

    // Decode a varint from `buf[offset..buf_len)`. On success advances `offset`
    // past the decoded varint, sets `value`, and returns true. On truncation
    // returns false and leaves `offset`/`value` unspecified.
    [[nodiscard]] static auto decode(const uint8_t* buf, std::size_t buf_len,
                                     std::size_t& offset, uint64_t& value) -> bool;

    // Decode-from-vector convenience overload.
    [[nodiscard]] static auto decode(const std::vector<uint8_t>& buf, std::size_t& offset,
                                     uint64_t& value) -> bool;
};

} // namespace quic
} // namespace spaznet
