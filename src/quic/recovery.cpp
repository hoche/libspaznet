#include <libspaznet/quic/recovery.hpp>

#include <algorithm>
#include <chrono>

namespace spaznet {
namespace quic {

auto Recovery::on_rtt_sample(Duration latest, Duration ack_delay) -> void {
    latest_rtt_ = latest;
    if (!have_sample_) {
        min_rtt_ = latest;
        smoothed_rtt_ = latest;
        rttvar_ = latest / 2;
        have_sample_ = true;
        return;
    }
    // min_rtt ignores ack_delay (RFC 9002 §5.2).
    min_rtt_ = std::min(min_rtt_, latest);

    // Adjusted RTT: subtract ack_delay only if doing so keeps the
    // sample above min_rtt (RFC 9002 §5.3).
    Duration adjusted = latest;
    if (latest >= min_rtt_ + ack_delay) {
        adjusted = latest - ack_delay;
    }
    // smoothed = 7/8 * smoothed + 1/8 * adjusted
    auto sm_ns = smoothed_rtt_.count();
    auto adj_ns = adjusted.count();
    sm_ns = (7 * sm_ns + adj_ns) / 8;
    smoothed_rtt_ = Duration{sm_ns};
    // rttvar = 3/4 * rttvar + 1/4 * |smoothed - adjusted|
    auto diff = sm_ns > adj_ns ? (sm_ns - adj_ns) : (adj_ns - sm_ns);
    auto rv_ns = rttvar_.count();
    rv_ns = (3 * rv_ns + diff) / 4;
    rttvar_ = Duration{rv_ns};
}

auto Recovery::pto_timeout(Duration max_ack_delay) const -> Duration {
    // PTO = smoothed_rtt + max(4*rttvar, granularity) + max_ack_delay
    // multiplied by 2^pto_count (RFC 9002 §6.2.1).
    Duration base = smoothed_rtt_ + std::max(rttvar_ * 4, kGranularity) + max_ack_delay;
    if (pto_count_ == 0) {
        return base;
    }
    // Shift by pto_count_ — cap to prevent overflow at unreasonable values.
    uint32_t shift = std::min<uint32_t>(pto_count_, 30);
    return Duration{base.count() << shift};
}

auto Recovery::loss_time_threshold() const -> Duration {
    // max(smoothed_rtt, latest_rtt) * 9/8 (RFC 9002 §6.1.2).
    Duration base = std::max(smoothed_rtt_, latest_rtt_);
    auto ns = base.count();
    return Duration{(ns * 9) / 8};
}

} // namespace quic
} // namespace spaznet
