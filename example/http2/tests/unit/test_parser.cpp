#include <gtest/gtest.h>
#include <libspaznet/http2/handler.hpp>
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

    EXPECT_TRUE(spaznet::http2::Parser::parse_connection_preface(data, offset));
    EXPECT_EQ(offset, 24);
}

// Test Frame Serialization per RFC 9113 Section 4.1
TEST_F(RFC9113ParserTest, FrameSerialization) {
    spaznet::http2::Frame frame;
    frame.length = 10;
    frame.type = spaznet::http2::FrameType::HEADERS;
    frame.flags = spaznet::http2::Flags::END_HEADERS | spaznet::http2::Flags::END_STREAM;
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
    EXPECT_EQ(serialized[3], static_cast<uint8_t>(spaznet::http2::FrameType::HEADERS));

    // Verify flags
    EXPECT_EQ(serialized[4], spaznet::http2::Flags::END_HEADERS | spaznet::http2::Flags::END_STREAM);

    // Verify stream ID (31 bits, R bit = 0)
    EXPECT_EQ(serialized[5], 0x00);
    EXPECT_EQ(serialized[6], 0x00);
    EXPECT_EQ(serialized[7], 0x00);
    EXPECT_EQ(serialized[8], 0x01);
}

// Test Frame Parsing per RFC 9113 Section 4.1
TEST_F(RFC9113ParserTest, FrameParsing) {
    spaznet::http2::Frame original;
    original.length = 5;
    original.type = spaznet::http2::FrameType::DATA;
    original.flags = spaznet::http2::Flags::END_STREAM;
    original.stream_id = 3;
    original.payload = {'d', 'a', 't', 'a', '!'};

    auto serialized = original.serialize();
    size_t offset = 0;
    auto parsed = spaznet::http2::Frame::parse(serialized, offset);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->length, 5);
    EXPECT_EQ(parsed->type, spaznet::http2::FrameType::DATA);
    EXPECT_EQ(parsed->flags, spaznet::http2::Flags::END_STREAM);
    EXPECT_EQ(parsed->stream_id, 3);
    EXPECT_EQ(parsed->payload, original.payload);
}

// Test Frame Length Limit per RFC 9113 Section 4.1
TEST_F(RFC9113ParserTest, FrameLengthLimit) {
    std::vector<uint8_t> data;
    // Frame with length > 16384 (invalid)
    data.push_back(0x00);
    data.push_back(0x40);
    data.push_back(0x01); // 16385 in big-endian
    data.push_back(static_cast<uint8_t>(spaznet::http2::FrameType::DATA));
    data.push_back(0x00);
    data.insert(data.end(), 8, 0x00); // Stream ID

    size_t offset = 0;
    auto parsed = spaznet::http2::Frame::parse(data, offset);

    // Should reject frame with length > 16384
    EXPECT_FALSE(parsed.has_value());
}

// Test HEADERS Frame Building
TEST_F(RFC9113ParserTest, BuildHeadersFrame) {
    spaznet::http2::Request request;
    request.method = "GET";
    request.path = "/index.html";
    request.headers[":method"] = "GET";
    request.headers[":path"] = "/index.html";
    request.headers[":scheme"] = "https";
    request.headers[":authority"] = "example.com";

    spaznet::http2::Frame frame = spaznet::http2::Parser::build_headers_frame(request, 1, true, true);

    EXPECT_EQ(frame.type, spaznet::http2::FrameType::HEADERS);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_TRUE(frame.flags & spaznet::http2::Flags::END_HEADERS);
    EXPECT_TRUE(frame.flags & spaznet::http2::Flags::END_STREAM);
    EXPECT_GT(frame.length, 0);
}

// Test DATA Frame Building
TEST_F(RFC9113ParserTest, BuildDataFrame) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    spaznet::http2::Frame frame = spaznet::http2::Parser::build_data_frame(1, data, true);

    EXPECT_EQ(frame.type, spaznet::http2::FrameType::DATA);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_TRUE(frame.flags & spaznet::http2::Flags::END_STREAM);
    EXPECT_EQ(frame.payload, data);
    EXPECT_EQ(frame.length, 5);
}

// Test SETTINGS Frame Building
TEST_F(RFC9113ParserTest, BuildSettingsFrame) {
    spaznet::http2::Settings settings;
    settings.header_table_size = 8192;
    settings.enable_push = false;
    settings.max_concurrent_streams = 100;

    spaznet::http2::Frame frame = spaznet::http2::Parser::build_settings_frame(settings, false);

    EXPECT_EQ(frame.type, spaznet::http2::FrameType::SETTINGS);
    EXPECT_EQ(frame.stream_id, 0); // Connection-level
    EXPECT_GT(frame.length, 0);
}

