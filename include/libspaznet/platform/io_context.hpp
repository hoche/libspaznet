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
struct TaskPromiseBase;
struct TaskPromise;
struct Task;
template <typename T> struct ValueTaskPromise;
template <typename T> struct ValueTask;
class IOContext;
struct CoroutineControlBlock;
void add_ref_coroutine_control_block(CoroutineControlBlock* cb) noexcept;
void release_coroutine_control_block(CoroutineControlBlock* cb) noexcept;

// Recursion-bounding for synchronous coroutine resume chains. A long
// keep-alive HTTP connection with buffered data exercises a pattern
// where every co_await completes synchronously (recv returned data,
// http handler ran in-place, send fit the kernel buffer), so every
// FinalAwaiter/Task-Awaiter does cont.resume() inline. With no
// suspend points the stack grows by one frame per await — 1000
// iterations easily overflows the default macOS pthread stack
// (~512 KiB). ASan catches it as a stack-overflow in
// test_performance's ThroughputTest.
//
// The fix: when the synchronous chain reaches a depth threshold, the
// next inline-resume gets deferred onto a thread-local queue instead.
// When the outermost resume returns to the worker, the queue is
// drained — each entry runs at fresh depth=1, so the chain bounces
// through the queue and the stack stays bounded.
//
// Lifetime: each queued entry optionally carries a CB pointer to
// release_coroutine_control_block AFTER the handle has been resumed,
// preserving the OLD semantics of FinalAwaiter::await_suspend
// (release-after-resume keeps the awaiter's frame alive across the
// resume).
namespace detail {
struct PendingResume {
    std::coroutine_handle<> handle;
    CoroutineControlBlock* cb_to_release; // nullptr if no ref to drop
};
inline thread_local int g_resume_depth = 0;
inline thread_local std::vector<PendingResume> g_pending_resumes;
constexpr int kResumeDepthThreshold = 32;
} // namespace detail

inline void resume_with_depth_bound(std::coroutine_handle<> handle,
                                    CoroutineControlBlock* cb_to_release_after) noexcept {
    if (!handle) {
        if (cb_to_release_after != nullptr) {
            release_coroutine_control_block(cb_to_release_after);
        }
        return;
    }
    if (detail::g_resume_depth >= detail::kResumeDepthThreshold) {
        detail::g_pending_resumes.push_back({handle, cb_to_release_after});
        return;
    }
    ++detail::g_resume_depth;
    handle.resume();
    --detail::g_resume_depth;
    if (cb_to_release_after != nullptr) {
        release_coroutine_control_block(cb_to_release_after);
    }
}

inline void drain_pending_resumes() noexcept {
    while (!detail::g_pending_resumes.empty()) {
        auto pending = std::move(detail::g_pending_resumes);
        for (auto& entry : pending) {
            ++detail::g_resume_depth;
            entry.handle.resume();
            --detail::g_resume_depth;
            if (entry.cb_to_release != nullptr) {
                release_coroutine_control_block(entry.cb_to_release);
            }
        }
    }
}

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
//
// `handle` is type-erased: the runtime only ever needs resume()/done()/
// destroy() on it, none of which require the promise type. `promise`
// holds the same coroutine's promise viewed as TaskPromiseBase, so the
// continuation machinery can reach the base fields without reinterpreting
// one promise type as another (Task vs ValueTask<T> have distinct
// promise types but share the base).
struct CoroutineControlBlock {
    std::coroutine_handle<> handle;
    TaskPromiseBase* promise{nullptr};
    std::atomic<std::size_t> ref_count{1};
};

// Awaiter returned from final_suspend (defined just below, once
// TaskPromiseBase is complete). Defined at namespace scope rather than as
// a local class because its await_suspend is a member template, and local
// classes can't have member templates.
struct TaskFinalAwaiter;

// Shared promise machinery for both the void `Task` and the value-
// carrying `ValueTask<T>`. Holds continuation + control-block state and
// the suspend/resume hooks; the return-channel (return_void vs
// return_value) lives in the derived promises so each coroutine declares
// exactly one of them (declaring both — even via inheritance — is
// ill-formed).
struct TaskPromiseBase {
    // Continuation support:
    // If a coroutine awaits a Task/ValueTask, the awaited coroutine stores
    // a ref-counted pointer to the awaiter's control block here. That keeps
    // the awaiter coroutine alive even if the scheduler drops its Task
    // wrapper while it is suspended.
    CoroutineControlBlock* continuation_cb{nullptr};
    // Fallback continuation for non-Task awaiters.
    std::coroutine_handle<> continuation_raw{std::noop_coroutine()};

