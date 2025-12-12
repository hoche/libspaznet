#ifdef USE_KQUEUE

#include <errno.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstring>
#include <libspaznet/platform_io.hpp>
#include <unordered_map>

namespace spaznet {

class PlatformIOKqueue : public PlatformIO {
  private:
    int kqueue_fd_;
    std::unordered_map<int, void*> fd_to_user_data_;
    static constexpr int MAX_EVENTS = 64;

  public:
    PlatformIOKqueue() : kqueue_fd_(-1) {}

    ~PlatformIOKqueue() override {
        cleanup();
    }

    bool init() override {
        kqueue_fd_ = kqueue();
        return kqueue_fd_ >= 0;
    }

    bool add_fd(int fd, uint32_t events, void* user_data) override {
        struct kevent changes[2];
        int nchanges = 0;

        if (events & EVENT_READ) {
            EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, user_data);
            nchanges++;
        }

        if (events & EVENT_WRITE) {
            EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, user_data);
            nchanges++;
        }

        if (nchanges == 0) {
            return false;
        }

        fd_to_user_data_[fd] = user_data;

        return kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) == 0;
    }

    bool modify_fd(int fd, uint32_t events, void* user_data) override {
        // Remove existing filters
        struct kevent changes[2];
        int nchanges = 0;

        if (fd_to_user_data_.find(fd) != fd_to_user_data_.end()) {
            EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            nchanges++;
            EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            nchanges++;
            kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr);
        }

        return add_fd(fd, events, user_data);
    }

    bool remove_fd(int fd) override {
        struct kevent changes[2];
        int nchanges = 0;

        EV_SET(&changes[nchanges], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        nchanges++;
        EV_SET(&changes[nchanges], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        nchanges++;

        fd_to_user_data_.erase(fd);

        return kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) == 0;
    }

    int wait(std::vector<Event>& events, int timeout_ms) override {
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
            Event ev{};
            ev.fd = static_cast<int>(kevents[i].ident);
            ev.user_data = kevents[i].udata;
            ev.events = 0;

            if (kevents[i].filter == EVFILT_READ) {
                ev.events |= EVENT_READ;
            } else if (kevents[i].filter == EVFILT_WRITE) {
                ev.events |= EVENT_WRITE;
            }

            if (kevents[i].flags & EV_EOF) {
                ev.events |= EVENT_ERROR;
            }

            events.push_back(ev);
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
