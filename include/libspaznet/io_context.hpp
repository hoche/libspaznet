#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <libspaznet/platform_io.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace spaznet {

// Forward declarations
struct Task;

// Coroutine task handle
struct TaskPromise {
    std::coroutine_handle<> continuation{std::noop_coroutine()};

    // Declaration - implementation after Task is defined
    Task get_return_object();

    auto initial_suspend() noexcept {
        return std::suspend_always{};
    }

    auto final_suspend() noexcept {
        struct FinalAwaiter {
            bool await_ready() const noexcept {
                return false;
            }
            void await_suspend(std::coroutine_handle<TaskPromise> h) noexcept {
                auto cont = h.promise().continuation;
                if (cont) {
                    cont.resume();
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
        if (!handle || handle.done()) {
            return false;
        }
        handle.resume();
        return !handle.done();
    }

    bool done() const {
        return !handle || handle.done();
    }

    // Make Task awaitable
    auto operator co_await() const noexcept {
        struct Awaiter {
            std::coroutine_handle<TaskPromise> h;
            bool await_ready() const noexcept {
                return !h || h.done();
            }
            void await_suspend(std::coroutine_handle<> cont) const noexcept {
                h.promise().continuation = cont;
                if (h && !h.done()) {
                    h.resume();
                } else {
                    cont.resume();
                }
            }
            void await_resume() const noexcept {}
        };
        return Awaiter{handle};
    }
};

// Implement TaskPromise::get_return_object after Task is defined
inline Task TaskPromise::get_return_object() {
    return Task{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

// Task queue using atomic enqueue and mutex-protected dequeue
// (Simpler and safer than lock-free with hazard pointers)
class TaskQueue {
  private:
    struct Node {
        Task task;
        std::atomic<Node*> next;

        Node(Task t) : task(std::move(t)), next(nullptr) {}
    };

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    std::mutex dequeue_mutex_; // Protect dequeue from races

  public:
    TaskQueue() {
        // Create dummy node with default-constructed Task (null handle is fine for dummy)
        Node* dummy = new Node(Task{});
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
        std::lock_guard<std::mutex> lock(dequeue_mutex_);

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
    // Handles stored as raw addresses for atomic access
    struct PendingIO {
        std::atomic<void*> read_handle;
        std::atomic<void*> write_handle;

        PendingIO() : read_handle(nullptr), write_handle(nullptr) {}
    };
    std::unordered_map<int, PendingIO> pending_io_;
    mutable std::atomic_flag map_lock_ = ATOMIC_FLAG_INIT; // Spinlock for map structure only

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

    // Remove I/O registration for a file descriptor
    void remove_io(int fd);

    // Get platform I/O interface
    PlatformIO& platform_io() {
        return *platform_io_;
    }

    // Awaitable for async operations
    template <typename T> struct Awaiter {
        T value;
        std::coroutine_handle<> continuation;
        bool ready = false;

        bool await_ready() const noexcept {
            return ready;
        }

        void await_suspend(std::coroutine_handle<> h) {
            continuation = h;
        }

        T await_resume() noexcept {
            return value;
        }
    };
};

// Helper to create awaitable
template <typename T> IOContext::Awaiter<T> make_awaiter(T value) {
    IOContext::Awaiter<T> awaiter;
    awaiter.value = value;
    awaiter.ready = true;
    return awaiter;
}

} // namespace spaznet
