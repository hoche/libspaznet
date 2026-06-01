#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace spaznet {
namespace quic {

// QUIC v1 frame types (RFC 9000 §19). Values match the type byte / varint.
// STREAM frames span 0x08..0x0F (low three bits OFF/LEN/FIN); we only
// expose the canonical 0x08 here and carry the variant flags on the
// struct itself.
enum class FrameType : uint64_t {
    Padding = 0x00,
    Ping = 0x01,
    Ack = 0x02,
    AckEcn = 0x03,
    ResetStream = 0x04,
    StopSending = 0x05,
    Crypto = 0x06,
    NewToken = 0x07,
    Stream = 0x08, // 0x08..0x0f
    MaxData = 0x10,
    MaxStreamData = 0x11,
    MaxStreamsBidi = 0x12,
    MaxStreamsUni = 0x13,
    DataBlocked = 0x14,
    StreamDataBlocked = 0x15,
    StreamsBlockedBidi = 0x16,
    StreamsBlockedUni = 0x17,
    NewConnectionId = 0x18,
    RetireConnectionId = 0x19,
    PathChallenge = 0x1a,
    PathResponse = 0x1b,
    ConnectionCloseTransport = 0x1c,
    ConnectionCloseApplication = 0x1d,
    HandshakeDone = 0x1e,
};

// Each frame parses to one of the structs below. We use std::variant to
// avoid object slicing while keeping the codec free of virtual calls.

struct PaddingFrame {
    std::size_t count = 1; // run length of consecutive PADDING bytes
};

struct PingFrame {};

struct AckRange {
    uint64_t gap = 0;
    uint64_t length = 0;
};

struct AckFrame {
    uint64_t largest_acked = 0;
    uint64_t ack_delay = 0; // in microseconds-scaled units (peer's exponent)
    uint64_t first_range = 0;
    std::vector<AckRange> ranges;
    bool ecn = false;
    uint64_t ect0 = 0;
    uint64_t ect1 = 0;
    uint64_t ce = 0;
};

struct ResetStreamFrame {
    uint64_t stream_id = 0;
    uint64_t app_error = 0;
    uint64_t final_size = 0;
};

struct StopSendingFrame {
    uint64_t stream_id = 0;
    uint64_t app_error = 0;
};

struct CryptoFrame {
    uint64_t offset = 0;
    std::vector<uint8_t> data;
};

struct NewTokenFrame {
    std::vector<uint8_t> token;
};

struct StreamFrame {
    uint64_t stream_id = 0;
    uint64_t offset = 0;
    bool has_length = false; // true if Length field was present on wire
    bool fin = false;
    std::vector<uint8_t> data;
};

struct MaxDataFrame {
    uint64_t maximum = 0;
};

struct MaxStreamDataFrame {
    uint64_t stream_id = 0;
    uint64_t maximum = 0;
};

struct MaxStreamsFrame {
    bool bidi = true;
    uint64_t maximum = 0;
};

struct DataBlockedFrame {
    uint64_t limit = 0;
};

struct StreamDataBlockedFrame {
    uint64_t stream_id = 0;
    uint64_t limit = 0;
};

struct StreamsBlockedFrame {
    bool bidi = true;
    uint64_t limit = 0;
};

struct NewConnectionIdFrame {
    uint64_t sequence_number = 0;
    uint64_t retire_prior_to = 0;
    std::vector<uint8_t> connection_id;
    std::array<uint8_t, 16> stateless_reset_token{};
};

struct RetireConnectionIdFrame {
    uint64_t sequence_number = 0;
};

struct PathChallengeFrame {
    std::array<uint8_t, 8> data{};
};

struct PathResponseFrame {
    std::array<uint8_t, 8> data{};
};

struct ConnectionCloseFrame {
    bool application = false; // 0x1c (transport) vs 0x1d (application)
    uint64_t error_code = 0;
    uint64_t frame_type = 0; // transport-close only
    std::vector<uint8_t> reason;
};

struct HandshakeDoneFrame {};

using Frame = std::variant<PaddingFrame, PingFrame, AckFrame, ResetStreamFrame, StopSendingFrame,
                           CryptoFrame, NewTokenFrame, StreamFrame, MaxDataFrame,
                           MaxStreamDataFrame, MaxStreamsFrame, DataBlockedFrame,
                           StreamDataBlockedFrame, StreamsBlockedFrame, NewConnectionIdFrame,
                           RetireConnectionIdFrame, PathChallengeFrame, PathResponseFrame,
                           ConnectionCloseFrame, HandshakeDoneFrame>;

// Parse a single frame at `buf[offset..]`. Advances `offset` past the
// frame on success. PADDING frames coalesce a run of 0x00 bytes into a
// single PaddingFrame with `count` set. Returns false on truncation /
// unknown frame type.
[[nodiscard]] auto parse_frame(std::span<const uint8_t> buf, std::size_t& offset, Frame& out)
    -> bool;

// Parse every frame in the payload until exhausted. Returns false if a
// frame fails to parse partway through.
[[nodiscard]] auto parse_frames(std::span<const uint8_t> payload, std::vector<Frame>& out)
    -> bool;

// Append a single frame's wire encoding to `out`. For STREAM frames the
// emitted type byte sets LEN=1 always and OFF/FIN according to the
// struct fields.
auto encode_frame(std::vector<uint8_t>& out, const Frame& f) -> void;

// Whether a frame is "ack-eliciting" (RFC 9000 §13.2.1). Everything
// except ACK, PADDING, and CONNECTION_CLOSE elicits an ACK.
[[nodiscard]] auto is_ack_eliciting(const Frame& f) -> bool;

} // namespace quic
} // namespace spaznet