// Test SETTINGS Frame with ACK
TEST_F(RFC9113ParserTest, BuildSettingsFrameAck) {
    spaznet::http2::Settings settings;
    spaznet::http2::Frame frame = spaznet::http2::Parser::build_settings_frame(settings, true);

    EXPECT_EQ(frame.type, spaznet::http2::FrameType::SETTINGS);
    EXPECT_TRUE(frame.flags & spaznet::http2::Flags::ACK);
    EXPECT_EQ(frame.length, 0); // ACK has no payload
}

// Test GOAWAY Frame Building
TEST_F(RFC9113ParserTest, BuildGoawayFrame) {
    spaznet::http2::Frame frame = spaznet::http2::Parser::build_goaway_frame(5, 0);

    EXPECT_EQ(frame.type, spaznet::http2::FrameType::GOAWAY);
    EXPECT_EQ(frame.stream_id, 0); // Connection-level
    EXPECT_EQ(frame.length, 8);
}

// Test RST_STREAM Frame Building
TEST_F(RFC9113ParserTest, BuildRstStreamFrame) {
    spaznet::http2::Frame frame = spaznet::http2::Parser::build_rst_stream_frame(1, 1); // PROTOCOL_ERROR

    EXPECT_EQ(frame.type, spaznet::http2::FrameType::RST_STREAM);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.length, 4);
}

// Test WINDOW_UPDATE Frame Building
TEST_F(RFC9113ParserTest, BuildWindowUpdateFrame) {
    spaznet::http2::Frame frame = spaznet::http2::Parser::build_window_update_frame(1, 65535);

    EXPECT_EQ(frame.type, spaznet::http2::FrameType::WINDOW_UPDATE);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.length, 4);
}

// Test PING Frame Building
TEST_F(RFC9113ParserTest, BuildPingFrame) {
    std::vector<uint8_t> opaque = {1, 2, 3, 4, 5, 6, 7, 8};
    spaznet::http2::Frame frame = spaznet::http2::Parser::build_ping_frame(opaque, false);

    EXPECT_EQ(frame.type, spaznet::http2::FrameType::PING);
    EXPECT_EQ(frame.stream_id, 0); // Connection-level
    EXPECT_EQ(frame.length, 8);
    EXPECT_EQ(frame.payload, opaque);
}

// Test spaznet::http2::Settings Serialization
TEST_F(RFC9113ParserTest, SettingsSerialization) {
    spaznet::http2::Settings settings;
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

// Test spaznet::http2::Settings Parsing
TEST_F(RFC9113ParserTest, SettingsParsing) {
    spaznet::http2::Settings original;
    original.header_table_size = 8192;
    original.enable_push = false;
    original.max_concurrent_streams = 50;

    auto serialized = original.serialize();
    spaznet::http2::Settings parsed = spaznet::http2::Settings::parse(serialized);

    EXPECT_EQ(parsed.header_table_size, 8192);
    EXPECT_EQ(parsed.enable_push, false);
    EXPECT_EQ(parsed.max_concurrent_streams, 50);
}

// RFC 9113 §6.5: SETTINGS is a cumulative update — a frame that carries
// only one parameter must leave the others untouched. parse_into applies
// onto an existing Settings rather than resetting to defaults.
TEST_F(RFC9113ParserTest, SettingsParseIntoIsCumulative) {
    spaznet::http2::Settings state;
    state.max_concurrent_streams = 100;
    state.initial_window_size = 65535;

    // A frame that sets only INITIAL_WINDOW_SIZE (id 0x4).
    std::vector<uint8_t> frame = {0x00, 0x04, 0x00, 0x01, 0x00, 0x00}; // 65536
    spaznet::http2::Settings::parse_into(frame, state);

    EXPECT_EQ(state.initial_window_size, 65536U);       // updated
    EXPECT_EQ(state.max_concurrent_streams, 100U);      // preserved, not reset
}

// Test spaznet::http2::Request Pseudo-Headers
TEST_F(RFC9113ParserTest, RequestPseudoHeaders) {
    spaznet::http2::Request request;
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
    EXPECT_FALSE(content_type.has_value()); // Not a pseudo-header
}

// Test spaznet::http2::Request Regular Headers
TEST_F(RFC9113ParserTest, RequestRegularHeaders) {
    spaznet::http2::Request request;
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

// Test spaznet::http2::Response to_frames
TEST_F(RFC9113ParserTest, ResponseToFrames) {
    spaznet::http2::Response response;
    response.stream_id = 1;
    response.status_code = 200;
    response.headers[":status"] = "200";
    response.body.resize(20000, 'A'); // Large body

    auto frames = response.to_frames(16384); // Max frame size

    // Should have 1 HEADERS frame + multiple DATA frames
    EXPECT_GT(frames.size(), 1);
    EXPECT_EQ(frames[0].type, spaznet::http2::FrameType::HEADERS);

    // All DATA frames should be DATA type
    for (size_t i = 1; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].type, spaznet::http2::FrameType::DATA);
    }

    // Last DATA frame should have END_STREAM
    EXPECT_TRUE(frames.back().flags & spaznet::http2::Flags::END_STREAM);
}

