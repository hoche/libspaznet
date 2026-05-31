#include <gtest/gtest.h>
#include <cstdint>
#include <libspaznet/websocket/dispatcher.hpp>
#include <libspaznet/websocket/send.hpp>
#include <vector>

using namespace spaznet;

TEST(WSFrameTest, SerializeBasicTextFrame) {
    spaznet::websocket::Frame frame;
    frame.fin = true;
    frame.rsv1 = false;
    frame.rsv2 = false;
    frame.rsv3 = false;
    frame.opcode = spaznet::websocket::Opcode::Text;
    frame.masked = false;
    frame.payload = {'H', 'e', 'l', 'l', 'o'};
    frame.payload_length = frame.payload.size();

    auto serialized = frame.serialize();

    EXPECT_GE(serialized.size(), 6); // At least 2 bytes header + 5 bytes payload
    EXPECT_EQ(serialized[0] & 0x0F, static_cast<uint8_t>(spaznet::websocket::Opcode::Text));
    EXPECT_TRUE(serialized[0] & 0x80);  // FIN bit set
    EXPECT_EQ(serialized[1] & 0x7F, 5); // Payload length
}

TEST(WSFrameTest, SerializeMaskedFrame) {
    spaznet::websocket::Frame frame;
    frame.fin = true;
    frame.opcode = spaznet::websocket::Opcode::Text;
    frame.masked = true;
    frame.masking_key = 0x12345678;
    frame.payload = {'T', 'e', 's', 't'};
    frame.payload_length = frame.payload.size();

    auto serialized = frame.serialize();

    EXPECT_GE(serialized.size(), 10);   // 2 bytes header + 4 bytes mask + 4 bytes payload
    EXPECT_TRUE(serialized[1] & 0x80);  // MASK bit set
    EXPECT_EQ(serialized[1] & 0x7F, 4); // Payload length
}

TEST(WSFrameTest, SerializeLargeFrame) {
    spaznet::websocket::Frame frame;
    frame.fin = true;
    frame.opcode = spaznet::websocket::Opcode::Binary;
    frame.masked = false;
    frame.payload.resize(126, 0x42);
    frame.payload_length = frame.payload.size();

    auto serialized = frame.serialize();

    EXPECT_EQ(serialized[1] & 0x7F, 126);  // Extended length indicator
    EXPECT_EQ(serialized.size(), 4 + 126); // 2 bytes header + 2 bytes length + 126 bytes payload
}

TEST(WSFrameTest, ParseBasicFrame) {
    std::vector<uint8_t> data = {0x81, // FIN + Text opcode
                                 0x05, // Unmasked, length 5
                                 'H',  'e', 'l', 'l', 'o'};

    auto frame = spaznet::websocket::Frame::parse(data);

    EXPECT_TRUE(frame.fin);
    EXPECT_EQ(frame.opcode, spaznet::websocket::Opcode::Text);
    EXPECT_FALSE(frame.masked);
    EXPECT_EQ(frame.payload_length, 5);
    EXPECT_EQ(frame.payload, std::vector<uint8_t>({'H', 'e', 'l', 'l', 'o'}));
}

TEST(WSFrameTest, ParseMaskedFrame) {
    std::vector<uint8_t> data = {
        0x81,                        // FIN + Text opcode
        0x85,                        // Masked, length 5
        0x37, 0xfa, 0x21, 0x3d,      // Masking key
        0x7f, 0x9f, 0x4d, 0x51, 0x58 // Masked payload
    };

    auto frame = spaznet::websocket::Frame::parse(data);

    EXPECT_TRUE(frame.masked);
    EXPECT_EQ(frame.masking_key, 0x37fa213d);
    EXPECT_EQ(frame.payload_length, 5);
    // Verify unmasking
    EXPECT_EQ(frame.payload, std::vector<uint8_t>({'H', 'e', 'l', 'l', 'o'}));
}

