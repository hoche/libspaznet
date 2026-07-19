#include <libspaznet/quic/pn_space.hpp>

#include <algorithm>

namespace spaznet {
namespace quic {

auto PnSpace::on_received(uint64_t pn, bool ack_eliciting) -> void {
    if (ack_eliciting) {
        needs_ack_ = true;
    }
    if (!largest_received_ || pn > *largest_received_) {
        largest_received_ = pn;
    }

    // Insertion: find the place where `pn` could merge with a range.
    // ranges_ is kept sorted by descending `hi`. We walk in that order,
    // merge into adjacent ranges, and re-sort if a merge expanded a
    // range past its neighbours.
    auto it = ranges_.begin();
    while (it != ranges_.end() && it->hi >= pn) {
        if (it->lo <= pn) {
            // already covered
            return;
        }
        ++it;
    }
    // it now points at the first range whose hi < pn (or end()).

    // Try to extend a higher range downward (hi >= pn but lo > pn → lo == pn+1).
    if (it != ranges_.begin()) {
        auto higher = std::prev(it);
        if (higher->lo == pn + 1) {
            higher->lo = pn;
            // Maybe also merges with the next-lower range (it->hi == pn-1).
            if (it != ranges_.end() && it->hi + 1 == higher->lo) {
                higher->lo = it->lo;
                ranges_.erase(it);
            }
            return;
        }
    }

    // Try to extend a lower range upward (hi == pn-1).
    if (it != ranges_.end() && it->hi + 1 == pn) {
        it->hi = pn;
        return;
    }

    // Otherwise insert a new singleton range at the correct sorted spot.
    ranges_.insert(it, Range{pn, pn});

    // Bound the number of retained ACK ranges. A peer that sends packet
    // numbers with deliberate gaps (e.g. only odd PNs) would otherwise make
    // ranges_ — and the ACK frame we build from it — grow without limit.
    // ranges_ is sorted by descending `hi`, so dropping the tail discards
    // the oldest (lowest-PN) ranges; not acking very old packets is
    // harmless (RFC 9000 §13.2.1 does not require acking every packet).
    constexpr std::size_t kMaxAckRanges = 32;
    if (ranges_.size() > kMaxAckRanges) {
        ranges_.resize(kMaxAckRanges);
    }
}

auto PnSpace::largest_received() const -> std::optional<uint64_t> {
    return largest_received_;
}

auto PnSpace::build_ack_frame(uint64_t ack_delay_us, AckFrame& out) -> void {
    out = AckFrame{};
    if (ranges_.empty()) {
        return;
    }
    out.largest_acked = ranges_.front().hi;
    out.ack_delay = ack_delay_us;
    out.first_range = ranges_.front().hi - ranges_.front().lo;
    // Subsequent ranges: gap and length per RFC 9000 §19.3.1. The "gap"
    // is the number of PNs between this range's smallest PN and the
    // previous (larger) range's smallest-1.
    for (std::size_t i = 1; i < ranges_.size(); ++i) {
        const auto& prev = ranges_[i - 1];
        const auto& cur = ranges_[i];
        AckRange r;
        // Gap = (smallest of previous range - 1) - largest of this range - 1
        r.gap = prev.lo - cur.hi - 2;
        r.length = cur.hi - cur.lo;
        out.ranges.push_back(r);
    }
    needs_ack_ = false;
}

auto PnSpace::trim_below(uint64_t pn) -> void {
    while (!ranges_.empty() && ranges_.back().hi < pn) {
        ranges_.pop_back();
    }
    if (!ranges_.empty() && ranges_.back().lo < pn) {
        ranges_.back().lo = pn;
    }
}

auto PnSpace::reset() -> void {
    ranges_.clear();
    largest_received_.reset();
    needs_ack_ = false;
}

} // namespace quic
} // namespace spaznet
