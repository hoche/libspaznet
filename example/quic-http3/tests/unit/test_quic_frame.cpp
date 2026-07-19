// Round-trip tests for every QUIC frame type the server cares about.
//
// For each frame we encode it to bytes, parse those bytes back, and
// expect the parsed Frame to compare equal to the original. Equality is
// done by re-encoding and comparing the byte streams — that catches any
// asymmetry between the encoder and decoder without us having to define
// operator== on every struct in the variant.

#include <gtest/gtest.h>

#include <libspaznet/quic/frame.hpp>
#include <libspaznet/quic/pn_space.hpp>
#include <libspaznet/quic/transport_params.hpp>
#include <libspaznet/quic/varint.hpp>

#include <cstring>
#include <vector>

namespace {

using namespace spaznet::quic;

auto round_trip_one(const Frame& f) -> testing::AssertionResult {
    std::vector<uint8_t> wire;
    encode_frame(wire, f);
    std::vector<Frame> parsed;
    if (!parse_frames({wire.data(), wire.size()}, parsed)) {
        return testing::AssertionFailure() << "parse_frames failed";
    }
    if (parsed.size() != 1) {
        return testing::AssertionFailure() << "expected 1 parsed frame, got " << parsed.size();
    }
    std::vector<uint8_t> re_encoded;
    encode_frame(re_encoded, parsed[0]);
    if (re_encoded != wire) {
        return testing::AssertionFailure() << "re-encode differs";
    }
    return testing::AssertionSuccess();
}

} // namespace

TEST(QuicFrame, Padding) {
    PaddingFrame f;
    f.count = 17;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, Ping) {
    EXPECT_TRUE(round_trip_one(PingFrame{}));
}

TEST(QuicFrame, AckSimple) {
    AckFrame f;
    f.largest_acked = 42;
    f.ack_delay = 11;
    f.first_range = 5;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, AckMultipleRanges) {
    AckFrame f;
    f.largest_acked = 1000;
    f.ack_delay = 0;
    f.first_range = 7;
    f.ranges = {{2, 3}, {1, 4}};
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, AckEcn) {
    AckFrame f;
    f.ecn = true;
    f.largest_acked = 99;
    f.ack_delay = 0;
    f.first_range = 0;
    f.ect0 = 5;
    f.ect1 = 0;
    f.ce = 2;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, ResetStream) {
    ResetStreamFrame f;
    f.stream_id = 4;
    f.app_error = 0x100;
    f.final_size = 1234;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, StopSending) {
    StopSendingFrame f;
    f.stream_id = 0;
    f.app_error = 1;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, Crypto) {
    CryptoFrame f;
    f.offset = 100;
    f.data = {0xAA, 0xBB, 0xCC, 0xDD};
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, NewToken) {
    NewTokenFrame f;
    f.token = {1, 2, 3, 4, 5, 6};
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, StreamWithOffsetAndFin) {
    StreamFrame f;
    f.stream_id = 4;
    f.offset = 50;
    f.fin = true;
    f.data = {0, 1, 2, 3};
    // encoder always emits LEN bit, so parsed back has_length=true.
    std::vector<uint8_t> wire;
    encode_frame(wire, f);
    std::vector<Frame> parsed;
    ASSERT_TRUE(parse_frames({wire.data(), wire.size()}, parsed));
    ASSERT_EQ(parsed.size(), 1U);
    auto& sf = std::get<StreamFrame>(parsed[0]);
    EXPECT_EQ(sf.stream_id, 4U);
    EXPECT_EQ(sf.offset, 50U);
    EXPECT_TRUE(sf.fin);
    EXPECT_EQ(sf.data, f.data);
}

TEST(QuicFrame, StreamZeroOffsetOmitsOffField) {
    StreamFrame f;
    f.stream_id = 8;
    f.offset = 0;
    f.fin = false;
    f.data = {0xFF};
    std::vector<uint8_t> wire;
    encode_frame(wire, f);
    // Type byte = 0x0A (base 0x08 | LEN 0x02).
    EXPECT_EQ(wire[0], 0x0A);
    std::vector<Frame> parsed;
    ASSERT_TRUE(parse_frames({wire.data(), wire.size()}, parsed));
    auto& sf = std::get<StreamFrame>(parsed[0]);
    EXPECT_EQ(sf.offset, 0U);
    EXPECT_FALSE(sf.fin);
    EXPECT_EQ(sf.data, f.data);
}

