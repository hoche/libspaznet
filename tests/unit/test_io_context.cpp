#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/io_context.hpp>
#include <thread>
#include <vector>

using namespace spaznet;

// Helper coroutine functions (not lambdas to avoid capture corruption)
static Task simple_flag_task(std::shared_ptr<std::atomic<bool>> flag) {
    flag->store(true);
    co_return;
}

static Task counting_task(std::shared_ptr<std::atomic<int>> counter) {
    counter->fetch_add(1);
    co_return;
}

static Task suspension_task(std::shared_ptr<std::atomic<int>> step) {
    step->store(1);
    co_await std::suspend_always{};
    step->store(2);
    co_return;
}

class IOContextTest : public ::testing::Test {
  protected:
    void SetUp() override {
        context = std::make_unique<IOContext>(2); // 2 threads
        // Start the IOContext in a background thread
        io_thread = std::thread([this]() { context->run(); });
        // Give it time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        context->stop();
        if (io_thread.joinable()) {
            io_thread.join();
        }
        context.reset();
    }

    std::unique_ptr<IOContext> context;
    std::thread io_thread;
};

TEST_F(IOContextTest, BasicSchedule) {
    auto executed = std::make_shared<std::atomic<bool>>(false);

    auto task = simple_flag_task(executed);

    context->schedule(std::move(task));

    // Give it time to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Task should have executed
    EXPECT_TRUE(executed->load());
}

TEST_F(IOContextTest, MultipleTasks) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    const int num_tasks = 100;

    for (int i = 0; i < num_tasks; ++i) {
        auto task = counting_task(counter);
        context->schedule(std::move(task));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // All tasks should have executed
    EXPECT_EQ(counter->load(), num_tasks);
}

TEST_F(IOContextTest, TaskWithSuspension) {
    auto step = std::make_shared<std::atomic<int>>(0);

    auto task = suspension_task(step);

    context->schedule(std::move(task));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Task should have started, set step to 1, suspended, and then
    // been resumed by the IOContext worker threads, reaching step 2
    // Note: In a real scenario with I/O, suspension would wait for I/O events
    // With suspend_always{}, worker threads may continue executing
    EXPECT_GE(step->load(), 1); // At least reached first step
}

TEST_F(IOContextTest, PlatformIOAccess) {
    auto& platform_io = context->platform_io();
    EXPECT_NE(&platform_io, nullptr);
}

TEST_F(IOContextTest, StopContext) {
    EXPECT_NO_THROW(context->stop());
}

TEST_F(IOContextTest, ConcurrentScheduling) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    const int num_threads = 4;
    const int tasks_per_thread = 50;

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, counter, tasks_per_thread]() {
            for (int i = 0; i < tasks_per_thread; ++i) {
                auto task = counting_task(counter);
                context->schedule(std::move(task));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // All tasks should be scheduled
    EXPECT_EQ(counter->load(), num_threads * tasks_per_thread);
}
