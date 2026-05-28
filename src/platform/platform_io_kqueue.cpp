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
    // Per-fd record: which filters are currently registered (so
    // remove_fd / modify_fd only issue EV_DELETE for filters we
    // actually have — kqueue returns -1/ENOENT otherwise, which the
    // test suite previously caught as a spurious remove_fd failure on
    // macOS), and the user_data token to re-attach on modify.
    struct FdRecord {
        void* user_data;
        uint32_t events; // EVENT_READ / EVENT_WRITE bits currently active
    };
    std::unordered_map<int, FdRecord> fd_records_;
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

        if (kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) != 0) {
            return false;
        }
        fd_records_[file_descriptor] = {user_data, events & (EVENT_READ | EVENT_WRITE)};
        return true;
    }

    auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        auto it = fd_records_.find(file_descriptor);
        if (it == fd_records_.end()) {
            return add_fd(file_descriptor, events, user_data);
        }

        struct kevent changes[4];
        int nchanges = 0;
        const uint32_t old_events = it->second.events;
        const uint32_t new_events = events & (EVENT_READ | EVENT_WRITE);

        // Drop filters that are no longer wanted; add/refresh the ones
        // that are. EV_ADD acts as "add or replace" so we don't need a
        // separate EV_DELETE pass for filters that are staying.
        if ((old_events & EVENT_READ) != 0U && (new_events & EVENT_READ) == 0U) {
            EV_SET(&changes[nchanges++], file_descriptor, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        }
        if ((old_events & EVENT_WRITE) != 0U && (new_events & EVENT_WRITE) == 0U) {
            EV_SET(&changes[nchanges++], file_descriptor, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        }
        if ((new_events & EVENT_READ) != 0U) {
            EV_SET(&changes[nchanges++], file_descriptor, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0,
                   user_data);
        }
        if ((new_events & EVENT_WRITE) != 0U) {
            EV_SET(&changes[nchanges++], file_descriptor, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0,
                   user_data);
        }

        if (nchanges == 0) {
            it->second.user_data = user_data;
            it->second.events = new_events;
            return true;
        }
        if (kevent(kqueue_fd_, changes, nchanges, nullptr, 0, nullptr) != 0) {
            return false;
        }
        it->second.user_data = user_data;
        it->second.events = new_events;
        return true;
    }

    auto remove_fd(int file_descriptor) -> bool override {
        auto it = fd_records_.find(file_descriptor);
        if (it == fd_records_.end()) {
            return false; // never registered
        }
        struct kevent changes[2];
        int nchanges = 0;
        if ((it->second.events & EVENT_READ) != 0U) {
            EV_SET(&changes[nchanges++], file_descriptor, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        }
        if ((it->second.events & EVENT_WRITE) != 0U) {
            EV_SET(&changes[nchanges++], file_descriptor, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        }
        fd_records_.erase(it);
        if (nchanges == 0) {
            return true; // nothing to actually delete
        }
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

        // EINTR is benign — debugger attach, SIGWINCH, etc. — and must
        // not kill the event loop. Retry transparently. The kqueue
        // timeout is reset each call; the cost of EINTR is at most one
        // extra spurious wait with the original timeout.
        int nfds;
        do {
            nfds = kevent(kqueue_fd_, nullptr, 0, kevents, MAX_EVENTS, timeout_ptr);
        } while (nfds < 0 && errno == EINTR);

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
                // EV_EOF fires on half-close. Map it to EVENT_READ as
                // well as EVENT_ERROR so a coroutine suspended on
                // async_read wakes and recv() can return 0. Without
                // this, the reader sleeps forever after the peer
                // closes its write side.
                event.events |= EVENT_READ | EVENT_ERROR;
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
        fd_records_.clear();
    }
};

} // namespace spaznet

#endif // USE_KQUEUE
