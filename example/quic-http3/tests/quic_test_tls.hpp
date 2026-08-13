#pragma once

// Shared OpenSSL / wolfSSL helpers for quic-http3 unit/bench tests.
// Included only by test TUs that already link spaznet_quic_http3.

#include <libspaznet/quic/crypto.hpp>
#include <libspaznet/quic/detail/tls_compat.hpp>
#include <libspaznet/quic/tls.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace spaznet {
namespace quic {
namespace test {

inline auto make_test_cert_pem(const char* cn = "localhost")
    -> std::pair<std::string, std::string> {
#if defined(SPAZNET_TLS_OPENSSL)
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (pkey == nullptr) {
        throw std::runtime_error("EVP_EC_gen failed");
    }
#else
    EC_KEY* ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (ec == nullptr || EC_KEY_generate_key(ec) != 1) {
        if (ec != nullptr) EC_KEY_free(ec);
        throw std::runtime_error("EC_KEY_generate_key failed");
    }
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (pkey == nullptr || EVP_PKEY_assign_EC_KEY(pkey, ec) != 1) {
        EC_KEY_free(ec);
        if (pkey != nullptr) EVP_PKEY_free(pkey);
        throw std::runtime_error("EVP_PKEY_assign_EC_KEY failed");
    }
    // pkey owns ec after assign.
#endif

    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME* nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(x, nm);
#if defined(SPAZNET_TLS_OPENSSL)
    X509_EXTENSION* san_ext = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_subject_alt_name,
        const_cast<char*>("DNS:localhost,IP:127.0.0.1"));
    if (san_ext != nullptr) {
        X509_add_ext(x, san_ext, -1);
        X509_EXTENSION_free(san_ext);
    }
#endif
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

inline auto aead_from_ssl(SSL* ssl) -> Aead {
    const SSL_CIPHER* cph = SSL_get_current_cipher(ssl);
    if (cph == nullptr) {
        return Aead::Aes128Gcm;
    }
#if defined(SPAZNET_TLS_OPENSSL)
    return aead_from_tls_cipher_id(static_cast<uint16_t>(SSL_CIPHER_get_protocol_id(cph)));
#else
    const char* name = SSL_CIPHER_get_name(cph);
    if (name == nullptr) {
        return Aead::Aes128Gcm;
    }
    if (std::strstr(name, "CHACHA") != nullptr) {
        return Aead::ChaCha20Poly1305;
    }
    if (std::strstr(name, "AES256") != nullptr || std::strstr(name, "AES_256") != nullptr) {
        return Aead::Aes256Gcm;
    }
    return Aead::Aes128Gcm;
#endif
}

inline auto level_index(EncryptionLevel level) -> std::size_t {
    return static_cast<std::size_t>(level);
}

#if defined(SPAZNET_TLS_WOLFSSL)
inline auto to_wolf_level(EncryptionLevel level) -> WOLFSSL_ENCRYPTION_LEVEL {
    switch (level) {
        case EncryptionLevel::Initial:
            return wolfssl_encryption_initial;
        case EncryptionLevel::EarlyData:
            return wolfssl_encryption_early_data;
        case EncryptionLevel::Handshake:
            return wolfssl_encryption_handshake;
        case EncryptionLevel::Application:
        default:
            return wolfssl_encryption_application;
    }
}

inline auto from_wolf_level(WOLFSSL_ENCRYPTION_LEVEL level) -> EncryptionLevel {
    switch (level) {
        case wolfssl_encryption_initial:
            return EncryptionLevel::Initial;
        case wolfssl_encryption_early_data:
            return EncryptionLevel::EarlyData;
        case wolfssl_encryption_handshake:
            return EncryptionLevel::Handshake;
        case wolfssl_encryption_application:
        default:
            return EncryptionLevel::Application;
    }
}
#endif

// Feed decrypted CRYPTO bytes into a test client SSL*. OpenSSL buffers
// them for the pull callback; wolfSSL takes them via provide_quic_data.
inline auto client_feed_crypto(SSL* ssl, EncryptionLevel level, std::span<const uint8_t> data,
                               std::array<std::vector<uint8_t>, 4>* openssl_in) -> void {
    if (data.empty()) {
        return;
    }
#if defined(SPAZNET_TLS_OPENSSL)
    (void)ssl;
    (*openssl_in)[level_index(level)].insert((*openssl_in)[level_index(level)].end(), data.begin(),
                                             data.end());
#else
    (void)openssl_in;
    SSL_provide_quic_data(ssl, to_wolf_level(level), data.data(), data.size());
#endif
}

inline auto client_poll_peer_tp(SSL* ssl, std::vector<uint8_t>& peer_tp_wire, bool& got_peer_tp)
    -> void {
#if defined(SPAZNET_TLS_WOLFSSL)
    if (got_peer_tp) {
        return;
    }
    const uint8_t* params = nullptr;
    std::size_t len = 0;
    SSL_get_peer_quic_transport_params(ssl, &params, &len);
    if (params != nullptr && len > 0) {
        peer_tp_wire.assign(params, params + len);
        got_peer_tp = true;
    }
#else
    (void)ssl;
    (void)peer_tp_wire;
    (void)got_peer_tp;
#endif
}

// Minimal TLS-facing state shared by in-process QUIC test clients.
// Concrete QuicClient types embed or inherit this and wire packet crypto
// on top.
struct TlsClientSide {
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    std::array<std::vector<uint8_t>, 4> tls_out{};
#if defined(SPAZNET_TLS_OPENSSL)
    std::array<std::vector<uint8_t>, 4> tls_in{};
    std::array<std::size_t, 4> tls_in_cursor{0, 0, 0, 0};
    uint32_t send_level{OSSL_RECORD_PROTECTION_LEVEL_NONE};
#else
    // wolfSSL passes the level into add_handshake_data; unused mirror.
    std::size_t send_level{0};
#endif
    std::vector<uint8_t> own_tp_wire{};
    std::vector<uint8_t> peer_tp_wire{};
    bool got_peer_tp{false};
    uint8_t alert_code{0};

