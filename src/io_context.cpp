#include <fcntl.h>
#include <unistd.h>
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

    // Create wakeup pipe (non-blocking) so timer additions can interrupt wait().
    int fds[2]{-1, -1};
    if (pipe(fds) == 0) {
        wake_read_fd_ = fds[0];
        wake_write_fd_ = fds[1];
        // Set non-blocking
        int flags = fcntl(wake_read_fd_, F_GETFL, 0);
        if (flags != -1) {
            fcntl(wake_read_fd_, F_SETFL, flags | O_NONBLOCK);
        }
        flags = fcntl(wake_write_fd_, F_GETFL, 0);
        if (flags != -1) {
            fcntl(wake_write_fd_, F_SETFL, flags | O_NONBLOCK);
        }
        // Register wake_read_fd_ for read events
        platform_io_->add_fd(wake_read_fd_, PlatformIO::EVENT_READ, nullptr);
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

    if (wake_read_fd_ >= 0) {
        // Best-effort remove; ignore failures during shutdown.
        platform_io_->remove_fd(wake_read_fd_);
        close(wake_read_fd_);
        wake_read_fd_ = -1;
    }
    if (wake_write_fd_ >= 0) {
        close(wake_write_fd_);
        wake_write_fd_ = -1;
    }
}

void IOContext::wakeup_event_loop() {
    if (wake_write_fd_ < 0) {
        return;
    }
    // Write a single byte; ignore EAGAIN if pipe is full.
    uint8_t b = 1;
    (void)write(wake_write_fd_, &b, 1);
}

void IOContext::drain_wakeup_pipe() {
    if (wake_read_fd_ < 0) {
        return;
    }
    uint8_t buf[64];
    for (;;) {
        ssize_t n = read(wake_read_fd_, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
    }
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

                // Resume at most once. Do NOT auto-reschedule here:
                // - If it suspended on I/O, process_io_events() will schedule it.
                // - If it suspended on a timer, process_timers() will schedule it.
                // - If it is awaiting another Task, that Task will resume it via continuation.
                if (!task.done()) {
                    task.resume();
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
    // With reference counting, we can safely enqueue the task
    // The handle won't be destroyed while any Task references it
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

            // Resume at most once. Do NOT auto-reschedule:
            // resumption should be driven by I/O/timers/continuations.
            if (!task.done()) {
                task.resume();
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

    // Determine whether this fd was already registered before storing new handles.
    auto it = pending_io_.find(fd);
    bool already_registered = it != pending_io_.end();
    PendingIO& pending = already_registered ? it->second : pending_io_[fd];

    // Convert to our TaskPromise coroutine handle and keep it alive while registered.
    // NOTE: Storing only the raw address is not enough; the coroutine frame may be destroyed
    // while suspended. We must retain a strong reference here.
    uint32_t new_events = 0;
    if (handle) {
        auto task_handle = std::coroutine_handle<TaskPromise>::from_address(handle.address());
        if (task_handle) {
            if (events & PlatformIO::EVENT_READ) {
                pending.read = CoroutineHandle::from_handle(task_handle);
            }
            if (events & PlatformIO::EVENT_WRITE) {
                pending.write = CoroutineHandle::from_handle(task_handle);
            }
        }
    }

    // Enable event bits that currently have waiters.
    if (pending.read) {
        new_events |= PlatformIO::EVENT_READ;
    }
    if (pending.write) {
        new_events |= PlatformIO::EVENT_WRITE;
    }

    // Keep the map lock until after add/modify to avoid rehash invalidating &pending.
    if (already_registered) {
        platform_io_->modify_fd(fd, new_events, &pending);
    } else {
        platform_io_->add_fd(fd, new_events, &pending);
    }

    map_lock_.clear(std::memory_order_release);
}

void IOContext::remove_io(int fd) {
    while (map_lock_.test_and_set(std::memory_order_acquire)) {
        // Spin
    }
    pending_io_.erase(fd);
    map_lock_.clear(std::memory_order_release);
}

void IOContext::process_io_events(const std::vector<PlatformIO::Event>& events) {
    // Extract tasks to schedule BEFORE scheduling (avoid holding lock during schedule)
    std::vector<Task> tasks_to_schedule;

    {
        // Spinlock for map access
        while (map_lock_.test_and_set(std::memory_order_acquire)) {
            // Spin
        }

        for (const auto& ev : events) {
            // Wakeup pipe event: drain and continue.
            if (ev.fd == wake_read_fd_) {
                drain_wakeup_pipe();
                continue;
            }
            // Look up by fd (safe), not by pointer (can be invalidated)
            auto it = pending_io_.find(ev.fd);
            if (it == pending_io_.end()) {
                continue; // fd was removed
            }

            PendingIO& pending = it->second;

            bool changed = false;
            if (ev.events & PlatformIO::EVENT_READ) {
                if (pending.read) {
                    tasks_to_schedule.emplace_back(Task{std::move(pending.read)});
                    pending.read = {};
                    changed = true;
                }
            }
            if (ev.events & PlatformIO::EVENT_WRITE) {
                if (pending.write) {
                    tasks_to_schedule.emplace_back(Task{std::move(pending.write)});
                    pending.write = {};
                    changed = true;
                }
            }

            // Update interest mask to avoid spurious wakeups.
            if (changed) {
                uint32_t new_events = 0;
                if (pending.read) {
                    new_events |= PlatformIO::EVENT_READ;
                }
                if (pending.write) {
                    new_events |= PlatformIO::EVENT_WRITE;
                }
                platform_io_->modify_fd(ev.fd, new_events, &pending);
            }
        }

        map_lock_.clear(std::memory_order_release);
    }
    // Lock released here

    // Now schedule all tasks without holding the lock.
    for (auto& task : tasks_to_schedule) {
        schedule(std::move(task));
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
    bool should_wake = false;
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        // Wake the event loop if this timer is the new earliest timer.
        if (timers_.empty() || first_fire < timers_.top().next_fire) {
            should_wake = true;
        }

        // Track this task as suspended on a timer
        if (task_ptr && task_ptr->handle) {
            suspended_tasks_[task_ptr->handle.address()] = task_ptr;
        }
        timers_.push(TimerEntry{id, first_fire, interval, repeat, task_ptr});
    }
    if (should_wake) {
        wakeup_event_loop();
    }
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
                // Keep intervals stable: schedule next fire relative to "now".
                // This avoids "catch-up" compression where a delayed tick makes the next tick fire
                // too soon (which breaks IntervalStaysPeriodic).
                entry.next_fire = now + entry.interval;
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

        // Create new Task from the handle - reference counting handles ownership
        // The handle from task_ptr is already reference-counted, so we can safely
        // create a new Task from it
        auto task_handle = handle.get();
        if (task_handle) {
            Task task_to_schedule{task_handle};
            // Schedule the task - Task::resume() will check if handle is done
            schedule(std::move(task_to_schedule));
        }
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