TEST(WSFrameTest, ParseExtendedLength) {
    std::vector<uint8_t> data = {
        0x82,       // FIN + Binary opcode
        0x7E,       // Extended length (16-bit)
        0x00, 0x80, // Length = 128
    };
    data.resize(132); // Add 128 bytes of payload
    std::fill(data.begin() + 4, data.end(), 0x42);

    auto frame = spaznet::websocket::Frame::parse(data);

    EXPECT_EQ(frame.opcode, spaznet::websocket::Opcode::Binary);
    EXPECT_EQ(frame.payload_length, 128);
    EXPECT_EQ(frame.payload.size(), 128);
}

TEST(WSFrameTest, ParseCloseFrame) {
    std::vector<uint8_t> data = {
        0x88, // FIN + Close opcode
        0x00  // No payload
    };

    auto frame = spaznet::websocket::Frame::parse(data);

    EXPECT_EQ(frame.opcode, spaznet::websocket::Opcode::Close);
    EXPECT_EQ(frame.payload_length, 0);
}

TEST(WSFrameTest, ParsePingPong) {
    std::vector<uint8_t> ping = {0x89, // FIN + Ping opcode
                                 0x05, // Length 5
                                 'p',  'i', 'n', 'g', '!'};

    auto ping_frame = spaznet::websocket::Frame::parse(ping);
    EXPECT_EQ(ping_frame.opcode, spaznet::websocket::Opcode::Ping);

    std::vector<uint8_t> pong = {0x8A, // FIN + Pong opcode
                                 0x05, // Length 5
                                 'p',  'o', 'n', 'g', '!'};

    auto pong_frame = spaznet::websocket::Frame::parse(pong);
    EXPECT_EQ(pong_frame.opcode, spaznet::websocket::Opcode::Pong);
}

TEST(WSFrameTest, RoundTripSerialization) {
    spaznet::websocket::Frame original;
    original.fin = true;
    original.opcode = spaznet::websocket::Opcode::Text;
    original.masked = false;
    original.payload = {'T', 'e', 's', 't', ' ', 'M', 'e', 's', 's', 'a', 'g', 'e'};
    original.payload_length = original.payload.size();

    auto serialized = original.serialize();
    auto parsed = spaznet::websocket::Frame::parse(serialized);

    EXPECT_EQ(parsed.fin, original.fin);
    EXPECT_EQ(parsed.opcode, original.opcode);
    EXPECT_EQ(parsed.masked, original.masked);
    EXPECT_EQ(parsed.payload, original.payload);
}

TEST(WSFrameTest, ContinuationFrame) {
    spaznet::websocket::Frame frame;
    frame.fin = false; // Not final
    frame.opcode = spaznet::websocket::Opcode::Continuation;
    frame.masked = false;
    frame.payload = {'c', 'o', 'n', 't', 'i', 'n', 'u', 'e'};
    frame.payload_length = frame.payload.size();

    auto serialized = frame.serialize();
    auto parsed = spaznet::websocket::Frame::parse(serialized);

    EXPECT_FALSE(parsed.fin);
    EXPECT_EQ(parsed.opcode, spaznet::websocket::Opcode::Continuation);
}

TEST(WSFrameTest, SerializeAndParse64BitLength) {
    spaznet::websocket::Frame frame;
    frame.fin = true;
    frame.rsv1 = frame.rsv2 = frame.rsv3 = false;
    frame.opcode = spaznet::websocket::Opcode::Binary;
    frame.masked = false;
    frame.payload.resize(70000, 0xAA);
    frame.payload_length = frame.payload.size();

    auto serialized = frame.serialize();
    EXPECT_EQ(serialized[1] & 0x7F, 127); // Uses 64-bit length path

    auto parsed = spaznet::websocket::Frame::parse(serialized);
    EXPECT_EQ(parsed.payload_length, frame.payload_length);
    EXPECT_EQ(parsed.payload.size(), frame.payload.size());
    EXPECT_EQ(parsed.payload.front(), 0xAA);
    EXPECT_EQ(parsed.payload.back(), 0xAA);
}

// --- RFC 6455 compliance tests ---

