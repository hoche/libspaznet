// libFuzzer harness for the QUIC transport-parameters codec
// (RFC 9000 §18).  The peer's transport parameters arrive inside
// the TLS extension on the ClientHello and decode_transport_params
// pulls them apart — a malformed extension must not crash us.

#include <libspaznet/quic/transport_params.hpp>

#include <cstddef>
#include <cstdint>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::span<const uint8_t> wire{data, size};
    spaznet::quic::TransportParameters tp;
    (void)spaznet::quic::decode_transport_params(wire, tp);
    return 0;
}