TEST(QuicFrame, MaxData) {
    MaxDataFrame f;
    f.maximum = 1ULL << 20;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, MaxStreamData) {
    MaxStreamDataFrame f;
    f.stream_id = 8;
    f.maximum = 65536;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, MaxStreamsBidiAndUni) {
    MaxStreamsFrame a;
    a.bidi = true;
    a.maximum = 100;
    EXPECT_TRUE(round_trip_one(a));
    MaxStreamsFrame b;
    b.bidi = false;
    b.maximum = 3;
    EXPECT_TRUE(round_trip_one(b));
}

TEST(QuicFrame, DataBlocked) {
    DataBlockedFrame f;
    f.limit = 1024;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, StreamDataBlocked) {
    StreamDataBlockedFrame f;
    f.stream_id = 12;
    f.limit = 256;
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, StreamsBlockedBidiAndUni) {
    StreamsBlockedFrame a;
    a.bidi = true;
    a.limit = 50;
    EXPECT_TRUE(round_trip_one(a));
    StreamsBlockedFrame b;
    b.bidi = false;
    b.limit = 1;
    EXPECT_TRUE(round_trip_one(b));
}

TEST(QuicFrame, NewConnectionId) {
    NewConnectionIdFrame f;
    f.sequence_number = 1;
    f.retire_prior_to = 0;
    f.connection_id = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33};
    for (std::size_t i = 0; i < f.stateless_reset_token.size(); ++i) {
        f.stateless_reset_token[i] = static_cast<uint8_t>(i + 1);
    }
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, RetireConnectionId) {
    RetireConnectionIdFrame f;
    f.sequence_number = 2;
    EXPECT_TRUE(round_trip_one(f));
}

// RFC 9000 §19.15: a NEW_CONNECTION_ID connection ID length outside 1..20
// is a FRAME_ENCODING_ERROR — the parser must reject it.
TEST(QuicFrame, NewConnectionIdRejectsOversizedCid) {
    // Hand-encode: type, seq, retire_prior_to, cid_len=21, 21 CID bytes,
    // then a 16-byte stateless reset token.
    std::vector<uint8_t> wire;
    VarInt::append(wire, static_cast<uint64_t>(FrameType::NewConnectionId));
    VarInt::append(wire, /*sequence_number=*/1);
    VarInt::append(wire, /*retire_prior_to=*/0);
    wire.push_back(21); // illegal: > 20
    for (int i = 0; i < 21; ++i) wire.push_back(static_cast<uint8_t>(i));
    for (int i = 0; i < 16; ++i) wire.push_back(0xAB);
    std::vector<Frame> parsed;
    EXPECT_FALSE(parse_frames({wire.data(), wire.size()}, parsed))
        << "parser accepted a 21-byte connection ID";

    // A zero-length CID is likewise illegal.
    std::vector<uint8_t> wire0;
    VarInt::append(wire0, static_cast<uint64_t>(FrameType::NewConnectionId));
    VarInt::append(wire0, 1);
    VarInt::append(wire0, 0);
    wire0.push_back(0); // illegal: < 1
    for (int i = 0; i < 16; ++i) wire0.push_back(0xAB);
    std::vector<Frame> parsed0;
    EXPECT_FALSE(parse_frames({wire0.data(), wire0.size()}, parsed0))
        << "parser accepted a zero-length connection ID";
}

TEST(QuicFrame, PathChallengeAndResponse) {
    PathChallengeFrame a;
    a.data = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_TRUE(round_trip_one(a));
    PathResponseFrame b;
    b.data = {9, 8, 7, 6, 5, 4, 3, 2};
    EXPECT_TRUE(round_trip_one(b));
}

TEST(QuicFrame, ConnectionCloseTransport) {
    ConnectionCloseFrame f;
    f.application = false;
    f.error_code = 0x01; // INTERNAL_ERROR
    f.frame_type = static_cast<uint64_t>(FrameType::Crypto);
    f.reason = {'b', 'a', 'd'};
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, ConnectionCloseApplication) {
    ConnectionCloseFrame f;
    f.application = true;
    f.error_code = 0x100; // H3_NO_ERROR
    f.frame_type = 0;
    f.reason = {};
    EXPECT_TRUE(round_trip_one(f));
}

TEST(QuicFrame, HandshakeDone) {
    EXPECT_TRUE(round_trip_one(HandshakeDoneFrame{}));
}

