#include <libspaznet/quic/listener.hpp>
#include <libspaznet/detail/socket_compat.hpp>

#include <algorithm>
#include <cstring>
#include <random>
#include <stdexcept>

#include <libspaznet/quic/crypto.hpp>
#include <libspaznet/quic/packet.hpp>
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

namespace {

// Serialize the peer's network-level address (IP + port) into a compact
// byte string.  IPv4: 4 bytes addr + 2 bytes port = 6.  IPv6: 16 bytes
// addr + 2 bytes port = 18.  Anything else returns empty (token
// minting / verification will fail).
auto encode_peer_addr(const PeerAddr& peer, std::vector<uint8_t>& out) -> bool {
    out.clear();
    if (peer.length >= sizeof(sockaddr_in) && peer.storage.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&peer.storage);
        const auto* ip = reinterpret_cast<const uint8_t*>(&a->sin_addr);
        out.insert(out.end(), ip, ip + 4);
        const uint16_t port = a->sin_port; // network order — opaque to us
        out.push_back(static_cast<uint8_t>(port & 0xFFU));
        out.push_back(static_cast<uint8_t>((port >> 8) & 0xFFU));
        return true;
    }
    if (peer.length >= sizeof(sockaddr_in6) && peer.storage.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&peer.storage);
        const auto* ip = reinterpret_cast<const uint8_t*>(&a6->sin6_addr);
        out.insert(out.end(), ip, ip + 16);
        const uint16_t port = a6->sin6_port;
        out.push_back(static_cast<uint8_t>(port & 0xFFU));
        out.push_back(static_cast<uint8_t>((port >> 8) & 0xFFU));
        return true;
    }
    return false;
}

} // namespace

auto Listener::make_retry_token(std::span<const uint8_t> odcid, const PeerAddr& peer)
    -> std::vector<uint8_t> {
    std::vector<uint8_t> token;
    // 16-byte nonce so repeated tokens for the same peer differ.
    std::array<uint8_t, 16> nonce{};
    for (auto& b : nonce) {
        prng_state_ ^= prng_state_ << 13;
        prng_state_ ^= prng_state_ >> 7;
        prng_state_ ^= prng_state_ << 17;
        b = static_cast<uint8_t>(prng_state_ & 0xFFU);
    }
    token.insert(token.end(), nonce.begin(), nonce.end());
    // Peer address.
    std::vector<uint8_t> addr_bytes;
    if (!encode_peer_addr(peer, addr_bytes)) {
        return {};
    }
    token.push_back(static_cast<uint8_t>(addr_bytes.size()));
    token.insert(token.end(), addr_bytes.begin(), addr_bytes.end());
    // ODCID.
    token.push_back(static_cast<uint8_t>(odcid.size()));
    token.insert(token.end(), odcid.begin(), odcid.end());
    // MAC over everything so far.
    auto mac = hkdf_extract(Hash::Sha256, {token_key_.data(), token_key_.size()},
                            {token.data(), token.size()});
    token.insert(token.end(), mac.begin(), mac.begin() + 16);
    return token;
}

