#include <libspaznet/detail/tls_stream.hpp>

#include <libspaznet/detail/socket_compat.hpp>

#include <openssl/bio.h>
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
    // TLS 1.3 NewSessionTicket is emitted during a later SSL_read and yields
    // WANT_WRITE with an empty app OutputBuffer. That path is easy to mishandle
    // on non-blocking Windows/IOCP; we don't resume TCP-TLS sessions anyway.
    SSL_CTX_set_num_tickets(ctx, 0);

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

    // Memory BIOs: OpenSSL never calls recv/send. We pump ciphertext so this
    // works with epoll/kqueue and with IOCP overlapped probes alike.
    BIO* rbio = BIO_new(BIO_s_mem());
    BIO* wbio = BIO_new(BIO_s_mem());
    if (rbio == nullptr || wbio == nullptr) {
        BIO_free(rbio);
        BIO_free(wbio);
        SSL_free(ssl);
        throw std::runtime_error("BIO_new(BIO_s_mem) failed");
    }
    // Empty mem BIO would otherwise look like EOF to SSL_read.
    BIO_set_mem_eof_return(rbio, -1);
    BIO_set_mem_eof_return(wbio, -1);
    SSL_set_bio(ssl, rbio, wbio); // ssl owns both
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
    if (err == SSL_ERROR_SYSCALL) {
        if (ssl_ret == 0) {
            return {TlsIoResult::Kind::Closed, 0};
        }
        // With memory BIOs this is unexpected; treat retryable socket errno
        // as WantRead so a mis-mapped path still re-arms rather than killing
        // the connection under Windows errno=0 quirks.
        const int sock_err = detail::last_socket_error();
        if (detail::is_retryable_socket_error(sock_err) || sock_err == 0) {
            const int want = SSL_want(ssl);
            if (want == SSL_WRITING) {
                return {TlsIoResult::Kind::WantWrite, 0};
            }
            return {TlsIoResult::Kind::WantRead, 0};
        }
    }
    return {TlsIoResult::Kind::Error, 0};
}

void TlsStream::drain_wbio_to_pending() {
    auto* ssl = static_cast<SSL*>(ssl_);
    BIO* wbio = SSL_get_wbio(ssl);
    if (wbio == nullptr) {
        return;
    }
    uint8_t chunk[16 * 1024];
    for (;;) {
        int n = BIO_read(wbio, chunk, static_cast<int>(sizeof(chunk)));
        if (n > 0) {
            pending_out_.insert(pending_out_.end(), chunk, chunk + n);
            continue;
        }
        break;
    }
}

auto TlsStream::flush_network() -> TlsIoResult {
    drain_wbio_to_pending();
    while (pending_out_off_ < pending_out_.size()) {
        ssize_t n = detail::socket_send(fd_, pending_out_.data() + pending_out_off_,
                                        pending_out_.size() - pending_out_off_, MSG_NOSIGNAL);
        if (n > 0) {
            pending_out_off_ += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            return {TlsIoResult::Kind::WantWrite, 0};
        }
        const int err = detail::last_socket_error();
        if (detail::is_retryable_socket_error(err)) {
            return {TlsIoResult::Kind::WantWrite, 0};
        }
        return {TlsIoResult::Kind::Error, 0};
    }
    pending_out_.clear();
    pending_out_off_ = 0;
    return {TlsIoResult::Kind::Ok, 0};
}

auto TlsStream::feed_network() -> TlsIoResult {
    auto* ssl = static_cast<SSL*>(ssl_);
    BIO* rbio = SSL_get_rbio(ssl);
    if (rbio == nullptr) {
        return {TlsIoResult::Kind::Error, 0};
    }
    uint8_t chunk[16 * 1024];
    ssize_t n = detail::socket_recv(fd_, chunk, sizeof(chunk), 0);
    if (n > 0) {
        int w = BIO_write(rbio, chunk, static_cast<int>(n));
        if (w <= 0) {
            return {TlsIoResult::Kind::Error, 0};
        }
        // Rare: mem BIO rejected part of the write. Put the rest back by
        // failing hard rather than dropping ciphertext.
        if (w != static_cast<int>(n)) {
            return {TlsIoResult::Kind::Error, 0};
        }
        return {TlsIoResult::Kind::Ok, static_cast<std::size_t>(n)};
    }
    if (n == 0) {
        return {TlsIoResult::Kind::Closed, 0};
    }
    const int err = detail::last_socket_error();
    if (detail::is_retryable_socket_error(err)) {
        return {TlsIoResult::Kind::WantRead, 0};
    }
    return {TlsIoResult::Kind::Error, 0};
}

auto TlsStream::wants_write() const noexcept -> bool {
    std::lock_guard<std::recursive_mutex> lock(io_mu_);
    if (pending_out_off_ < pending_out_.size()) {
        return true;
    }
    auto* ssl = static_cast<SSL*>(ssl_);
    if (ssl == nullptr) {
        return false;
    }
    BIO* wbio = SSL_get_wbio(ssl);
    return wbio != nullptr && BIO_ctrl_pending(wbio) > 0;
}

auto TlsStream::flush() -> TlsIoResult {
    std::lock_guard<std::recursive_mutex> lock(io_mu_);
    return flush_network();
}

