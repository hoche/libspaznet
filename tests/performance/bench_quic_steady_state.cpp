// Steady-state QUIC throughput benchmark.
//
// Sets up an in-memory loopback between our server-side quic::Connection
// and a hand-rolled OpenSSL-backed QUIC client, completes one TLS 1.3
// handshake, then opens the flow-control floodgates and times how long
// it takes to push N 1-RTT packets from server to client. Reports
// packets/sec, bytes/sec, and ns/packet on stdout in a single Markdown
// row so the output is easy to diff before/after a change.
//
// What this measures: the full server-side send path (encode frames,
// serialize header, in-place AEAD seal, header protection) plus the
// client-side receive path (header protection removal, in-place AEAD
// open, frame parse, stream reassembly). It does NOT model UDP socket
// latency — both sides are in the same process exchanging datagrams via
// in-memory queues. The intent is to isolate the QUIC-layer cost from
// the kernel.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <libspaznet/quic/connection.hpp>
#include <libspaznet/quic/varint.hpp>

#include <openssl/core_dispatch.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

using namespace spaznet::quic;
using std::chrono::duration;
using std::chrono::steady_clock;

namespace {

// --- self-signed cert generation (mirrors tests/unit/) --------------------

auto make_self_signed_p256() -> std::pair<std::string, std::string> {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME* nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("bench"), -1, -1, 0);
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

// --- QUIC client harness (copy of the test_http3_end_to_end harness;
// the bench is a one-off, deduplication can wait) ------------------------

struct QuicClient {
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    std::vector<uint8_t> dcid;
    std::vector<uint8_t> scid;
    std::array<std::vector<uint8_t>, 4> tls_out;
    std::array<std::vector<uint8_t>, 4> tls_in;
    std::array<std::size_t, 4> tls_in_cursor{0, 0, 0, 0};
    uint32_t send_level{OSSL_RECORD_PROTECTION_LEVEL_NONE};
    std::vector<uint8_t> own_tp_wire;
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
    std::deque<std::vector<uint8_t>> outbox;
    std::deque<std::vector<uint8_t>> pending_dgs;
    std::size_t bytes_received_on_stream0{0};
    bool got_handshake_done{false};

    ~QuicClient() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
    }
};

