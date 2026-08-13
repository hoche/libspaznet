#include <libspaznet/detail/tls_stream.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <stdexcept>
#include <utility>

namespace spaznet::detail {

namespace {

auto alpn_select_cb(SSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
                    const unsigned char* in, unsigned int inlen, void* arg) -> int {
    auto* wire = static_cast<std::vector<unsigned char>*>(arg);
    if (wire == nullptr || wire->empty()) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    unsigned char* selected = nullptr;
    unsigned char selected_len = 0;
    int r = SSL_select_next_proto(&selected, &selected_len, wire->data(),
                                  static_cast<unsigned int>(wire->size()), in, inlen);
    if (r == OPENSSL_NPN_NEGOTIATED && selected != nullptr) {
        *out = selected;
        *outlen = selected_len;
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

auto build_alpn_wire(const std::vector<std::string>& alpn) -> std::vector<unsigned char> {
    std::vector<unsigned char> wire;
    for (const auto& p : alpn) {
        if (p.empty() || p.size() > 255) {
            continue;
        }
        wire.push_back(static_cast<unsigned char>(p.size()));
        wire.insert(wire.end(), p.begin(), p.end());
    }
    return wire;
}

} // namespace

std::mutex TlsStream::stash_mu_;
std::unordered_map<int, std::unique_ptr<TlsStream>> TlsStream::stash_;

TlsContext::TlsContext(void* ctx, std::vector<std::string> alpn,
                       std::vector<unsigned char> alpn_wire)
    : ctx_(ctx), alpn_(std::move(alpn)), alpn_wire_(std::move(alpn_wire)) {}

TlsContext::~TlsContext() {
    if (ctx_ != nullptr) {
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
        ctx_ = nullptr;
    }
}

auto TlsContext::create(const TlsConfig& cfg) -> std::shared_ptr<TlsContext> {
    if (cfg.cert_pem.empty() || cfg.key_pem.empty()) {
        throw std::runtime_error("TlsConfig: cert_pem and key_pem are required");
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) {
        throw std::runtime_error("SSL_CTX_new failed");
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    BIO* cbio = BIO_new_mem_buf(cfg.cert_pem.data(), static_cast<int>(cfg.cert_pem.size()));
    if (cbio == nullptr) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("BIO_new_mem_buf(cert) failed");
    }
    X509* cert = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr);
    if (cert == nullptr) {
        BIO_free(cbio);
        SSL_CTX_free(ctx);
        throw std::runtime_error("PEM_read_bio_X509 failed");
    }
    if (SSL_CTX_use_certificate(ctx, cert) != 1) {
        X509_free(cert);
        BIO_free(cbio);
        SSL_CTX_free(ctx);
        throw std::runtime_error("SSL_CTX_use_certificate failed");
    }
    X509_free(cert);
    // Optional intermediates in the same PEM blob.
    X509* extra = nullptr;
    while ((extra = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr)) != nullptr) {
        if (SSL_CTX_add_extra_chain_cert(ctx, extra) != 1) {
            X509_free(extra);
            BIO_free(cbio);
            SSL_CTX_free(ctx);
            throw std::runtime_error("SSL_CTX_add_extra_chain_cert failed");
        }
        // ctx owns extra on success.
    }
    BIO_free(cbio);

    BIO* kbio = BIO_new_mem_buf(cfg.key_pem.data(), static_cast<int>(cfg.key_pem.size()));
    if (kbio == nullptr) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("BIO_new_mem_buf(key) failed");
    }
    EVP_PKEY* key = PEM_read_bio_PrivateKey(kbio, nullptr, nullptr, nullptr);
    BIO_free(kbio);
    if (key == nullptr) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("PEM_read_bio_PrivateKey failed");
    }
    if (SSL_CTX_use_PrivateKey(ctx, key) != 1) {
        EVP_PKEY_free(key);
        SSL_CTX_free(ctx);
        throw std::runtime_error("SSL_CTX_use_PrivateKey failed");
    }
    EVP_PKEY_free(key);
    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("SSL_CTX_check_private_key failed");
    }

    auto wire = build_alpn_wire(cfg.alpn);
    auto holder =
        std::shared_ptr<TlsContext>(new TlsContext(ctx, cfg.alpn, std::move(wire)));
    if (!holder->alpn_wire_.empty()) {
        SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, &holder->alpn_wire_);
    }
    return holder;
}

