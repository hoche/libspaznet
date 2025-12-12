#include <libspaznet/utils/binary_utils.hpp>

namespace spaznet {

namespace {
constexpr uint8_t BYTE_MASK = 0xFF;
constexpr int BITS_PER_BYTE = 8;
constexpr int SHIFT_16_BITS = 16;
constexpr int SHIFT_24_BITS = 24;
} // namespace

auto BinaryUtils::encode_uint16_be(uint16_t value) -> std::vector<uint8_t> {
    return {static_cast<uint8_t>((value >> BITS_PER_BYTE) & BYTE_MASK),
            static_cast<uint8_t>(value & BYTE_MASK)};
}

auto BinaryUtils::encode_uint24_be(uint32_t value) -> std::vector<uint8_t> {
    return {static_cast<uint8_t>((value >> SHIFT_16_BITS) & BYTE_MASK),
            static_cast<uint8_t>((value >> BITS_PER_BYTE) & BYTE_MASK),
            static_cast<uint8_t>(value & BYTE_MASK)};
}

auto BinaryUtils::encode_uint32_be(uint32_t value) -> std::vector<uint8_t> {
    return {static_cast<uint8_t>((value >> SHIFT_24_BITS) & BYTE_MASK),
            static_cast<uint8_t>((value >> SHIFT_16_BITS) & BYTE_MASK),
            static_cast<uint8_t>((value >> BITS_PER_BYTE) & BYTE_MASK),
            static_cast<uint8_t>(value & BYTE_MASK)};
}

auto BinaryUtils::decode_uint16_be(const uint8_t* data) -> uint16_t {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << BITS_PER_BYTE) |
                                 static_cast<uint16_t>(data[1]));
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

auto BinaryUtils::decode_uint24_be(const uint8_t* data) -> uint32_t {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return (static_cast<uint32_t>(data[0]) << SHIFT_16_BITS) |
           (static_cast<uint32_t>(data[1]) << BITS_PER_BYTE) | static_cast<uint32_t>(data[2]);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

auto BinaryUtils::decode_uint32_be(const uint8_t* data) -> uint32_t {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return (static_cast<uint32_t>(data[0]) << SHIFT_24_BITS) |
           (static_cast<uint32_t>(data[1]) << SHIFT_16_BITS) |
           (static_cast<uint32_t>(data[2]) << BITS_PER_BYTE) | static_cast<uint32_t>(data[3]);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

} // namespace spaznet
