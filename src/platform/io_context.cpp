#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/platform/io_context.hpp>
#include <libspaznet/platform/platform_io.hpp>
#include <limits>
#include <mutex>

namespace spaznet {

namespace {

#ifdef _WIN32
// Winsock has no pipe(2). A connected TCP loopback pair is pollable the
// same way and is what every portable event-loop uses on Windows.
auto create_wakeup_fds(int& read_fd, int& write_fd) -> bool {
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listener);
        return false;
    }

    int addr_len = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        closesocket(listener);
        return false;
    }

    if (::listen(listener, 1) != 0) {
        closesocket(listener);
        return false;
    }

    const SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        closesocket(listener);
        return false;
    }

    if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(client);
        closesocket(listener);
        return false;
    }

    const SOCKET server = ::accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (server == INVALID_SOCKET) {
        closesocket(client);
        return false;
    }

    if (!detail::set_nonblocking(static_cast<int>(server)) ||
        !detail::set_nonblocking(static_cast<int>(client))) {
        closesocket(server);
        closesocket(client);
        return false;
    }

    read_fd = static_cast<int>(server);
    write_fd = static_cast<int>(client);
    return true;
}
#else
auto create_wakeup_fds(int& read_fd, int& write_fd) -> bool {
    std::array<int, 2> fds{-1, -1};
    if (pipe(fds.data()) != 0) {
        return false;
    }
    read_fd = fds[0];
    write_fd = fds[1];
    int flags = fcntl(read_fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(read_fd, F_SETFL, flags | O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
    }
    flags = fcntl(write_fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(write_fd, F_SETFL, flags | O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
    }
    return true;
}
#endif

} // namespace

IOContext::IOContext(std::size_t num_threads)
    : platform_io_(create_platform_io()),
#ifdef SPAZNET_HAS_COROUTINES
      thread_queues_((std::max)(std::size_t{1}, num_threads == 0 ? 1 : num_threads)),
#endif
      callback_queues_((std::max)(std::size_t{1}, num_threads == 0 ? 1 : num_threads)), running_(false),
      next_queue_(0), num_threads_(num_threads),
      queue_count_((std::max)(std::size_t{1}, num_threads == 0 ? 1 : num_threads)) {
#ifdef _WIN32
    // Must run before platform_io_->init() — IOCP itself, and every
    // socket call the demultiplexer subsequently makes, depends on
    // Winsock being initialised.
    detail::ensure_winsock();
#endif
    if (!platform_io_->init()) {
        throw std::runtime_error("Failed to initialize platform I/O");
    }

    // Create wakeup pipe / socket pair (non-blocking) so timer additions
    // can interrupt wait().
    if (create_wakeup_fds(wake_read_fd_, wake_write_fd_)) {
        platform_io_->add_fd(wake_read_fd_, PlatformIO::EVENT_READ, nullptr);
    }

#ifdef SPAZNET_HAS_COROUTINES
    // Set global statistics pointer for lock-free tracking
    g_statistics.store(&statistics_, std::memory_order_release);
#endif
}

void IOContext::join_workers() {
    std::lock_guard<std::mutex> lock(worker_join_mutex_);
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();
}

IOContext::~IOContext() {
    // Wake parked workers, then join them before any member destructors run.
    // Destroying worker_wake_mutex_ / worker_wake_cv_ while a worker is still
    // inside condition_variable::wait() is undefined behaviour and shows up
    // on macOS ARM64 as std::system_error("mutex lock failed: Invalid argument").
    stop();
    join_workers();

#ifdef SPAZNET_HAS_COROUTINES
    // Clear global statistics pointer
    StatisticsInternal* expected = &statistics_;
    g_statistics.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
#endif

    platform_io_->cleanup();

    if (wake_read_fd_ >= 0) {
        // Best-effort remove; ignore failures during shutdown.
        platform_io_->remove_fd(wake_read_fd_);
        detail::close_socket_fd(wake_read_fd_);
        wake_read_fd_ = -1;
    }
    if (wake_write_fd_ >= 0) {
        detail::close_socket_fd(wake_write_fd_);
        wake_write_fd_ = -1;
    }
}

auto IOContext::wakeup_event_loop() const -> void {
    if (wake_write_fd_ < 0) {
        return;
    }
    // Write a single byte; ignore EAGAIN if the pipe/socket is full.
    const uint8_t byte = 1;
#ifdef _WIN32
    (void)detail::socket_send(wake_write_fd_, &byte, 1, 0);
#else
    (void)::write(wake_write_fd_, &byte, 1);
#endif
}

auto IOContext::drain_wakeup_pipe() const -> void {
    if (wake_read_fd_ < 0) {
        return;
    }
    constexpr std::size_t kDrainChunk = 64;
    std::array<uint8_t, kDrainChunk> buffer{};
    for (;;) {
#ifdef _WIN32
        const ssize_t bytes_read =
            detail::socket_recv(wake_read_fd_, buffer.data(), buffer.size(), 0);
#else
        const ssize_t bytes_read = ::read(wake_read_fd_, buffer.data(), buffer.size());
#endif
        if (bytes_read <= 0) {
            break;
        }
    }
}

void IOContext::run() {
    running_.store(true, std::memory_order_release);

    // Join workers even if the event loop throws — otherwise ~IOContext would
    // destroy worker_wake_mutex_ under a parked condition_variable::wait().
    struct JoinWorkersGuard {
        IOContext* self;
        ~JoinWorkersGuard() {
            self->running_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(self->worker_wake_mutex_);
                self->worker_wake_cv_.notify_all();
            }
            self->wakeup_event_loop();
            self->join_workers();
        }
    } join_guard{this};

    // Start worker threads
    for (std::size_t i = 0; i < num_threads_; ++i) {
        worker_threads_.emplace_back(&IOContext::worker_thread, this, i);
    }

    // Main event loop
    std::vector<PlatformIO::Event> events;
    constexpr std::size_t kInitialEventCapacity = 64;
    events.reserve(kInitialEventCapacity);

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

#ifdef SPAZNET_HAS_COROUTINES
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
                drain_pending_resumes();
            }
        }
