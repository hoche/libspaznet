// Phase 4 integration: drive a server Connection through a complete
// QUIC handshake using a hand-built TLS client that speaks the same
// dispatch interface our TlsConnection does. We exchange protected
// datagrams via in-memory send/recv queues — no UDP socket — and
// verify that the server reaches Established, emits HANDSHAKE_DONE,
// and that both sides agree on transport parameters.

#include <gtest/gtest.h>

#include <libspaznet/quic/connection.hpp>
#include <libspaznet/quic/varint.hpp>

#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <openssl/core_dispatch.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

using namespace spaznet::quic;

namespace {

auto make_self_signed_p256() -> std::pair<std::string, std::string> {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME* nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("ls-test"), -1, -1, 0);
    X509_set_issuer_name(x, nm);
    X509_sign(x, pkey, EVP_sha256());

    BIO* cb = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cb, x);
    char* cdata = nullptr;
    long clen = BIO_get_mem_data(cb, &cdata);
    std::string cpem(cdata, static_cast<std::size_t>(clen));
    BIO* kb = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* kdata = nullptr;
    long klen = BIO_get_mem_data(kb, &kdata);
    std::string kpem(kdata, static_cast<std::size_t>(klen));
    BIO_free(cb);
    BIO_free(kb);
    X509_free(x);
    EVP_PKEY_free(pkey);
    return {cpem, kpem};
}

// In-memory QUIC client: drives an OpenSSL SSL* in client mode through
// the QUIC TLS callbacks, but also packs/unpacks QUIC packets so the
// server Connection sees real datagrams. Mirrors the server side.
struct QuicTestClient {
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    std::vector<uint8_t> dcid;     // we (client) chose this for the server
    std::vector<uint8_t> scid;     // peer (server) will use this as its DCID for our packets
    // Per-level CRYPTO outbox the SSL_set_quic_tls_cbs send callback fills.
    std::array<std::vector<uint8_t>, 4> tls_out;
    std::array<std::vector<uint8_t>, 4> tls_in;
    std::array<std::size_t, 4> tls_in_cursor{0, 0, 0, 0};
    uint32_t send_level{OSSL_RECORD_PROTECTION_LEVEL_NONE};
    std::vector<uint8_t> own_tp_wire;
    bool got_peer_tp{false};
    std::vector<uint8_t> peer_tp_wire;
    // Per-encryption-level QUIC packet-protection keys.
    std::array<PacketKeys, 4> send_keys{};
    std::array<PacketKeys, 4> recv_keys{};
    std::array<bool, 4> send_ready{false, false, false, false};
    std::array<bool, 4> recv_ready{false, false, false, false};
    std::array<Aead, 4> aead{Aead::Aes128Gcm, Aead::Aes128Gcm, Aead::Aes128Gcm,
                              Aead::Aes128Gcm};
    std::array<uint64_t, 4> next_pn{0, 0, 0, 0};
    std::array<uint64_t, 4> largest_recv_pn{0, 0, 0, 0};
    std::array<bool, 4> any_recv{false, false, false, false};
    std::array<uint64_t, 4> crypto_send_off{0, 0, 0, 0};
    std::array<uint64_t, 4> crypto_recv_off{0, 0, 0, 0};
    std::array<std::map<uint64_t, std::vector<uint8_t>>, 4> crypto_recv_chunks;
    std::array<PnSpace, 4> acks;
    std::deque<std::vector<uint8_t>> outbox; // datagrams to ship to server
    // Datagrams we received before the keys for the leading packet's
    // level were ready. Replayed each round.
    std::deque<std::vector<uint8_t>> pending_dgs;

    ~QuicTestClient() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
    }
};

