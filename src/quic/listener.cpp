#include <libspaznet/quic/listener.hpp>

#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>

#include <libspaznet/quic/crypto.hpp>
#include <libspaznet/quic/varint.hpp>

namespace spaznet {
namespace quic {

namespace {

// RFC 9001 §5.8: Retry Integrity Protection (QUIC v1).
constexpr std::array<uint8_t, 16> kRetryIntegrityKeyV1 = {0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66,
                                                          0x57, 0x5a, 0x1d, 0x76, 0x6b, 0x54,
                                                          0xe3, 0x68, 0xc8, 0x4e};
constexpr std::array<uint8_t, 12> kRetryIntegrityNonceV1 = {0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63,
                                                             0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb};

} // namespace

auto compute_retry_integrity_tag(std::span<const uint8_t> original_dcid,
                                 std::span<const uint8_t> retry_packet_without_tag,
                                 std::array<uint8_t, 16>& tag_out) -> bool {
    // Build pseudo-packet = ODCID Length (1 byte) | ODCID | Retry Packet.
    std::vector<uint8_t> pseudo;
    pseudo.reserve(1 + original_dcid.size() + retry_packet_without_tag.size());
    pseudo.push_back(static_cast<uint8_t>(original_dcid.size()));
    pseudo.insert(pseudo.end(), original_dcid.begin(), original_dcid.end());
    pseudo.insert(pseudo.end(), retry_packet_without_tag.begin(),
                  retry_packet_without_tag.end());

    // Tag = AES-128-GCM(key, nonce, AAD=pseudo, plaintext=empty).
    std::vector<uint8_t> sealed;
    if (!aead_seal(Aead::Aes128Gcm,
                   {kRetryIntegrityKeyV1.data(), kRetryIntegrityKeyV1.size()},
                   {kRetryIntegrityNonceV1.data(), kRetryIntegrityNonceV1.size()},
                   {pseudo.data(), pseudo.size()}, {}, sealed)) {
        return false;
    }
    if (sealed.size() != tag_out.size()) {
        return false;
    }
    std::memcpy(tag_out.data(), sealed.data(), tag_out.size());
    return true;
}

auto build_retry_packet(std::span<const uint8_t> client_scid,
                        std::span<const uint8_t> server_scid,
                        std::span<const uint8_t> token, std::span<const uint8_t> odcid)
    -> std::vector<uint8_t> {
    std::vector<uint8_t> pkt;
    // First byte: Long Form=1, Fixed Bit=1, Type=Retry(3). Low 4 bits
    // are "Unused" per RFC 9000 §17.2.5 — the sender picks any value
    // and the integrity tag covers them. We match the RFC 9001 §A.4
    // example's value (0xF) so test vectors are reproducible.
    pkt.push_back(static_cast<uint8_t>(0x80 | 0x40 |
                                       (static_cast<uint8_t>(LongType::Retry) << 4) | 0x0F));
    // Version (QUIC v1).
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 24));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 16));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 8));
    pkt.push_back(static_cast<uint8_t>(kQuicV1));
    // DCID == peer's SCID echoed back.
    pkt.push_back(static_cast<uint8_t>(client_scid.size()));
    pkt.insert(pkt.end(), client_scid.begin(), client_scid.end());
    // SCID == server's chosen new CID.
    pkt.push_back(static_cast<uint8_t>(server_scid.size()));
    pkt.insert(pkt.end(), server_scid.begin(), server_scid.end());
    // Retry Token (variable length, no length prefix — fills space).
    pkt.insert(pkt.end(), token.begin(), token.end());
    // Compute integrity tag and append.
    std::array<uint8_t, 16> tag{};
    if (!compute_retry_integrity_tag(odcid, {pkt.data(), pkt.size()}, tag)) {
        throw std::runtime_error("compute_retry_integrity_tag failed");
    }
    pkt.insert(pkt.end(), tag.begin(), tag.end());
    return pkt;
}

