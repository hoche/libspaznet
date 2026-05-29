// Phase 3 verification.
//
// 1. Transport-parameter round-trip (RFC 9000 §18).
//
// 2. An end-to-end TLS handshake driven entirely in-memory. We construct
//    a TlsConnection in server mode against a freshly-generated
//    self-signed cert, and we use a second SSL* in client mode (also
//    wired through SSL_set_quic_tls_cbs but with a much simpler shim)
//    to feed it CRYPTO frames. By the end of the dance both sides should
//    reach TLS_ST_OK and have exchanged transport parameters.

#include <gtest/gtest.h>

#include <libspaznet/quic/tls.hpp>
#include <libspaznet/quic/transport_params.hpp>

#include <memory>
#include <string>
#include <vector>

#include <openssl/bn.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace {

using namespace spaznet::quic;

// Minimal self-signed P-256 ECDSA cert generated at test time. Returns
// the cert PEM and key PEM as a pair.
auto make_self_signed() -> std::pair<std::string, std::string> {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (pkey == nullptr) {
        throw std::runtime_error("EVP_EC_gen failed");
    }

    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 60 * 60);
    X509_set_pubkey(x, pkey);

    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("libspaznet-test"), -1, -1,
                               0);
    X509_set_issuer_name(x, name);

    // SAN: DNS:localhost so OpenSSL's hostname checks (if enabled) pass.
    GENERAL_NAMES* gens = sk_GENERAL_NAME_new_null();
    GENERAL_NAME* gen = GENERAL_NAME_new();
    ASN1_IA5STRING* ia5 = ASN1_IA5STRING_new();
    ASN1_STRING_set(ia5, "localhost", static_cast<int>(std::strlen("localhost")));
    GENERAL_NAME_set0_value(gen, GEN_DNS, ia5);
    sk_GENERAL_NAME_push(gens, gen);
    X509_add1_ext_i2d(x, NID_subject_alt_name, gens, 0, 0);
    sk_GENERAL_NAME_pop_free(gens, GENERAL_NAME_free);

    X509_sign(x, pkey, EVP_sha256());

    BIO* cb = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cb, x);
    char* cdata = nullptr;
    long clen = BIO_get_mem_data(cb, &cdata);
    std::string cert_pem(cdata, static_cast<std::size_t>(clen));

    BIO* kb = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* kdata = nullptr;
    long klen = BIO_get_mem_data(kb, &kdata);
    std::string key_pem(kdata, static_cast<std::size_t>(klen));

    BIO_free(cb);
    BIO_free(kb);
    X509_free(x);
    EVP_PKEY_free(pkey);

    return {cert_pem, key_pem};
}

// A toy client TLS state used to drive the server's handshake. It mirrors
// the server-side TlsConnection's structure but speaks the client role.
struct TestClient {
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    std::array<std::vector<uint8_t>, 4> out{};
    std::array<std::vector<uint8_t>, 4> in{};
    std::array<std::size_t, 4> in_cursor{0, 0, 0, 0};
    uint32_t send_level{OSSL_RECORD_PROTECTION_LEVEL_NONE};
    std::vector<uint8_t> own_tp_wire{};
    std::vector<uint8_t> peer_tp_wire{};
    bool got_peer_tp{false};
    uint8_t alert_code{0};
    bool handshake_done{false};

    ~TestClient() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
    }

    static auto level_to_idx(uint32_t l) -> std::size_t {
        switch (l) {
            case OSSL_RECORD_PROTECTION_LEVEL_NONE:
                return 0;
            case OSSL_RECORD_PROTECTION_LEVEL_EARLY:
                return 1;
            case OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE:
                return 2;
            default:
                return 3;
        }
    }

    static auto idx_to_level(std::size_t i) -> uint32_t {
        switch (i) {
            case 0:
                return OSSL_RECORD_PROTECTION_LEVEL_NONE;
            case 1:
                return OSSL_RECORD_PROTECTION_LEVEL_EARLY;
            case 2:
                return OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE;
            default:
                return OSSL_RECORD_PROTECTION_LEVEL_APPLICATION;
        }
    }
};

auto client_cb_send(SSL*, const unsigned char* buf, size_t buf_len, size_t* consumed, void* arg)
    -> int {
    auto* c = static_cast<TestClient*>(arg);
    c->out[TestClient::level_to_idx(c->send_level)].insert(
        c->out[TestClient::level_to_idx(c->send_level)].end(), buf, buf + buf_len);
    *consumed = buf_len;
    return 1;
}

auto client_cb_recv(SSL*, const unsigned char** buf, size_t* bytes_read, void* arg) -> int {
    auto* c = static_cast<TestClient*>(arg);
    for (std::size_t i = 0; i < 4; ++i) {
        if (c->in_cursor[i] < c->in[i].size()) {
            *buf = c->in[i].data() + c->in_cursor[i];
            *bytes_read = c->in[i].size() - c->in_cursor[i];
            return 1;
        }
    }
    *buf = nullptr;
    *bytes_read = 0;
    return 1;
}

