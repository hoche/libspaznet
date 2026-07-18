#include <libspaznet/quic/frame.hpp>

#include <cstring>
#include <stdexcept>

#include <libspaznet/quic/varint.hpp>

namespace spaznet {
namespace quic {

namespace {

constexpr uint8_t kStreamTypeMask = 0xF8;
constexpr uint8_t kStreamBaseType = 0x08;
constexpr uint8_t kStreamOffBit = 0x04;
constexpr uint8_t kStreamLenBit = 0x02;
constexpr uint8_t kStreamFinBit = 0x01;

auto read_varint(std::span<const uint8_t> buf, std::size_t& off, uint64_t& v) -> bool {
    return VarInt::decode(buf.data(), buf.size(), off, v);
}

auto read_bytes(std::span<const uint8_t> buf, std::size_t& off, std::size_t n,
                std::vector<uint8_t>& out) -> bool {
    if (off + n > buf.size()) {
        return false;
    }
    out.assign(buf.begin() + static_cast<std::ptrdiff_t>(off),
               buf.begin() + static_cast<std::ptrdiff_t>(off + n));
    off += n;
    return true;
}

} // namespace

auto parse_frame(std::span<const uint8_t> buf, std::size_t& offset, Frame& out) -> bool {
    if (offset >= buf.size()) {
        return false;
    }
    const std::size_t start = offset;
    uint64_t type_raw = 0;
    if (!read_varint(buf, offset, type_raw)) {
        return false;
    }

    // PADDING — coalesce a run of zero bytes.
    if (type_raw == static_cast<uint64_t>(FrameType::Padding)) {
        PaddingFrame f;
        f.count = 1;
        while (offset < buf.size() && buf[offset] == 0) {
            ++f.count;
            ++offset;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::Ping)) {
        out = PingFrame{};
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::Ack) ||
        type_raw == static_cast<uint64_t>(FrameType::AckEcn)) {
        AckFrame f;
        f.ecn = type_raw == static_cast<uint64_t>(FrameType::AckEcn);
        uint64_t ack_range_count = 0;
        if (!read_varint(buf, offset, f.largest_acked) ||
            !read_varint(buf, offset, f.ack_delay) ||
            !read_varint(buf, offset, ack_range_count) ||
            !read_varint(buf, offset, f.first_range)) {
            offset = start;
            return false;
        }
        // `ack_range_count` is an attacker-controlled varint (up to 2^62-1).
        // Reserving on it directly would attempt a multi-terabyte allocation
        // and throw bad_alloc/length_error (a remote crash). Each range needs
        // at least two bytes (gap + length varints, one byte minimum each),
        // so a count larger than half the remaining buffer cannot be
        // satisfied — reject it before reserving.
        if (ack_range_count > (buf.size() - offset) / 2) {
            offset = start;
            return false;
        }
        f.ranges.reserve(static_cast<std::size_t>(ack_range_count));
        for (uint64_t i = 0; i < ack_range_count; ++i) {
            AckRange r;
            if (!read_varint(buf, offset, r.gap) || !read_varint(buf, offset, r.length)) {
                offset = start;
                return false;
            }
            f.ranges.push_back(r);
        }
        if (f.ecn) {
            if (!read_varint(buf, offset, f.ect0) || !read_varint(buf, offset, f.ect1) ||
                !read_varint(buf, offset, f.ce)) {
                offset = start;
                return false;
            }
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::ResetStream)) {
        ResetStreamFrame f;
        if (!read_varint(buf, offset, f.stream_id) ||
            !read_varint(buf, offset, f.app_error) ||
            !read_varint(buf, offset, f.final_size)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::StopSending)) {
        StopSendingFrame f;
        if (!read_varint(buf, offset, f.stream_id) ||
            !read_varint(buf, offset, f.app_error)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::Crypto)) {
        CryptoFrame f;
        uint64_t length = 0;
        if (!read_varint(buf, offset, f.offset) || !read_varint(buf, offset, length) ||
            !read_bytes(buf, offset, length, f.data)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::NewToken)) {
        NewTokenFrame f;
        uint64_t length = 0;
        if (!read_varint(buf, offset, length) || !read_bytes(buf, offset, length, f.token)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    // STREAM 0x08..0x0f
    if ((type_raw & kStreamTypeMask) == kStreamBaseType) {
        StreamFrame f;
        const uint8_t flags = static_cast<uint8_t>(type_raw);
        f.fin = (flags & kStreamFinBit) != 0;
        const bool has_off = (flags & kStreamOffBit) != 0;
        f.has_length = (flags & kStreamLenBit) != 0;
        if (!read_varint(buf, offset, f.stream_id)) {
            offset = start;
            return false;
        }
        if (has_off) {
            if (!read_varint(buf, offset, f.offset)) {
                offset = start;
                return false;
            }
        }
        std::size_t data_len = 0;
        if (f.has_length) {
            uint64_t len = 0;
            if (!read_varint(buf, offset, len)) {
                offset = start;
                return false;
            }
            data_len = static_cast<std::size_t>(len);
        } else {
            // Extends to end of payload.
            data_len = buf.size() - offset;
        }
        if (!read_bytes(buf, offset, data_len, f.data)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::MaxData)) {
        MaxDataFrame f;
        if (!read_varint(buf, offset, f.maximum)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::MaxStreamData)) {
        MaxStreamDataFrame f;
        if (!read_varint(buf, offset, f.stream_id) || !read_varint(buf, offset, f.maximum)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::MaxStreamsBidi) ||
        type_raw == static_cast<uint64_t>(FrameType::MaxStreamsUni)) {
        MaxStreamsFrame f;
        f.bidi = type_raw == static_cast<uint64_t>(FrameType::MaxStreamsBidi);
        if (!read_varint(buf, offset, f.maximum)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::DataBlocked)) {
        DataBlockedFrame f;
        if (!read_varint(buf, offset, f.limit)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::StreamDataBlocked)) {
        StreamDataBlockedFrame f;
        if (!read_varint(buf, offset, f.stream_id) || !read_varint(buf, offset, f.limit)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::StreamsBlockedBidi) ||
        type_raw == static_cast<uint64_t>(FrameType::StreamsBlockedUni)) {
        StreamsBlockedFrame f;
        f.bidi = type_raw == static_cast<uint64_t>(FrameType::StreamsBlockedBidi);
        if (!read_varint(buf, offset, f.limit)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::NewConnectionId)) {
        NewConnectionIdFrame f;
        if (!read_varint(buf, offset, f.sequence_number) ||
            !read_varint(buf, offset, f.retire_prior_to)) {
            offset = start;
            return false;
        }
        if (offset >= buf.size()) {
            offset = start;
            return false;
        }
        std::size_t cid_len = buf[offset++];
        if (!read_bytes(buf, offset, cid_len, f.connection_id)) {
            offset = start;
            return false;
        }
        if (offset + f.stateless_reset_token.size() > buf.size()) {
            offset = start;
            return false;
        }
        std::memcpy(f.stateless_reset_token.data(), buf.data() + offset,
                    f.stateless_reset_token.size());
        offset += f.stateless_reset_token.size();
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::RetireConnectionId)) {
        RetireConnectionIdFrame f;
        if (!read_varint(buf, offset, f.sequence_number)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::PathChallenge) ||
        type_raw == static_cast<uint64_t>(FrameType::PathResponse)) {
        if (offset + 8 > buf.size()) {
            offset = start;
            return false;
        }
        if (type_raw == static_cast<uint64_t>(FrameType::PathChallenge)) {
            PathChallengeFrame f;
            std::memcpy(f.data.data(), buf.data() + offset, 8);
            offset += 8;
            out = f;
        } else {
            PathResponseFrame f;
            std::memcpy(f.data.data(), buf.data() + offset, 8);
            offset += 8;
            out = f;
        }
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::ConnectionCloseTransport) ||
        type_raw == static_cast<uint64_t>(FrameType::ConnectionCloseApplication)) {
        ConnectionCloseFrame f;
        f.application = type_raw == static_cast<uint64_t>(FrameType::ConnectionCloseApplication);
        if (!read_varint(buf, offset, f.error_code)) {
            offset = start;
            return false;
        }
        if (!f.application) {
            if (!read_varint(buf, offset, f.frame_type)) {
                offset = start;
                return false;
            }
        }
        uint64_t reason_len = 0;
        if (!read_varint(buf, offset, reason_len) ||
            !read_bytes(buf, offset, reason_len, f.reason)) {
            offset = start;
            return false;
        }
        out = f;
        return true;
    }

