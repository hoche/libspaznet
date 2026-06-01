// libFuzzer harness for spaznet::quic::parse_long_header — the first
// thing run on every received Initial / Handshake / 0-RTT packet, on
// any wire-format byte sequence from a peer.  Must not crash on
// truncated, oversized, or otherwise malformed long-header packets.

#include <libspaznet/quic/packet.hpp>

#include <cstddef>
#include <cstdint>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::span<const uint8_t> dg{data, size};
    std::size_t off = 0;
    spaznet::quic::LongHeader hdr;
    (void)spaznet::quic::parse_long_header(dg, off, hdr);
    return 0;
}