#endif

        // Drain posted callbacks (reactor primitive; never coroutine-related).
        for (auto& queue : callback_queues_) {
            std::function<void()> fn;
            while (queue.dequeue(fn)) {
                if (fn) {
                    fn();
                }
            }
        }

        // Reap anything deferred during this iteration's IO/timer/callback
        // dispatch, now that nothing from this iteration is still on the
        // stack.
        drain_reap_list();
    }
}

void IOContext::stop() {
    running_.store(false, std::memory_order_release);
    // Wake every parked worker so it observes running_ == false and exits.
    // Notify under the lock so the store to running_ and the wakeup are
    // visible to a waiter that is between its predicate check and wait().
    {
        std::lock_guard<std::mutex> lock(worker_wake_mutex_);
        worker_wake_cv_.notify_all();
    }
    // Ensure any blocking wait() returns promptly.
    wakeup_event_loop();
}

#ifdef SPAZNET_HAS_COROUTINES
void IOContext::schedule(Task task) {
    // Single-thread fast path: with no worker threads to hand off to,
    // putting the task on a queue just so the I/O thread will dequeue
    // it next loop iteration is pure overhead — a fetch_add, an atomic
    // lock acquire/release on the queue, a wakeup pipe write, and a
    // queue dequeue on the way back. Resume inline instead.
    //
    // (Multi-thread mode still uses the round-robin queues so workers
    // can pick up tasks in parallel.)
    if (num_threads_ == 0) {
        if (task.handle && !task.done()) {
            task.resume();
            drain_pending_resumes();
        }
        return;
    }

    // With reference counting, we can safely enqueue the task
    // The handle won't be destroyed while any Task references it
    // Round-robin scheduling
    std::size_t index = next_queue_.fetch_add(1, std::memory_order_acq_rel) % queue_count_;
    thread_queues_[index].enqueue(std::move(task));
    // Wake a parked worker. Take worker_wake_mutex_ (after the enqueue is
    // visible) so this can't slip between a worker's predicate check and its
    // wait() — that would otherwise lose the wakeup and strand the task.
    {
        std::lock_guard<std::mutex> lock(worker_wake_mutex_);
        worker_wake_cv_.notify_one();
    }
    // If the run() thread is blocked in wait(), wake it too so it can steal
    // and help drain the queues promptly.
    wakeup_event_loop();
}
#endif // SPAZNET_HAS_COROUTINES

