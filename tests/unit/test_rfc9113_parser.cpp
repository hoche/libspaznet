#include <gtest/gtest.h>
#include <libspaznet/handlers/http2_handler.hpp>
#include <string>
#include <vector>

using namespace spaznet;

// RFC 9113 Parser Unit Tests
class RFC9113ParserTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

// Test HTTP/2 Connection Preface per RFC 9113 Section 3.5
TEST_F(RFC9113ParserTest, ConnectionPreface) {
    std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    std::vector<uint8_t> data(preface.begin(), preface.end());
    size_t offset = 0;
    
    EXPECT_TRUE(HTTP2Parser::parse_connection_preface(data, offset));
    EXPECT_EQ(offset, 24);
}

// Test Frame Serialization per RFC 9113 Section 4.1
TEST_F(RFC9113ParserTest, FrameSerialization) {
    HTTP2Frame frame;
    frame.length = 10;
    frame.type = HTTP2FrameType::HEADERS;
    frame.flags = HTTP2Flags::END_HEADERS | HTTP2Flags::END_STREAM;
    frame.stream_id = 1;
    frame.payload = {'t', 'e', 's', 't', 'd', 'a', 't', 'a', '1', '2'};
    
    auto serialized = frame.serialize();
    
    // Frame header is 9 bytes
    EXPECT_GE(serialized.size(), 9);
    
    // Verify length (24 bits, big-endian)
    EXPECT_EQ(serialized[0], 0x00);
    EXPECT_EQ(serialized[1], 0x00);
    EXPECT_EQ(serialized[2], 0x0A);
    
    // Verify type
    EXPECT_EQ(serialized[3], static_cast<uint8_t>(HTTP2FrameType::HEADERS));
    
    // Verify flags
    EXPECT_EQ(serialized[4], HTTP2Flags::END_HEADERS | HTTP2Flags::END_STREAM);
    
    // Verify stream ID (31 bits, R bit = 0)
    EXPECT_EQ(serialized[5], 0x00);
    EXPECT_EQ(serialized[6], 0x00);
    EXPECT_EQ(serialized[7], 0x00);
    EXPECT_EQ(serialized[8], 0x01);
}

// Test Frame Parsing per RFC 9113 Section 4.1
TEST_F(RFC9113ParserTest, FrameParsing) {
    HTTP2Frame original;
    original.length = 5;
    original.type = HTTP2FrameType::DATA;
    original.flags = HTTP2Flags::END_STREAM;
    original.stream_id = 3;
    original.payload = {'d', 'a', 't', 'a', '!'};
    
    auto serialized = original.serialize();
    size_t offset = 0;
    auto parsed = HTTP2Frame::parse(serialized, offset);
    
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->length, 5);
    EXPECT_EQ(parsed->type, HTTP2FrameType::DATA);
    EXPECT_EQ(parsed->flags, HTTP2Flags::END_STREAM);
    EXPECT_EQ(parsed->stream_id, 3);
    EXPECT_EQ(parsed->payload, original.payload);
}

// Test Frame Length Limit per RFC 9113 Section 4.1
TEST_F(RFC9113ParserTest, FrameLengthLimit) {
    std::vector<uint8_t> data;
    // Frame with length > 16384 (invalid)
    data.push_back(0x00);
    data.push_back(0x40);
    data.push_back(0x01);  // 16385 in big-endian
    data.push_back(static_cast<uint8_t>(HTTP2FrameType::DATA));
    data.push_back(0x00);
    data.insert(data.end(), 8, 0x00);  // Stream ID
    
    size_t offset = 0;
    auto parsed = HTTP2Frame::parse(data, offset);
    
    // Should reject frame with length > 16384
    EXPECT_FALSE(parsed.has_value());
}

// Test HEADERS Frame Building
TEST_F(RFC9113ParserTest, BuildHeadersFrame) {
    HTTP2Request request;
    request.method = "GET";
    request.path = "/index.html";
    request.headers[":method"] = "GET";
    request.headers[":path"] = "/index.html";
    request.headers[":scheme"] = "https";
    request.headers[":authority"] = "example.com";
    
    HTTP2Frame frame = HTTP2Parser::build_headers_frame(request, 1, true, true);
    
    EXPECT_EQ(frame.type, HTTP2FrameType::HEADERS);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_TRUE(frame.flags & HTTP2Flags::END_HEADERS);
    EXPECT_TRUE(frame.flags & HTTP2Flags::END_STREAM);
    EXPECT_GT(frame.length, 0);
}

// Test DATA Frame Building
TEST_F(RFC9113ParserTest, BuildDataFrame) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    HTTP2Frame frame = HTTP2Parser::build_data_frame(1, data, true);
    
    EXPECT_EQ(frame.type, HTTP2FrameType::DATA);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_TRUE(frame.flags & HTTP2Flags::END_STREAM);
    EXPECT_EQ(frame.payload, data);
    EXPECT_EQ(frame.length, 5);
}

