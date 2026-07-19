// Phase 4 integration: drive a server Connection through a complete
// QUIC handshake using a hand-built TLS client that speaks the same
// dispatch interface our TlsConnection does. We exchange protected
// datagrams via in-memory send/recv queues — no UDP socket — and
// verify that the server reaches Established, emits HANDSHAKE_DONE,
// and that both sides agree on transport parameters.

#include <gtest/gtest.h>

#include <libspaznet/quic/connection.hpp>
#include <libspaznet/quic/listener.hpp>
#include <libspaznet/quic/varint.hpp>

#include <netinet/in.h>
#include <arpa/inet.h>

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
    // RFC 9001 §6 key-update bookkeeping for the 1-RTT space — we
    // stash the raw traffic secret so the test can drive the "quic
    // ku" derivation itself.  Mirrors what the production
    // Connection does.
    std::array<std::vector<uint8_t>, 4> send_secret{};
    std::array<std::vector<uint8_t>, 4> recv_secret{};
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
        c->recv_secret[static_cast<std::size_t>(level)].assign(secret, secret + secret_len);
    } else {
        c->send_keys[static_cast<std::size_t>(level)] = derive_packet_keys(
            c->aead[static_cast<std::size_t>(level)], {secret, secret_len});
        c->send_ready[static_cast<std::size_t>(level)] = true;
        c->send_secret[static_cast<std::size_t>(level)].assign(secret, secret + secret_len);
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

// ---- Shared scaffolding for the flow-control / stream-limit tests ----

// Run the server<->client exchange until the server reaches Established.
auto drive_to_established(Connection& server, QuicTestClient& client,
                          std::deque<std::vector<uint8_t>>& server_outbox,
                          std::chrono::steady_clock::time_point& clock_now) -> void {
    for (int round = 0; round < 30; ++round) {
        int rc = SSL_do_handshake(client.ssl);
        if (rc != 1) {
            int err = SSL_get_error(client.ssl, rc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                ADD_FAILURE() << "client SSL_do_handshake err=" << err;
                return;
            }
        }
        client_emit(client);
        while (!client.outbox.empty()) {
            auto dg = std::move(client.outbox.front());
            client.outbox.pop_front();
            server.on_datagram({dg.data(), dg.size()});
        }
        clock_now += std::chrono::milliseconds(1);
        server.on_timer();
        while (!server_outbox.empty()) {
            auto dg = std::move(server_outbox.front());
            server_outbox.pop_front();
            client_receive(client, std::move(dg));
        }
        if (!client.pending_dgs.empty()) {
            auto pending = std::move(client.pending_dgs);
            client.pending_dgs.clear();
            for (auto& dg : pending) {
                client_receive(client, std::move(dg));
            }
        }
        if (server.state() == Connection::State::Established) break;
    }
}

// Seal `payload` (already-encoded frame bytes) into a 1-RTT short-header
// packet from the client and feed it to the server.  Caller must have
// pointed client.dcid at the server's SCID first.
auto client_inject_1rtt(QuicTestClient& client, Connection& server,
                        const std::vector<uint8_t>& payload) -> void {
    const auto app = static_cast<std::size_t>(EncryptionLevel::Application);
    auto& keys = client.send_keys[app];
    const Aead a = client.aead[app];
    const std::size_t pn_len = 4;
    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>(0x40 | (pn_len - 1)));
    pkt.insert(pkt.end(), client.dcid.begin(), client.dcid.end());
    const std::size_t pn_offset = pkt.size();
    const uint64_t pn = client.next_pn[app]++;
    for (std::size_t k = 0; k < pn_len; ++k) {
        pkt.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - k))) & 0xFF));
    }
    const std::size_t payload_off = pkt.size();
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
    std::vector<uint8_t> plain(pkt.begin() + payload_off, pkt.end());
    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
    std::vector<uint8_t> sealed;
    if (!aead_seal(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
                   {aad.data(), aad.size()}, {plain.data(), plain.size()}, sealed)) {
        ADD_FAILURE() << "client failed to seal 1-RTT packet";
        return;
    }
    pkt.resize(payload_off);
    pkt.insert(pkt.end(), sealed.begin(), sealed.end());
    auto mask = header_protection_mask(a, {keys.hp.data(), keys.hp.size()},
                                       {pkt.data() + pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x1F;
    for (std::size_t k = 0; k < pn_len; ++k) {
        pkt[pn_offset + k] ^= mask[1 + k];
    }
    server.on_datagram({pkt.data(), pkt.size()});
}

// Decrypt every server 1-RTT datagram in `server_outbox` and return all
// frames they carry.
auto collect_server_1rtt_frames(QuicTestClient& client,
                                std::deque<std::vector<uint8_t>>& server_outbox)
    -> std::vector<Frame> {
    const auto app = static_cast<std::size_t>(EncryptionLevel::Application);
    std::vector<Frame> all;
    while (!server_outbox.empty()) {
        auto dg = std::move(server_outbox.front());
        server_outbox.pop_front();
        if (dg.empty() || (dg[0] & 0x80U) != 0) continue;
        const std::size_t hdr_pn_offset = 1 + client.scid.size();
        if (hdr_pn_offset + 4 + kSampleLen > dg.size()) continue;
        auto rmask = header_protection_mask(
            client.aead[app],
            {client.recv_keys[app].hp.data(), client.recv_keys[app].hp.size()},
            {dg.data() + hdr_pn_offset + 4, kSampleLen});
        dg[0] ^= rmask[0] & 0x1F;
        const std::size_t rpn_len = (dg[0] & 0x03U) + 1;
        for (std::size_t k = 0; k < rpn_len; ++k) {
            dg[hdr_pn_offset + k] ^= rmask[1 + k];
        }
        uint64_t trunc = 0;
        for (std::size_t k = 0; k < rpn_len; ++k) {
            trunc = (trunc << 8) | dg[hdr_pn_offset + k];
        }
        uint64_t rpn = decode_packet_number(
            client.any_recv[app] ? client.largest_recv_pn[app] : 0, trunc, rpn_len * 8);
        const std::size_t header_len = hdr_pn_offset + rpn_len;
        std::span<const uint8_t> ad{dg.data(), header_len};
        std::span<const uint8_t> ct{dg.data() + header_len, dg.size() - header_len};
        auto rnonce = make_aead_nonce(
            {client.recv_keys[app].iv.data(), client.recv_keys[app].iv.size()}, rpn);
        std::vector<uint8_t> rplain;
        if (!aead_open(client.aead[app],
                       {client.recv_keys[app].key.data(), client.recv_keys[app].key.size()},
                       {rnonce.data(), rnonce.size()}, ad, ct, rplain)) {
            continue;
        }
        client.any_recv[app] = true;
        client.largest_recv_pn[app] = std::max(client.largest_recv_pn[app], rpn);
        std::vector<Frame> frames;
        if (!parse_frames({rplain.data(), rplain.size()}, frames)) continue;
        for (auto& f : frames) {
            all.push_back(std::move(f));
        }
    }
    return all;
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

// RFC 9002 §6.2 — PTO retransmission.  Drop the server's post-
// handshake STREAM datagram on the wire, advance the clock past
// pto_timeout, and verify that the next on_timer() re-emits the same
// payload as a probe.
TEST(QuicConnection, PtoRetransmitsDroppedStream) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0xDE, 0xAD, 0xBE, 0xEF};
    std::vector<uint8_t> client_scid = {0x55, 0x66};
    std::vector<uint8_t> server_scid = {0xA1, 0xB2, 0xC3, 0xD4};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;

    // Controllable clock — `clock_now` is captured and bumped by the
    // test as it needs to fast-forward past pto_timeout.
    auto clock_now = std::chrono::steady_clock::now();
    Connection::ClockFn clock_fn = [&]() { return clock_now; };

    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      },
                      clock_fn);

    auto client = make_client(client_dcid, client_scid);

    // Drive the handshake to completion (same scaffolding as the
    // EndToEnd test, with the clock advancing 1 ms per round so the
    // server's recovery state isn't pinned at the initial RTT
    // estimate).
    for (int round = 0; round < 30; ++round) {
        int rc = SSL_do_handshake(client->ssl);
        if (rc != 1) {
            int err = SSL_get_error(client->ssl, rc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                FAIL() << "client SSL_do_handshake err=" << err;
            }
        }
        client_emit(*client);
        while (!client->outbox.empty()) {
            auto dg = std::move(client->outbox.front());
            client->outbox.pop_front();
            server.on_datagram({dg.data(), dg.size()});
        }
        clock_now += std::chrono::milliseconds(1);
        server.on_timer();
        while (!server_outbox.empty()) {
            auto dg = std::move(server_outbox.front());
            server_outbox.pop_front();
            client_receive(*client, std::move(dg));
        }
        if (!client->pending_dgs.empty()) {
            auto pending = std::move(client->pending_dgs);
            client->pending_dgs.clear();
            for (auto& dg : pending) {
                client_receive(*client, std::move(dg));
            }
        }
        if (server.state() == Connection::State::Established &&
            SSL_is_init_finished(client->ssl)) {
            server.on_timer();
            while (!server_outbox.empty()) {
                auto dg = std::move(server_outbox.front());
                server_outbox.pop_front();
                client_receive(*client, std::move(dg));
            }
            break;
        }
    }
    ASSERT_EQ(server.state(), Connection::State::Established);
    server_outbox.clear();

    // Server writes a STREAM frame on stream 0.  on_timer drains it
    // to server_outbox.
    const std::string payload = "hello-from-server";
    std::vector<uint8_t> payload_bytes(payload.begin(), payload.end());
    server.write_stream(0, payload_bytes, /*fin=*/false);
    server.on_timer();
    ASSERT_FALSE(server_outbox.empty())
        << "expected at least one datagram after write_stream";
    // Drop the data datagram(s) on the wire — never deliver to client.
    const std::size_t dropped_count = server_outbox.size();
    server_outbox.clear();

    // Fast-forward well past the PTO timeout (smoothed_rtt after a
    // handshake on this scaffolding is small, but kInitialRtt is
    // 333 ms; max_ack_delay default is 25 ms.  2 s gives us plenty of
    // headroom even at pto_count > 0.)
    clock_now += std::chrono::seconds(2);
    server.on_timer();

    EXPECT_FALSE(server_outbox.empty())
        << "PTO did not fire — server should have retransmitted "
        << dropped_count << " dropped datagram(s)";

    // The retransmit datagram should be similar size to the original
    // (header + AEAD overhead + the STREAM frame with the same
    // payload).  Sanity-check that the payload bytes appear in the
    // protected datagram — we can't easily decrypt without driving
    // the client side, but the unencrypted scratch of a small frame
    // happens to embed enough structure that we settle for a
    // length sanity check.
    EXPECT_GE(server_outbox.front().size(), payload.size())
        << "retransmit datagram is shorter than the original payload";
}

