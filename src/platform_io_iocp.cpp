#ifdef USE_IOCP

#include <libspaznet/platform_io.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace spaznet {

class PlatformIOIOCP : public PlatformIO {
private:
    HANDLE iocp_handle_;
    std::unordered_map<HANDLE, void*> handle_to_user_data_;
    
    struct OverlappedContext {
        OVERLAPPED overlapped;
        void* user_data;
        int fd;
        uint32_t events;
    };
    
public:
    PlatformIOIOCP() : iocp_handle_(INVALID_HANDLE_VALUE) {}
    
    ~PlatformIOIOCP() override {
        cleanup();
    }
    
    bool init() override {
        iocp_handle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        return iocp_handle_ != INVALID_HANDLE_VALUE;
    }
    
    bool add_fd(int fd, uint32_t events, void* user_data) override {
        HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
        
        // Associate socket with IOCP
        if (CreateIoCompletionPort(handle, iocp_handle_, reinterpret_cast<ULONG_PTR>(user_data), 0) == nullptr) {
            return false;
        }
        
        handle_to_user_data_[handle] = user_data;
        
        // Start async operations
        if (events & EVENT_READ) {
            // Post a read operation
            WSABUF buf{};
            DWORD flags = 0;
            OverlappedContext* ctx = new OverlappedContext{};
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->user_data = user_data;
            ctx->fd = fd;
            ctx->events = EVENT_READ;
            
            DWORD bytes_received;
            if (WSARecv(fd, &buf, 1, &bytes_received, &flags, &ctx->overlapped, nullptr) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    delete ctx;
                    return false;
                }
            }
        }
        
        return true;
    }
    
    bool modify_fd(int fd, uint32_t events, void* user_data) override {
        HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
        handle_to_user_data_[handle] = user_data;
        
        // For IOCP, we need to post new operations
        if (events & EVENT_READ) {
            WSABUF buf{};
            DWORD flags = 0;
            OverlappedContext* ctx = new OverlappedContext{};
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->user_data = user_data;
            ctx->fd = fd;
            ctx->events = EVENT_READ;
            
            DWORD bytes_received;
            WSARecv(fd, &buf, 1, &bytes_received, &flags, &ctx->overlapped, nullptr);
        }
        
        return true;
    }
    
    bool remove_fd(int fd) override {
        HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
        handle_to_user_data_.erase(handle);
        return true;
    }
    
    int wait(std::vector<Event>& events, int timeout_ms) override {
        DWORD bytes_transferred;
        ULONG_PTR completion_key;
        OVERLAPPED* overlapped;
        
        BOOL result = GetQueuedCompletionStatus(
            iocp_handle_,
            &bytes_transferred,
            &completion_key,
            &overlapped,
            timeout_ms >= 0 ? timeout_ms : INFINITE
        );
        
        if (!result) {
            if (overlapped == nullptr) {
                // Timeout
                return 0;
            }
            return -1;
        }
        
        if (overlapped == nullptr) {
            return 0;
        }
        
        OverlappedContext* ctx = reinterpret_cast<OverlappedContext*>(overlapped);
        
        Event ev{};
        ev.fd = ctx->fd;
        ev.user_data = ctx->user_data;
        ev.events = ctx->events;
        
        if (bytes_transferred == 0 && ctx->events & EVENT_READ) {
            ev.events |= EVENT_ERROR;  // Connection closed
        }
        
        events.clear();
        events.push_back(ev);
        
        delete ctx;
        
        return 1;
    }
    
    void cleanup() override {
        if (iocp_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(iocp_handle_);
            iocp_handle_ = INVALID_HANDLE_VALUE;
        }
        handle_to_user_data_.clear();
    }
};

} // namespace spaznet

#endif // USE_IOCP

