#include <gtest/gtest.h>
#include <libspaznet/http/dispatcher.hpp>
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
    spaznet::http::HTTPRequest request;
    std::string line = "GET /index.html HTTP/1.1";

    EXPECT_TRUE(spaznet::http::HTTPParser::parse_request_line(line, request));
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.request_target, "/index.html");
    EXPECT_EQ(request.version, "1.1"); // Parser extracts version after "HTTP/"
}

TEST_F(RFC9112ParserTest, ParseRequestLineWithAbsoluteForm) {
    spaznet::http::HTTPRequest request;
    std::string line = "GET http://example.com/path HTTP/1.1";

    EXPECT_TRUE(spaznet::http::HTTPParser::parse_request_line(line, request));
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.request_target, "http://example.com/path");
    EXPECT_EQ(request.version, "1.1");
}

TEST_F(RFC9112ParserTest, ParseRequestLineInvalidMethod) {
    spaznet::http::HTTPRequest request;
    std::string line = "GET@ /path HTTP/1.1"; // Invalid token character

    EXPECT_FALSE(spaznet::http::HTTPParser::parse_request_line(line, request));
}

TEST_F(RFC9112ParserTest, ParseStatusLine) {
    spaznet::http::HTTPResponse response;
    std::string line = "HTTP/1.1 200 OK";

    EXPECT_TRUE(spaznet::http::HTTPParser::parse_status_line(line, response));
    EXPECT_EQ(response.version, "1.1");
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.reason_phrase, "OK");
}

TEST_F(RFC9112ParserTest, ParseStatusLineWithoutReasonPhrase) {
    spaznet::http::HTTPResponse response;
    std::string line = "HTTP/1.1 204";

    EXPECT_TRUE(spaznet::http::HTTPParser::parse_status_line(line, response));
    EXPECT_EQ(response.version, "1.1");
    EXPECT_EQ(response.status_code, 204);
    EXPECT_EQ(response.reason_phrase, "");
}

// Test header field parsing per RFC 9112 Section 5.5
TEST_F(RFC9112ParserTest, ParseHeaderField) {
    std::unordered_map<std::string, std::string> headers;
    std::string line = "Content-Type: text/html";

    EXPECT_TRUE(spaznet::http::HTTPParser::parse_header_field(line, headers));
    EXPECT_EQ(headers["Content-Type"], "text/html");
}

TEST_F(RFC9112ParserTest, ParseHeaderFieldWithOWS) {
    std::unordered_map<std::string, std::string> headers;
    std::string line = "Content-Type:   text/html   ";

    EXPECT_TRUE(spaznet::http::HTTPParser::parse_header_field(line, headers));
    EXPECT_EQ(headers["Content-Type"], "text/html");
}

TEST_F(RFC9112ParserTest, ParseHeaderFieldInvalid) {
    std::unordered_map<std::string, std::string> headers;
    std::string line = "Invalid-Header@: value"; // Invalid token character

    EXPECT_FALSE(spaznet::http::HTTPParser::parse_header_field(line, headers));
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
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
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
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_TRUE(request.is_chunked());
    EXPECT_EQ(std::string(request.body.begin(), request.body.end()), "data");
}

TEST_F(RFC9112ParserTest, ParseIncompleteRequest) {
    std::string request_str = "GET /test HTTP/1.1\r\n"
                              "Host: example.com\r\n";
    // Missing final CRLF and body

    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Incomplete);
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

    auto result = spaznet::http::HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_EQ(std::string(body.begin(), body.end()), "testchunk1");
}

TEST_F(RFC9112ParserTest, ParseChunkedBodyIncomplete) {
    std::string chunked_data = "4\r\n"
                               "test\r\n";
    // Missing final chunk

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Incomplete);
}

// RFC 9112 §7.1.2: zero-or-more trailer-field lines may appear between
// the last-chunk and the final CRLF.  The body is unchanged; trailers
// are consumed and dropped (we don't surface them on HTTPRequest).
TEST_F(RFC9112ParserTest, ParseChunkedBodyWithTrailers) {
    std::string chunked_data = "5\r\n"
                               "hello\r\n"
                               "0\r\n"
                               "X-Trailer: tag-abc\r\n"
                               "X-Other: yes\r\n"
                               "\r\n";

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_EQ(std::string(body.begin(), body.end()), "hello");
    EXPECT_EQ(bytes_consumed, chunked_data.size());
}

