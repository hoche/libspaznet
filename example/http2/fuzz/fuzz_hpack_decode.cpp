// libFuzzer harness for the RFC 7541 HPACK decoder.  Hostile peers can
// drive arbitrary bytes through this on every HEADERS / CONTINUATION
// frame, so the parse path needs to survive any input without crashing.
// The HPACK rewrite that landed on 2026-05-31 added proper prefix-N
// varints + Huffman decode + dynamic-table-size-update handling; this
// harness covers all four representations and the varint extension
// paths.

#include <libspaznet/http2/handler.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::vector<uint8_t> buf(data, data + size);
    (void)spaznet::http2::HPACK::decode_headers(buf);
    return 0;
}
