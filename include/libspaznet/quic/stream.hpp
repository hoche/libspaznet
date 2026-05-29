#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace spaznet {
namespace quic {

// QUIC stream type encoded in the low two bits of the stream ID
// (RFC 9000 §2.1). Bit 0: 0 = client-initiated, 1 = server-initiated.
// Bit 1: 0 = bidirectional, 1 = unidirectional.
enum class StreamType : uint8_t {
    ClientBidi = 0,
    ServerBidi = 1,
    ClientUni = 2,
    ServerUni = 3,
};

auto stream_type(uint64_t stream_id) -> StreamType;
auto is_bidi(uint64_t stream_id) -> bool;
auto is_client_initiated(uint64_t stream_id) -> bool;

// Receive-side state per RFC 9000 §3.2.
enum class RecvState : uint8_t {
    Recv,      // accepting data
    SizeKnown, // FIN received but unread data remains
    DataRecvd, // all data received
    DataRead,  // application has read all data
    ResetRecvd,
    ResetRead,
};

// Send-side state per RFC 9000 §3.1.
enum class SendState : uint8_t {
    Ready,
    Send,
    DataSent,
    DataRecvd,
    ResetSent,
    ResetRecvd,
};

// One stream: handles ordered reassembly on the receive side, a byte
// queue on the send side, and per-direction flow control accounting.
//
// Receive side:
//   - `deliver(offset, data, fin)` inserts a chunk into the reassembler.
//     Out-of-order, duplicate, and partially-overlapping chunks are all
//     supported.
//   - `read_contiguous(out)` drains as many bytes as are contiguously
//     available starting at the current read offset.
//
// Send side:
//   - `write(bytes, fin)` appends to the send queue.
//   - `pull_send(max_bytes, offset_out, data_out, fin_out)` extracts up
//     to `max_bytes` consecutive bytes ready to put on the wire, along
//     with their starting offset and whether the FIN should be set on
//     this chunk. The bytes are NOT removed from the queue yet — they
//     stay buffered for retransmission until acked.
//   - `on_acked(offset, length)` releases buffered send bytes that have
//     been acknowledged. When the entire send buffer (including FIN)
//     is acked the send side transitions to DataRecvd.
//   - `on_lost(offset, length)` re-queues bytes for retransmission.
class Stream {
  public:
    Stream(uint64_t stream_id, uint64_t initial_max_recv, uint64_t initial_max_send);

    [[nodiscard]] auto id() const -> uint64_t {
        return id_;
    }
    [[nodiscard]] auto recv_state() const -> RecvState {
        return recv_state_;
    }
    [[nodiscard]] auto send_state() const -> SendState {
        return send_state_;
    }

    // ---- Receive side --------------------------------------------------

    // Returns false if the chunk would push past the receive flow-control
    // limit (FLOW_CONTROL_ERROR — RFC 9000 §4.1).
    [[nodiscard]] auto deliver(uint64_t offset, const std::vector<uint8_t>& data, bool fin)
        -> bool;

    // Drain contiguous bytes starting at the current read cursor.
    // Returns the number of bytes copied into `out`. Sets `fin_out`
    // when the FIN has been reached and the buffer is exhausted.
    auto read_contiguous(std::vector<uint8_t>& out, bool& fin_out) -> std::size_t;

    // Update the local receive limit (i.e. extend the window). Returns
    // true if the limit actually grew; this is what the connection uses
    // to decide whether to emit a MAX_STREAM_DATA.
    auto set_recv_limit(uint64_t new_limit) -> bool;

    [[nodiscard]] auto recv_limit() const -> uint64_t {
        return recv_limit_;
    }
    // The largest absolute offset (exclusive) we've seen from the peer.
    [[nodiscard]] auto recv_high_water() const -> uint64_t {
        return recv_high_water_;
    }

    // Reset (peer sent RESET_STREAM). Returns false on protocol error.
    [[nodiscard]] auto reset_recvd(uint64_t final_size, uint64_t app_error) -> bool;
    [[nodiscard]] auto reset_error() const -> std::optional<uint64_t> {
        return reset_error_;
    }

    // ---- Send side -----------------------------------------------------

    auto write(const std::vector<uint8_t>& bytes, bool fin) -> void;

    // Pull bytes ready to send. Returns 0 if nothing to send.
    auto pull_send(std::size_t max_bytes, uint64_t& offset_out,
                   std::vector<uint8_t>& data_out, bool& fin_out) -> std::size_t;

    auto on_acked(uint64_t offset, std::size_t length) -> void;
    auto on_lost(uint64_t offset, std::size_t length) -> void;

    // Peer raised our send window.
    auto set_send_limit(uint64_t new_limit) -> void;
    [[nodiscard]] auto send_limit() const -> uint64_t {
        return send_limit_;
    }
    [[nodiscard]] auto send_high_water() const -> uint64_t {
        return send_high_water_;
    }
    [[nodiscard]] auto send_buffer_size() const -> std::size_t {
        return send_buf_.size();
    }

  private:
    // Receive-side data is stored as a sparse map keyed by offset; each
    // entry holds a contiguous chunk. We merge on insert. This is O(N)
    // in the number of holes; for QUIC streams the number of disjoint
    // chunks is bounded by the number of in-flight packets.
    std::map<uint64_t, std::vector<uint8_t>> recv_chunks_;
    uint64_t recv_read_offset_{0};
    uint64_t recv_high_water_{0};
    uint64_t recv_limit_{0};
    std::optional<uint64_t> recv_final_size_;

    // Send-side buffer. We keep all unacked bytes contiguous starting
    // at send_buf_base_offset_. Bytes the peer has acked at the front
    // are erased and the base offset moved forward.
    std::vector<uint8_t> send_buf_;
    uint64_t send_buf_base_offset_{0};
    // Next offset we'll hand out on the wire (incremented by pull_send).
    uint64_t send_next_offset_{0};
    // Highest absolute offset we've ever written (== base + buf.size()).
    uint64_t send_high_water_{0};
    uint64_t send_limit_{0};
    bool send_fin_queued_{false};
    bool send_fin_sent_{false};
    bool send_fin_acked_{false};

    // For retransmission, track which ranges are still in flight vs
    // already acked. The send buffer holds bytes from send_buf_base_offset_
    // up to send_buf_base_offset_ + send_buf_.size(); acked_ranges_ are
    // (offset, length) pairs of bytes we've received an ACK for but that
    // we still hold in the buffer because earlier bytes aren't acked yet.
    std::vector<std::pair<uint64_t, std::size_t>> acked_ranges_;
    // Bytes we need to retransmit (subset of [base, base+size) but
    // re-emitted with their own offsets). When non-empty, pull_send
    // services these before fresh bytes.
    std::vector<std::pair<uint64_t, std::size_t>> lost_ranges_;

    std::optional<uint64_t> reset_error_;
    uint64_t id_;
    RecvState recv_state_{RecvState::Recv};
    SendState send_state_{SendState::Ready};

    auto try_advance_send_state_after_ack() -> void;
};

} // namespace quic
} // namespace spaznet
