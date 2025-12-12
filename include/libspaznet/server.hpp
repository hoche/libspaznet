#pragma once

#include <libspaznet/io_context.hpp>
#include <libspaznet/platform_io.hpp>
#include <memory>
#include <functional>
#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <thread>
#include <coroutine>

namespace spaznet {

// Forward declarations
class UDPHandler;
class HTTPHandler;
class HTTP2Handler;
class WebSocketHandler;

// Socket wrapper
class Socket {
private:
    int fd_;
    IOContext* io_context_;
    
public:
    Socket(int fd, IOContext* ctx) : fd_(fd), io_context_(ctx) {}
    
    int fd() const { return fd_; }
    IOContext* context() const { return io_context_; }
    
    // Async read
    Task async_read(std::vector<uint8_t>& buffer, std::size_t size);
    
    // Async write
    Task async_write(const std::vector<uint8_t>& data);
    
    void close();
};

} // namespace spaznet

// Include handlers after Socket is defined
#include <libspaznet/udp_handler.hpp>
#include <libspaznet/http_handler.hpp>
#include <libspaznet/http2_handler.hpp>
#include <libspaznet/websocket_handler.hpp>

namespace spaznet {

// Server class
class Server {
private:
    std::unique_ptr<IOContext> io_context_;
    std::unique_ptr<UDPHandler> udp_handler_;
    std::unique_ptr<HTTPHandler> http_handler_;
    std::unique_ptr<HTTP2Handler> http2_handler_;
    std::unique_ptr<WebSocketHandler> websocket_handler_;
    
    std::unordered_map<int, std::coroutine_handle<>> socket_handles_;
    
    Task handle_connection(Socket socket);
    
public:
    Task accept_connections(int listen_fd);
    
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
    
    // Run the server
    void run();
    
    // Stop the server
    void stop();
};

} // namespace spaznet

