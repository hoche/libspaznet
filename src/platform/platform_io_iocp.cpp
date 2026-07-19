#ifdef USE_IOCP

// Readiness-style IOCP demultiplexer matching the PlatformIO contract used
// by epoll/kqueue/poll:
//   - opaque user_data tokens round-trip unchanged (generation, fd)
//   - nullptr token = wakeup socket
//   - emit readable/writable; Socket performs the real recv/send
//   - logical oneshot: modify_fd / remove_fd cancel outstanding probes
//
// Connected TCP: zero-byte WSARecv / WSASend probes.
// UDP: zero-byte WSARecv with MSG_PEEK (avoids consuming a datagram).
// Listening sockets: WSAEventSelect(FD_ACCEPT) + RegisterWaitForSingleObject
//   posting a custom completion (zero-byte WSARecv is invalid on listeners).

#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/platform/platform_io.hpp>

#include <windows.h>

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace spaznet {

namespace {

struct OverlappedContext {
    OVERLAPPED overlapped{};
    int fd{-1};
    uint32_t event_bit{0}; // EVENT_READ or EVENT_WRITE
    void* token{nullptr};
};

struct FdRecord {
    void* token{nullptr};
    uint32_t interest{0};
    bool associated{false};
    bool is_listen{false};
    bool is_datagram{false};

    OverlappedContext* read_probe{nullptr};
    OverlappedContext* write_probe{nullptr};

    HANDLE accept_event{nullptr};
    HANDLE accept_wait{nullptr};
};

auto socket_handle(int fd) -> SOCKET {
    return static_cast<SOCKET>(static_cast<UINT_PTR>(fd));
}

auto as_handle(int fd) -> HANDLE {
    return reinterpret_cast<HANDLE>(socket_handle(fd));
}

auto is_listening_socket(int fd) -> bool {
    BOOL accept_conn = FALSE;
    int len = sizeof(accept_conn);
    if (getsockopt(socket_handle(fd), SOL_SOCKET, SO_ACCEPTCONN,
                   reinterpret_cast<char*>(&accept_conn), &len) != 0) {
        return false;
    }
    return accept_conn != FALSE;
}

auto is_datagram_socket(int fd) -> bool {
    int type = 0;
    int len = sizeof(type);
    if (getsockopt(socket_handle(fd), SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &len) !=
        0) {
        return false;
    }
    return type == SOCK_DGRAM;
}

VOID CALLBACK listen_wait_callback(PVOID context, BOOLEAN /*timed_out*/) {
    auto* ctx = static_cast<OverlappedContext*>(context);
    if (ctx == nullptr) {
        return;
    }
    // hEvent temporarily stores the IOCP handle for the callback (see arm_listen_read).
    const HANDLE iocp = ctx->overlapped.hEvent;
    ctx->overlapped.hEvent = nullptr;
    if (iocp != nullptr) {
        PostQueuedCompletionStatus(iocp, 0, 0, &ctx->overlapped);
    }
}

} // namespace

class PlatformIOIOCP : public PlatformIO {
  private:
    HANDLE iocp_handle_{INVALID_HANDLE_VALUE};
    std::unordered_map<int, FdRecord> fds_;
    mutable std::mutex mutex_;

    auto associate_locked(int fd, FdRecord& rec) -> bool {
        if (rec.associated) {
            return true;
        }
        if (CreateIoCompletionPort(as_handle(fd), iocp_handle_,
                                   static_cast<ULONG_PTR>(static_cast<UINT_PTR>(fd)), 0) ==
            nullptr) {
            return false;
        }
        rec.associated = true;
        return true;
    }

    // Probe post failed because the socket is not connected/bound yet.
    // Poll allows registering such fds; keep interest without an outstanding op.
    static auto is_deferred_probe_error(int err) -> bool {
        return err == WSAENOTCONN || err == WSAEDESTADDRREQ || err == WSAEINVAL ||
               err == WSAENOTSOCK;
    }

    auto arm_listen_read_locked(int fd, FdRecord& rec) -> bool {
        if (rec.read_probe != nullptr) {
            return true;
        }

        if (rec.accept_event == nullptr) {
            rec.accept_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (rec.accept_event == nullptr) {
                return false;
            }
            if (WSAEventSelect(socket_handle(fd), rec.accept_event, FD_ACCEPT) != 0) {
                CloseHandle(rec.accept_event);
                rec.accept_event = nullptr;
                return false;
            }
        } else {
            ResetEvent(rec.accept_event);
        }

        auto* ctx = new OverlappedContext{};
        ctx->fd = fd;
        ctx->event_bit = EVENT_READ;
        ctx->token = rec.token;
        // Temporary: stash IOCP handle for the wait callback.
        ctx->overlapped.hEvent = iocp_handle_;

        if (!RegisterWaitForSingleObject(&rec.accept_wait, rec.accept_event, listen_wait_callback,
                                         ctx, INFINITE, WT_EXECUTEONLYONCE)) {
            ctx->overlapped.hEvent = nullptr;
            delete ctx;
            return false;
        }

        rec.read_probe = ctx;

        // WSAEventSelect is edge-oriented: ResetEvent can hide connections
        // already in the listen backlog. If accept is already possible,
        // re-signal so the wait callback posts immediately.
        WSAPOLLFD pfd{};
        pfd.fd = socket_handle(fd);
        pfd.events = POLLRDNORM;
        pfd.revents = 0;
        if (WSAPoll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLRDNORM | POLLIN | POLLERR)) != 0) {
            SetEvent(rec.accept_event);
        }

        return true;
    }

    auto arm_read_probe_locked(int fd, FdRecord& rec) -> bool {
        if (rec.read_probe != nullptr) {
            return true;
        }
        if (rec.is_listen) {
            return arm_listen_read_locked(fd, rec);
        }

        auto* ctx = new OverlappedContext{};
        ctx->fd = fd;
        ctx->event_bit = EVENT_READ;
        ctx->token = rec.token;

        WSABUF buf{};
        buf.buf = nullptr;
        buf.len = 0;
        DWORD bytes = 0;
        DWORD flags = rec.is_datagram ? static_cast<DWORD>(MSG_PEEK) : 0;
        const int rc =
            WSARecv(socket_handle(fd), &buf, 1, &bytes, &flags, &ctx->overlapped, nullptr);
        if (rc == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                delete ctx;
                return is_deferred_probe_error(err);
            }
        }
        rec.read_probe = ctx;
        return true;
    }

    auto arm_write_probe_locked(int fd, FdRecord& rec) -> bool {
        if (rec.write_probe != nullptr || rec.is_listen) {
            return true;
        }

        auto* ctx = new OverlappedContext{};
        ctx->fd = fd;
        ctx->event_bit = EVENT_WRITE;
        ctx->token = rec.token;

        WSABUF buf{};
        buf.buf = nullptr;
        buf.len = 0;
        DWORD bytes = 0;
        const int rc =
            WSASend(socket_handle(fd), &buf, 1, &bytes, 0, &ctx->overlapped, nullptr);
        if (rc == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                delete ctx;
                return is_deferred_probe_error(err);
            }
        }
        rec.write_probe = ctx;
        return true;
    }

    auto cancel_read_probe_locked(int fd, FdRecord& rec) -> void {
        if (rec.read_probe == nullptr) {
            return;
        }

        if (rec.is_listen) {
            if (rec.accept_wait != nullptr) {
                UnregisterWaitEx(rec.accept_wait, INVALID_HANDLE_VALUE);
                rec.accept_wait = nullptr;
            }
            OverlappedContext* ctx = rec.read_probe;
            rec.read_probe = nullptr;
            // Callback clears hEvent before PostQueuedCompletionStatus. If it
            // is still set, the callback never ran — free here. Otherwise a
            // completion is (or will be) queued and wait() frees the ctx.
            if (ctx->overlapped.hEvent != nullptr) {
                ctx->overlapped.hEvent = nullptr;
                delete ctx;
            }
            return;
        }

        OverlappedContext* ctx = rec.read_probe;
        rec.read_probe = nullptr;
        CancelIoEx(as_handle(fd), &ctx->overlapped);
        // wait() frees ctx on ERROR_OPERATION_ABORTED (or success race).
    }

    auto cancel_write_probe_locked(int fd, FdRecord& rec) -> void {
        if (rec.write_probe == nullptr) {
            return;
        }
        OverlappedContext* ctx = rec.write_probe;
        rec.write_probe = nullptr;
        CancelIoEx(as_handle(fd), &ctx->overlapped);
    }

    auto cancel_all_probes_locked(int fd, FdRecord& rec) -> void {
        cancel_read_probe_locked(fd, rec);
        cancel_write_probe_locked(fd, rec);
    }

    auto arm_interest_locked(int fd, FdRecord& rec) -> bool {
        if ((rec.interest & EVENT_READ) != 0U) {
            if (!arm_read_probe_locked(fd, rec)) {
                return false;
            }
        }
        if ((rec.interest & EVENT_WRITE) != 0U) {
            if (!arm_write_probe_locked(fd, rec)) {
                return false;
            }
        }
        return true;
    }

    auto add_fd_locked(int file_descriptor, uint32_t events, void* user_data) -> bool {
        if (fds_.find(file_descriptor) != fds_.end()) {
            return false;
        }

        FdRecord rec{};
        rec.token = user_data;
        rec.interest = events & (EVENT_READ | EVENT_WRITE);
        rec.is_listen = is_listening_socket(file_descriptor);
        rec.is_datagram = is_datagram_socket(file_descriptor);

        // Listen sockets use WSAEventSelect + PostQueuedCompletionStatus and
        // are not associated for WSARecv. Connected / datagram sockets must
        // be associated once.
        if (!rec.is_listen) {
            if (!associate_locked(file_descriptor, rec)) {
                return false;
            }
        }

        if (!arm_interest_locked(file_descriptor, rec)) {
            cancel_all_probes_locked(file_descriptor, rec);
            if (rec.accept_event != nullptr) {
                WSAEventSelect(socket_handle(file_descriptor), nullptr, 0);
                CloseHandle(rec.accept_event);
                rec.accept_event = nullptr;
            }
            return false;
        }

        fds_.emplace(file_descriptor, std::move(rec));
        return true;
    }

    // Clear probe bookkeeping for a completed ctx. Returns false if the fd
    // was removed or the event bit is no longer in interest (stale / cancelled).
    auto note_completion_locked(OverlappedContext* ctx, Event& event) -> bool {
        auto it = fds_.find(ctx->fd);
        if (it == fds_.end()) {
            return false;
        }
        FdRecord& rec = it->second;
        if (rec.read_probe == ctx) {
            rec.read_probe = nullptr;
            if (rec.is_listen) {
                rec.accept_wait = nullptr;
            }
        }
        if (rec.write_probe == ctx) {
            rec.write_probe = nullptr;
        }
        if ((rec.interest & ctx->event_bit) == 0U) {
            return false;
        }
        event.fd = ctx->fd;
        event.user_data = rec.token;
        event.events = ctx->event_bit;
        return true;
    }

  public:
    PlatformIOIOCP() = default;

    PlatformIOIOCP(const PlatformIOIOCP&) = delete;
    auto operator=(const PlatformIOIOCP&) -> PlatformIOIOCP& = delete;
    PlatformIOIOCP(PlatformIOIOCP&&) = delete;
    auto operator=(PlatformIOIOCP&&) -> PlatformIOIOCP& = delete;

    ~PlatformIOIOCP() override {
        cleanup();
    }

    auto init() -> bool override {
        detail::ensure_winsock();
        iocp_handle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        return iocp_handle_ != INVALID_HANDLE_VALUE;
    }

    auto add_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        std::lock_guard<std::mutex> lock(mutex_);
        return add_fd_locked(file_descriptor, events, user_data);
    }

    auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = fds_.find(file_descriptor);
        if (it == fds_.end()) {
            return add_fd_locked(file_descriptor, events, user_data);
        }

        FdRecord& rec = it->second;
        const uint32_t new_interest = events & (EVENT_READ | EVENT_WRITE);
        rec.token = user_data;

        const uint32_t cleared = rec.interest & ~new_interest;
        rec.interest = new_interest;

        if ((cleared & EVENT_READ) != 0U) {
            cancel_read_probe_locked(file_descriptor, rec);
        }
        if ((cleared & EVENT_WRITE) != 0U) {
            cancel_write_probe_locked(file_descriptor, rec);
        }

        if ((new_interest & EVENT_READ) != 0U && rec.read_probe == nullptr) {
            if (!arm_read_probe_locked(file_descriptor, rec)) {
                return false;
            }
        }
        if ((new_interest & EVENT_WRITE) != 0U && rec.write_probe == nullptr) {
            if (!arm_write_probe_locked(file_descriptor, rec)) {
                return false;
            }
        }

        return true;
    }

    auto remove_fd(int file_descriptor) -> bool override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = fds_.find(file_descriptor);
        if (it == fds_.end()) {
            return false;
        }

        FdRecord& rec = it->second;
        cancel_all_probes_locked(file_descriptor, rec);

        if (rec.accept_event != nullptr) {
            WSAEventSelect(socket_handle(file_descriptor), nullptr, 0);
            CloseHandle(rec.accept_event);
            rec.accept_event = nullptr;
        }

        fds_.erase(it);
        return true;
    }

    auto wait(std::vector<Event>& events, int timeout_ms) -> int override {
        events.clear();

        // Skip cancelled completions without consuming the caller's full
        // timeout budget on each abort.
        const auto deadline = (timeout_ms < 0)
                                  ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(timeout_ms);

        for (;;) {
            DWORD timeout = INFINITE;
            if (timeout_ms >= 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    return 0;
                }
                timeout = static_cast<DWORD>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            }

            DWORD bytes_transferred = 0;
            ULONG_PTR completion_key = 0;
            OVERLAPPED* overlapped = nullptr;

            const BOOL ok =
                GetQueuedCompletionStatus(iocp_handle_, &bytes_transferred, &completion_key,
                                          &overlapped, timeout);

            if (overlapped == nullptr) {
                return 0; // timeout
            }

            auto* ctx = reinterpret_cast<OverlappedContext*>(overlapped);

            if (!ok) {
                const DWORD err = GetLastError();
                if (err == ERROR_OPERATION_ABORTED) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto it = fds_.find(ctx->fd);
                        if (it != fds_.end()) {
                            if (it->second.read_probe == ctx) {
                                it->second.read_probe = nullptr;
                            }
                            if (it->second.write_probe == ctx) {
                                it->second.write_probe = nullptr;
                            }
                        }
                    }
                    delete ctx;
                    continue; // try next completion / wait for timeout
                }

                Event event{};
                bool deliver = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    deliver = note_completion_locked(ctx, event);
                    if (deliver) {
                        event.events |= EVENT_ERROR;
                    }
                }
                delete ctx;
                if (!deliver) {
                    continue;
                }
                events.push_back(event);
                return 1;
            }

            // Success — including zero-byte readiness probes. Do NOT map
            // bytes_transferred==0 to EVENT_ERROR; the coroutine's recv()
            // distinguishes "readable" from "peer closed".
            Event event{};
            bool deliver = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                deliver = note_completion_locked(ctx, event);
            }
            delete ctx;
            if (!deliver) {
                continue;
            }
            events.push_back(event);
            return 1;
        }
    }

    void cleanup() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [fd, rec] : fds_) {
            cancel_all_probes_locked(fd, rec);
            if (rec.accept_event != nullptr) {
                WSAEventSelect(socket_handle(fd), nullptr, 0);
                CloseHandle(rec.accept_event);
                rec.accept_event = nullptr;
            }
        }
        fds_.clear();

        if (iocp_handle_ != INVALID_HANDLE_VALUE) {
            for (;;) {
                DWORD bytes = 0;
                ULONG_PTR key = 0;
                OVERLAPPED* ov = nullptr;
                if (!GetQueuedCompletionStatus(iocp_handle_, &bytes, &key, &ov, 0)) {
                    if (ov == nullptr) {
                        break;
                    }
                }
                if (ov != nullptr) {
                    delete reinterpret_cast<OverlappedContext*>(ov);
                }
            }
            CloseHandle(iocp_handle_);
            iocp_handle_ = INVALID_HANDLE_VALUE;
        }
    }
};

} // namespace spaznet

#endif // USE_IOCP