    ~TlsClientSide() {
        if (ssl != nullptr) SSL_free(ssl);
        if (ctx != nullptr) SSL_CTX_free(ctx);
        ssl = nullptr;
        ctx = nullptr;
    }

    TlsClientSide() = default;
    TlsClientSide(const TlsClientSide&) = delete;
    auto operator=(const TlsClientSide&) -> TlsClientSide& = delete;
};

#if defined(SPAZNET_TLS_OPENSSL)
inline auto ossl_level_to_idx(uint32_t l) -> std::size_t {
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

inline auto ossl_idx_to_level(std::size_t i) -> uint32_t {
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

inline auto cb_send(SSL*, const unsigned char* buf, size_t buf_len, size_t* consumed, void* arg)
    -> int {
    auto* c = static_cast<TlsClientSide*>(arg);
    auto idx = ossl_level_to_idx(c->send_level);
    c->tls_out[idx].insert(c->tls_out[idx].end(), buf, buf + buf_len);
    *consumed = buf_len;
    return 1;
}

inline auto cb_recv(SSL*, const unsigned char** buf, size_t* bytes_read, void* arg) -> int {
    auto* c = static_cast<TlsClientSide*>(arg);
    for (std::size_t i = 0; i < 4; ++i) {
        if (c->tls_in_cursor[i] < c->tls_in[i].size()) {
            *buf = c->tls_in[i].data() + c->tls_in_cursor[i];
            *bytes_read = c->tls_in[i].size() - c->tls_in_cursor[i];
            return 1;
        }
    }
    *buf = nullptr;
    *bytes_read = 0;
    return 1;
}

inline auto cb_release(SSL*, size_t bytes_read, void* arg) -> int {
    auto* c = static_cast<TlsClientSide*>(arg);
    for (std::size_t i = 0; i < 4; ++i) {
        if (c->tls_in_cursor[i] < c->tls_in[i].size()) {
            c->tls_in_cursor[i] +=
                std::min(bytes_read, c->tls_in[i].size() - c->tls_in_cursor[i]);
            return 1;
        }
    }
    return 1;
}

inline auto cb_yield(SSL*, uint32_t prot_level, int /*direction*/, const unsigned char* /*secret*/,
                     size_t /*secret_len*/, void* arg) -> int {
    auto* c = static_cast<TlsClientSide*>(arg);
    c->send_level = prot_level;
    return 1;
}

inline auto cb_got_tp(SSL*, const unsigned char* params, size_t params_len, void* arg) -> int {
    auto* c = static_cast<TlsClientSide*>(arg);
    c->peer_tp_wire.assign(params, params + params_len);
    c->got_peer_tp = true;
    return 1;
}

inline auto cb_alert(SSL*, unsigned char alert_code, void* arg) -> int {
    auto* c = static_cast<TlsClientSide*>(arg);
    c->alert_code = alert_code;
    return 1;
}

inline auto client_dispatch() -> const OSSL_DISPATCH* {
    static const OSSL_DISPATCH t[] = {
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND, reinterpret_cast<void (*)()>(cb_send)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD, reinterpret_cast<void (*)()>(cb_recv)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD, reinterpret_cast<void (*)()>(cb_release)},
        {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET, reinterpret_cast<void (*)()>(cb_yield)},
        {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS, reinterpret_cast<void (*)()>(cb_got_tp)},
        {OSSL_FUNC_SSL_QUIC_TLS_ALERT, reinterpret_cast<void (*)()>(cb_alert)},
        {0, nullptr},
    };
    return t;
}
#else
inline auto wolf_set_secrets(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL /*level*/,
                             const uint8_t* /*read_secret*/, const uint8_t* /*write_secret*/,
                             size_t /*secret_len*/) -> int {
    (void)ssl;
    return 1;
}

inline auto wolf_add_hs(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level, const uint8_t* data,
                        size_t len) -> int {
    auto* c = static_cast<TlsClientSide*>(SSL_get_app_data(ssl));
    if (c == nullptr) {
        return 0;
    }
    auto idx = static_cast<std::size_t>(from_wolf_level(level));
    c->tls_out[idx].insert(c->tls_out[idx].end(), data, data + len);
    c->send_level = idx;
    return 1;
}

inline auto wolf_flush(WOLFSSL* /*ssl*/) -> int {
    return 1;
}

inline auto wolf_alert(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL /*level*/, uint8_t alert) -> int {
    auto* c = static_cast<TlsClientSide*>(SSL_get_app_data(ssl));
    if (c == nullptr) {
        return 0;
    }
    c->alert_code = alert;
    return 1;
}

inline auto client_quic_method() -> const WOLFSSL_QUIC_METHOD* {
    static const WOLFSSL_QUIC_METHOD m = {
        wolf_set_secrets,
        wolf_add_hs,
        wolf_flush,
        wolf_alert,
    };
    return &m;
}
#endif

// Secret callback variant used by full-packet QuicClients that derive
// PacketKeys when secrets arrive. `Client` must provide:
//   SSL* ssl; array<PacketKeys,4> send_keys/recv_keys; array<bool,4> send_ready/recv_ready;
//   array<Aead,4> aead; and for OpenSSL: uint32_t send_level; tls_out/tls_in buffers
//   matching TlsClientSide layout for the OpenSSL path.
//
// For wolfSSL, secrets arrive via set_encryption_secrets on the method
// installed by install_packet_client_quic below.

template <typename Client>
inline auto apply_secret(Client* c, EncryptionLevel level, int direction,
                         std::span<const uint8_t> secret) -> void {
    const auto i = level_index(level);
    c->aead[i] = aead_from_ssl(c->ssl);
    if (direction == 0) {
        c->recv_keys[i] = derive_packet_keys(c->aead[i], secret);
        c->recv_ready[i] = true;
        if constexpr (requires { c->recv_secret[i]; }) {
            c->recv_secret[i].assign(secret.begin(), secret.end());
        }
    } else {
        c->send_keys[i] = derive_packet_keys(c->aead[i], secret);
        c->send_ready[i] = true;
        if constexpr (requires { c->send_secret[i]; }) {
            c->send_secret[i].assign(secret.begin(), secret.end());
        }
    }
}

#if defined(SPAZNET_TLS_OPENSSL)
template <typename Client>
inline auto pkt_cb_send(SSL*, const unsigned char* buf, size_t len, size_t* consumed, void* arg)
    -> int {
    auto* c = static_cast<Client*>(arg);
    c->tls_out[c->send_level].insert(c->tls_out[c->send_level].end(), buf, buf + len);
    *consumed = len;
    return 1;
}

template <typename Client>
inline auto pkt_cb_recv(SSL*, const unsigned char** buf, size_t* br, void* arg) -> int {
    auto* c = static_cast<Client*>(arg);
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

template <typename Client>
inline auto pkt_cb_release(SSL*, size_t br, void* arg) -> int {
    auto* c = static_cast<Client*>(arg);
    for (std::size_t i = 0; i < 4; ++i) {
        if (c->tls_in_cursor[i] < c->tls_in[i].size()) {
            c->tls_in_cursor[i] += std::min(br, c->tls_in[i].size() - c->tls_in_cursor[i]);
            return 1;
        }
    }
    return 1;
}

template <typename Client>
inline auto pkt_cb_yield(SSL*, uint32_t prot_level, int direction, const unsigned char* secret,
                         size_t secret_len, void* arg) -> int {
    auto* c = static_cast<Client*>(arg);
    c->send_level = prot_level;
    EncryptionLevel level = static_cast<EncryptionLevel>(ossl_level_to_idx(prot_level));
    apply_secret(c, level, direction, {secret, secret_len});
    return 1;
}

template <typename Client>
inline auto pkt_cb_got_tp(SSL*, const unsigned char* p, size_t plen, void* arg) -> int {
    auto* c = static_cast<Client*>(arg);
    if constexpr (requires {
                      c->peer_tp_wire;
                      c->got_peer_tp;
                  }) {
        c->peer_tp_wire.assign(p, p + plen);
        c->got_peer_tp = true;
    } else {
        (void)p;
        (void)plen;
    }
    return 1;
}

template <typename Client>
inline auto pkt_cb_alert(SSL*, unsigned char, void*) -> int {
    return 1;
}

template <typename Client>
inline auto packet_client_dispatch() -> const OSSL_DISPATCH* {
    static const OSSL_DISPATCH t[] = {
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND, reinterpret_cast<void (*)()>(pkt_cb_send<Client>)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD,
         reinterpret_cast<void (*)()>(pkt_cb_recv<Client>)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD,
         reinterpret_cast<void (*)()>(pkt_cb_release<Client>)},
        {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET, reinterpret_cast<void (*)()>(pkt_cb_yield<Client>)},
        {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS,
         reinterpret_cast<void (*)()>(pkt_cb_got_tp<Client>)},
        {OSSL_FUNC_SSL_QUIC_TLS_ALERT, reinterpret_cast<void (*)()>(pkt_cb_alert<Client>)},
        {0, nullptr}};
    return t;
}
#else
template <typename Client>
inline auto pkt_wolf_secrets(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level,
                             const uint8_t* read_secret, const uint8_t* write_secret,
                             size_t secret_len) -> int {
    auto* c = static_cast<Client*>(SSL_get_app_data(ssl));
    if (c == nullptr) {
        return 0;
    }
    EncryptionLevel lvl = from_wolf_level(level);
    if (read_secret != nullptr) {
        apply_secret(c, lvl, 0, {read_secret, secret_len});
    }
    if (write_secret != nullptr) {
        apply_secret(c, lvl, 1, {write_secret, secret_len});
    }
    return 1;
}

template <typename Client>
inline auto pkt_wolf_add_hs(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level, const uint8_t* data,
                            size_t len) -> int {
    auto* c = static_cast<Client*>(SSL_get_app_data(ssl));
    if (c == nullptr) {
        return 0;
    }
    auto idx = static_cast<std::size_t>(from_wolf_level(level));
    c->tls_out[idx].insert(c->tls_out[idx].end(), data, data + len);
    return 1;
}

template <typename Client>
inline auto pkt_wolf_flush(WOLFSSL*) -> int {
    return 1;
}

template <typename Client>
inline auto pkt_wolf_alert(WOLFSSL*, WOLFSSL_ENCRYPTION_LEVEL, uint8_t) -> int {
    return 1;
}

template <typename Client>
inline auto packet_client_quic_method() -> const WOLFSSL_QUIC_METHOD* {
    static const WOLFSSL_QUIC_METHOD m = {
        pkt_wolf_secrets<Client>,
        pkt_wolf_add_hs<Client>,
        pkt_wolf_flush<Client>,
        pkt_wolf_alert<Client>,
    };
    return &m;
}
#endif

template <typename Client>
inline auto install_packet_client_quic(Client* c) -> bool {
#if defined(SPAZNET_TLS_OPENSSL)
    return SSL_set_quic_tls_cbs(c->ssl, packet_client_dispatch<Client>(), c) == 1 &&
           SSL_set_quic_tls_transport_params(c->ssl, c->own_tp_wire.data(),
                                             c->own_tp_wire.size()) == 1;
#else
    SSL_set_app_data(c->ssl, c);
    return SSL_set_quic_method(c->ssl, packet_client_quic_method<Client>()) == WOLFSSL_SUCCESS &&
           SSL_set_quic_transport_params(c->ssl, c->own_tp_wire.data(), c->own_tp_wire.size()) ==
               WOLFSSL_SUCCESS;
#endif
}

inline auto install_plain_client_quic(TlsClientSide* c) -> bool {
#if defined(SPAZNET_TLS_OPENSSL)
    return SSL_set_quic_tls_cbs(c->ssl, client_dispatch(), c) == 1 &&
           SSL_set_quic_tls_transport_params(c->ssl, c->own_tp_wire.data(),
                                             c->own_tp_wire.size()) == 1;
#else
    SSL_set_app_data(c->ssl, c);
    return SSL_set_quic_method(c->ssl, client_quic_method()) == WOLFSSL_SUCCESS &&
           SSL_set_quic_transport_params(c->ssl, c->own_tp_wire.data(), c->own_tp_wire.size()) ==
               WOLFSSL_SUCCESS;
#endif
}

inline auto make_client_ssl_ctx() -> SSL_CTX* {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    static const unsigned char alpn[] = {2, 'h', '3'};
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn));
    return ctx;
}

} // namespace test
} // namespace quic
} // namespace spaznet
