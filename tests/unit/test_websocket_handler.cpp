#include <gtest/gtest.h>
#include <libspaznet/websocket_handler.hpp>
#include <vector>
#include <cstdint>

using namespace spaznet;

TEST(WebSocketFrameTest, SerializeBasicTextFrame) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.rsv1 = false;
    frame.rsv2 = false;
    frame.rsv3 = false;
    frame.opcode = WebSocketOpcode::Text;
    frame.masked = false;
    frame.payload = {'H', 'e', 'l', 'l', 'o'};
    frame.payload_length = frame.payload.size();
    
    auto serialized = frame.serialize();
    
    EXPECT_GE(serialized.size(), 6);  // At least 2 bytes header + 5 bytes payload
    EXPECT_EQ(serialized[0] & 0x0F, static_cast<uint8_t>(WebSocketOpcode::Text));
    EXPECT_TRUE(serialized[0] & 0x80);  // FIN bit set
    EXPECT_EQ(serialized[1] & 0x7F, 5);  // Payload length
}

TEST(WebSocketFrameTest, SerializeMaskedFrame) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketOpcode::Text;
    frame.masked = true;
    frame.masking_key = 0x12345678;
    frame.payload = {'T', 'e', 's', 't'};
    frame.payload_length = frame.payload.size();
    
    auto serialized = frame.serialize();
    
    EXPECT_GE(serialized.size(), 10);  // 2 bytes header + 4 bytes mask + 4 bytes payload
    EXPECT_TRUE(serialized[1] & 0x80);  // MASK bit set
    EXPECT_EQ(serialized[1] & 0x7F, 4);  // Payload length
}

TEST(WebSocketFrameTest, SerializeLargeFrame) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketOpcode::Binary;
    frame.masked = false;
    frame.payload.resize(126, 0x42);
    frame.payload_length = frame.payload.size();
    
    auto serialized = frame.serialize();
    
    EXPECT_EQ(serialized[1] & 0x7F, 126);  // Extended length indicator
    EXPECT_EQ(serialized.size(), 4 + 126);  // 2 bytes header + 2 bytes length + 126 bytes payload
}

TEST(WebSocketFrameTest, ParseBasicFrame) {
    std::vector<uint8_t> data = {
        0x81,  // FIN + Text opcode
        0x05,  // Unmasked, length 5
        'H', 'e', 'l', 'l', 'o'
    };
    
    auto frame = WebSocketFrame::parse(data);
    
    EXPECT_TRUE(frame.fin);
    EXPECT_EQ(frame.opcode, WebSocketOpcode::Text);
    EXPECT_FALSE(frame.masked);
    EXPECT_EQ(frame.payload_length, 5);
    EXPECT_EQ(frame.payload, std::vector<uint8_t>({'H', 'e', 'l', 'l', 'o'}));
}

TEST(WebSocketFrameTest, ParseMaskedFrame) {
    std::vector<uint8_t> data = {
        0x81,  // FIN + Text opcode
        0x85,  // Masked, length 5
        0x37, 0xfa, 0x21, 0x3d,  // Masking key
        0x7f, 0x9f, 0x4d, 0x51, 0x58  // Masked payload
    };
    
    auto frame = WebSocketFrame::parse(data);
    
    EXPECT_TRUE(frame.masked);
    EXPECT_EQ(frame.masking_key, 0x37fa213d);
    EXPECT_EQ(frame.payload_length, 5);
    // Verify unmasking
    EXPECT_EQ(frame.payload, std::vector<uint8_t>({'H', 'e', 'l', 'l', 'o'}));
}

TEST(WebSocketFrameTest, ParseExtendedLength) {
    std::vector<uint8_t> data = {
        0x82,  // FIN + Binary opcode
        0x7E,  // Extended length (16-bit)
        0x00, 0x80,  // Length = 128
    };
    data.resize(132);  // Add 128 bytes of payload
    std::fill(data.begin() + 4, data.end(), 0x42);
    
    auto frame = WebSocketFrame::parse(data);
    
    EXPECT_EQ(frame.opcode, WebSocketOpcode::Binary);
    EXPECT_EQ(frame.payload_length, 128);
    EXPECT_EQ(frame.payload.size(), 128);
}

TEST(WebSocketFrameTest, ParseCloseFrame) {
    std::vector<uint8_t> data = {
        0x88,  // FIN + Close opcode
        0x00   // No payload
    };
    
    auto frame = WebSocketFrame::parse(data);
    
    EXPECT_EQ(frame.opcode, WebSocketOpcode::Close);
    EXPECT_EQ(frame.payload_length, 0);
}

TEST(WebSocketFrameTest, ParsePingPong) {
    std::vector<uint8_t> ping = {
        0x89,  // FIN + Ping opcode
        0x05,  // Length 5
        'p', 'i', 'n', 'g', '!'
    };
    
    auto ping_frame = WebSocketFrame::parse(ping);
    EXPECT_EQ(ping_frame.opcode, WebSocketOpcode::Ping);
    
    std::vector<uint8_t> pong = {
        0x8A,  // FIN + Pong opcode
        0x05,  // Length 5
        'p', 'o', 'n', 'g', '!'
    };
    
    auto pong_frame = WebSocketFrame::parse(pong);
    EXPECT_EQ(pong_frame.opcode, WebSocketOpcode::Pong);
}

TEST(WebSocketFrameTest, RoundTripSerialization) {
    WebSocketFrame original;
    original.fin = true;
    original.opcode = WebSocketOpcode::Text;
    original.masked = false;
    original.payload = {'T', 'e', 's', 't', ' ', 'M', 'e', 's', 's', 'a', 'g', 'e'};
    original.payload_length = original.payload.size();
    
    auto serialized = original.serialize();
    auto parsed = WebSocketFrame::parse(serialized);
    
    EXPECT_EQ(parsed.fin, original.fin);
    EXPECT_EQ(parsed.opcode, original.opcode);
    EXPECT_EQ(parsed.masked, original.masked);
    EXPECT_EQ(parsed.payload, original.payload);
}

TEST(WebSocketFrameTest, ContinuationFrame) {
    WebSocketFrame frame;
    frame.fin = false;  // Not final
    frame.opcode = WebSocketOpcode::Continuation;
    frame.masked = false;
    frame.payload = {'c', 'o', 'n', 't', 'i', 'n', 'u', 'e'};
    frame.payload_length = frame.payload.size();
    
    auto serialized = frame.serialize();
    auto parsed = WebSocketFrame::parse(serialized);
    
    EXPECT_FALSE(parsed.fin);
    EXPECT_EQ(parsed.opcode, WebSocketOpcode::Continuation);
}