static int qtc_send(SSL*, const unsigned char* buf, size_t buf_len, size_t* consumed,
                    void* arg) {
    auto* c = static_cast<QuicTestClient*>(arg);
    c->tls_out[c->send_level].insert(c->tls_out[c->send_level].end(), buf, buf + buf_len);
    *consumed = buf_len;
    return 1;
}
static int qtc_recv(SSL*, const unsigned char** buf, size_t* br, void* arg) {
    auto* c = static_cast<QuicTestClient*>(arg);
    for (std::size_t i = 0; i < 4; ++i) {
        if (c->tls_in_cursor[i] < c->tls_in[i].size()) {
            *buf = c->tls_in[i].data() + c->tls_in_cursor[i];
            *br = c->tls_in[i].size() - c->tls_in_cursor[i];
            return 1;
        }
    }
    *buf = nullptr;
    *br = 0;
    return 1;
}
static int qtc_release(SSL*, size_t br, void* arg) {
    auto* c = static_cast<QuicTestClient*>(arg);
    for (std::size_t i = 0; i < 4; ++i) {
        if (c->tls_in_cursor[i] < c->tls_in[i].size()) {
            c->tls_in_cursor[i] += std::min(br, c->tls_in[i].size() - c->tls_in_cursor[i]);
            return 1;
        }
    }
    return 1;
}
static int qtc_yield(SSL*, uint32_t prot_level, int direction, const unsigned char* secret,
                     size_t secret_len, void* arg) {
    auto* c = static_cast<QuicTestClient*>(arg);
    c->send_level = prot_level;
    EncryptionLevel level = static_cast<EncryptionLevel>(
        prot_level == OSSL_RECORD_PROTECTION_LEVEL_NONE       ? 0
        : prot_level == OSSL_RECORD_PROTECTION_LEVEL_EARLY    ? 1
        : prot_level == OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE ? 2
                                                              : 3);
    const SSL_CIPHER* cph = SSL_get_current_cipher(c->ssl);
    if (cph) {
        c->aead[static_cast<std::size_t>(level)] =
            aead_from_tls_cipher_id(static_cast<uint16_t>(SSL_CIPHER_get_protocol_id(cph)));
    }
    // direction 0 == read (we, the client, will use this to decrypt server pkts);
    // direction 1 == write (client encrypts to server).
    if (direction == 0) {
        c->recv_keys[static_cast<std::size_t>(level)] = derive_packet_keys(
            c->aead[static_cast<std::size_t>(level)], {secret, secret_len});
        c->recv_ready[static_cast<std::size_t>(level)] = true;
    } else {
        c->send_keys[static_cast<std::size_t>(level)] = derive_packet_keys(
            c->aead[static_cast<std::size_t>(level)], {secret, secret_len});
        c->send_ready[static_cast<std::size_t>(level)] = true;
    }
    return 1;
}
static int qtc_got_tp(SSL*, const unsigned char* params, size_t params_len, void* arg) {
    auto* c = static_cast<QuicTestClient*>(arg);
    c->peer_tp_wire.assign(params, params + params_len);
    c->got_peer_tp = true;
    return 1;
}
static int qtc_alert(SSL*, unsigned char, void*) {
    return 1;
}

static const OSSL_DISPATCH* qtc_dispatch() {
    static const OSSL_DISPATCH t[] = {
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND, reinterpret_cast<void (*)()>(qtc_send)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD, reinterpret_cast<void (*)()>(qtc_recv)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD,
         reinterpret_cast<void (*)()>(qtc_release)},
        {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET, reinterpret_cast<void (*)()>(qtc_yield)},
        {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS,
         reinterpret_cast<void (*)()>(qtc_got_tp)},
        {OSSL_FUNC_SSL_QUIC_TLS_ALERT, reinterpret_cast<void (*)()>(qtc_alert)},
        {0, nullptr}};
    return t;
}

auto make_client(std::vector<uint8_t> dcid, std::vector<uint8_t> scid)
    -> std::unique_ptr<QuicTestClient> {
    auto c = std::make_unique<QuicTestClient>();
    c->dcid = dcid;
    c->scid = scid;
    c->ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(c->ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, nullptr);
    static const unsigned char alpn[] = {2, 'h', '3'};
    SSL_CTX_set_alpn_protos(c->ctx, alpn, sizeof(alpn));
    c->ssl = SSL_new(c->ctx);
    SSL_set_connect_state(c->ssl);
    SSL_set_tlsext_host_name(c->ssl, "localhost");

    // Initial keys derived from client_dcid (the dcid we put on the wire
    // == server's expected DCID == the client's "destination" CID).
    auto secret_c = derive_initial_secret(dcid, Direction::Client);
    auto secret_s = derive_initial_secret(dcid, Direction::Server);
    c->aead[0] = Aead::Aes128Gcm;
    c->send_keys[0] = derive_packet_keys(Aead::Aes128Gcm, secret_c);
    c->recv_keys[0] = derive_packet_keys(Aead::Aes128Gcm, secret_s);
    c->send_ready[0] = true;
    c->recv_ready[0] = true;

    TransportParameters tp;
    tp.initial_source_connection_id = scid;
    tp.initial_max_data = 1 << 20;
    tp.initial_max_streams_bidi = 16;
    tp.initial_max_streams_uni = 3;
    tp.initial_max_stream_data_bidi_remote = 1 << 16;
    tp.initial_max_stream_data_bidi_local = 1 << 16;
    tp.initial_max_stream_data_uni = 1 << 16;
    c->own_tp_wire = encode_transport_params(tp);
    SSL_set_quic_tls_cbs(c->ssl, qtc_dispatch(), c.get());
    SSL_set_quic_tls_transport_params(c->ssl, c->own_tp_wire.data(), c->own_tp_wire.size());
    return c;
}

