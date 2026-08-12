#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/platform_io.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Server/public API header: suppress noisy style checks.
// NOLINTBEGIN

namespace spaznet {


// Socket wrapper
class Socket {
  private:
    int fd_;
    IOContext* io_context_;
    bool owns_fd_;

  public:
    Socket(int fd, IOContext* ctx, bool owns_fd = true)
        : fd_(fd), io_context_(ctx), owns_fd_(owns_fd) {}

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

    // Async read. Resizes `buffer` to the bytes received and returns that
    // count; returns 0 on orderly EOF (buffer cleared) and -1 on a hard
    // error (buffer cleared). Callers that only need the data can ignore
    // the result and inspect buffer.size() as before.
    ValueTask<ssize_t> async_read(std::vector<uint8_t>& buffer, std::size_t size);

    // Async write
    Task async_write(std::vector<uint8_t> data);

    void close();
};

} // namespace spaznet

// NOLINTEND


namespace spaznet {

// Raw UDP datagram delivered to a DatagramHandler.  The peer fields are
// filled in from the kernel-reported sockaddr; `peer` + `peer_len` are
// kept verbatim so a handler can sendto() back without re-resolving.
struct Datagram {
    std::vector<std::uint8_t> data;
    std::string peer_addr;     // dotted-quad / colon-hex (best-effort, diagnostics)
    std::uint16_t peer_port{0};
    sockaddr_storage peer{};
    socklen_t peer_len{0};
    int fd{-1};                // the UDP socket the datagram arrived on
};

// Per-connection callback: the Server invokes this once for each
// accepted TCP connection, handing ownership of the Socket.  The
// connection lives until the Task completes; the Socket destructor
// closes the fd if the handler hasn't already.
using ConnectionHandler = std::function<Task(Socket)>;

// Per-datagram callback: the Server invokes this once for each UDP
// datagram received on any port it's listening on.
using DatagramHandler = std::function<Task(Datagram)>;

// Reactor-side per-connection callback: invoked once for each accepted TCP
// connection with the fd (already set non-blocking), the IOContext it was
// accepted on, and an on_closed callback. The factory mints and returns
// whatever IoHandler drives that connection's state machine — typically a
// BufferedConnection or a protocol dispatcher wrapping one — and registers
// its own read/write interest before returning (see
// BufferedConnection::start()). It must arrange for on_closed to be
// invoked exactly once, whenever that connection is done with itself (the
// same contract as BufferedConnection::set_on_closed) — typically by
// passing it straight through to set_on_closed, or chaining it after the
// factory's own cleanup. Server uses on_closed only to drop its reference
// and update statistics; it takes no other action on the fd itself.
//
// The Server keeps the returned shared_ptr alive in a registry keyed by
// fd from the moment the factory returns until on_closed fires (or, on
// Server::stop(), until IoHandler::shutdown() forces it to).
//
// Returning nullptr declines the connection; Server closes the fd itself
// in that case, so the factory must NOT close it first (that would race
// Server's cleanup against whatever fd number the kernel reassigns in
// between).
//
// A Server has at most one active TCP acceptance strategy: setting a
// ConnectionFactory here takes precedence over set_connection_handler() —
// accepted connections go to whichever was set most recently.
using ConnectionFactory =
    std::function<std::shared_ptr<IoHandler>(int, IOContext&, std::function<void()>)>;

// Reactor-side per-datagram callback: invoked synchronously (no coroutine
// involved) for each UDP datagram received on any port the Server is
// listening on. Runs on whichever IOContext worker thread received the
// packet; like ConnectionFactory, setting this takes precedence over
// set_datagram_handler() for datagrams delivered afterward.
using SyncDatagramHandler = std::function<void(Datagram)>;

// Server class
class Server {
  private:
    std::unique_ptr<IOContext> io_context_;
    ConnectionHandler connection_handler_;
    DatagramHandler datagram_handler_;
    ConnectionFactory connection_factory_;
    SyncDatagramHandler sync_datagram_handler_;
    std::unordered_map<int, std::coroutine_handle<>> socket_handles_;
    // Track active listening sockets so stop()/destructor can close them even if coroutines are
    // currently suspended on accept.
    std::mutex listen_fds_mutex_;
    std::vector<int> listen_fds_;
    // Track active per-connection coroutines so stop() can drain them
    // before the IOContext is torn down. Each handle_connection
    // increments active_connections_ on entry and decrements on exit (RAII
    // guard, fires on every co_return / unwind), and registers the client
    // fd here so stop() can shutdown(2) it and force the suspended
    // recv/send to fail.
    std::mutex client_fds_mutex_;
    std::unordered_set<int> active_client_fds_;
    std::atomic<int> active_connections_{0};
    // Reactor-side counterpart of active_client_fds_: connections minted by
    // connection_factory_, keyed by fd. Server owns the shared_ptr from the
    // point the factory returns it until the connection reports itself
    // closed (erased from here by finish_reactor_connection, invoked as an
    // on_closed-style hook) or until stop() shuts every remaining one down.
    std::mutex reactor_conns_mutex_;
    std::unordered_map<int, std::shared_ptr<IoHandler>> reactor_connections_;
    std::atomic<bool> running_;

    Task handle_connection(Socket socket);
    Task accept_connections(int listen_fd);
    Task receive_udp(int udp_fd);
    // Drops fd from reactor_connections_ (called once the connection is
    // done with itself) and updates the shared active-connection count.
    // Safe to call from any thread; safe to call from inside the very
    // IoHandler callback that's finishing, since it only erases this
    // Server's map entry — the object itself stays alive for the rest of
    // that callback via the shared_ptr the caller already holds.
    void finish_reactor_connection(int fd);

  public:
    // `num_threads` is the number of IO worker threads to spawn (0 = non-threaded default).
    Server(std::size_t num_threads = 0);
    ~Server();

    // Start listening on a port (schedules the listen task)
    void listen_tcp(uint16_t port);
    void listen_udp(uint16_t port);

    // ---- Low-level callbacks (preferred). ----
    // set_connection_handler is invoked once per accepted TCP
    // connection.  set_datagram_handler is invoked once per received
    // UDP datagram.  Examples under example/<protocol>/ provide
    // factory helpers (e.g. spaznet::http::make_dispatcher) that
    // build these callbacks from higher-level handler interfaces.
    void set_connection_handler(ConnectionHandler handler);
    void set_datagram_handler(DatagramHandler handler);

    // ---- Reactor-side callbacks. ----
    // set_connection_factory / set_sync_datagram_handler are the
    // coroutine-free counterparts of the two setters above: no Task, no
    // co_await, just an IoHandler and a synchronous callback respectively.
    // See the ConnectionFactory / SyncDatagramHandler typedefs above for
    // the precedence rules against the coroutine-based setters.
    void set_connection_factory(ConnectionFactory factory);
    void set_sync_datagram_handler(SyncDatagramHandler handler);

    // ---- Legacy handler-pattern setters (deprecated). ----
    // These remain as compatibility wrappers around
    // set_connection_handler / set_datagram_handler while the
    // protocol-specific handlers are moved out of the core library.
    // New code should depend on the example/<protocol> libraries
    // and use the low-level setters above instead.

    // Run the server
    void run();

    // Stop the server
    void stop();

    // Get current statistics (lock-free read)
    [[nodiscard]] auto get_statistics() const -> Statistics {
        return io_context_->get_statistics();
    }
};

} // namespace spaznet
