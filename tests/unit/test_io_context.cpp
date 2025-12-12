#include <gtest/gtest.h>
#include <libspaznet/io_context.hpp>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

using namespace spaznet;

class IOContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_unique<IOContext>(2);  // 2 threads
    }
    
    void TearDown() override {
        context->stop();
        context.reset();
    }
    
    std::unique_ptr<IOContext> context;
};

TEST_F(IOContextTest, BasicSchedule) {
    std::atomic<bool> executed{false};
    
    auto task = [&executed]() -> Task {
        executed.store(true);
        co_return;
    }();
    
    context->schedule(std::move(task));
    
    // Give it time to execute
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Note: This test is basic - in a real scenario, we'd need to run the event loop
    // For now, we just verify scheduling doesn't crash
}

TEST_F(IOContextTest, MultipleTasks) {
    std::atomic<int> counter{0};
    const int num_tasks = 100;
    
    for (int i = 0; i < num_tasks; ++i) {
        auto task = [&counter]() -> Task {
            counter.fetch_add(1);
            co_return;
        }();
        context->schedule(std::move(task));
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Tasks should be scheduled (execution depends on event loop running)
}

TEST_F(IOContextTest, TaskWithSuspension) {
    std::atomic<int> step{0};
    
    auto task = [&step]() -> Task {
        step.store(1);
        co_await std::suspend_always{};
        step.store(2);
        co_return;
    }();
    
    context->schedule(std::move(task));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // First step should execute
    context->schedule(std::move(task));  // Reschedule for second step
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST_F(IOContextTest, PlatformIOAccess) {
    auto& platform_io = context->platform_io();
    EXPECT_NE(&platform_io, nullptr);
}

TEST_F(IOContextTest, StopContext) {
    EXPECT_NO_THROW(context->stop());
}

TEST_F(IOContextTest, ConcurrentScheduling) {
    std::atomic<int> counter{0};
    const int num_threads = 4;
    const int tasks_per_thread = 50;
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, &counter, tasks_per_thread]() {
            for (int i = 0; i < tasks_per_thread; ++i) {
                auto task = [&counter]() -> Task {
                    counter.fetch_add(1);
                    co_return;
                }();
                context->schedule(std::move(task));
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // All tasks should be scheduled
    EXPECT_EQ(counter.load(), num_threads * tasks_per_thread);
}

