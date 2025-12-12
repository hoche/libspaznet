#include <gtest/gtest.h>
#include <chrono>
#include <libspaznet/handlers/quic_handler.hpp>
#include <libspaznet/server.hpp>
#include <thread>
#include <vector>

using namespace spaznet;

TEST(QUICPerformance, StreamCreation) {
    const int stream_count = 1000;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < stream_count; ++i) {
        QUICStream stream(static_cast<uint64_t>(i), true);
        EXPECT_EQ(stream.stream_id(), static_cast<uint64_t>(i));
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Should create 1000 streams quickly
    EXPECT_LT(ms, 1000);
}

TEST(QUICPerformance, ConnectionIDComparison) {
    const int comparison_count = 10000;
    ConnectionID id1;
    id1.bytes = {0x01, 0x02, 0x03, 0x04};

    ConnectionID id2;
    id2.bytes = {0x01, 0x02, 0x03, 0x04};

    ConnectionID id3;
    id3.bytes = {0x01, 0x02, 0x03, 0x05};

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < comparison_count; ++i) {
        bool eq = (id1 == id2);
        bool ne = (id1 != id3);
        EXPECT_TRUE(eq);
        EXPECT_TRUE(ne);
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Should perform comparisons quickly
    EXPECT_LT(ms, 100);
}
