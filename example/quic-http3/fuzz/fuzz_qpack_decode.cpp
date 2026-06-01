// libFuzzer harness for the RFC 9204 QPACK decoder.  Headers on every
// HTTP/3 request stream pass through this; attacker-controlled bytes
// land directly on qpack_decode.

#include <libspaznet/http3/qpack.hpp>

#include <cstddef>
#include <cstdint>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::span<const uint8_t> in{data, size};
    spaznet::http3::HeaderList out;
    (void)spaznet::http3::qpack_decode(in, out);
    return 0;
}
