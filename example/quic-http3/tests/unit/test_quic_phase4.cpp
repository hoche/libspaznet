// Phase 4 verification: PN space ACK bookkeeping, stream state machine
// and reassembly buffer, RTT/PTO math, and NewReno congestion control.

#include <gtest/gtest.h>

#include <libspaznet/quic/congestion.hpp>
#include <libspaznet/quic/frame.hpp>
#include <libspaznet/quic/pn_space.hpp>
#include <libspaznet/quic/recovery.hpp>
#include <libspaznet/quic/stream.hpp>

#include <chrono>
#include <vector>

using namespace spaznet::quic;
using namespace std::chrono_literals;

// ---------- PnSpace -------------------------------------------------------

TEST(QuicPnSpace, BuildsAckFrameForContiguousRange) {
    PnSpace s;
    for (uint64_t pn = 0; pn <= 10; ++pn) {
        s.on_received(pn, /*ack_eliciting=*/true);
    }
    AckFrame f;
    s.build_ack_frame(0, f);
    EXPECT_EQ(f.largest_acked, 10U);
    EXPECT_EQ(f.first_range, 10U);
    EXPECT_TRUE(f.ranges.empty());
    EXPECT_FALSE(s.needs_ack());
}

TEST(QuicPnSpace, OutOfOrderProducesGaps) {
    PnSpace s;
    s.on_received(0, true);
    s.on_received(1, true);
    s.on_received(5, true);
    s.on_received(6, true);
    s.on_received(10, true);
    AckFrame f;
    s.build_ack_frame(0, f);
    EXPECT_EQ(f.largest_acked, 10U);
    EXPECT_EQ(f.first_range, 0U);
    ASSERT_EQ(f.ranges.size(), 2U);
    // range[0]: between 10 and {6,5} — gap = 10 - 1 - 6 - 1 = 2 ; length = 6-5 = 1
    EXPECT_EQ(f.ranges[0].gap, 2U);
    EXPECT_EQ(f.ranges[0].length, 1U);
    // range[1]: between 5 and {1,0} — gap = 5 - 1 - 1 - 1 = 2 ; length = 1-0 = 1
    EXPECT_EQ(f.ranges[1].gap, 2U);
    EXPECT_EQ(f.ranges[1].length, 1U);
}

TEST(QuicPnSpace, MergesAdjacentSinglesIntoRange) {
    PnSpace s;
    // Insert 5 first, then 3, then 4 — should coalesce to [3,5].
    s.on_received(5, true);
    s.on_received(3, true);
    s.on_received(4, true);
    AckFrame f;
    s.build_ack_frame(0, f);
    EXPECT_EQ(f.largest_acked, 5U);
    EXPECT_EQ(f.first_range, 2U);
    EXPECT_TRUE(f.ranges.empty());
}

TEST(QuicPnSpace, NonAckElicitingDoesNotSetNeedsAck) {
    PnSpace s;
    s.on_received(0, false);
    EXPECT_FALSE(s.needs_ack());
    s.on_received(1, true);
    EXPECT_TRUE(s.needs_ack());
}

// ---------- Stream --------------------------------------------------------

TEST(QuicStream, TypeBitsMatchRfc) {
    EXPECT_EQ(stream_type(0), StreamType::ClientBidi);
    EXPECT_EQ(stream_type(1), StreamType::ServerBidi);
    EXPECT_EQ(stream_type(2), StreamType::ClientUni);
    EXPECT_EQ(stream_type(3), StreamType::ServerUni);
    EXPECT_TRUE(is_bidi(0));
    EXPECT_TRUE(is_bidi(1));
    EXPECT_FALSE(is_bidi(2));
    EXPECT_TRUE(is_client_initiated(0));
    EXPECT_FALSE(is_client_initiated(1));
}

TEST(QuicStream, InOrderRecv) {
    Stream s(0, /*recv_limit=*/1000, /*send_limit=*/1000);
    ASSERT_TRUE(s.deliver(0, {1, 2, 3}, false));
    ASSERT_TRUE(s.deliver(3, {4, 5}, true));
    std::vector<uint8_t> out;
    bool fin = false;
    EXPECT_EQ(s.read_contiguous(out, fin), 5U);
    EXPECT_EQ(out, (std::vector<uint8_t>{1, 2, 3, 4, 5}));
    EXPECT_TRUE(fin);
    EXPECT_EQ(s.recv_state(), RecvState::DataRead);
}

