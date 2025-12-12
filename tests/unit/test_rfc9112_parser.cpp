#include <gtest/gtest.h>
#include <libspaznet/handlers/http_handler.hpp>
#include <string>
#include <unordered_map>
#include <vector>

using namespace spaznet;

// RFC 9112 Parser Unit Tests
class RFC9112ParserTest : public ::testing::Test {
  protected:
    void SetUp() override {}
};

// Test request line parsing per RFC 9112 Section 3.1.1
TEST_F(RFC9112ParserTest, ParseRequestLine) {
    HTTPRequest request;
    std::string line = "GET /index.html HTTP/1.1";

    EXPECT_TRUE(HTTPParser::parse_request_line(line, request));
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.request_target, "/index.html");
    EXPECT_EQ(request.version, "1.1"); // Parser extracts version after "HTTP/"
}

TEST_F(RFC9112ParserTest, ParseRequestLineWithAbsoluteForm) {
    HTTPRequest request;
    std::string line = "GET http://example.com/path HTTP/1.1";

    EXPECT_TRUE(HTTPParser::parse_request_line(line, request));
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.request_target, "http://example.com/path");
    EXPECT_EQ(request.version, "1.1");
}

TEST_F(RFC9112ParserTest, ParseRequestLineInvalidMethod) {
    HTTPRequest request;
    std::string line = "GET@ /path HTTP/1.1"; // Invalid token character

    EXPECT_FALSE(HTTPParser::parse_request_line(line, request));
}

TEST_F(RFC9112ParserTest, ParseStatusLine) {
    HTTPResponse response;
    std::string line = "HTTP/1.1 200 OK";

    EXPECT_TRUE(HTTPParser::parse_status_line(line, response));
    EXPECT_EQ(response.version, "1.1");
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.reason_phrase, "OK");
}

TEST_F(RFC9112ParserTest, ParseStatusLineWithoutReasonPhrase) {
    HTTPResponse response;
    std::string line = "HTTP/1.1 204";

    EXPECT_TRUE(HTTPParser::parse_status_line(line, response));
    EXPECT_EQ(response.version, "1.1");
    EXPECT_EQ(response.status_code, 204);
    EXPECT_EQ(response.reason_phrase, "");
}

// Test header field parsing per RFC 9112 Section 5.5
TEST_F(RFC9112ParserTest, ParseHeaderField) {
    std::unordered_map<std::string, std::string> headers;
    std::string line = "Content-Type: text/html";

    EXPECT_TRUE(HTTPParser::parse_header_field(line, headers));
    EXPECT_EQ(headers["Content-Type"], "text/html");
}

TEST_F(RFC9112ParserTest, ParseHeaderFieldWithOWS) {
    std::unordered_map<std::string, std::string> headers;
    std::string line = "Content-Type:   text/html   ";

    EXPECT_TRUE(HTTPParser::parse_header_field(line, headers));
    EXPECT_EQ(headers["Content-Type"], "text/html");
}

TEST_F(RFC9112ParserTest, ParseHeaderFieldInvalid) {
    std::unordered_map<std::string, std::string> headers;
    std::string line = "Invalid-Header@: value"; // Invalid token character

    EXPECT_FALSE(HTTPParser::parse_header_field(line, headers));
}

// Test full request parsing
TEST_F(RFC9112ParserTest, ParseCompleteRequest) {
    std::string request_str = "GET /test HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "User-Agent: test-agent\r\n"
                              "Content-Length: 4\r\n"
                              "\r\n"
                              "data";

    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    HTTPRequest request;
    size_t bytes_consumed = 0;

    auto result = HTTPParser::parse_request(buffer, request, bytes_consumed);

    EXPECT_EQ(result, HTTPParser::ParseResult::Success);
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.request_target, "/test");
    EXPECT_EQ(request.version, "1.1");
    EXPECT_EQ(request.headers["Host"], "example.com");
    EXPECT_EQ(request.headers["User-Agent"], "test-agent");
    EXPECT_EQ(request.body.size(), 4);
    EXPECT_EQ(std::string(request.body.begin(), request.body.end()), "data");
}

TEST_F(RFC9112ParserTest, ParseRequestWithChunkedEncoding) {
    std::string request_str = "POST /upload HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Transfer-Encoding: chunked\r\n"
                              "\r\n"
                              "4\r\n"
                              "data\r\n"
                              "0\r\n"
                              "\r\n";

    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    HTTPRequest request;
    size_t bytes_consumed = 0;

    auto result = HTTPParser::parse_request(buffer, request, bytes_consumed);

    EXPECT_EQ(result, HTTPParser::ParseResult::Success);
    EXPECT_TRUE(request.is_chunked());
    EXPECT_EQ(std::string(request.body.begin(), request.body.end()), "data");
}

TEST_F(RFC9112ParserTest, ParseIncompleteRequest) {
    std::string request_str = "GET /test HTTP/1.1\r\n"
                              "Host: example.com\r\n";
    // Missing final CRLF and body

    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    HTTPRequest request;
    size_t bytes_consumed = 0;

    auto result = HTTPParser::parse_request(buffer, request, bytes_consumed);

    EXPECT_EQ(result, HTTPParser::ParseResult::Incomplete);
}

