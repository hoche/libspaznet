#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#ifdef SPAZNET_HAS_QUIC
#include <libspaznet/http3/service.hpp>
#endif
#include <libspaznet/io_context.hpp>
#include <libspaznet/server.hpp>
#include <map>
#include <optional>
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
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#define close_socket ::close
#endif

// This translation unit is intentionally low-level (socket I/O, protocol parsing) and uses many
// protocol-defined constants (e.g. bitmasks, opcodes, fixed header sizes). We suppress a few
// noisy style checks here to keep clang-tidy signal high for the rest of the codebase.
// NOLINTBEGIN(
//   cppcoreguidelines-avoid-magic-numbers,
//   readability-magic-numbers,
//   readability-identifier-length,
//   modernize-use-trailing-return-type,
//   modernize-avoid-c-arrays,
//   cppcoreguidelines-avoid-c-arrays,
//   cppcoreguidelines-pro-bounds-constant-array-index,
//   cppcoreguidelines-pro-bounds-array-to-pointer-decay,
//   cppcoreguidelines-pro-bounds-pointer-arithmetic,
//   cppcoreguidelines-pro-type-reinterpret-cast,
//   cppcoreguidelines-pro-type-vararg,
//   cppcoreguidelines-pro-type-member-init,
//   readability-implicit-bool-conversion,
//   readability-isolate-declaration,
//   readability-make-member-function-const,
//   readability-convert-member-functions-to-static,
//   readability-function-cognitive-complexity,
//   cppcoreguidelines-avoid-reference-coroutine-parameters,
//   cppcoreguidelines-avoid-capturing-lambda-coroutines
// )

