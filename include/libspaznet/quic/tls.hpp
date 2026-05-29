#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <libspaznet/quic/crypto.hpp>
#include <libspaznet/quic/transport_params.hpp>

// Forward declarations to keep the OpenSSL types out of this header for
// downstream consumers.
struct ssl_st;
struct ssl_ctx_st;

namespace spaznet {
namespace quic {

// QUIC encryption levels, matching OpenSSL's protection-level numbering.
enum class EncryptionLevel : uint8_t {
    Initial = 0,
    EarlyData = 1,
    Handshake = 2,
    Application = 3,
};

// Configuration for the server-side SSL_CTX used by every connection.
struct TlsServerConfig {
    // PEM-encoded certificate (chain) and matching private key.
    std::string cert_pem;
    std::string key_pem;
    // ALPN protocols offered to the peer, in preference order. Empty
    // means "do not configure ALPN" — but for HTTP/3 the caller is
    // expected to include "h3".
    std::vector<std::string> alpn;
};

// Lifetime-managed SSL_CTX wrapper. Created once per server; one or more
// TlsConnection objects can be derived from it. RAII frees the SSL_CTX.
class TlsContext {
  public:
    // Build an SSL_CTX configured for QUIC server use. Returns nullptr on
    // failure (e.g. invalid PEM, missing key/cert pair).
    static auto make_server(const TlsServerConfig& cfg) -> std::shared_ptr<TlsContext>;

    TlsContext(const TlsContext&) = delete;
    auto operator=(const TlsContext&) -> TlsContext& = delete;
    TlsContext(TlsContext&&) = delete;
    auto operator=(TlsContext&&) -> TlsContext& = delete;
    ~TlsContext();

    [[nodiscard]] auto ssl_ctx() const -> ssl_ctx_st* {
        return ctx_;
    }

  private:
    TlsContext() = default;
    ssl_ctx_st* ctx_{nullptr};
};

// Per-connection TLS state machine. Wraps an OpenSSL SSL* configured with
// SSL_set_quic_tls_cbs, plus the per-PN-space input/output CRYPTO buffers
// and derived QUIC packet keys.
//
// Lifecycle:
//   - Construct as server with the original DCID (used to derive Initial
//     keys before TLS gets involved).
//   - Set transport parameters.
//   - On every received CRYPTO frame for a given EncryptionLevel, call
//     deliver_crypto(level, data).
//   - After each delivery call advance(): this drains OpenSSL's outbound
//     handshake bytes into out_crypto(level) and surfaces any newly
//     derived secrets.
//   - When state() == Established the handshake is complete and the peer
//     transport parameters are available via peer_transport_params().
class TlsConnection {
  public:
    enum class State : uint8_t {
        Handshaking,
        Established,
        Failed,
    };

    // Server-side constructor. `dcid` is the destination connection ID
    // chosen by the client in its first Initial — used to derive the
    // Initial-level packet keys before TLS callbacks fire.
    TlsConnection(std::shared_ptr<TlsContext> ctx, std::span<const uint8_t> client_dcid,
                  const TransportParameters& tp);

    TlsConnection(const TlsConnection&) = delete;
    auto operator=(const TlsConnection&) -> TlsConnection& = delete;
    TlsConnection(TlsConnection&&) = delete;
    auto operator=(TlsConnection&&) -> TlsConnection& = delete;
    ~TlsConnection();

    // Feed CRYPTO-frame bytes received at `level` into the TLS engine.
    // Bytes are appended; out-of-order delivery is the caller's concern.
    auto deliver_crypto(EncryptionLevel level, std::span<const uint8_t> data) -> void;

    // Drive the handshake forward. Returns the new state.
    auto advance() -> State;

    [[nodiscard]] auto state() const -> State {
        return state_;
    }

    // Outgoing CRYPTO-frame bytes that the transport should put on the
    // wire at `level`. Caller should drain the returned buffer (move it
    // out or copy it) and then call ack_crypto_sent(level, n).
    [[nodiscard]] auto out_crypto(EncryptionLevel level) -> std::vector<uint8_t>&;