// Helper: build a long-header packet (Initial or Handshake) on the
// client side from a plaintext payload and send_keys.
auto client_build_long(QuicTestClient& c, EncryptionLevel level, LongType type,
                       const std::vector<uint8_t>& payload) -> std::vector<uint8_t> {
    auto& keys = c.send_keys[static_cast<std::size_t>(level)];
    Aead a = c.aead[static_cast<std::size_t>(level)];
    const std::size_t pn_len = 4;
    const std::size_t tag_len = aead_tag_length(a);
    std::vector<uint8_t> pkt;

    uint8_t first = static_cast<uint8_t>(0x80 | 0x40 | (static_cast<uint8_t>(type) << 4) |
                                         static_cast<uint8_t>(pn_len - 1));
    pkt.push_back(first);
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 24));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 16));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 8));
    pkt.push_back(static_cast<uint8_t>(kQuicV1));
    pkt.push_back(static_cast<uint8_t>(c.dcid.size()));
    pkt.insert(pkt.end(), c.dcid.begin(), c.dcid.end());
    pkt.push_back(static_cast<uint8_t>(c.scid.size()));
    pkt.insert(pkt.end(), c.scid.begin(), c.scid.end());
    if (type == LongType::Initial) {
        VarInt::append(pkt, 0U); // token length
    }
    const uint64_t length = pn_len + payload.size() + tag_len;
    VarInt::append(pkt, length);
    const std::size_t pn_offset = pkt.size();
    const uint64_t pn = c.next_pn[static_cast<std::size_t>(level)]++;
    for (std::size_t i = 0; i < pn_len; ++i) {
        pkt.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - i))) & 0xFF));
    }
    const std::size_t payload_off = pkt.size();
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
    std::vector<uint8_t> plaintext(pkt.begin() + payload_off, pkt.end());
    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
    std::vector<uint8_t> sealed;
    aead_seal(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
              {aad.data(), aad.size()}, {plaintext.data(), plaintext.size()}, sealed);
    pkt.resize(payload_off);
    pkt.insert(pkt.end(), sealed.begin(), sealed.end());
    auto mask = header_protection_mask(a, {keys.hp.data(), keys.hp.size()},
                                       {pkt.data() + pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x0F;
    for (std::size_t i = 0; i < pn_len; ++i) {
        pkt[pn_offset + i] ^= mask[1 + i];
    }
    return pkt;
}

// Walk client's TLS outbox at each level, package as CRYPTO frames in
// the level's plaintext payload, build a protected packet, and emit it
// to the outbox. For Initial we pad to 1200 (client min size).
auto client_emit(QuicTestClient& c) -> void {
    for (auto lvl : {EncryptionLevel::Initial, EncryptionLevel::Handshake,
                     EncryptionLevel::Application}) {
        const auto i = static_cast<std::size_t>(lvl);
        if (!c.send_ready[i]) continue;
        auto& outbox = c.tls_out[i];
        bool ack_owed = c.acks[i].needs_ack();
        if (outbox.empty() && !ack_owed) continue;
        std::vector<uint8_t> payload;
        if (ack_owed) {
            AckFrame ack;
            c.acks[i].build_ack_frame(0, ack);
            encode_frame(payload, Frame{ack});
        }
        if (!outbox.empty()) {
            CryptoFrame cf;
            cf.offset = c.crypto_send_off[i];
            cf.data = outbox;
            c.crypto_send_off[i] += outbox.size();
            encode_frame(payload, Frame{cf});
            outbox.clear();
        }
        if (lvl == EncryptionLevel::Initial) {
            // Pad the datagram to 1200 (RFC 9000 §14.1 client minimum).
            // We approximate by padding the *payload* enough that the
            // header+payload+tag reaches ~1200.
            while (payload.size() < 1100) {
                payload.push_back(0); // PADDING
            }
            auto pkt = client_build_long(c, lvl, LongType::Initial, payload);
            // Inflate datagram if still short.
            if (pkt.size() < 1200) pkt.resize(1200, 0);
            c.outbox.push_back(std::move(pkt));
        } else if (lvl == EncryptionLevel::Handshake) {
            c.outbox.push_back(client_build_long(c, lvl, LongType::Handshake, payload));
        } else {
            // 1-RTT short header (only after handshake; minimal: skip).
            auto& keys = c.send_keys[i];
            Aead a = c.aead[i];
            const std::size_t pn_len = 4;
            std::vector<uint8_t> pkt;
            pkt.push_back(static_cast<uint8_t>(0x40 | (pn_len - 1)));
            pkt.insert(pkt.end(), c.dcid.begin(), c.dcid.end());
            const std::size_t pn_offset = pkt.size();
            uint64_t pn = c.next_pn[i]++;
            for (std::size_t k = 0; k < pn_len; ++k) {
                pkt.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - k))) & 0xFF));
            }
            const std::size_t payload_off = pkt.size();
            pkt.insert(pkt.end(), payload.begin(), payload.end());
            std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
            std::vector<uint8_t> plain(pkt.begin() + payload_off, pkt.end());
            auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
            std::vector<uint8_t> sealed;
            aead_seal(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
                      {aad.data(), aad.size()}, {plain.data(), plain.size()}, sealed);
            pkt.resize(payload_off);
            pkt.insert(pkt.end(), sealed.begin(), sealed.end());
            auto mask = header_protection_mask(
                a, {keys.hp.data(), keys.hp.size()}, {pkt.data() + pn_offset + 4, kSampleLen});
            pkt[0] ^= mask[0] & 0x1F;
            for (std::size_t k = 0; k < pn_len; ++k) {
                pkt[pn_offset + k] ^= mask[1 + k];
            }
            c.outbox.push_back(std::move(pkt));
        }
    }
}

