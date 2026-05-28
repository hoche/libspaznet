#include <gtest/gtest.h>
#include <libspaznet/handlers/http_handler.hpp>
#include <sstream>
#include <string>

using namespace spaznet;

TEST(HTTPResponseTest, SerializeBasic) {
    HTTPResponse response;
    response.status_code = 200;
    response.reason_phrase = "OK";
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
    response.reason_phrase = "Not Found";
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
    response.reason_phrase = "No Content";

    auto serialized = response.serialize();
    std::string result(serialized.begin(), serialized.end());

    EXPECT_NE(result.find("204 No Content"), std::string::npos);
    // Should not have Content-Length for empty body
}

TEST(HTTPResponseTest, SerializeLargeBody) {
    HTTPResponse response;
    response.status_code = 200;
    response.reason_phrase = "OK";
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
    request.request_target = "/test";
    request.version = "1.1"; // Version is just "1.1", not "HTTP/1.1"
    request.headers["Host"] = "example.com";
    request.headers["User-Agent"] = "test-agent";
    request.body = {'d', 'a', 't', 'a'};

    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.request_target, "/test");
    EXPECT_EQ(request.version, "1.1");
    EXPECT_EQ(request.headers["Host"], "example.com");
    EXPECT_EQ(request.headers["User-Agent"], "test-agent");
    EXPECT_EQ(request.body.size(), 4);
}

// --- Response header sanitization (CR/LF/NUL injection defense) ---

// A header value containing CR/LF must not appear in the serialized output;
// otherwise the peer would see a smuggled second response.
TEST(HTTPResponseTest, DropsHeaderValueWithCRLF) {
    HTTPResponse response;
    response.status_code = 200;
    response.reason_phrase = "OK";
    response.set_header("Content-Type", "text/plain");
    response.set_header("X-Inject", "evil\r\nSet-Cookie: pwn=1");

    auto serialized = response.serialize();
    std::string out(serialized.begin(), serialized.end());

    EXPECT_NE(out.find("Content-Type: text/plain"), std::string::npos);
    EXPECT_EQ(out.find("X-Inject"), std::string::npos);
    EXPECT_EQ(out.find("Set-Cookie"), std::string::npos);
    EXPECT_EQ(out.find("\r\n\r\nSet-Cookie"), std::string::npos);
}

// Bare LF should be rejected just as firmly as CRLF.
TEST(HTTPResponseTest, DropsHeaderValueWithBareLF) {
    HTTPResponse response;
    response.set_header("X-Inject", "evil\nSet-Cookie: pwn=1");
    auto out = response.serialize();
    std::string s(out.begin(), out.end());
    EXPECT_EQ(s.find("X-Inject"), std::string::npos);
    EXPECT_EQ(s.find("Set-Cookie"), std::string::npos);
}

// NUL is a common terminator-handling bug; reject it.
TEST(HTTPResponseTest, DropsHeaderValueWithNUL) {
    HTTPResponse response;
    response.headers["X-Inject"] = std::string("a\0b", 3);
    auto out = response.serialize();
    std::string s(out.begin(), out.end());
    EXPECT_EQ(s.find("X-Inject"), std::string::npos);
}

// A header NAME with whitespace or CR/LF is also rejected (and can be more
// dangerous than a bad value if any proxy strips the value's CR/LF).
TEST(HTTPResponseTest, DropsHeaderNameWithIllegalChars) {
    HTTPResponse response;
    response.headers["Bad Name"] = "ok";
    response.headers["Bad\r\nName"] = "ok";
    response.set_header("Content-Type", "text/plain");
    auto out = response.serialize();
    std::string s(out.begin(), out.end());
    EXPECT_NE(s.find("Content-Type: text/plain"), std::string::npos);
    EXPECT_EQ(s.find("Bad Name"), std::string::npos);
    EXPECT_EQ(s.find("Bad\r\nName"), std::string::npos);
}

// The same sanitization must apply on the chunked path.
TEST(HTTPResponseTest, ChunkedSerializeAlsoDropsInjected) {
    HTTPResponse response;
    response.set_header("X-Inject", "x\r\nX-Pwn: 1");
    response.body = {'a', 'b', 'c'};
    auto out = response.serialize_chunked();
    std::string s(out.begin(), out.end());
    EXPECT_EQ(s.find("X-Inject"), std::string::npos);
    EXPECT_EQ(s.find("X-Pwn"), std::string::npos);
    EXPECT_NE(s.find("Transfer-Encoding: chunked"), std::string::npos);
}