namespace spaznet {

namespace {

// (WebSocket / RFC 6455 helpers — sha1, base64_encode, header_has_token,
// parse_websocket_request, compute_websocket_accept — moved to
// example/http-websocket/src/dispatcher.cpp along with the rest of the
// WS dispatch.)

} // namespace

// Socket implementation
//
// Awaiter design notes:
//   - await_ready() does ONE non-blocking recv/send. On success or hard
//     error it returns true and the syscall's result is read by
//     await_resume(). On EAGAIN/EWOULDBLOCK/EINTR it suspends the
//     coroutine, registering EVENT_READ/EVENT_WRITE with the IOContext.
//   - await_resume() does ONE more recv/send after wakeup and returns the
//     ssize_t directly. No sleep_for, no retry loop inside the awaiter —
//     spurious wakeups are handled by the outer co_await loop, which
//     re-enters the awaiter (and thus re-registers with epoll) without
//     blocking a worker thread.
//   - The outer Task body distinguishes:
//        result  > 0  → got data,  buffer resized, return.
//        result == 0  → orderly EOF, buffer cleared, return.
//        result <  0  + EAGAIN/EWOULDBLOCK/EINTR → spurious; re-await.
//        result <  0  otherwise → hard error, buffer cleared, return.

namespace {

bool is_retryable_errno(int e) {
    return e == EAGAIN || e == EWOULDBLOCK || e == EINTR;
}

} // namespace

Task Socket::async_read(std::vector<uint8_t>& buffer, std::size_t size) {
    buffer.resize(size);

    while (true) {
        struct ReadAwaiter {
            Socket* socket;
            std::vector<uint8_t>* buffer;
            std::size_t size;
            mutable ssize_t result = 0;
            mutable int saved_errno = 0;
            mutable bool ready_flag = false;

            bool await_ready() const noexcept {
                result = recv(socket->fd(), buffer->data(), size, 0);
                if (result >= 0) {
                    ready_flag = true;
                    return true;
                }
                saved_errno = errno;
                if (is_retryable_errno(saved_errno)) {
                    ready_flag = false;
                    return false;
                }
                ready_flag = true;
                return true;
            }

            void await_suspend(std::coroutine_handle<> h) {
                socket->context()->register_io(socket->fd(), PlatformIO::EVENT_READ, h);
            }

            ssize_t await_resume() noexcept {
                if (!ready_flag) {
                    result = recv(socket->fd(), buffer->data(), size, 0);
                    saved_errno = (result < 0) ? errno : 0;
                }
                return result;
            }
        };

        ReadAwaiter awaiter{this, &buffer, size};
        ssize_t result = co_await awaiter;

        if (result > 0) {
            buffer.resize(static_cast<std::size_t>(result));
            co_return;
        }
        if (result == 0) {
            // Peer closed the connection (orderly EOF).
            buffer.clear();
            co_return;
        }
        // result < 0
        if (is_retryable_errno(awaiter.saved_errno)) {
            // Spurious wakeup or interrupted syscall — re-await. No
            // sleeping: the IOContext will resume us when data really is
            // available.
            continue;
        }
        // Hard error.
        buffer.clear();
        co_return;
    }
}

Task Socket::async_write(std::vector<uint8_t> data) {
    std::size_t total_sent = 0;

    while (total_sent < data.size()) {
        struct WriteAwaiter {
            Socket* socket;
            const uint8_t* data_ptr;
            std::size_t remaining;
            mutable ssize_t result = 0;
            mutable int saved_errno = 0;
            mutable bool ready_flag = false;

            bool await_ready() const noexcept {
                result = send(socket->fd(), data_ptr, remaining, MSG_NOSIGNAL);
                if (result >= 0) {
                    ready_flag = true;
                    return true;
                }
                saved_errno = errno;
                if (is_retryable_errno(saved_errno)) {
                    ready_flag = false;
                    return false;
                }
                ready_flag = true;
                return true;
            }

            void await_suspend(std::coroutine_handle<> h) {
                socket->context()->register_io(socket->fd(), PlatformIO::EVENT_WRITE, h);
            }

            ssize_t await_resume() noexcept {
                if (!ready_flag) {
                    result = send(socket->fd(), data_ptr, remaining, MSG_NOSIGNAL);
                    saved_errno = (result < 0) ? errno : 0;
                }
                return result;
            }
        };

        WriteAwaiter awaiter{this, data.data() + total_sent, data.size() - total_sent};
        ssize_t sent = co_await awaiter;

        if (sent > 0) {
            total_sent += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == 0) {
            // send() returning 0 is highly unusual; treat as broken pipe
            // and exit so the caller (which sees a partial write via
            // unchanged total_sent / closed socket on next op) can
            // recover.
            break;
        }
        // sent < 0
        if (is_retryable_errno(awaiter.saved_errno)) {
            // Spurious EAGAIN — re-await without sleeping.
            continue;
        }
        // Hard error.
        break;
    }
}

void Socket::close() {
    if (owns_fd_ && fd_ >= 0) {
        // Remove from both platform I/O and pending I/O map (remove_io
        // now handles both under its spinlock).
        io_context_->remove_io(fd_);
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
}

void Server::set_connection_handler(ConnectionHandler handler) {
    connection_handler_ = std::move(handler);
}

void Server::set_datagram_handler(DatagramHandler handler) {
    datagram_handler_ = std::move(handler);
}

void Server::set_udp_handler(std::unique_ptr<UDPHandler> handler) {
    udp_handler_ = std::move(handler);
}

#ifdef SPAZNET_HAS_QUIC
void Server::set_quic_http3_service(std::unique_ptr<http3::QuicHttp3Service> service) {
    quic_http3_service_ = std::move(service);
}
#endif

void Server::listen_tcp(uint16_t port) {
    // Use getaddrinfo for IPv4/IPv6 compatibility
    struct addrinfo hints {
    }, *result = nullptr;
    hints.ai_family = AF_INET6; // IPv6 socket (can accept IPv4 via IPv4-mapped addresses)
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // For wildcard bind address

    std::string port_str = std::to_string(port);
    if (getaddrinfo(nullptr, port_str.c_str(), &hints, &result) != 0) {
        throw std::runtime_error("Failed to resolve address");
    }

    int listen_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listen_fd < 0) {
        freeaddrinfo(result);
        throw std::runtime_error("Failed to create socket");
    }

    // Set socket options
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // Allow IPv4 connections on IPv6 socket
    int no = 0;
    setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(listen_fd, FIONBIO, &mode);
#else
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
#endif

    // Bind
    if (bind(listen_fd, result->ai_addr, result->ai_addrlen) < 0) {
        close_socket(listen_fd);
        freeaddrinfo(result);
        throw std::runtime_error("Failed to bind socket");
    }

    freeaddrinfo(result);

    // Listen.  SOMAXCONN is 128 on macOS and that's enough to let
    // hundreds of concurrent connect()s collide and overflow the SYN
    // queue.  Pass a larger value; the kernel clamps to its own ceiling
    // (sysctl kern.ipc.somaxconn on BSD/macOS, net.core.somaxconn on
    // Linux), which is typically much higher on Linux and lets bursty
    // tests succeed without retries.
    constexpr int kListenBacklog = 4096;
    if (listen(listen_fd, kListenBacklog) < 0) {
        close_socket(listen_fd);
        throw std::runtime_error("Failed to listen on socket");
    }

    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(listen_fds_mutex_);
        listen_fds_.push_back(listen_fd);
    }
    // Schedule accept loop on the IOContext (works in both threaded and non-threaded modes).
    io_context_->schedule(accept_connections(listen_fd));
}

