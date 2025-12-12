#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <libspaznet/handlers/http2_handler.hpp>
#include <random>
#include <string>
#include <vector>

using namespace spaznet;

class RFC9113PerformanceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        generate_test_data();
    }

    void generate_test_data() {
        // Generate test requests
        small_request.headers[":method"] = "GET";
        small_request.headers[":path"] = "/";
        small_request.headers[":scheme"] = "https";
        small_request.headers[":authority"] = "example.com";
        small_request.method = "GET";
        small_request.path = "/";

        medium_request.headers[":method"] = "POST";
        medium_request.headers[":path"] = "/api/data";
        medium_request.headers[":scheme"] = "https";
        medium_request.headers[":authority"] = "example.com";
        medium_request.headers["content-type"] = "application/json";
        medium_request.headers["user-agent"] = "test-agent/1.0";
        medium_request.method = "POST";
        medium_request.path = "/api/data";
        medium_request.body.resize(1024, 'A');

        large_request.headers[":method"] = "POST";
        large_request.headers[":path"] = "/upload";
        large_request.headers[":scheme"] = "https";
        large_request.headers[":authority"] = "example.com";
        large_request.headers["content-type"] = "application/octet-stream";
        large_request.method = "POST";
        large_request.path = "/upload";
        large_request.body.resize(100000, 'B');

        // Generate test responses
        small_response.stream_id = 1;
        small_response.status_code = 200;
        small_response.set_status(200);
        small_response.headers["content-type"] = "text/html";
        small_response.body = {'<', 'h', '1', '>', 'H', 'e', 'l',
                               'l', 'o', '<', '/', 'h', '1', '>'};

        medium_response.stream_id = 1;
        medium_response.status_code = 200;
        medium_response.set_status(200);
        medium_response.headers["content-type"] = "application/json";
        medium_response.body.resize(4096, 'C');

        large_response.stream_id = 1;
        large_response.status_code = 200;
        large_response.set_status(200);
        large_response.headers["content-type"] = "application/octet-stream";
        large_response.body.resize(100000, 'D');
    }

    HTTP2Request small_request;
    HTTP2Request medium_request;
    HTTP2Request large_request;

    HTTP2Response small_response;
    HTTP2Response medium_response;
    HTTP2Response large_response;
};

// Benchmark frame serialization
TEST_F(RFC9113PerformanceTest, FrameSerializationSmall) {
    const int iterations = 100000;
    HTTP2Frame frame;
    frame.length = 10;
    frame.type = HTTP2FrameType::HEADERS;
    frame.flags = HTTP2Flags::END_HEADERS;
    frame.stream_id = 1;
    frame.payload.resize(10, 'X');

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        frame.serialize();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Small frame serialization: " << avg_time_us << " μs/frame" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 10);
}

TEST_F(RFC9113PerformanceTest, FrameSerializationLarge) {
    const int iterations = 1000;
    HTTP2Frame frame;
    frame.length = 16384;
    frame.type = HTTP2FrameType::DATA;
    frame.flags = HTTP2Flags::END_STREAM;
    frame.stream_id = 1;
    frame.payload.resize(16384, 'Y');

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        frame.serialize();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Large frame serialization: " << avg_time_us << " μs/frame" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 500);
}

// Benchmark frame parsing
TEST_F(RFC9113PerformanceTest, FrameParsingSmall) {
    const int iterations = 100000;
    HTTP2Frame original;
    original.length = 10;
    original.type = HTTP2FrameType::HEADERS;
    original.flags = HTTP2Flags::END_HEADERS;
    original.stream_id = 1;
    original.payload.resize(10, 'Z');

    auto serialized = original.serialize();

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        size_t offset = 0;
        HTTP2Frame::parse(serialized, offset);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Small frame parsing: " << avg_time_us << " μs/frame" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 10);
}

TEST_F(RFC9113PerformanceTest, FrameParsingLarge) {
    const int iterations = 1000;
    HTTP2Frame original;
    original.length = 16384;
    original.type = HTTP2FrameType::DATA;
    original.flags = HTTP2Flags::END_STREAM;
    original.stream_id = 1;
    original.payload.resize(16384, 'W');

    auto serialized = original.serialize();

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        size_t offset = 0;
        HTTP2Frame::parse(serialized, offset);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Large frame parsing: " << avg_time_us << " μs/frame" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 500);
}

// Benchmark HEADERS frame building
TEST_F(RFC9113PerformanceTest, BuildHeadersFrameSmall) {
    const int iterations = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTP2Parser::build_headers_frame(small_request, 1, true, true);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Small HEADERS frame building: " << avg_time_us << " μs/frame"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 100);
}

TEST_F(RFC9113PerformanceTest, BuildHeadersFrameMedium) {
    const int iterations = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTP2Parser::build_headers_frame(medium_request, 1, true, true);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Medium HEADERS frame building: " << avg_time_us << " μs/frame"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 500);
}

