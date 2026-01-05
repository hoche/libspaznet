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

    auto operator==(const ConnectionID& other) const -> bool {
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
enum class QUICConnectionState : uint8_t {
    Idle,
    Handshake,
    Established,
    Closing,
    Closed,
};

// QUIC Stream State
enum class QUICStreamState : uint8_t {
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

    // Delete copy and move operations
    QUICStream(const QUICStream&) = delete;
    auto operator=(const QUICStream&) -> QUICStream& = delete;
    QUICStream(QUICStream&&) = delete;
    auto operator=(QUICStream&&) -> QUICStream& = delete;

    [[nodiscard]] auto stream_id() const -> uint64_t {
        return stream_id_;
    }
    [[nodiscard]] auto bidirectional() const -> bool {
        return bidirectional_;
    }
    [[nodiscard]] auto state() const -> QUICStreamState {
        return state_;
    }

    // Read data from stream
    auto read(std::vector<uint8_t>& buffer, std::size_t max_size) -> Task;

    // Write data to stream
    auto write(const std::vector<uint8_t>& data, bool fin = false) -> Task;

    // Close stream
    auto close() -> void;

    // Set stream state (for QUICConnection)
    auto set_state(QUICStreamState new_state) -> void {
        state_ = new_state;
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
    QUICConnection(ConnectionID dest_conn_id, ConnectionID src_conn_id);
    ~QUICConnection() = default;

    // Delete copy and move operations
    QUICConnection(const QUICConnection&) = delete;
    auto operator=(const QUICConnection&) -> QUICConnection& = delete;
    QUICConnection(QUICConnection&&) = delete;
    auto operator=(QUICConnection&&) -> QUICConnection& = delete;

    auto destination_connection_id() const -> ConnectionID {
        return dest_conn_id_;
    }
    auto source_connection_id() const -> ConnectionID {
        return src_conn_id_;
    }
    auto state() const -> QUICConnectionState {
        return state_;
    }

    // Get or create stream
    auto get_stream(uint64_t stream_id) -> std::shared_ptr<QUICStream>;

    // Process incoming QUIC packet
    // Returns true on successful parse; `frames_out` will contain any STREAM frames.
    auto process_packet(const std::vector<uint8_t>& packet,
                        std::vector<QUICStreamFrame>& frames_out) -> bool;

    // Build a QUIC packet containing STREAM frames (toy implementation).
    [[nodiscard]] auto build_packet(QUICPacketType type, const std::vector<QUICStreamFrame>& frames)
        const -> std::vector<uint8_t>;

    // Close connection
    auto close() -> Task;

  private:
    ConnectionID dest_conn_id_;
    ConnectionID src_conn_id_;
    QUICConnectionState state_;
    std::unordered_map<uint64_t, std::shared_ptr<QUICStream>> streams_;
    uint64_t next_stream_id_;
    uint64_t max_stream_id_;

    // Parse QUIC packet
    auto parse_packet(const std::vector<uint8_t>& packet, QUICPacketType& type,
                      ConnectionID& dest_conn_id, ConnectionID& src_conn_id,
                      std::vector<QUICStreamFrame>& frames) -> bool;

    // Serialize QUIC packet
    auto serialize_packet(QUICPacketType type,
                          const std::vector<QUICStreamFrame>& frames) const -> std::vector<uint8_t>;
};

// QUIC Handler interface
class QUICHandler {
  public:
    QUICHandler() = default;
    virtual ~QUICHandler() = default;

    // Delete copy and move operations
    QUICHandler(const QUICHandler&) = delete;
    auto operator=(const QUICHandler&) -> QUICHandler& = delete;
    QUICHandler(QUICHandler&&) = delete;
    auto operator=(QUICHandler&&) -> QUICHandler& = delete;

    // Handle new QUIC connection
    virtual auto on_connection(std::shared_ptr<QUICConnection> connection) -> Task = 0;

    // Handle stream data
    virtual auto on_stream_data(std::shared_ptr<QUICConnection> connection,
                                std::shared_ptr<QUICStream> stream,
                                const std::vector<uint8_t>& data, bool fin) -> Task = 0;
};

} // namespace spaznet
