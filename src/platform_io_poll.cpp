#ifndef USE_EPOLL
#ifndef USE_KQUEUE
#ifndef USE_IOCP

#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <libspaznet/platform_io.hpp>
#include <unordered_map>
#include <vector>

namespace spaznet {

class PlatformIOPoll : public PlatformIO {
  private:
    std::vector<pollfd> pollfds_;
    std::unordered_map<int, std::pair<void*, uint32_t>> fd_info_;

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

    auto add_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        if (fd_info_.find(file_descriptor) != fd_info_.end()) {
            return false; // Already exists
        }

        pollfd pfd{};
        pfd.fd = file_descriptor;
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

    auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        auto it = fd_info_.find(file_descriptor);
        if (it == fd_info_.end()) {
            return add_fd(file_descriptor, events, user_data);
        }

        it->second = {user_data, events};

        for (auto& pfd : pollfds_) {
            if (pfd.fd == file_descriptor) {
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
        auto it = fd_info_.find(file_descriptor);
        if (it == fd_info_.end()) {
            return false;
        }

        fd_info_.erase(it);

        for (auto pfd_it = pollfds_.begin(); pfd_it != pollfds_.end(); ++pfd_it) {
            if (pfd_it->fd == file_descriptor) {
                pollfds_.erase(pfd_it);
                break;
            }
        }

        return true;
    }

    auto wait(std::vector<Event>& events, int timeout_ms) -> int override {
        if (pollfds_.empty()) {
            return 0;
        }

        int nfds = poll(pollfds_.data(), pollfds_.size(), timeout_ms);

        if (nfds < 0) {
            return -1;
        }

        events.clear();
        events.reserve(nfds);

        for (const auto& pfd : pollfds_) {
            if (pfd.revents == 0) {
                continue;
            }

            Event event{};
            event.fd = pfd.fd;
            event.events = 0;

            if ((pfd.revents & POLLIN) != 0U) {
                event.events |= EVENT_READ;
            }
            if ((pfd.revents & POLLOUT) != 0U) {
                event.events |= EVENT_WRITE;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0U) {
                event.events |= EVENT_ERROR;
            }

            auto it = fd_info_.find(pfd.fd);
            if (it != fd_info_.end()) {
                event.user_data = it->second.first;
            } else {
                event.user_data = nullptr;
            }

            events.push_back(event);
        }

        return nfds;
    }

    void cleanup() override {
        pollfds_.clear();
        fd_info_.clear();
    }
};

} // namespace spaznet

#endif // !USE_IOCP
#endif // !USE_KQUEUE
#endif // !USE_EPOLL
