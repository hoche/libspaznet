#include <algorithm>
#include <cstddef>
#include <cstring>
#include <libspaznet/websocket/handler.hpp>
#include <limits>

namespace spaznet::websocket {

namespace {
constexpr uint8_t kFinBit = 0x80;
constexpr uint8_t kRsv1Bit = 0x40;
constexpr uint8_t kRsv2Bit = 0x20;
constexpr uint8_t kRsv3Bit = 0x10;
constexpr uint8_t kMaskBit = 0x80;
constexpr uint8_t kPayloadLenMask = 0x7F;
constexpr uint8_t kOpcodeMask = 0x0F;
constexpr uint8_t kByteMask = 0xFF;
constexpr int kBitsPerByte = 8;
constexpr int kMaskingKeyBytes = 4;
constexpr uint64_t kPayloadLen16Code = 126;
constexpr uint64_t kPayloadLen64Code = 127;
constexpr std::size_t kBaseHeaderSize = 2;
constexpr std::size_t kHeaderSizeWith16BitLength = 4;
constexpr std::size_t kHeaderSizeWith64BitLength = 10;
constexpr std::size_t kExtendedLen64Bytes = 8;
constexpr std::size_t kMaxHeaderSizeBytes = 14;
constexpr uint64_t kPayloadLen16MaxExclusive = 65536;
constexpr int kMostSignificantByteIndex = 7;
constexpr uint32_t kShift24 = 24;
constexpr uint32_t kShift16 = 16;
} // namespace

auto Frame::serialize() const -> std::vector<uint8_t> {
    std::vector<uint8_t> result;
    result.reserve(kMaxHeaderSizeBytes + payload.size()); // Max header size

    // First byte: FIN, RSV, Opcode
    uint8_t byte1 = (fin ? kFinBit : 0x00) | (rsv1 ? kRsv1Bit : 0x00) | (rsv2 ? kRsv2Bit : 0x00) |
                    (rsv3 ? kRsv3Bit : 0x00) | static_cast<uint8_t>(opcode);
    result.push_back(byte1);

    // Second byte: MASK, Payload length
    uint8_t byte2 = masked ? kMaskBit : 0x00;

    if (payload_length < kPayloadLen16Code) {
        byte2 |= payload_length;
        result.push_back(byte2);
    } else if (payload_length < kPayloadLen16MaxExclusive) {
        byte2 |= kPayloadLen16Code;
        result.push_back(byte2);
        result.push_back((payload_length >> kBitsPerByte) & kByteMask);
        result.push_back(payload_length & kByteMask);
    } else {
        byte2 |= kPayloadLen64Code;
        result.push_back(byte2);
        for (int i = kMostSignificantByteIndex; i >= 0; --i) {
            result.push_back((payload_length >> (i * kBitsPerByte)) & kByteMask);
        }
    }

    // Masking key (if masked)
    if (masked) {
        result.push_back((masking_key >> kShift24) & kByteMask);
        result.push_back((masking_key >> kShift16) & kByteMask);
        result.push_back((masking_key >> kBitsPerByte) & kByteMask);
        result.push_back(masking_key & kByteMask);
    }

    // Payload
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            result.push_back(
                payload[i] ^
                ((masking_key >> ((3 - (i % kMaskingKeyBytes)) * kBitsPerByte)) & kByteMask));
        }
    } else {
        result.insert(result.end(), payload.begin(), payload.end());
    }

    return result;
}

namespace {

// RFC 6455 §5.2 defines a fixed set of opcodes. Anything else (0x3–0x7,
// 0xB–0xF) is reserved and MUST cause the connection to fail.
bool is_known_opcode(Opcode op) {
    switch (op) {
        case Opcode::Continuation:
        case Opcode::Text:
        case Opcode::Binary:
        case Opcode::Close:
        case Opcode::Ping:
        case Opcode::Pong:
            return true;
    }
    return false;
}

bool is_control_opcode(Opcode op) {
    return op == Opcode::Close || op == Opcode::Ping ||
           op == Opcode::Pong;
}

} // namespace