auto client_cb_release(SSL*, size_t bytes_read, void* arg) -> int {
    auto* c = static_cast<TestClient*>(arg);
    for (std::size_t i = 0; i < 4; ++i) {
        if (c->in_cursor[i] < c->in[i].size()) {
            std::size_t avail = c->in[i].size() - c->in_cursor[i];
            c->in_cursor[i] += std::min(bytes_read, avail);
            return 1;
        }
    }
    return 1;
}

auto client_cb_yield(SSL*, uint32_t prot_level, int /*direction*/,
                     const unsigned char* /*secret*/, size_t /*secret_len*/, void* arg) -> int {
    auto* c = static_cast<TestClient*>(arg);
    c->send_level = prot_level;
    return 1;
}

auto client_cb_got_tp(SSL*, const unsigned char* params, size_t params_len, void* arg) -> int {
    auto* c = static_cast<TestClient*>(arg);
    c->peer_tp_wire.assign(params, params + params_len);
    c->got_peer_tp = true;
    return 1;
}

auto client_cb_alert(SSL*, unsigned char alert_code, void* arg) -> int {
    auto* c = static_cast<TestClient*>(arg);
    c->alert_code = alert_code;
    return 1;
}

auto build_client_dispatch() -> const OSSL_DISPATCH* {
    static const OSSL_DISPATCH t[] = {
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND,
         reinterpret_cast<void (*)()>(client_cb_send)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD,
         reinterpret_cast<void (*)()>(client_cb_recv)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD,
         reinterpret_cast<void (*)()>(client_cb_release)},
        {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET,
         reinterpret_cast<void (*)()>(client_cb_yield)},
        {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS,
         reinterpret_cast<void (*)()>(client_cb_got_tp)},
        {OSSL_FUNC_SSL_QUIC_TLS_ALERT,
         reinterpret_cast<void (*)()>(client_cb_alert)},
        {0, nullptr},
    };
    return t;
}

auto make_test_client(const std::vector<uint8_t>& tp_wire) -> std::unique_ptr<TestClient> {
    auto c = std::make_unique<TestClient>();
    c->ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(c->ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, nullptr);

    // ALPN: offer h3.
    const unsigned char alpn[] = {2, 'h', '3'};
    SSL_CTX_set_alpn_protos(c->ctx, alpn, sizeof(alpn));

    c->ssl = SSL_new(c->ctx);
    SSL_set_connect_state(c->ssl);
    SSL_set_tlsext_host_name(c->ssl, "localhost");
    c->own_tp_wire = tp_wire;
    // The cbs install must precede the transport-params install (the
    // latter is only meaningful once the SSL* is in QUIC mode).
    SSL_set_quic_tls_cbs(c->ssl, build_client_dispatch(), c.get());
    SSL_set_quic_tls_transport_params(c->ssl, c->own_tp_wire.data(), c->own_tp_wire.size());
    return c;
}

} // namespace

TEST(QuicTransportParams, RoundTrip) {
    TransportParameters tp;
    tp.original_destination_connection_id = std::vector<uint8_t>{1, 2, 3, 4};
    tp.initial_source_connection_id = std::vector<uint8_t>{5, 6, 7, 8};
    tp.stateless_reset_token = std::array<uint8_t, 16>{};
    for (int i = 0; i < 16; ++i) (*tp.stateless_reset_token)[i] = static_cast<uint8_t>(i);
    tp.max_idle_timeout_ms = 30000;
    tp.initial_max_data = 1 << 20;
    tp.initial_max_stream_data_bidi_local = 65535;
    tp.initial_max_stream_data_bidi_remote = 65535;
    tp.initial_max_stream_data_uni = 65535;
    tp.initial_max_streams_bidi = 100;
    tp.initial_max_streams_uni = 3;
    tp.ack_delay_exponent = 7; // non-default
    tp.max_ack_delay_ms = 50;  // non-default
    tp.active_connection_id_limit = 4;
    tp.disable_active_migration = true;

    auto wire = encode_transport_params(tp);
    TransportParameters parsed;
    ASSERT_TRUE(decode_transport_params({wire.data(), wire.size()}, parsed));

    EXPECT_EQ(parsed.original_destination_connection_id,
              tp.original_destination_connection_id);
    EXPECT_EQ(parsed.initial_source_connection_id, tp.initial_source_connection_id);
    EXPECT_EQ(parsed.stateless_reset_token, tp.stateless_reset_token);
    EXPECT_EQ(parsed.max_idle_timeout_ms, 30000U);
    EXPECT_EQ(parsed.initial_max_data, 1U << 20);
    EXPECT_EQ(parsed.initial_max_streams_bidi, 100U);
    EXPECT_EQ(parsed.ack_delay_exponent, 7U);
    EXPECT_EQ(parsed.max_ack_delay_ms, 50U);
    EXPECT_EQ(parsed.active_connection_id_limit, 4U);
    EXPECT_TRUE(parsed.disable_active_migration);
}