// A single trailer field, then the terminator.
TEST_F(RFC9112ParserTest, ParseChunkedBodyWithSingleTrailer) {
    std::string chunked_data = "0\r\n"
                               "Etag: \"abc\"\r\n"
                               "\r\n";

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_TRUE(body.empty());
    EXPECT_EQ(bytes_consumed, chunked_data.size());
}

// Trailers, but the message is truncated mid-trailer-line.  The parser
// should report Incomplete and wait for more bytes, not fail.
TEST_F(RFC9112ParserTest, ParseChunkedBodyIncompleteTrailer) {
    std::string chunked_data = "0\r\n"
                               "X-Trailer: tag-abc"; // no CRLF yet

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Incomplete);
}

// RFC 9112 §7.1.1: chunk-extension lines have no length limit.  Our
// cap is now 4 KiB; a 200-byte extension (typical integrity-tag use)
// must parse successfully.  Pre-fix, the 64-byte cap rejected this.
TEST_F(RFC9112ParserTest, ParseChunkedBodyWithLongExtension) {
    const std::string long_ext(200, 'x');
    std::string chunked_data = "4;tag=" + long_ext + "\r\n"
                               "data\r\n"
                               "0\r\n"
                               "\r\n";

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_EQ(std::string(body.begin(), body.end()), "data");
}

// A chunk-extension longer than the 4 KiB cap must still be rejected
// — this is what defends against an unbounded scan.
TEST_F(RFC9112ParserTest, ParseChunkedBodyRejectsOverlongExtension) {
    const std::string huge_ext(8192, 'x');
    std::string chunked_data = "4;tag=" + huge_ext + "\r\n"
                               "data\r\n";

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;

    auto result = spaznet::http::HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);

    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}

// Test spaznet::http::HTTPRequest helper methods
TEST_F(RFC9112ParserTest, RequestGetHeaderCaseInsensitive) {
    spaznet::http::HTTPRequest request;
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
    spaznet::http::HTTPRequest request1;
    request1.version = "1.1";
    EXPECT_TRUE(request1.should_keep_alive());

    spaznet::http::HTTPRequest request2;
    request2.version = "1.1";
    request2.headers["Connection"] = "close";
    EXPECT_FALSE(request2.should_keep_alive());

    spaznet::http::HTTPRequest request3;
    request3.version = "1.0";
    request3.headers["Connection"] = "keep-alive";
    EXPECT_TRUE(request3.should_keep_alive());

    spaznet::http::HTTPRequest request4;
    request4.version = "1.0";
    EXPECT_FALSE(request4.should_keep_alive());
}

TEST_F(RFC9112ParserTest, RequestGetContentLength) {
    spaznet::http::HTTPRequest request;
    request.headers["Content-Length"] = "1024";

    auto cl = request.get_content_length();
    EXPECT_TRUE(cl.has_value());
    EXPECT_EQ(*cl, 1024);
}

TEST_F(RFC9112ParserTest, RequestIsChunked) {
    spaznet::http::HTTPRequest request1;
    request1.headers["Transfer-Encoding"] = "chunked";
    EXPECT_TRUE(request1.is_chunked());

    spaznet::http::HTTPRequest request2;
    request2.headers["Transfer-Encoding"] = "gzip, chunked";
    EXPECT_TRUE(request2.is_chunked());

    spaznet::http::HTTPRequest request3;
    request3.headers["Transfer-Encoding"] = "gzip";
    EXPECT_FALSE(request3.is_chunked());
}

// Test spaznet::http::HTTPResponse serialization
TEST_F(RFC9112ParserTest, ResponseSerializeChunked) {
    spaznet::http::HTTPResponse response;
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
    spaznet::http::HTTPResponse response;
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
        spaznet::http::HTTPRequest request;
        EXPECT_TRUE(spaznet::http::HTTPParser::parse_request_line(line, request))
            << "Failed to parse method: " << method;
        EXPECT_EQ(request.method, method);
    }
}