    // Per-coroutine control block (allocated in get_return_object).
    // This is the single source of truth for coroutine lifetime management.
    CoroutineControlBlock* control_block{nullptr};

    auto initial_suspend() noexcept {
        return std::suspend_always{};
    }

    // Defined out of line below: the body needs TaskFinalAwaiter complete.
    auto final_suspend() noexcept -> TaskFinalAwaiter;

    void unhandled_exception() {
        std::terminate();
    }
};

// Templated over the concrete promise type P so `handle.promise()` binds
// to the TaskPromiseBase of whichever coroutine (Task or ValueTask<T>) is
// finishing.
struct TaskFinalAwaiter {
    [[nodiscard]] auto await_ready() const noexcept -> bool {
        return false;
    }
    template <typename P> void await_suspend(std::coroutine_handle<P> handle) noexcept {
        TaskPromiseBase& promise = handle.promise();

        if (promise.continuation_cb != nullptr) {
            CoroutineControlBlock* cb = promise.continuation_cb;
            promise.continuation_cb = nullptr;
            // Helper handles the resume + release_cb-after-resume sequence,
            // bouncing through the thread-local queue if the synchronous
            // chain depth would otherwise overflow the stack. cb's ref is
            // consumed by the helper (released after resume() returns, or
            // in the drain loop for deferred entries).
            resume_with_depth_bound(cb->handle, cb);
            return;
        }

        auto cont = promise.continuation_raw;
        promise.continuation_raw = std::noop_coroutine();
        resume_with_depth_bound(cont, nullptr);
    }
    void await_resume() noexcept {}
};

inline auto TaskPromiseBase::final_suspend() noexcept -> TaskFinalAwaiter {
    return TaskFinalAwaiter{};
}

// Allocate + wire up a control block for a freshly-created coroutine.
// Shared by both promises' get_return_object. Defined out of line once
// CoroutineControlBlock and Statistics are complete.
template <typename P> auto make_coroutine_control_block(P& promise) -> CoroutineControlBlock*;

// Void coroutine task.
struct TaskPromise : TaskPromiseBase {
    // Declaration - implementation after Task is defined
    auto get_return_object() -> Task;
    void return_void() {}
};

