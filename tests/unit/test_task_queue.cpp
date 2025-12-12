#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/io_context.hpp>
#include <thread>
#include <vector>

using namespace spaznet;

// Helper coroutine function (not a lambda to avoid capture issues)
static Task simple_task_helper(std::shared_ptr<std::atomic<bool>> flag) {
    flag->store(true);
    co_return;
}

// Test basic task creation and execution
TEST(TaskTest, BasicTaskExecution) {
    auto executed = std::make_shared<std::atomic<bool>>(false);

    auto task = simple_task_helper(executed);

    EXPECT_FALSE(executed->load());
    EXPECT_FALSE(task.done());

    ASSERT_NE(task.handle, std::coroutine_handle<TaskPromise>{});
    ASSERT_FALSE(task.handle.done());

    bool resumed = task.resume();

    EXPECT_TRUE(executed->load()) << "Coroutine body did not execute";
    EXPECT_TRUE(task.done());
}

// Helper for suspension test
static Task suspension_task_helper(std::shared_ptr<std::atomic<int>> counter) {
    counter->fetch_add(1);
    co_await std::suspend_always{};
    counter->fetch_add(1);
    co_return;
}

// Test task with multiple suspension points
TEST(TaskTest, TaskWithSuspension) {
    auto counter = std::make_shared<std::atomic<int>>(0);

    auto task = suspension_task_helper(counter);

    EXPECT_EQ(counter->load(), 0);
    EXPECT_FALSE(task.done());

    task.resume();
    EXPECT_EQ(counter->load(), 1);
    EXPECT_FALSE(task.done());

    task.resume();
    EXPECT_EQ(counter->load(), 2);
    EXPECT_TRUE(task.done());
}

// Test TaskQueue basic operations
TEST(TaskQueueTest, BasicEnqueueDequeue) {
    TaskQueue queue;

    auto executed1 = std::make_shared<std::atomic<bool>>(false);
    auto executed2 = std::make_shared<std::atomic<bool>>(false);

    auto task1 = simple_task_helper(executed1);
    auto task2 = simple_task_helper(executed2);

    queue.enqueue(std::move(task1));
    queue.enqueue(std::move(task2));

    EXPECT_FALSE(queue.empty());

    Task dequeued1;
    EXPECT_TRUE(queue.dequeue(dequeued1));
    dequeued1.resume();
    EXPECT_TRUE(executed1->load());

    Task dequeued2;
    EXPECT_TRUE(queue.dequeue(dequeued2));
    dequeued2.resume();
    EXPECT_TRUE(executed2->load());

    EXPECT_TRUE(queue.empty());
}

// Helper for counting task
static Task counting_task_helper(std::shared_ptr<std::atomic<int>> counter) {
    counter->fetch_add(1);
    co_return;
}

// Test TaskQueue thread safety
TEST(TaskQueueTest, ConcurrentEnqueueDequeue) {
    TaskQueue queue;
    const int num_tasks = 1000;
    auto completed = std::make_shared<std::atomic<int>>(0);

    // Producer thread
    std::thread producer([&queue, completed]() {
        for (int i = 0; i < num_tasks; ++i) {
            auto task = counting_task_helper(completed);
            queue.enqueue(std::move(task));
        }
    });

    // Consumer thread
    std::thread consumer([&queue]() {
        int count = 0;
        while (count < num_tasks) {
            Task task;
            if (queue.dequeue(task)) {
                task.resume();
                count++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(completed->load(), num_tasks);
}

// Test TaskQueue with multiple consumers
TEST(TaskQueueTest, MultipleConsumers) {
    TaskQueue queue;
    const int num_tasks = 1000;
    const int num_consumers = 4;
    auto completed = std::make_shared<std::atomic<int>>(0);
    auto dequeued_count = std::make_shared<std::atomic<int>>(0);

    // Enqueue tasks
    for (int i = 0; i < num_tasks; ++i) {
        auto task = counting_task_helper(completed);
        queue.enqueue(std::move(task));
    }

    // Multiple consumer threads - each consumes until queue is empty
    std::vector<std::thread> consumers;
    for (int i = 0; i < num_consumers; ++i) {
        consumers.emplace_back([&queue, dequeued_count, num_tasks]() {
            int local_count = 0;
            int miss_count = 0;
            const int max_misses = 100; // Exit after 100 consecutive empty dequeues

            while (dequeued_count->load() < num_tasks && miss_count < max_misses) {
                Task task;
                if (queue.dequeue(task)) {
                    task.resume();
                    local_count++;
                    dequeued_count->fetch_add(1);
                    miss_count = 0; // Reset miss counter
                } else {
                    miss_count++;
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : consumers) {
        t.join();
    }

    EXPECT_EQ(completed->load(), num_tasks);
    EXPECT_EQ(dequeued_count->load(), num_tasks);
}

// Helper for awaiter test
static Task awaiter_task_helper(std::shared_ptr<std::atomic<int>> result) {
    IOContext::Awaiter<int> awaiter;
    awaiter.value = 42;
    awaiter.ready = true;
    int value = co_await awaiter;
    result->store(value);
    co_return;
}

// Test task return values through awaiters
TEST(TaskTest, TaskWithAwaiter) {
    auto result = std::make_shared<std::atomic<int>>(0);

    auto task = awaiter_task_helper(result);

    task.resume();
    EXPECT_EQ(result->load(), 42);
    EXPECT_TRUE(task.done());
}