TEST(QuicStream, OutOfOrderReassembly) {
    Stream s(0, 1000, 1000);
    ASSERT_TRUE(s.deliver(3, {4, 5, 6}, false));
    ASSERT_TRUE(s.deliver(0, {1, 2, 3}, false));
    std::vector<uint8_t> out;
    bool fin = false;
    EXPECT_EQ(s.read_contiguous(out, fin), 6U);
    EXPECT_EQ(out, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_FALSE(fin);
}

TEST(QuicStream, DuplicateAndOverlappingChunks) {
    Stream s(0, 1000, 1000);
    ASSERT_TRUE(s.deliver(0, {1, 2, 3, 4, 5}, false));
    // Same range again — no effect.
    ASSERT_TRUE(s.deliver(0, {1, 2, 3, 4, 5}, false));
    // Overlapping but extending.
    ASSERT_TRUE(s.deliver(3, {4, 5, 6, 7}, false));
    std::vector<uint8_t> out;
    bool fin = false;
    EXPECT_EQ(s.read_contiguous(out, fin), 7U);
    EXPECT_EQ(out, (std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7}));
}

TEST(QuicStream, FlowControlEnforced) {
    Stream s(0, /*recv_limit=*/4, /*send_limit=*/0);
    EXPECT_FALSE(s.deliver(0, {1, 2, 3, 4, 5}, false));
}

TEST(QuicStream, ReassemblerHandlesHole) {
    Stream s(0, 1000, 1000);
    ASSERT_TRUE(s.deliver(0, {1, 2}, false));
    ASSERT_TRUE(s.deliver(5, {6, 7}, false));
    std::vector<uint8_t> out;
    bool fin = false;
    EXPECT_EQ(s.read_contiguous(out, fin), 2U); // only the first chunk
    out.clear();
    EXPECT_EQ(s.read_contiguous(out, fin), 0U); // hole, stalls
    ASSERT_TRUE(s.deliver(2, {3, 4, 5}, false));
    out.clear();
    EXPECT_EQ(s.read_contiguous(out, fin), 5U); // rest pours through
    EXPECT_EQ(out, (std::vector<uint8_t>{3, 4, 5, 6, 7}));
}

TEST(QuicStream, SendPullsBytesRespectingFlowControl) {
    Stream s(0, 1000, /*send_limit=*/5);
    s.write({1, 2, 3, 4, 5, 6, 7, 8}, true);
    uint64_t off = 0;
    std::vector<uint8_t> data;
    bool fin = false;
    EXPECT_EQ(s.pull_send(100, off, data, fin), 5U); // bounded by send_limit
    EXPECT_EQ(off, 0U);
    EXPECT_EQ(data, (std::vector<uint8_t>{1, 2, 3, 4, 5}));
    EXPECT_FALSE(fin);
    // Window opens.
    s.set_send_limit(10);
    EXPECT_EQ(s.pull_send(100, off, data, fin), 3U);
    EXPECT_EQ(off, 5U);
    EXPECT_EQ(data, (std::vector<uint8_t>{6, 7, 8}));
    EXPECT_TRUE(fin);
}

