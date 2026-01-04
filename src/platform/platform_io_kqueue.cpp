#ifdef USE_KQUEUE

#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <libspaznet/platform/platform_io.hpp>
#include <unordered_map>

namespace spaznet {

class PlatformIOKqueue : public PlatformIO {
  private:
    int kqueue_fd_;
    std::unordered_map<int, void*> fd_to_user_data_;
    static constexpr int MAX_EVENTS = 64;

  public:
    PlatformIOKqueue() : kqueue_fd_(-1) {}

    // Delete copy and move operations
    PlatformIOKqueue(const PlatformIOKqueue&) = delete;
    auto operator=(const PlatformIOKqueue&) -> PlatformIOKqueue& = delete;
    PlatformIOKqueue(PlatformIOKqueue&&) = delete;
    auto operator=(PlatformIOKqueue&&) -> PlatformIOKqueue& = delete;

    ~PlatformIOKqueue() override {
        cleanup();
    }

    auto init() -> bool override {
        kqueue_fd_ = kqueue();
        return kqueue_fd_ >= 0;
    }

    auto add_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        struct kevent changes[2];
        int nchanges = 0;

        if ((events & EVENT_READ) != 0U) {
            EV_SET(&changes[nchanges], file_descriptor, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
                   user_data);
            nchanges++;
        }

        if ((events & EVENT_WRITE) != 0U) {
            EV_SET(&changes[nchanges], file_descriptor, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0,
                   user_data);
            nchanges++;
        }

        if (nchanges == 0) {
            return false;
        }

        fd_to_user_data_[file_descriptor] = user_data;

        return kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) == 0;
    }

    auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        // Remove existing filters
        struct kevent changes[2];
        int nchanges = 0;

        if (fd_to_user_data_.find(file_descriptor) != fd_to_user_data_.end()) {
            EV_SET(&changes[nchanges], file_descriptor, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            nchanges++;
            EV_SET(&changes[nchanges], file_descriptor, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            nchanges++;
            kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr);
        }

        return add_fd(file_descriptor, events, user_data);
    }

    auto remove_fd(int file_descriptor) -> bool override {
        struct kevent changes[2];
        int nchanges = 0;

        EV_SET(&changes[nchanges], file_descriptor, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        nchanges++;
        EV_SET(&changes[nchanges], file_descriptor, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        nchanges++;

        fd_to_user_data_.erase(file_descriptor);

        return kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) == 0;
    }

    auto wait(std::vector<Event>& events, int timeout_ms) -> int override {
        struct kevent kevents[MAX_EVENTS];
        struct timespec timeout;
        struct timespec* timeout_ptr = nullptr;

        if (timeout_ms >= 0) {
            timeout.tv_sec = timeout_ms / 1000;
            timeout.tv_nsec = (timeout_ms % 1000) * 1000000;
            timeout_ptr = &timeout;
        }

        int nfds = kevent(kqueue_fd_, nullptr, 0, kevents, MAX_EVENTS, timeout_ptr);

        if (nfds < 0) {
            return -1;
        }

        events.clear();
        events.reserve(nfds);

        for (int i = 0; i < nfds; ++i) {
            Event event{};
            event.fd = static_cast<int>(kevents[i].ident);
            event.user_data = kevents[i].udata;
            event.events = 0;

            if (kevents[i].filter == EVFILT_READ) {
                event.events |= EVENT_READ;
            } else if (kevents[i].filter == EVFILT_WRITE) {
                event.events |= EVENT_WRITE;
            }

            if ((kevents[i].flags & EV_EOF) != 0U) {
                event.events |= EVENT_ERROR;
            }

            events.push_back(event);
        }

        return nfds;
    }

    void cleanup() override {
        if (kqueue_fd_ >= 0) {
            close(kqueue_fd_);
            kqueue_fd_ = -1;
        }
        fd_to_user_data_.clear();
    }
};

} // namespace spaznet

#endif // USE_KQUEUE
