// libFuzzer harness for spaznet::http2::Frame::parse.  Every byte that
// follows the connection preface on an h2c connection passes through
// this, so it's the most heavily-exposed parser in example/http2.

#include <libspaznet/http2/handler.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int {
    std::vector<uint8_t> buf(data, data + size);
    size_t offset = 0;
    // Drain frames until parse fails (truncated) or we run out of
    // bytes.  Walking the stream verifies that successive calls
    // don't compound state errors.
    while (offset < buf.size()) {
        auto frame = spaznet::http2::Frame::parse(buf, offset);
        if (!frame) break;
    }
    // Also exercise Settings::parse on the same bytes (Settings frames
    // carry a payload that goes through a separate decoder).
    (void)spaznet::http2::Settings::parse(buf);
    return 0;
}
