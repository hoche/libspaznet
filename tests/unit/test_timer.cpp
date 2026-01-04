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
    // Also wait a bit more to ensure the coroutine completes after timer fires
    std::this_thread::sleep_for(150ms);

    // Wait for the task to complete (should have fired by now)
    int attempts = 0;
    while (hits.load() < 1 && attempts < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        attempts++;
    }

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

    std::thread runner([&ctx]() { ctx.run(); });
    // Give the event loop time to start.
    std::this_thread::sleep_for(50ms);

    ctx.schedule(task());

    // Wait up to a reasonable deadline for 3 ticks (avoid flaky fixed sleeps).
    int attempts = 0;
    while (ticks.size() < 3 && attempts < 200) { // 200 * 5ms = 1s
        std::this_thread::sleep_for(5ms);
        ++attempts;
    }

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
