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

    // Set global statistics pointer for lock-free tracking
    g_statistics.store(&statistics_, std::memory_order_release);
}

IOContext::~IOContext() {
    stop();

    // Clear global statistics pointer
    StatisticsInternal* expected = &statistics_;
    g_statistics.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);

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
                // Check if task handle is valid before accessing it
                if (!task.handle) {
                    continue;
                }

                // Check if this task is suspended on a timer BEFORE checking done()
                // This avoids accessing potentially destroyed coroutine frame
                void* handle_addr = task.handle.address();
                bool is_suspended_on_timer = false;
                {
                    std::lock_guard<std::mutex> timer_lock(timer_mutex_);
                    auto it = suspended_tasks_.find(handle_addr);
                    if (it != suspended_tasks_.end()) {
                        is_suspended_on_timer = true;
                    }
                }

                // If suspended on timer, don't check done() or resume - timer will handle it
                if (is_suspended_on_timer) {
                    // Make Task non-owning - timer will handle resumption
                    task.owns_handle = false;
                    {
                        std::lock_guard<std::mutex> timer_lock(timer_mutex_);
                        suspended_tasks_.erase(handle_addr);
                    }
                    // Don't reschedule - timer will do it
                    continue;
                }

                // Not suspended on timer - proceed normally
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
            // Check if task handle is valid before accessing it
            if (!task.handle) {
                continue;
            }

            if (!task.done()) {
                task.resume();
                if (!task.done()) {
                    // Check if this task is suspended on a timer
                    // by looking it up in the suspended_tasks_ map
                    void* handle_addr = task.handle.address();
                    bool is_suspended_on_timer = false;
                    {
                        std::lock_guard<std::mutex> timer_lock(timer_mutex_);
                        auto it = suspended_tasks_.find(handle_addr);
                        if (it != suspended_tasks_.end()) {
                            is_suspended_on_timer = true;
                            // Task is suspended on a timer - make it non-owning
                            // The timer will handle resumption
                            task.owns_handle = false;
                            // Don't reschedule - timer will do it
                            suspended_tasks_.erase(it);
                        }
                    }
                    if (!is_suspended_on_timer) {
                        // Task not done and not on timer, reschedule
                        schedule(std::move(task));
                    }
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
        // Check if handle is valid before converting
        if (!handle || handle.done()) {
            continue;
        }
        // Convert to TaskPromise handle
        auto task_handle = std::coroutine_handle<TaskPromise>::from_address(handle.address());
        // Additional validation - ensure handle is still valid
        if (task_handle && !task_handle.done()) {
            schedule(Task{task_handle});
        }
    }
}

uint64_t IOContext::add_timer(std::chrono::steady_clock::time_point first_fire,
                              std::chrono::nanoseconds interval, bool repeat,
                              std::shared_ptr<Task> task_ptr) {
    uint64_t id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    // Prevent zero-length repeating intervals from spinning
    if (repeat && interval.count() <= 0) {
        interval = std::chrono::milliseconds(1);
    }
    // Ensure the timer fires at least 1ms in the future to avoid immediate firing
    auto now = std::chrono::steady_clock::now();
    if (first_fire <= now) {
        first_fire = now + std::chrono::milliseconds(1);
    }
    std::lock_guard<std::mutex> lock(timer_mutex_);
    // Track this task as suspended on a timer
    if (task_ptr && task_ptr->handle) {
        suspended_tasks_[task_ptr->handle.address()] = task_ptr;
    }
    timers_.push(TimerEntry{id, first_fire, interval, repeat, task_ptr});
    return id;
}

void IOContext::cancel_timer(uint64_t timer_id) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    cancelled_timers_[timer_id] = true;
}

void IOContext::process_timers() {
    using namespace std::chrono;

    // Don't process timers if we're stopping
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    auto now = steady_clock::now();
    std::vector<std::shared_ptr<Task>> ready_tasks;

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

            // Store task for later resumption
            ready_tasks.push_back(entry.task_ptr);

            if (entry.repeat) {
                // Maintain consistent period regardless of processing delay
                do {
                    entry.next_fire += entry.interval;
                } while (entry.next_fire <= now);
                // Re-add the entry with the same task_ptr
                timers_.push(entry);
            }
        }
    }

    // Check again before processing - context might have stopped
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    for (auto& task_ptr : ready_tasks) {
        // Check if context is still running before scheduling
        if (!running_.load(std::memory_order_acquire)) {
            continue;
        }

        // Check if task_ptr is valid
        if (!task_ptr) {
            continue;
        }

        // Extract the handle first
        auto handle = task_ptr->handle;
        if (!handle) {
            continue;
        }

        // Remove from suspended_tasks_ map since timer is firing
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            suspended_tasks_.erase(handle.address());
        }

        // Clear the handle in the shared Task and make it non-owning
        // This transfers ownership to the new Task we'll create
        task_ptr->handle = {};
        task_ptr->owns_handle = false;

        // Create new Task with ownership of the handle
        Task task_to_schedule{handle};
        task_to_schedule.owns_handle = true;

        // Schedule the task - Task::resume() will check if handle is done
        schedule(std::move(task_to_schedule));
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