int qc_send(SSL*, const unsigned char* buf, size_t len, size_t* consumed, void* arg) {
    auto* c = static_cast<QuicClient*>(arg);
    c->tls_out[c->send_level].insert(c->tls_out[c->send_level].end(), buf, buf + len);
    *consumed = len;
    return 1;
}
int qc_recv(SSL*, const unsigned char** buf, size_t* br, void* arg) {
    auto* c = static_cast<QuicClient*>(arg);
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
int qc_release(SSL*, size_t br, void* arg) {
    auto* c = static_cast<QuicClient*>(arg);
    for (std::size_t i = 0; i < 4; ++i) {
        if (c->tls_in_cursor[i] < c->tls_in[i].size()) {
            c->tls_in_cursor[i] += std::min(br, c->tls_in[i].size() - c->tls_in_cursor[i]);
            return 1;
        }
    }
    return 1;
}
int qc_yield(SSL*, uint32_t prot_level, int direction, const unsigned char* secret,
             size_t secret_len, void* arg) {
    auto* c = static_cast<QuicClient*>(arg);
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
int qc_got_tp(SSL*, const unsigned char*, size_t, void*) {
    return 1;
}
int qc_alert(SSL*, unsigned char, void*) {
    return 1;
}

const OSSL_DISPATCH* qc_dispatch() {
    static const OSSL_DISPATCH t[] = {
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND, reinterpret_cast<void (*)()>(qc_send)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD, reinterpret_cast<void (*)()>(qc_recv)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD,
         reinterpret_cast<void (*)()>(qc_release)},
        {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET, reinterpret_cast<void (*)()>(qc_yield)},
        {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS,
         reinterpret_cast<void (*)()>(qc_got_tp)},
        {OSSL_FUNC_SSL_QUIC_TLS_ALERT, reinterpret_cast<void (*)()>(qc_alert)},
        {0, nullptr}};
    return t;
}

auto make_client(std::vector<uint8_t> dcid, std::vector<uint8_t> scid)
    -> std::unique_ptr<QuicClient> {
    auto c = std::make_unique<QuicClient>();
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

    auto secret_c = derive_initial_secret(dcid, Direction::Client);
    auto secret_s = derive_initial_secret(dcid, Direction::Server);
    c->aead[0] = Aead::Aes128Gcm;
    c->send_keys[0] = derive_packet_keys(Aead::Aes128Gcm, secret_c);
    c->recv_keys[0] = derive_packet_keys(Aead::Aes128Gcm, secret_s);
    c->send_ready[0] = true;
    c->recv_ready[0] = true;

    // Huge transport-param windows so the bench is never flow-controlled.
    TransportParameters tp;
    tp.initial_source_connection_id = scid;
    tp.initial_max_data = 1ULL << 30;
    tp.initial_max_streams_bidi = 16;
    tp.initial_max_streams_uni = 16;
    tp.initial_max_stream_data_bidi_remote = 1ULL << 30;
    tp.initial_max_stream_data_bidi_local = 1ULL << 30;
    tp.initial_max_stream_data_uni = 1ULL << 30;
    c->own_tp_wire = encode_transport_params(tp);
    SSL_set_quic_tls_cbs(c->ssl, qc_dispatch(), c.get());
    SSL_set_quic_tls_transport_params(c->ssl, c->own_tp_wire.data(), c->own_tp_wire.size());
    return c;
}

auto build_long(QuicClient& c, EncryptionLevel level, LongType type,
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
    if (type == LongType::Initial) VarInt::append(pkt, 0U);
    const uint64_t length = pn_len + payload.size() + tag_len;
    VarInt::append(pkt, length);
    const std::size_t pn_offset = pkt.size();
    const uint64_t pn = c.next_pn[static_cast<std::size_t>(level)]++;
    for (std::size_t i = 0; i < pn_len; ++i)
        pkt.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - i))) & 0xFF));
    const std::size_t payload_off = pkt.size();
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
    std::vector<uint8_t> pt(pkt.begin() + payload_off, pkt.end());
    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
    std::vector<uint8_t> sealed;
    aead_seal(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
              {aad.data(), aad.size()}, {pt.data(), pt.size()}, sealed);
    pkt.resize(payload_off);
    pkt.insert(pkt.end(), sealed.begin(), sealed.end());
    auto mask = header_protection_mask(a, {keys.hp.data(), keys.hp.size()},
                                       {pkt.data() + pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x0F;
    for (std::size_t i = 0; i < pn_len; ++i) pkt[pn_offset + i] ^= mask[1 + i];
    return pkt;
}

auto build_short(QuicClient& c, const std::vector<uint8_t>& payload) -> std::vector<uint8_t> {
    const auto i = static_cast<std::size_t>(EncryptionLevel::Application);
    auto& keys = c.send_keys[i];
    Aead a = c.aead[i];
    const std::size_t pn_len = 4;
    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>(0x40 | (pn_len - 1)));
    pkt.insert(pkt.end(), c.dcid.begin(), c.dcid.end());
    const std::size_t pn_offset = pkt.size();
    uint64_t pn = c.next_pn[i]++;
    for (std::size_t k = 0; k < pn_len; ++k)
        pkt.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - k))) & 0xFF));
    const std::size_t payload_off = pkt.size();
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
    std::vector<uint8_t> pt(pkt.begin() + payload_off, pkt.end());
    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
    std::vector<uint8_t> sealed;
    aead_seal(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
              {aad.data(), aad.size()}, {pt.data(), pt.size()}, sealed);
    pkt.resize(payload_off);
    pkt.insert(pkt.end(), sealed.begin(), sealed.end());
    auto mask = header_protection_mask(a, {keys.hp.data(), keys.hp.size()},
                                       {pkt.data() + pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x1F;
    for (std::size_t k = 0; k < pn_len; ++k) pkt[pn_offset + k] ^= mask[1 + k];
    return pkt;
}

auto emit(QuicClient& c) -> void {
    for (auto lvl :
         {EncryptionLevel::Initial, EncryptionLevel::Handshake, EncryptionLevel::Application}) {
        const auto i = static_cast<std::size_t>(lvl);
        if (!c.send_ready[i]) continue;
        std::vector<uint8_t> payload;
        if (c.acks[i].needs_ack()) {
            AckFrame ack;
            c.acks[i].build_ack_frame(0, ack);
            encode_frame(payload, Frame{ack});
        }
        if (!c.tls_out[i].empty()) {
            CryptoFrame cf;
            cf.offset = c.crypto_send_off[i];
            cf.data = c.tls_out[i];
            c.crypto_send_off[i] += c.tls_out[i].size();
            encode_frame(payload, Frame{cf});
            c.tls_out[i].clear();
        }
        if (payload.empty()) continue;
        if (lvl == EncryptionLevel::Initial) {
            while (payload.size() < 1100) payload.push_back(0);
            auto pkt = build_long(c, lvl, LongType::Initial, payload);
            if (pkt.size() < 1200) pkt.resize(1200, 0);
            c.outbox.push_back(std::move(pkt));
        } else if (lvl == EncryptionLevel::Handshake) {
            c.outbox.push_back(build_long(c, lvl, LongType::Handshake, payload));
        } else {
            c.outbox.push_back(build_short(c, payload));
        }
    }
}

auto receive(QuicClient& c, std::vector<uint8_t> dg) -> void {
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
                c.pending_dgs.emplace_back(dg.begin() + static_cast<std::ptrdiff_t>(off),
                                           dg.end());
                return;
            }
            std::vector<uint8_t> pkt(dg.begin() + static_cast<std::ptrdiff_t>(off),
                                     dg.begin() + static_cast<std::ptrdiff_t>(cursor));
            LongHeader local = hdr;
            local.pn_offset -= off;
            uint64_t pn = 0;
            std::vector<uint8_t> plain;
            if (!decrypt_long_packet(c.aead[i], c.recv_keys[i],
                                     c.any_recv[i] ? c.largest_recv_pn[i] : 0, pkt, local,
                                     pn, plain)) {
                return;
            }
            c.any_recv[i] = true;
            if (pn > c.largest_recv_pn[i]) c.largest_recv_pn[i] = pn;
            c.acks[i].on_received(pn, true);
            std::vector<Frame> frames;
            if (!parse_frames({plain.data(), plain.size()}, frames)) return;
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
            const auto i = static_cast<std::size_t>(EncryptionLevel::Application);
            if (!c.recv_ready[i]) return;
            std::vector<uint8_t> pkt(dg.begin() + static_cast<std::ptrdiff_t>(off), dg.end());
            const std::size_t pn_offset = 1 + c.scid.size();
            if (pn_offset + 4 + kSampleLen > pkt.size()) return;
            auto mask = header_protection_mask(c.aead[i], {c.recv_keys[i].hp.data(),
                                                            c.recv_keys[i].hp.size()},
                                                {pkt.data() + pn_offset + 4, kSampleLen});
            pkt[0] ^= mask[0] & 0x1F;
            const std::size_t pn_len = (pkt[0] & 0x03U) + 1;
            for (std::size_t k = 0; k < pn_len; ++k) pkt[pn_offset + k] ^= mask[1 + k];
            uint64_t trunc = 0;
            for (std::size_t k = 0; k < pn_len; ++k) trunc = (trunc << 8) | pkt[pn_offset + k];
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
            std::vector<Frame> frames;
            if (!parse_frames({plain.data(), plain.size()}, frames)) return;
            for (auto& f : frames) {
                std::visit(
                    [&](auto& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, StreamFrame>) {
                            if (x.stream_id == 0) {
                                c.bytes_received_on_stream0 += x.data.size();
                            }
                        } else if constexpr (std::is_same_v<T, HandshakeDoneFrame>) {
                            c.got_handshake_done = true;
                        }
                    },
                    f);
            }
            return;
        }
    }
}

