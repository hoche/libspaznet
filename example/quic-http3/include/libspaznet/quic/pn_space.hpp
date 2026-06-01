#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <libspaznet/quic/frame.hpp>

namespace spaznet {
namespace quic {

// Per-packet-number-space ACK bookkeeping.
//
// Tracks every packet we've received in this PN space, in compressed
// form (sorted-descending list of disjoint ranges), so we can emit an
// ACK frame on demand (RFC 9000 §19.3). Also tracks whether the space
// has any unacked ack-eliciting packets that warrant sending an ACK in
// the next outgoing packet (RFC 9000 §13.2.1).
//
// One instance per encryption level (Initial, Handshake, Application).
class PnSpace {
  public:
    // Record that we received `pn`. `ack_eliciting` follows the RFC 9000
    // §13.2.1 definition: any frame other than ACK / PADDING /
    // CONNECTION_CLOSE qualifies.
    auto on_received(uint64_t pn, bool ack_eliciting) -> void;

    // Largest packet number received so far, or std::nullopt if nothing.
    [[nodiscard]] auto largest_received() const -> std::optional<uint64_t>;

    // Whether at least one ack-eliciting packet has arrived since we
    // last emitted an ACK.
    [[nodiscard]] auto needs_ack() const -> bool {
        return needs_ack_;
    }

    // Build an ACK frame describing every packet number we hold. Clears
    // the needs_ack_ flag. `ack_delay_us` is the microseconds-elapsed
    // value the caller computed; we leave the encoding-scale exponent
    // to the connection layer (RFC 9000 §19.3.2 — the value handed to
    // the AckFrame is the raw exponent-scaled count).
    auto build_ack_frame(uint64_t ack_delay_us, AckFrame& out) -> void;

    // Receiver-side trimming: once `pn` is known to be smaller than any
    // PN the peer could still re-send (because the peer has acked our
    // ACK that included it), trim ranges below `pn` to bound memory.
    // RFC 9000 §13.2 calls this "Limiting Ranges by Tracking ACK Frames".
    auto trim_below(uint64_t pn) -> void;

    // Reset all state.
    auto reset() -> void;

  private:
    // Disjoint half-open intervals [lo, hi] of received PNs, sorted
    // descending by hi. We merge on insert.
    struct Range {
        uint64_t lo;
        uint64_t hi;
    };
    std::vector<Range> ranges_;
    std::optional<uint64_t> largest_received_;
    bool needs_ack_{false};
};

} // namespace quic
} // namespace spaznet