auto build_version_negotiation_packet(std::span<const uint8_t> client_dcid_echo,
                                      std::span<const uint8_t> client_scid_echo,
                                      std::span<const uint8_t> supported_versions_be)
    -> std::vector<uint8_t> {
    // RFC 9000 §17.2.1: first byte has the Long-form bit set; the
    // remaining bits are unused (servers SHOULD set them to random
    // values). Version is the 4-byte zero word. DCID + SCID are echoed
    // from the client's incoming packet. Supported versions follow as
    // a sequence of 4-byte network-order uint32s.
    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>(0xC0)); // long-form + fixed bit + unused
    // Version = 0 signals Version Negotiation.
    pkt.push_back(0);
    pkt.push_back(0);
    pkt.push_back(0);
    pkt.push_back(0);
    pkt.push_back(static_cast<uint8_t>(client_dcid_echo.size()));
    pkt.insert(pkt.end(), client_dcid_echo.begin(), client_dcid_echo.end());
    pkt.push_back(static_cast<uint8_t>(client_scid_echo.size()));
    pkt.insert(pkt.end(), client_scid_echo.begin(), client_scid_echo.end());
    pkt.insert(pkt.end(), supported_versions_be.begin(), supported_versions_be.end());
    return pkt;
}

// ---- Listener --------------------------------------------------------

Listener::Listener(Config cfg, SendFn send_fn)
    : cfg_(std::move(cfg)), send_fn_(std::move(send_fn)),
      prng_state_(cfg_.random_seed != 0 ? cfg_.random_seed
                                        : std::random_device{}() ^
                                              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this))) {
    if (cfg_.server_cid_length == 0 || cfg_.server_cid_length > kMaxConnectionIdLen) {
        cfg_.server_cid_length = 8;
    }
    // Token-validation key: derive deterministically from the PRNG seed
    // so a single Listener emits the same tokens through its lifetime
    // but two Listeners with different seeds disagree.
    uint64_t s = prng_state_;
    for (std::size_t i = 0; i < token_key_.size(); ++i) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        token_key_[i] = static_cast<uint8_t>(s & 0xFFU);
    }
}

auto Listener::new_random_cid(std::vector<uint8_t>& out) -> void {
    out.resize(cfg_.server_cid_length);
    uint64_t s = prng_state_;
    for (std::size_t i = 0; i < out.size(); ++i) {
        // xorshift64*
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        out[i] = static_cast<uint8_t>(s & 0xFFU);
    }
    prng_state_ = s;
}

auto Listener::make_retry_token(std::span<const uint8_t> odcid,
                                std::span<const uint8_t> peer_token_nonce)
    -> std::vector<uint8_t> {
    // Token format: peer_nonce (16) || odcid_len (1) || odcid || HMAC-trunc
    // The HMAC is computed with token_key_; we use a tiny ad-hoc MAC
    // (HKDF-Extract repurposed as HMAC-SHA256 via the existing helper).
    std::vector<uint8_t> token;
    token.insert(token.end(), peer_token_nonce.begin(), peer_token_nonce.end());
    token.push_back(static_cast<uint8_t>(odcid.size()));
    token.insert(token.end(), odcid.begin(), odcid.end());
    auto mac = hkdf_extract(Hash::Sha256, {token_key_.data(), token_key_.size()},
                            {token.data(), token.size()});
    token.insert(token.end(), mac.begin(), mac.begin() + 16);
    return token;
}

