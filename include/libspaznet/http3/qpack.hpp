#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// RFC 9204 QPACK — static-table-only encoder/decoder. We advertise
// SETTINGS_QPACK_MAX_TABLE_CAPACITY=0 to the peer so the dynamic table
// is never used: every header field is encoded as either an "Indexed
// Header Field — Static" reference, a "Literal Header Field with
// Static Name Reference", or a "Literal Header Field with Literal
// Name". String literals always use Huffman compression on encode but
// the decoder accepts either form.
//
// We don't implement the encoder/decoder stream traffic; the field
// section prefix is always emitted as "Required Insert Count = 0,
// Delta Base = 0, Sign = 0", which is valid when no dynamic entries
// are referenced.

namespace spaznet {
namespace http3 {

using HeaderField = std::pair<std::string, std::string>;
using HeaderList = std::vector<HeaderField>;

// Encode a header list into a QPACK field section (RFC 9204 §4.5).
// The output begins with the Encoded Field Section Prefix (two varints,
// both zero in static-only mode).
auto qpack_encode(const HeaderList& headers, std::vector<uint8_t>& out) -> void;

// Decode a QPACK field section. Returns true on success. Required
// Insert Count and Delta Base values are parsed and required to be 0.
[[nodiscard]] auto qpack_decode(std::span<const uint8_t> input, HeaderList& out) -> bool;

// Direct access to the static table (RFC 9204 Appendix A). Useful for
// tests; index 0 is ":authority", etc.
struct StaticEntry {
    const char* name;
    const char* value; // empty string for name-only entries
};
auto qpack_static_table() -> std::span<const StaticEntry>;

} // namespace http3
} // namespace spaznet