TEST_F(RFC9112ParserTest, ParseChunkedBody) {
    std::string chunked_data = "4\r\n"
                               "test\r\n"
                               "6\r\n"
                               "chunk1\r\n"
                               "0\r\n"
                               "\r\n";

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;

    auto result = HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, HTTPParser::ParseResult::Success);
    EXPECT_EQ(std::string(body.begin(), body.end()), "testchunk1");
}

TEST_F(RFC9112ParserTest, ParseChunkedBodyIncomplete) {
    std::string chunked_data = "4\r\n"
                               "test\r\n";
    // Missing final chunk

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;

    auto result = HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, HTTPParser::ParseResult::Incomplete);
}

// Test HTTPRequest helper methods
TEST_F(RFC9112ParserTest, RequestGetHeaderCaseInsensitive) {
    HTTPRequest request;
    request.headers["Content-Type"] = "text/html";

    auto header1 = request.get_header("Content-Type");
    auto header2 = request.get_header("content-type");
    auto header3 = request.get_header("CONTENT-TYPE");

    EXPECT_TRUE(header1.has_value());
    EXPECT_TRUE(header2.has_value());
    EXPECT_TRUE(header3.has_value());
    EXPECT_EQ(*header1, "text/html");
    EXPECT_EQ(*header2, "text/html");
    EXPECT_EQ(*header3, "text/html");
}

TEST_F(RFC9112ParserTest, RequestShouldKeepAlive) {
    HTTPRequest request1;
    request1.version = "1.1";
    EXPECT_TRUE(request1.should_keep_alive());

    HTTPRequest request2;
    request2.version = "1.1";
    request2.headers["Connection"] = "close";
    EXPECT_FALSE(request2.should_keep_alive());

    HTTPRequest request3;
    request3.version = "1.0";
    request3.headers["Connection"] = "keep-alive";
    EXPECT_TRUE(request3.should_keep_alive());
}

TEST_F(RFC9112ParserTest, RequestGetContentLength) {
    HTTPRequest request;
    request.headers["Content-Length"] = "1024";

    auto cl = request.get_content_length();
    EXPECT_TRUE(cl.has_value());
    EXPECT_EQ(*cl, 1024);
}

TEST_F(RFC9112ParserTest, RequestIsChunked) {
    HTTPRequest request1;
    request1.headers["Transfer-Encoding"] = "chunked";
    EXPECT_TRUE(request1.is_chunked());

    HTTPRequest request2;
    request2.headers["Transfer-Encoding"] = "gzip, chunked";
    EXPECT_TRUE(request2.is_chunked());

    HTTPRequest request3;
    request3.headers["Transfer-Encoding"] = "gzip";
    EXPECT_FALSE(request3.is_chunked());
}

// Test HTTPResponse serialization
TEST_F(RFC9112ParserTest, ResponseSerializeChunked) {
    HTTPResponse response;
    response.version = "1.1";
    response.status_code = 200;
    response.reason_phrase = "OK";
    response.set_chunked();
    response.body = {'t', 'e', 's', 't'};

    auto serialized = response.serialize_chunked();
    std::string result(serialized.begin(), serialized.end());

    EXPECT_NE(result.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_NE(result.find("4\r\n"), std::string::npos); // Chunk size
    EXPECT_NE(result.find("test"), std::string::npos);
    EXPECT_NE(result.find("0\r\n\r\n"), std::string::npos); // Final chunk
}

TEST_F(RFC9112ParserTest, ResponseGetHeaderCaseInsensitive) {
    HTTPResponse response;
    response.headers["Content-Type"] = "application/json";

    auto header1 = response.get_header("Content-Type");
    auto header2 = response.get_header("content-type");

    EXPECT_TRUE(header1.has_value());
    EXPECT_TRUE(header2.has_value());
    EXPECT_EQ(*header1, "application/json");
    EXPECT_EQ(*header2, "application/json");
}

// Test various HTTP methods per RFC 9112 Section 9
TEST_F(RFC9112ParserTest, ParseVariousMethods) {
    const char* methods[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", "PATCH"};

    for (const char* method : methods) {
        std::string line = std::string(method) + " /path HTTP/1.1";
        HTTPRequest request;
        EXPECT_TRUE(HTTPParser::parse_request_line(line, request))
            << "Failed to parse method: " << method;
        EXPECT_EQ(request.method, method);
    }
}

// Test request target forms per RFC 9112 Section 3.2
TEST_F(RFC9112ParserTest, ParseRequestTargetForms) {
    // Origin form
    HTTPRequest req1;
    EXPECT_TRUE(HTTPParser::parse_request_line("GET /path HTTP/1.1", req1));
    EXPECT_EQ(req1.request_target, "/path");

    // Absolute form
    HTTPRequest req2;
    EXPECT_TRUE(HTTPParser::parse_request_line("GET http://example.com/path HTTP/1.1", req2));
    EXPECT_EQ(req2.request_target, "http://example.com/path");

    // Authority form (for CONNECT)
    HTTPRequest req3;
    EXPECT_TRUE(HTTPParser::parse_request_line("CONNECT example.com:443 HTTP/1.1", req3));
    EXPECT_EQ(req3.request_target, "example.com:443");
}
