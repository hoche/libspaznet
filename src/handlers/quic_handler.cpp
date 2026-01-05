#include <algorithm>
#include <cstring>
#include <libspaznet/handlers/quic_handler.hpp>
#include <stdexcept>

namespace spaznet {

// QUICStream implementation
QUICStream::QUICStream(uint64_t stream_id, bool bidirectional)
    : stream_id_(stream_id), bidirectional_(bidirectional), state_(QUICStreamState::Idle),
      receive_offset_(0), send_offset_(0), receive_fin_(false), send_fin_(false) {
    if (bidirectional) {
        state_ = QUICStreamState::Open;
    }
}

Task QUICStream::read(std::vector<uint8_t>& buffer, std::size_t max_size) {
    while (receive_buffer_.empty() && !receive_fin_ && state_ != QUICStreamState::Closed) {
        co_await make_awaiter(0); // Yield to allow data to arrive
    }

    if (!receive_buffer_.empty()) {
        std::size_t to_read = std::min(max_size, receive_buffer_.size());
        buffer.assign(receive_buffer_.begin(), receive_buffer_.begin() + to_read);
        receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + to_read);
    } else if (receive_fin_) {
        buffer.clear();
        if (state_ == QUICStreamState::HalfClosedRemote) {
            state_ = QUICStreamState::Closed;
        } else {
            state_ = QUICStreamState::HalfClosedRemote;
        }
    } else {
        buffer.clear();
    }
}

Task QUICStream::write(const std::vector<uint8_t>& data, bool fin) {
    if (state_ == QUICStreamState::Closed || state_ == QUICStreamState::HalfClosedLocal) {
        co_return;
    }

    send_buffer_.insert(send_buffer_.end(), data.begin(), data.end());
    send_fin_ = fin;

    if (fin) {
        if (state_ == QUICStreamState::HalfClosedRemote) {
            state_ = QUICStreamState::Closed;
        } else {
            state_ = QUICStreamState::HalfClosedLocal;
        }
    }
}

void QUICStream::close() {
    state_ = QUICStreamState::Closed;
}

// QUICConnection implementation
QUICConnection::QUICConnection(ConnectionID dest_conn_id, ConnectionID src_conn_id)
    : dest_conn_id_(std::move(dest_conn_id)), src_conn_id_(std::move(src_conn_id)),
      state_(QUICConnectionState::Handshake), next_stream_id_(0), max_stream_id_(100) {}

std::shared_ptr<QUICStream> QUICConnection::get_stream(uint64_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        return it->second;
    }

    // Determine if bidirectional (RFC9000 Section 2.1)
    // Client-initiated: even stream IDs are bidirectional
    // Server-initiated: odd stream IDs are bidirectional
    bool bidirectional = (stream_id % 4) < 2;

    auto stream = std::make_shared<QUICStream>(stream_id, bidirectional);
    streams_[stream_id] = stream;
    return stream;
}

bool QUICConnection::process_packet(const std::vector<uint8_t>& packet,
                                    std::vector<QUICStreamFrame>& frames_out) {
    frames_out.clear();
    if (packet.empty()) {
        return false;
    }

    QUICPacketType type;
    ConnectionID dest_id, src_id;
    std::vector<QUICStreamFrame> frames;

    if (!parse_packet(packet, type, dest_id, src_id, frames)) {
        return false;
    }

    // Update connection IDs if packet carried them (long header).
    if (!dest_id.bytes.empty()) {
        dest_conn_id_ = dest_id;
    }
    if (!src_id.bytes.empty()) {
        src_conn_id_ = src_id;
    }

    // Process frames
    for (const auto& frame : frames) {
        auto stream = get_stream(frame.stream_id);
        if (stream) {
            stream->receive_buffer_.insert(stream->receive_buffer_.end(), frame.data.begin(),
                                           frame.data.end());
            stream->receive_offset_ += frame.data.size();
            if (frame.fin) {
                stream->receive_fin_ = true;
                if (stream->state() == QUICStreamState::Open) {
                    stream->set_state(QUICStreamState::HalfClosedRemote);
                } else if (stream->state() == QUICStreamState::HalfClosedLocal) {
                    stream->set_state(QUICStreamState::Closed);
                }
            }
        }
    }

    frames_out = std::move(frames);
    return true;
}

