#pragma once

#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spaznet {

// Forward declaration
class Socket;

// QUIC Connection ID (RFC9000 Section 5.1)
struct ConnectionID {
    std::vector<uint8_t> bytes;

    bool operator==(const ConnectionID& other) const {
        return bytes == other.bytes;
    }
};

// QUIC Packet Types (RFC9000 Section 17)
enum class QUICPacketType : uint8_t {
    Initial = 0x00,
    ZeroRTT = 0x01,
    Handshake = 0x02,
    Retry = 0x03,
    OneRTT = 0x80, // Short header packet
};

// QUIC Stream Frame (RFC9000 Section 19.8)
struct QUICStreamFrame {
    uint64_t stream_id;
    uint64_t offset;
    std::vector<uint8_t> data;
    bool fin;
};

// QUIC Connection State
enum class QUICConnectionState {
    Idle,
    Handshake,
    Established,
    Closing,
    Closed,
};

// QUIC Stream State
enum class QUICStreamState {
    Idle,
    Open,
    HalfClosedLocal,
    HalfClosedRemote,
    Closed,
};

// QUIC Stream (RFC9000 Section 2)
class QUICStream {
  public:
    QUICStream(uint64_t stream_id, bool bidirectional);
    ~QUICStream() = default;

    uint64_t stream_id() const {
        return stream_id_;
    }
    bool bidirectional() const {
        return bidirectional_;
    }
    QUICStreamState state() const {
        return state_;
    }

    // Read data from stream
    Task read(std::vector<uint8_t>& buffer, std::size_t max_size);

    // Write data to stream
    Task write(const std::vector<uint8_t>& data, bool fin = false);

    // Close stream
    void close();

    // Set stream state (for QUICConnection)
    void set_state(QUICStreamState state) {
        state_ = state;
    }

    // Internal access for QUICConnection
    std::vector<uint8_t> receive_buffer_;
    std::vector<uint8_t> send_buffer_;
    uint64_t receive_offset_;
    uint64_t send_offset_;
    bool receive_fin_;
    bool send_fin_;

  private:
    uint64_t stream_id_;
    bool bidirectional_;
    QUICStreamState state_;
};

// QUIC Connection (RFC9000)
class QUICConnection {
  public:
    QUICConnection(ConnectionID dest_conn_id, ConnectionID src_conn_id, Socket& socket);
    ~QUICConnection() = default;

    ConnectionID destination_connection_id() const {
        return dest_conn_id_;
    }
    ConnectionID source_connection_id() const {
        return src_conn_id_;
    }
    QUICConnectionState state() const {
        return state_;
    }

    // Get or create stream
    std::shared_ptr<QUICStream> get_stream(uint64_t stream_id);

    // Process incoming QUIC packet
    Task process_packet(const std::vector<uint8_t>& packet);

    // Send data on stream
    Task send_stream_data(uint64_t stream_id, const std::vector<uint8_t>& data, bool fin = false);

    // Close connection
    Task close();

  private:
    ConnectionID dest_conn_id_;
    ConnectionID src_conn_id_;
    Socket& socket_;
    QUICConnectionState state_;
    std::unordered_map<uint64_t, std::shared_ptr<QUICStream>> streams_;
    uint64_t next_stream_id_;
    uint64_t max_stream_id_;

    // Parse QUIC packet
    bool parse_packet(const std::vector<uint8_t>& packet, QUICPacketType& type,
                      ConnectionID& dest_conn_id, ConnectionID& src_conn_id,
                      std::vector<QUICStreamFrame>& frames);

    // Serialize QUIC packet
    std::vector<uint8_t> serialize_packet(QUICPacketType type,
                                          const std::vector<QUICStreamFrame>& frames);
};

// QUIC Handler interface
class QUICHandler {
  public:
    virtual ~QUICHandler() = default;

    // Handle new QUIC connection
    virtual Task on_connection(std::shared_ptr<QUICConnection> connection) = 0;

    // Handle stream data
    virtual Task on_stream_data(std::shared_ptr<QUICConnection> connection,
                                std::shared_ptr<QUICStream> stream,
                                const std::vector<uint8_t>& data, bool fin) = 0;
};

} // namespace spaznet
