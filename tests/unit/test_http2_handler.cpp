#include <gtest/gtest.h>
#include <libspaznet/http2_handler.hpp>

using namespace spaznet;

TEST(HTTP2FrameTest, FrameStructure) {
    HTTP2Frame frame;
    frame.length = 100;
    frame.type = 0x01;  // HEADERS
    frame.flags = 0x04;  // END_HEADERS
    frame.stream_id = 1;
    frame.payload.resize(100, 0x42);
    
    EXPECT_EQ(frame.length, 100);
    EXPECT_EQ(frame.type, 0x01);
    EXPECT_EQ(frame.flags, 0x04);
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
    response.status_code = 200;
    response.headers["content-type"] = "text/html";
    response.body = {'<', 'h', '1', '>', 'H', 'e', 'l', 'l', 'o', '<', '/', 'h', '1', '>'};
    
    auto frame = response.to_frame();
    
    EXPECT_EQ(frame.stream_id, 1);
    EXPECT_EQ(frame.type, 0x01);  // HEADERS frame
    EXPECT_EQ(frame.flags, 0x04);  // END_HEADERS
    EXPECT_GT(frame.length, 0);
}

TEST(HTTP2ResponseTest, ToFrameWithStatus) {
    HTTP2Response response;
    response.stream_id = 42;
    response.status_code = 404;
    response.headers["content-type"] = "text/plain";
    response.body = {'N', 'o', 't', ' ', 'F', 'o', 'u', 'n', 'd'};
    
    auto frame = response.to_frame();
    
    EXPECT_EQ(frame.stream_id, 42);
    std::string payload_str(frame.payload.begin(), frame.payload.end());
    EXPECT_NE(payload_str.find(":status: 404"), std::string::npos);
}

