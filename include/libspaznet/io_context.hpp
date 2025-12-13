#pragma once

#include <atomic>
#include <chrono>
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
class IOContext;

// Statistics structure for lock-free tracking
// Internal structure uses atomics, snapshot uses plain values for copying
struct Statistics {
    std::size_t active_requests{0};          // Currently active HTTP requests
    std::size_t total_coroutines_created{0}; // Total coroutines created
    std::size_t active_coroutines{0};        // Currently active coroutines
    std::size_t total_memory_bytes{0};       // Estimated memory in use (bytes)

    // Helper to estimate coroutine frame size (approximate)
    static constexpr std::size_t ESTIMATED_COROUTINE_FRAME_SIZE = 512; // bytes per coroutine frame
};

// Internal statistics storage with atomics
struct StatisticsInternal {
    std::atomic<std::size_t> active_requests{0};
    std::atomic<std::size_t> total_coroutines_created{0};
    std::atomic<std::size_t> active_coroutines{0};
    std::atomic<std::size_t> total_memory_bytes{0};
};

// Global pointer to current IOContext statistics (set by IOContext constructor)
inline std::atomic<StatisticsInternal*> g_statistics{nullptr};

// Coroutine task handle
struct TaskPromise {
    std::coroutine_handle<> continuation{std::noop_coroutine()};

    // Declaration - implementation after Task is defined
    auto get_return_object() -> Task;

    auto initial_suspend() noexcept {
        return std::suspend_always{};
    }