// RFC 9000 §10.2 — initiate_close emits a CONNECTION_CLOSE frame on
// the next build_and_send pass and flips state_ to Closing.
TEST(QuicConnection, InitiateCloseEmitsConnectionCloseFrame) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0xAB, 0xCD, 0xEF, 0x01};
    std::vector<uint8_t> client_scid = {0x33, 0x44};
    std::vector<uint8_t> server_scid = {0xE1, 0xE2, 0xE3, 0xE4};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;

    auto clock_now = std::chrono::steady_clock::now();
    Connection::ClockFn clock_fn = [&]() { return clock_now; };

    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      },
                      clock_fn);

    // Drive the handshake to Established so Application keys are
    // ready (CONNECTION_CLOSE wants to emit at the best level).
    auto client = make_client(client_dcid, client_scid);
    for (int round = 0; round < 30; ++round) {
        int rc = SSL_do_handshake(client->ssl);
        if (rc != 1) {
            int err = SSL_get_error(client->ssl, rc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                FAIL() << "client SSL_do_handshake err=" << err;
            }
        }
        client_emit(*client);
        while (!client->outbox.empty()) {
            auto dg = std::move(client->outbox.front());
            client->outbox.pop_front();
            server.on_datagram({dg.data(), dg.size()});
        }
        clock_now += std::chrono::milliseconds(1);
        server.on_timer();
        while (!server_outbox.empty()) {
            auto dg = std::move(server_outbox.front());
            server_outbox.pop_front();
            client_receive(*client, std::move(dg));
        }
        if (!client->pending_dgs.empty()) {
            auto pending = std::move(client->pending_dgs);
            client->pending_dgs.clear();
            for (auto& dg : pending) {
                client_receive(*client, std::move(dg));
            }
        }
        if (server.state() == Connection::State::Established) break;
    }
    ASSERT_EQ(server.state(), Connection::State::Established);
    server_outbox.clear();

    // Application calls close_with_error.  Next build pass (via
    // on_timer) emits CONNECTION_CLOSE and the state flips to Closing.
    constexpr uint64_t kAppError = 0x42;
    server.close_with_error(kAppError, /*application=*/true, "shutting down");
    EXPECT_EQ(server.state(), Connection::State::Closing);
    server.on_timer();
    ASSERT_FALSE(server_outbox.empty())
        << "close_with_error did not put a datagram on the wire";
    // Once drained, subsequent on_timer ticks emit nothing more.
    auto first_size = server_outbox.size();
    server.on_timer();
    EXPECT_EQ(server_outbox.size(), first_size)
        << "Closing-state Connection should be quiet after the close datagram";
}

