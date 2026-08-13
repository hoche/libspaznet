#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/server.hpp>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
inline auto close_socket(int fd) -> void {
    spaznet::detail::close_socket_fd(fd);
}
} // namespace

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

#ifdef SPAZNET_HAS_COROUTINES
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

ValueTask<ssize_t> Socket::async_read(std::vector<uint8_t>& buffer, std::size_t size) {
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
                result = detail::socket_recv(socket->fd(), buffer->data(), size, 0);
                if (result >= 0) {
                    ready_flag = true;
                    return true;
                }
                saved_errno = detail::last_socket_error();
                if (detail::is_retryable_socket_error(saved_errno)) {
                    ready_flag = false;
                    return false;
                }
                ready_flag = true;
                return true;
            }

            // The enclosing coroutine's promise is ValueTaskPromise<ssize_t>;
            // naming it lets us hand register_io a ref-counted handle
            // without reinterpreting the promise type.
            void await_suspend(std::coroutine_handle<ValueTaskPromise<ssize_t>> h) {
                socket->context()->register_io(socket->fd(), PlatformIO::EVENT_READ,
                                               CoroutineHandle::from_handle(h));
            }

            ssize_t await_resume() noexcept {
                if (!ready_flag) {
                    result = detail::socket_recv(socket->fd(), buffer->data(), size, 0);
                    saved_errno = (result < 0) ? detail::last_socket_error() : 0;
                }
                return result;
            }
        };

        ReadAwaiter awaiter{this, &buffer, size};
        ssize_t result = co_await awaiter;

        if (result > 0) {
            buffer.resize(static_cast<std::size_t>(result));
            co_return result;
        }
        if (result == 0) {
            // Peer closed the connection (orderly EOF).
            buffer.clear();
            co_return 0;
        }
        // result < 0
        if (detail::is_retryable_socket_error(awaiter.saved_errno)) {
            // Spurious wakeup or interrupted syscall — re-await. No
            // sleeping: the IOContext will resume us when data really is
            // available.
            continue;
        }
        // Hard error. Report it as a negative result so callers can tell
        // an error apart from an orderly EOF (which co_returns 0); the
        // buffer is cleared in both cases.
        buffer.clear();
        co_return -1;
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
                result = detail::socket_send(socket->fd(), data_ptr, remaining, MSG_NOSIGNAL);
                if (result >= 0) {
                    ready_flag = true;
                    return true;
                }
                saved_errno = detail::last_socket_error();
                if (detail::is_retryable_socket_error(saved_errno)) {
                    ready_flag = false;
                    return false;
                }
                ready_flag = true;
                return true;
            }

            void await_suspend(std::coroutine_handle<TaskPromise> h) {
                socket->context()->register_io(socket->fd(), PlatformIO::EVENT_WRITE,
                                               CoroutineHandle::from_handle(h));
            }

            ssize_t await_resume() noexcept {
                if (!ready_flag) {
                    result = detail::socket_send(socket->fd(), data_ptr, remaining, MSG_NOSIGNAL);
                    saved_errno = (result < 0) ? detail::last_socket_error() : 0;
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
        if (detail::is_retryable_socket_error(awaiter.saved_errno)) {
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
#endif // SPAZNET_HAS_COROUTINES

// Server implementation
Server::Server(std::size_t num_threads)
    : Server(ServerConfig{.loops = 1, .workers_per_loop = num_threads}) {}

Server::Server(ServerConfig config) : running_(false) {
    const std::size_t loops = std::max<std::size_t>(1, config.loops);
    io_contexts_.reserve(loops);
    for (std::size_t i = 0; i < loops; ++i) {
        io_contexts_.push_back(std::make_unique<IOContext>(config.workers_per_loop));
    }
}

Server::~Server() {
    stop();
}

auto Server::pick_accept_loop() -> IOContext& {
    if (io_contexts_.size() == 1) {
        return *io_contexts_.front();
    }
    const std::size_t idx =
        next_accept_loop_.fetch_add(1, std::memory_order_relaxed) % io_contexts_.size();
    return *io_contexts_[idx];
}

auto Server::get_statistics() const -> Statistics {
    Statistics agg;
    for (const auto& ctx : io_contexts_) {
        const Statistics s = ctx->get_statistics();
        agg.active_requests += s.active_requests;
        agg.total_coroutines_created += s.total_coroutines_created;
        agg.active_coroutines += s.active_coroutines;
        agg.total_memory_bytes += s.total_memory_bytes;
        agg.active_connections += s.active_connections;
        agg.bytes_buffered += s.bytes_buffered;
    }
    return agg;
}

#ifdef SPAZNET_HAS_COROUTINES
void Server::set_connection_handler(ConnectionHandler handler) {
    connection_handler_ = std::move(handler);
}

void Server::set_datagram_handler(DatagramHandler handler) {
    datagram_handler_ = std::move(handler);
}
#endif // SPAZNET_HAS_COROUTINES

void Server::set_connection_factory(ConnectionFactory factory) {
    connection_factory_ = std::move(factory);
}

void Server::set_sync_datagram_handler(SyncDatagramHandler handler) {
    sync_datagram_handler_ = std::move(handler);
}

void Server::finish_reactor_connection(int fd, IOContext* ctx) {
    {
        std::lock_guard<std::mutex> lock(reactor_conns_mutex_);
        reactor_connections_.erase(fd);
    }
    if (ctx != nullptr) {
        ctx->decrement_active_connections();
    }
}

// Persistent reactor handler for a listening TCP socket: on_readable()
// drains accept_ready()'s accept() loop, then — unless accept_ready()
// says otherwise (stop() flipped running_, or a hard accept() error
// already closed the fd) — re-registers itself for the next readability
// event via shared_from_this(), the same "read to EAGAIN, then re-arm"
// pattern BufferedConnection::on_readable() uses. A nested class of
// Server so it can call the private accept_ready() directly. Defined here
// (ahead of listen_tcp()'s first use) rather than down by accept_ready()
// itself so the compiler has seen its full definition — including the
// `public IoHandler` base — by the time listen_tcp() upcasts a
// shared_ptr<ListenHandler> to shared_ptr<IoHandler>.
class Server::ListenHandler : public IoHandler,
                               public std::enable_shared_from_this<ListenHandler> {
  public:
    ListenHandler(Server& server, int fd) : server_(server), fd_(fd) {}

    void on_readable() override {
        if (server_.accept_ready(fd_)) {
            server_.primary_context().set_io_handler(fd_, PlatformIO::EVENT_READ,
                                                     shared_from_this());
        }
    }
    void on_writable() override {}

  private:
    Server& server_;
    int fd_;
};

// Same pattern as ListenHandler, for a UDP socket's datagram_ready().
class Server::DatagramReadHandler : public IoHandler,
                                    public std::enable_shared_from_this<DatagramReadHandler> {
  public:
    DatagramReadHandler(Server& server, int fd) : server_(server), fd_(fd) {}

    void on_readable() override {
        if (server_.datagram_ready(fd_)) {
            server_.primary_context().set_io_handler(fd_, PlatformIO::EVENT_READ,
                                                     shared_from_this());
        }
    }
    void on_writable() override {}

  private:
    Server& server_;
    int fd_;
};

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
    detail::setsockopt_int(listen_fd, SOL_SOCKET, SO_REUSEADDR, 1);
    // Allow IPv4 connections on IPv6 socket
    detail::setsockopt_int(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, 0);

    // Set non-blocking
    detail::set_nonblocking(listen_fd);

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
    // Register a persistent reactor handler on the IOContext (works in
    // both threaded and non-threaded modes, no coroutine involved) — see
    // ListenHandler below and accept_ready()'s comment in server.hpp.
    primary_context().set_io_handler(listen_fd, PlatformIO::EVENT_READ,
                                     std::make_shared<ListenHandler>(*this, listen_fd));
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
    detail::setsockopt_int(udp_fd, SOL_SOCKET, SO_REUSEADDR, 1);
    // Allow IPv4 on IPv6 socket
    detail::setsockopt_int(udp_fd, IPPROTO_IPV6, IPV6_V6ONLY, 0);

    // Set non-blocking
    detail::set_nonblocking(udp_fd);

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
    // UDP stays on loop 0 until a later SO_REUSEPORT / CID-hash design —
    // one datagram socket cannot be sharded by connection the way TCP fds can.
    primary_context().set_io_handler(udp_fd, PlatformIO::EVENT_READ,
                                     std::make_shared<DatagramReadHandler>(*this, udp_fd));
}

bool Server::datagram_ready(int udp_fd) {
    while (running_.load(std::memory_order_acquire)) {
        std::vector<uint8_t> buffer(64 * 1024);
        sockaddr_storage addr{};
        socklen_t addr_len = sizeof(addr);

        ssize_t received = detail::socket_recvfrom(udp_fd, buffer.data(), buffer.size(), 0,
                                                   reinterpret_cast<struct sockaddr*>(&addr),
                                                   &addr_len);

        if (received < 0) {
            const int err = detail::last_socket_error();
            if (detail::is_retryable_socket_error(err)) {
                return true; // Drained to EAGAIN; caller re-arms read interest.
            }
            // Hard error: matches the old coroutine loop's behavior of
            // just exiting without independently closing udp_fd —
            // Server::stop()'s listen_fds_ cleanup owns that.
            return false;
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

        // Reactor path takes precedence: a plain synchronous call, no
        // coroutine involved. Falls back to the coroutine-based
        // datagram_handler_ (fire-and-forget via schedule(), matching how
        // accept_ready() below hands off connection_handler_) if no sync
        // handler is installed.
        if (sync_datagram_handler_) {
            Datagram dg;
            dg.data = buffer;
            dg.peer_addr = host;
            dg.peer_port = port;
            std::memcpy(&dg.peer, &addr, addr_len);
            dg.peer_len = addr_len;
            dg.fd = udp_fd;
            try {
                sync_datagram_handler_(std::move(dg));
            } catch (...) {
            }
        }
#ifdef SPAZNET_HAS_COROUTINES
        else if (datagram_handler_) {
            Datagram dg;
            dg.data = buffer;
            dg.peer_addr = host;
            dg.peer_port = port;
            std::memcpy(&dg.peer, &addr, addr_len);
            dg.peer_len = addr_len;
            dg.fd = udp_fd;
            try {
                primary_context().schedule(datagram_handler_(std::move(dg)));
            } catch (...) {
            }
        }
#endif
    }
    return false; // running_ went false.
}

bool Server::accept_ready(int listen_fd) {
    while (running_.load(std::memory_order_acquire)) {
        struct sockaddr_storage client_addr {}; // Can hold IPv4 or IPv6
        socklen_t client_len = sizeof(client_addr);

        int client_fd =
            accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            const int err = detail::last_socket_error();
            if (detail::is_retryable_socket_error(err)) {
                return true; // Drained to EAGAIN; caller re-arms read interest.
            }
            break; // Hard error: fall through to the listen-fd cleanup below.
        }

        // Set non-blocking
        detail::set_nonblocking(client_fd);

        // Reactor path takes precedence over the coroutine-based
        // connection_handler_ (see ConnectionFactory's docs above).
        // Accept-and-shard: the listen fd stays on loop 0; the client fd is
        // handed to a target loop and never registered on the accept loop.
        if (connection_factory_) {
            int fd = client_fd; // captured by value below; client_fd is reused next loop iteration
            IOContext* target = &pick_accept_loop();
            auto on_closed = [this, fd, target]() { finish_reactor_connection(fd, target); };
            auto handler = connection_factory_(client_fd, *target, std::move(on_closed));
            if (handler) {
                {
                    std::lock_guard<std::mutex> lock(reactor_conns_mutex_);
                    reactor_connections_[client_fd] =
                        ReactorConn{std::move(handler), target};
                }
                target->increment_active_connections();
            } else {
                // Factory declined the connection outright (e.g. some
                // limit); nothing registered it for events, so close it
                // ourselves rather than leaking the fd.
                close_socket(client_fd);
            }
            continue;
        }

#ifdef SPAZNET_HAS_COROUTINES
        // Coroutine connections stay on loop 0 so Server(N) keeps its
        // historical "workers on one loop" meaning.
        Socket socket(client_fd, &primary_context());
        primary_context().schedule(handle_connection(std::move(socket)));
#else
        // No connection_factory_ installed and no coroutine runtime to
        // fall back to — nothing can drive this connection, so drop it
        // rather than leak the fd.
        close_socket(client_fd);
#endif
    }

    // Either running_ went false, or accept() hit a hard error: same
    // listen-fd cleanup the coroutine path used to do after its loop
    // exited. During normal shutdown, stop() has usually already done
    // this (in which case this is a no-op) — see stop()'s Step 2.
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
        primary_context().remove_io(listen_fd);
        close_socket(listen_fd);
    }
    return false;
}

#ifdef SPAZNET_HAS_COROUTINES
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
                // Also feed the runtime-neutral IOContext-level gauge (see
                // Statistics::active_connections) so get_statistics() is
                // accurate whether this Server uses the coroutine path,
                // the reactor path, or — during migration — both.
                server->primary_context().increment_active_connections();
            }
            ~ConnGuard() {
                {
                    std::lock_guard<std::mutex> lock(server->client_fds_mutex_);
                    server->active_client_fds_.erase(fd);
                }
                server->active_connections_.fetch_sub(1, std::memory_order_acq_rel);
                server->primary_context().decrement_active_connections();
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
#endif // SPAZNET_HAS_COROUTINES

void Server::run() {
    // Start loops 1..N-1 on their own threads, then block in loop 0.
    // Accept stays on loop 0; reactor connections may already be registered
    // on secondary loops by the time those threads enter run() — set_io_handler
    // before wait() is fine (interest is recorded, events fire once wait runs).
    loop_threads_.clear();
    loop_threads_.reserve(io_contexts_.size() > 0 ? io_contexts_.size() - 1 : 0);
    for (std::size_t i = 1; i < io_contexts_.size(); ++i) {
        loop_threads_.emplace_back([this, i]() { io_contexts_[i]->run(); });
    }
    primary_context().run();
    for (auto& t : loop_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    loop_threads_.clear();
}

void Server::stop() {
    // Step 1: stop accepting new connections.
    running_.store(false);

    // Step 2: close listening sockets so accept coroutines unwind. We do
    // this BEFORE asking the IOContext to stop so the event loop can keep
    // processing the unwinds. Listen/UDP fds live only on loop 0.
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(listen_fds_mutex_);
        fds.swap(listen_fds_);
    }
    for (int fd : fds) {
        if (fd < 0) {
            continue;
        }
        primary_context().remove_io(fd);
        close_socket(fd);
    }

#ifdef SPAZNET_HAS_COROUTINES
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
#endif // SPAZNET_HAS_COROUTINES

    // Step 3.5: force-close every reactor connection still registered.
    // Unlike coroutines, these have no suspended call stack to unwind —
    // just hand each one's shared_ptr to IoHandler::shutdown() (which, for
    // BufferedConnection, closes the fd and fires its on_closed hook,
    // which in turn calls finish_reactor_connection() below). Routed
    // through each owning loop's IOContext::post_to_io_thread() (not the
    // round-robining post()) so this is GUARANTEED to run on that
    // connection's IO thread instead of racing this thread (stop() may be
    // called from any thread) — or a worker thread, which plain post()
    // could otherwise hand it to — against in-flight
    // on_readable()/on_writable() calls for the same connections.
    //
    // Done flags are heap-allocated because if run() was never called, or
    // a loop has already exited, post_to_io_thread() never executes and
    // the bounded wait below simply times out; the lambda — and the
    // connections it would have shut down — leak, on the same "don't
    // deadlock stop()" basis the coroutine drain below accepts.
    {
        std::unordered_map<int, ReactorConn> conns;
        {
            std::lock_guard<std::mutex> lock(reactor_conns_mutex_);
            conns.swap(reactor_connections_);
        }
        std::unordered_map<IOContext*, std::vector<std::shared_ptr<IoHandler>>> by_ctx;
        by_ctx.reserve(io_contexts_.size());
        for (auto& [fd, entry] : conns) {
            (void)fd;
            if (entry.ctx != nullptr && entry.handler) {
                by_ctx[entry.ctx].push_back(std::move(entry.handler));
            }
        }
        std::vector<std::shared_ptr<std::atomic<bool>>> dones;
        dones.reserve(by_ctx.size());
        for (auto& [ctx, handlers] : by_ctx) {
            auto done = std::make_shared<std::atomic<bool>>(false);
            dones.push_back(done);
            ctx->post_to_io_thread(
                [handlers = std::move(handlers), done]() mutable {
                    for (auto& handler : handlers) {
                        handler->shutdown();
                    }
                    done->store(true, std::memory_order_release);
                });
        }
        const auto reactor_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        for (const auto& done : dones) {
            while (!done->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < reactor_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

#ifdef SPAZNET_HAS_COROUTINES
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
#endif // SPAZNET_HAS_COROUTINES

    // Step 5: signal every IOContext loop to exit. After primary run()
    // returns, Server::run() joins the secondary loop threads. Worker
    // threads inside each IOContext are joined by that context's run().
    // Any coroutines still suspended past the drain deadline will leak —
    // we accept that over a deadlocked shutdown.
    for (auto& ctx : io_contexts_) {
        ctx->stop();
    }
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
