#include <gtest/gtest.h>
#include <libspaznet/http_handler.hpp>
#include <sstream>
#include <string>

using namespace spaznet;

TEST(HTTPResponseTest, SerializeBasic) {
    HTTPResponse response;
    response.status_code = 200;
    response.status_message = "OK";
    response.set_header("Content-Type", "text/plain");
    response.body = {'H', 'e', 'l', 'l', 'o'};

    auto serialized = response.serialize();
    std::string result(serialized.begin(), serialized.end());

    EXPECT_NE(result.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(result.find("Content-Type: text/plain"), std::string::npos);
    EXPECT_NE(result.find("Content-Length: 5"), std::string::npos);
    EXPECT_NE(result.find("Hello"), std::string::npos);
}

TEST(HTTPResponseTest, SerializeWithCustomHeaders) {
    HTTPResponse response;
    response.status_code = 404;
    response.status_message = "Not Found";
    response.set_header("Content-Type", "text/html");
    response.set_header("X-Custom-Header", "custom-value");
    response.body = {'<', 'h', '1', '>', '4', '0', '4', '<', '/', 'h', '1', '>'};

    auto serialized = response.serialize();
    std::string result(serialized.begin(), serialized.end());

    EXPECT_NE(result.find("404 Not Found"), std::string::npos);
    EXPECT_NE(result.find("X-Custom-Header: custom-value"), std::string::npos);
    EXPECT_NE(result.find("Content-Length: 12"), std::string::npos);
}

TEST(HTTPResponseTest, SerializeEmptyBody) {
    HTTPResponse response;
    response.status_code = 204;
    response.status_message = "No Content";

    auto serialized = response.serialize();
    std::string result(serialized.begin(), serialized.end());

    EXPECT_NE(result.find("204 No Content"), std::string::npos);
    // Should not have Content-Length for empty body
}

TEST(HTTPResponseTest, SerializeLargeBody) {
    HTTPResponse response;
    response.status_code = 200;
    response.status_message = "OK";
    response.set_header("Content-Type", "application/octet-stream");

    // Create a large body
    response.body.resize(10000, 'A');

    auto serialized = response.serialize();
    std::string result(serialized.begin(), serialized.end());

    EXPECT_NE(result.find("Content-Length: 10000"), std::string::npos);
    EXPECT_EQ(serialized.size(), result.find("\r\n\r\n") + 4 + 10000);
}

TEST(HTTPResponseTest, SetHeader) {
    HTTPResponse response;
    response.set_header("Content-Type", "application/json");
    response.set_header("Cache-Control", "no-cache");

    EXPECT_EQ(response.headers["Content-Type"], "application/json");
    EXPECT_EQ(response.headers["Cache-Control"], "no-cache");
}

TEST(HTTPResponseTest, OverwriteHeader) {
    HTTPResponse response;
    response.set_header("Content-Type", "text/plain");
    response.set_header("Content-Type", "text/html");

    EXPECT_EQ(response.headers["Content-Type"], "text/html");
    EXPECT_EQ(response.headers.size(), 1);
}

TEST(HTTPRequestTest, RequestStructure) {
    HTTPRequest request;
    request.method = "GET";
    request.path = "/test";
    request.version = "HTTP/1.1";
    request.headers["Host"] = "example.com";
    request.headers["User-Agent"] = "test-agent";
    request.body = {'d', 'a', 't', 'a'};

    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.path, "/test");
    EXPECT_EQ(request.version, "HTTP/1.1");
    EXPECT_EQ(request.headers["Host"], "example.com");
    EXPECT_EQ(request.headers["User-Agent"], "test-agent");
    EXPECT_EQ(request.body.size(), 4);
}