void IOContext::defer_destruction(std::shared_ptr<void> obj) {
    if (!obj) {
        return;
    }
    std::lock_guard<std::mutex> lock(reap_mutex_);
    reap_list_.push_back(std::move(obj));
}

void IOContext::drain_reap_list() {
    // Swap out under the lock, destroy outside it: a reaped object's
    // destructor might itself call defer_destruction (e.g. tearing down
    // a child connection), which would deadlock on reap_mutex_ if held
    // across the destroy.
    std::vector<std::shared_ptr<void>> to_destroy;
    {
        std::lock_guard<std::mutex> lock(reap_mutex_);
        if (reap_list_.empty()) {
            return;
        }
        to_destroy.swap(reap_list_);
    }
    // Destructors run here as `to_destroy` goes out of scope.
}

void IOContext::post(std::function<void()> fn) {
    // Unlike schedule(), there is no non-threaded fast path here: post()
    // always queues, even with num_threads_ == 0. Reactor-flavored callers
    // (timers firing plain callbacks, future non-coroutine I/O handlers)
    // rely on that to avoid growing the caller's stack through chained
    // synchronous invocations, mirroring why schedule()'s coroutine fast
    // path needs resume_with_depth_bound in the first place.
    std::size_t index = next_callback_queue_.fetch_add(1, std::memory_order_acq_rel) % queue_count_;
    callback_queues_[index].enqueue(std::move(fn));
    {
        std::lock_guard<std::mutex> lock(worker_wake_mutex_);
        worker_wake_cv_.notify_one();
    }
    wakeup_event_loop();
}

void IOContext::worker_thread(std::size_t queue_index) {
    const std::size_t queue_n = callback_queues_.size();

#ifdef SPAZNET_HAS_COROUTINES
    // True if any thread queue currently holds a task. Called both to grab
    // work and as the CV predicate, so a worker never parks while work is
    // pending. Workers steal across all queues (not just their own) so a
    // notify_one that happens to wake the "wrong" worker still drains the
    // task instead of spinning.
    auto try_take = [&](Task& out) -> bool {
        for (std::size_t k = 0; k < queue_n; ++k) {
            if (thread_queues_[(queue_index + k) % queue_n].dequeue(out)) {
                return true;
            }
        }
        return false;
    };
#endif

    // Same stealing pattern as try_take, for the callback queues.
    auto try_take_callback = [&](std::function<void()>& out) -> bool {
        for (std::size_t k = 0; k < queue_n; ++k) {
            if (callback_queues_[(queue_index + k) % queue_n].dequeue(out)) {
                return true;
            }
        }
        return false;
    };

    while (running_.load(std::memory_order_acquire)) {
#ifdef SPAZNET_HAS_COROUTINES
        Task task;
        if (try_take(task)) {
            // Check if task handle is valid before accessing it
            if (task.handle && !task.done()) {
                // Resume at most once. Do NOT auto-reschedule:
                // resumption should be driven by I/O/timers/continuations.
                task.resume();
            }
            // Drain any continuations deferred by resume_with_depth_bound
            // so a deep synchronous chain that exceeded the threshold
            // makes forward progress before the next dequeue.
            drain_pending_resumes();
            drain_reap_list();
            continue;
        }
#endif

        std::function<void()> fn;
        if (try_take_callback(fn)) {
            if (fn) {
                fn();
            }
            drain_reap_list();
            continue;
        }

        // No work: park until schedule()/post() enqueues something or stop()
        // runs. The predicate is re-checked under worker_wake_mutex_, and
        // both schedule() and post() take that same mutex after enqueuing,
        // so an enqueue that races this park cannot be lost (either the
        // predicate sees it, or the notify arrives after we begin waiting).
        std::unique_lock<std::mutex> lock(worker_wake_mutex_);
        worker_wake_cv_.wait(lock, [this, queue_n] {
            if (!running_.load(std::memory_order_acquire)) {
                return true;
            }
#ifdef SPAZNET_HAS_COROUTINES
            for (std::size_t i = 0; i < queue_n; ++i) {
                if (!thread_queues_[i].empty()) {
                    return true;
                }
            }
#endif
            for (std::size_t i = 0; i < queue_n; ++i) {
                if (!callback_queues_[i].empty()) {
                    return true;
                }
            }
            return false;
        });
    }
}