void Server::listen_udp(uint16_t port) {
    // Use getaddrinfo for IPv4/IPv6 compatibility
    struct addrinfo hints {
    }, *result = nullptr;
    hints.ai_family = AF_INET6; // IPv6 socket (can accept IPv4 via IPv4-mapped addresses)
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(nullptr, port_str.c_str(), &hints, &result) != 0) {
        throw std::runtime_error("Failed to resolve address for UDP");
    }

    int udp_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (udp_fd < 0) {
        freeaddrinfo(result);
        throw std::runtime_error("Failed to create UDP socket");
    }

    // Set socket options for reuse
    int opt = 1;
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // Allow IPv4 on IPv6 socket
    int no = 0;
    setsockopt(udp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(udp_fd, FIONBIO, &mode);
#else
    int flags = fcntl(udp_fd, F_GETFL, 0);
    fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK);
#endif

    // Bind
    if (bind(udp_fd, result->ai_addr, result->ai_addrlen) < 0) {
        close_socket(udp_fd);
        freeaddrinfo(result);
        throw std::runtime_error("Failed to bind UDP socket");
    }

    freeaddrinfo(result);

    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(listen_fds_mutex_);
        listen_fds_.push_back(udp_fd);
    }
    io_context_->schedule(receive_udp(udp_fd));
}

Task Server::receive_udp(int udp_fd) {
    struct ReadableAwaiter {
        IOContext* ctx;
        int fd;
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) const {
            ctx->register_io(fd, PlatformIO::EVENT_READ, h);
        }
        void await_resume() const noexcept {}
    };

    Socket udp_socket(udp_fd, io_context_.get(), /*owns_fd=*/false);

    while (running_.load(std::memory_order_acquire)) {
        std::vector<uint8_t> buffer(64 * 1024);
        sockaddr_storage addr{};
        socklen_t addr_len = sizeof(addr);

        ssize_t received = recvfrom(udp_fd, buffer.data(), buffer.size(), 0,
                                    reinterpret_cast<struct sockaddr*>(&addr), &addr_len);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                co_await ReadableAwaiter{io_context_.get(), udp_fd};
                continue;
            }
            break;
        }

        if (received == 0) {
            continue;
        }

        buffer.resize(static_cast<size_t>(received));

        // Best-effort address/port stringification for diagnostics.
        char host[INET6_ADDRSTRLEN]{};
        uint16_t port = 0;
        if (addr.ss_family == AF_INET) {
            const auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
            inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
            port = ntohs(a->sin_port);
        } else if (addr.ss_family == AF_INET6) {
            const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&addr);
            inet_ntop(AF_INET6, &a6->sin6_addr, host, sizeof(host));
            port = ntohs(a6->sin6_port);
        }

        // Low-level path: deliver the raw datagram to the user's
        // datagram_handler if one is installed.
        if (datagram_handler_) {
            Datagram dg;
            dg.data = buffer;
            dg.peer_addr = host;
            dg.peer_port = port;
            std::memcpy(&dg.peer, &addr, addr_len);
            dg.peer_len = addr_len;
            dg.fd = udp_fd;
            try {
                co_await datagram_handler_(std::move(dg));
            } catch (...) {
            }
        }

        if (udp_handler_) {
            UDPPacket pkt;
            pkt.data = buffer;
            pkt.address = host;
            pkt.port = port;
            co_await udp_handler_->handle_packet(pkt, udp_socket);
        }

#ifdef SPAZNET_HAS_QUIC
        if (quic_http3_service_) {
            spaznet::quic::PeerAddr peer{};
            peer.length = addr_len;
            std::memcpy(&peer.storage, &addr, addr_len);
            quic_http3_service_->handle_datagram(peer, {buffer.data(), buffer.size()});
        }
#endif
    }

    co_return;
}

Task Server::accept_connections(int listen_fd) {
    struct ReadableAwaiter {
        IOContext* ctx;
        int fd;
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> h) const {
            ctx->register_io(fd, PlatformIO::EVENT_READ, h);
        }
        void await_resume() const noexcept {}
    };

    while (running_.load(std::memory_order_acquire)) {
        struct sockaddr_storage client_addr {}; // Can hold IPv4 or IPv6
        socklen_t client_len = sizeof(client_addr);

        int client_fd =
            accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Wait until the listening socket becomes readable (new connection ready).
                co_await ReadableAwaiter{io_context_.get(), listen_fd};
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

    // If we exit the accept loop due to error while still running, close the listening fd here.
    // During normal shutdown, stop() closes all listening fds and may destroy this coroutine while
    // suspended.
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lock(listen_fds_mutex_);
        auto it = std::find(listen_fds_.begin(), listen_fds_.end(), listen_fd);
        if (it != listen_fds_.end()) {
            listen_fds_.erase(it);
            should_close = true;
        }
    }
    if (should_close) {
        io_context_->remove_io(listen_fd);
        close_socket(listen_fd);
    }
    co_return;
}