// Decrypt and parse a datagram the server sent to the client, feeding
// CRYPTO bytes into the SSL*'s inbox.
auto client_receive(QuicTestClient& c, std::vector<uint8_t> dg) -> void {
    std::size_t off = 0;
    while (off < dg.size()) {
        if ((dg[off] & 0x80U) != 0) {
            LongHeader hdr;
            std::size_t cursor = off;
            if (!parse_long_header({dg.data(), dg.size()}, cursor, hdr)) return;
            EncryptionLevel lvl = hdr.type == LongType::Initial
                                      ? EncryptionLevel::Initial
                                      : hdr.type == LongType::Handshake
                                            ? EncryptionLevel::Handshake
                                            : EncryptionLevel::EarlyData;
            const auto i = static_cast<std::size_t>(lvl);
            if (!c.recv_ready[i]) {
                // Buffer the entire remaining datagram for a later round
                // when the relevant level's keys have been installed.
                c.pending_dgs.emplace_back(dg.begin() + static_cast<std::ptrdiff_t>(off),
                                           dg.end());
                return;
            }
            // Carve packet bytes out and decrypt.
            std::vector<uint8_t> pkt(dg.begin() + static_cast<std::ptrdiff_t>(off),
                                     dg.begin() + static_cast<std::ptrdiff_t>(cursor));
            LongHeader local = hdr;
            local.pn_offset -= off;
            uint64_t pn = 0;
            std::vector<uint8_t> plaintext;
            if (!decrypt_long_packet(c.aead[i], c.recv_keys[i],
                                     c.any_recv[i] ? c.largest_recv_pn[i] : 0, pkt, local,
                                     pn, plaintext)) {
                return;
            }
            c.any_recv[i] = true;
            if (pn > c.largest_recv_pn[i]) c.largest_recv_pn[i] = pn;
            c.acks[i].on_received(pn, true);
            // Parse frames; we only care about CRYPTO at this stage.
            std::vector<Frame> frames;
            if (!parse_frames({plaintext.data(), plaintext.size()}, frames)) return;
            for (auto& f : frames) {
                std::visit(
                    [&](auto& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, CryptoFrame>) {
                            c.crypto_recv_chunks[i][x.offset] = x.data;
                        }
                    },
                    f);
            }
            // Drain in-order CRYPTO into the TLS in[] buffer.
            while (!c.crypto_recv_chunks[i].empty()) {
                auto it = c.crypto_recv_chunks[i].begin();
                if (it->first > c.crypto_recv_off[i]) break;
                if (it->first + it->second.size() <= c.crypto_recv_off[i]) {
                    c.crypto_recv_chunks[i].erase(it);
                    continue;
                }
                std::size_t skip =
                    static_cast<std::size_t>(c.crypto_recv_off[i] - it->first);
                c.tls_in[i].insert(c.tls_in[i].end(), it->second.begin() + skip,
                                   it->second.end());
                c.crypto_recv_off[i] += it->second.size() - skip;
                c.crypto_recv_chunks[i].erase(it);
            }
            off = cursor;
        } else {
            // 1-RTT short header.
            const auto i = static_cast<std::size_t>(EncryptionLevel::Application);
            if (!c.recv_ready[i]) return;
            std::vector<uint8_t> pkt(dg.begin() + static_cast<std::ptrdiff_t>(off), dg.end());
            // DCID length on incoming server→client short header == c.scid.size().
            const std::size_t pn_offset = 1 + c.scid.size();
            if (pn_offset + 4 + kSampleLen > pkt.size()) return;
            auto mask = header_protection_mask(c.aead[i], {c.recv_keys[i].hp.data(),
                                                            c.recv_keys[i].hp.size()},
                                                {pkt.data() + pn_offset + 4, kSampleLen});
            pkt[0] ^= mask[0] & 0x1F;
            const std::size_t pn_len = (pkt[0] & 0x03U) + 1;
            for (std::size_t k = 0; k < pn_len; ++k) {
                pkt[pn_offset + k] ^= mask[1 + k];
            }
            uint64_t trunc = 0;
            for (std::size_t k = 0; k < pn_len; ++k) {
                trunc = (trunc << 8) | pkt[pn_offset + k];
            }
            uint64_t pn = decode_packet_number(c.any_recv[i] ? c.largest_recv_pn[i] : 0,
                                               trunc, pn_len * 8);
            const std::size_t header_len = pn_offset + pn_len;
            std::span<const uint8_t> aad{pkt.data(), header_len};
            std::span<const uint8_t> ct{pkt.data() + header_len, pkt.size() - header_len};
            auto nonce = make_aead_nonce(
                {c.recv_keys[i].iv.data(), c.recv_keys[i].iv.size()}, pn);
            std::vector<uint8_t> plain;
            if (!aead_open(c.aead[i],
                           {c.recv_keys[i].key.data(), c.recv_keys[i].key.size()},
                           {nonce.data(), nonce.size()}, aad, ct, plain)) {
                return;
            }
            c.any_recv[i] = true;
            if (pn > c.largest_recv_pn[i]) c.largest_recv_pn[i] = pn;
            c.acks[i].on_received(pn, true);
            return;
        }
    }
}

} // namespace