// Test request target forms per RFC 9112 Section 3.2
TEST_F(RFC9112ParserTest, ParseRequestTargetForms) {
    // Origin form
    spaznet::http::HTTPRequest req1;
    EXPECT_TRUE(spaznet::http::HTTPParser::parse_request_line("GET /path HTTP/1.1", req1));
    EXPECT_EQ(req1.request_target, "/path");

    // Absolute form
    spaznet::http::HTTPRequest req2;
    EXPECT_TRUE(spaznet::http::HTTPParser::parse_request_line("GET http://example.com/path HTTP/1.1", req2));
    EXPECT_EQ(req2.request_target, "http://example.com/path");

    // Authority form (for CONNECT)
    spaznet::http::HTTPRequest req3;
    EXPECT_TRUE(spaznet::http::HTTPParser::parse_request_line("CONNECT example.com:443 HTTP/1.1", req3));
    EXPECT_EQ(req3.request_target, "example.com:443");
}

// --- Request-smuggling defenses (RFC 9112 §5.1, §6.1, §7.1) ---

// A message must not declare both Content-Length and Transfer-Encoding.
TEST_F(RFC9112ParserTest, RejectContentLengthAndTransferEncoding) {
    std::string request_str = "POST /x HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Content-Length: 5\r\n"
                              "Transfer-Encoding: chunked\r\n"
                              "\r\n"
                              "hello";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}

// Two Content-Length headers with different values must be rejected.
TEST_F(RFC9112ParserTest, RejectDuplicateContentLengthDifferentValues) {
    std::string request_str = "POST /x HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Content-Length: 5\r\n"
                              "content-length: 17\r\n"
                              "\r\n"
                              "hello";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}

// Repeated Content-Length headers with the same value are tolerated.
TEST_F(RFC9112ParserTest, AllowDuplicateContentLengthSameValue) {
    std::string request_str = "POST /x HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Content-Length: 5\r\n"
                              "content-length: 5\r\n"
                              "\r\n"
                              "hello";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_EQ(std::string(request.body.begin(), request.body.end()), "hello");
}

// A Content-Length value that is a comma-separated list of identical
// integers is permitted by RFC 9112 §6.3.
TEST_F(RFC9112ParserTest, AllowContentLengthListWithIdenticalValues) {
    std::string request_str = "POST /x HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Content-Length: 5, 5\r\n"
                              "\r\n"
                              "hello";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_EQ(std::string(request.body.begin(), request.body.end()), "hello");
    // Validation should normalize the merged list back to a single
    // value so downstream parsing doesn't depend on stoull's quirks.
    EXPECT_EQ(request.headers["Content-Length"], "5");
}

// A Content-Length list with differing values is still a framing error.
TEST_F(RFC9112ParserTest, RejectContentLengthListWithDifferentValues) {
    std::string request_str = "POST /x HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Content-Length: 5, 17\r\n"
                              "\r\n"
                              "hello";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}

// And two same-cased Content-Length headers with identical values, which
// parse_header_field merges into "5, 5", must reach the same accepted
// state as the explicit list above.
TEST_F(RFC9112ParserTest, AllowTwoIdenticalContentLengthHeaders) {
    std::string request_str = "POST /x HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Content-Length: 5\r\n"
                              "Content-Length: 5\r\n"
                              "\r\n"
                              "hello";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_EQ(std::string(request.body.begin(), request.body.end()), "hello");
    EXPECT_EQ(request.headers["Content-Length"], "5");
}

// RFC 9112 §5.1: OWS between field name and colon must be rejected.
TEST_F(RFC9112ParserTest, RejectWhitespaceBeforeColon) {
    std::unordered_map<std::string, std::string> headers;
    EXPECT_FALSE(spaznet::http::HTTPParser::parse_header_field("Host : example.com", headers));
    EXPECT_FALSE(spaznet::http::HTTPParser::parse_header_field("Host\t: example.com", headers));
}

// Transfer-Encoding whose final coding is not chunked is unsupported framing.
TEST_F(RFC9112ParserTest, RejectTransferEncodingNotEndingInChunked) {
    std::string request_str = "POST /x HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Transfer-Encoding: chunked, gzip\r\n"
                              "\r\n";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}

// Malformed chunk-size must be a hard parse error, not silently treated as
// the last-chunk (size 0).
TEST_F(RFC9112ParserTest, MalformedChunkSizeIsError) {
    std::string chunked_data = "garbage\r\n";
    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    std::vector<uint8_t> body;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}