    auto final_suspend() noexcept {
        struct FinalAwaiter {
            [[nodiscard]] auto await_ready() const noexcept -> bool {
                return false;
            }
            void await_suspend(std::coroutine_handle<TaskPromise> handle) noexcept {
                auto cont = handle.promise().continuation;
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
    bool owns_handle{true}; // Track if this Task owns the coroutine (can destroy it)

    Task() = default;

    Task(std::coroutine_handle<TaskPromise> handle_param)
        : handle(handle_param), owns_handle(true) {}

    ~Task() {
        if (handle && owns_handle) {
            // Track coroutine destruction (lock-free) before destroying
            StatisticsInternal* stats = g_statistics.load(std::memory_order_acquire);
            if (stats != nullptr) {
                stats->active_coroutines.fetch_sub(1, std::memory_order_relaxed);
                stats->total_memory_bytes.fetch_sub(Statistics::ESTIMATED_COROUTINE_FRAME_SIZE,
                                                    std::memory_order_relaxed);
            }

            handle.destroy();
        }
    }

    Task(const Task&) = delete;
    auto operator=(const Task&) -> Task& = delete;

    Task(Task&& other) noexcept : handle(other.handle), owns_handle(other.owns_handle) {
        other.handle = {};
        other.owns_handle = false;
    }

    auto operator=(Task&& other) noexcept -> Task& {
        if (this != &other) {
            if (handle && owns_handle) {
                // Track coroutine destruction (lock-free) before destroying
                StatisticsInternal* stats = g_statistics.load(std::memory_order_acquire);
                if (stats != nullptr) {
                    stats->active_coroutines.fetch_sub(1, std::memory_order_relaxed);
                    stats->total_memory_bytes.fetch_sub(Statistics::ESTIMATED_COROUTINE_FRAME_SIZE,
                                                        std::memory_order_relaxed);
                }

                handle.destroy();
            }
            handle = other.handle;
            owns_handle = other.owns_handle;
            other.handle = {};
            other.owns_handle = false;
        }
        return *this;
    }

    // Create a non-owning Task view (for timers)
    static auto from_handle(std::coroutine_handle<TaskPromise> handle_param) -> Task {
        Task task;
        task.handle = handle_param;
        task.owns_handle = false;
        return task;
    }

    auto resume() -> bool {
        if ((handle == nullptr) || handle.done()) {
            return false;
        }
        handle.resume();
        return !handle.done();
    }

    [[nodiscard]] auto done() const -> bool {
        return (handle == nullptr) || handle.done();
    }

    // Make Task awaitable
    auto operator co_await() const noexcept {
        struct Awaiter {
            std::coroutine_handle<TaskPromise> handle;
            [[nodiscard]] auto await_ready() const noexcept -> bool {
                return (handle == nullptr) || handle.done();
            }
            void await_suspend(std::coroutine_handle<> cont) const noexcept {
                handle.promise().continuation = cont;
                if ((handle != nullptr) && !handle.done()) {
                    handle.resume();
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
inline auto TaskPromise::get_return_object() -> Task {
    auto handle = std::coroutine_handle<TaskPromise>::from_promise(*this);

    // Track coroutine creation (lock-free)
    StatisticsInternal* stats = g_statistics.load(std::memory_order_acquire);
    if (stats != nullptr) {
        stats->total_coroutines_created.fetch_add(1, std::memory_order_relaxed);
        stats->active_coroutines.fetch_add(1, std::memory_order_relaxed);
        stats->total_memory_bytes.fetch_add(Statistics::ESTIMATED_COROUTINE_FRAME_SIZE,
                                            std::memory_order_relaxed);
    }

    return Task{handle};
}

// Task queue using atomic enqueue and mutex-protected dequeue
// (Simpler and safer than lock-free with hazard pointers)
class TaskQueue {
  private:
    struct Node {
        Task task;
        std::atomic<Node*> next;

        Node(Task task_param) : task(std::move(task_param)), next(nullptr) {}
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

    // Delete copy and move operations
    TaskQueue(const TaskQueue&) = delete;
    auto operator=(const TaskQueue&) -> TaskQueue& = delete;
    TaskQueue(TaskQueue&&) = delete;
    auto operator=(TaskQueue&&) -> TaskQueue& = delete;

    ~TaskQueue() {
        Node* node = head_.load();
        while (node != nullptr) {
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

    auto dequeue(Task& task) -> bool {
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

    [[nodiscard]] auto empty() const -> bool {
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

    // Timer management
    struct TimerEntry {
        uint64_t id;
        std::chrono::steady_clock::time_point next_fire;
        std::chrono::nanoseconds interval;
        bool repeat;
        std::shared_ptr<Task> task_ptr; // Store shared_ptr to keep Task and coroutine alive
    };

    struct TimerCompare {
        bool operator()(const TimerEntry& lhs, const TimerEntry& rhs) const {
            return lhs.next_fire > rhs.next_fire; // Min-heap by next fire
        }
    };

    std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCompare> timers_;
    std::unordered_map<uint64_t, bool> cancelled_timers_;
    std::unordered_map<void*, std::shared_ptr<Task>>
        suspended_tasks_; // Track tasks suspended on timers
    std::mutex timer_mutex_;
    std::atomic<uint64_t> next_timer_id_{1};

    // Map from file descriptor to pending coroutine handles
    // Handles stored as raw addresses for atomic access
    struct PendingIO {
        std::atomic<void*> read_handle;
        std::atomic<void*> write_handle;

        PendingIO() : read_handle(nullptr), write_handle(nullptr) {}
    };
    std::unordered_map<int, PendingIO> pending_io_;
    mutable std::atomic_flag map_lock_ = ATOMIC_FLAG_INIT; // Spinlock for map structure only

    // Statistics tracking (lock-free)
    StatisticsInternal statistics_;

    void worker_thread(std::size_t queue_index);
    void process_io_events(const std::vector<PlatformIO::Event>& events);
    void process_timers();
    int compute_wait_timeout_ms();
    uint64_t add_timer(std::chrono::steady_clock::time_point first_fire,
                       std::chrono::nanoseconds interval, bool repeat,
                       std::shared_ptr<Task> task_ptr);

  public:
    explicit IOContext(std::size_t num_threads = std::thread::hardware_concurrency());
    ~IOContext();

    // Delete copy and move operations
    IOContext(const IOContext&) = delete;
    auto operator=(const IOContext&) -> IOContext& = delete;
    IOContext(IOContext&&) = delete;
    auto operator=(IOContext&&) -> IOContext& = delete;

    // Run the event loop (blocking)
    auto run() -> void;

    // Stop the event loop
    auto stop() -> void;

    // Schedule a coroutine task
    auto schedule(Task task) -> void;

    // Register I/O operation
    auto register_io(int file_descriptor, uint32_t events, std::coroutine_handle<> handle) -> void;

    // Remove I/O registration for a file descriptor
    auto remove_io(int file_descriptor) -> void;

    // Cancel a scheduled timer
    auto cancel_timer(uint64_t timer_id) -> void;

    // Awaitables for timers
    struct TimerAwaiter {
        IOContext* context;
        std::chrono::steady_clock::time_point next_fire;
        std::chrono::nanoseconds interval_duration;
        bool repeat;
        uint64_t id{0};

        [[nodiscard]] auto await_ready() const noexcept -> bool {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle_param) {
            // Create a Task to keep the coroutine alive while suspended on timer
            if (handle_param) {
                auto task_handle =
                    std::coroutine_handle<TaskPromise>::from_address(handle_param.address());
                if (task_handle) {
                    // Create shared_ptr to Task to keep it alive
                    auto task_ptr = std::make_shared<Task>(task_handle);
                    id = context->add_timer(next_fire, interval_duration, repeat, task_ptr);
                }
            }
        }

        void await_resume() const noexcept {}
    };

    TimerAwaiter sleep_for(std::chrono::steady_clock::duration delay) {
        auto now = std::chrono::steady_clock::now();
        auto delay_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay);
        return TimerAwaiter{this, now + delay, delay_ns, false};
    }

    TimerAwaiter sleep_until(std::chrono::steady_clock::time_point time_point) {
        auto now = std::chrono::steady_clock::now();
        auto delay = std::chrono::steady_clock::duration::zero();
        if (time_point > now) {
            delay = time_point - now;
        }
        auto delay_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay);
        return TimerAwaiter{this, now + delay, delay_ns, false};
    }

    TimerAwaiter interval(std::chrono::steady_clock::duration period) {
        auto now = std::chrono::steady_clock::now();
        auto period_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(period);
        return TimerAwaiter{this, now + period, period_ns, true};
    }

    // Get platform I/O interface
    PlatformIO& platform_io() {
        return *platform_io_;
    }

    // Get current statistics (lock-free read)
    [[nodiscard]] auto get_statistics() const -> Statistics {
        Statistics stats;
        stats.active_requests = statistics_.active_requests.load(std::memory_order_acquire);
        stats.total_coroutines_created =
            statistics_.total_coroutines_created.load(std::memory_order_acquire);
        stats.active_coroutines = statistics_.active_coroutines.load(std::memory_order_acquire);
        stats.total_memory_bytes = statistics_.total_memory_bytes.load(std::memory_order_acquire);
        return stats;
    }

    // Increment active requests counter (lock-free)
    void increment_active_requests() {
        statistics_.active_requests.fetch_add(1, std::memory_order_relaxed);
    }

    // Decrement active requests counter (lock-free)
    void decrement_active_requests() {
        statistics_.active_requests.fetch_sub(1, std::memory_order_relaxed);
    }

    // Awaitable for async operations
    template <typename T> struct Awaiter {
        T value;
        std::coroutine_handle<> continuation;
        bool ready = false;

        [[nodiscard]] auto await_ready() const noexcept -> bool {
            return ready;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            continuation = handle;
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
