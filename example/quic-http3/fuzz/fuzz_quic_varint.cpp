// libFuzzer harness for the QUIC varint codec (RFC 9000 §16).
// VarInt::decode is the lowest-level primitive in every QUIC parser;
// anything reachable here is reachable from frames, headers,
// transport params, HTTP/3 frames, and QPACK.

#include <libspaznet/quic/varint.hpp>

#include <cstddef>
#include <cstdint>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::size_t off = 0;
    while (off < size) {
        uint64_t v = 0;
        if (!spaznet::quic::VarInt::decode(data, size, off, v)) break;
    }
    return 0;
}
