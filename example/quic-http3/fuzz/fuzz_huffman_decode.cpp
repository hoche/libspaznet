// libFuzzer harness for the shared RFC 7541 §B Huffman decoder used
// by both HPACK (HTTP/2) and QPACK (HTTP/3).  Lives in core
// (spaznet::codec) so a single fuzzer covers both protocol stacks'
// Huffman input paths.

#include <libspaznet/codec/huffman.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::span<const uint8_t> in{data, size};
    std::string out;
    (void)spaznet::codec::huffman_decode(in, out);
    return 0;
}
