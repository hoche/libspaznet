// libFuzzer harness for spaznet::quic::parse_frame / parse_frames.
// Every byte inside a decrypted QUIC packet body passes through these.

#include <libspaznet/quic/frame.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::span<const uint8_t> buf{data, size};
    std::size_t off = 0;
    spaznet::quic::Frame f;
    while (off < buf.size() && spaznet::quic::parse_frame(buf, off, f)) {
    }
    std::vector<spaznet::quic::Frame> frames;
    (void)spaznet::quic::parse_frames(buf, frames);
    return 0;
}
