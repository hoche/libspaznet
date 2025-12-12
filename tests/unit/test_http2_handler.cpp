#include <gtest/gtest.h>
#include <libspaznet/handlers/http2_handler.hpp>

using namespace spaznet;

TEST(HTTP2FrameTest, FrameStructure) {
    HTTP2Frame frame;
    frame.length = 100;
    frame.type = HTTP2FrameType::HEADERS;
    frame.flags = HTTP2Flags::END_HEADERS;
    frame.stream_id = 1;
    frame.payload.resize(100, 0x42);

    EXPECT_EQ(frame.length, 100);
    EXPECT_EQ(frame.type, HTTP2FrameType::HEADERS);
    EXPECT_EQ(frame.flags, HTTP2Flags::END_HEADERS);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.payload.size(), 100);
}

TEST(HTTP2RequestTest, RequestStructure) {
    HTTP2Request request;
    request.stream_id = 1;
    request.method = "GET";
    request.path = "/index.html";
    request.headers[":authority"] = "example.com";
    request.headers[":scheme"] = "https";
    request.body = {'d', 'a', 't', 'a'};

    EXPECT_EQ(request.stream_id, 1);
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.path, "/index.html");
    EXPECT_EQ(request.headers[":authority"], "example.com");
    EXPECT_EQ(request.body.size(), 4);
}

TEST(HTTP2ResponseTest, ToFrame) {
    HTTP2Response response;
    response.stream_id = 1;
    response.set_status(200);
    response.headers["content-type"] = "text/html";
    response.body = {'<', 'h', '1', '>', 'H', 'e', 'l', 'l', 'o', '<', '/', 'h', '1', '>'};

    auto frame = response.to_frame();

    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.type, HTTP2FrameType::HEADERS);
    EXPECT_GT(frame.length, 0);
}

TEST(HTTP2ResponseTest, ToFrameWithStatus) {
    HTTP2Response response;
    response.stream_id = 42;
    response.set_status(404);
    response.headers["content-type"] = "text/plain";
    response.body = {'N', 'o', 't', ' ', 'F', 'o', 'u', 'n', 'd'};

    auto frames = response.to_frames();
    EXPECT_GT(frames.size(), 0);
    EXPECT_EQ(frames[0].stream_id, 42);
    EXPECT_EQ(frames[0].type, HTTP2FrameType::HEADERS);
}