auto Frame::parse(const std::vector<uint8_t>& data) -> Frame {
    Frame frame;

    if (data.size() < kBaseHeaderSize) {
        throw std::runtime_error("WebSocket frame: short header");
    }

    // Parse first byte
    frame.fin = (data[0] & kFinBit) != 0;
    frame.rsv1 = (data[0] & kRsv1Bit) != 0;
    frame.rsv2 = (data[0] & kRsv2Bit) != 0;
    frame.rsv3 = (data[0] & kRsv3Bit) != 0;
    frame.opcode = static_cast<Opcode>(data[0] & kOpcodeMask);

    // RFC 6455 §5.2: reserved opcodes and RSV bits (absent a negotiated
    // extension) MUST fail the connection. We have no extensions, so any
    // RSV bit set is a protocol error.
    if (!is_known_opcode(frame.opcode) || frame.rsv1 || frame.rsv2 || frame.rsv3) {
        throw std::runtime_error("WebSocket frame: reserved opcode or RSV bit");
    }

    // Parse second byte
    frame.masked = (data[1] & kMaskBit) != 0;
    uint8_t payload_len_byte = data[1] & kPayloadLenMask;

    std::size_t header_size = kBaseHeaderSize;

    if (payload_len_byte < kPayloadLen16Code) {
        frame.payload_length = payload_len_byte;
    } else if (payload_len_byte == kPayloadLen16Code) {
        if (data.size() < kHeaderSizeWith16BitLength) {
            throw std::runtime_error("WebSocket frame: short 16-bit length");
        }
        frame.payload_length = (static_cast<uint64_t>(data[2]) << kBitsPerByte) | data[3];
        // RFC 6455 §5.2: the 16-bit form MUST be used only when length >= 126.
        if (frame.payload_length < kPayloadLen16Code) {
            throw std::runtime_error("WebSocket frame: non-minimal 16-bit length");
        }
        header_size = kHeaderSizeWith16BitLength;
    } else {
        if (data.size() < kHeaderSizeWith64BitLength) {
            throw std::runtime_error("WebSocket frame: short 64-bit length");
        }
        frame.payload_length = 0;
        for (std::size_t i = 0; i < kExtendedLen64Bytes; ++i) {
            frame.payload_length = (frame.payload_length << kBitsPerByte) | data[2 + i];
        }
        // RFC 6455 §5.2: the 64-bit form MUST be used only when length >=
        // 65536, AND the high bit MUST be 0.
        if (frame.payload_length < kPayloadLen16MaxExclusive) {
            throw std::runtime_error("WebSocket frame: non-minimal 64-bit length");
        }
        if ((frame.payload_length & (1ULL << 63)) != 0) {
            throw std::runtime_error("WebSocket frame: 64-bit length high bit set");
        }
        header_size = kHeaderSizeWith64BitLength;
    }

    // RFC 6455 §5.5: control frames MUST NOT be fragmented and MUST have a
    // payload length of 125 bytes or less.
    if (is_control_opcode(frame.opcode) && (!frame.fin || frame.payload_length > 125)) {
        throw std::runtime_error("WebSocket frame: bad control frame");
    }

    // Cap before allocating: anything past kMaxPayloadBytes is treated as
    // "message too big" — caller should close with code 1009.
    if (frame.payload_length > kMaxPayloadBytes) {
        throw std::runtime_error("WebSocket frame: payload too large");
    }

    // Parse masking key
    if (frame.masked) {
        if (data.size() < header_size + kMaskingKeyBytes) {
            throw std::runtime_error("WebSocket frame: short masking key");
        }
        frame.masking_key = (static_cast<uint32_t>(data[header_size]) << kShift24) |
                            (static_cast<uint32_t>(data[header_size + 1]) << kShift16) |
                            (static_cast<uint32_t>(data[header_size + 2]) << kBitsPerByte) |
                            static_cast<uint32_t>(data[header_size + 3]);
        header_size += kMaskingKeyBytes;
    }

    // Parse payload. payload_length is already bounded by kMaxPayloadBytes
    // above, so the cast to size_t cannot overflow on a 32-bit host either
    // (the cap fits in 25 bits).
    const auto payload_len = static_cast<std::size_t>(frame.payload_length);
    if (data.size() < header_size + payload_len) {
        throw std::runtime_error("WebSocket frame: short payload");
    }

    const auto header_off = static_cast<std::ptrdiff_t>(header_size);
    const auto payload_off = static_cast<std::ptrdiff_t>(payload_len);
    frame.payload.assign(data.begin() + header_off, data.begin() + header_off + payload_off);

    // Unmask if needed
    if (frame.masked) {
        for (size_t i = 0; i < frame.payload.size(); ++i) {
            frame.payload[i] ^=
                ((frame.masking_key >> ((3 - (i % kMaskingKeyBytes)) * kBitsPerByte)) & kByteMask);
        }
    }

    return frame;
}

} // namespace spaznet::websocket
