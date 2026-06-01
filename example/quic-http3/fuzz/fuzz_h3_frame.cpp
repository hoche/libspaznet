// libFuzzer harness for the HTTP/3 frame codec (RFC 9114 §7).
// HEADERS, DATA, SETTINGS, GOAWAY, PUSH_PROMISE all share a varint
// type + varint length prefix; parse_h3_frame walks that wire format.

#include <libspaznet/http3/h3frame.hpp>

#include <cstddef>
#include <cstdint>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::span<const uint8_t> buf{data, size};
    std::size_t off = 0;
    spaznet::http3::H3Frame f;
    while (off < buf.size() && spaznet::http3::parse_h3_frame(buf, off, f)) {
    }
    return 0;
}