Task Server::handle_connection(Socket socket) {
    // Low-level path: if the user installed a connection_handler_,
    // hand the Socket over and let them speak whatever protocol they
    // want.  The Socket is moved into a guard so cleanup is correct
    // whether the user's coroutine completes normally, throws, or
    // unwinds via Server::stop().
    if (connection_handler_) {
        struct ConnGuard {
            Server* server;
            int fd;
            ConnGuard(Server* s, int f) : server(s), fd(f) {
                {
                    std::lock_guard<std::mutex> lock(server->client_fds_mutex_);
                    server->active_client_fds_.insert(fd);
                }
                server->active_connections_.fetch_add(1, std::memory_order_acq_rel);
            }
            ~ConnGuard() {
                {
                    std::lock_guard<std::mutex> lock(server->client_fds_mutex_);
                    server->active_client_fds_.erase(fd);
                }
                server->active_connections_.fetch_sub(1, std::memory_order_acq_rel);
            }
            ConnGuard(const ConnGuard&) = delete;
            ConnGuard& operator=(const ConnGuard&) = delete;
        };
        ConnGuard cg(this, socket.fd());
        try {
            co_await connection_handler_(std::move(socket));
        } catch (...) {
            // Swallow; the Socket's destructor closes the fd if the
            // handler didn't already.
        }
        co_return;
    }
    // No connection_handler_ installed — drop the connection.
    socket.close();
    co_return;
}

void Server::run() {
    io_context_->run();
}

void Server::stop() {
    // Step 1: stop accepting new connections.
    running_.store(false);

    // Step 2: close listening sockets so accept coroutines unwind. We do
    // this BEFORE asking the IOContext to stop so the event loop can keep
    // processing the unwinds.
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(listen_fds_mutex_);
        fds.swap(listen_fds_);
    }
    for (int fd : fds) {
        if (fd < 0) {
            continue;
        }
        io_context_->remove_io(fd);
        close_socket(fd);
    }

    // Step 3: shutdown(2) every active client fd. This forces any
    // coroutine suspended on recv/send for that connection to wake up
    // with an error, unwind through its destructors, and decrement
    // active_connections_ via the ConnectionGuard.
    {
        std::lock_guard<std::mutex> lock(client_fds_mutex_);
        for (int fd : active_client_fds_) {
            if (fd < 0) {
                continue;
            }
#ifdef _WIN32
            shutdown(fd, SD_BOTH);
#else
            ::shutdown(fd, SHUT_RDWR);
#endif
        }
    }

    // Step 4: drain in-flight coroutines, with a deadline so a wedged
    // handler can't deadlock stop(). 1 second is a defensible upper bound
    // for any reasonable in-flight request to either complete or fail
    // out after its socket has been shut down.
    const auto drain_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (active_connections_.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < drain_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Step 5: signal the IOContext loop to exit. After this returns,
    // worker threads (joined inside IOContext::run) will terminate. Any
    // coroutines still suspended past the drain deadline will leak — we
    // accept that over a deadlocked shutdown.
    io_context_->stop();
}

// NOLINTEND(
//   cppcoreguidelines-avoid-magic-numbers,
//   readability-magic-numbers,
//   readability-identifier-length,
//   modernize-use-trailing-return-type,
//   modernize-avoid-c-arrays,
//   cppcoreguidelines-avoid-c-arrays,
//   cppcoreguidelines-pro-bounds-constant-array-index,
//   cppcoreguidelines-pro-bounds-array-to-pointer-decay,
//   cppcoreguidelines-pro-bounds-pointer-arithmetic,
//   cppcoreguidelines-pro-type-reinterpret-cast,
//   cppcoreguidelines-pro-type-vararg,
//   cppcoreguidelines-pro-type-member-init,
//   readability-implicit-bool-conversion,
//   readability-isolate-declaration,
//   readability-make-member-function-const,
//   readability-convert-member-functions-to-static,
//   readability-function-cognitive-complexity,
//   cppcoreguidelines-avoid-reference-coroutine-parameters,
//   cppcoreguidelines-avoid-capturing-lambda-coroutines
// )

} // namespace spaznet
