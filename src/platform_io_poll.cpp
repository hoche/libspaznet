#ifndef USE_EPOLL
#ifndef USE_KQUEUE
#ifndef USE_IOCP

#include <libspaznet/platform_io.hpp>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <vector>
#include <unordered_map>

namespace spaznet {

class PlatformIOPoll : public PlatformIO {
private:
    std::vector<pollfd> pollfds_;
    std::unordered_map<int, std::pair<void*, uint32_t>> fd_info_;
    
public:
    PlatformIOPoll() = default;
    
    ~PlatformIOPoll() override {
        cleanup();
    }
    
    bool init() override {
        return true;
    }
    
    bool add_fd(int fd, uint32_t events, void* user_data) override {
        if (fd_info_.find(fd) != fd_info_.end()) {
            return false;  // Already exists
        }
        
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = 0;
        if (events & EVENT_READ) pfd.events |= POLLIN;
        if (events & EVENT_WRITE) pfd.events |= POLLOUT;
        pfd.revents = 0;
        
        pollfds_.push_back(pfd);
        fd_info_[fd] = {user_data, events};
        
        return true;
    }
    
    bool modify_fd(int fd, uint32_t events, void* user_data) override {
        auto it = fd_info_.find(fd);
        if (it == fd_info_.end()) {
            return add_fd(fd, events, user_data);
        }
        
        it->second = {user_data, events};
        
        for (auto& pfd : pollfds_) {
            if (pfd.fd == fd) {
                pfd.events = 0;
                if (events & EVENT_READ) pfd.events |= POLLIN;
                if (events & EVENT_WRITE) pfd.events |= POLLOUT;
                break;
            }
        }
        
        return true;
    }
    
    bool remove_fd(int fd) override {
        auto it = fd_info_.find(fd);
        if (it == fd_info_.end()) {
            return false;
        }
        
        fd_info_.erase(it);
        
        for (auto pfd_it = pollfds_.begin(); pfd_it != pollfds_.end(); ++pfd_it) {
            if (pfd_it->fd == fd) {
                pollfds_.erase(pfd_it);
                break;
            }
        }
        
        return true;
    }
    
    int wait(std::vector<Event>& events, int timeout_ms) override {
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
            
            Event ev{};
            ev.fd = pfd.fd;
            ev.events = 0;
            
            if (pfd.revents & POLLIN) ev.events |= EVENT_READ;
            if (pfd.revents & POLLOUT) ev.events |= EVENT_WRITE;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) ev.events |= EVENT_ERROR;
            
            auto it = fd_info_.find(pfd.fd);
            if (it != fd_info_.end()) {
                ev.user_data = it->second.first;
            } else {
                ev.user_data = nullptr;
            }
            
            events.push_back(ev);
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