auto TlsStream::handshake() -> TlsIoResult {
    std::lock_guard<std::recursive_mutex> lock(io_mu_);
    if (handshake_done_) {
        // Final flight may still be in pending_out_ after SSL_accept
        // returned 1 with WantWrite; keep flushing until the wire is clear.
        auto flushed = flush_network();
        if (flushed.kind != TlsIoResult::Kind::Ok) {
            return flushed;
        }
        return {TlsIoResult::Kind::Ok, 0};
    }
    auto* ssl = static_cast<SSL*>(ssl_);
    for (;;) {
        ERR_clear_error();
        int r = SSL_accept(ssl);
        auto flushed = flush_network();
        if (flushed.kind == TlsIoResult::Kind::Error ||
            flushed.kind == TlsIoResult::Kind::Closed) {
            return flushed;
        }
        if (r == 1) {
            handshake_done_ = true;
            // Handshake records may still need a socket write.
            if (flushed.kind == TlsIoResult::Kind::WantWrite ||
                pending_out_off_ < pending_out_.size() ||
                (SSL_get_wbio(ssl) != nullptr && BIO_ctrl_pending(SSL_get_wbio(ssl)) > 0)) {
                return {TlsIoResult::Kind::WantWrite, 0};
            }
            return {TlsIoResult::Kind::Ok, 0};
        }
        auto err = map_ssl_error(r);
        if (err.kind == TlsIoResult::Kind::WantRead) {
            auto fed = feed_network();
            if (fed.kind == TlsIoResult::Kind::Ok) {
                continue;
            }
            return fed;
        }
        if (err.kind == TlsIoResult::Kind::WantWrite) {
            if (flushed.kind == TlsIoResult::Kind::WantWrite ||
                pending_out_off_ < pending_out_.size()) {
                return {TlsIoResult::Kind::WantWrite, 0};
            }
            continue;
        }
        return err;
    }
}

auto TlsStream::read(void* buf, std::size_t len) -> TlsIoResult {
    std::lock_guard<std::recursive_mutex> lock(io_mu_);
    if (!handshake_done_) {
        return {TlsIoResult::Kind::Error, 0};
    }
    if (len == 0) {
        return {TlsIoResult::Kind::Ok, 0};
    }
    auto* ssl = static_cast<SSL*>(ssl_);
    for (;;) {
        ERR_clear_error();
        int r = SSL_read(ssl, buf, static_cast<int>(len));
        auto flushed = flush_network();
        if (flushed.kind == TlsIoResult::Kind::Error ||
            flushed.kind == TlsIoResult::Kind::Closed) {
            return flushed;
        }
        if (r > 0) {
            // App data ready; if tickets/KeyUpdate left ciphertext, caller
            // must arm WRITE via wants_write().
            return {TlsIoResult::Kind::Ok, static_cast<std::size_t>(r)};
        }
        auto err = map_ssl_error(r);
        if (err.kind == TlsIoResult::Kind::WantRead) {
            auto fed = feed_network();
            if (fed.kind == TlsIoResult::Kind::Ok) {
                continue;
            }
            // Prefer reporting WantWrite if we also have outbound ciphertext
            // (e.g. post-handshake message) so the reactor arms both.
            if (fed.kind == TlsIoResult::Kind::WantRead &&
                (flushed.kind == TlsIoResult::Kind::WantWrite ||
                 pending_out_off_ < pending_out_.size() ||
                 (SSL_get_wbio(ssl) != nullptr && BIO_ctrl_pending(SSL_get_wbio(ssl)) > 0))) {
                return {TlsIoResult::Kind::WantWrite, 0};
            }
            return fed;
        }
        if (err.kind == TlsIoResult::Kind::WantWrite) {
            if (flushed.kind == TlsIoResult::Kind::WantWrite ||
                pending_out_off_ < pending_out_.size()) {
                return {TlsIoResult::Kind::WantWrite, 0};
            }
            continue;
        }
        return err;
    }
}

auto TlsStream::write(const void* buf, std::size_t len) -> TlsIoResult {
    std::lock_guard<std::recursive_mutex> lock(io_mu_);
    if (!handshake_done_) {
        return {TlsIoResult::Kind::Error, 0};
    }
    if (len == 0) {
        return flush_network();
    }
    // Finish any prior ciphertext before accepting more app data so a
    // WantWrite here never implies "retry the same SSL_write".
    if (pending_out_off_ < pending_out_.size() ||
        (SSL_get_wbio(static_cast<SSL*>(ssl_)) != nullptr &&
         BIO_ctrl_pending(SSL_get_wbio(static_cast<SSL*>(ssl_))) > 0)) {
        auto flushed = flush_network();
        if (flushed.kind != TlsIoResult::Kind::Ok) {
            return flushed;
        }
    }

    auto* ssl = static_cast<SSL*>(ssl_);
    ERR_clear_error();
    int r = SSL_write(ssl, buf, static_cast<int>(len));
    auto flushed = flush_network();
    if (flushed.kind == TlsIoResult::Kind::Error || flushed.kind == TlsIoResult::Kind::Closed) {
        return flushed;
    }
    if (r > 0) {
        // App bytes accepted into SSL. Ciphertext residue is drained via
        // wants_write()/flush() or the next write/read.
        return {TlsIoResult::Kind::Ok, static_cast<std::size_t>(r)};
    }
    auto err = map_ssl_error(r);
    if (err.kind == TlsIoResult::Kind::WantRead) {
        auto fed = feed_network();
        if (fed.kind == TlsIoResult::Kind::Ok) {
            // Retry once with the same buffer; outer try_flush loops.
            return {TlsIoResult::Kind::WantRead, 0};
        }
        return fed;
    }
    if (err.kind == TlsIoResult::Kind::WantWrite) {
        return {TlsIoResult::Kind::WantWrite, 0};
    }
    return err;
}

void TlsStream::shutdown() noexcept {
    std::lock_guard<std::recursive_mutex> lock(io_mu_);
    if (shutdown_done_ || ssl_ == nullptr) {
        return;
    }
    shutdown_done_ = true;
    // Best-effort; ignore WANT_*/errors on teardown.
    SSL_shutdown(static_cast<SSL*>(ssl_));
    (void)flush_network();
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
