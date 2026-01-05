#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <libspaznet/platform/platform_io.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

// IOContext is a low-level coroutine runtime; it intentionally uses some patterns clang-tidy flags
// (e.g. coroutine machinery, internal control blocks). Suppress noisy checks to keep analysis
// actionable.
// NOLINTBEGIN

namespace spaznet {

// Forward declarations
struct TaskPromise;
struct Task;
class IOContext;
struct CoroutineControlBlock;
void add_ref_coroutine_control_block(CoroutineControlBlock* cb) noexcept;
void release_coroutine_control_block(CoroutineControlBlock* cb) noexcept;

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

// Per-coroutine control block (one per coroutine frame).
// Stored in the coroutine's promise and referenced by CoroutineHandle/Task/IOContext.
// NOTE: TaskPromise is forward-declared here; std::coroutine_handle<TaskPromise> is OK as a member.
struct CoroutineControlBlock {
    std::coroutine_handle<TaskPromise> handle;
    std::atomic<std::size_t> ref_count{1};
};

// Coroutine task handle
struct TaskPromise {
    // Continuation support:
    // If a coroutine (TaskPromise) awaits another Task, the awaited Task stores a ref-counted
    // pointer to the awaiter's control block here. That keeps the awaiter coroutine alive even
    // if the scheduler drops its Task wrapper while it is suspended.
    CoroutineControlBlock* continuation_cb{nullptr};
    // Fallback continuation for non-Task awaiters.
    std::coroutine_handle<> continuation_raw{std::noop_coroutine()};

    // Per-coroutine control block (allocated in get_return_object).
    // This is the single source of truth for coroutine lifetime management.
    CoroutineControlBlock* control_block{nullptr};

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
                auto& promise = handle.promise();

                if (promise.continuation_cb != nullptr) {
                    CoroutineControlBlock* cb = promise.continuation_cb;
                    promise.continuation_cb = nullptr;

                    auto cont = cb->handle;
                    if (cont) {
                        cont.resume();
                    }
                    release_coroutine_control_block(cb);
                    return;
                }

                auto cont = promise.continuation_raw;
                promise.continuation_raw = std::noop_coroutine();
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

// Intrusive reference-counted coroutine handle wrapper.
// Critical invariant: there is exactly one CoroutineControlBlock per coroutine frame.
class CoroutineHandle {
  private:
    CoroutineControlBlock* cb_{nullptr};

