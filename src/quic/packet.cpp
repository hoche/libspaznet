#include <libspaznet/quic/packet.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <libspaznet/quic/varint.hpp>

namespace spaznet {
namespace quic {

namespace {

constexpr uint8_t kLongHeaderBit = 0x80;
constexpr uint8_t kFixedBit = 0x40;
constexpr uint8_t kLongTypeMask = 0x30;
constexpr uint8_t kLongTypeShift = 4;
constexpr uint8_t kLongHpMask = 0x0F; // bits subject to HP on long headers
constexpr uint8_t kPnLenMask = 0x03;

auto pn_length_from_first_byte(uint8_t first) -> std::size_t {
    return (first & kPnLenMask) + 1;
}

} // namespace

auto parse_long_header(std::span<const uint8_t> datagram, std::size_t& offset, LongHeader& hdr)
    -> bool {
    const std::size_t start = offset;
    if (start >= datagram.size()) {
        return false;
    }
    const uint8_t first = datagram[start];
    if ((first & kLongHeaderBit) == 0) {
        return false; // short header
    }
    // Fixed Bit MUST be 1 for non-Version-Negotiation packets; we let
    // the caller handle VN separately (Version == 0 path below still
    // returns the header).
    std::size_t off = start + 1;
    if (off + 4 > datagram.size()) {
        return false;
    }
    uint32_t version = (uint32_t(datagram[off]) << 24) | (uint32_t(datagram[off + 1]) << 16) |
                       (uint32_t(datagram[off + 2]) << 8) | uint32_t(datagram[off + 3]);
    off += 4;

    if (off >= datagram.size()) {
        return false;
    }
    std::size_t dcid_len = datagram[off++];
    if (dcid_len > kMaxConnectionIdLen || off + dcid_len > datagram.size()) {
        return false;
    }
    std::vector<uint8_t> dcid(datagram.data() + off, datagram.data() + off + dcid_len);
    off += dcid_len;

    if (off >= datagram.size()) {
        return false;
    }
    std::size_t scid_len = datagram[off++];
    if (scid_len > kMaxConnectionIdLen || off + scid_len > datagram.size()) {
        return false;
    }
    std::vector<uint8_t> scid(datagram.data() + off, datagram.data() + off + scid_len);
    off += scid_len;

    hdr.version = version;
    hdr.dcid = std::move(dcid);
    hdr.scid = std::move(scid);
    hdr.type = static_cast<LongType>((first & kLongTypeMask) >> kLongTypeShift);

    // Version 0 == Version Negotiation; no length/PN/payload follows.
    if (version == 0) {
        hdr.pn_offset = 0;
        hdr.packet_len = datagram.size() - start;
        offset = datagram.size();
        return true;
    }

    if (hdr.type == LongType::Retry) {
        // Retry has no Length / PN; the remainder of the datagram is
        // Retry Token followed by a 16-byte Retry Integrity Tag.
        if (datagram.size() < off + 16) {
            return false;
        }
        hdr.token.assign(datagram.data() + off, datagram.data() + datagram.size() - 16);
        hdr.pn_offset = 0;
        hdr.packet_len = datagram.size() - start;
        offset = datagram.size();
        return true;
    }

    if (hdr.type == LongType::Initial) {
        uint64_t token_len = 0;
        if (!VarInt::decode(datagram.data(), datagram.size(), off, token_len)) {
            return false;
        }
        if (off + token_len > datagram.size()) {
            return false;
        }
        hdr.token.assign(datagram.data() + off, datagram.data() + off + token_len);
        off += token_len;
    }

    uint64_t length = 0;
    if (!VarInt::decode(datagram.data(), datagram.size(), off, length)) {
        return false;
    }
    hdr.length = length;
    hdr.pn_offset = off;
    if (off + length > datagram.size()) {
        return false;
    }
    hdr.packet_len = (off - start) + length;
    offset = off + length;
    return true;
}

auto make_aead_nonce(std::span<const uint8_t> iv, uint64_t packet_number)
    -> std::vector<uint8_t> {
    std::vector<uint8_t> nonce(iv.begin(), iv.end());
    for (std::size_t i = 0; i < 8; ++i) {
        nonce[nonce.size() - 1 - i] ^= static_cast<uint8_t>((packet_number >> (8 * i)) & 0xFF);
    }
    return nonce;
}

auto decode_packet_number(uint64_t largest_pn, uint64_t truncated_pn, std::size_t pn_nbits)
    -> uint64_t {
    // RFC 9000 Appendix A.3. The RFC pseudocode uses unbounded integers;
    // for unsigned arithmetic we have to guard the subtractions/additions
    // against underflow/overflow before applying the window adjustments.
    const uint64_t expected_pn = largest_pn + 1;
    const uint64_t pn_win = 1ULL << pn_nbits;
    const uint64_t pn_hwin = pn_win / 2;
    const uint64_t pn_mask = pn_win - 1;
    uint64_t candidate_pn = (expected_pn & ~pn_mask) | truncated_pn;
    if (expected_pn > pn_hwin && candidate_pn <= expected_pn - pn_hwin &&
        candidate_pn < (1ULL << 62) - pn_win) {
        candidate_pn += pn_win;
    } else if (candidate_pn > expected_pn + pn_hwin && candidate_pn >= pn_win) {
        candidate_pn -= pn_win;
    }
    return candidate_pn;
}

auto decrypt_long_packet(Aead aead, const PacketKeys& keys, uint64_t largest_pn_seen,
                         std::vector<uint8_t>& datagram, const LongHeader& hdr,
                         uint64_t& packet_number, std::vector<uint8_t>& plaintext) -> bool {
    if (hdr.pn_offset == 0 || hdr.length < kSampleLen + 1) {
        return false;
    }
    // Sample is 16 bytes starting 4 bytes past the start of the PN field.
    const std::size_t sample_off = hdr.pn_offset + 4;
    if (sample_off + kSampleLen > datagram.size()) {
        return false;
    }
    auto mask = header_protection_mask(
        aead, std::span<const uint8_t>{keys.hp.data(), keys.hp.size()},
        std::span<const uint8_t>{datagram.data() + sample_off, kSampleLen});

    // Remove HP from the first byte (low 4 bits for long header).
    datagram[0] ^= mask[0] & kLongHpMask;
    const std::size_t pn_len = pn_length_from_first_byte(datagram[0]);

    // Remove HP from PN bytes.
    for (std::size_t i = 0; i < pn_len; ++i) {
        datagram[hdr.pn_offset + i] ^= mask[1 + i];
    }

    // Reconstruct truncated PN and full PN.
    uint64_t truncated = 0;
    for (std::size_t i = 0; i < pn_len; ++i) {
        truncated = (truncated << 8) | datagram[hdr.pn_offset + i];
    }
    packet_number = decode_packet_number(largest_pn_seen, truncated, pn_len * 8);

    // AAD = unprotected header bytes (from byte 0 through end of PN).
    const std::size_t header_len = hdr.pn_offset + pn_len;
    // Find the packet's start. parse_long_header consumed packets from
    // datagram[0]; for coalesced packets the caller must operate per
    // packet — for now we assume hdr describes the leading packet.
    std::span<const uint8_t> aad{datagram.data(), header_len};

    // Ciphertext = remainder of the packet payload (PN-length bytes already
    // accounted for; the AEAD covers payload+tag). Total payload length
    // from the wire = hdr.length - pn_len.
    const std::size_t ct_off = header_len;
    const std::size_t ct_len = hdr.length - pn_len;
    if (ct_off + ct_len > datagram.size()) {
        return false;
    }
    std::span<const uint8_t> ciphertext{datagram.data() + ct_off, ct_len};

    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, packet_number);
    return aead_open(aead, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()}, aad,
                     ciphertext, plaintext);
}