// parse_chunk_size returns nullopt on parse failure but Some(0) on legitimate
// last-chunk.
TEST_F(RFC9112ParserTest, ChunkSizeOptionalDistinguishesErrorFromZero) {
    EXPECT_EQ(spaznet::http::HTTPParser::parse_chunk_size("0"), std::optional<size_t>{0});
    EXPECT_EQ(spaznet::http::HTTPParser::parse_chunk_size("ff"), std::optional<size_t>{255});
    EXPECT_EQ(spaznet::http::HTTPParser::parse_chunk_size("FF;ext=1"), std::optional<size_t>{255});
    EXPECT_FALSE(spaznet::http::HTTPParser::parse_chunk_size("").has_value());
    EXPECT_FALSE(spaznet::http::HTTPParser::parse_chunk_size("-1").has_value());
    EXPECT_FALSE(spaznet::http::HTTPParser::parse_chunk_size("garbage").has_value());
    EXPECT_FALSE(spaznet::http::HTTPParser::parse_chunk_size(" 1").has_value());
}

// An oversize header block is rejected before unbounded memory growth.
TEST_F(RFC9112ParserTest, OversizeHeaderBlockIsError) {
    std::string request_str = "GET / HTTP/1.1\r\nX-Huge: ";
    request_str.append(spaznet::http::HTTPParser::kMaxHeaderBytes, 'a');
    request_str.append("\r\n\r\n");
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}

// A request declaring too many headers is rejected.
TEST_F(RFC9112ParserTest, TooManyHeadersIsError) {
    std::string request_str = "GET / HTTP/1.1\r\n";
    for (size_t i = 0; i < spaznet::http::HTTPParser::kMaxHeaders + 5; ++i) {
        request_str += "X-H" + std::to_string(i) + ": v\r\n";
    }
    request_str += "\r\n";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}

// parse_request must be idempotent across calls that share the same
// output request: a server that re-parses an extended buffer after an
// Incomplete result must observe the same successful parse as a
// single-shot call. The framing-headers fix introduced in commit
// 0559935 regressed this — re-parsing concatenated each header into
// itself via parse_header_field's duplicate-merge path, producing
// "Content-Length: 10000, 10000" on the second attempt and failing
// validation. Exercised end-to-end by the LargeRequestBody
// integration test; this is the unit-level guard.
TEST_F(RFC9112ParserTest, ParseRequestIsIdempotentOnReparse) {
    std::string headers = "POST /x HTTP/1.1\r\n"
                          "Host: example.com\r\n"
                          "Content-Length: 5\r\n"
                          "\r\n";
    std::string full = headers + "hello";

    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;

    std::vector<uint8_t> partial(headers.begin(), headers.end());
    auto r1 = spaznet::http::HTTPParser::parse_request(partial, request, bytes_consumed);
    EXPECT_EQ(r1, spaznet::http::HTTPParser::ParseResult::Incomplete);

    std::vector<uint8_t> complete(full.begin(), full.end());
    auto r2 = spaznet::http::HTTPParser::parse_request(complete, request, bytes_consumed);
    EXPECT_EQ(r2, spaznet::http::HTTPParser::ParseResult::Success);
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.headers["Content-Length"], "5");
    EXPECT_EQ(std::string(request.body.begin(), request.body.end()), "hello");
}

// A peer-declared Content-Length above kMaxBodySize is rejected without
// allocating that buffer.
TEST_F(RFC9112ParserTest, OversizeContentLengthIsError) {
    std::string request_str = "POST / HTTP/1.1\r\n"
                              "Host: example.com\r\n"
                              "Content-Length: " +
                              std::to_string(spaznet::http::HTTPParser::kMaxBodySize + 1) +
                              "\r\n"
                              "\r\n";
    std::vector<uint8_t> buffer(request_str.begin(), request_str.end());
    spaznet::http::HTTPRequest request;
    size_t bytes_consumed = 0;
    auto result = spaznet::http::HTTPParser::parse_request(buffer, request, bytes_consumed);
    EXPECT_EQ(result, spaznet::http::HTTPParser::ParseResult::Error);
}