    // Mark `n` bytes of out_crypto(level) as sent. The bytes are removed
    // from the front of the buffer.
    auto consume_out_crypto(EncryptionLevel level, std::size_t n) -> void;

    // The AEAD/Hash chosen by TLS for the protected (Handshake/1-RTT)
    // levels. Valid once the matching secrets have been installed.
    [[nodiscard]] auto negotiated_aead() const -> Aead {
        return negotiated_aead_;
    }

    // Per-direction traffic secrets indexed by EncryptionLevel. Empty
    // until OpenSSL hands them to us via the yield_secret callback.
    [[nodiscard]] auto read_secret(EncryptionLevel level) const -> const std::vector<uint8_t>&;
    [[nodiscard]] auto write_secret(EncryptionLevel level) const -> const std::vector<uint8_t>&;

    // Peer transport parameters (decoded). Empty until the peer's TLS
    // extension has been received.
    [[nodiscard]] auto peer_transport_params() const -> const TransportParameters& {
        return peer_tp_;
    }

    // Set to true once we've seen the peer's transport-parameters
    // extension. Independent of `state_` because TPs arrive mid-handshake.
    [[nodiscard]] auto have_peer_transport_params() const -> bool {
        return have_peer_tp_;
    }

    // Last TLS alert code, if state() == Failed.
    [[nodiscard]] auto alert_code() const -> uint8_t {
        return alert_code_;
    }

    // Negotiated ALPN protocol (empty until handshake completes).
    [[nodiscard]] auto negotiated_alpn() const -> std::string;

    // Internal entry points for the OpenSSL dispatch callbacks. Public
    // only so the static thunks in tls.cpp can reach them.
    auto on_crypto_send(std::span<const uint8_t> data, std::size_t& consumed) -> int;
    auto on_crypto_recv(const uint8_t** buf, std::size_t* bytes_read) -> int;
    auto on_crypto_release(std::size_t bytes_read) -> int;
    auto on_yield_secret(uint32_t prot_level, int direction, std::span<const uint8_t> secret)
        -> int;
    auto on_got_transport_params(std::span<const uint8_t> params) -> int;
    auto on_alert(uint8_t alert_code) -> int;

  private:
    auto set_state_from_handshake_result(int handshake_ret) -> void;

    std::shared_ptr<TlsContext> ctx_;
    ssl_st* ssl_{nullptr};
    State state_{State::Handshaking};
    uint8_t alert_code_{0};

    // Inbound CRYPTO buffer per level, plus a cursor for crypto_recv_rcd.
    struct LevelIn {
        std::vector<uint8_t> data;
        std::size_t read_cursor{0};
    };
    std::array<LevelIn, 4> in_{};
    // Outbound CRYPTO buffer per level.
    std::array<std::vector<uint8_t>, 4> out_{};
    // Current send level (set on the most recent crypto_send call).
    EncryptionLevel send_level_{EncryptionLevel::Initial};
    // Read/Write secrets indexed by EncryptionLevel.
    std::array<std::vector<uint8_t>, 4> read_secret_{};
    std::array<std::vector<uint8_t>, 4> write_secret_{};

    TransportParameters peer_tp_{};
    bool have_peer_tp_{false};

    Aead negotiated_aead_{Aead::Aes128Gcm};
    // Our own transport parameters as encoded for the TLS extension; the
    // SSL* keeps a pointer to the buffer we hand it, so we own the
    // storage.
    std::vector<uint8_t> own_tp_wire_{};
};

// Helper: pick a QUIC AEAD/Hash pair from an OpenSSL TLS cipher suite ID
// (the 16-bit IANA codepoint used in TLS). Returns Aes128Gcm if the
// cipher is unknown, which is the default for Initial-level packets.
auto aead_from_tls_cipher_id(uint16_t cipher_id) -> Aead;

} // namespace quic
} // namespace spaznet