namespace {

auto pick_pn_length(uint64_t /*pn*/) -> std::size_t {
    // For our server's outgoing packets we use a fixed 4-byte PN; this is
    // simple and always within the RFC 9000 §17.1 range.
    return 4;
}

} // namespace

auto build_initial_packet(Aead aead, const PacketKeys& keys, std::span<const uint8_t> dcid,
                          std::span<const uint8_t> scid, std::span<const uint8_t> token,
                          std::span<const uint8_t> payload_frames, uint64_t packet_number)
    -> std::vector<uint8_t> {
    const std::size_t pn_len = pick_pn_length(packet_number);
    const std::size_t tag_len = aead_tag_length(aead);

    std::vector<uint8_t> pkt;
    pkt.reserve(64 + token.size() + payload_frames.size() + tag_len);

    // First byte: 1100 00LL — Initial, Fixed=1, type=0, reserved=0, PN length-1.
    uint8_t first = static_cast<uint8_t>(kLongHeaderBit | kFixedBit |
                                         (static_cast<uint8_t>(LongType::Initial) << kLongTypeShift) |
                                         static_cast<uint8_t>(pn_len - 1));
    pkt.push_back(first);

    // Version.
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 24));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 16));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 8));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 & 0xFF));

    // DCID + SCID.
    pkt.push_back(static_cast<uint8_t>(dcid.size()));
    pkt.insert(pkt.end(), dcid.begin(), dcid.end());
    pkt.push_back(static_cast<uint8_t>(scid.size()));
    pkt.insert(pkt.end(), scid.begin(), scid.end());

    // Token length + token (Initial only).
    VarInt::append(pkt, token.size());
    pkt.insert(pkt.end(), token.begin(), token.end());

    // Length field covers PN + payload + tag.
    const uint64_t length = pn_len + payload_frames.size() + tag_len;
    VarInt::append(pkt, length);

    // PN (encoded as pn_len big-endian bytes).
    const std::size_t pn_offset = pkt.size();
    for (std::size_t i = 0; i < pn_len; ++i) {
        pkt.push_back(static_cast<uint8_t>((packet_number >> (8 * (pn_len - 1 - i))) & 0xFF));
    }

    // Append plaintext payload as the AEAD input; it gets sealed in place.
    const std::size_t payload_off = pkt.size();
    pkt.insert(pkt.end(), payload_frames.begin(), payload_frames.end());

    // AAD = header bytes (everything written so far up to start of payload).
    std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
    std::vector<uint8_t> plaintext(pkt.begin() + payload_off, pkt.end());

    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, packet_number);
    std::vector<uint8_t> sealed;
    if (!aead_seal(aead, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
                   {aad.data(), aad.size()}, {plaintext.data(), plaintext.size()}, sealed)) {
        throw std::runtime_error("aead_seal failed");
    }
    // Replace plaintext bytes with sealed (ciphertext || tag).
    pkt.resize(payload_off);
    pkt.insert(pkt.end(), sealed.begin(), sealed.end());

    // Apply header protection.
    const std::size_t sample_off = pn_offset + 4;
    if (sample_off + kSampleLen > pkt.size()) {
        throw std::runtime_error("packet too short for HP sample");
    }
    auto mask = header_protection_mask(aead, {keys.hp.data(), keys.hp.size()},
                                       {pkt.data() + sample_off, kSampleLen});
    pkt[0] ^= mask[0] & kLongHpMask;
    for (std::size_t i = 0; i < pn_len; ++i) {
        pkt[pn_offset + i] ^= mask[1 + i];
    }

    return pkt;
}

} // namespace quic
} // namespace spaznet