TEST(QuicFrame, MultiFrameParse) {
    std::vector<uint8_t> wire;
    encode_frame(wire, PingFrame{});
    encode_frame(wire, AckFrame{42, 0, 0, {}, false, 0, 0, 0});
    encode_frame(wire, PaddingFrame{5});
    encode_frame(wire, HandshakeDoneFrame{});
    std::vector<Frame> parsed;
    ASSERT_TRUE(parse_frames({wire.data(), wire.size()}, parsed));
    ASSERT_EQ(parsed.size(), 4U);
    EXPECT_TRUE(std::holds_alternative<PingFrame>(parsed[0]));
    EXPECT_TRUE(std::holds_alternative<AckFrame>(parsed[1]));
    EXPECT_TRUE(std::holds_alternative<PaddingFrame>(parsed[2]));
    EXPECT_EQ(std::get<PaddingFrame>(parsed[2]).count, 5U);
    EXPECT_TRUE(std::holds_alternative<HandshakeDoneFrame>(parsed[3]));
}

TEST(QuicFrame, AckEliciting) {
    EXPECT_FALSE(is_ack_eliciting(Frame{PaddingFrame{1}}));
    EXPECT_FALSE(is_ack_eliciting(Frame{AckFrame{}}));
    EXPECT_FALSE(is_ack_eliciting(Frame{ConnectionCloseFrame{}}));
    EXPECT_TRUE(is_ack_eliciting(Frame{PingFrame{}}));
    EXPECT_TRUE(is_ack_eliciting(Frame{StreamFrame{}}));
    EXPECT_TRUE(is_ack_eliciting(Frame{CryptoFrame{}}));
    EXPECT_TRUE(is_ack_eliciting(Frame{HandshakeDoneFrame{}}));
}

// RFC 9000 §18: a connection ID in a transport parameter is at most 20
// bytes; a longer value is a TRANSPORT_PARAMETER_ERROR (decode fails).
TEST(QuicTransportParams, RejectsOversizedConnectionId) {
    std::vector<uint8_t> wire;
    VarInt::append(wire,
                   static_cast<uint64_t>(TransportParamId::InitialSourceConnectionId));
    VarInt::append(wire, 21); // length > 20
    for (int i = 0; i < 21; ++i) wire.push_back(static_cast<uint8_t>(i));
    TransportParameters tp;
    EXPECT_FALSE(decode_transport_params({wire.data(), wire.size()}, tp));
}

// RFC 9000 §7.4: a transport parameter appearing twice is a connection
// error.
TEST(QuicTransportParams, RejectsDuplicateParameter) {
    std::vector<uint8_t> wire;
    for (int rep = 0; rep < 2; ++rep) {
        VarInt::append(wire, static_cast<uint64_t>(TransportParamId::InitialMaxData));
        std::vector<uint8_t> v;
        VarInt::append(v, 4096);
        VarInt::append(wire, v.size());
        wire.insert(wire.end(), v.begin(), v.end());
    }
    TransportParameters tp;
    EXPECT_FALSE(decode_transport_params({wire.data(), wire.size()}, tp));
}

// A well-formed single set of parameters still decodes.
TEST(QuicTransportParams, AcceptsWellFormed) {
    TransportParameters in;
    in.initial_max_data = 4096;
    in.initial_max_streams_bidi = 8;
    in.initial_source_connection_id = std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04};
    auto wire = encode_transport_params(in);
    TransportParameters out;
    ASSERT_TRUE(decode_transport_params({wire.data(), wire.size()}, out));
    EXPECT_EQ(out.initial_max_data, 4096U);
    EXPECT_EQ(out.initial_max_streams_bidi, 8U);
    ASSERT_TRUE(out.initial_source_connection_id.has_value());
    EXPECT_EQ(out.initial_source_connection_id->size(), 4U);
}

// RFC 9000 §13.2.1: the stored ACK-range set is bounded so a peer sending
// packet numbers with gaps can't grow it (and the ACK frame) without limit.
TEST(QuicPnSpace, BoundsAckRanges) {
    PnSpace sp;
    // Deliver 500 packets spaced two apart (0, 2, 4, ...), so every one
    // starts its own range that never merges with a neighbour.
    for (uint64_t pn = 0; pn < 1000; pn += 2) {
        sp.on_received(pn, /*ack_eliciting=*/true);
    }
    AckFrame ack;
    sp.build_ack_frame(/*ack_delay_us=*/0, ack);
    // first_range + subsequent ranges must stay within the cap (32).
    EXPECT_LE(ack.ranges.size() + 1, 32U);
}
