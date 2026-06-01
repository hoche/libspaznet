#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace spaznet {
namespace quic {

// RFC 9000 §18 transport-parameter identifiers we encode/decode. We only
// surface the parameters needed for server-side interop. Unknown IDs are
// preserved as raw bytes on decode and re-emitted unchanged on encode.
enum class TransportParamId : uint64_t {
    OriginalDestinationConnectionId = 0x00,
    MaxIdleTimeout = 0x01,
    StatelessResetToken = 0x02,
    MaxUdpPayloadSize = 0x03,
    InitialMaxData = 0x04,
    InitialMaxStreamDataBidiLocal = 0x05,
    InitialMaxStreamDataBidiRemote = 0x06,
    InitialMaxStreamDataUni = 0x07,
    InitialMaxStreamsBidi = 0x08,
    InitialMaxStreamsUni = 0x09,
    AckDelayExponent = 0x0a,
    MaxAckDelay = 0x0b,
    DisableActiveMigration = 0x0c,
    ActiveConnectionIdLimit = 0x0e,
    InitialSourceConnectionId = 0x0f,
    RetrySourceConnectionId = 0x10,
};

struct TransportParameters {
    // Connection-identifier parameters (server emits the first two; client
    // emits only initial_source_connection_id).
    std::optional<std::vector<uint8_t>> original_destination_connection_id;
    std::optional<std::vector<uint8_t>> initial_source_connection_id;
    std::optional<std::vector<uint8_t>> retry_source_connection_id;
    std::optional<std::array<uint8_t, 16>> stateless_reset_token;

    // Defaults per RFC 9000 §18.2.
    uint64_t max_idle_timeout_ms = 0;          // 0 = disabled
    uint64_t max_udp_payload_size = 65527;
    uint64_t initial_max_data = 0;
    uint64_t initial_max_stream_data_bidi_local = 0;
    uint64_t initial_max_stream_data_bidi_remote = 0;
    uint64_t initial_max_stream_data_uni = 0;
    uint64_t initial_max_streams_bidi = 0;
    uint64_t initial_max_streams_uni = 0;
    uint64_t ack_delay_exponent = 3;
    uint64_t max_ack_delay_ms = 25;
    uint64_t active_connection_id_limit = 2;
    bool disable_active_migration = false;

    // Raw bytes for any unknown parameter IDs we saw on the wire. We
    // store the original (id, value) so they can be re-emitted verbatim
    // if needed (the server normally ignores these).
    struct Unknown {
        uint64_t id = 0;
        std::vector<uint8_t> value;
    };
    std::vector<Unknown> unknown;
};

// Encode a TransportParameters struct into the wire format used in the
// TLS QUIC transport parameters extension (RFC 9000 §18.2).
auto encode_transport_params(const TransportParameters& tp) -> std::vector<uint8_t>;

// Decode the wire format. Returns true on success.
[[nodiscard]] auto decode_transport_params(std::span<const uint8_t> wire,
                                           TransportParameters& tp) -> bool;

} // namespace quic
} // namespace spaznet
