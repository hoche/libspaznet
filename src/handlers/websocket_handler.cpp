#include <algorithm>
#include <cstring>
#include <libspaznet/handlers/websocket_handler.hpp>

namespace spaznet {

std::vector<uint8_t> WebSocketFrame::serialize() const {
    std::vector<uint8_t> result;
    result.reserve(14 + payload.size()); // Max header size

    // First byte: FIN, RSV, Opcode
    uint8_t byte1 = (fin ? 0x80 : 0x00) | (rsv1 ? 0x40 : 0x00) | (rsv2 ? 0x20 : 0x00) |
                    (rsv3 ? 0x10 : 0x00) | static_cast<uint8_t>(opcode);
    result.push_back(byte1);

    // Second byte: MASK, Payload length
    uint8_t byte2 = masked ? 0x80 : 0x00;

    if (payload_length < 126) {
        byte2 |= payload_length;
        result.push_back(byte2);
    } else if (payload_length < 65536) {
        byte2 |= 126;
        result.push_back(byte2);
        result.push_back((payload_length >> 8) & 0xFF);
        result.push_back(payload_length & 0xFF);
    } else {
        byte2 |= 127;
        result.push_back(byte2);
        for (int i = 7; i >= 0; --i) {
            result.push_back((payload_length >> (i * 8)) & 0xFF);
        }
    }

    // Masking key (if masked)
    if (masked) {
        result.push_back((masking_key >> 24) & 0xFF);
        result.push_back((masking_key >> 16) & 0xFF);
        result.push_back((masking_key >> 8) & 0xFF);
        result.push_back(masking_key & 0xFF);
    }

    // Payload
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            result.push_back(payload[i] ^ ((masking_key >> ((3 - (i % 4)) * 8)) & 0xFF));
        }
    } else {
        result.insert(result.end(), payload.begin(), payload.end());
    }

    return result;
}

WebSocketFrame WebSocketFrame::parse(const std::vector<uint8_t>& data) {
    WebSocketFrame frame;

    if (data.size() < 2) {
        throw std::runtime_error("Invalid WebSocket frame");
    }

    // Parse first byte
    frame.fin = (data[0] & 0x80) != 0;
    frame.rsv1 = (data[0] & 0x40) != 0;
    frame.rsv2 = (data[0] & 0x20) != 0;
    frame.rsv3 = (data[0] & 0x10) != 0;
    frame.opcode = static_cast<WebSocketOpcode>(data[0] & 0x0F);

    // Parse second byte
    frame.masked = (data[1] & 0x80) != 0;
    uint8_t payload_len_byte = data[1] & 0x7F;

    size_t header_size = 2;

    if (payload_len_byte < 126) {
        frame.payload_length = payload_len_byte;
    } else if (payload_len_byte == 126) {
        if (data.size() < 4) {
            throw std::runtime_error("Invalid WebSocket frame");
        }
        frame.payload_length = (static_cast<uint64_t>(data[2]) << 8) | data[3];
        header_size = 4;
    } else {
        if (data.size() < 10) {
            throw std::runtime_error("Invalid WebSocket frame");
        }
        frame.payload_length = 0;
        for (int i = 0; i < 8; ++i) {
            frame.payload_length = (frame.payload_length << 8) | data[2 + i];
        }
        header_size = 10;
    }

    // Parse masking key
    if (frame.masked) {
        if (data.size() < header_size + 4) {
            throw std::runtime_error("Invalid WebSocket frame");
        }
        frame.masking_key = (static_cast<uint32_t>(data[header_size]) << 24) |
                            (static_cast<uint32_t>(data[header_size + 1]) << 16) |
                            (static_cast<uint32_t>(data[header_size + 2]) << 8) |
                            data[header_size + 3];
        header_size += 4;
    }

    // Parse payload
    if (data.size() < header_size + frame.payload_length) {
        throw std::runtime_error("Invalid WebSocket frame");
    }

    frame.payload.assign(data.begin() + header_size,
                         data.begin() + header_size + frame.payload_length);

    // Unmask if needed
    if (frame.masked) {
        for (size_t i = 0; i < frame.payload.size(); ++i) {
            frame.payload[i] ^= ((frame.masking_key >> ((3 - (i % 4)) * 8)) & 0xFF);
        }
    }

    return frame;
}

} // namespace spaznet

