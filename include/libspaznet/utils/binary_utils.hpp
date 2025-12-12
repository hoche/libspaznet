#pragma once

#include <cstdint>
#include <vector>

namespace spaznet {

class BinaryUtils {
  public:
    // Encode unsigned integers to big-endian byte vectors
    static auto encode_uint16_be(uint16_t value) -> std::vector<uint8_t>;
    static auto encode_uint24_be(uint32_t value) -> std::vector<uint8_t>; // lower 24 bits
    static auto encode_uint32_be(uint32_t value) -> std::vector<uint8_t>;

    // Decode unsigned integers from big-endian byte arrays
    static auto decode_uint16_be(const uint8_t* data) -> uint16_t;
    static auto decode_uint24_be(const uint8_t* data) -> uint32_t;
    static auto decode_uint32_be(const uint8_t* data) -> uint32_t;
};

} // namespace spaznet
