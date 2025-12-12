#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <libspaznet/io_context.hpp>
#include <libspaznet/server.hpp>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
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
    std::size_t total_sent = 0;

    while (total_sent < data.size()) {
        struct WriteAwaiter {
            Socket* socket;
            const uint8_t* data_ptr;
            std::size_t remaining;
            mutable ssize_t result = 0;
            mutable bool ready = false;

            bool await_ready() const noexcept {
                // Try to write immediately
                result = send(socket->fd(), data_ptr, remaining, MSG_NOSIGNAL);
                if (result >= 0) {
                    ready = true;
                    return true; // Wrote some data
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ready = false;
                    return false; // Would block, need to suspend
                }
                // Error occurred
                ready = true;
                return true;
            }

            void await_suspend(std::coroutine_handle<> h) {
                // Register for write event
                socket->context()->register_io(socket->fd(), PlatformIO::EVENT_WRITE, h);
            }

            ssize_t await_resume() noexcept {
                if (!ready) {
                    // Try writing again after resume
                    result = send(socket->fd(), data_ptr, remaining, MSG_NOSIGNAL);
                }
                return result;
            }
        };

        WriteAwaiter awaiter{this, data.data() + total_sent, data.size() - total_sent};
        ssize_t sent = co_await awaiter;

        if (sent <= 0) {
            break; // Error or connection closed
        }

        total_sent += sent;
    }
}

void Socket::close() {
    if (owns_fd_ && fd_ >= 0) {
        // Remove from both platform I/O and pending I/O map
        io_context_->remove_io(fd_);
        io_context_->platform_io().remove_fd(fd_);
        close_socket(fd_);
        fd_ = -1;
        owns_fd_ = false;
    }
}

// Server implementation
Server::Server(std::size_t num_threads)
    : io_context_(std::make_unique<IOContext>(num_threads)), running_(false) {}

Server::~Server() {
    stop();
    for (auto& t : accept_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
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

void Server::listen_tcp(uint16_t port) {
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
    struct sockaddr_in addr {};
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

    // Start accept thread
    running_.store(true);
    accept_threads_.emplace_back(&Server::accept_connections, this, listen_fd);
}

void Server::listen_udp(uint16_t port) {
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        throw std::runtime_error("Failed to create UDP socket");
    }

    // Set socket options for reuse
    int opt = 1;
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(udp_fd, FIONBIO, &mode);
#else
    int flags = fcntl(udp_fd, F_GETFL, 0);
    fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK);
#endif

    // Bind
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(udp_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(udp_fd);
        throw std::runtime_error("Failed to bind UDP socket");
    }

    // For now, UDP just binds - actual packet handling would need a receive loop
    // This is a simplified implementation
}

void Server::accept_connections(int listen_fd) {
    while (running_.load()) {
        struct sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);

        int client_fd =
            accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No connection available, sleep briefly and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
    close_socket(listen_fd);
}

Task Server::handle_connection(Socket socket) {
    if (!http_handler_ && !http2_handler_ && !websocket_handler_) {
        socket.close();
        co_return;
    }

    try {
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
            // Remove trailing \r if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.find("HTTP/") != std::string::npos) {
                // HTTP request - parse the request line
                if (http_handler_) {
                    HTTPRequest request;
                    HTTPResponse response;

                    // Simple parsing (in production, use a proper parser)
                    std::istringstream line_stream(line);
                    line_stream >> request.method >> request.path >> request.version;

                    // Parse headers
                    while (std::getline(iss, line)) {
                        // Remove trailing \r
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }
                        // Empty line marks end of headers
                        if (line.empty()) {
                            break;
                        }

                        size_t colon = line.find(':');
                        if (colon != std::string::npos) {
                            std::string key = line.substr(0, colon);
                            std::string value = line.substr(colon + 1);
                            // Trim leading whitespace from value
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
            } else if (websocket_handler_ && (line.find("GET") == 0 || line.find("POST") == 0)) {
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
    } catch (...) {
        // Catch any exceptions to prevent coroutine crashes
    }

    socket.close();
}

void Server::run() {
    io_context_->run();
}

void Server::stop() {
    running_.store(false);
    io_context_->stop();
}

} // namespace spaznet
