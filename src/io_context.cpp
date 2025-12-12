#include <algorithm>
#include <chrono>
#include <iostream>
#include <libspaznet/io_context.hpp>
#include <libspaznet/platform_io.hpp>
#include <limits>

namespace spaznet {

IOContext::IOContext(std::size_t num_threads)
    : platform_io_(create_platform_io()), thread_queues_(num_threads), running_(false),
      next_queue_(0), num_threads_(num_threads) {
    if (!platform_io_->init()) {
        throw std::runtime_error("Failed to initialize platform I/O");
    }
}

IOContext::~IOContext() {
    stop();
    platform_io_->cleanup();
}

void IOContext::run() {
    running_.store(true, std::memory_order_release);

    // Start worker threads
    for (std::size_t i = 0; i < num_threads_; ++i) {
        worker_threads_.emplace_back(&IOContext::worker_thread, this, i);
    }

    // Main event loop
    std::vector<PlatformIO::Event> events;
    events.reserve(64);

    while (running_.load(std::memory_order_acquire)) {
        // Process timers that are already due before waiting on I/O
        process_timers();

        int timeout_ms = compute_wait_timeout_ms();
        int num_events = platform_io_->wait(events, timeout_ms);

        if (num_events < 0) {
            // Error occurred
            break;
        }

        // Process I/O events
        process_io_events(events);

        // Process timers that became due while handling events
        process_timers();

        // Steal work from queues if needed
        for (auto& queue : thread_queues_) {
            Task task;
            while (queue.dequeue(task)) {
                if (!task.done()) {
                    task.resume();
                    if (!task.done()) {
                        // Task not done, reschedule
                        schedule(std::move(task));
                    }
                }
            }
        }
    }

    // Wait for worker threads
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void IOContext::stop() {
    running_.store(false, std::memory_order_release);
}

void IOContext::schedule(Task task) {
    // Round-robin scheduling
    std::size_t index = next_queue_.fetch_add(1, std::memory_order_acq_rel) % num_threads_;
    thread_queues_[index].enqueue(std::move(task));
}

void IOContext::worker_thread(std::size_t queue_index) {
    TaskQueue& queue = thread_queues_[queue_index];

    while (running_.load(std::memory_order_acquire)) {
        Task task;
        if (queue.dequeue(task)) {
            if (!task.done()) {
                task.resume();
                if (!task.done()) {
                    // Task not done, reschedule
                    schedule(std::move(task));
                }
            }
        } else {
            // No work, yield
            std::this_thread::yield();
        }
    }
}

void IOContext::register_io(int fd, uint32_t events, std::coroutine_handle<> handle) {
    // Spinlock for map structure access only
    while (map_lock_.test_and_set(std::memory_order_acquire)) {
        // Spin - this is brief, just map access
    }

    auto& pending = pending_io_[fd];
    uint32_t new_events = events;

    // Store handles atomically
    void* handle_addr = handle.address();
    if (events & PlatformIO::EVENT_READ) {
        pending.read_handle.store(handle_addr, std::memory_order_release);
    }
    if (events & PlatformIO::EVENT_WRITE) {
        pending.write_handle.store(handle_addr, std::memory_order_release);
    }

    // Check if we need to add or modify
    bool has_read = pending.read_handle.load(std::memory_order_acquire) != nullptr;
    bool has_write = pending.write_handle.load(std::memory_order_acquire) != nullptr;
    bool exists = has_read || has_write;

    map_lock_.clear(std::memory_order_release);

    if (exists) {
        // Combine events
        if (has_read)
            new_events |= PlatformIO::EVENT_READ;
        if (has_write)
            new_events |= PlatformIO::EVENT_WRITE;
        platform_io_->modify_fd(fd, new_events, &pending);
    } else {
        platform_io_->add_fd(fd, new_events, &pending);
    }
}

void IOContext::remove_io(int fd) {
    while (map_lock_.test_and_set(std::memory_order_acquire)) {
        // Spin
    }
    pending_io_.erase(fd);
    map_lock_.clear(std::memory_order_release);
}

void IOContext::process_io_events(const std::vector<PlatformIO::Event>& events) {
    // Extract handles to resume BEFORE scheduling (avoid holding lock during schedule)
    std::vector<std::coroutine_handle<>> handles_to_resume;

    {
        // Spinlock for map access
        while (map_lock_.test_and_set(std::memory_order_acquire)) {
            // Spin
        }

        for (const auto& ev : events) {
            // Look up by fd (safe), not by pointer (can be invalidated)
            auto it = pending_io_.find(ev.fd);
            if (it == pending_io_.end()) {
                continue; // fd was removed
            }

            PendingIO& pending = it->second;

            // Load and clear handles atomically
            if (ev.events & PlatformIO::EVENT_READ) {
                void* addr = pending.read_handle.exchange(nullptr, std::memory_order_acq_rel);
                if (addr) {
                    handles_to_resume.push_back(std::coroutine_handle<>::from_address(addr));
                }
            }
            if (ev.events & PlatformIO::EVENT_WRITE) {
                void* addr = pending.write_handle.exchange(nullptr, std::memory_order_acq_rel);
                if (addr) {
                    handles_to_resume.push_back(std::coroutine_handle<>::from_address(addr));
                }
            }
        }

        map_lock_.clear(std::memory_order_release);
    }
    // Lock released here

    // Now schedule all handles without holding the lock
    for (auto handle : handles_to_resume) {
        // Convert to TaskPromise handle
        auto task_handle = std::coroutine_handle<TaskPromise>::from_address(handle.address());
        schedule(Task{task_handle});
    }
}

uint64_t IOContext::add_timer(std::chrono::steady_clock::time_point first_fire,
                              std::chrono::nanoseconds interval, bool repeat,
                              std::coroutine_handle<> handle) {
    uint64_t id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    // Prevent zero-length repeating intervals from spinning
    if (repeat && interval.count() <= 0) {
        interval = std::chrono::milliseconds(1);
    }
    std::lock_guard<std::mutex> lock(timer_mutex_);
    timers_.push(TimerEntry{id, first_fire, interval, repeat, handle});
    return id;
}

void IOContext::cancel_timer(uint64_t timer_id) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    cancelled_timers_[timer_id] = true;
}

void IOContext::process_timers() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    std::vector<std::coroutine_handle<>> ready;

