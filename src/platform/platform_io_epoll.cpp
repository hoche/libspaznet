#ifdef USE_EPOLL

#include <sys/epoll.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <libspaznet/platform/platform_io.hpp>

namespace spaznet {

class PlatformIOEpoll : public PlatformIO {
  private:
    int epoll_fd_{-1};
    static constexpr int MAX_EVENTS = 64;

  public:
    PlatformIOEpoll() = default;

    // Delete copy and move operations
    PlatformIOEpoll(const PlatformIOEpoll&) = delete;
    auto operator=(const PlatformIOEpoll&) -> PlatformIOEpoll& = delete;
    PlatformIOEpoll(PlatformIOEpoll&&) = delete;
    auto operator=(PlatformIOEpoll&&) -> PlatformIOEpoll& = delete;

    ~PlatformIOEpoll() override {
        cleanup();
    }

    auto init() -> bool override {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        return epoll_fd_ >= 0;
    }

    auto add_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        epoll_event event{};
        event.events = 0;
        if ((events & EVENT_READ) != 0U) {
            event.events |= EPOLLIN;
        }
        if ((events & EVENT_WRITE) != 0U) {
            event.events |= EPOLLOUT;
        }
        if ((events & EVENT_ERROR) != 0U) {
            event.events |= EPOLLERR | EPOLLHUP;
        }
        if ((events & EVENT_EDGE_TRIGGER) != 0U) {
            event.events |= EPOLLET;
        }
        // Round-trip the caller's token through epoll's data union. The
        // IOContext encodes (generation, fd) here and decodes it on
        // event delivery to filter out events from a previous
        // registration on a recycled fd.
        event.data.ptr = user_data;

        return epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, file_descriptor, &event) == 0;
    }

    auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        epoll_event event{};
        event.events = 0;
        if ((events & EVENT_READ) != 0U) {
            event.events |= EPOLLIN;
        }
        if ((events & EVENT_WRITE) != 0U) {
            event.events |= EPOLLOUT;
        }
        if ((events & EVENT_ERROR) != 0U) {
            event.events |= EPOLLERR | EPOLLHUP;
        }
        if ((events & EVENT_EDGE_TRIGGER) != 0U) {
            event.events |= EPOLLET;
        }
        event.data.ptr = user_data;

        return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, file_descriptor, &event) == 0;
    }

    auto remove_fd(int file_descriptor) -> bool override {
        epoll_event event{};
        return epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, file_descriptor, &event) == 0;
    }

    auto wait(std::vector<Event>& events, int timeout_ms) -> int override {
        std::array<epoll_event, MAX_EVENTS> epoll_events{};
        // EINTR is not a real error: a debugger attach, SIGWINCH, or any
        // benign signal can wake epoll_wait. Retry transparently so the
        // event loop survives. (Note: we do not adjust timeout_ms — the
        // worst case is one extra brief loop iteration.)
        int nfds;
        do {
            nfds = epoll_wait(epoll_fd_, epoll_events.data(), MAX_EVENTS, timeout_ms);
        } while (nfds < 0 && errno == EINTR);

        if (nfds < 0) {
            return -1;
        }

        events.clear();
        events.reserve(nfds);

        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
        for (int i = 0; i < nfds; ++i) {
            Event event{};
            event.user_data = epoll_events[i].data.ptr;
            // The caller (IOContext) encodes its fd in the low 32 bits
            // of user_data. We expose evt.fd as a convenience for any
            // consumer that needs it without decoding the token. A
            // null user_data (wakeup pipe registration) gives fd 0,
            // which is harmless because the IOContext keys off
            // user_data == nullptr, not the fd value.
            const auto packed = reinterpret_cast<uintptr_t>(event.user_data);
            event.fd = static_cast<int>(static_cast<int32_t>(packed & 0xFFFFFFFFU));
            event.events = 0;

            if ((epoll_events[i].events & EPOLLIN) != 0U) {
                event.events |= EVENT_READ;
            }
            if ((epoll_events[i].events & EPOLLOUT) != 0U) {
                event.events |= EVENT_WRITE;
            }
            if ((epoll_events[i].events & (EPOLLERR | EPOLLHUP)) != 0U) {
                // EPOLLHUP without EPOLLIN means the peer closed cleanly;
                // any read-waiter must be woken so recv() can return 0
                // (orderly EOF). Without this, a reader registered for
                // EVENT_READ would sleep forever on a half-closed
                // connection.
                event.events |= EVENT_READ | EVENT_ERROR;
            }

            events.push_back(event);
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

        return nfds;
    }

    void cleanup() override {
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
    }
};

} // namespace spaznet

#endif // USE_EPOLL
