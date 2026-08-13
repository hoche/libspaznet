#include <libspaznet/quic/tls.hpp>
#include <libspaznet/quic/detail/tls_compat.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace spaznet {
namespace quic {

namespace {

#if defined(SPAZNET_TLS_WOLFSSL)
struct WolfSslInit {
    WolfSslInit() {
        wolfSSL_Init();
    }
    ~WolfSslInit() {
        wolfSSL_Cleanup();
    }
};
const WolfSslInit kWolfSslInit;
#endif

constexpr std::size_t kNumLevels = 4;

auto level_to_index(EncryptionLevel l) -> std::size_t {
    return static_cast<std::size_t>(l);
}

#if defined(SPAZNET_TLS_OPENSSL)
auto from_ossl_level(uint32_t ossl) -> EncryptionLevel {
    switch (ossl) {
        case OSSL_RECORD_PROTECTION_LEVEL_NONE:
            return EncryptionLevel::Initial;
        case OSSL_RECORD_PROTECTION_LEVEL_EARLY:
            return EncryptionLevel::EarlyData;
        case OSSL_RECORD_PROTECTION_LEVEL_HANDSHAKE:
            return EncryptionLevel::Handshake;
        case OSSL_RECORD_PROTECTION_LEVEL_APPLICATION:
        default:
            return EncryptionLevel::Application;
    }
}

// Static thunks bridging OpenSSL's C dispatch table to our member fns.
auto cb_crypto_send(SSL* ssl, const unsigned char* buf, size_t buf_len, size_t* consumed,
                    void* arg) -> int {
    (void)ssl;
    auto* self = static_cast<TlsConnection*>(arg);
    std::size_t out_consumed = 0;
    int rc = self->on_crypto_send({buf, buf_len}, out_consumed);
    *consumed = out_consumed;
    return rc;
}

auto cb_crypto_recv_rcd(SSL* ssl, const unsigned char** buf, size_t* bytes_read, void* arg)
    -> int {
    (void)ssl;
    auto* self = static_cast<TlsConnection*>(arg);
    std::size_t br = 0;
    int rc = self->on_crypto_recv(buf, &br);
    *bytes_read = br;
    return rc;
}

auto cb_crypto_release_rcd(SSL* ssl, size_t bytes_read, void* arg) -> int {
    (void)ssl;
    auto* self = static_cast<TlsConnection*>(arg);
    return self->on_crypto_release(bytes_read);
}

auto cb_yield_secret(SSL* ssl, uint32_t prot_level, int direction,
                     const unsigned char* secret, size_t secret_len, void* arg) -> int {
    (void)ssl;
    auto* self = static_cast<TlsConnection*>(arg);
    return self->on_yield_secret(prot_level, direction, {secret, secret_len});
}

auto cb_got_transport_params(SSL* ssl, const unsigned char* params, size_t params_len, void* arg)
    -> int {
    (void)ssl;
    auto* self = static_cast<TlsConnection*>(arg);
    return self->on_got_transport_params({params, params_len});
}

auto cb_alert(SSL* ssl, unsigned char alert_code, void* arg) -> int {
    (void)ssl;
    auto* self = static_cast<TlsConnection*>(arg);
    return self->on_alert(alert_code);
}

auto build_dispatch_table() -> const OSSL_DISPATCH* {
    static const OSSL_DISPATCH table[] = {
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND,
         reinterpret_cast<void (*)()>(cb_crypto_send)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RECV_RCD,
         reinterpret_cast<void (*)()>(cb_crypto_recv_rcd)},
        {OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_RELEASE_RCD,
         reinterpret_cast<void (*)()>(cb_crypto_release_rcd)},
        {OSSL_FUNC_SSL_QUIC_TLS_YIELD_SECRET,
         reinterpret_cast<void (*)()>(cb_yield_secret)},
        {OSSL_FUNC_SSL_QUIC_TLS_GOT_TRANSPORT_PARAMS,
         reinterpret_cast<void (*)()>(cb_got_transport_params)},
        {OSSL_FUNC_SSL_QUIC_TLS_ALERT, reinterpret_cast<void (*)()>(cb_alert)},
        {0, nullptr},
    };
    return table;
}
#endif // SPAZNET_TLS_OPENSSL

#if defined(SPAZNET_TLS_WOLFSSL)
auto from_wolf_level(WOLFSSL_ENCRYPTION_LEVEL level) -> EncryptionLevel {
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

auto to_wolf_level(EncryptionLevel level) -> WOLFSSL_ENCRYPTION_LEVEL {
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

auto cb_set_encryption_secrets(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level,
                               const uint8_t* read_secret, const uint8_t* write_secret,
                               size_t secret_len) -> int {
    auto* self = static_cast<TlsConnection*>(SSL_get_app_data(ssl));
    if (self == nullptr) {
        return 0;
    }
    return self->on_set_encryption_secrets(from_wolf_level(level), read_secret, write_secret,
                                           secret_len);
}

auto cb_add_handshake_data(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL level, const uint8_t* data,
                           size_t len) -> int {
    auto* self = static_cast<TlsConnection*>(SSL_get_app_data(ssl));
    if (self == nullptr) {
        return 0;
    }
    return self->on_add_handshake_data(from_wolf_level(level), {data, len});
}

auto cb_flush_flight(WOLFSSL* /*ssl*/) -> int {
    return 1;
}

auto cb_send_alert(WOLFSSL* ssl, WOLFSSL_ENCRYPTION_LEVEL /*level*/, uint8_t alert) -> int {
    auto* self = static_cast<TlsConnection*>(SSL_get_app_data(ssl));
    if (self == nullptr) {
        return 0;
    }
    return self->on_alert(alert);
}

auto wolf_quic_method() -> const WOLFSSL_QUIC_METHOD* {
    static const WOLFSSL_QUIC_METHOD method = {
        cb_set_encryption_secrets,
        cb_add_handshake_data,
        cb_flush_flight,
        cb_send_alert,
    };
    return &method;
}
#endif // SPAZNET_TLS_WOLFSSL

// ALPN selection callback on the server side. We accept the first peer
// protocol that we also offer.
auto alpn_select_cb(SSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
                    const unsigned char* in, unsigned int inlen, void* arg) -> int {
    const auto* offered = static_cast<const std::vector<std::vector<uint8_t>>*>(arg);
    for (unsigned int i = 0; i + 1 <= inlen;) {
        unsigned int len = in[i];
        if (i + 1 + len > inlen) {
            break;
        }
        for (const auto& opt : *offered) {
            if (opt.size() == len && std::memcmp(opt.data(), in + i + 1, len) == 0) {
                *out = in + i + 1;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
        }
        i += 1 + len;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

} // namespace

auto TlsContext::make_server(const TlsServerConfig& cfg) -> std::shared_ptr<TlsContext> {
    auto out = std::shared_ptr<TlsContext>(new TlsContext);
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) {
        return nullptr;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    // Load cert chain.
    BIO* cbio = BIO_new_mem_buf(cfg.cert_pem.data(), static_cast<int>(cfg.cert_pem.size()));
    X509* cert = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr);
    if (cert == nullptr || SSL_CTX_use_certificate(ctx, cert) != 1) {
        if (cert != nullptr) X509_free(cert);
        BIO_free(cbio);
        SSL_CTX_free(ctx);
        return nullptr;
    }
    // Any intermediates after the leaf.
    X509* extra = nullptr;
    while ((extra = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr)) != nullptr) {
        if (SSL_CTX_add0_chain_cert(ctx, extra) != 1) {
            X509_free(extra);
            break;
        }
    }
    X509_free(cert);
    BIO_free(cbio);

    BIO* kbio = BIO_new_mem_buf(cfg.key_pem.data(), static_cast<int>(cfg.key_pem.size()));
    EVP_PKEY* key = PEM_read_bio_PrivateKey(kbio, nullptr, nullptr, nullptr);
    if (key == nullptr || SSL_CTX_use_PrivateKey(ctx, key) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        if (key != nullptr) EVP_PKEY_free(key);
        BIO_free(kbio);
        SSL_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY_free(key);
    BIO_free(kbio);

    // ALPN: keep offered protocols on TlsContext so the select callback
    // arg stays valid for the CTX lifetime (avoids SSL_CTX ex_data, which
    // wolfSSL's OpenSSL-compat layer does not always expose).
    if (!cfg.alpn.empty()) {
        out->alpn_offered_.clear();
        for (const auto& p : cfg.alpn) {
            out->alpn_offered_.emplace_back(p.begin(), p.end());
        }
        SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, &out->alpn_offered_);
    }

    out->ctx_ = ctx;
    return out;
}

TlsContext::~TlsContext() {
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
    }
}

TlsConnection::TlsConnection(std::shared_ptr<TlsContext> ctx,
                             std::span<const uint8_t> client_dcid, const TransportParameters& tp)
    : ctx_(std::move(ctx)) {
    ssl_ = SSL_new(ctx_->ssl_ctx());
    if (ssl_ == nullptr) {
        state_ = State::Failed;
        return;
    }
    SSL_set_accept_state(ssl_);

    own_tp_wire_ = encode_transport_params(tp);
#if defined(SPAZNET_TLS_OPENSSL)
    // Install the dispatch table first to put the SSL* in QUIC mode;
    // the transport-params call is only meaningful afterwards.
    if (SSL_set_quic_tls_cbs(ssl_, build_dispatch_table(), this) != 1) {
        state_ = State::Failed;
        return;
    }
    if (SSL_set_quic_tls_transport_params(ssl_, own_tp_wire_.data(), own_tp_wire_.size()) != 1) {
        state_ = State::Failed;
        return;
    }
#else
    SSL_set_app_data(ssl_, this);
    if (SSL_set_quic_method(ssl_, wolf_quic_method()) != WOLFSSL_SUCCESS) {
        state_ = State::Failed;
        return;
    }
    if (SSL_set_quic_transport_params(ssl_, own_tp_wire_.data(), own_tp_wire_.size()) !=
        WOLFSSL_SUCCESS) {
        state_ = State::Failed;
        return;
    }
#endif
    // client_dcid is informational here; the transport derives Initial
    // keys from it. We accept the parameter so future code (e.g. Retry)
    // can stash the OD-CID alongside this object.
    (void)client_dcid;
}

TlsConnection::~TlsConnection() {
    if (ssl_ != nullptr) {
        SSL_free(ssl_);
    }
}

auto TlsConnection::deliver_crypto(EncryptionLevel level, std::span<const uint8_t> data) -> void {
#if defined(SPAZNET_TLS_OPENSSL)
    auto& lvl = in_[level_to_index(level)];
    lvl.data.insert(lvl.data.end(), data.begin(), data.end());
#else
    if (ssl_ == nullptr || data.empty()) {
        return;
    }
    if (SSL_provide_quic_data(ssl_, to_wolf_level(level), data.data(), data.size()) !=
        WOLFSSL_SUCCESS) {
        state_ = State::Failed;
    }
#endif
}

auto TlsConnection::set_state_from_handshake_result(int rc) -> void {
    if (rc == 1) {
        state_ = State::Established;
        return;
    }
    int err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE
#if defined(SPAZNET_TLS_OPENSSL)
        || err == SSL_ERROR_WANT_RETRY_VERIFY || err == SSL_ERROR_WANT_CLIENT_HELLO_CB ||
        err == SSL_ERROR_WANT_X509_LOOKUP
#endif
    ) {
        state_ = State::Handshaking;
        return;
    }
    // Anything else is a fatal handshake failure. Stash the TLS error
    // code in alert_code_ so the caller can report it; the TLS alert
    // (if any) was already captured by on_alert.
    if (alert_code_ == 0) {
        alert_code_ = static_cast<uint8_t>(err);
    }
    state_ = State::Failed;
}

auto TlsConnection::refresh_negotiated_aead() -> void {
    const SSL_CIPHER* c = SSL_get_current_cipher(ssl_);
    if (c == nullptr) {
        return;
    }
#if defined(SPAZNET_TLS_OPENSSL)
    negotiated_aead_ =
        aead_from_tls_cipher_id(static_cast<uint16_t>(SSL_CIPHER_get_protocol_id(c)));
#else
    const char* name = SSL_CIPHER_get_name(c);
    if (name == nullptr) {
        return;
    }
    // wolfSSL uses names like "TLS13-AES128-GCM-SHA256".
    if (std::strstr(name, "CHACHA") != nullptr) {
        negotiated_aead_ = Aead::ChaCha20Poly1305;
    } else if (std::strstr(name, "AES256") != nullptr || std::strstr(name, "AES_256") != nullptr) {
        negotiated_aead_ = Aead::Aes256Gcm;
    } else {
        negotiated_aead_ = Aead::Aes128Gcm;
    }
#endif
}

auto TlsConnection::poll_peer_transport_params() -> void {
#if defined(SPAZNET_TLS_WOLFSSL)
    if (have_peer_tp_ || ssl_ == nullptr) {
        return;
    }
    const uint8_t* params = nullptr;
    std::size_t params_len = 0;
    SSL_get_peer_quic_transport_params(ssl_, &params, &params_len);
    if (params != nullptr && params_len > 0) {
        on_got_transport_params({params, params_len});
    }
#else
    (void)0;
#endif
}

auto TlsConnection::advance() -> State {
    if (state_ == State::Failed || ssl_ == nullptr) {
        return state_;
    }
    int rc = SSL_do_handshake(ssl_);
    set_state_from_handshake_result(rc);
    poll_peer_transport_params();

    if (state_ == State::Established) {
        refresh_negotiated_aead();
        poll_peer_transport_params();
    }
    return state_;
}

auto TlsConnection::out_crypto(EncryptionLevel level) -> std::vector<uint8_t>& {
    return out_[level_to_index(level)];
}

auto TlsConnection::consume_out_crypto(EncryptionLevel level, std::size_t n) -> void {
    auto& buf = out_[level_to_index(level)];
    n = std::min(n, buf.size());
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
}

auto TlsConnection::read_secret(EncryptionLevel level) const -> const std::vector<uint8_t>& {
    return read_secret_[level_to_index(level)];
}

auto TlsConnection::write_secret(EncryptionLevel level) const -> const std::vector<uint8_t>& {
    return write_secret_[level_to_index(level)];
}

auto TlsConnection::negotiated_alpn() const -> std::string {
    const unsigned char* p = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl_, &p, &len);
    if (p == nullptr || len == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(p), len);
}