#ifdef SPAZNET_HAS_COROUTINES
void IOContext::register_io(int file_descriptor, uint32_t events, CoroutineHandle handle) {
    // Thin coroutine-flavored wrapper: adapt the handle to the generic
    // IoHandler interface and register it the same way any other readiness
    // callback would be. See set_io_handler for the shared implementation.
    auto resume_handler = std::make_shared<CoroutineResumeHandler>(*this, std::move(handle));
    set_io_handler(file_descriptor, events, std::move(resume_handler));
}
#endif // SPAZNET_HAS_COROUTINES

void IOContext::set_io_handler(int file_descriptor, uint32_t events,
                               std::shared_ptr<IoHandler> handler) {
    // Guard the pending-io map AND the platform-IO side-table mutation
    // below. This was an atomic_flag spinlock, but add_fd / modify_fd
    // issue a syscall while the lock is held, and a blocked syscall under
    // a spinlock burns CPU on every other thread that needs the map. A
    // std::mutex parks waiters instead. We keep the syscall inside the
    // critical section on purpose: it must stay mutually exclusive with
    // remove_io's remove_fd, which shares this lock to close the
    // platform-layer side-table fd-reuse race (see remove_io).
    //
    // Poll/WSAPoll snapshots the interest set before blocking. A worker
    // that re-arms POLLIN/POLLOUT while wait() is inside that syscall
    // would otherwise wait until the default 100ms timeout. Wake the
    // event loop after releasing map_lock_ so the next wait() sees the
    // updated interest (epoll/kqueue already observe ctl immediately;
    // the extra wakeup byte is harmless there).
    bool need_wakeup = false;
    {
        std::lock_guard<std::mutex> lock(map_lock_);

        // Determine whether this fd was already registered before storing new handles.
        auto pending_it = pending_io_.find(file_descriptor);
        bool already_registered = pending_it != pending_io_.end();
        PendingIO& pending = already_registered ? pending_it->second : pending_io_[file_descriptor];

        // Bump the generation on first registration AND on every re-registration:
        // any event still in the kernel's queue (or already returned to
        // userspace but not yet dispatched) for the previous instance carries
        // the old generation and will be filtered out by process_io_events.
        //
        // generation 0 is the wakeup-pipe sentinel (delivered as a null
        // token). If the counter wraps back to 0 after 2^32-1 registrations,
        // re-roll so a real fd never gets the sentinel generation and has its
        // events silently dropped as wakeup-pipe traffic.
        uint32_t generation = next_generation_.fetch_add(1, std::memory_order_relaxed);
        if (generation == 0) {
            generation = next_generation_.fetch_add(1, std::memory_order_relaxed);
        }
        pending.generation = generation;

        // Keep the handler alive while it is registered (shared_ptr, not a
        // raw pointer): for the coroutine adapter this transitively keeps
        // the coroutine frame alive — it would otherwise be destroyed while
        // suspended.
        uint32_t new_events = 0;
        if (handler) {
            if ((events & PlatformIO::EVENT_READ) != 0) {
                pending.read = handler;
            }
            if ((events & PlatformIO::EVENT_WRITE) != 0) {
                pending.write = handler;
            }
        }

        // Enable event bits that currently have waiters.
        if (pending.read) {
            new_events |= PlatformIO::EVENT_READ;
        }
        if (pending.write) {
            new_events |= PlatformIO::EVENT_WRITE;
        }

        // Pack (generation, fd) as the user_data token so the platform layer
        // can hand it back verbatim on event delivery. The token is opaque
        // to the platform — it must NOT be dereferenced.
        void* token = encode_token(pending.generation, file_descriptor);
        if (already_registered) {
            platform_io_->modify_fd(file_descriptor, new_events, token);
        } else {
            platform_io_->add_fd(file_descriptor, new_events, token);
        }
        need_wakeup = (new_events != 0);
    }

    if (need_wakeup) {
        wakeup_event_loop();
    }
}

