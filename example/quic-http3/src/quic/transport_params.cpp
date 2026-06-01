#include <libspaznet/quic/transport_params.hpp>

#include <cstring>

#include <libspaznet/quic/varint.hpp>

namespace spaznet {
namespace quic {

namespace {

auto append_param(std::vector<uint8_t>& out, uint64_t id, std::span<const uint8_t> value) -> void {
    VarInt::append(out, id);
    VarInt::append(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

auto append_varint_param(std::vector<uint8_t>& out, uint64_t id, uint64_t value) -> void {
    std::vector<uint8_t> v;
    VarInt::append(v, value);
    append_param(out, id, {v.data(), v.size()});
}

auto append_empty_param(std::vector<uint8_t>& out, uint64_t id) -> void {
    VarInt::append(out, id);
    VarInt::append(out, 0U);
}

} // namespace

auto encode_transport_params(const TransportParameters& tp) -> std::vector<uint8_t> {
    std::vector<uint8_t> out;

    if (tp.original_destination_connection_id) {
        append_param(out,
                     static_cast<uint64_t>(TransportParamId::OriginalDestinationConnectionId),
                     {tp.original_destination_connection_id->data(),
                      tp.original_destination_connection_id->size()});
    }
    if (tp.initial_source_connection_id) {
        append_param(out, static_cast<uint64_t>(TransportParamId::InitialSourceConnectionId),
                     {tp.initial_source_connection_id->data(),
                      tp.initial_source_connection_id->size()});
    }
    if (tp.retry_source_connection_id) {
        append_param(out, static_cast<uint64_t>(TransportParamId::RetrySourceConnectionId),
                     {tp.retry_source_connection_id->data(),
                      tp.retry_source_connection_id->size()});
    }
    if (tp.stateless_reset_token) {
        append_param(out, static_cast<uint64_t>(TransportParamId::StatelessResetToken),
                     {tp.stateless_reset_token->data(), tp.stateless_reset_token->size()});
    }

    // Numeric parameters — emit only when they differ from RFC defaults.
    if (tp.max_idle_timeout_ms != 0) {
        append_varint_param(out, static_cast<uint64_t>(TransportParamId::MaxIdleTimeout),
                            tp.max_idle_timeout_ms);
    }
    if (tp.max_udp_payload_size != 65527) {
        append_varint_param(out, static_cast<uint64_t>(TransportParamId::MaxUdpPayloadSize),
                            tp.max_udp_payload_size);
    }
    if (tp.initial_max_data != 0) {
        append_varint_param(out, static_cast<uint64_t>(TransportParamId::InitialMaxData),
                            tp.initial_max_data);
    }
    if (tp.initial_max_stream_data_bidi_local != 0) {
        append_varint_param(out,
                            static_cast<uint64_t>(
                                TransportParamId::InitialMaxStreamDataBidiLocal),
                            tp.initial_max_stream_data_bidi_local);
    }
    if (tp.initial_max_stream_data_bidi_remote != 0) {
        append_varint_param(out,
                            static_cast<uint64_t>(
                                TransportParamId::InitialMaxStreamDataBidiRemote),
                            tp.initial_max_stream_data_bidi_remote);
    }
    if (tp.initial_max_stream_data_uni != 0) {
        append_varint_param(out,
                            static_cast<uint64_t>(TransportParamId::InitialMaxStreamDataUni),
                            tp.initial_max_stream_data_uni);
    }
    if (tp.initial_max_streams_bidi != 0) {
        append_varint_param(out,
                            static_cast<uint64_t>(TransportParamId::InitialMaxStreamsBidi),
                            tp.initial_max_streams_bidi);
    }
    if (tp.initial_max_streams_uni != 0) {
        append_varint_param(out, static_cast<uint64_t>(TransportParamId::InitialMaxStreamsUni),
                            tp.initial_max_streams_uni);
    }
    if (tp.ack_delay_exponent != 3) {
        append_varint_param(out, static_cast<uint64_t>(TransportParamId::AckDelayExponent),
                            tp.ack_delay_exponent);
    }
    if (tp.max_ack_delay_ms != 25) {
        append_varint_param(out, static_cast<uint64_t>(TransportParamId::MaxAckDelay),
                            tp.max_ack_delay_ms);
    }
    if (tp.active_connection_id_limit != 2) {
        append_varint_param(out,
                            static_cast<uint64_t>(TransportParamId::ActiveConnectionIdLimit),
                            tp.active_connection_id_limit);
    }
    if (tp.disable_active_migration) {
        append_empty_param(out, static_cast<uint64_t>(TransportParamId::DisableActiveMigration));
    }

    // Preserve any unknown params verbatim — useful when forwarding or
    // round-tripping for diagnostics.
    for (const auto& u : tp.unknown) {
        append_param(out, u.id, {u.value.data(), u.value.size()});
    }
    return out;
}

namespace {

auto read_varint_value(std::span<const uint8_t> value, uint64_t& out) -> bool {
    std::size_t off = 0;
    if (!VarInt::decode(value.data(), value.size(), off, out)) {
        return false;
    }
    return off == value.size();
}

} // namespace

auto decode_transport_params(std::span<const uint8_t> wire, TransportParameters& tp) -> bool {
    std::size_t off = 0;
    while (off < wire.size()) {
        uint64_t id = 0;
        uint64_t len = 0;
        if (!VarInt::decode(wire.data(), wire.size(), off, id) ||
            !VarInt::decode(wire.data(), wire.size(), off, len)) {
            return false;
        }
        if (off + len > wire.size()) {
            return false;
        }
        std::span<const uint8_t> value{wire.data() + off, static_cast<std::size_t>(len)};
        off += static_cast<std::size_t>(len);

        switch (static_cast<TransportParamId>(id)) {
            case TransportParamId::OriginalDestinationConnectionId:
                tp.original_destination_connection_id =
                    std::vector<uint8_t>(value.begin(), value.end());
                break;
            case TransportParamId::InitialSourceConnectionId:
                tp.initial_source_connection_id =
                    std::vector<uint8_t>(value.begin(), value.end());
                break;
            case TransportParamId::RetrySourceConnectionId:
                tp.retry_source_connection_id =
                    std::vector<uint8_t>(value.begin(), value.end());
                break;
            case TransportParamId::StatelessResetToken:
                if (value.size() != 16) {
                    return false;
                }
                {
                    std::array<uint8_t, 16> tok{};
                    std::memcpy(tok.data(), value.data(), 16);
                    tp.stateless_reset_token = tok;
                }
                break;
            case TransportParamId::MaxIdleTimeout:
                if (!read_varint_value(value, tp.max_idle_timeout_ms)) return false;
                break;
            case TransportParamId::MaxUdpPayloadSize:
                if (!read_varint_value(value, tp.max_udp_payload_size)) return false;
                break;
            case TransportParamId::InitialMaxData:
                if (!read_varint_value(value, tp.initial_max_data)) return false;
                break;
            case TransportParamId::InitialMaxStreamDataBidiLocal:
                if (!read_varint_value(value, tp.initial_max_stream_data_bidi_local)) return false;
                break;
            case TransportParamId::InitialMaxStreamDataBidiRemote:
                if (!read_varint_value(value, tp.initial_max_stream_data_bidi_remote)) return false;
                break;
            case TransportParamId::InitialMaxStreamDataUni:
                if (!read_varint_value(value, tp.initial_max_stream_data_uni)) return false;
                break;
            case TransportParamId::InitialMaxStreamsBidi:
                if (!read_varint_value(value, tp.initial_max_streams_bidi)) return false;
                break;
            case TransportParamId::InitialMaxStreamsUni:
                if (!read_varint_value(value, tp.initial_max_streams_uni)) return false;
                break;
            case TransportParamId::AckDelayExponent:
                if (!read_varint_value(value, tp.ack_delay_exponent)) return false;
                break;
            case TransportParamId::MaxAckDelay:
                if (!read_varint_value(value, tp.max_ack_delay_ms)) return false;
                break;
            case TransportParamId::ActiveConnectionIdLimit:
                if (!read_varint_value(value, tp.active_connection_id_limit)) return false;
                break;
            case TransportParamId::DisableActiveMigration:
                if (!value.empty()) return false;
                tp.disable_active_migration = true;
                break;
            default:
                tp.unknown.push_back({id, std::vector<uint8_t>(value.begin(), value.end())});
                break;
        }
    }
    return true;
}

} // namespace quic
} // namespace spaznet