std::vector<uint8_t> QUICConnection::build_packet(
    QUICPacketType type, const std::vector<QUICStreamFrame>& frames) const {
    return serialize_packet(type, frames);
}

Task QUICConnection::close() {
    state_ = QUICConnectionState::Closing;
    // Send CONNECTION_CLOSE frame
    state_ = QUICConnectionState::Closed;
    co_return;
}

bool QUICConnection::parse_packet(const std::vector<uint8_t>& packet, QUICPacketType& type,
                                  ConnectionID& dest_conn_id, ConnectionID& src_conn_id,
                                  std::vector<QUICStreamFrame>& frames) {
    if (packet.empty()) {
        return false;
    }

    uint8_t first_byte = packet[0];
    size_t offset = 1;

    // Check if long header (RFC9000 Section 17.2)
    if ((first_byte & 0x80) == 0) {
        // Long header packet
        uint8_t packet_type = (first_byte >> 4) & 0x07;
        type = static_cast<QUICPacketType>(packet_type);

        // Version (4 bytes)
        if (packet.size() < offset + 4) {
            return false;
        }
        offset += 4;

        // Destination Connection ID Length
        if (packet.size() < offset + 1) {
            return false;
        }
        uint8_t dest_len = packet[offset++];
        if (dest_len > 0 && packet.size() >= offset + dest_len) {
            dest_conn_id.bytes.assign(packet.begin() + offset, packet.begin() + offset + dest_len);
            offset += dest_len;
        }

        // Source Connection ID Length
        if (packet.size() < offset + 1) {
            return false;
        }
        uint8_t src_len = packet[offset++];
        if (src_len > 0 && packet.size() >= offset + src_len) {
            src_conn_id.bytes.assign(packet.begin() + offset, packet.begin() + offset + src_len);
            offset += src_len;
        }
    } else {
        // Short header packet (OneRTT)
        type = QUICPacketType::OneRTT;
        dest_conn_id = dest_conn_id_; // Use connection's destination ID
    }

    // Parse frames (simplified - just look for STREAM frames)
    // In a full implementation, we'd parse all frame types
    while (offset < packet.size()) {
        if (packet.size() < offset + 1) {
            break;
        }

        uint8_t frame_type = packet[offset++];

        // STREAM frame (RFC9000 Section 19.8)
        if ((frame_type & 0x08) != 0) {
            QUICStreamFrame frame;

            // Stream ID length + Stream ID (toy encoding: 1 byte length, then big-endian ID)
            if (packet.size() < offset + 1) {
                break;
            }
            uint8_t stream_id_len = packet[offset++];
            if (stream_id_len == 0 || packet.size() < offset + stream_id_len) {
                break;
            }
            uint64_t stream_id = 0;
            for (int i = 0; i < stream_id_len; ++i) {
                stream_id = (stream_id << 8) | packet[offset++];
            }
            frame.stream_id = stream_id;

            // Offset (if present)
            if ((frame_type & 0x04) != 0) {
                if (packet.size() < offset + 1) {
                    break;
                }
                uint8_t offset_len = packet[offset++];
                if (packet.size() < offset + offset_len) {
                    break;
                }
                frame.offset = 0;
                for (int i = 0; i < offset_len; ++i) {
                    frame.offset = (frame.offset << 8) | packet[offset++];
                }
            } else {
                frame.offset = 0;
            }

            // Length (if present)
            uint64_t length = 0;
            if ((frame_type & 0x02) != 0) {
                if (packet.size() < offset + 1) {
                    break;
                }
                uint8_t length_len = packet[offset++];
                if (packet.size() < offset + length_len) {
                    break;
                }
                for (int i = 0; i < length_len; ++i) {
                    length = (length << 8) | packet[offset++];
                }
            } else {
                length = packet.size() - offset;
            }

            // Data
            if (packet.size() >= offset + length) {
                frame.data.assign(packet.begin() + offset, packet.begin() + offset + length);
                offset += length;
            }

            frame.fin = (frame_type & 0x01) != 0;
            frames.push_back(frame);
        } else {
            // Skip unknown frame types
            break;
        }
    }

    return true;
}

