#ifdef USE_IOCP

#include <mswsock.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <libspaznet/platform_io.hpp>
#include <unordered_map>
#include <vector>

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

    // Delete copy and move operations
    PlatformIOIOCP(const PlatformIOIOCP&) = delete;
    auto operator=(const PlatformIOIOCP&) -> PlatformIOIOCP& = delete;
    PlatformIOIOCP(PlatformIOIOCP&&) = delete;
    auto operator=(PlatformIOIOCP&&) -> PlatformIOIOCP& = delete;

    ~PlatformIOIOCP() override {
        cleanup();
    }

    auto init() -> bool override {
        iocp_handle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        return iocp_handle_ != INVALID_HANDLE_VALUE;
    }

    auto add_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(file_descriptor));

        // Associate socket with IOCP
        if (CreateIoCompletionPort(handle, iocp_handle_, reinterpret_cast<ULONG_PTR>(user_data),
                                   0) == nullptr) {
            return false;
        }

        handle_to_user_data_[handle] = user_data;

        // Start async operations
        if ((events & EVENT_READ) != 0U) {
            // Post a read operation
            WSABUF buf{};
            DWORD flags = 0;
            OverlappedContext* ctx = new OverlappedContext{};
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->user_data = user_data;
            ctx->fd = file_descriptor;
            ctx->events = EVENT_READ;

            DWORD bytes_received;
            if (WSARecv(file_descriptor, &buf, 1, &bytes_received, &flags, &ctx->overlapped,
                        nullptr) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    delete ctx;
                    return false;
                }
            }
        }

        return true;
    }

    auto modify_fd(int file_descriptor, uint32_t events, void* user_data) -> bool override {
        HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(file_descriptor));
        handle_to_user_data_[handle] = user_data;

        // For IOCP, we need to post new operations
        if ((events & EVENT_READ) != 0U) {
            WSABUF buf{};
            DWORD flags = 0;
            OverlappedContext* ctx = new OverlappedContext{};
            ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
            ctx->user_data = user_data;
            ctx->fd = file_descriptor;
            ctx->events = EVENT_READ;

            DWORD bytes_received;
            WSARecv(file_descriptor, &buf, 1, &bytes_received, &flags, &ctx->overlapped, nullptr);
        }

        return true;
    }

    auto remove_fd(int file_descriptor) -> bool override {
        HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(file_descriptor));
        handle_to_user_data_.erase(handle);
        return true;
    }

    auto wait(std::vector<Event>& events, int timeout_ms) -> int override {
        DWORD bytes_transferred;
        ULONG_PTR completion_key;
        OVERLAPPED* overlapped;

        BOOL result =
            GetQueuedCompletionStatus(iocp_handle_, &bytes_transferred, &completion_key,
                                      &overlapped, timeout_ms >= 0 ? timeout_ms : INFINITE);

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

        Event event{};
        event.fd = ctx->fd;
        event.user_data = ctx->user_data;
        event.events = ctx->events;

        if (bytes_transferred == 0 && (ctx->events & EVENT_READ) != 0u) {
            event.events |= EVENT_ERROR; // Connection closed
        }

        events.clear();
        events.push_back(event);

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