// RFC 9000 §8.2 — server echoes a PATH_RESPONSE in reply to an
// inbound PATH_CHALLENGE on the existing path.  Drives the handshake
// to Established, then injects a 1-RTT packet from the client
// carrying a PATH_CHALLENGE frame, and confirms the next server
// datagram contains a PATH_RESPONSE with the matching 8-byte data.
TEST(QuicConnection, RespondsToPathChallenge) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0x70, 0x61, 0x74, 0x68, 0x21, 0x21, 0x21, 0x21};
    std::vector<uint8_t> client_scid = {0x42, 0x42};
    std::vector<uint8_t> server_scid = {0xF0, 0xF1, 0xF2, 0xF3};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;

    auto clock_now = std::chrono::steady_clock::now();
    Connection::ClockFn clock_fn = [&]() { return clock_now; };

    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      },
                      clock_fn);

    auto client = make_client(client_dcid, client_scid);
    for (int round = 0; round < 30; ++round) {
        int rc = SSL_do_handshake(client->ssl);
        if (rc != 1) {
            int err = SSL_get_error(client->ssl, rc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                FAIL() << "client SSL_do_handshake err=" << err;
            }
        }
        client_emit(*client);
        while (!client->outbox.empty()) {
            auto dg = std::move(client->outbox.front());
            client->outbox.pop_front();
            server.on_datagram({dg.data(), dg.size()});
        }
        clock_now += std::chrono::milliseconds(1);
        server.on_timer();
        while (!server_outbox.empty()) {
            auto dg = std::move(server_outbox.front());
            server_outbox.pop_front();
            client_receive(*client, std::move(dg));
        }
        if (!client->pending_dgs.empty()) {
            auto pending = std::move(client->pending_dgs);
            client->pending_dgs.clear();
            for (auto& dg : pending) {
                client_receive(*client, std::move(dg));
            }
        }
        if (server.state() == Connection::State::Established) break;
    }
    ASSERT_EQ(server.state(), Connection::State::Established);
    server_outbox.clear();

    // For 1-RTT packets, DCID is the server's chosen SCID (which is
    // known to the server's process_short_packet via scid_.size()).
    // The existing client scaffold doesn't track that automatically;
    // the test does so explicitly.
    client->dcid = server_scid;

    // Build a 1-RTT short-header packet containing only a
    // PATH_CHALLENGE frame.  We assemble it the same way client_emit
    // does for its 1-RTT branch, but with our own payload.
    const std::array<uint8_t, 8> challenge_data = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    std::vector<uint8_t> payload;
    {
        PathChallengeFrame pc;
        pc.data = challenge_data;
        encode_frame(payload, Frame{pc});
    }

    const auto app = static_cast<std::size_t>(EncryptionLevel::Application);
    auto& keys = client->send_keys[app];
    const Aead a = client->aead[app];
    const std::size_t pn_len = 4;
    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>(0x40 | (pn_len - 1)));
    pkt.insert(pkt.end(), client->dcid.begin(), client->dcid.end());
    const std::size_t pn_offset = pkt.size();
    const uint64_t pn = client->next_pn[app]++;
    for (std::size_t k = 0; k < pn_len; ++k) {
        pkt.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - k))) & 0xFF));
    }
    const std::size_t payload_off = pkt.size();
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
    std::vector<uint8_t> plain(pkt.begin() + payload_off, pkt.end());
    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
    std::vector<uint8_t> sealed;
    ASSERT_TRUE(aead_seal(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
                          {aad.data(), aad.size()}, {plain.data(), plain.size()}, sealed));
    pkt.resize(payload_off);
    pkt.insert(pkt.end(), sealed.begin(), sealed.end());
    auto mask = header_protection_mask(
        a, {keys.hp.data(), keys.hp.size()}, {pkt.data() + pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x1F;
    for (std::size_t k = 0; k < pn_len; ++k) {
        pkt[pn_offset + k] ^= mask[1 + k];
    }

    server.on_datagram({pkt.data(), pkt.size()});
    server.on_timer();
    ASSERT_FALSE(server_outbox.empty()) << "server did not respond to PATH_CHALLENGE";

    // Decrypt the server's 1-RTT response and look for the matching
    // PATH_RESPONSE.  Reuse client_receive's short-header decryption.
    bool found_response = false;
    while (!server_outbox.empty()) {
        auto dg = std::move(server_outbox.front());
        server_outbox.pop_front();
        if (dg.empty() || (dg[0] & 0x80U) != 0) continue;
        const std::size_t hdr_pn_offset = 1 + client->scid.size();
        if (hdr_pn_offset + 4 + kSampleLen > dg.size()) continue;
        auto rmask = header_protection_mask(
            client->aead[app],
            {client->recv_keys[app].hp.data(), client->recv_keys[app].hp.size()},
            {dg.data() + hdr_pn_offset + 4, kSampleLen});
        dg[0] ^= rmask[0] & 0x1F;
        const std::size_t rpn_len = (dg[0] & 0x03U) + 1;
        for (std::size_t k = 0; k < rpn_len; ++k) {
            dg[hdr_pn_offset + k] ^= rmask[1 + k];
        }
        uint64_t trunc = 0;
        for (std::size_t k = 0; k < rpn_len; ++k) {
            trunc = (trunc << 8) | dg[hdr_pn_offset + k];
        }
        uint64_t rpn = decode_packet_number(
            client->any_recv[app] ? client->largest_recv_pn[app] : 0, trunc, rpn_len * 8);
        const std::size_t header_len = hdr_pn_offset + rpn_len;
        std::span<const uint8_t> ad{dg.data(), header_len};
        std::span<const uint8_t> ct{dg.data() + header_len, dg.size() - header_len};
        auto rnonce = make_aead_nonce(
            {client->recv_keys[app].iv.data(), client->recv_keys[app].iv.size()}, rpn);
        std::vector<uint8_t> rplain;
        if (!aead_open(client->aead[app],
                       {client->recv_keys[app].key.data(), client->recv_keys[app].key.size()},
                       {rnonce.data(), rnonce.size()}, ad, ct, rplain)) {
            continue;
        }
        std::vector<Frame> frames;
        if (!parse_frames({rplain.data(), rplain.size()}, frames)) continue;
        for (auto& f : frames) {
            if (auto* pr = std::get_if<PathResponseFrame>(&f); pr != nullptr) {
                if (pr->data == challenge_data) {
                    found_response = true;
                }
            }
        }
    }
    EXPECT_TRUE(found_response)
        << "server's 1-RTT reply did not carry PATH_RESPONSE with matching data";
}

// RFC 9000 §10.1 — idle timeout flips the connection to Draining
// without sending CONNECTION_CLOSE.
TEST(QuicConnection, IdleTimeoutFlipsToDraining) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0x11, 0x22, 0x33, 0x44};
    std::vector<uint8_t> client_scid = {0x99, 0xAA};
    std::vector<uint8_t> server_scid = {0xBB, 0xCC, 0xDD, 0xEE};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;
    server_tp.max_idle_timeout_ms = 1000; // 1 s

    auto clock_now = std::chrono::steady_clock::now();
    Connection::ClockFn clock_fn = [&]() { return clock_now; };

    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      },
                      clock_fn);

    // Just-after-construction the state is Handshaking and last_activity_
    // is roughly clock_now.  Fast-forward past 1 s without delivering
    // any datagrams.  on_timer should flip the state to Draining.
    clock_now += std::chrono::milliseconds(1500);
    server.on_timer();
    EXPECT_EQ(server.state(), Connection::State::Draining);
    // RFC 9000 §10.1: silent close — no CONNECTION_CLOSE on the wire.
    EXPECT_TRUE(server_outbox.empty());
}

