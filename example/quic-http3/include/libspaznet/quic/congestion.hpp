#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <libspaznet/quic/recovery.hpp>

namespace spaznet {
namespace quic {

// NewReno congestion control for QUIC (RFC 9002 Appendix B).
//
// Operates in bytes of in-flight data. The connection layer calls:
//   - on_packet_sent(time, bytes) for every ack-eliciting packet,
//   - on_packets_acked(time, total_bytes) after processing an ACK,
//   - on_packets_lost(time, total_bytes, largest_lost_time) when loss
//     is declared.
// The congestion window is read via congestion_window() and the
// per-direction "can I send now?" check via can_send(bytes).
class Congestion {
  public:
    static constexpr std::size_t kMaxDatagramSize = 1200;
    static constexpr std::size_t kInitialWindowPackets = 10; // RFC 9002 §B.1
    static constexpr std::size_t kMinWindowPackets = 2;
    static constexpr double kLossReductionFactor = 0.5;

    explicit Congestion(std::size_t max_datagram_size = kMaxDatagramSize);

    auto on_packet_sent(TimePoint, std::size_t bytes) -> void {
        bytes_in_flight_ += bytes;
    }
    auto on_packets_acked(TimePoint now, std::size_t bytes_acked) -> void;
    auto on_packets_lost(TimePoint now, std::size_t bytes_lost, TimePoint largest_lost_time)
        -> void;

    [[nodiscard]] auto congestion_window() const -> std::size_t {
        return cwnd_;
    }
    [[nodiscard]] auto bytes_in_flight() const -> std::size_t {
        return bytes_in_flight_;
    }
    [[nodiscard]] auto in_recovery(TimePoint now) const -> bool {
        return recovery_start_time_ && now > *recovery_start_time_;
    }
    [[nodiscard]] auto can_send(std::size_t bytes) const -> bool {
        return bytes_in_flight_ + bytes <= cwnd_;
    }

  private:
    auto enter_recovery(TimePoint now) -> void;

    std::size_t max_datagram_size_;
    std::size_t cwnd_;
    std::size_t bytes_in_flight_{0};
    std::size_t ssthresh_{SIZE_MAX};
    std::optional<TimePoint> recovery_start_time_;
};

} // namespace quic
} // namespace spaznet
