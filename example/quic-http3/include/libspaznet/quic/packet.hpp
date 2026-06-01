#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <libspaznet/quic/crypto.hpp>

namespace spaznet {
namespace quic {

// QUIC v1 wire constants.
constexpr uint32_t kQuicV1 = 0x00000001U;
constexpr std::size_t kMaxConnectionIdLen = 20;
constexpr std::size_t kSampleLen = 16;
constexpr std::size_t kMaxPnLen = 4;

// Long-header packet types (RFC 9000 §17.2 — top-bit nibble values).
enum class LongType : uint8_t {
    Initial = 0,
    ZeroRtt = 1,
    Handshake = 2,
    Retry = 3,
};

// Decoded view of a parsed long-header packet, before header/payload
// protection has been removed.
struct LongHeader {
    LongType type{};
    uint32_t version{};
    std::vector<uint8_t> dcid;
    std::vector<uint8_t> scid;
    // Initial-only:
    std::vector<uint8_t> token;
    // Length field (Initial/0-RTT/Handshake): the encoded length covering
    // packet number + payload + auth tag, as advertised on the wire.
    uint64_t length{0};
    // Byte offset into the datagram at which the protected packet-number
    // field begins. Used by header protection.
    std::size_t pn_offset{0};
    // Total bytes consumed in the datagram (header + length + payload + tag).
    std::size_t packet_len{0};
};

// Parse the long header at `datagram[offset..]`. On success returns true,
// fills `hdr`, and advances `offset` to the start of the next coalesced
// packet in the datagram. On failure returns false (offset unchanged).
//
// This does NOT remove header protection or decrypt the payload; the
// caller is expected to feed `hdr` into `remove_header_protection` and
// `decrypt_payload` (or use `decrypt_long_packet`).
[[nodiscard]] auto parse_long_header(std::span<const uint8_t> datagram, std::size_t& offset,
                                     LongHeader& hdr) -> bool;

// Compute the nonce for a given packet number against an IV by
// big-endian-padding the PN to iv.size() bytes and XOR-ing with the IV.
auto make_aead_nonce(std::span<const uint8_t> iv, uint64_t packet_number)
    -> std::vector<uint8_t>;

// Decode a truncated packet number against the receiver's largest-acked PN
// per RFC 9000 §A.3.
auto decode_packet_number(uint64_t largest_pn, uint64_t truncated_pn, std::size_t pn_nbits)
    -> uint64_t;

// Decrypt a long-header packet in place using `keys`. On entry `datagram`
// holds the entire protected datagram and `hdr` describes the (still
// protected) header. On success returns the decoded packet number and
// fills `plaintext` with the frame payload. The header in `datagram` is
// also unmasked, so subsequent packets in the same datagram can be parsed.
//
// `largest_pn_seen` is the receiver's largest seen PN in this PN space (0
// before any have been received).
[[nodiscard]] auto decrypt_long_packet(Aead aead, const PacketKeys& keys, uint64_t largest_pn_seen,
                                       std::vector<uint8_t>& datagram, const LongHeader& hdr,
                                       uint64_t& packet_number, std::vector<uint8_t>& plaintext)
    -> bool;

// Serialize an Initial packet (RFC 9000 §17.2.2 + RFC 9001 §5.3 + §5.4).
//
// The returned bytes are fully protected (AEAD + header protection) and
// can be transmitted as-is. `packet_number` must be the full 64-bit PN; we
// pick the shortest sensible truncation. Caller is responsible for
// supplying any required PADDING in `payload_frames` to meet the 1200-byte
// client-Initial minimum (RFC 9000 §14.1) — this routine only encodes.
[[nodiscard]] auto build_initial_packet(Aead aead, const PacketKeys& keys,
                                        std::span<const uint8_t> dcid,
                                        std::span<const uint8_t> scid,
                                        std::span<const uint8_t> token,
                                        std::span<const uint8_t> payload_frames,
                                        uint64_t packet_number) -> std::vector<uint8_t>;

} // namespace quic
} // namespace spaznet
