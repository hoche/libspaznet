#include <gtest/gtest.h>
#include <libspaznet/io_context.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace spaznet;

// Test basic task creation and execution
TEST(TaskTest, BasicTaskExecution) {
    bool executed = false;
    
    auto task = [&executed]() -> Task {
        executed = true;
        co_return;
    }();
    
    EXPECT_FALSE(executed);
    task.resume();
    EXPECT_TRUE(executed);
    EXPECT_TRUE(task.done());
}

// Test task with multiple suspension points
TEST(TaskTest, TaskWithSuspension) {
    int counter = 0;
    
    auto task = [&counter]() -> Task {
        counter++;
        co_await std::suspend_always{};
        counter++;
        co_return;
    }();
    
    EXPECT_EQ(counter, 0);
    EXPECT_FALSE(task.done());
    
    task.resume();
    EXPECT_EQ(counter, 1);
    EXPECT_FALSE(task.done());
    
    task.resume();
    EXPECT_EQ(counter, 2);
    EXPECT_TRUE(task.done());
}

// Test TaskQueue basic operations
TEST(TaskQueueTest, BasicEnqueueDequeue) {
    TaskQueue queue;
    
    bool executed1 = false;
    bool executed2 = false;
    
    auto task1 = [&executed1]() -> Task {
        executed1 = true;
        co_return;
    }();
    
    auto task2 = [&executed2]() -> Task {
        executed2 = true;
        co_return;
    }();
    
    queue.enqueue(std::move(task1));
    queue.enqueue(std::move(task2));
    
    EXPECT_FALSE(queue.empty());
    
    Task dequeued1;
    EXPECT_TRUE(queue.dequeue(dequeued1));
    dequeued1.resume();
    EXPECT_TRUE(executed1);
    
    Task dequeued2;
    EXPECT_TRUE(queue.dequeue(dequeued2));
    dequeued2.resume();
    EXPECT_TRUE(executed2);
    
    EXPECT_TRUE(queue.empty());
}

// Test TaskQueue thread safety
TEST(TaskQueueTest, ConcurrentEnqueueDequeue) {
    TaskQueue queue;
    const int num_tasks = 1000;
    std::atomic<int> completed{0};
    
    // Producer thread
    std::thread producer([&queue, &completed]() {
        for (int i = 0; i < num_tasks; ++i) {
            auto task = [&completed]() -> Task {
                completed.fetch_add(1);
                co_return;
            }();
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
    
    EXPECT_EQ(completed.load(), num_tasks);
}

// Test TaskQueue with multiple consumers
TEST(TaskQueueTest, MultipleConsumers) {
    TaskQueue queue;
    const int num_tasks = 1000;
    const int num_consumers = 4;
    std::atomic<int> completed{0};
    
    // Enqueue tasks
    for (int i = 0; i < num_tasks; ++i) {
        auto task = [&completed]() -> Task {
            completed.fetch_add(1);
            co_return;
        }();
        queue.enqueue(std::move(task));
    }
    
    // Multiple consumer threads
    std::vector<std::thread> consumers;
    for (int i = 0; i < num_consumers; ++i) {
        consumers.emplace_back([&queue, num_tasks]() {
            int count = 0;
            while (count < num_tasks / num_consumers) {
                Task task;
                if (queue.dequeue(task)) {
                    task.resume();
                    count++;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    
    for (auto& t : consumers) {
        t.join();
    }
    
    EXPECT_EQ(completed.load(), num_tasks);
}

// Test task return values through awaiters
TEST(TaskTest, TaskWithAwaiter) {
    int result = 0;
    
    auto task = [&result]() -> Task {
        IOContext::Awaiter<int> awaiter;
        awaiter.value = 42;
        awaiter.ready = true;
        result = co_await awaiter;
        co_return;
    }();
    
    task.resume();
    EXPECT_EQ(result, 42);
    EXPECT_TRUE(task.done());
}

