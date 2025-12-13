#ifdef USE_EPOLL

#include <sys/epoll.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <libspaznet/platform_io.hpp>

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
        (void)user_data; // unused for epoll implementation
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
        // Store the file descriptor so IOContext can map the event back to PendingIO.
        event.data.fd = file_descriptor;

        return epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, file_descriptor, &event) == 0;
    }

    auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        (void)user_data; // unused for epoll implementation
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
        // Keep fd in the event payload (IOContext does fd-based lookup).
        event.data.fd = file_descriptor;

        return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, file_descriptor, &event) == 0;
    }

    auto remove_fd(int file_descriptor) -> bool override {
        epoll_event event{};
        return epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, file_descriptor, &event) == 0;
    }

    auto wait(std::vector<Event>& events, int timeout_ms) -> int override {
        std::array<epoll_event, MAX_EVENTS> epoll_events{};
        int nfds = epoll_wait(epoll_fd_, epoll_events.data(), MAX_EVENTS, timeout_ms);

        if (nfds < 0) {
            return -1;
        }

        events.clear();
        events.reserve(nfds);

        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
        for (int i = 0; i < nfds; ++i) {
            Event event{};
            event.fd = epoll_events[i].data.fd;
            event.user_data = nullptr;
            event.events = 0;

            if ((epoll_events[i].events & EPOLLIN) != 0U) {
                event.events |= EVENT_READ;
            }
            if ((epoll_events[i].events & EPOLLOUT) != 0U) {
                event.events |= EVENT_WRITE;
            }
            if ((epoll_events[i].events & (EPOLLERR | EPOLLHUP)) != 0U) {
                event.events |= EVENT_ERROR;
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