namespace {

// Parse a long header just enough to identify version + DCID + SCID for
// dispatch decisions. Returns false if the bytes don't look like a long
// header at all.
auto peek_long(std::span<const uint8_t> dg, uint32_t& version,
               std::vector<uint8_t>& dcid, std::vector<uint8_t>& scid) -> bool {
    if (dg.size() < 7) return false;
    if ((dg[0] & 0x80U) == 0) return false;
    version = (uint32_t(dg[1]) << 24) | (uint32_t(dg[2]) << 16) | (uint32_t(dg[3]) << 8) |
              uint32_t(dg[4]);
    std::size_t off = 5;
    if (off + 1 > dg.size()) return false;
    std::size_t dlen = dg[off++];
    if (off + dlen + 1 > dg.size() || dlen > kMaxConnectionIdLen) return false;
    dcid.assign(dg.begin() + static_cast<std::ptrdiff_t>(off),
                dg.begin() + static_cast<std::ptrdiff_t>(off + dlen));
    off += dlen;
    std::size_t slen = dg[off++];
    if (off + slen > dg.size() || slen > kMaxConnectionIdLen) return false;
    scid.assign(dg.begin() + static_cast<std::ptrdiff_t>(off),
                dg.begin() + static_cast<std::ptrdiff_t>(off + slen));
    return true;
}

} // namespace

auto Listener::on_datagram(std::span<const uint8_t> dg) -> void {
    if (dg.empty()) return;

    if ((dg[0] & 0x80U) != 0) {
        uint32_t version = 0;
        std::vector<uint8_t> dcid;
        std::vector<uint8_t> scid;
        if (!peek_long(dg, version, dcid, scid)) {
            return;
        }
        if (version != kQuicV1) {
            // Reply with Version Negotiation. Supported = {v1}.
            std::array<uint8_t, 4> v1_be = {0x00, 0x00, 0x00, 0x01};
            auto vn = build_version_negotiation_packet({dcid.data(), dcid.size()},
                                                       {scid.data(), scid.size()},
                                                       {v1_be.data(), v1_be.size()});
            send_fn_({vn.data(), vn.size()});
            return;
        }

        // Existing connection? Try direct first, then alias (for first
        // Initial retransmissions before the peer learns our SCID).
        auto it = connections_.find(dcid);
        if (it == connections_.end()) {
            auto alias_it = connections_aliases_.find(dcid);
            if (alias_it != connections_aliases_.end()) {
                it = connections_.find(alias_it->second);
            }
        }
        if (it == connections_.end()) {
            // Must be an Initial to start a new connection.
            const uint8_t type = (dg[0] >> 4) & 0x03U;
            if (type != static_cast<uint8_t>(LongType::Initial)) {
                return; // stateless drop
            }
            // Pick a server SCID and create a Connection. The client's
            // chosen DCID becomes our OD-CID (we use it for Initial-key
            // derivation inside Connection).
            std::vector<uint8_t> server_scid;
            new_random_cid(server_scid);
            auto conn = std::make_unique<Connection>(
                cfg_.tls_ctx, std::span<const uint8_t>{dcid.data(), dcid.size()},
                std::span<const uint8_t>{server_scid.data(), server_scid.size()},
                cfg_.server_tp, send_fn_);
            // Key map by server_scid so subsequent short-header packets
            // route correctly; also alias the original DCID until the
            // peer learns server_scid (after first server packet).
            auto* raw = conn.get();
            connections_.emplace(server_scid, std::move(conn));
            connections_aliases_[dcid] = server_scid;
            raw->on_datagram(dg);
            raw->on_timer();
            return;
        }
        it->second->on_datagram(dg);
        it->second->on_timer();
        return;
    }

    // Short header: DCID length is fixed at the server's chosen length.
    if (dg.size() < 1 + cfg_.server_cid_length) return;
    std::vector<uint8_t> dcid(dg.begin() + 1,
                              dg.begin() + 1 + static_cast<std::ptrdiff_t>(cfg_.server_cid_length));
    auto it = connections_.find(dcid);
    if (it == connections_.end()) {
        return; // unknown SCID → drop
    }
    it->second->on_datagram(dg);
    it->second->on_timer();
}

auto Listener::on_timer() -> void {
    for (auto& [scid, conn] : connections_) {
        conn->on_timer();
    }
}

auto Listener::find_connection(std::span<const uint8_t> scid) -> Connection* {
    auto it = connections_.find(std::vector<uint8_t>(scid.begin(), scid.end()));
    return it == connections_.end() ? nullptr : it->second.get();
}

} // namespace quic
} // namespace spaznet
