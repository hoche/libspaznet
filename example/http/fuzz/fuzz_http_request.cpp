// libFuzzer harness for HTTPParser::parse_request — the entry point
// that turns wire bytes into an HTTPRequest.  Built when
// SPAZNET_BUILD_FUZZ=ON.
//
// We feed the fuzzer arbitrary bytes as if they were a freshly-arrived
// HTTP/1.1 request and look for crashes / UB / sanitizer hits.
// HTTPParser::parse_request is the surface a network-facing peer can
// reach through Server::set_connection_handler +
// spaznet::http::make_dispatcher, so anything reachable here is
// reachable from a hostile client.

#include <libspaznet/http/handler.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::vector<uint8_t> buf(data, data + size);
    spaznet::http::HTTPRequest req;
    size_t consumed = 0;
    (void)spaznet::http::HTTPParser::parse_request(buf, req, consumed);
    return 0;
}