void IOContext::remove_io(int file_descriptor) {
    std::lock_guard<std::mutex> lock(map_lock_);
    pending_io_.erase(file_descriptor);
    // Drop the kernel-side registration too while holding the spinlock.
    // Pre-fix this was the caller's responsibility (Socket::close +
    // Server::stop both called platform_io().remove_fd(fd) right after
    // remove_io); but that left a tiny window in which one worker
    // thread's add_fd/modify_fd raced with another's remove_fd over
    // the platform layer's side-table (PlatformIOKqueue::fd_records_,
    // PlatformIOPoll::fd_info_). ASan reliably caught it as a UAF on
    // macOS ARM64 CI runs of test_integration's ConcurrentConnections
    // suite; keep the platform-layer mutation under the same lock as
    // the pending-io map.
    platform_io_->remove_fd(file_descriptor);
}

void IOContext::process_io_events(const std::vector<PlatformIO::Event>& events) {
    // Extract fired handlers BEFORE invoking them (avoid holding the lock
    // while a handler runs — it may call back into set_io_handler/
    // remove_io, e.g. a BufferedConnection re-arming write interest).
    struct FiredHandler {
        std::shared_ptr<IoHandler> handler;
        bool readable; // true => on_readable(), false => on_writable()
    };
    std::vector<FiredHandler> fired;

    {
        std::lock_guard<std::mutex> lock(map_lock_);

        for (const auto& evt : events) {
            // A null token identifies the wakeup pipe (registered with
            // user_data = nullptr in the constructor).
            if (evt.user_data == nullptr) {
                drain_wakeup_pipe();
                continue;
            }
            // Decode the token and verify the generation matches the
            // currently-registered one. A mismatch means this event was
            // produced for an earlier registration of (potentially) the
            // same fd that has since been closed and reused.
            uint32_t token_gen = 0;
            int token_fd = 0;
            decode_token(evt.user_data, token_gen, token_fd);

            auto pending_it = pending_io_.find(token_fd);
            if (pending_it == pending_io_.end()) {
                continue; // fd was removed
            }
            PendingIO& pending = pending_it->second;
            if (pending.generation != token_gen) {
                continue; // stale event for a previous registration on this fd
            }

            bool changed = false;
            if ((evt.events & PlatformIO::EVENT_READ) != 0) {
                if (pending.read) {
                    fired.push_back({std::move(pending.read), true});
                    pending.read = nullptr;
                    changed = true;
                }
            }
            if ((evt.events & PlatformIO::EVENT_WRITE) != 0) {
                if (pending.write) {
                    fired.push_back({std::move(pending.write), false});
                    pending.write = nullptr;
                    changed = true;
                }
            }

            // Update interest mask to avoid spurious wakeups. Re-issue
            // the token (same generation; nothing new was registered).
            if (changed) {
                uint32_t new_events = 0;
                if (pending.read) {
                    new_events |= PlatformIO::EVENT_READ;
                }
                if (pending.write) {
                    new_events |= PlatformIO::EVENT_WRITE;
                }
                platform_io_->modify_fd(token_fd, new_events,
                                        encode_token(pending.generation, token_fd));
            }
        }
    }
    // Lock released here

    // Invoke every fired handler without holding the lock. For the
    // coroutine adapter, on_readable()/on_writable() calls schedule(),
    // reproducing the previous "resume via Task" behavior exactly.
    for (auto& entry : fired) {
        if (entry.readable) {
            entry.handler->on_readable();
        } else {
            entry.handler->on_writable();
        }
    }
}