// Reserved opcodes (0x3–0x7, 0xB–0xF) MUST cause the connection to fail.
TEST(WSFrameTest, RejectReservedOpcode) {
    std::vector<uint8_t> data = {0x83, 0x00}; // FIN + reserved opcode 0x3
    EXPECT_THROW(spaznet::websocket::Frame::parse(data), std::runtime_error);

    std::vector<uint8_t> data2 = {0x8B, 0x00}; // FIN + reserved opcode 0xB
    EXPECT_THROW(spaznet::websocket::Frame::parse(data2), std::runtime_error);
}

// RSV1/2/3 bits set without a negotiated extension MUST fail.
TEST(WSFrameTest, RejectRsvBits) {
    std::vector<uint8_t> rsv1 = {0xC1, 0x00}; // FIN + RSV1 + Text
    EXPECT_THROW(spaznet::websocket::Frame::parse(rsv1), std::runtime_error);

    std::vector<uint8_t> rsv2 = {0xA1, 0x00}; // FIN + RSV2 + Text
    EXPECT_THROW(spaznet::websocket::Frame::parse(rsv2), std::runtime_error);

    std::vector<uint8_t> rsv3 = {0x91, 0x00}; // FIN + RSV3 + Text
    EXPECT_THROW(spaznet::websocket::Frame::parse(rsv3), std::runtime_error);
}

// 16-bit length must be used only when length >= 126.
TEST(WSFrameTest, RejectNonMinimal16BitLength) {
    std::vector<uint8_t> data = {
        0x82,       // FIN + Binary
        0x7E,       // 16-bit length form
        0x00, 0x05, // Length 5 — should have used 7-bit form
    };
    data.resize(4 + 5, 0); // pad to make the payload itself complete
    EXPECT_THROW(spaznet::websocket::Frame::parse(data), std::runtime_error);
}

// 64-bit length must be used only when length >= 65536.
TEST(WSFrameTest, RejectNonMinimal64BitLength) {
    std::vector<uint8_t> data = {
        0x82, // FIN + Binary
        0x7F, // 64-bit length form
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, // Length 256 — should have used 16-bit form
    };
    EXPECT_THROW(spaznet::websocket::Frame::parse(data), std::runtime_error);
}

// 64-bit length with the high bit set is invalid (RFC 6455 §5.2).
TEST(WSFrameTest, Reject64BitLengthWithHighBit) {
    std::vector<uint8_t> data = {
        0x82, // FIN + Binary
        0x7F, // 64-bit length form
        0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, // High bit set
    };
    EXPECT_THROW(spaznet::websocket::Frame::parse(data), std::runtime_error);
}

// Control frames MUST NOT be fragmented (FIN must be set).
TEST(WSFrameTest, RejectFragmentedControlFrame) {
    std::vector<uint8_t> ping_no_fin = {0x09, 0x00}; // !FIN + Ping
    EXPECT_THROW(spaznet::websocket::Frame::parse(ping_no_fin), std::runtime_error);

    std::vector<uint8_t> close_no_fin = {0x08, 0x00}; // !FIN + Close
    EXPECT_THROW(spaznet::websocket::Frame::parse(close_no_fin), std::runtime_error);
}

// Control frames MUST have a payload length of 125 or less.
TEST(WSFrameTest, RejectOversizeControlFrame) {
    // FIN + Ping, 16-bit length form claiming 200 bytes
    std::vector<uint8_t> data = {0x89, 0x7E, 0x00, 0xC8};
    data.resize(4 + 200, 0);
    EXPECT_THROW(spaznet::websocket::Frame::parse(data), std::runtime_error);
}

// Payload above kMaxPayloadBytes must not be allocated.
TEST(WSFrameTest, RejectOversizePayload) {
    const uint64_t huge = spaznet::websocket::Frame::kMaxPayloadBytes + 1;
    std::vector<uint8_t> data = {0x82, 0x7F};
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<uint8_t>((huge >> (i * 8)) & 0xFF));
    }
    // Note: we do NOT extend data to the full declared length — parse must
    // reject before even attempting to read that much.
    EXPECT_THROW(spaznet::websocket::Frame::parse(data), std::runtime_error);
}
