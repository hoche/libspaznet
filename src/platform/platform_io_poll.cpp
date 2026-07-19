#ifndef USE_EPOLL
#ifndef USE_KQUEUE
#ifndef USE_IOCP

#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/platform/platform_io.hpp>

#include <cerrno>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
// WSAPoll uses the same pollfd layout / event flags as POSIX poll.
using pollfd = WSAPOLLFD;
inline auto poll(pollfd* fds, unsigned long nfds, int timeout) -> int {
    return WSAPoll(fds, static_cast<ULONG>(nfds), timeout);
}
#else
#include <poll.h>
#endif

namespace spaznet {

class PlatformIOPoll : public PlatformIO {
  private:
    std::vector<pollfd> pollfds_;
    // fd → (user_data token, requested events). poll has no place on the
    // wire to round-trip caller-supplied data, so we keep it here and
    // hand it back in wait().
    std::unordered_map<int, std::pair<void*, uint32_t>> fd_info_;

    // Guards pollfds_ and fd_info_. Unlike the epoll/kqueue backends —
    // whose registration state lives in the kernel, so their wait()
    // touches no shared userspace table — poll keeps its interest set in
    // these two containers and reads them in wait(). Since wait() runs on
    // the event-loop thread while worker threads call add_fd/modify_fd/
    // remove_fd (IOContext holds its own map_lock_ there, which wait()
    // does NOT take), those accesses would race and reallocate the vector
    // / rehash the map out from under wait(). This mutex serializes them.
    // wait() only holds it to snapshot / map results, never across the
    // blocking poll() syscall, so registrations don't stall.
    mutable std::mutex mutex_;

  public:
    PlatformIOPoll() = default;

    // Delete copy and move operations
    PlatformIOPoll(const PlatformIOPoll&) = delete;
    auto operator=(const PlatformIOPoll&) -> PlatformIOPoll& = delete;
    PlatformIOPoll(PlatformIOPoll&&) = delete;
    auto operator=(PlatformIOPoll&&) -> PlatformIOPoll& = delete;

    ~PlatformIOPoll() override {
        cleanup();
    }

    auto init() -> bool override {
        return true;
    }

    // Unlocked insert; caller must hold mutex_. Shared by add_fd and
    // modify_fd's "not yet registered" fallback so we never re-lock.
    auto add_fd_locked(int file_descriptor, uint32_t events, void* user_data) -> bool {
        if (fd_info_.find(file_descriptor) != fd_info_.end()) {
            return false; // Already exists
        }

        pollfd pfd{};
        pfd.fd = static_cast<decltype(pfd.fd)>(file_descriptor);
        pfd.events = 0;
        if ((events & EVENT_READ) != 0U) {
            pfd.events |= POLLIN;
        }
        if ((events & EVENT_WRITE) != 0U) {
            pfd.events |= POLLOUT;
        }
        pfd.revents = 0;

        pollfds_.push_back(pfd);
        fd_info_[file_descriptor] = {user_data, events};

        return true;
    }

    auto add_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        std::lock_guard<std::mutex> lock(mutex_);
        return add_fd_locked(file_descriptor, events, user_data);
    }

    auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = fd_info_.find(file_descriptor);
        if (it == fd_info_.end()) {
            return add_fd_locked(file_descriptor, events, user_data);
        }

        it->second = {user_data, events};

        for (auto& pfd : pollfds_) {
            if (pfd.fd == static_cast<decltype(pfd.fd)>(file_descriptor)) {
                pfd.events = 0;
                if ((events & EVENT_READ) != 0U) {
                    pfd.events |= POLLIN;
                }
                if ((events & EVENT_WRITE) != 0U) {
                    pfd.events |= POLLOUT;
                }
                break;
            }
        }

        return true;
    }

    auto remove_fd(int file_descriptor) -> bool override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = fd_info_.find(file_descriptor);
        if (it == fd_info_.end()) {
            return false;
        }

        fd_info_.erase(it);

        for (auto pfd_it = pollfds_.begin(); pfd_it != pollfds_.end(); ++pfd_it) {
            if (pfd_it->fd == static_cast<decltype(pfd_it->fd)>(file_descriptor)) {
                pollfds_.erase(pfd_it);
                break;
            }
        }

        return true;
    }

    auto wait(std::vector<Event>& events, int timeout_ms) -> int override {
        // Snapshot the interest set under the lock, then run the blocking
        // poll() on the copy WITHOUT the lock held — otherwise a worker
        // thread's add_fd/modify_fd/remove_fd would block until poll()
        // returns. poll() itself only reads its argument array (revents
        // are written into our local copy), so a snapshot is race-free.
        std::vector<pollfd> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pollfds_.empty()) {
                return 0;
            }
            snapshot = pollfds_;
        }

        // EINTR is benign; retry so the loop survives signals.
        int nfds;
        do {
            nfds = poll(snapshot.data(), static_cast<unsigned long>(snapshot.size()), timeout_ms);
        } while (nfds < 0 && detail::last_socket_error() ==
#ifdef _WIN32
                                 WSAEINTR
#else
                                 EINTR
#endif
        );

        if (nfds < 0) {
            return -1;
        }

        events.clear();
        events.reserve(static_cast<std::size_t>(nfds));

        // Re-acquire the lock to translate revents into Events and look up
        // each fd's token. An fd may have been removed between the snapshot
        // and now; if it's no longer in fd_info_ we drop the event rather
        // than reporting a null token (which IOContext would misread as the
        // wakeup pipe). The real wakeup pipe stays in fd_info_ with a null
        // token, so it is still reported correctly.
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& pfd : snapshot) {
            if (pfd.revents == 0) {
                continue;
            }

            auto info_it = fd_info_.find(static_cast<int>(pfd.fd));
            if (info_it == fd_info_.end()) {
                continue; // fd was removed after the snapshot
            }

            Event event{};
            event.fd = static_cast<int>(pfd.fd);
            event.events = 0;

            if ((pfd.revents & POLLIN) != 0U) {
                event.events |= EVENT_READ;
            }
            if ((pfd.revents & POLLOUT) != 0U) {
                event.events |= EVENT_WRITE;
            }
            if ((pfd.revents & (POLLERR | POLLHUP)) != 0U) {
                // Wake any read-waiter on EOF so recv() can return 0
                // for an orderly half-close. POLLNVAL means the fd was
                // closed under us — flag only as an error.
                event.events |= EVENT_READ | EVENT_ERROR;
            }
            if ((pfd.revents & POLLNVAL) != 0U) {
                event.events |= EVENT_ERROR;
            }

            // Hand the caller's token back unchanged so IOContext can
            // verify the registration generation.
            event.user_data = info_it->second.first;

            events.push_back(event);
        }

        return nfds;
    }

    void cleanup() override {
        std::lock_guard<std::mutex> lock(mutex_);
        pollfds_.clear();
        fd_info_.clear();
    }
};

} // namespace spaznet

#endif // !USE_IOCP
#endif // !USE_KQUEUE
#endif // !USE_EPOLL