#if defined(SPAZNET_TLS_OPENSSL)
auto TlsConnection::on_crypto_send(std::span<const uint8_t> data, std::size_t& consumed) -> int {
    out_[level_to_index(send_level_)].insert(out_[level_to_index(send_level_)].end(),
                                             data.begin(), data.end());
    consumed = data.size();
    return 1;
}

auto TlsConnection::on_crypto_recv(const uint8_t** buf, std::size_t* bytes_read) -> int {
    // Hand OpenSSL the next chunk of unread CRYPTO bytes for the lowest
    // level that has any. We pick the lowest available level so the
    // handshake makes monotonic progress.
    for (std::size_t i = 0; i < kNumLevels; ++i) {
        auto& lvl = in_[i];
        if (lvl.read_cursor < lvl.data.size()) {
            *buf = lvl.data.data() + lvl.read_cursor;
            *bytes_read = lvl.data.size() - lvl.read_cursor;
            return 1;
        }
    }
    *buf = nullptr;
    *bytes_read = 0;
    return 1;
}

auto TlsConnection::on_crypto_release(std::size_t bytes_read) -> int {
    // Advance the cursor on whichever level we last served from.
    for (std::size_t i = 0; i < kNumLevels; ++i) {
        auto& lvl = in_[i];
        if (lvl.read_cursor < lvl.data.size()) {
            lvl.read_cursor += std::min(bytes_read, lvl.data.size() - lvl.read_cursor);
            return 1;
        }
    }
    return 1;
}