TlsStream::TlsStream(void* ssl, int fd) : ssl_(ssl), fd_(fd) {}

TlsStream::~TlsStream() {
    shutdown();
    if (ssl_ != nullptr) {
        SSL_free(static_cast<SSL*>(ssl_));
        ssl_ = nullptr;
    }
}

auto TlsStream::create_server(const std::shared_ptr<TlsContext>& ctx, int fd)
    -> std::unique_ptr<TlsStream> {
    if (!ctx || ctx->raw() == nullptr || fd < 0) {
        throw std::runtime_error("TlsStream::create_server: invalid args");
    }
    SSL* ssl = SSL_new(static_cast<SSL_CTX*>(ctx->raw()));
    if (ssl == nullptr) {
        throw std::runtime_error("SSL_new failed");
    }
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    return std::unique_ptr<TlsStream>(new TlsStream(ssl, fd));
}

auto TlsStream::map_ssl_error(int ssl_ret) const -> TlsIoResult {
    auto* ssl = static_cast<SSL*>(ssl_);
    int err = SSL_get_error(ssl, ssl_ret);
    if (err == SSL_ERROR_WANT_READ) {
        return {TlsIoResult::Kind::WantRead, 0};
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        return {TlsIoResult::Kind::WantWrite, 0};
    }
    if (err == SSL_ERROR_ZERO_RETURN) {
        return {TlsIoResult::Kind::Closed, 0};
    }
    if (err == SSL_ERROR_SYSCALL && ssl_ret == 0) {
        return {TlsIoResult::Kind::Closed, 0};
    }
    return {TlsIoResult::Kind::Error, 0};
}

auto TlsStream::handshake() -> TlsIoResult {
    if (handshake_done_) {
        return {TlsIoResult::Kind::Ok, 0};
    }
    auto* ssl = static_cast<SSL*>(ssl_);
    int r = SSL_accept(ssl);
    if (r == 1) {
        handshake_done_ = true;
        return {TlsIoResult::Kind::Ok, 0};
    }
    return map_ssl_error(r);
}

auto TlsStream::read(void* buf, std::size_t len) -> TlsIoResult {
    if (!handshake_done_) {
        return {TlsIoResult::Kind::Error, 0};
    }
    if (len == 0) {
        return {TlsIoResult::Kind::Ok, 0};
    }
    auto* ssl = static_cast<SSL*>(ssl_);
    int r = SSL_read(ssl, buf, static_cast<int>(len));
    if (r > 0) {
        return {TlsIoResult::Kind::Ok, static_cast<std::size_t>(r)};
    }
    if (r == 0) {
        return map_ssl_error(r);
    }
    return map_ssl_error(r);
}

auto TlsStream::write(const void* buf, std::size_t len) -> TlsIoResult {
    if (!handshake_done_) {
        return {TlsIoResult::Kind::Error, 0};
    }
    if (len == 0) {
        return {TlsIoResult::Kind::Ok, 0};
    }
    auto* ssl = static_cast<SSL*>(ssl_);
    int r = SSL_write(ssl, buf, static_cast<int>(len));
    if (r > 0) {
        return {TlsIoResult::Kind::Ok, static_cast<std::size_t>(r)};
    }
    return map_ssl_error(r);
}

void TlsStream::shutdown() noexcept {
    if (shutdown_done_ || ssl_ == nullptr) {
        return;
    }
    shutdown_done_ = true;
    // Best-effort; ignore WANT_*/errors on teardown.
    SSL_shutdown(static_cast<SSL*>(ssl_));
}

void TlsStream::stash_for_fd(int fd, std::unique_ptr<TlsStream> stream) {
    std::lock_guard<std::mutex> lock(stash_mu_);
    stash_[fd] = std::move(stream);
}

auto TlsStream::claim_for_fd(int fd) -> std::unique_ptr<TlsStream> {
    std::lock_guard<std::mutex> lock(stash_mu_);
    auto it = stash_.find(fd);
    if (it == stash_.end()) {
        return nullptr;
    }
    auto out = std::move(it->second);
    stash_.erase(it);
    return out;
}

} // namespace spaznet::detail