#ifdef SPAZNET_HAS_COROUTINES
auto IOContext::add_timer(std::chrono::steady_clock::time_point first_fire,
                          std::chrono::nanoseconds interval, bool repeat,
                          const std::shared_ptr<Task>& task_ptr) -> uint64_t {
    uint64_t timer_id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
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
        timers_.push(TimerEntry{timer_id, first_fire, interval, repeat, task_ptr, /*callback=*/{}});
    }
    if (should_wake) {
        wakeup_event_loop();
    }
    return timer_id;
}
#endif // SPAZNET_HAS_COROUTINES

void IOContext::cancel_timer(uint64_t timer_id) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    cancelled_timers_[timer_id] = true;
}

auto IOContext::add_timer_callback(std::chrono::steady_clock::time_point first_fire,
                                   std::chrono::nanoseconds interval, bool repeat,
                                   std::function<void()> callback) -> uint64_t {
    uint64_t timer_id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    if (repeat && interval.count() <= 0) {
        interval = std::chrono::milliseconds(1);
    }
    auto now = std::chrono::steady_clock::now();
    if (first_fire <= now) {
        first_fire = now + std::chrono::milliseconds(1);
    }
    bool should_wake = false;
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (timers_.empty() || first_fire < timers_.top().next_fire) {
            should_wake = true;
        }
        TimerEntry entry;
        entry.id = timer_id;
        entry.next_fire = first_fire;
        entry.interval = interval;
        entry.repeat = repeat;
        entry.callback = std::move(callback);
        timers_.push(std::move(entry));
    }
    if (should_wake) {
        wakeup_event_loop();
    }
    return timer_id;
}

void IOContext::process_timers() {
    using namespace std::chrono;

    // Don't process timers if we're stopping
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    auto now = steady_clock::now();
    // Exactly one of task_ptr/callback is set per entry (see TimerEntry).
    std::vector<TimerEntry> ready_entries;

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

            // Store the entry for later resumption/invocation
            ready_entries.push_back(entry);

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

    for (auto& entry : ready_entries) {
        // Check if context is still running before dispatching
        if (!running_.load(std::memory_order_acquire)) {
            continue;
        }

#ifdef SPAZNET_HAS_COROUTINES
        if (entry.task_ptr) {
            // Coroutine-flavored path (TimerAwaiter / sleep_for / interval).
            auto handle = entry.task_ptr->handle;
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
            Task task_to_schedule{handle};
            // Schedule the task - Task::resume() will check if handle is done
            schedule(std::move(task_to_schedule));
            continue;
        }
#endif
        if (entry.callback) {
            // Reactor-flavored path (add_timer_callback). Routed through
            // post() (always queues) rather than invoked inline here, so a
            // user callback never runs with timer_mutex_-adjacent state on
            // the stack and can't recursively re-enter process_timers.
            post(entry.callback);
        }
    }
}

auto IOContext::compute_wait_timeout_ms() -> int {
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
        auto clamped = (std::min)(diff.count(), static_cast<int64_t>((std::numeric_limits<int>::max)()));
        return static_cast<int>(clamped);
    }

    constexpr int kDefaultTimeoutMs = 100;
    return kDefaultTimeoutMs; // Default timeout when no timers are pending
}

} // namespace spaznet
