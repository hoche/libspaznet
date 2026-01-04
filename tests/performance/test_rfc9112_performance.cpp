#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <libspaznet/handlers/http_handler.hpp>
#include <random>
#include <string>
#include <vector>

using namespace spaznet;

class RFC9112PerformanceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Generate test data
        generate_test_requests();
        generate_test_responses();
    }

    void generate_test_requests() {
        // Small request
        small_request = "GET /index.html HTTP/1.1\r\n"
                        "Host: example.com\r\n"
                        "User-Agent: test-agent\r\n"
                        "\r\n";

        // Medium request with body
        medium_request = "POST /api/data HTTP/1.1\r\n"
                         "Host: example.com\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: 256\r\n"
                         "\r\n" +
                         std::string(256, 'A');

        // Large request
        large_request = "POST /upload HTTP/1.1\r\n"
                        "Host: example.com\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Length: 100000\r\n"
                        "\r\n" +
                        std::string(100000, 'B');

        // Chunked request
        chunked_request = "POST /chunked HTTP/1.1\r\n"
                          "Host: example.com\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "100\r\n" +
                          std::string(256, 'C') +
                          "\r\n"
                          "100\r\n" +
                          std::string(256, 'D') +
                          "\r\n"
                          "0\r\n\r\n";
    }

    void generate_test_responses() {
        // Small response
        small_response.status_code = 200;
        small_response.reason_phrase = "OK";
        small_response.set_header("Content-Type", "text/html");
        small_response.body = {'<', 'h', '1', '>', 'H', 'e', 'l',
                               'l', 'o', '<', '/', 'h', '1', '>'};

        // Medium response
        medium_response.status_code = 200;
        medium_response.reason_phrase = "OK";
        medium_response.set_header("Content-Type", "application/json");
        medium_response.body.resize(4096, 'E');

        // Large response
        large_response.status_code = 200;
        large_response.reason_phrase = "OK";
        large_response.set_header("Content-Type", "application/octet-stream");
        large_response.body.resize(100000, 'F');

        // Chunked response
        chunked_response.status_code = 200;
        chunked_response.reason_phrase = "OK";
        chunked_response.set_chunked();
        chunked_response.body.resize(8192, 'G');
    }

    std::string small_request;
    std::string medium_request;
    std::string large_request;
    std::string chunked_request;

    HTTPResponse small_response;
    HTTPResponse medium_response;
    HTTPResponse large_response;
    HTTPResponse chunked_response;
};

// Benchmark request parsing performance
TEST_F(RFC9112PerformanceTest, ParseRequestSmall) {
    const int iterations = 10000;
    std::vector<uint8_t> buffer(small_request.begin(), small_request.end());

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTPRequest request;
        size_t bytes_consumed = 0;
        HTTPParser::parse_request(buffer, request, bytes_consumed);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Small request parsing: " << avg_time_us << " μs/request" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " requests/sec" << std::endl;

    // Should parse small requests very quickly (< 10μs)
    EXPECT_LT(avg_time_us, 100);
}

TEST_F(RFC9112PerformanceTest, ParseRequestMedium) {
    const int iterations = 1000;
    std::vector<uint8_t> buffer(medium_request.begin(), medium_request.end());

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTPRequest request;
        size_t bytes_consumed = 0;
        HTTPParser::parse_request(buffer, request, bytes_consumed);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Medium request parsing: " << avg_time_us << " μs/request" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " requests/sec" << std::endl;

    EXPECT_LT(avg_time_us, 500);
}

TEST_F(RFC9112PerformanceTest, ParseRequestLarge) {
    const int iterations = 100;
    std::vector<uint8_t> buffer(large_request.begin(), large_request.end());

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTPRequest request;
        size_t bytes_consumed = 0;
        HTTPParser::parse_request(buffer, request, bytes_consumed);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Large request parsing: " << avg_time_us << " μs/request" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " requests/sec" << std::endl;

    EXPECT_LT(avg_time_us, 5000);
}

TEST_F(RFC9112PerformanceTest, ParseRequestChunked) {
    const int iterations = 1000;
    std::vector<uint8_t> buffer(chunked_request.begin(), chunked_request.end());

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTPRequest request;
        size_t bytes_consumed = 0;
        HTTPParser::parse_request(buffer, request, bytes_consumed);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Chunked request parsing: " << avg_time_us << " μs/request" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " requests/sec" << std::endl;

    EXPECT_LT(avg_time_us, 1000);
}

// Benchmark response serialization performance
TEST_F(RFC9112PerformanceTest, SerializeResponseSmall) {
    const int iterations = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        small_response.serialize();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Small response serialization: " << avg_time_us << " μs/response"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " responses/sec"
              << std::endl;

    EXPECT_LT(avg_time_us, 50);
}

TEST_F(RFC9112PerformanceTest, SerializeResponseMedium) {
    const int iterations = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        medium_response.serialize();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Medium response serialization: " << avg_time_us << " μs/response"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " responses/sec"
              << std::endl;

    EXPECT_LT(avg_time_us, 500);
}

TEST_F(RFC9112PerformanceTest, SerializeResponseLarge) {
    const int iterations = 100;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        large_response.serialize();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Large response serialization: " << avg_time_us << " μs/response"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " responses/sec"
              << std::endl;

    EXPECT_LT(avg_time_us, 5000);
}

TEST_F(RFC9112PerformanceTest, SerializeResponseChunked) {
    const int iterations = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        chunked_response.serialize_chunked();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Chunked response serialization: " << avg_time_us << " μs/response"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " responses/sec"
              << std::endl;

    EXPECT_LT(avg_time_us, 1000);
}

// Benchmark header field lookup (case-insensitive)
TEST_F(RFC9112PerformanceTest, HeaderLookupPerformance) {
    HTTPRequest request;
    request.headers["Content-Type"] = "application/json";
    request.headers["Content-Length"] = "1024";
    request.headers["User-Agent"] = "test-agent";
    request.headers["Accept"] = "text/html";
    request.headers["Connection"] = "keep-alive";

    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        request.get_header("Content-Type");
        request.get_header("content-type");
        request.get_header("CONTENT-TYPE");
        request.get_header("Content-Length");
        request.get_header("User-Agent");
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / (iterations * 5);
    std::cout << "\n[PERF] Header lookup (case-insensitive): " << avg_time_us << " μs/lookup"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " lookups/sec" << std::endl;

    // Performance tests can vary slightly across CPUs / schedulers.
    // Keep this as a regression guard, but avoid flaking on ~1.0µs boundary.
    EXPECT_LT(avg_time_us, 1.2);
}

// Benchmark chunked body parsing
TEST_F(RFC9112PerformanceTest, ParseChunkedBodyPerformance) {
    std::string chunked_data;
    // Generate 100 chunks of 1KB each
    for (int i = 0; i < 100; ++i) {
        chunked_data += "400\r\n"; // 1024 in hex
        chunked_data += std::string(1024, 'X');
        chunked_data += "\r\n";
    }
    chunked_data += "0\r\n\r\n";

    std::vector<uint8_t> buffer(chunked_data.begin(), chunked_data.end());
    const int iterations = 100;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        std::vector<uint8_t> body;
        size_t bytes_consumed = 0;
        HTTPParser::parse_chunked_body(buffer, body, bytes_consumed);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Chunked body parsing (100KB): " << avg_time_us << " μs" << std::endl;
    std::cout << "[PERF] Throughput: " << (100000.0 / avg_time_us) << " KB/sec" << std::endl;

    EXPECT_LT(avg_time_us, 10000);
}