// Value-carrying coroutine task: co_return yields a T that the awaiter
// observes via `co_await`'s result. Used by Socket::async_read so callers
// can tell bytes-read (>0) from EOF (0) from error (<0) instead of
// inferring it from the output buffer's size.
template <typename T> struct ValueTaskPromise : TaskPromiseBase {
    T value{};
    // Declaration - implementation after ValueTask is defined
    auto get_return_object() -> ValueTask<T>;
    void return_value(T result) {
        value = std::move(result);
    }
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
    // Templated over the promise type so it works for any coroutine whose
    // promise derives from TaskPromiseBase (Task, ValueTask<T>).
    template <typename P> static auto from_handle(std::coroutine_handle<P> h) -> CoroutineHandle {
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

    [[nodiscard]] auto get() const -> std::coroutine_handle<> {
        return cb_ ? cb_->handle : std::coroutine_handle<>{};
    }

    // The owning coroutine's promise, viewed as the shared base. Used by
    // the Task/ValueTask awaiters to wire up continuations without
    // knowing the concrete promise type.
    [[nodiscard]] auto promise_base() const -> TaskPromiseBase* {
        return cb_ ? cb_->promise : nullptr;
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
        return get().address() == other.address();
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

// Shared suspend logic for the Task/ValueTask awaiters. `awaited` is the
// coroutine being co_awaited; `awaiter_cb` is the awaiting coroutine's
// control block (or nullptr for a non-Task awaiter, in which case
// `cont_raw` is used as a bare continuation). Resuming the awaited
// coroutine here drives it forward; when it finishes, its
// TaskFinalAwaiter resumes whichever continuation we stored.
inline void io_task_suspend(const CoroutineHandle& awaited, CoroutineControlBlock* awaiter_cb,
                            std::coroutine_handle<> cont_raw) noexcept {
    TaskPromiseBase* p = awaited.promise_base();
    if (p == nullptr) {
        resume_with_depth_bound(cont_raw, nullptr);
        return;
    }
    // Clear any previous continuation (shouldn't happen, but be safe).
    if (p->continuation_cb != nullptr) {
        release_coroutine_control_block(p->continuation_cb);
        p->continuation_cb = nullptr;
    }
    p->continuation_raw = std::noop_coroutine();

    if (awaiter_cb != nullptr) {
        // Hold a ref to the awaiting coroutine while we are suspended.
        add_ref_coroutine_control_block(awaiter_cb);
        p->continuation_cb = awaiter_cb;
    } else {
        p->continuation_raw = cont_raw;
    }

    if (!awaited.done()) {
        resume_with_depth_bound(awaited.get(), nullptr);
    } else {
        resume_with_depth_bound(cont_raw, nullptr);
    }
}

// Awaiter for `co_await someTask`. Namespace scope (not a local class)
// because await_suspend is a member template. The void Task yields
// nothing; ValueTaskAwaiter<T> below mirrors it but yields the result.
struct TaskAwaiter {
    CoroutineHandle handle;
    [[nodiscard]] auto await_ready() const noexcept -> bool {
        return !handle || handle.done();
    }
    template <typename P> void await_suspend(std::coroutine_handle<P> cont) const noexcept {
        io_task_suspend(handle, cont.promise().control_block, cont);
    }
    void await_suspend(std::coroutine_handle<> cont) const noexcept {
        io_task_suspend(handle, nullptr, cont);
    }
    void await_resume() const noexcept {}
};

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
        return TaskAwaiter{handle};
    }
};

// Awaiter for `co_await someValueTask`. Same suspend logic as TaskAwaiter,
// but await_resume hands back the value the coroutine co_return'd. The
// awaited frame is kept alive by `handle`'s ref while we read it.
template <typename T> struct ValueTaskAwaiter {
    CoroutineHandle handle;
    [[nodiscard]] auto await_ready() const noexcept -> bool {
        return !handle || handle.done();
    }
    template <typename P> void await_suspend(std::coroutine_handle<P> cont) const noexcept {
        io_task_suspend(handle, cont.promise().control_block, cont);
    }
    void await_suspend(std::coroutine_handle<> cont) const noexcept {
        io_task_suspend(handle, nullptr, cont);
    }
    auto await_resume() const noexcept -> T {
        if (!handle) {
            return T{};
        }
        // The frame's promise is exactly ValueTaskPromise<T> here, so this
        // recovers the correct type (no cross-type reinterpretation).
        auto h = std::coroutine_handle<ValueTaskPromise<T>>::from_address(handle.address());
        return std::move(h.promise().value);
    }
};

// Value-carrying coroutine task. Mirrors Task but co_await yields a T.
template <typename T> struct ValueTask {
    using promise_type = ValueTaskPromise<T>;
    CoroutineHandle handle;

    ValueTask() = default;
    explicit ValueTask(CoroutineHandle handle_param) : handle(std::move(handle_param)) {}

    ValueTask(const ValueTask&) = default;
    auto operator=(const ValueTask&) -> ValueTask& = default;
    ValueTask(ValueTask&&) noexcept = default;
    auto operator=(ValueTask&&) noexcept -> ValueTask& = default;
    ~ValueTask() = default;

    [[nodiscard]] auto done() const -> bool {
        return !handle || handle.done();
    }

    auto operator co_await() const noexcept {
        return ValueTaskAwaiter<T>{handle};
    }
};

// Allocate + wire up a control block for a freshly-created coroutine and
// record creation in the statistics. Shared by both promises'
// get_return_object.
template <typename P> inline auto make_coroutine_control_block(P& promise) -> CoroutineControlBlock* {
    auto handle = std::coroutine_handle<P>::from_promise(promise);
    auto* cb = new CoroutineControlBlock{};
    cb->handle = handle;
    cb->promise = &promise; // P derives from TaskPromiseBase
    promise.control_block = cb;

    // Track coroutine creation (lock-free)
    StatisticsInternal* stats = g_statistics.load(std::memory_order_acquire);
    if (stats != nullptr) {
        stats->total_coroutines_created.fetch_add(1, std::memory_order_relaxed);
        stats->active_coroutines.fetch_add(1, std::memory_order_relaxed);
        stats->total_memory_bytes.fetch_add(Statistics::ESTIMATED_COROUTINE_FRAME_SIZE,
                                            std::memory_order_relaxed);
    }
    return cb;
}

// Implement get_return_object after Task / ValueTask are defined.
inline auto TaskPromise::get_return_object() -> Task {
    return Task{CoroutineHandle::adopt(make_coroutine_control_block(*this))};
}

template <typename T> inline auto ValueTaskPromise<T>::get_return_object() -> ValueTask<T> {
    return ValueTask<T>{CoroutineHandle::adopt(make_coroutine_control_block(*this))};
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
        cb->handle = std::coroutine_handle<>{};
        cb->promise = nullptr;

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

// Task queue (Michael-Scott style singly-linked list with sentinel).
//
// This was originally a lock-free MS queue: enqueue did
//     prev_tail = tail_.exchange(new_node);
//     prev_tail->next.store(new_node);
// while dequeue used a mutex to free the previous head. That left a
// classic UAF window: between the tail exchange and the next store,
// another thread's dequeue could free prev_tail. Under load with two
// or more consumers the dangling write corrupted the list (or worse,
// freed memory the allocator had already reused).
//
// The fix here is to take a single mutex on BOTH ends. We give up the
// "lock-free enqueue" property; in exchange the queue is now
// straightforwardly correct, and benchmarking on the affected paths
// showed the bus traffic of the original atomic_exchange dominated
// real work anyway above ~16 cores. If we ever need a true MS queue,
// it must come with hazard pointers or epoch-based reclamation.
class TaskQueue {
  private:
    struct Node {
        Task task;
        Node* next;

        explicit Node(Task task_param) : task(std::move(task_param)), next(nullptr) {}
    };

    Node* head_; // never null after construction; sentinel sits at head_
    Node* tail_;
    mutable std::mutex mutex_;

  public:
    TaskQueue() {
        // Sentinel node: head_ always points at it (or its successor once
        // we've advanced). Sentinel's Task is empty.
        Node* dummy = new Node(Task{});
        head_ = dummy;
        tail_ = dummy;
    }

    // Delete copy and move operations
    TaskQueue(const TaskQueue&) = delete;
    auto operator=(const TaskQueue&) -> TaskQueue& = delete;
    TaskQueue(TaskQueue&&) = delete;
    auto operator=(TaskQueue&&) -> TaskQueue& = delete;

    ~TaskQueue() {
        Node* node = head_;
        while (node != nullptr) {
            Node* next = node->next;
            delete node;
            node = next;
        }
    }

    void enqueue(Task task) {
        Node* node = new Node(std::move(task));
        std::lock_guard<std::mutex> lock(mutex_);
        tail_->next = node;
        tail_ = node;
    }

    auto dequeue(Task& task) -> bool {
        std::lock_guard<std::mutex> lock(mutex_);
        Node* head = head_;
        Node* next = head->next;
        if (next == nullptr) {
            return false; // Queue is empty.
        }
        // Advance the sentinel: the node we just popped becomes the new
        // sentinel, and its successor is the new head's successor.
        head_ = next;
        task = std::move(next->task);
        delete head;
        return true;
    }

    [[nodiscard]] auto empty() const -> bool {
        std::lock_guard<std::mutex> lock(mutex_);
        return head_->next == nullptr;
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

    // Map from file descriptor to pending coroutine handles.
    //
    // Each entry carries a per-registration generation. Every time a
    // coroutine registers on an fd we bump the generation; when an
    // event is delivered from the kernel we compare the generation
    // stored in the event's user_data against the current value here,
    // and drop the event if they disagree. That defeats the fd-reuse
    // race where a close()+accept() reuses a numeric fd while an
    // event for the old registration was still queued.
    struct PendingIO {
        CoroutineHandle read;
        CoroutineHandle write;
        uint32_t generation = 0;

        PendingIO() = default;
    };
    std::unordered_map<int, PendingIO> pending_io_;
    // Guards pending_io_ and the platform-IO side-table mutation done
    // under it (add_fd/modify_fd/remove_fd). Was an atomic_flag spinlock;
    // switched to a mutex so threads waiting on a blocked add_fd/modify_fd
    // syscall park instead of burning CPU spinning.
    mutable std::mutex map_lock_;
    std::atomic<uint32_t> next_generation_{1}; // 0 reserved for the wakeup pipe

    // The user_data we hand to the platform layer is an opaque token,
    // not a pointer to anything dereferenceable. It packs (generation,
    // fd) into 64 bits. A null user_data identifies the wakeup pipe.
    static_assert(sizeof(void*) >= sizeof(uint64_t),
                  "IOContext requires a 64-bit address space to round-trip its event token.");
    static auto encode_token(uint32_t generation, int fd) -> void* {
        const auto packed = (static_cast<uint64_t>(generation) << 32) |
                            static_cast<uint64_t>(static_cast<uint32_t>(fd));
        return reinterpret_cast<void*>(static_cast<uintptr_t>(packed));
    }
    static auto decode_token(void* token, uint32_t& generation_out, int& fd_out) -> void {
        const auto packed = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(token));
        generation_out = static_cast<uint32_t>(packed >> 32);
        fd_out = static_cast<int>(static_cast<int32_t>(packed & 0xFFFFFFFFU));
    }

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

    // Register I/O operation. The handle is the awaiting coroutine,
    // already wrapped in a ref-counted CoroutineHandle by the awaiter
    // (which knows its concrete promise type); register_io keeps it alive
    // until the matching event fires.
    auto register_io(int file_descriptor, uint32_t events, CoroutineHandle handle) -> void;

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