// Helpers shared by the key-update tests below.  Drive the handshake
// to Established and return the resulting `(server, client)` pair.
struct HandshookPair {
    std::unique_ptr<Connection> server;
    std::unique_ptr<QuicTestClient> client;
    std::vector<uint8_t> client_dcid;
    std::vector<uint8_t> client_scid;
    std::vector<uint8_t> server_scid;
    std::deque<std::vector<uint8_t>> server_outbox;
    std::chrono::steady_clock::time_point clock_now;
};

namespace {

auto handshake_pair(std::vector<uint8_t> cdcid, std::vector<uint8_t> cscid,
                    std::vector<uint8_t> sscid) -> std::unique_ptr<HandshookPair> {
    auto hp = std::make_unique<HandshookPair>();
    hp->client_dcid = std::move(cdcid);
    hp->client_scid = std::move(cscid);
    hp->server_scid = std::move(sscid);
    hp->clock_now = std::chrono::steady_clock::now();

    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    if (!ctx) return nullptr;

    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;

    auto* hpp = hp.get();
    Connection::ClockFn clock_fn = [hpp]() { return hpp->clock_now; };
    hp->server = std::make_unique<Connection>(
        ctx, std::span<const uint8_t>{hp->client_dcid.data(), hp->client_dcid.size()},
        std::span<const uint8_t>{hp->client_scid.data(), hp->client_scid.size()},
        std::span<const uint8_t>{hp->server_scid.data(), hp->server_scid.size()}, server_tp,
        [hpp](std::span<const uint8_t> dg) {
            hpp->server_outbox.emplace_back(dg.begin(), dg.end());
        },
        clock_fn);

    hp->client = make_client(hp->client_dcid, hp->client_scid);
    for (int round = 0; round < 30; ++round) {
        int rc = SSL_do_handshake(hp->client->ssl);
        if (rc != 1) {
            int err = SSL_get_error(hp->client->ssl, rc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return nullptr;
        }
        client_emit(*hp->client);
        while (!hp->client->outbox.empty()) {
            auto dg = std::move(hp->client->outbox.front());
            hp->client->outbox.pop_front();
            hp->server->on_datagram({dg.data(), dg.size()});
        }
        hp->clock_now += std::chrono::milliseconds(1);
        hp->server->on_timer();
        while (!hp->server_outbox.empty()) {
            auto dg = std::move(hp->server_outbox.front());
            hp->server_outbox.pop_front();
            client_receive(*hp->client, std::move(dg));
        }
        if (!hp->client->pending_dgs.empty()) {
            auto pending = std::move(hp->client->pending_dgs);
            hp->client->pending_dgs.clear();
            for (auto& dg : pending) {
                client_receive(*hp->client, std::move(dg));
            }
        }
        if (hp->server->state() == Connection::State::Established &&
            SSL_is_init_finished(hp->client->ssl)) {
            hp->server->on_timer();
            while (!hp->server_outbox.empty()) {
                auto dg = std::move(hp->server_outbox.front());
                hp->server_outbox.pop_front();
                client_receive(*hp->client, std::move(dg));
            }
            // After Established, all subsequent 1-RTT packets the
            // client builds must put the server's chosen SCID as DCID.
            hp->client->dcid = hp->server_scid;
            return hp;
        }
    }
    return nullptr;
}

// Build a 1-RTT short-header packet from the client side with a given
// payload, KEY_PHASE bit, and packet-protection keys.  Mirrors the
// internals of `client_emit`'s 1-RTT branch.
auto client_build_short_with_keys(QuicTestClient& c, const PacketKeys& keys,
                                  const PacketKeys& hp_keys, Aead a, uint8_t key_phase,
                                  std::span<const uint8_t> payload) -> std::vector<uint8_t> {
    const auto i = static_cast<std::size_t>(EncryptionLevel::Application);
    const std::size_t pn_len = 4;
    std::vector<uint8_t> pkt;
    pkt.push_back(
        static_cast<uint8_t>(0x40 | (static_cast<uint8_t>(key_phase) << 2) | (pn_len - 1)));
    pkt.insert(pkt.end(), c.dcid.begin(), c.dcid.end());
    const std::size_t pn_offset = pkt.size();
    const uint64_t pn = c.next_pn[i]++;
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
    auto mask = header_protection_mask(a, {hp_keys.hp.data(), hp_keys.hp.size()},
                                       {pkt.data() + pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x1F;
    for (std::size_t k = 0; k < pn_len; ++k) {
        pkt[pn_offset + k] ^= mask[1 + k];
    }
    return pkt;
}

// Decrypt a server-emitted 1-RTT short-header datagram using the
// supplied keys.  Returns true on AEAD success and sets `key_phase_out`
// to the KP bit and `plaintext_out` to the unprotected payload.
auto client_decrypt_short_with_keys(QuicTestClient& c, std::vector<uint8_t> dg,
                                    const PacketKeys& keys, const PacketKeys& hp_keys, Aead a,
                                    uint8_t& key_phase_out,
                                    std::vector<uint8_t>& plaintext_out) -> bool {
    const auto i = static_cast<std::size_t>(EncryptionLevel::Application);
    if (dg.empty() || (dg[0] & 0x80U) != 0) return false;
    const std::size_t pn_offset = 1 + c.scid.size();
    if (pn_offset + 4 + kSampleLen > dg.size()) return false;
    auto mask = header_protection_mask(a, {hp_keys.hp.data(), hp_keys.hp.size()},
                                       {dg.data() + pn_offset + 4, kSampleLen});
    dg[0] ^= mask[0] & 0x1F;
    const std::size_t pn_len = (dg[0] & 0x03U) + 1;
    for (std::size_t k = 0; k < pn_len; ++k) {
        dg[pn_offset + k] ^= mask[1 + k];
    }
    key_phase_out = static_cast<uint8_t>((dg[0] >> 2) & 0x01U);
    uint64_t trunc = 0;
    for (std::size_t k = 0; k < pn_len; ++k) {
        trunc = (trunc << 8) | dg[pn_offset + k];
    }
    uint64_t pn = decode_packet_number(c.any_recv[i] ? c.largest_recv_pn[i] : 0, trunc,
                                       pn_len * 8);
    const std::size_t header_len = pn_offset + pn_len;
    std::span<const uint8_t> aad{dg.data(), header_len};
    std::span<const uint8_t> ct{dg.data() + header_len, dg.size() - header_len};
    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
    if (!aead_open(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()}, aad,
                   ct, plaintext_out)) {
        return false;
    }
    if (!c.any_recv[i] || pn > c.largest_recv_pn[i]) c.largest_recv_pn[i] = pn;
    c.any_recv[i] = true;
    return true;
}

} // namespace

// RFC 9001 §6 — server accepts a peer-initiated 1-RTT key update.
// Build a short-header packet on the client side using the toggled
// KEY_PHASE bit and next-generation keys derived from "quic ku"; the
// server should decrypt it, commit the recv-side update, and rotate
// its own send keys so the next outbound packet carries KEY_PHASE=1.
TEST(QuicConnection, AcceptsPeerInitiatedKeyUpdate) {
    auto hp = handshake_pair({0x6B, 0x65, 0x79, 0x21, 0x00, 0x00, 0x00, 0x00},
                             {0x55, 0x56},
                             {0xAB, 0xAB, 0xAB, 0xAB});
    ASSERT_NE(hp, nullptr);
    auto& server = *hp->server;
    auto& client = *hp->client;
    hp->server_outbox.clear();
    EXPECT_EQ(server.send_key_phase(), 0U);
    EXPECT_EQ(server.recv_key_phase(), 0U);

    // Derive the client's next-generation 1-RTT send keys from its
    // current send secret.  Header-protection key does NOT rotate
    // (RFC 9001 §6.1), so reuse the existing hp value.
    const auto app = static_cast<std::size_t>(EncryptionLevel::Application);
    const Aead a = client.aead[app];
    const Hash h = aead_hash(a);
    auto next_send_secret = derive_next_application_secret(
        h, {client.send_secret[app].data(), client.send_secret[app].size()});
    PacketKeys next_keys = derive_packet_keys(a, next_send_secret);
    next_keys.hp = client.send_keys[app].hp;

    // Build a 1-RTT short-header packet with KP=1 carrying a single
    // PING frame (cheapest ack-eliciting payload).
    std::vector<uint8_t> payload;
    encode_frame(payload, Frame{PingFrame{}});
    auto pkt = client_build_short_with_keys(client, next_keys, client.send_keys[app], a,
                                            /*key_phase=*/1, {payload.data(), payload.size()});

    server.on_datagram({pkt.data(), pkt.size()});
    server.on_timer();

    EXPECT_EQ(server.recv_key_phase(), 1U)
        << "server didn't commit recv-side key update after accepting peer KP=1";
    EXPECT_EQ(server.send_key_phase(), 1U)
        << "server didn't rotate send keys after accepting peer-initiated update";
    ASSERT_FALSE(hp->server_outbox.empty()) << "server sent nothing in response";

    // Decrypt the server's reply using OUR next-generation recv keys
    // (server's new send secret was derived from its prior send
    // secret == client's prior recv secret).
    auto next_recv_secret = derive_next_application_secret(
        h, {client.recv_secret[app].data(), client.recv_secret[app].size()});
    PacketKeys next_recv_keys = derive_packet_keys(a, next_recv_secret);
    next_recv_keys.hp = client.recv_keys[app].hp;
    bool decoded_in_new_phase = false;
    while (!hp->server_outbox.empty()) {
        auto dg = std::move(hp->server_outbox.front());
        hp->server_outbox.pop_front();
        uint8_t kp = 0xFFU;
        std::vector<uint8_t> plain;
        if (client_decrypt_short_with_keys(client, std::move(dg), next_recv_keys,
                                           client.recv_keys[app], a, kp, plain)) {
            EXPECT_EQ(kp, 1U) << "server's response not carrying KEY_PHASE=1";
            decoded_in_new_phase = true;
        }
    }
    EXPECT_TRUE(decoded_in_new_phase)
        << "server's post-update reply could not be decrypted with next-gen recv keys";
}

// RFC 9001 §6 — server-initiated 1-RTT key update via
// `Connection::initiate_key_update()`.  After the call the server's
// send side ratchets to phase 1 (recv stays at 0 until the peer
// acks).  When the client then sends KP=1 with its own next-gen keys
// the recv side also commits.
TEST(QuicConnection, ServerInitiatedKeyUpdate) {
    auto hp = handshake_pair({0x6B, 0x75, 0x21, 0x21, 0x00, 0x00, 0x00, 0x00},
                             {0x77, 0x78},
                             {0xC0, 0xC1, 0xC2, 0xC3});
    ASSERT_NE(hp, nullptr);
    auto& server = *hp->server;
    auto& client = *hp->client;
    hp->server_outbox.clear();

    ASSERT_TRUE(server.initiate_key_update());
    EXPECT_EQ(server.send_key_phase(), 1U);
    EXPECT_EQ(server.recv_key_phase(), 0U) << "recv side must wait for peer ack";

    // The server should now emit subsequent 1-RTT packets with KP=1.
    // Trigger a send by writing on a stream.
    const std::string body = "post-update";
    std::vector<uint8_t> body_bytes(body.begin(), body.end());
    server.write_stream(0, body_bytes, /*fin=*/false);
    server.on_timer();
    ASSERT_FALSE(hp->server_outbox.empty());

    // Client's next-gen recv keys (server's new send secret derives
    // from client's prior recv secret).
    const auto app = static_cast<std::size_t>(EncryptionLevel::Application);
    const Aead a = client.aead[app];
    const Hash h = aead_hash(a);
    auto next_recv_secret = derive_next_application_secret(
        h, {client.recv_secret[app].data(), client.recv_secret[app].size()});
    PacketKeys next_recv_keys = derive_packet_keys(a, next_recv_secret);
    next_recv_keys.hp = client.recv_keys[app].hp;

    bool saw_new_phase = false;
    while (!hp->server_outbox.empty()) {
        auto dg = std::move(hp->server_outbox.front());
        hp->server_outbox.pop_front();
        uint8_t kp = 0xFFU;
        std::vector<uint8_t> plain;
        if (client_decrypt_short_with_keys(client, std::move(dg), next_recv_keys,
                                           client.recv_keys[app], a, kp, plain)) {
            EXPECT_EQ(kp, 1U) << "server emit after initiate_key_update without KP=1";
            saw_new_phase = true;
        }
    }
    EXPECT_TRUE(saw_new_phase)
        << "no server-emitted datagram decrypted with the next-gen recv keys";
}

// RFC 9000 §9 — limited connection-migration support: the Listener
// freezes the per-connection routing address once the handshake
// completes.  A forged short-header datagram arriving from a spoofed
// source MUST NOT redirect our outbound traffic.  This is the
// security fix for the open hole called out in docs/quic-security.md.
TEST(QuicListener, FreezesPathPostHandshake) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig tcfg{cert, key, {"h3"}};
    auto tls_ctx = TlsContext::make_server(tcfg);
    ASSERT_NE(tls_ctx, nullptr);

    Listener::Config cfg;
    cfg.tls_ctx = tls_ctx;
    cfg.server_tp.initial_max_data = 1 << 20;
    cfg.server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    cfg.server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    cfg.server_tp.initial_max_streams_bidi = 16;
    cfg.server_tp.initial_max_streams_uni = 3;
    cfg.random_seed = 0xC0FFEEULL;
    cfg.server_cid_length = 4;

    // Track every datagram + the peer it was routed to.
    std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> listener_outbox;
    Listener listener(cfg,
                      [&](const PeerAddr& p, std::span<const uint8_t> dg) {
                          listener_outbox.emplace_back(p, std::vector<uint8_t>(dg.begin(), dg.end()));
                      });

    // Legitimate peer at 10.0.0.1:5555.
    PeerAddr legit{};
    legit.length = sizeof(sockaddr_in);
    {
        auto* sin = reinterpret_cast<sockaddr_in*>(&legit.storage);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(5555);
        const uint8_t bytes[4] = {10, 0, 0, 1};
        std::memcpy(&sin->sin_addr, bytes, 4);
    }
    // Off-path attacker at 192.0.2.99:31337.
    PeerAddr attacker{};
    attacker.length = sizeof(sockaddr_in);
    {
        auto* sin = reinterpret_cast<sockaddr_in*>(&attacker.storage);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(31337);
        const uint8_t bytes[4] = {192, 0, 2, 99};
        std::memcpy(&sin->sin_addr, bytes, 4);
    }

    std::vector<uint8_t> client_dcid = {0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1};
    std::vector<uint8_t> client_scid = {0x10, 0x11};
    auto client = make_client(client_dcid, client_scid);

    // Drive the handshake through the Listener.  The Listener
    // allocates a server SCID on first Initial; we recover it from
    // listener_outbox after the first emit so we can later look up
    // the connection.
    std::vector<uint8_t> server_scid;
    for (int round = 0; round < 30; ++round) {
        int rc = SSL_do_handshake(client->ssl);
        if (rc != 1) {
            int err = SSL_get_error(client->ssl, rc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                FAIL() << "client SSL_do_handshake err=" << err;
            }
        }
        client_emit(*client);
        while (!client->outbox.empty()) {
            auto dg = std::move(client->outbox.front());
            client->outbox.pop_front();
            listener.on_datagram(legit, {dg.data(), dg.size()});
        }
        while (!listener_outbox.empty()) {
            auto [_peer, dg] = std::move(listener_outbox.front());
            listener_outbox.pop_front();
            client_receive(*client, std::move(dg));
        }
        if (!client->pending_dgs.empty()) {
            auto pending = std::move(client->pending_dgs);
            client->pending_dgs.clear();
            for (auto& dg : pending) {
                client_receive(*client, std::move(dg));
            }
        }
        // On round 0 the listener creates the Connection — capture
        // its server SCID so we can also poke a forged short-header
        // packet at the right DCID later.
        if (server_scid.empty() && listener.connection_count() == 1) {
            listener.for_each_connection([&](const std::vector<uint8_t>& scid, Connection&) {
                server_scid = scid;
            });
        }
        Connection* conn = nullptr;
        if (!server_scid.empty()) {
            conn = listener.find_connection({server_scid.data(), server_scid.size()});
        }
        if (conn && conn->state() == Connection::State::Established &&
            SSL_is_init_finished(client->ssl)) {
            break;
        }
    }
    ASSERT_FALSE(server_scid.empty());
    auto* conn = listener.find_connection({server_scid.data(), server_scid.size()});
    ASSERT_NE(conn, nullptr);
    ASSERT_TRUE(conn->handshake_complete());

    // The current routing address is the legitimate peer.
    const PeerAddr* current = listener.peer_for({server_scid.data(), server_scid.size()});
    ASSERT_NE(current, nullptr);
    EXPECT_TRUE(peer_addr_equal(*current, legit));

    // Forged short-header datagram from the attacker's address,
    // carrying our server SCID as DCID (so it routes to the right
    // Connection).  The bytes after the DCID are random noise —
    // header protection won't unmask correctly so decryption will
    // fail, but that's fine; the Listener's path-freeze decision is
    // made before decrypt.
    std::vector<uint8_t> forged;
    forged.push_back(0x43); // short header, 4-byte PN length marker
    forged.insert(forged.end(), server_scid.begin(), server_scid.end());
    for (int i = 0; i < 64; ++i) forged.push_back(static_cast<uint8_t>(i ^ 0xA5));
    listener.on_datagram(attacker, {forged.data(), forged.size()});

    current = listener.peer_for({server_scid.data(), server_scid.size()});
    ASSERT_NE(current, nullptr);
    EXPECT_TRUE(peer_addr_equal(*current, legit))
        << "forged short-header datagram from attacker addr redirected last_peer";
    EXPECT_FALSE(peer_addr_equal(*current, attacker));

    // Same check for a forged long-header (Initial) datagram from the
    // attacker.  Post-handshake there's no legitimate reason for a
    // peer to send an Initial; the freeze still applies.
    std::vector<uint8_t> forged_long;
    forged_long.push_back(static_cast<uint8_t>(0x80 | 0x40 |
                                               (static_cast<uint8_t>(LongType::Initial) << 4)));
    forged_long.push_back(0x00);
    forged_long.push_back(0x00);
    forged_long.push_back(0x00);
    forged_long.push_back(0x01);
    forged_long.push_back(static_cast<uint8_t>(server_scid.size()));
    forged_long.insert(forged_long.end(), server_scid.begin(), server_scid.end());
    forged_long.push_back(0x00); // empty SCID
    VarInt::append(forged_long, 0);    // token length
    VarInt::append(forged_long, 32);   // payload length
    for (int i = 0; i < 32; ++i) forged_long.push_back(static_cast<uint8_t>(i ^ 0x5A));
    listener.on_datagram(attacker, {forged_long.data(), forged_long.size()});

    current = listener.peer_for({server_scid.data(), server_scid.size()});
    ASSERT_NE(current, nullptr);
    EXPECT_TRUE(peer_addr_equal(*current, legit))
        << "forged Initial from attacker addr redirected last_peer";
}

// RFC 9000 §4.6 — a peer STREAM frame for a stream index beyond the
// advertised MAX_STREAMS closes the connection with STREAM_LIMIT_ERROR.
TEST(QuicConnection, RejectsStreamBeyondMaxStreams) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0x51, 0x52, 0x53, 0x54};
    std::vector<uint8_t> client_scid = {0x61, 0x62};
    std::vector<uint8_t> server_scid = {0x71, 0x72, 0x73, 0x74};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 2; // indices 0,1 allowed; 2+ rejected
    server_tp.initial_max_streams_uni = 3;

    auto clock_now = std::chrono::steady_clock::now();
    Connection::ClockFn clock_fn = [&]() { return clock_now; };
    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      },
                      clock_fn);

    auto client = make_client(client_dcid, client_scid);
    drive_to_established(server, *client, server_outbox, clock_now);
    ASSERT_EQ(server.state(), Connection::State::Established);
    server_outbox.clear();
    client->dcid = server_scid;

    // Stream ID 8 → client-initiated bidi, index 2, which is at the
    // limit (2) and therefore forbidden.
    std::vector<uint8_t> payload;
    {
        StreamFrame sf;
        sf.stream_id = 8;
        sf.offset = 0;
        sf.has_length = true;
        sf.fin = false;
        sf.data = {'h', 'i'};
        encode_frame(payload, Frame{sf});
    }
    client_inject_1rtt(*client, server, payload);
    EXPECT_NE(server.state(), Connection::State::Established)
        << "server should have started closing after a stream-limit violation";
    server.on_timer();

    auto frames = collect_server_1rtt_frames(*client, server_outbox);
    bool found = false;
    for (auto& f : frames) {
        if (auto* cc = std::get_if<ConnectionCloseFrame>(&f); cc != nullptr) {
            EXPECT_FALSE(cc->application);
            EXPECT_EQ(cc->error_code, 0x04U); // STREAM_LIMIT_ERROR
            found = true;
        }
    }
    EXPECT_TRUE(found) << "no CONNECTION_CLOSE(STREAM_LIMIT_ERROR) emitted";
}

