#include <libspaznet/server.hpp>
#include <libspaznet/io_context.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <sstream>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define close_socket ::close
#endif

namespace spaznet {

// Socket implementation
Task Socket::async_read(std::vector<uint8_t>& buffer, std::size_t size) {
    buffer.resize(size);
    
    struct ReadAwaiter {
        Socket* socket;
        std::vector<uint8_t>* buffer;
        std::size_t size;
        mutable ssize_t result = 0;
        mutable bool ready = false;
        
        bool await_ready() const noexcept { 
            // Try to read immediately
            result = recv(socket->fd(), buffer->data(), size, 0);
            if (result >= 0) {
                ready = true;
                return true;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ready = false;
                return false;
            }
            ready = true;
            return true;
        }
        
        void await_suspend(std::coroutine_handle<> h) {
            // Register for read event
            socket->context()->register_io(socket->fd(), PlatformIO::EVENT_READ, h);
        }
        
        ssize_t await_resume() noexcept {
            if (!ready) {
                // Try reading again
                result = recv(socket->fd(), buffer->data(), size, 0);
            }
            if (result > 0) {
                buffer->resize(result);
            } else if (result < 0) {
                buffer->clear();
            }
            return result;
        }
    };
    
    ReadAwaiter awaiter{this, &buffer, size};
    ssize_t result = co_await awaiter;
    
    if (result < 0) {
        buffer.clear();
    }
}

Task Socket::async_write(const std::vector<uint8_t>& data) {
    struct WriteAwaiter {
        Socket* socket;
        const std::vector<uint8_t>* data;
        mutable std::size_t offset = 0;
        mutable ssize_t result = 0;
        mutable bool ready = false;
        
        bool await_ready() const noexcept { 
            // Try to write immediately
            result = send(socket->fd(), data->data() + offset, data->size() - offset, 0);
            if (result >= 0) {
                offset += result;
                if (offset >= data->size()) {
                    ready = true;
                    return true;
                }
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ready = false;
                return false;
            }
            ready = true;
            return true;
        }
        
        void await_suspend(std::coroutine_handle<> h) {
            // Register for write event
            socket->context()->register_io(socket->fd(), PlatformIO::EVENT_WRITE, h);
        }
        
        ssize_t await_resume() noexcept {
            if (!ready) {
                // Try writing again
                result = send(socket->fd(), data->data() + offset, data->size() - offset, 0);
                if (result > 0) {
                    offset += result;
                }
            }
            return result;
        }
    };
    
    WriteAwaiter awaiter{this, &data};
    co_await awaiter;
}

void Socket::close() {
    if (fd_ >= 0) {
        io_context_->platform_io().remove_fd(fd_);
        close_socket(fd_);
        fd_ = -1;
    }
}


// Server implementation
Server::Server(std::size_t num_threads)
    : io_context_(std::make_unique<IOContext>(num_threads))
{
}

Server::~Server() {
    stop();
}

void Server::set_udp_handler(std::unique_ptr<UDPHandler> handler) {
    udp_handler_ = std::move(handler);
}

void Server::set_http_handler(std::unique_ptr<HTTPHandler> handler) {
    http_handler_ = std::move(handler);
}

void Server::set_http2_handler(std::unique_ptr<HTTP2Handler> handler) {
    http2_handler_ = std::move(handler);
}

void Server::set_websocket_handler(std::unique_ptr<WebSocketHandler> handler) {
    websocket_handler_ = std::move(handler);
}

namespace {
    Task listen_tcp_impl(IOContext* ctx, uint16_t port, Server* server) {
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            throw std::runtime_error("Failed to create socket");
        }
        
        // Set socket options
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // Set non-blocking
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(listen_fd, FIONBIO, &mode);
#else
        int flags = fcntl(listen_fd, F_GETFL, 0);
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
#endif
        
        // Bind
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(listen_fd);
            throw std::runtime_error("Failed to bind socket");
        }
        
        // Listen
        if (listen(listen_fd, SOMAXCONN) < 0) {
            close_socket(listen_fd);
            throw std::runtime_error("Failed to listen on socket");
        }
        