    if (type_raw == static_cast<uint64_t>(FrameType::HandshakeDone)) {
        out = HandshakeDoneFrame{};
        return true;
    }

    // Unknown frame type — rewind and signal failure.
    offset = start;
    return false;
}

auto parse_frames(std::span<const uint8_t> payload, std::vector<Frame>& out) -> bool {
    out.clear();
    std::size_t off = 0;
    while (off < payload.size()) {
        Frame f;
        if (!parse_frame(payload, off, f)) {
            return false;
        }
        out.push_back(std::move(f));
    }
    return true;
}

namespace {

auto write_type(std::vector<uint8_t>& out, FrameType t) -> void {
    VarInt::append(out, static_cast<uint64_t>(t));
}

} // namespace

auto encode_frame(std::vector<uint8_t>& out, const Frame& f) -> void {
    std::visit(
        [&out](auto const& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, PaddingFrame>) {
                for (std::size_t i = 0; i < x.count; ++i) {
                    out.push_back(0x00);
                }
            } else if constexpr (std::is_same_v<T, PingFrame>) {
                write_type(out, FrameType::Ping);
            } else if constexpr (std::is_same_v<T, AckFrame>) {
                write_type(out, x.ecn ? FrameType::AckEcn : FrameType::Ack);
                VarInt::append(out, x.largest_acked);
                VarInt::append(out, x.ack_delay);
                VarInt::append(out, x.ranges.size());
                VarInt::append(out, x.first_range);
                for (auto const& r : x.ranges) {
                    VarInt::append(out, r.gap);
                    VarInt::append(out, r.length);
                }
                if (x.ecn) {
                    VarInt::append(out, x.ect0);
                    VarInt::append(out, x.ect1);
                    VarInt::append(out, x.ce);
                }
            } else if constexpr (std::is_same_v<T, ResetStreamFrame>) {
                write_type(out, FrameType::ResetStream);
                VarInt::append(out, x.stream_id);
                VarInt::append(out, x.app_error);
                VarInt::append(out, x.final_size);
            } else if constexpr (std::is_same_v<T, StopSendingFrame>) {
                write_type(out, FrameType::StopSending);
                VarInt::append(out, x.stream_id);
                VarInt::append(out, x.app_error);
            } else if constexpr (std::is_same_v<T, CryptoFrame>) {
                write_type(out, FrameType::Crypto);
                VarInt::append(out, x.offset);
                VarInt::append(out, x.data.size());
                out.insert(out.end(), x.data.begin(), x.data.end());
            } else if constexpr (std::is_same_v<T, NewTokenFrame>) {
                write_type(out, FrameType::NewToken);
                VarInt::append(out, x.token.size());
                out.insert(out.end(), x.token.begin(), x.token.end());
            } else if constexpr (std::is_same_v<T, StreamFrame>) {
                uint8_t type_byte = kStreamBaseType | kStreamLenBit;
                if (x.offset != 0) {
                    type_byte |= kStreamOffBit;
                }
                if (x.fin) {
                    type_byte |= kStreamFinBit;
                }
                out.push_back(type_byte);
                VarInt::append(out, x.stream_id);
                if (x.offset != 0) {
                    VarInt::append(out, x.offset);
                }
                VarInt::append(out, x.data.size());
                out.insert(out.end(), x.data.begin(), x.data.end());
            } else if constexpr (std::is_same_v<T, MaxDataFrame>) {
                write_type(out, FrameType::MaxData);
                VarInt::append(out, x.maximum);
            } else if constexpr (std::is_same_v<T, MaxStreamDataFrame>) {
                write_type(out, FrameType::MaxStreamData);
                VarInt::append(out, x.stream_id);
                VarInt::append(out, x.maximum);
            } else if constexpr (std::is_same_v<T, MaxStreamsFrame>) {
                write_type(out, x.bidi ? FrameType::MaxStreamsBidi : FrameType::MaxStreamsUni);
                VarInt::append(out, x.maximum);
            } else if constexpr (std::is_same_v<T, DataBlockedFrame>) {
                write_type(out, FrameType::DataBlocked);
                VarInt::append(out, x.limit);
            } else if constexpr (std::is_same_v<T, StreamDataBlockedFrame>) {
                write_type(out, FrameType::StreamDataBlocked);
                VarInt::append(out, x.stream_id);
                VarInt::append(out, x.limit);
            } else if constexpr (std::is_same_v<T, StreamsBlockedFrame>) {
                write_type(out, x.bidi ? FrameType::StreamsBlockedBidi
                                       : FrameType::StreamsBlockedUni);
                VarInt::append(out, x.limit);
            } else if constexpr (std::is_same_v<T, NewConnectionIdFrame>) {
                write_type(out, FrameType::NewConnectionId);
                VarInt::append(out, x.sequence_number);
                VarInt::append(out, x.retire_prior_to);
                out.push_back(static_cast<uint8_t>(x.connection_id.size()));
                out.insert(out.end(), x.connection_id.begin(), x.connection_id.end());
                out.insert(out.end(), x.stateless_reset_token.begin(),
                           x.stateless_reset_token.end());
            } else if constexpr (std::is_same_v<T, RetireConnectionIdFrame>) {
                write_type(out, FrameType::RetireConnectionId);
                VarInt::append(out, x.sequence_number);
            } else if constexpr (std::is_same_v<T, PathChallengeFrame>) {
                write_type(out, FrameType::PathChallenge);
                out.insert(out.end(), x.data.begin(), x.data.end());
            } else if constexpr (std::is_same_v<T, PathResponseFrame>) {
                write_type(out, FrameType::PathResponse);
                out.insert(out.end(), x.data.begin(), x.data.end());
            } else if constexpr (std::is_same_v<T, ConnectionCloseFrame>) {
                write_type(out, x.application ? FrameType::ConnectionCloseApplication
                                              : FrameType::ConnectionCloseTransport);
                VarInt::append(out, x.error_code);
                if (!x.application) {
                    VarInt::append(out, x.frame_type);
                }
                VarInt::append(out, x.reason.size());
                out.insert(out.end(), x.reason.begin(), x.reason.end());
            } else if constexpr (std::is_same_v<T, HandshakeDoneFrame>) {
                write_type(out, FrameType::HandshakeDone);
            }
        },
        f);
}

auto is_ack_eliciting(const Frame& f) -> bool {
    return std::visit(
        [](auto const& x) -> bool {
            using T = std::decay_t<decltype(x)>;
            return !(std::is_same_v<T, PaddingFrame> || std::is_same_v<T, AckFrame> ||
                     std::is_same_v<T, ConnectionCloseFrame>);
        },
        f);
}

} // namespace quic
} // namespace spaznet