TEST(QuicStream, SendRespectsConnectionLevelFreshBudget) {
    // Per-stream window is wide open; the connection-level budget
    // (max_fresh) is what limits fresh bytes here.
    Stream s(0, 1000, /*send_limit=*/1000);
    s.write({1, 2, 3, 4, 5, 6, 7, 8}, false);
    uint64_t off = 0;
    std::vector<uint8_t> data;
    bool fin = false;
    // Only 3 fresh bytes allowed by the connection budget.
    EXPECT_EQ(s.pull_send(100, off, data, fin, /*max_fresh=*/3), 3U);
    EXPECT_EQ(off, 0U);
    EXPECT_EQ(data, (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_EQ(s.send_next_offset(), 3U);
    // Budget exhausted: no fresh bytes come out.
    EXPECT_EQ(s.pull_send(100, off, data, fin, /*max_fresh=*/0), 0U);
    // Budget reopens; the rest flows.
    EXPECT_EQ(s.pull_send(100, off, data, fin, /*max_fresh=*/100), 5U);
    EXPECT_EQ(off, 3U);
    EXPECT_EQ(data, (std::vector<uint8_t>{4, 5, 6, 7, 8}));

    // Retransmissions ignore the connection budget entirely.
    s.on_lost(0, 3);
    EXPECT_EQ(s.pull_send(100, off, data, fin, /*max_fresh=*/0), 3U);
    EXPECT_EQ(off, 0U);
    EXPECT_EQ(data, (std::vector<uint8_t>{1, 2, 3}));
}

TEST(QuicStream, AckAdvancesSendStateToDataRecvd) {
    Stream s(0, 1000, 100);
    s.write({1, 2, 3}, true);
    uint64_t off = 0;
    std::vector<uint8_t> data;
    bool fin = false;
    ASSERT_EQ(s.pull_send(100, off, data, fin), 3U);
    EXPECT_TRUE(fin);
    EXPECT_EQ(s.send_state(), SendState::DataSent);
    s.on_acked(0, 3);
    EXPECT_EQ(s.send_state(), SendState::DataRecvd);
}

TEST(QuicStream, LostBytesRequeuedForRetransmit) {
    Stream s(0, 1000, 100);
    s.write({1, 2, 3, 4, 5}, false);
    uint64_t off = 0;
    std::vector<uint8_t> data;
    bool fin = false;
    ASSERT_EQ(s.pull_send(5, off, data, fin), 5U);
    s.on_lost(0, 5);
    // Pulling again should return the same bytes from offset 0.
    ASSERT_EQ(s.pull_send(5, off, data, fin), 5U);
    EXPECT_EQ(off, 0U);
    EXPECT_EQ(data, (std::vector<uint8_t>{1, 2, 3, 4, 5}));
}

TEST(QuicStream, ResetClearsRecvBuffer) {
    Stream s(0, 1000, 1000);
    ASSERT_TRUE(s.deliver(0, {1, 2}, false));
    ASSERT_TRUE(s.reset_recvd(/*final_size=*/2, /*app_error=*/0x42));
    EXPECT_EQ(s.recv_state(), RecvState::ResetRecvd);
    EXPECT_TRUE(s.reset_error().has_value());
    EXPECT_EQ(*s.reset_error(), 0x42U);
}

// ---------- Recovery ------------------------------------------------------

TEST(QuicRecovery, FirstSampleSeedsAllStats) {
    Recovery r;
    r.on_rtt_sample(100ms, 5ms);
    EXPECT_EQ(r.latest_rtt(), 100ms);
    EXPECT_EQ(r.smoothed_rtt(), 100ms);
    EXPECT_EQ(r.rttvar(), 50ms);
    EXPECT_EQ(r.min_rtt(), 100ms);
}

TEST(QuicRecovery, MinRttDecreasesOnLowerSample) {
    Recovery r;
    r.on_rtt_sample(100ms, 0ms);
    r.on_rtt_sample(40ms, 0ms);
    EXPECT_EQ(r.min_rtt(), 40ms);
}

TEST(QuicRecovery, PtoBacksOffExponentially) {
    Recovery r;
    r.on_rtt_sample(100ms, 0ms);
    auto base = r.pto_timeout(25ms);
    r.on_pto_fired();
    auto once = r.pto_timeout(25ms);
    r.on_pto_fired();
    auto twice = r.pto_timeout(25ms);
    EXPECT_EQ(once, base * 2);
    EXPECT_EQ(twice, base * 4);
}

TEST(QuicRecovery, LossTimeThresholdIs9_8thsOfRtt) {
    Recovery r;
    r.on_rtt_sample(80ms, 0ms);
    EXPECT_EQ(r.loss_time_threshold(), 90ms);
}

// ---------- Congestion ----------------------------------------------------

TEST(QuicCongestion, InitialWindowIsTenMaxDatagrams) {
    Congestion c;
    EXPECT_EQ(c.congestion_window(), 12000U);
    EXPECT_TRUE(c.can_send(12000));
    EXPECT_FALSE(c.can_send(12001));
}

TEST(QuicCongestion, SlowStartIncreasesWindowByAcked) {
    Congestion c;
    auto now = TimePoint{};
    c.on_packet_sent(now, 1200);
    c.on_packets_acked(now, 1200);
    EXPECT_EQ(c.congestion_window(), 12000U + 1200U);
}

TEST(QuicCongestion, LossHalvesWindowAndEntersRecovery) {
    Congestion c;
    auto t0 = TimePoint{};
    c.on_packet_sent(t0, 6000);
    auto t1 = t0 + 10ms;
    c.on_packets_lost(t1, 1200, t0);
    // ssthresh = cwnd * 0.5 = 6000; cwnd = max(ssthresh, 2*max_datagram) = max(6000, 2400) = 6000.
    EXPECT_EQ(c.congestion_window(), 6000U);
    EXPECT_TRUE(c.in_recovery(t1 + 1ms));
}

TEST(QuicCongestion, OnlyOneRecoveryPerEpoch) {
    Congestion c;
    auto t0 = TimePoint{};
    c.on_packet_sent(t0, 6000);
    auto t1 = t0 + 10ms;
    c.on_packets_lost(t1, 1200, t0);
    auto cwnd_after_first = c.congestion_window();
    // Loss for a packet sent before recovery started must not re-shrink.
    c.on_packets_lost(t1 + 5ms, 1200, t0);
    EXPECT_EQ(c.congestion_window(), cwnd_after_first);
}