auto Listener::validate_retry_token(std::span<const uint8_t> token, const PeerAddr& peer,
                                    std::vector<uint8_t>& odcid_out) -> bool {
    odcid_out.clear();
    constexpr std::size_t kNonceLen = 16;
    constexpr std::size_t kMacLen = 16;
    if (token.size() < kNonceLen + 1 + kMacLen) return false;
    std::size_t off = kNonceLen;
    const std::size_t addr_len = token[off++];
    if (off + addr_len + 1 > token.size()) return false;
    std::span<const uint8_t> token_addr{token.data() + off, addr_len};
    off += addr_len;
    const std::size_t odcid_len = token[off++];
    if (odcid_len > kMaxConnectionIdLen) return false;
    if (off + odcid_len + kMacLen != token.size()) return false;
    std::span<const uint8_t> token_odcid{token.data() + off, odcid_len};
    off += odcid_len;
    std::span<const uint8_t> token_mac{token.data() + off, kMacLen};
    // Recompute MAC and constant-time compare.
    auto mac = hkdf_extract(Hash::Sha256, {token_key_.data(), token_key_.size()},
                            {token.data(), kNonceLen + 1 + addr_len + 1 + odcid_len});
    uint8_t diff = 0;
    for (std::size_t i = 0; i < kMacLen; ++i) {
        diff |= static_cast<uint8_t>(mac[i] ^ token_mac[i]);
    }
    if (diff != 0) return false;
    // Compare embedded address against current peer.
    std::vector<uint8_t> current_addr;
    if (!encode_peer_addr(peer, current_addr)) return false;
    if (current_addr.size() != addr_len) return false;
    if (std::memcmp(current_addr.data(), token_addr.data(), addr_len) != 0) return false;
    odcid_out.assign(token_odcid.begin(), token_odcid.end());
    return true;
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

auto Listener::on_datagram(const PeerAddr& peer, std::span<const uint8_t> dg) -> void {
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
            send_fn_(peer, {vn.data(), vn.size()});
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

            // For both Retry validation and OD-CID transport param
            // pre-fill we need the Initial's full parse (peek_long only
            // returns version/dcid/scid).  Run it now.
            LongHeader full{};
            std::size_t cur = 0;
            if (!parse_long_header(dg, cur, full) || full.type != LongType::Initial) {
                return;
            }

            // Validated-via-Retry status + the OD-CID we should report
            // in our transport parameters.  Without Retry, OD-CID is
            // just the DCID in the current Initial.  With Retry, it
            // comes out of the token we previously minted.
            bool validated_by_token = false;
            std::vector<uint8_t> od_cid_for_tp(dcid);
            std::vector<uint8_t> retry_scid_for_tp; // empty unless Retry

            if (cfg_.require_retry) {
                if (full.token.empty()) {
                    // No token — send a fresh Retry and drop.  Pick a
                    // new SCID; encode the client's original DCID in
                    // the token bound to its source address.
                    std::vector<uint8_t> retry_scid;
                    new_random_cid(retry_scid);
                    auto token = make_retry_token(
                        {dcid.data(), dcid.size()}, peer);
                    if (token.empty()) return;
                    auto retry = build_retry_packet(
                        {scid.data(), scid.size()},
                        {retry_scid.data(), retry_scid.size()},
                        {token.data(), token.size()},
                        {dcid.data(), dcid.size()});
                    send_fn_(peer, {retry.data(), retry.size()});
                    return;
                }
                // Token present — validate.  Reject on any failure
                // (mismatched MAC or peer address).  We could re-send
                // Retry, but dropping is RFC-conformant and avoids an
                // infinite Retry loop against a confused or malicious
                // client.
                std::vector<uint8_t> decoded_odcid;
                if (!validate_retry_token({full.token.data(), full.token.size()},
                                          peer, decoded_odcid)) {
                    return;
                }
                validated_by_token = true;
                od_cid_for_tp = std::move(decoded_odcid);
                // The DCID in this post-Retry Initial is the Retry SCID
                // we issued earlier; we'll reuse it as our SCID for
                // this Connection (so the peer's view of our CID stays
                // stable) and report it in retry_source_connection_id.
                retry_scid_for_tp = dcid;
            }

            // Pick a server SCID.  Under Retry we reuse the DCID from
            // the post-Retry Initial (= the Retry-SCID the client
            // already knows).  Without Retry we generate a fresh one.
            std::vector<uint8_t> server_scid;
            if (validated_by_token) {
                server_scid = dcid;
            } else {
                new_random_cid(server_scid);
            }

            // Pre-fill transport parameters so finalize_tp in the
            // Connection ctor doesn't overwrite our OD-CID (Retry case)
            // and so retry_source_connection_id is emitted when needed.
            TransportParameters tp = cfg_.server_tp;
            tp.original_destination_connection_id = od_cid_for_tp;
            if (!retry_scid_for_tp.empty()) {
                tp.retry_source_connection_id = std::move(retry_scid_for_tp);
            }

            ConnState state;
            state.last_peer = peer;
            auto* self = this;
            auto scid_copy = server_scid;
            auto send_fn_for_conn =
                [self, scid_copy](std::span<const uint8_t> bytes) {
                    auto it2 = self->connections_.find(scid_copy);
                    if (it2 == self->connections_.end()) return;
                    self->send_fn_(it2->second.last_peer, bytes);
                };
            state.conn = std::make_unique<Connection>(
                cfg_.tls_ctx, std::span<const uint8_t>{dcid.data(), dcid.size()},
                std::span<const uint8_t>{scid.data(), scid.size()},
                std::span<const uint8_t>{server_scid.data(), server_scid.size()},
                std::move(tp), send_fn_for_conn);
            auto* raw = state.conn.get();
            if (validated_by_token) {
                raw->mark_peer_address_validated();
            } else {
                // Without Retry, the Connection's anti-amp budget gates
                // outbound bytes until the first Handshake packet
                // decrypts — see Connection::build_and_send.
            }
            connections_aliases_[dcid] = server_scid;
            connections_.emplace(server_scid, std::move(state));
            raw->on_datagram(dg);
            raw->on_timer();
            return;
        }
        // RFC 9000 §9 (connection migration) — limited support: we
        // don't run PATH_CHALLENGE / PATH_RESPONSE probes on candidate
        // paths.  An attacker who can inject a forged datagram from a
        // spoofed source could otherwise redirect our response traffic
        // to that address.  We close the hole by freezing the routing
        // address once the connection's handshake completes: from then
        // on, all replies keep flowing to the path the client used to
        // finish the handshake, regardless of where subsequent
        // datagrams *appear* to come from.  Pre-handshake, the path is
        // still permitted to drift because legitimate NAT rebinding
        // during connection setup is unblockable; anti-amplification
        // (§8.1.2) limits the damage.
        if (!it->second.conn->handshake_complete()) {
            it->second.last_peer = peer;
        }
        it->second.conn->on_datagram(dg);
        it->second.conn->on_timer();
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
    // Short-header packets only fly post-handshake, so the
    // freeze-after-handshake rule above applies in full here: never
    // update last_peer on the strength of a short-header datagram
    // alone.  A peer that's legitimately migrated would need
    // PATH_CHALLENGE / PATH_RESPONSE support, which we don't
    // implement.
    if (!it->second.conn->handshake_complete()) {
        it->second.last_peer = peer;
    }
    it->second.conn->on_datagram(dg);
    it->second.conn->on_timer();
}

auto Listener::on_timer() -> void {
    for (auto& [scid, state] : connections_) {
        state.conn->on_timer();
    }
}

auto Listener::find_connection(std::span<const uint8_t> scid) -> Connection* {
    auto it = connections_.find(std::vector<uint8_t>(scid.begin(), scid.end()));
    return it == connections_.end() ? nullptr : it->second.conn.get();
}

auto Listener::peer_for(std::span<const uint8_t> scid) -> const PeerAddr* {
    auto it = connections_.find(std::vector<uint8_t>(scid.begin(), scid.end()));
    return it == connections_.end() ? nullptr : &it->second.last_peer;
}

} // namespace quic
} // namespace spaznet