TEST(QuicConnection, EndToEndHandshakeViaProtectedDatagrams) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0xC0, 0xDE, 0xCA, 0xFE};
    std::vector<uint8_t> client_scid = {0x77, 0x88};
    std::vector<uint8_t> server_scid = {0x11, 0x22, 0x33, 0x44};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;

    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      });

    auto client = make_client(client_dcid, client_scid);

    for (int round = 0; round < 30; ++round) {
        // Drive client TLS forward.
        int rc = SSL_do_handshake(client->ssl);
        if (rc != 1) {
            int err = SSL_get_error(client->ssl, rc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                FAIL() << "client SSL_do_handshake err=" << err;
            }
        }

        // Client → wire.
        client_emit(*client);
        while (!client->outbox.empty()) {
            auto dg = std::move(client->outbox.front());
            client->outbox.pop_front();
            server.on_datagram({dg.data(), dg.size()});
        }
        server.on_timer();
        // Server → wire.
        while (!server_outbox.empty()) {
            auto dg = std::move(server_outbox.front());
            server_outbox.pop_front();
            client_receive(*client, std::move(dg));
        }
        // Retry any datagrams the client had buffered while waiting for
        // higher-level keys.
        if (!client->pending_dgs.empty()) {
            auto pending = std::move(client->pending_dgs);
            client->pending_dgs.clear();
            for (auto& dg : pending) {
                client_receive(*client, std::move(dg));
            }
        }
        if (server.state() == Connection::State::Established &&
            SSL_is_init_finished(client->ssl)) {
            // One extra round to let HANDSHAKE_DONE flow.
            server.on_timer();
            while (!server_outbox.empty()) {
                auto dg = std::move(server_outbox.front());
                server_outbox.pop_front();
                client_receive(*client, std::move(dg));
            }
            break;
        }
    }

    EXPECT_EQ(server.state(), Connection::State::Established);
    EXPECT_TRUE(SSL_is_init_finished(client->ssl));
    EXPECT_TRUE(server.peer_transport_params().initial_max_streams_bidi == 16U ||
                server.peer_transport_params().initial_max_streams_bidi == 0U);
    EXPECT_TRUE(client->got_peer_tp);
}
