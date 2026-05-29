#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include <libspaznet/quic/connection.hpp>

namespace spaznet {
namespace quic {

// Retry-packet integrity tag (RFC 9001 §5.8).
//
// The "Retry Pseudo-Packet" is built from the Original DCID (the one the
// client put in its first Initial, before Retry) prefixed by its length
// byte, followed by the bytes of the Retry packet *excluding* the
// 16-byte integrity tag. The tag is AES-128-GCM applied with the
// version-specific key+nonce, an empty plaintext, and the pseudo-packet
// as AAD; the resulting auth tag IS the integrity tag appended to the
// Retry packet on the wire.
auto compute_retry_integrity_tag(std::span<const uint8_t> original_dcid,
                                 std::span<const uint8_t> retry_packet_without_tag,
                                 std::array<uint8_t, 16>& tag_out) -> bool;

// Build a Retry packet (RFC 9000 §17.2.5) carrying the given token.
// `odcid` is the client's original DCID (used to compute the integrity
// tag — the server learned it from the client's first Initial).
auto build_retry_packet(std::span<const uint8_t> client_scid,
                        std::span<const uint8_t> server_scid,
                        std::span<const uint8_t> token, std::span<const uint8_t> odcid)
    -> std::vector<uint8_t>;

// Build a Version Negotiation packet (RFC 9000 §17.2.1). The packet
// echoes the client's DCID and SCID and advertises the supported
// versions. `supported` defaults to {QUIC v1}.
auto build_version_negotiation_packet(std::span<const uint8_t> client_dcid_echo,
                                      std::span<const uint8_t> client_scid_echo,
                                      std::span<const uint8_t> supported_versions_be)
    -> std::vector<uint8_t>;

// UDP-side dispatcher. One Listener per UDP socket: it owns the set of
// active Connections keyed by the server-chosen SCID (== peer's DCID on
// inbound short-header packets). Long-header packets are routed by
// their wire DCID. New Initials trigger Connection creation; non-v1
// versions emit a Version Negotiation; clients that fail to validate
// the path budget see a Retry.
class Listener {
  public:
    using SendFn = std::function<void(std::span<const uint8_t>)>;

    struct Config {
        std::shared_ptr<TlsContext> tls_ctx;
        TransportParameters server_tp;
        // Length of server-chosen Source Connection IDs (1..20).
        std::size_t server_cid_length{8};
        // If true, emit a Retry on the very first Initial from each
        // peer to demand path validation before allocating a real
        // Connection (RFC 9000 §8.1.2 Address Validation). Off by
        // default for simplicity in early bring-up.
        bool require_retry{false};
        // Random seed for SCID and Retry-token generation. Pass 0 for
        // automatic seeding from std::random_device.
        uint64_t random_seed{0};
    };

    explicit Listener(Config cfg, SendFn send_fn);

    // Feed a UDP datagram with the peer address (opaque to us — used
    // only to route response packets back through `send_fn`). The
    // listener identifies the connection and dispatches.
    auto on_datagram(std::span<const uint8_t> dg) -> void;

    // Periodic pump of all owned connections.
    auto on_timer() -> void;

    [[nodiscard]] auto connection_count() const -> std::size_t {
        return connections_.size();
    }

    // Test hook: look up a connection by its server-chosen SCID.
    [[nodiscard]] auto find_connection(std::span<const uint8_t> scid) -> Connection*;

  private:
    auto new_random_cid(std::vector<uint8_t>& out) -> void;
    auto make_retry_token(std::span<const uint8_t> odcid,
                          std::span<const uint8_t> peer_token_nonce)
        -> std::vector<uint8_t>;

    Config cfg_;
    SendFn send_fn_;
    // Connections owned by the listener, keyed by the server-chosen SCID
    // (which becomes the DCID the peer uses from its second packet
    // onward).
    std::map<std::vector<uint8_t>, std::unique_ptr<Connection>> connections_;
    // Aliases mapping the client's initial DCID (a value the client
    // picked before learning the server's SCID) to the canonical SCID
    // key in `connections_`. Used to route retransmissions of the
    // very first Initial to the right connection.
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> connections_aliases_;
    // Token-validation HMAC key (one per Listener instance lifetime).
    std::array<uint8_t, 32> token_key_{};
    uint64_t prng_state_;
};

} // namespace quic
} // namespace spaznet