std::vector<uint8_t> QUICConnection::serialize_packet(
    QUICPacketType type, const std::vector<QUICStreamFrame>& frames) const {
    std::vector<uint8_t> packet;

    if (type == QUICPacketType::OneRTT) {
        // Short header (RFC9000 Section 17.3.1)
        // Use bit 7 set to distinguish from long header in this toy parser.
        packet.push_back(0x80); // Short header marker (toy)
    } else {
        // Long header
        uint8_t first_byte = static_cast<uint8_t>(type) << 4;
        packet.push_back(first_byte);

        // Version (RFC9000 version 1 = 0x00000001)
        packet.push_back(0x00);
        packet.push_back(0x00);
        packet.push_back(0x00);
        packet.push_back(0x01);

        // Destination Connection ID
        packet.push_back(static_cast<uint8_t>(dest_conn_id_.bytes.size()));
        packet.insert(packet.end(), dest_conn_id_.bytes.begin(), dest_conn_id_.bytes.end());

        // Source Connection ID
        packet.push_back(static_cast<uint8_t>(src_conn_id_.bytes.size()));
        packet.insert(packet.end(), src_conn_id_.bytes.begin(), src_conn_id_.bytes.end());
    }

    // Serialize frames
    for (const auto& frame : frames) {
        // STREAM frame
        uint8_t frame_type = 0x08; // STREAM frame base (toy)

        // Offset present
        if (frame.offset > 0) {
            frame_type |= 0x04;
        }

        // Length present
        frame_type |= 0x02;

        // FIN bit
        if (frame.fin) {
            frame_type |= 0x01;
        }

        packet.push_back(frame_type);

        // Stream ID length (toy encoding) + Stream ID (big-endian)
        uint64_t stream_id = frame.stream_id;
        int stream_id_bytes = 0;
        if (stream_id < (1ULL << 8)) {
            stream_id_bytes = 1;
        } else if (stream_id < (1ULL << 16)) {
            stream_id_bytes = 2;
        } else if (stream_id < (1ULL << 24)) {
            stream_id_bytes = 3;
        } else {
            stream_id_bytes = 4;
        }
        packet.push_back(static_cast<uint8_t>(stream_id_bytes));

        // Stream ID
        for (int i = stream_id_bytes - 1; i >= 0; --i) {
            packet.push_back(static_cast<uint8_t>((stream_id >> (i * 8)) & 0xFF));
        }

        // Offset
        if (frame.offset > 0) {
            int offset_bytes = 0;
            uint64_t off = frame.offset;
            while (off > 0) {
                offset_bytes++;
                off >>= 8;
            }
            packet.push_back(static_cast<uint8_t>(offset_bytes));
            off = frame.offset;
            for (int i = offset_bytes - 1; i >= 0; --i) {
                packet.push_back(static_cast<uint8_t>((off >> (i * 8)) & 0xFF));
            }
        }

        // Length
        uint64_t length = frame.data.size();
        int length_bytes = 0;
        uint64_t len = length;
        while (len > 0) {
            length_bytes++;
            len >>= 8;
        }
        if (length_bytes == 0) {
            length_bytes = 1;
        }
        packet.push_back(static_cast<uint8_t>(length_bytes));
        len = length;
        for (int i = length_bytes - 1; i >= 0; --i) {
            packet.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }

        // Data
        packet.insert(packet.end(), frame.data.begin(), frame.data.end());
    }

    return packet;
}

} // namespace spaznet
