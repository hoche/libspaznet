#pragma once

#include <chrono>
#include <cstdint>

namespace spaznet {
namespace quic {

using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::nanoseconds;

// RTT measurement + loss detection state (RFC 9002).
//
// Loss detection is performed by the connection layer; this object
// tracks the inputs (RTT samples, PTO backoff, time threshold) and
// exposes helpers for the connection to use.
class Recovery {
  public:
    // Defaults from RFC 9002 §6.2.2.
    static constexpr Duration kInitialRtt = std::chrono::milliseconds(333);
    static constexpr Duration kGranularity = std::chrono::milliseconds(1);
    static constexpr uint32_t kPacketThreshold = 3;
    static constexpr double kTimeThreshold = 9.0 / 8.0;

    // Record a new RTT sample. `ack_delay` is the peer's advertised ACK
    // delay (already in real-time units, not encoded). Per RFC 9002
    // §5.3 we subtract ack_delay when min_rtt has been established.
    auto on_rtt_sample(Duration latest, Duration ack_delay) -> void;

    [[nodiscard]] auto smoothed_rtt() const -> Duration {
        return smoothed_rtt_;
    }
    [[nodiscard]] auto rttvar() const -> Duration {
        return rttvar_;
    }
    [[nodiscard]] auto min_rtt() const -> Duration {
        return min_rtt_;
    }
    [[nodiscard]] auto latest_rtt() const -> Duration {
        return latest_rtt_;
    }
    [[nodiscard]] auto have_sample() const -> bool {
        return have_sample_;
    }

    // PTO timeout (RFC 9002 §6.2). `max_ack_delay` is the peer-advertised
    // maximum-acknowledgment-delay transport parameter.
    [[nodiscard]] auto pto_timeout(Duration max_ack_delay) const -> Duration;

    // Bump the PTO backoff exponent (called when PTO fires).
    auto on_pto_fired() -> void {
        ++pto_count_;
    }
    // Reset PTO count when we receive an ack-eliciting ack.
    auto reset_pto() -> void {
        pto_count_ = 0;
    }
    [[nodiscard]] auto pto_count() const -> uint32_t {
        return pto_count_;
    }

    // Loss-detection time threshold: how long after a packet was sent
    // before we declare it lost (RFC 9002 §6.1.2).
    [[nodiscard]] auto loss_time_threshold() const -> Duration;

  private:
    Duration latest_rtt_{0};
    Duration smoothed_rtt_{kInitialRtt};
    Duration rttvar_{kInitialRtt / 2};
    Duration min_rtt_{Duration::max()};
    bool have_sample_{false};
    uint32_t pto_count_{0};
};

} // namespace quic
} // namespace spaznet