TEST(QuicTransportParams, UnknownPreserved) {
    // Hand-build a wire string with id=0x1234 and value bytes 0xAA 0xBB.
    std::vector<uint8_t> wire;
    // id varint 0x1234 -> 0x52 0x34 (2-byte form).
    wire.insert(wire.end(), {0x52, 0x34, 0x02, 0xAA, 0xBB});
    TransportParameters parsed;
    ASSERT_TRUE(decode_transport_params({wire.data(), wire.size()}, parsed));
    ASSERT_EQ(parsed.unknown.size(), 1U);
    EXPECT_EQ(parsed.unknown[0].id, 0x1234U);
    EXPECT_EQ(parsed.unknown[0].value, (std::vector<uint8_t>{0xAA, 0xBB}));

    // Round-trip preserves the unknown.
    auto re = encode_transport_params(parsed);
    EXPECT_EQ(re, wire);
}

TEST(QuicTls, EndToEndHandshake) {
    auto [cert_pem, key_pem] = make_self_signed();
    TlsServerConfig cfg;
    cfg.cert_pem = cert_pem;
    cfg.key_pem = key_pem;
    cfg.alpn = {"h3"};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    TransportParameters server_tp;
    server_tp.original_destination_connection_id = std::vector<uint8_t>{0xCA, 0xFE, 0xBA, 0xBE};
    server_tp.initial_source_connection_id = std::vector<uint8_t>{0x01, 0x02};
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_streams_bidi = 10;
    server_tp.initial_max_streams_uni = 3;

    std::vector<uint8_t> client_dcid = {0xCA, 0xFE, 0xBA, 0xBE};
    TlsConnection server(ctx, {client_dcid.data(), client_dcid.size()}, server_tp);

    TransportParameters client_tp;
    client_tp.initial_source_connection_id = std::vector<uint8_t>{0xDD};
    client_tp.initial_max_data = 1 << 20;
    client_tp.initial_max_streams_bidi = 100;
    auto client_tp_wire = encode_transport_params(client_tp);
    auto client = make_test_client(client_tp_wire);

    // Pump until both sides finish or we hit a safety cap.
    for (int round = 0; round < 16; ++round) {
        // Client side: drive handshake; copy its outbound bytes into the
        // server's inbound at the corresponding level.
        int crc = SSL_do_handshake(client->ssl);
        if (crc != 1) {
            int err = SSL_get_error(client->ssl, crc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                FAIL() << "client SSL_do_handshake failed err=" << err;
            }
        }
        for (std::size_t i = 0; i < 4; ++i) {
            if (!client->out[i].empty()) {
                server.deliver_crypto(static_cast<EncryptionLevel>(i),
                                      {client->out[i].data(), client->out[i].size()});
                client->out[i].clear();
            }
        }

        auto srv_state = server.advance();
        for (std::size_t i = 0; i < 4; ++i) {
            auto& buf = server.out_crypto(static_cast<EncryptionLevel>(i));
            if (!buf.empty()) {
                client->in[i].insert(client->in[i].end(), buf.begin(), buf.end());
                buf.clear();
            }
        }

        bool client_done = SSL_is_init_finished(client->ssl) != 0;
        bool server_done = srv_state == TlsConnection::State::Established;
        if (client_done && server_done) {
            break;
        }
        ASSERT_NE(srv_state, TlsConnection::State::Failed)
            << "server failed at round " << round
            << " alert=" << static_cast<int>(server.alert_code());
    }

    EXPECT_EQ(server.state(), TlsConnection::State::Established);
    EXPECT_TRUE(SSL_is_init_finished(client->ssl));
    EXPECT_EQ(server.negotiated_alpn(), "h3");
    // Server must have parsed client's transport parameters.
    EXPECT_TRUE(server.have_peer_transport_params());
    EXPECT_EQ(server.peer_transport_params().initial_max_streams_bidi, 100U);
    // Client must have received the server's transport parameters.
    EXPECT_TRUE(client->got_peer_tp);
    TransportParameters parsed_server_tp;
    ASSERT_TRUE(decode_transport_params(
        {client->peer_tp_wire.data(), client->peer_tp_wire.size()}, parsed_server_tp));
    EXPECT_EQ(parsed_server_tp.original_destination_connection_id,
              std::vector<uint8_t>{client_dcid});
    // Application-level secrets must be derived on both sides.
    EXPECT_FALSE(server.read_secret(EncryptionLevel::Application).empty());
    EXPECT_FALSE(server.write_secret(EncryptionLevel::Application).empty());
}
