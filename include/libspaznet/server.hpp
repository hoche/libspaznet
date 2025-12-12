#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <libspaznet/io_context.hpp>
#include <libspaznet/platform_io.hpp>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace spaznet {

// Forward declarations
class UDPHandler;
class HTTPHandler;
class HTTP2Handler;
class WebSocketHandler;
class QUICHandler;
class HTTP3Handler;

// Socket wrapper
class Socket {
  private:
    int fd_;
    IOContext* io_context_;
    bool owns_fd_;

  public:
    Socket(int fd, IOContext* ctx) : fd_(fd), io_context_(ctx), owns_fd_(true) {}

    // Move constructor
    Socket(Socket&& other) noexcept
        : fd_(other.fd_), io_context_(other.io_context_), owns_fd_(other.owns_fd_) {
        other.owns_fd_ = false;
    }

    // Move assignment
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (owns_fd_) {
                close();
            }
            fd_ = other.fd_;
            io_context_ = other.io_context_;
            owns_fd_ = other.owns_fd_;
            other.owns_fd_ = false;
        }
        return *this;
    }

    // Delete copy
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Destructor
    ~Socket() {
        if (owns_fd_) {
            close();
        }
    }

    int fd() const {
        return fd_;
    }
    IOContext* context() const {
        return io_context_;
    }

    // Async read
    Task async_read(std::vector<uint8_t>& buffer, std::size_t size);

    // Async write
    Task async_write(const std::vector<uint8_t>& data);

    void close();
};

} // namespace spaznet

// Include handlers after Socket is defined
#include <libspaznet/handlers/http2_handler.hpp>
#include <libspaznet/handlers/http3_handler.hpp>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/handlers/quic_handler.hpp>
#include <libspaznet/handlers/udp_handler.hpp>
#include <libspaznet/handlers/websocket_handler.hpp>

namespace spaznet {

// Server class
class Server {
  private:
    std::unique_ptr<IOContext> io_context_;
    std::unique_ptr<UDPHandler> udp_handler_;
    std::unique_ptr<HTTPHandler> http_handler_;
    std::unique_ptr<HTTP2Handler> http2_handler_;
    std::unique_ptr<WebSocketHandler> websocket_handler_;
    std::unique_ptr<QUICHandler> quic_handler_;
    std::unique_ptr<HTTP3Handler> http3_handler_;

    std::unordered_map<int, std::coroutine_handle<>> socket_handles_;
    std::vector<std::thread> accept_threads_;
    std::atomic<bool> running_;

    Task handle_connection(Socket socket);
    void accept_connections(int listen_fd); // Changed from Task to void

  public:
  public:
    Server(std::size_t num_threads = std::thread::hardware_concurrency());
    ~Server();

    // Start listening on a port (schedules the listen task)
    void listen_tcp(uint16_t port);
    void listen_udp(uint16_t port);

    // Register handlers
    void set_udp_handler(std::unique_ptr<UDPHandler> handler);
    void set_http_handler(std::unique_ptr<HTTPHandler> handler);
    void set_http2_handler(std::unique_ptr<HTTP2Handler> handler);
    void set_websocket_handler(std::unique_ptr<WebSocketHandler> handler);
    void set_quic_handler(std::unique_ptr<QUICHandler> handler);
    void set_http3_handler(std::unique_ptr<HTTP3Handler> handler);

    // Run the server
    void run();

    // Stop the server
    void stop();
};

} // namespace spaznet
