#include <gtest/gtest.h>
#include <libspaznet/http2/handler.hpp>

using namespace spaznet;

TEST(Http2FrameTest, FrameStructure) {
    spaznet::http2::Frame frame;
    frame.length = 100;
    frame.type = spaznet::http2::FrameType::HEADERS;
    frame.flags = spaznet::http2::Flags::END_HEADERS;
    frame.stream_id = 1;
    frame.payload.resize(100, 0x42);

    EXPECT_EQ(frame.length, 100);
    EXPECT_EQ(frame.type, spaznet::http2::FrameType::HEADERS);
    EXPECT_EQ(frame.flags, spaznet::http2::Flags::END_HEADERS);
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.payload.size(), 100);
}

TEST(Http2RequestTest, RequestStructure) {
    spaznet::http2::Request request;
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

TEST(Http2ResponseTest, ToFrame) {
    spaznet::http2::Response response;
    response.stream_id = 1;
    response.set_status(200);
    response.headers["content-type"] = "text/html";
    response.body = {'<', 'h', '1', '>', 'H', 'e', 'l', 'l', 'o', '<', '/', 'h', '1', '>'};

    auto frame = response.to_frame();

    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.type, spaznet::http2::FrameType::HEADERS);
    EXPECT_GT(frame.length, 0);
}

TEST(Http2ResponseTest, ToFrameWithStatus) {
    spaznet::http2::Response response;
    response.stream_id = 42;
    response.set_status(404);
    response.headers["content-type"] = "text/plain";
    response.body = {'N', 'o', 't', ' ', 'F', 'o', 'u', 'n', 'd'};

    auto frames = response.to_frames();
    EXPECT_GT(frames.size(), 0);
    EXPECT_EQ(frames[0].stream_id, 42);
    EXPECT_EQ(frames[0].type, spaznet::http2::FrameType::HEADERS);
}