// Benchmark DATA frame building
TEST_F(RFC9113PerformanceTest, BuildDataFrameSmall) {
    const int iterations = 100000;
    std::vector<uint8_t> data(100, 'X');

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTP2Parser::build_data_frame(1, data, false);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Small DATA frame building: " << avg_time_us << " μs/frame" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 10);
}

TEST_F(RFC9113PerformanceTest, BuildDataFrameLarge) {
    const int iterations = 1000;
    std::vector<uint8_t> data(16384, 'Y');

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTP2Parser::build_data_frame(1, data, false);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Large DATA frame building: " << avg_time_us << " μs/frame" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 500);
}

// Benchmark HPACK encoding
TEST_F(RFC9113PerformanceTest, HPACKEncoding) {
    const int iterations = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HPACK::encode_headers(medium_request.headers);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] HPACK encoding: " << avg_time_us << " μs/headers" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " headers/sec" << std::endl;

    EXPECT_LT(avg_time_us, 50);
}

// Benchmark HPACK decoding
TEST_F(RFC9113PerformanceTest, HPACKDecoding) {
    const int iterations = 10000;
    auto encoded = HPACK::encode_headers(medium_request.headers);

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HPACK::decode_headers(encoded);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] HPACK decoding: " << avg_time_us << " μs/headers" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " headers/sec" << std::endl;

    EXPECT_LT(avg_time_us, 50);
}

// Benchmark response to_frames (multiple frames)
TEST_F(RFC9113PerformanceTest, ResponseToFramesSmall) {
    const int iterations = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        small_response.to_frames();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Small response to_frames: " << avg_time_us << " μs/response"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " responses/sec"
              << std::endl;

    EXPECT_LT(avg_time_us, 200);
}

TEST_F(RFC9113PerformanceTest, ResponseToFramesLarge) {
    const int iterations = 100;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        large_response.to_frames();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Large response to_frames: " << avg_time_us << " μs/response"
              << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " responses/sec"
              << std::endl;

    EXPECT_LT(avg_time_us, 5000);
}

// Benchmark multiple frames round-trip
TEST_F(RFC9113PerformanceTest, MultipleFramesRoundTrip) {
    const int iterations = 1000;
    const int frames_per_iteration = 10;

    // Create frames
    std::vector<HTTP2Frame> frames;
    for (int i = 0; i < frames_per_iteration; ++i) {
        HTTP2Frame frame;
        frame.length = 100;
        frame.type = HTTP2FrameType::DATA;
        frame.flags = (i == frames_per_iteration - 1) ? HTTP2Flags::END_STREAM : 0;
        frame.stream_id = 1;
        frame.payload.resize(100, static_cast<uint8_t>('A' + i));
        frames.push_back(frame);
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        // Serialize all frames
        std::vector<uint8_t> buffer;
        for (const auto& frame : frames) {
            auto serialized = frame.serialize();
            buffer.insert(buffer.end(), serialized.begin(), serialized.end());
        }

        // Parse all frames back
        size_t offset = 0;
        while (offset < buffer.size()) {
            HTTP2Frame::parse(buffer, offset);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us =
        static_cast<double>(duration.count()) / (iterations * frames_per_iteration);
    std::cout << "\n[PERF] Multiple frames round-trip: " << avg_time_us << " μs/frame" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " frames/sec" << std::endl;

    EXPECT_LT(avg_time_us, 50);
}

// Benchmark connection stream management
TEST_F(RFC9113PerformanceTest, StreamManagement) {
    const int iterations = 100000;
    HTTP2Connection conn;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTP2Frame frame;
        frame.type = HTTP2FrameType::HEADERS;
        frame.stream_id = (i * 2) + 1; // Odd stream IDs
        frame.flags = HTTP2Flags::END_HEADERS;

        conn.process_frame(frame);
        conn.get_stream_state(frame.stream_id);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] Stream management: " << avg_time_us << " μs/operation" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " operations/sec"
              << std::endl;

    EXPECT_LT(avg_time_us, 5);
}

// Benchmark SETTINGS serialization/parsing
TEST_F(RFC9113PerformanceTest, SettingsSerialization) {
    const int iterations = 100000;
    HTTP2Settings settings;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        settings.serialize();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] SETTINGS serialization: " << avg_time_us << " μs/settings" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " settings/sec" << std::endl;

    EXPECT_LT(avg_time_us, 10);
}

TEST_F(RFC9113PerformanceTest, SettingsParsing) {
    const int iterations = 100000;
    HTTP2Settings settings;
    auto serialized = settings.serialize();

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        HTTP2Settings::parse(serialized);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_time_us = static_cast<double>(duration.count()) / iterations;
    std::cout << "\n[PERF] SETTINGS parsing: " << avg_time_us << " μs/settings" << std::endl;
    std::cout << "[PERF] Throughput: " << (1000000.0 / avg_time_us) << " settings/sec" << std::endl;

    EXPECT_LT(avg_time_us, 10);
}
