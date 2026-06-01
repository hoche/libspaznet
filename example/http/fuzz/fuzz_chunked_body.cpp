// libFuzzer harness for HTTPParser::parse_chunked_body.  This is the
// Transfer-Encoding: chunked decoder, including trailer-field parsing
// (RFC 9112 §7.1.2) and chunk-extension scanning (§7.1.1).  Both got
// fresh in 2026-05-31; this harness is the regression net for those
// changes.

#include <libspaznet/http/handler.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::vector<uint8_t> buf(data, data + size);
    std::vector<uint8_t> body;
    size_t consumed = 0;
    (void)spaznet::http::HTTPParser::parse_chunked_body(buf, body, consumed);
    return 0;
}