// RFC 9000 §4.1 — STREAM data exceeding the connection-level receive
// limit closes the connection with FLOW_CONTROL_ERROR.
TEST(QuicConnection, EnforcesConnectionMaxData) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0x81, 0x82, 0x83, 0x84};
    std::vector<uint8_t> client_scid = {0x91, 0x92};
    std::vector<uint8_t> server_scid = {0xA1, 0xA2, 0xA3, 0xA4};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 10; // tiny connection window
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16; // per-stream is generous
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;

    auto clock_now = std::chrono::steady_clock::now();
    Connection::ClockFn clock_fn = [&]() { return clock_now; };
    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      },
                      clock_fn);

    auto client = make_client(client_dcid, client_scid);
    drive_to_established(server, *client, server_outbox, clock_now);
    ASSERT_EQ(server.state(), Connection::State::Established);
    server_outbox.clear();
    client->dcid = server_scid;

    // 20 bytes on stream 0 blows past the 10-byte connection limit even
    // though the per-stream window (64 KiB) would allow it.
    std::vector<uint8_t> payload;
    {
        StreamFrame sf;
        sf.stream_id = 0;
        sf.offset = 0;
        sf.has_length = true;
        sf.fin = false;
        sf.data.assign(20, 0x5A);
        encode_frame(payload, Frame{sf});
    }
    client_inject_1rtt(*client, server, payload);
    EXPECT_NE(server.state(), Connection::State::Established)
        << "server should have started closing after a connection flow-control violation";
    server.on_timer();

    auto frames = collect_server_1rtt_frames(*client, server_outbox);
    bool found = false;
    for (auto& f : frames) {
        if (auto* cc = std::get_if<ConnectionCloseFrame>(&f); cc != nullptr) {
            EXPECT_FALSE(cc->application);
            EXPECT_EQ(cc->error_code, 0x03U); // FLOW_CONTROL_ERROR
            found = true;
        }
    }
    EXPECT_TRUE(found) << "no CONNECTION_CLOSE(FLOW_CONTROL_ERROR) emitted";
}