    {
        std::lock_guard<std::mutex> lock(timer_mutex_);

        while (!timers_.empty()) {
            TimerEntry entry = timers_.top();
            auto cancelled = cancelled_timers_.find(entry.id);
            if (cancelled != cancelled_timers_.end()) {
                timers_.pop();
                cancelled_timers_.erase(cancelled);
                continue;
            }

            if (entry.next_fire > now) {
                break; // Earliest timer not ready
            }

            timers_.pop();

            ready.push_back(entry.handle);

            if (entry.repeat) {
                // Maintain consistent period regardless of processing delay
                do {
                    entry.next_fire += entry.interval;
                } while (entry.next_fire <= now);
                timers_.push(entry);
            }
        }
    }

    for (auto handle : ready) {
        auto task_handle = std::coroutine_handle<TaskPromise>::from_address(handle.address());
        schedule(Task{task_handle});
    }
}

int IOContext::compute_wait_timeout_ms() {
    using namespace std::chrono;
    std::lock_guard<std::mutex> lock(timer_mutex_);

    while (!timers_.empty()) {
        auto top = timers_.top();
        auto cancelled = cancelled_timers_.find(top.id);
        if (cancelled != cancelled_timers_.end()) {
            timers_.pop();
            cancelled_timers_.erase(cancelled);
            continue;
        }

        auto now = steady_clock::now();
        auto next_fire = top.next_fire;

        if (next_fire <= now) {
            return 0;
        }

        auto diff = duration_cast<milliseconds>(next_fire - now);
        auto clamped = std::min<int64_t>(diff.count(), std::numeric_limits<int>::max());
        return static_cast<int>(clamped);
    }

    return 100; // Default timeout when no timers are pending
}

} // namespace spaznet
