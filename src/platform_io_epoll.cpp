#ifdef USE_EPOLL

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>
#include <libspaznet/platform_io.hpp>

namespace spaznet {

class PlatformIOEpoll : public PlatformIO {
  private:
    int epoll_fd_;
    static constexpr int MAX_EVENTS = 64;

  public:
    PlatformIOEpoll() : epoll_fd_(-1) {}

    ~PlatformIOEpoll() override {
        cleanup();
    }

    bool init() override {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        return epoll_fd_ >= 0;
    }

    bool add_fd(int fd, uint32_t events, void* user_data) override {
        epoll_event ev{};
        ev.events = 0;
        if (events & EVENT_READ)
            ev.events |= EPOLLIN;
        if (events & EVENT_WRITE)
            ev.events |= EPOLLOUT;
        if (events & EVENT_ERROR)
            ev.events |= EPOLLERR | EPOLLHUP;
        if (events & EVENT_EDGE_TRIGGER)
            ev.events |= EPOLLET;
        ev.data.ptr = user_data;

        return epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    bool modify_fd(int fd, uint32_t events, void* user_data) override {
        epoll_event ev{};
        ev.events = 0;
        if (events & EVENT_READ)
            ev.events |= EPOLLIN;
        if (events & EVENT_WRITE)
            ev.events |= EPOLLOUT;
        if (events & EVENT_ERROR)
            ev.events |= EPOLLERR | EPOLLHUP;
        if (events & EVENT_EDGE_TRIGGER)
            ev.events |= EPOLLET;
        ev.data.ptr = user_data;

        return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    bool remove_fd(int fd) override {
        epoll_event ev{};
        return epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev) == 0;
    }

    int wait(std::vector<Event>& events, int timeout_ms) override {
        epoll_event epoll_events[MAX_EVENTS];
        int nfds = epoll_wait(epoll_fd_, epoll_events, MAX_EVENTS, timeout_ms);

        if (nfds < 0) {
            return -1;
        }

        events.clear();
        events.reserve(nfds);

        for (int i = 0; i < nfds; ++i) {
            Event ev{};
            ev.fd = -1; // We use user_data to identify the socket
            ev.user_data = epoll_events[i].data.ptr;
            ev.events = 0;

            if (epoll_events[i].events & EPOLLIN)
                ev.events |= EVENT_READ;
            if (epoll_events[i].events & EPOLLOUT)
                ev.events |= EVENT_WRITE;
            if (epoll_events[i].events & (EPOLLERR | EPOLLHUP))
                ev.events |= EVENT_ERROR;

            events.push_back(ev);
        }

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