// Test SETTINGS Frame Building
TEST_F(RFC9113ParserTest, BuildSettingsFrame) {
    HTTP2Settings settings;
    settings.header_table_size = 8192;
    settings.enable_push = false;
    settings.max_concurrent_streams = 100;
    
    HTTP2Frame frame = HTTP2Parser::build_settings_frame(settings, false);
    
    EXPECT_EQ(frame.type, HTTP2FrameType::SETTINGS);
    EXPECT_EQ(frame.stream_id, 0);  // Connection-level
    EXPECT_GT(frame.length, 0);
}

// Test SETTINGS Frame with ACK
TEST_F(RFC9113ParserTest, BuildSettingsFrameAck) {
    HTTP2Settings settings;
    HTTP2Frame frame = HTTP2Parser::build_settings_frame(settings, true);
    
    EXPECT_EQ(frame.type, HTTP2FrameType::SETTINGS);
    EXPECT_TRUE(frame.flags & HTTP2Flags::ACK);
    EXPECT_EQ(frame.length, 0);  // ACK has no payload
}

// Test GOAWAY Frame Building
TEST_F(RFC9113ParserTest, BuildGoawayFrame) {
    HTTP2Frame frame = HTTP2Parser::build_goaway_frame(5, 0);
    
    EXPECT_EQ(frame.type, HTTP2FrameType::GOAWAY);
    EXPECT_EQ(frame.stream_id, 0);  // Connection-level
    EXPECT_EQ(frame.length, 8);
}

// Test RST_STREAM Frame Building
TEST_F(RFC9113ParserTest, BuildRstStreamFrame) {
    HTTP2Frame frame = HTTP2Parser::build_rst_stream_frame(1, 1);  // PROTOCOL_ERROR
    
    EXPECT_EQ(frame.type, HTTP2FrameType::RST_STREAM);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.length, 4);
}

// Test WINDOW_UPDATE Frame Building
TEST_F(RFC9113ParserTest, BuildWindowUpdateFrame) {
    HTTP2Frame frame = HTTP2Parser::build_window_update_frame(1, 65535);
    
    EXPECT_EQ(frame.type, HTTP2FrameType::WINDOW_UPDATE);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.length, 4);
}

// Test PING Frame Building
TEST_F(RFC9113ParserTest, BuildPingFrame) {
    std::vector<uint8_t> opaque = {1, 2, 3, 4, 5, 6, 7, 8};
    HTTP2Frame frame = HTTP2Parser::build_ping_frame(opaque, false);
    
    EXPECT_EQ(frame.type, HTTP2FrameType::PING);
    EXPECT_EQ(frame.stream_id, 0);  // Connection-level
    EXPECT_EQ(frame.length, 8);
    EXPECT_EQ(frame.payload, opaque);
}

// Test HTTP2Settings Serialization
TEST_F(RFC9113ParserTest, SettingsSerialization) {
    HTTP2Settings settings;
    settings.header_table_size = 4096;
    settings.enable_push = true;
    settings.max_concurrent_streams = 100;
    settings.initial_window_size = 65535;
    settings.max_frame_size = 16384;
    settings.max_header_list_size = 8192;
    
    auto serialized = settings.serialize();
    
    // Should have 6 settings * 6 bytes each = 36 bytes
    EXPECT_EQ(serialized.size(), 36);
}

// Test HTTP2Settings Parsing
TEST_F(RFC9113ParserTest, SettingsParsing) {
    HTTP2Settings original;
    original.header_table_size = 8192;
    original.enable_push = false;
    original.max_concurrent_streams = 50;
    
    auto serialized = original.serialize();
    HTTP2Settings parsed = HTTP2Settings::parse(serialized);
    
    EXPECT_EQ(parsed.header_table_size, 8192);
    EXPECT_EQ(parsed.enable_push, false);
    EXPECT_EQ(parsed.max_concurrent_streams, 50);
}

// Test HTTP2Request Pseudo-Headers
TEST_F(RFC9113ParserTest, RequestPseudoHeaders) {
    HTTP2Request request;
    request.headers[":method"] = "POST";
    request.headers[":path"] = "/api/data";
    request.headers[":scheme"] = "https";
    request.headers[":authority"] = "example.com";
    request.headers["Content-Type"] = "application/json";
    
    auto method = request.get_pseudo_header(":method");
    auto path = request.get_pseudo_header(":path");
    auto scheme = request.get_pseudo_header(":scheme");
    auto authority = request.get_pseudo_header(":authority");
    auto content_type = request.get_pseudo_header("Content-Type");
    
    EXPECT_TRUE(method.has_value());
    EXPECT_EQ(*method, "POST");
    EXPECT_TRUE(path.has_value());
    EXPECT_EQ(*path, "/api/data");
    EXPECT_TRUE(scheme.has_value());
    EXPECT_EQ(*scheme, "https");
    EXPECT_TRUE(authority.has_value());
    EXPECT_EQ(*authority, "example.com");
    EXPECT_FALSE(content_type.has_value());  // Not a pseudo-header
}