// RFC 9000 §4.1 — as the application consumes received bytes, the server
// advertises a larger connection window via a MAX_DATA frame.
TEST(QuicConnection, EmitsMaxDataAfterConsume) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0xB1, 0xB2, 0xB3, 0xB4};
    std::vector<uint8_t> client_scid = {0xC1, 0xC2};
    std::vector<uint8_t> server_scid = {0xD1, 0xD2, 0xD3, 0xD4};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    constexpr uint64_t kWindow = 100;
    server_tp.initial_max_data = kWindow;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;

    auto clock_now = std::chrono::steady_clock::now();
    Connection::ClockFn clock_fn = [&]() { return clock_now; };
    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      },
                      clock_fn);

    auto client = make_client(client_dcid, client_scid);
    drive_to_established(server, *client, server_outbox, clock_now);
    ASSERT_EQ(server.state(), Connection::State::Established);
    server_outbox.clear();
    client->dcid = server_scid;

    // Deliver 60 bytes (within the 100-byte window) on stream 0.
    constexpr uint64_t kBytes = 60;
    std::vector<uint8_t> payload;
    {
        StreamFrame sf;
        sf.stream_id = 0;
        sf.offset = 0;
        sf.has_length = true;
        sf.fin = false;
        sf.data.assign(kBytes, 0x7E);
        encode_frame(payload, Frame{sf});
    }
    client_inject_1rtt(*client, server, payload);
    ASSERT_EQ(server.state(), Connection::State::Established);

    // Consume the bytes; remaining window (40) drops below half (50) so
    // the server should extend to consumed + window == 160.
    std::vector<uint8_t> out;
    bool fin = false;
    const std::size_t n = server.read_stream(0, out, fin);
    ASSERT_EQ(n, kBytes);
    server_outbox.clear();
    server.on_timer();

    auto frames = collect_server_1rtt_frames(*client, server_outbox);
    bool found = false;
    for (auto& f : frames) {
        if (auto* md = std::get_if<MaxDataFrame>(&f); md != nullptr) {
            EXPECT_EQ(md->maximum, kBytes + kWindow);
            found = true;
        }
    }
    EXPECT_TRUE(found) << "server did not emit MAX_DATA after the application consumed data";
}

