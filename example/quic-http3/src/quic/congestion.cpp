#include <libspaznet/quic/congestion.hpp>

#include <algorithm>

namespace spaznet {
namespace quic {

Congestion::Congestion(std::size_t max_datagram_size)
    : max_datagram_size_(max_datagram_size), cwnd_(max_datagram_size * kInitialWindowPackets) {}

auto Congestion::on_packets_acked(TimePoint now, std::size_t bytes_acked) -> void {
    if (bytes_acked > bytes_in_flight_) {
        bytes_in_flight_ = 0;
    } else {
        bytes_in_flight_ -= bytes_acked;
    }
    // Don't increase the window while still recovering from prior loss.
    if (in_recovery(now)) {
        return;
    }
    if (cwnd_ < ssthresh_) {
        // Slow start.
        cwnd_ += bytes_acked;
    } else {
        // Congestion avoidance.
        cwnd_ += (max_datagram_size_ * bytes_acked) / cwnd_;
    }
}

auto Congestion::on_packets_lost(TimePoint now, std::size_t bytes_lost,
                                 TimePoint largest_lost_time) -> void {
    if (bytes_lost > bytes_in_flight_) {
        bytes_in_flight_ = 0;
    } else {
        bytes_in_flight_ -= bytes_lost;
    }
    // Only enter recovery once per RTT (RFC 9002 §7.6.2): if the loss
    // is for a packet sent before our current recovery epoch started,
    // we already accounted for it.
    if (recovery_start_time_ && largest_lost_time <= *recovery_start_time_) {
        (void)now;
        return;
    }
    enter_recovery(now);
}

auto Congestion::enter_recovery(TimePoint now) -> void {
    ssthresh_ = static_cast<std::size_t>(cwnd_ * kLossReductionFactor);
    const std::size_t min_window = max_datagram_size_ * kMinWindowPackets;
    cwnd_ = std::max(ssthresh_, min_window);
    recovery_start_time_ = now;
}

} // namespace quic
} // namespace spaznet