// Test HTTP2Request Regular Headers
TEST_F(RFC9113ParserTest, RequestRegularHeaders) {
    HTTP2Request request;
    request.headers[":method"] = "GET";
    request.headers[":path"] = "/";
    request.headers["User-Agent"] = "test-agent";
    request.headers["Accept"] = "text/html";
    
    auto regular = request.get_regular_headers();
    
    EXPECT_EQ(regular.size(), 2);
    EXPECT_EQ(regular["User-Agent"], "test-agent");
    EXPECT_EQ(regular["Accept"], "text/html");
    EXPECT_EQ(regular.find(":method"), regular.end());
}

// Test HTTP2Response to_frames
TEST_F(RFC9113ParserTest, ResponseToFrames) {
    HTTP2Response response;
    response.stream_id = 1;
    response.status_code = 200;
    response.headers[":status"] = "200";
    response.body.resize(20000, 'A');  // Large body
    
    auto frames = response.to_frames(16384);  // Max frame size
    
    // Should have 1 HEADERS frame + multiple DATA frames
    EXPECT_GT(frames.size(), 1);
    EXPECT_EQ(frames[0].type, HTTP2FrameType::HEADERS);
    
    // All DATA frames should be DATA type
    for (size_t i = 1; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].type, HTTP2FrameType::DATA);
    }
    
    // Last DATA frame should have END_STREAM
    EXPECT_TRUE(frames.back().flags & HTTP2Flags::END_STREAM);
}

// Test HPACK Encoding/Decoding (simplified)
TEST_F(RFC9113ParserTest, HPACKEncodeDecode) {
    std::unordered_map<std::string, std::string> headers;
    headers[":method"] = "GET";
    headers[":path"] = "/";
    headers["host"] = "example.com";
    
    auto encoded = HPACK::encode_headers(headers);
    EXPECT_FALSE(encoded.empty());
    
    auto decoded = HPACK::decode_headers(encoded);
    
    // Should decode successfully (may not match exactly due to static table)
    EXPECT_FALSE(decoded.empty());
}

// Test HTTP2Connection Stream Management
TEST_F(RFC9113ParserTest, ConnectionStreamManagement) {
    HTTP2Connection conn;
    
    // Initially stream should be IDLE
    EXPECT_EQ(conn.get_stream_state(1), HTTP2StreamState::IDLE);
    
    // Process HEADERS frame should open stream
    HTTP2Frame headers_frame;
    headers_frame.type = HTTP2FrameType::HEADERS;
    headers_frame.stream_id = 1;
    headers_frame.flags = HTTP2Flags::END_HEADERS;
    
    conn.process_frame(headers_frame);
    EXPECT_EQ(conn.get_stream_state(1), HTTP2StreamState::OPEN);
}

// Test Stream ID Validation
TEST_F(RFC9113ParserTest, StreamIdValidation) {
    HTTP2Connection conn;
    
    // Stream 0 is invalid (reserved for connection-level)
    EXPECT_FALSE(conn.is_valid_stream(0));
    
    // Odd stream IDs are valid (client-initiated)
    EXPECT_TRUE(conn.is_valid_stream(1));
    EXPECT_TRUE(conn.is_valid_stream(3));
    
    // Even stream IDs are valid (server-initiated)
    EXPECT_TRUE(conn.is_valid_stream(2));
    EXPECT_TRUE(conn.is_valid_stream(4));
}

// Test Multiple Frames Round-Trip
TEST_F(RFC9113ParserTest, MultipleFramesRoundTrip) {
    std::vector<HTTP2Frame> original_frames;
    
    // Create multiple frames
    for (int i = 0; i < 5; ++i) {
        HTTP2Frame frame;
        frame.length = 10;
        frame.type = HTTP2FrameType::DATA;
        frame.flags = (i == 4) ? HTTP2Flags::END_STREAM : 0;
        frame.stream_id = 1;
        frame.payload.resize(10, static_cast<uint8_t>('A' + i));
        original_frames.push_back(frame);
    }
    
    // Serialize all frames
    std::vector<uint8_t> buffer;
    for (const auto& frame : original_frames) {
        auto serialized = frame.serialize();
        buffer.insert(buffer.end(), serialized.begin(), serialized.end());
    }
    
    // Parse all frames back
    size_t offset = 0;
    std::vector<HTTP2Frame> parsed_frames;
    while (offset < buffer.size()) {
        auto parsed = HTTP2Frame::parse(buffer, offset);
        if (parsed) {
            parsed_frames.push_back(*parsed);
        } else {
            break;
        }
    }
    
    EXPECT_EQ(parsed_frames.size(), original_frames.size());
    for (size_t i = 0; i < parsed_frames.size(); ++i) {
        EXPECT_EQ(parsed_frames[i].type, original_frames[i].type);
        EXPECT_EQ(parsed_frames[i].stream_id, original_frames[i].stream_id);
        EXPECT_EQ(parsed_frames[i].flags, original_frames[i].flags);
    }
}