        // Register for accept events
        ctx->platform_io().add_fd(
            listen_fd,
            PlatformIO::EVENT_READ,
            server
        );
        
        // Start accepting connections
        ctx->schedule(server->accept_connections(listen_fd));
        
        co_return;
    }
}

void Server::listen_tcp(uint16_t port) {
    io_context_->schedule(listen_tcp_impl(io_context_.get(), port, this));
}

namespace {
    Task listen_udp_impl(IOContext* ctx, uint16_t port, Server* server) {
        int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_fd < 0) {
            throw std::runtime_error("Failed to create UDP socket");
        }
        
        // Set non-blocking
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(udp_fd, FIONBIO, &mode);
#else
        int flags = fcntl(udp_fd, F_GETFL, 0);
        fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK);
#endif
        
        // Bind
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        if (bind(udp_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(udp_fd);
            throw std::runtime_error("Failed to bind UDP socket");
        }
        
        // Register for read events
        ctx->platform_io().add_fd(
            udp_fd,
            PlatformIO::EVENT_READ,
            server
        );
        
        co_return;
    }
}

void Server::listen_udp(uint16_t port) {
    io_context_->schedule(listen_udp_impl(io_context_.get(), port, this));
}

Task Server::accept_connections(int listen_fd) {
    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No connection available, wait for event
                co_await std::suspend_always{};
                continue;
            }
            break;
        }
        
        // Set non-blocking
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(client_fd, FIONBIO, &mode);
#else
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
#endif
        
        // Create socket and handle connection
        Socket socket(client_fd, io_context_.get());
        io_context_->schedule(handle_connection(std::move(socket)));
    }
}

Task Server::handle_connection(Socket socket) {
    if (!http_handler_ && !http2_handler_ && !websocket_handler_) {
        socket.close();
        co_return;
    }
    
    std::vector<uint8_t> buffer;
    co_await socket.async_read(buffer, 4096);
    
    if (buffer.empty()) {
        socket.close();
        co_return;
    }
    
    // Try to parse as HTTP
    std::string request_str(buffer.begin(), buffer.end());
    std::istringstream iss(request_str);
    std::string line;
    
    if (std::getline(iss, line)) {
        if (line.find("HTTP/") != std::string::npos) {
            // HTTP request
            if (http_handler_) {
                HTTPRequest request;
                HTTPResponse response;
                
                // Simple parsing (in production, use a proper parser)
                std::istringstream line_stream(line);
                line_stream >> request.method >> request.path >> request.version;
                
                // Parse headers
                while (std::getline(iss, line) && line != "\r" && !line.empty()) {
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string key = line.substr(0, colon);
                        std::string value = line.substr(colon + 1);
                        // Trim whitespace
                        while (!value.empty() && value[0] == ' ') {
                            value.erase(0, 1);
                        }
                        request.headers[key] = value;
                    }
                }
                
                co_await http_handler_->handle_request(request, response, socket);
                
                auto response_data = response.serialize();
                co_await socket.async_write(response_data);
            }
        } else if (line.find("GET") == 0 || line.find("POST") == 0) {
            // WebSocket upgrade request
            if (websocket_handler_) {
                // Handle WebSocket upgrade
                co_await websocket_handler_->on_open(socket);
                
                // Read WebSocket frames
                while (true) {
                    std::vector<uint8_t> frame_data;
                    co_await socket.async_read(frame_data, 4096);
                    
                    if (frame_data.empty()) {
                        break;
                    }
                    
                    try {
                        auto frame = WebSocketFrame::parse(frame_data);
                        WebSocketMessage msg;
                        msg.opcode = frame.opcode;
                        msg.data = frame.payload;
                        
                        co_await websocket_handler_->handle_message(msg, socket);
                        
                        if (frame.opcode == WebSocketOpcode::Close) {
                            break;
                        }
                    } catch (...) {
                        break;
                    }
                }
                
                co_await websocket_handler_->on_close(socket);
            }
        }
    }
    
    socket.close();
}

void Server::run() {
    io_context_->run();
}

void Server::stop() {
    io_context_->stop();
}

} // namespace spaznet

