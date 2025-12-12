#pragma once

#include <cstdint>
#include <vector>

namespace spaznet {

class BinaryUtils {
  public:
    // Encode unsigned integers to big-endian byte vectors
    static std::vector<uint8_t> encode_uint16_be(uint16_t value);
    static std::vector<uint8_t> encode_uint24_be(uint32_t value); // lower 24 bits
    static std::vector<uint8_t> encode_uint32_be(uint32_t value);

    // Decode unsigned integers from big-endian byte arrays
    static uint16_t decode_uint16_be(const uint8_t* data);
    static uint32_t decode_uint24_be(const uint8_t* data);
    static uint32_t decode_uint32_be(const uint8_t* data);
};

} // namespace spaznet