// --- the bench ----------------------------------------------------------

struct BenchResult {
    std::size_t packets;
    std::size_t bytes;
    double seconds;

    auto packets_per_sec() const -> double { return packets / seconds; }
    auto megabits_per_sec() const -> double { return (bytes * 8.0) / seconds / 1e6; }
    auto ns_per_packet() const -> double { return seconds * 1e9 / packets; }
};

auto run_bench(std::size_t target_packets, std::size_t chunk_size) -> BenchResult {
    auto [cert, key] = make_self_signed_p256();
    TlsServerConfig tcfg{cert, key, {"h3"}};
    auto tls_ctx = TlsContext::make_server(tcfg);

    std::vector<uint8_t> dcid = {0xCA, 0xFE, 0xBA, 0xBE};
    std::vector<uint8_t> client_scid = {0x77};
    std::vector<uint8_t> server_scid = {0x11, 0x22, 0x33, 0x44};

    std::deque<std::vector<uint8_t>> server_outbox;
    std::size_t server_pkts_out = 0;
    std::size_t server_bytes_out = 0;
    bool measuring = false;

    TransportParameters server_tp;
    server_tp.initial_max_data = 1ULL << 30;
    server_tp.initial_max_stream_data_bidi_remote = 1ULL << 30;
    server_tp.initial_max_stream_data_bidi_local = 1ULL << 30;
    server_tp.initial_max_stream_data_uni = 1ULL << 30;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 16;

    Connection server(tls_ctx, {dcid.data(), dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          if (measuring) {
                              ++server_pkts_out;
                              server_bytes_out += dg.size();
                          }
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      });

    auto client = make_client(dcid, client_scid);

    // Phase 1: handshake.
    for (int round = 0; round < 30; ++round) {
        SSL_do_handshake(client->ssl);
        emit(*client);
        while (!client->outbox.empty()) {
            auto dg = std::move(client->outbox.front());
            client->outbox.pop_front();
            server.on_datagram({dg.data(), dg.size()});
        }
        server.on_timer();
        while (!server_outbox.empty()) {
            receive(*client, std::move(server_outbox.front()));
            server_outbox.pop_front();
        }
        if (!client->pending_dgs.empty()) {
            auto pending = std::move(client->pending_dgs);
            client->pending_dgs.clear();
            for (auto& dg : pending) receive(*client, std::move(dg));
        }
        if (server.state() == Connection::State::Established &&
            SSL_is_init_finished(client->ssl) && client->got_handshake_done) {
            break;
        }
    }
    if (server.state() != Connection::State::Established) {
        std::fprintf(stderr, "bench: handshake failed to complete\n");
        std::exit(1);
    }

    // Phase 2: steady state. The server writes `chunk_size` bytes to
    // stream 0 in a loop, then drains. The client processes datagrams
    // and emits ACKs so flow control stays open.
    std::vector<uint8_t> chunk(chunk_size, 0xAB);
    measuring = true;
    auto t0 = steady_clock::now();

    while (server_pkts_out < target_packets) {
        // Top off the server's send buffer.
        server.write_stream(0, {chunk.data(), chunk.size()}, /*fin=*/false);

        // Drive server.
        server.on_timer();

        // Drain server's output to the client; client emits ACKs.
        while (!server_outbox.empty()) {
            auto dg = std::move(server_outbox.front());
            server_outbox.pop_front();
            receive(*client, std::move(dg));
        }
        emit(*client);
        while (!client->outbox.empty()) {
            auto dg = std::move(client->outbox.front());
            client->outbox.pop_front();
            server.on_datagram({dg.data(), dg.size()});
        }
    }

    auto t1 = steady_clock::now();
    return BenchResult{server_pkts_out, server_bytes_out,
                       duration<double>(t1 - t0).count()};
}

} // namespace