    explicit CoroutineHandle(CoroutineControlBlock* cb, bool add_ref) : cb_(cb) {
        if (cb_ != nullptr && add_ref) {
            cb_->ref_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void release() noexcept {
        CoroutineControlBlock* cb = cb_;
        cb_ = nullptr;
        if (cb == nullptr) {
            return;
        }
        release_coroutine_control_block(cb);
    }

  public:
    CoroutineHandle() = default;

    // Adopt an existing reference (used only by TaskPromise::get_return_object).
    static auto adopt(CoroutineControlBlock* cb) -> CoroutineHandle {
        return CoroutineHandle(cb, /*add_ref=*/false);
    }

    // Obtain a CoroutineHandle from an existing coroutine handle (adds a ref).
    static auto from_handle(std::coroutine_handle<TaskPromise> h) -> CoroutineHandle {
        if (!h) {
            return {};
        }
        CoroutineControlBlock* cb = h.promise().control_block;
        if (cb == nullptr) {
            return {};
        }
        return CoroutineHandle(cb, /*add_ref=*/true);
    }

    // Intrusive helpers for TaskPromise continuation handling.
    static void add_ref(CoroutineControlBlock* cb) noexcept {
        add_ref_coroutine_control_block(cb);
    }

    CoroutineHandle(const CoroutineHandle& other) : cb_(other.cb_) {
        if (cb_ != nullptr) {
            cb_->ref_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    CoroutineHandle& operator=(const CoroutineHandle& other) {
        if (this == &other) {
            return *this;
        }
        release();
        cb_ = other.cb_;
        if (cb_ != nullptr) {
            cb_->ref_count.fetch_add(1, std::memory_order_relaxed);
        }
        return *this;
    }

    CoroutineHandle(CoroutineHandle&& other) noexcept : cb_(other.cb_) {
        other.cb_ = nullptr;
    }

    CoroutineHandle& operator=(CoroutineHandle&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        release();
        cb_ = other.cb_;
        other.cb_ = nullptr;
        return *this;
    }

    ~CoroutineHandle() {
        release();
    }

    [[nodiscard]] auto get() const -> std::coroutine_handle<TaskPromise> {
        return cb_ ? cb_->handle : std::coroutine_handle<TaskPromise>{};
    }

    [[nodiscard]] auto address() const -> void* {
        auto h = get();
        return h ? h.address() : nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return cb_ != nullptr && cb_->handle;
    }

    [[nodiscard]] auto done() const -> bool {
        auto h = get();
        return !h || h.done();
    }

    void resume() const {
        auto h = get();
        if (h) {
            h.resume();
        }
    }

    [[nodiscard]] auto ref_count() const -> std::size_t {
        return cb_ ? cb_->ref_count.load(std::memory_order_acquire) : 0;
    }

    [[nodiscard]] auto operator==(const std::coroutine_handle<TaskPromise>& other) const noexcept
        -> bool {
        return get() == other;
    }

    [[nodiscard]] auto operator!=(const std::coroutine_handle<TaskPromise>& other) const noexcept
        -> bool {
        return !(*this == other);
    }

    [[nodiscard]] auto operator==(const CoroutineHandle& other) const noexcept -> bool {
        return cb_ == other.cb_;
    }

    [[nodiscard]] auto operator!=(const CoroutineHandle& other) const noexcept -> bool {
        return !(*this == other);
    }
};

// Non-member comparison operators for reverse comparison
[[nodiscard]] inline auto operator==(const std::coroutine_handle<TaskPromise>& lhs,
                                     const CoroutineHandle& rhs) noexcept -> bool {
    return rhs == lhs;
}

[[nodiscard]] inline auto operator!=(const std::coroutine_handle<TaskPromise>& lhs,
                                     const CoroutineHandle& rhs) noexcept -> bool {
    return rhs != lhs;
}

struct Task {
    using promise_type = TaskPromise;
    CoroutineHandle handle; // Reference-counted handle

    Task() = default;

    explicit Task(std::coroutine_handle<TaskPromise> handle_param)
        : handle(CoroutineHandle::from_handle(handle_param)) {}

    explicit Task(CoroutineHandle handle_param) : handle(std::move(handle_param)) {}

    // Copy constructor - increments reference count
    Task(const Task& other) = default;

    // Copy assignment - handles reference counting automatically
    auto operator=(const Task& other) -> Task& = default;

    // Move constructor - transfers ownership
    Task(Task&& other) noexcept = default;

    // Move assignment - handles reference counting automatically
    auto operator=(Task&& other) noexcept -> Task& = default;

    // Destructor - decrements reference count (handled by CoroutineHandle)
    ~Task() = default;

    // Create a Task from a handle (increments reference count)
    static auto from_handle(std::coroutine_handle<TaskPromise> handle_param) -> Task {
        return Task(handle_param);
    }

    auto resume() -> bool {
        if (!handle) {
            return false;
        }
        // Check if done - handle is reference-counted, so it's safe
        if (handle.done()) {
            return false;
        }
        handle.resume();
        // Check if done after resume
        return !handle.done();
    }

    [[nodiscard]] auto done() const -> bool {
        if (!handle) {
            return true;
        }
        // Check if done - handle is reference-counted, so it's safe
        return handle.done();
    }

    // Make Task awaitable
    auto operator co_await() const noexcept {
        struct Awaiter {
            CoroutineHandle handle;
            [[nodiscard]] auto await_ready() const noexcept -> bool {
                return !handle || handle.done();
            }
            // Awaiting from a Task coroutine: store a ref-counted continuation to keep the awaiter
            // alive.
            void await_suspend(std::coroutine_handle<TaskPromise> cont) const noexcept {
                auto task_handle = handle.get();
                if (!task_handle) {
                    cont.resume();
                    return;
                }

                auto& p = task_handle.promise();
                // Clear any previous continuation (shouldn't happen, but be safe)
                if (p.continuation_cb != nullptr) {
                    release_coroutine_control_block(p.continuation_cb);
                    p.continuation_cb = nullptr;
                }
                p.continuation_raw = std::noop_coroutine();

                // Hold a ref to the awaiting coroutine while we are suspended.
                CoroutineControlBlock* awaiter_cb = cont.promise().control_block;
                p.continuation_cb = awaiter_cb;
                add_ref_coroutine_control_block(awaiter_cb);

                if (!handle.done()) {
                    handle.resume();
                } else {
                    cont.resume();
                }
            }

            // Fallback for non-Task awaiters.
            void await_suspend(std::coroutine_handle<> cont) const noexcept {
                auto task_handle = handle.get();
                if (!task_handle) {
                    cont.resume();
                    return;
                }

                auto& p = task_handle.promise();
                if (p.continuation_cb != nullptr) {
                    release_coroutine_control_block(p.continuation_cb);
                    p.continuation_cb = nullptr;
                }
                p.continuation_raw = cont;

                if (!handle.done()) {
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
    auto* cb = new CoroutineControlBlock{handle};
    control_block = cb;

    // Track coroutine creation (lock-free)
    StatisticsInternal* stats = g_statistics.load(std::memory_order_acquire);
    if (stats != nullptr) {
        stats->total_coroutines_created.fetch_add(1, std::memory_order_relaxed);
        stats->active_coroutines.fetch_add(1, std::memory_order_relaxed);
        stats->total_memory_bytes.fetch_add(Statistics::ESTIMATED_COROUTINE_FRAME_SIZE,
                                            std::memory_order_relaxed);
    }

    return Task{CoroutineHandle::adopt(cb)};
}

// Intrusive ref-count helpers (used by TaskPromise/Task awaiter).
inline void add_ref_coroutine_control_block(CoroutineControlBlock* cb) noexcept {
    if (cb != nullptr) {
        cb->ref_count.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void release_coroutine_control_block(CoroutineControlBlock* cb) noexcept {
    if (cb == nullptr) {
        return;
    }
    if (cb->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        auto h = cb->handle;
        cb->handle = std::coroutine_handle<TaskPromise>{};

        // Track coroutine destruction (lock-free)
        StatisticsInternal* stats = g_statistics.load(std::memory_order_acquire);
        if (stats != nullptr) {
            stats->active_coroutines.fetch_sub(1, std::memory_order_relaxed);
            stats->total_memory_bytes.fetch_sub(Statistics::ESTIMATED_COROUTINE_FRAME_SIZE,
                                                std::memory_order_relaxed);
        }

        if (h) {
            h.destroy();
        }
        delete cb;
    }
}

// Task queue using lock-free enqueue and dequeue
// Single-consumer design: each worker thread has its own queue
// Enqueue is multi-producer, dequeue is single-consumer (lock-free)
class TaskQueue {
  private:
    struct Node {
        Task task;
        std::atomic<Node*> next;

        Node(Task task_param) : task(std::move(task_param)), next(nullptr) {}
    };

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    mutable std::mutex dequeue_mutex_; // Protects dequeue to avoid use-after-free

  public:
    TaskQueue() {
        // Create dummy node with default-constructed Task (null handle is fine for dummy)
        Node* dummy = new Node(Task{});
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    // Delete copy and move operations
    TaskQueue(const TaskQueue&) = delete;
    auto operator=(const TaskQueue&) -> TaskQueue& = delete;
    TaskQueue(TaskQueue&&) = delete;
    auto operator=(TaskQueue&&) -> TaskQueue& = delete;

    ~TaskQueue() {
        Node* node = head_.load(std::memory_order_acquire);
        while (node != nullptr) {
            Node* next = node->next.load(std::memory_order_acquire);
            delete node;
            node = next;
        }
    }

    // Multi-producer enqueue (lock-free)
    void enqueue(Task task) {
        Node* node = new Node(std::move(task));
        Node* prev_tail = tail_.exchange(node, std::memory_order_acq_rel);
        prev_tail->next.store(node, std::memory_order_release);
    }

    // Multi-consumer dequeue (protected by mutex to avoid use-after-free)
    // Note: This is not fully lock-free, but ensures thread safety
    // A fully lock-free implementation would require hazard pointers or epoch-based reclamation
    auto dequeue(Task& task) -> bool {
        std::lock_guard<std::mutex> lock(dequeue_mutex_);

        Node* head = head_.load(std::memory_order_acquire);
        if (head == nullptr) {
            return false;
        }

        Node* next = head->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            return false; // Queue is empty
        }

        // Update head atomically
        head_.store(next, std::memory_order_release);

        // Safe to access next->task and delete head (protected by mutex)
        task = std::move(next->task);
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
    // Number of worker threads to spawn (0 means fully non-threaded: everything runs on run()
    // thread).
    std::size_t num_threads_;
    // Number of task queues. Must be >= 1 so schedule() never divides by zero.
    std::size_t queue_count_;

    // Wakeup pipe: lets worker threads interrupt PlatformIO::wait() when they add an earlier timer.
    int wake_read_fd_{-1};
    int wake_write_fd_{-1};

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
        CoroutineHandle read;
        CoroutineHandle write;

        PendingIO() = default;
    };
    std::unordered_map<int, PendingIO> pending_io_;
    mutable std::atomic_flag map_lock_ = ATOMIC_FLAG_INIT; // Spinlock for map structure only

    // Statistics tracking (lock-free)
    StatisticsInternal statistics_;

    auto wakeup_event_loop() const -> void;
    auto drain_wakeup_pipe() const -> void;

    void worker_thread(std::size_t queue_index);
    void process_io_events(const std::vector<PlatformIO::Event>& events);
    void process_timers();
    auto compute_wait_timeout_ms() -> int;
    auto add_timer(std::chrono::steady_clock::time_point first_fire,
                   std::chrono::nanoseconds interval, bool repeat,
                   const std::shared_ptr<Task>& task_ptr) -> uint64_t;

  public:
    // `num_threads` is the number of worker threads to spawn (not counting the calling thread
    // running `run()`). Default is 0, meaning non-threaded mode.
    explicit IOContext(std::size_t num_threads = 0);
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
        // IMPORTANT: `interval()` is typically used as `co_await ctx.interval(period)` inside a
        // loop. In that pattern, each await creates a fresh TimerAwaiter; making it repeating would
        // leak repeating timers and can resume the coroutine multiple times back-to-back.
        //
        // So we schedule a one-shot timer for now+period here.
        return TimerAwaiter{this, now + period, period_ns, false};
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

// NOLINTEND

} // namespace spaznet
