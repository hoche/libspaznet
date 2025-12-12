#pragma once

#include <coroutine>
#include <memory>
#include <queue>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <libspaznet/platform_io.hpp>

namespace spaznet {

// Forward declarations
struct Task;

// Coroutine task handle
struct TaskPromise {
    std::coroutine_handle<> continuation;
    bool ready = false;
    
    auto get_return_object() {
        return std::coroutine_handle<TaskPromise>::from_promise(*this);
    }
    
    auto initial_suspend() noexcept {
        return std::suspend_always{};
    }
    
    auto final_suspend() noexcept {
        struct FinalAwaiter {
            std::coroutine_handle<> continuation;
            
            bool await_ready() const noexcept { return false; }
            
            void await_suspend(std::coroutine_handle<TaskPromise> h) noexcept {
                if (h.promise().continuation) {
                    h.promise().continuation.resume();
                }
            }
            
            void await_resume() noexcept {}
        };
        return FinalAwaiter{};
    }
    
    void unhandled_exception() {
        std::terminate();
    }
    
    void return_void() {}
};

struct Task {
    using promise_type = TaskPromise;
    std::coroutine_handle<TaskPromise> handle;
    
    Task() : handle{} {}
    
    Task(std::coroutine_handle<TaskPromise> h) : handle(h) {}
    
    ~Task() {
        if (handle) {
            handle.destroy();
        }
    }
    
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    
    Task(Task&& other) noexcept : handle(other.handle) {
        other.handle = {};
    }
    
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle) {
                handle.destroy();
            }
            handle = other.handle;
            other.handle = {};
        }
        return *this;
    }
    
    bool resume() {
        if (handle && !handle.done()) {
            handle.resume();
            return !handle.done();
        }
        return false;
    }
    
    bool done() const {
        return !handle || handle.done();
    }
    
    // Make Task awaitable
    bool await_ready() const noexcept {
        if (!handle) return true;
        if (handle.done()) return true;
        return false;
    }
    
    void await_suspend(std::coroutine_handle<> continuation) const noexcept {
        if (handle && !handle.done()) {
            // Store continuation in promise
            handle.promise().continuation = continuation;
            // Resume the task
            handle.resume();
        } else {
            // Task already done, resume immediately
            continuation.resume();
        }
    }
    
    void await_resume() const noexcept {
        // Nothing to return for void tasks
    }
};

// Lock-free task queue using atomic operations
class TaskQueue {
private:
    struct Node {
        Task task;
        std::atomic<Node*> next;
        
        Node(Task t) : task(std::move(t)), next(nullptr) {}
    };
    
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    
public:
    TaskQueue() {
        Node* dummy = new Node(Task{std::coroutine_handle<TaskPromise>::from_address(nullptr)});
        head_.store(dummy);
        tail_.store(dummy);
    }
    
    ~TaskQueue() {
        Node* node = head_.load();
        while (node) {
            Node* next = node->next.load();
            delete node;
            node = next;
        }
    }
    
    void enqueue(Task task) {
        Node* node = new Node(std::move(task));
        Node* prev_tail = tail_.exchange(node, std::memory_order_acq_rel);
        prev_tail->next.store(node, std::memory_order_release);
    }
    
    bool dequeue(Task& task) {
        Node* head = head_.load(std::memory_order_acquire);
        Node* next = head->next.load(std::memory_order_acquire);
        
        if (next == nullptr) {
            return false;
        }
        
        task = std::move(next->task);
        head_.store(next, std::memory_order_release);
        delete head;
        return true;
    }
    
    bool empty() const {
        Node* head = head_.load(std::memory_order_acquire);
        return head->next.load(std::memory_order_acquire) == nullptr;
    }
};

// IO Context - manages coroutines and I/O events
class IOContext {
private:
    std::unique_ptr<PlatformIO> platform_io_;
    std::vector<TaskQueue> thread_queues_;
    std::vector<std::thread> worker_threads_;
    std::atomic<bool> running_;
    std::atomic<std::size_t> next_queue_;
    std::size_t num_threads_;
    
    // Map from file descriptor to pending coroutine handles
    // Using lock-free approach with atomics
    struct PendingIO {
        std::coroutine_handle<> read_handle;
        std::coroutine_handle<> write_handle;
    };
    std::unordered_map<int, PendingIO> pending_io_;
    std::mutex io_mutex_;  // Only for the map, not for coroutine execution
    
    void worker_thread(std::size_t queue_index);
    void process_io_events(const std::vector<PlatformIO::Event>& events);
    
public:
    explicit IOContext(std::size_t num_threads = std::thread::hardware_concurrency());
    ~IOContext();
    
    // Run the event loop (blocking)
    void run();
    
    // Stop the event loop
    void stop();
    
    // Schedule a coroutine task
    void schedule(Task task);
    
    // Register I/O operation
    void register_io(int fd, uint32_t events, std::coroutine_handle<> handle);
    
    // Get platform I/O interface
    PlatformIO& platform_io() { return *platform_io_; }
    
    // Awaitable for async operations
    template<typename T>
    struct Awaiter {
        T value;
        std::coroutine_handle<> continuation;
        bool ready = false;
        
        bool await_ready() const noexcept { return ready; }
        
        void await_suspend(std::coroutine_handle<> h) {
            continuation = h;
        }
        
        T await_resume() noexcept {
            return value;
        }
    };
};

// Helper to create awaitable
template<typename T>
IOContext::Awaiter<T> make_awaiter(T value) {
    IOContext::Awaiter<T> awaiter;
    awaiter.value = value;
    awaiter.ready = true;
    return awaiter;
}

} // namespace spaznet