auto TlsConnection::on_yield_secret(uint32_t prot_level, int direction,
                                    std::span<const uint8_t> secret) -> int {
    const EncryptionLevel level = from_ossl_level(prot_level);
    // direction: 0 = read (server reading client data), 1 = write (server
    // writing to client). The convention matches OpenSSL's prov ops.
    if (direction == 0) {
        read_secret_[level_to_index(level)].assign(secret.begin(), secret.end());
    } else {
        write_secret_[level_to_index(level)].assign(secret.begin(), secret.end());
    }
    refresh_negotiated_aead();
    // OpenSSL transitions us forward by handing higher-level secrets;
    // the next call to advance() will pick that up.
    send_level_ = level;
    return 1;
}
#endif // SPAZNET_TLS_OPENSSL

#if defined(SPAZNET_TLS_WOLFSSL)
auto TlsConnection::on_set_encryption_secrets(EncryptionLevel level, const uint8_t* read_secret,
                                              const uint8_t* write_secret, std::size_t secret_len)
    -> int {
    if (read_secret != nullptr) {
        read_secret_[level_to_index(level)].assign(read_secret, read_secret + secret_len);
    }
    if (write_secret != nullptr) {
        write_secret_[level_to_index(level)].assign(write_secret, write_secret + secret_len);
    }
    refresh_negotiated_aead();
    return 1;
}