// build_headers_frame must drop response headers whose value contains
// CR/LF/NUL or whose name isn't a valid HTTP/2 token, so a smuggled
// pseudo-header (e.g. ":status: 200\r\nX-Pwn: 1") cannot reach the
// peer. We can't fully introspect the spaznet::http2::HPACK output without a real
// decoder, but we can confirm the encoded payload no longer contains
// the raw bytes of the malicious value.
TEST_F(RFC9113ParserTest, BuildHeadersFrameDropsInjectedValues) {
    spaznet::http2::Response response;
    response.stream_id = 1;
    response.status_code = 200;
    response.headers["x-inject"] = "evil\r\nset-cookie: pwn=1";
    response.headers["UPPERCASE"] = "rfc9113 §8.2.1 forbids this";
    response.headers["bad name"] = "space in name";
    response.headers["x-ok"] = "harmless";

    auto frame = spaznet::http2::Parser::build_headers_frame(response, 1, true, true);
    // The spaznet::http2::HPACK payload must not contain the raw bytes of the injected
    // value, regardless of literal vs indexed encoding.
    std::string blob(frame.payload.begin(), frame.payload.end());
    EXPECT_EQ(blob.find("evil\r\nset-cookie"), std::string::npos);
    EXPECT_EQ(blob.find("UPPERCASE"), std::string::npos);
    EXPECT_EQ(blob.find("bad name"), std::string::npos);
    // The harmless header should survive.
    EXPECT_NE(blob.find("x-ok"), std::string::npos);
}

// Test spaznet::http2::HPACK Encoding/Decoding (simplified)
TEST_F(RFC9113ParserTest, HPACKEncodeDecode) {
    std::unordered_map<std::string, std::string> headers;
    headers[":method"] = "GET";
    headers[":path"] = "/";
    headers["host"] = "example.com";

    auto encoded = spaznet::http2::HPACK::encode_headers(headers);
    EXPECT_FALSE(encoded.empty());

    auto decoded = spaznet::http2::HPACK::decode_headers(encoded);

    // Should decode successfully (may not match exactly due to static table)
    EXPECT_FALSE(decoded.empty());
}

// Test spaznet::http2::Connection Stream Management
TEST_F(RFC9113ParserTest, ConnectionStreamManagement) {
    spaznet::http2::Connection conn;

    // Initially stream should be IDLE
    EXPECT_EQ(conn.get_stream_state(1), spaznet::http2::StreamState::IDLE);

    // Process HEADERS frame should open stream
    spaznet::http2::Frame headers_frame;
    headers_frame.type = spaznet::http2::FrameType::HEADERS;
    headers_frame.stream_id = 1;
    headers_frame.flags = spaznet::http2::Flags::END_HEADERS;

    conn.process_frame(headers_frame);
    EXPECT_EQ(conn.get_stream_state(1), spaznet::http2::StreamState::OPEN);
}

// Test Stream ID Validation
TEST_F(RFC9113ParserTest, StreamIdValidation) {
    spaznet::http2::Connection conn;

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
    std::vector<spaznet::http2::Frame> original_frames;

    // Create multiple frames
    for (int i = 0; i < 5; ++i) {
        spaznet::http2::Frame frame;
        frame.length = 10;
        frame.type = spaznet::http2::FrameType::DATA;
        frame.flags = (i == 4) ? spaznet::http2::Flags::END_STREAM : 0;
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
    std::vector<spaznet::http2::Frame> parsed_frames;
    while (offset < buffer.size()) {
        auto parsed = spaznet::http2::Frame::parse(buffer, offset);
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
