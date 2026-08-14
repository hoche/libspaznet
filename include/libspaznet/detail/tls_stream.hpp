#pragma once

// Internal TLS-over-TCP helpers shared by Socket and BufferedConnection.
// OpenSSL types stay out of public headers; only this detail header and
// tls_stream.cpp include <openssl/...>.
//
// Uses memory BIOs (not SSL_set_fd socket BIOs). OpenSSL never touches the
// socket; we pump ciphertext with recv/send. That keeps TLS compatible with
// IOCP's overlapped zero-byte probes on Windows, where mixing OpenSSL's
// socket BIO with WSARecv is undefined and stalls post-handshake reads
// (e.g. WSS echo after the 101 response).

#include <libspaznet/tls_config.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace spaznet::detail {

struct TlsIoResult {
    enum class Kind { Ok, WantRead, WantWrite, Closed, Error };
    Kind kind{Kind::Error};
    // Byte count when kind == Ok and the call was a read/write of data.
    // Handshake completion uses Ok with n == 0.
    std::size_t n{0};
};

class TlsContext {
  public:
    static auto create(const TlsConfig& cfg) -> std::shared_ptr<TlsContext>;

    TlsContext(const TlsContext&) = delete;
    auto operator=(const TlsContext&) -> TlsContext& = delete;
    ~TlsContext();

    // Opaque SSL_CTX*; only tls_stream.cpp casts it.
    [[nodiscard]] auto raw() const noexcept -> void* {
        return ctx_;
    }

  private:
    explicit TlsContext(void* ctx, std::vector<std::string> alpn,
                        std::vector<unsigned char> alpn_wire);
    void* ctx_{nullptr}; // SSL_CTX*
    // Kept alive for the ALPN select callback's arg pointer.
    std::vector<std::string> alpn_;
    // Length-prefixed wire form of alpn_; SSL_select_next_proto returns a
    // pointer into this buffer, so it must outlive every handshake.
    std::vector<unsigned char> alpn_wire_;
};

class TlsStream {
  public:
    static auto create_server(const std::shared_ptr<TlsContext>& ctx, int fd)
        -> std::unique_ptr<TlsStream>;

    TlsStream(const TlsStream&) = delete;
    auto operator=(const TlsStream&) -> TlsStream& = delete;
    ~TlsStream();

    // Drive SSL_accept until complete. Ok = done; Want* = re-arm that
    // direction; Error/Closed = give up and close the fd.
    auto handshake() -> TlsIoResult;

    auto read(void* buf, std::size_t len) -> TlsIoResult;
    auto write(const void* buf, std::size_t len) -> TlsIoResult;

    // Push any ciphertext sitting in the write BIO / pending_out_ to the
    // socket. Call from on_writable even when the app OutputBuffer is empty.
    auto flush() -> TlsIoResult;

    // True when ciphertext still needs a socket send (arm EVENT_WRITE).
    [[nodiscard]] auto wants_write() const noexcept -> bool;

    // Best-effort SSL_shutdown; safe to call more than once.
    void shutdown() noexcept;

    [[nodiscard]] auto handshake_done() const noexcept -> bool {
        return handshake_done_;
    }

    // Coroutine Socket path: reader and writer coroutines may call into the
    // same SSL* from different worker threads (HTTP/2 writer_loop, WS
    // WriteGate). Reactor BufferedConnection is IO-thread-affine and leaves
    // this off so the hot path takes no mutex.
    void enable_serialized_io() noexcept {
        serialize_io_ = true;
    }

    // Same-thread handoff from TlsHandshakeHandler::finish_ok into
    // BufferedConnection's constructor (factory runs synchronously on the
    // handshake's target loop). No mutex — thread_local only.
    static void stash_for_fd(int fd, std::unique_ptr<TlsStream> stream);
    static auto claim_for_fd(int fd) -> std::unique_ptr<TlsStream>;

  private:
    TlsStream(void* ssl, int fd);
    auto map_ssl_error(int ssl_ret) const -> TlsIoResult;
    auto feed_network() -> TlsIoResult;
    auto flush_network() -> TlsIoResult;
    auto drain_wbio_to_pending() -> void;

    // RAII: locks io_mu_ only when serialize_io_ is set.
    struct IoGate {
        explicit IoGate(const TlsStream& self) {
            if (self.serialize_io_) {
                lk_ = std::unique_lock<std::recursive_mutex>(self.io_mu_);
            }
        }
        std::unique_lock<std::recursive_mutex> lk_;
    };

    void* ssl_{nullptr}; // SSL*
    int fd_{-1};
    bool handshake_done_{false};
    bool shutdown_done_{false};
    bool serialize_io_{false};

    mutable std::recursive_mutex io_mu_;

    // Ciphertext waiting for a non-blocking send (partial send residue +
    // anything pulled from the write BIO that has not hit the wire yet).
    std::vector<uint8_t> pending_out_;
    std::size_t pending_out_off_{0};
};

} // namespace spaznet::detail