auto TlsConnection::on_add_handshake_data(EncryptionLevel level, std::span<const uint8_t> data)
    -> int {
    out_[level_to_index(level)].insert(out_[level_to_index(level)].end(), data.begin(),
                                       data.end());
    return 1;
}
#endif // SPAZNET_TLS_WOLFSSL

auto TlsConnection::on_got_transport_params(std::span<const uint8_t> params) -> int {
    have_peer_tp_ = decode_transport_params(params, peer_tp_);
    return have_peer_tp_ ? 1 : 0;
}

auto TlsConnection::on_alert(uint8_t alert_code) -> int {
    alert_code_ = alert_code;
    state_ = State::Failed;
    return 1;
}

auto aead_from_tls_cipher_id(uint16_t cipher_id) -> Aead {
    // IANA TLS cipher-suite codepoints (RFC 8446 §B.4).
    switch (cipher_id) {
        case 0x1301: // TLS_AES_128_GCM_SHA256
            return Aead::Aes128Gcm;
        case 0x1302: // TLS_AES_256_GCM_SHA384
            return Aead::Aes256Gcm;
        case 0x1303: // TLS_CHACHA20_POLY1305_SHA256
            return Aead::ChaCha20Poly1305;
        default:
            return Aead::Aes128Gcm;
    }
}

} // namespace quic
} // namespace spaznet
