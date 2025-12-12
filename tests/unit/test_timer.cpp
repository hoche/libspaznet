#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/io_context.hpp>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace spaznet;

// Validate that a one-shot timer resumes after roughly the requested delay
TEST(IOContextTimerTest, SleepForFiresOnce) {
    IOContext ctx(1);
    std::atomic<int> hits{0};
    std::atomic<int64_t> elapsed_ms{0};

    auto task = [&]() -> Task {
        auto start = std::chrono::steady_clock::now();
        co_await ctx.sleep_for(50ms);
        auto elapsed = std::chrono::steady_clock::now() - start;
        elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        hits.fetch_add(1);
    };

    std::thread runner([&ctx]() { ctx.run(); });

    // Give the event loop time to start and be ready
    std::this_thread::sleep_for(50ms);

    ctx.schedule(task());

    // Wait long enough for the timer to fire (50ms delay + some margin)
    std::this_thread::sleep_for(150ms);
    ctx.stop();
    runner.join();

    EXPECT_EQ(hits.load(), 1);
    EXPECT_GE(elapsed_ms.load(), 30)
        << "Timer fired too early, elapsed: " << elapsed_ms.load() << "ms";
}

// Ensure repeating timers keep a stable cadence (jitter corrected)
TEST(IOContextTimerTest, IntervalStaysPeriodic) {
    IOContext ctx(1);
    std::vector<std::chrono::steady_clock::time_point> ticks;

    auto task = [&]() -> Task {
        // Capture three ticks
        for (int i = 0; i < 3; ++i) {
            co_await ctx.interval(30ms);
            ticks.push_back(std::chrono::steady_clock::now());
        }
    };

    ctx.schedule(task());

    std::thread runner([&ctx]() { ctx.run(); });
    std::this_thread::sleep_for(250ms);
    ctx.stop();
    runner.join();

    ASSERT_EQ(ticks.size(), 3);

    auto delta1 =
        std::chrono::duration_cast<std::chrono::milliseconds>(ticks[1] - ticks[0]).count();
    auto delta2 =
        std::chrono::duration_cast<std::chrono::milliseconds>(ticks[2] - ticks[1]).count();

    EXPECT_NEAR(delta1, 30, 20); // allow modest jitter
    EXPECT_NEAR(delta2, 30, 20);
}