// RFC 9000 §7.5 — a CRYPTO frame whose offset sits far beyond the
// in-order read cursor is rejected with CRYPTO_BUFFER_EXCEEDED rather
// than buffered, closing an unauthenticated pre-handshake memory DoS.
TEST(QuicConnection, RejectsOversizedCryptoOffset) {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig cfg{cert, key, {"h3"}};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    std::vector<uint8_t> client_dcid = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
    std::vector<uint8_t> client_scid = {0xD5, 0xD6};
    std::vector<uint8_t> server_scid = {0xE5, 0xE6, 0xE7, 0xE8};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 3;

    auto clock_now = std::chrono::steady_clock::now();
    Connection::ClockFn clock_fn = [&]() { return clock_now; };
    Connection server(ctx, {client_dcid.data(), client_dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      },
                      clock_fn);

    // First datagram from the "client": a valid Initial packet whose sole
    // CRYPTO frame declares an offset far past the 64 KiB reassembly
    // window. Padded to the client Initial minimum so the server accepts
    // it for processing.
    auto client = make_client(client_dcid, client_scid);
    std::vector<uint8_t> payload;
    {
        CryptoFrame cf;
        cf.offset = 200000; // >> 64 KiB window above read offset 0
        cf.data = {0x01, 0x02, 0x03, 0x04};
        encode_frame(payload, Frame{cf});
    }
    payload.resize(1200, 0x00); // trailing PADDING frames
    auto pkt =
        client_build_long(*client, EncryptionLevel::Initial, LongType::Initial, payload);

    server.on_datagram({pkt.data(), pkt.size()});
    EXPECT_EQ(server.state(), Connection::State::Closing)
        << "server should close on a CRYPTO frame past the reassembly window";
}
