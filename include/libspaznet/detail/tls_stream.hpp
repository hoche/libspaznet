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
#include <unordered_map>
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

    // Factory path: Server stashes a completed (or ready-to-claim) stream
    // keyed by fd; BufferedConnection claims it in its constructor so
    // ConnectionFactory implementations need no TLS awareness.
    static void stash_for_fd(int fd, std::unique_ptr<TlsStream> stream);
    static auto claim_for_fd(int fd) -> std::unique_ptr<TlsStream>;

  private:
    TlsStream(void* ssl, int fd);
    auto map_ssl_error(int ssl_ret) const -> TlsIoResult;
    auto feed_network() -> TlsIoResult;
    auto flush_network() -> TlsIoResult;
    auto drain_wbio_to_pending() -> void;

    void* ssl_{nullptr}; // SSL*
    int fd_{-1};
    bool handshake_done_{false};
    bool shutdown_done_{false};

    // HTTP/2 (and any future protocol) may run a reader coroutine and a
    // writer coroutine on the same Socket concurrently. OpenSSL SSL*
    // objects are not thread-safe; memory-BIO pending_out_ isn't either.
    // recursive: write()/flush() callers also check wants_write().
    mutable std::recursive_mutex io_mu_;

    // Ciphertext waiting for a non-blocking send (partial send residue +
    // anything pulled from the write BIO that has not hit the wire yet).
    std::vector<uint8_t> pending_out_;
    std::size_t pending_out_off_{0};

    static std::mutex stash_mu_;
    static std::unordered_map<int, std::unique_ptr<TlsStream>> stash_;
};

} // namespace spaznet::detail
