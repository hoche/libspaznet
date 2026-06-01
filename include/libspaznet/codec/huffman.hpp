#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// RFC 7541 Appendix B static Huffman code used by HTTP/2 HPACK and QPACK
// for literal name/value compression. The encoder appends the bit stream
// to the byte vector, padding the final byte with EOS-pattern 1-bits per
// RFC 7541 §5.2. The decoder runs the inverse; it returns false on
// truncation, on an EOS code embedded in the stream (which the RFC
// forbids — §5.2), or on any pad longer than 7 bits.

namespace spaznet::codec {


// Encode `input` into Huffman-compressed bytes; appends to `out`.
auto huffman_encode(std::string_view input, std::vector<uint8_t>& out) -> void;

// Number of bytes the Huffman encoding of `input` would occupy.
auto huffman_encoded_size(std::string_view input) -> std::size_t;

// Decode `input` Huffman bytes into `out` (a string). Returns true on
// success.
[[nodiscard]] auto huffman_decode(std::span<const uint8_t> input, std::string& out) -> bool;


} // namespace spaznet::codec
