#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace spaznet {
namespace http3 {

// HTTP/3 frame types (RFC 9114 §7.2).
enum class H3FrameType : uint64_t {
    Data = 0x00,
    Headers = 0x01,
    CancelPush = 0x03,
    Settings = 0x04,
    PushPromise = 0x05,
    GoAway = 0x07,
    MaxPushId = 0x0d,
};

// HTTP/3 SETTINGS identifiers we care about (RFC 9114 + RFC 9204).
enum class H3SettingId : uint64_t {
    MaxFieldSectionSize = 0x06,
    QpackMaxTableCapacity = 0x01,
    QpackBlockedStreams = 0x07,
};

struct H3Settings {
    std::vector<std::pair<uint64_t, uint64_t>> entries;
};

struct H3Data {
    std::vector<uint8_t> data;
};
struct H3Headers {
    std::vector<uint8_t> encoded_field_section;
};
struct H3GoAway {
    uint64_t stream_or_push_id{0};
};
struct H3MaxPushId {
    uint64_t value{0};
};
struct H3CancelPush {
    uint64_t push_id{0};
};
// Reserved / unknown frame types should be discarded silently (RFC 9114
// §9). We surface them so callers can log or ignore.
struct H3Reserved {
    uint64_t type{0};
    std::vector<uint8_t> data;
};

using H3Frame =
    std::variant<H3Settings, H3Data, H3Headers, H3GoAway, H3MaxPushId, H3CancelPush, H3Reserved>;

// Parse a single HTTP/3 frame from `buf[offset..]`. Advances `offset`
// past the frame on success.
[[nodiscard]] auto parse_h3_frame(std::span<const uint8_t> buf, std::size_t& offset,
                                  H3Frame& out) -> bool;

// Append the wire encoding of one frame to `out`.
auto encode_h3_frame(std::vector<uint8_t>& out, const H3Frame& f) -> void;

// Stream type codes for HTTP/3 unidirectional streams (RFC 9114 §6.2).
enum class H3UniStreamType : uint64_t {
    Control = 0x00,
    Push = 0x01,
    QpackEncoder = 0x02,
    QpackDecoder = 0x03,
};

} // namespace http3
} // namespace spaznet