int main(int argc, char** argv) {
    std::size_t target_packets = 50000;
    std::size_t chunk_size = 4096;
    int reps = 3;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--packets" && i + 1 < argc) target_packets = std::stoul(argv[++i]);
        else if (a == "--chunk" && i + 1 < argc) chunk_size = std::stoul(argv[++i]);
        else if (a == "--reps" && i + 1 < argc) reps = std::atoi(argv[++i]);
    }

    std::printf("# QUIC steady-state throughput\n\n");
    std::printf("Target packets per rep: %zu, chunk size: %zu bytes, reps: %d\n\n",
                target_packets, chunk_size, reps);
    std::printf("| rep | packets | bytes      | seconds | pkts/sec    | Mbps    | ns/pkt |\n");
    std::printf("|-----|---------|------------|---------|-------------|---------|--------|\n");

    double sum_pps = 0;
    double sum_mbps = 0;
    double sum_ns = 0;
    for (int i = 0; i < reps; ++i) {
        auto r = run_bench(target_packets, chunk_size);
        std::printf("| %3d | %7zu | %10zu | %7.3f | %11.0f | %7.1f | %6.0f |\n",
                    i, r.packets, r.bytes, r.seconds, r.packets_per_sec(),
                    r.megabits_per_sec(), r.ns_per_packet());
        sum_pps += r.packets_per_sec();
        sum_mbps += r.megabits_per_sec();
        sum_ns += r.ns_per_packet();
    }
    std::printf("|     |         |            |  mean   | %11.0f | %7.1f | %6.0f |\n",
                sum_pps / reps, sum_mbps / reps, sum_ns / reps);
    return 0;
}
