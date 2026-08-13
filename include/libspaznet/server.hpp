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

#ifdef SPAZNET_HAS_COROUTINES
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
#endif // SPAZNET_HAS_COROUTINES

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

#ifdef SPAZNET_HAS_COROUTINES
// Per-connection callback: the Server invokes this once for each
// accepted TCP connection, handing ownership of the Socket.  The
// connection lives until the Task completes; the Socket destructor
// closes the fd if the handler hasn't already.
using ConnectionHandler = std::function<Task(Socket)>;

// Per-datagram callback: the Server invokes this once for each UDP
// datagram received on any port it's listening on.
using DatagramHandler = std::function<Task(Datagram)>;
#endif // SPAZNET_HAS_COROUTINES

// Reactor-side per-connection callback: invoked once for each accepted TCP
// connection with the fd (already set non-blocking), the IOContext it is
// pinned to for its lifetime (any of Server's loops under accept-and-shard;
// never move the connection to a different IOContext afterward), and an
// on_closed callback. The factory mints and returns whatever IoHandler
// drives that connection's state machine — typically a BufferedConnection
// or a protocol dispatcher wrapping one — and registers its own read/write
// interest on that same IOContext before returning (see
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

// Splits the two axes that `Server(N)` used to conflate. Coroutines want
// workers on one loop; reactors want N independent loops with connections
// pinned at accept (see docs/reactor-threading.md).
//
//   Reactor-only:   loops = N, workers_per_loop = 0
//   Coroutine-only: loops = 1, workers_per_loop = N   (== Server(N) today)
//
// Mixed shapes (loops > 1 and workers_per_loop > 0) are allowed but not
// the first-class target: TCP accept still shards reactor connections
// across loops; coroutine connections stay on loop 0.
struct ServerConfig {
    std::size_t loops = 1;            // independent IOContext instances (>= 1)
    std::size_t workers_per_loop = 0; // coroutine worker threads per loop
};

// Server class
class Server {
  private:
    // Loop 0 owns listen/UDP fds. Additional loops only run accepted TCP
    // reactor connections (accept-and-shard). Coroutine Socket paths always
    // use loop 0 so Server(N) keeps its historical workers-on-one-loop
    // meaning.
    std::vector<std::unique_ptr<IOContext>> io_contexts_;
    std::vector<std::thread> loop_threads_;
    std::atomic<std::size_t> next_accept_loop_{0};
#ifdef SPAZNET_HAS_COROUTINES
    ConnectionHandler connection_handler_;
    DatagramHandler datagram_handler_;
#endif
    ConnectionFactory connection_factory_;
    SyncDatagramHandler sync_datagram_handler_;
    // Track active listening sockets so stop()/destructor can close them even if coroutines are
    // currently suspended on accept.
    std::mutex listen_fds_mutex_;
    std::vector<int> listen_fds_;
#ifdef SPAZNET_HAS_COROUTINES
    // Track active per-connection coroutines so stop() can drain them
    // before the IOContext is torn down. Each handle_connection
    // increments active_connections_ on entry and decrements on exit (RAII
    // guard, fires on every co_return / unwind), and registers the client
    // fd here so stop() can shutdown(2) it and force the suspended
    // recv/send to fail.
    std::mutex client_fds_mutex_;
    std::unordered_set<int> active_client_fds_;
    std::atomic<int> active_connections_{0};
#endif
    // Reactor-side counterpart of active_client_fds_: connections minted by
    // connection_factory_, keyed by fd. Server owns the shared_ptr from the
    // point the factory returns it until the connection reports itself
    // closed (erased from here by finish_reactor_connection, invoked as an
    // on_closed-style hook) or until stop() shuts every remaining one down.
    // `ctx` is the loop the connection was pinned to at accept — stop()
    // must invoke IoHandler::shutdown() on that loop's IO thread.
    struct ReactorConn {
        std::shared_ptr<IoHandler> handler;
        IOContext* ctx = nullptr;
    };
    std::mutex reactor_conns_mutex_;
    std::unordered_map<int, ReactorConn> reactor_connections_;
    std::atomic<bool> running_;

    [[nodiscard]] auto primary_context() -> IOContext& {
        return *io_contexts_.front();
    }
    [[nodiscard]] auto primary_context() const -> const IOContext& {
        return *io_contexts_.front();
    }
    // Round-robin pick among all loops for a newly accepted reactor TCP
    // connection. Listen/UDP stay on primary_context().
    [[nodiscard]] auto pick_accept_loop() -> IOContext&;

#ifdef SPAZNET_HAS_COROUTINES
    Task handle_connection(Socket socket);
#endif
    // Reactor-native accept/datagram loops — always built, no coroutine
    // involved. Each is invoked from a small persistent IoHandler
    // (ListenHandler / DatagramReadHandler, defined in server_impl.cpp)
    // registered on the listening fd; both drain their fd with
    // non-blocking syscalls in a loop until EAGAIN, exactly reproducing
    // what the old accept_connections()/receive_udp() coroutines did
    // between suspension points. Returns true if the fd should have its
    // read interest re-armed (drained to EAGAIN while still running_);
    // false if the caller should NOT re-arm — either stop() flipped
    // running_ off, or (accept_ready only) a hard accept() error caused
    // this listen fd to be closed already.
    bool accept_ready(int listen_fd);
    bool datagram_ready(int udp_fd);
    class ListenHandler;
    class DatagramReadHandler;
    // Drops fd from reactor_connections_ (called once the connection is
    // done with itself) and updates the active-connection count on the
    // loop that owned it. Safe to call from any thread; safe to call from
    // inside the very IoHandler callback that's finishing, since it only
    // erases this Server's map entry — the object itself stays alive for
    // the rest of that callback via the shared_ptr the caller already holds.
    void finish_reactor_connection(int fd, IOContext* ctx);

  public:
    // Historical constructor: one event loop with `num_threads` coroutine
    // worker threads (0 = non-threaded). Equivalent to
    // Server(ServerConfig{.loops = 1, .workers_per_loop = num_threads}).
    // Reactor I/O still runs only on that one loop — use ServerConfig with
    // loops > 1 to scale reactor connections across cores.
    Server(std::size_t num_threads = 0);
    explicit Server(ServerConfig config);
    ~Server();

    // Start listening on a port (schedules the listen task)
    void listen_tcp(uint16_t port);
    void listen_udp(uint16_t port);

    // ---- Low-level callbacks (preferred, coroutine runtime only). ----
    // set_connection_handler is invoked once per accepted TCP
    // connection.  set_datagram_handler is invoked once per received
    // UDP datagram.  Examples under example/<protocol>/ provide
    // factory helpers (e.g. spaznet::http::make_dispatcher) that
    // build these callbacks from higher-level handler interfaces. Only
    // declared when SPAZNET_HAS_COROUTINES is defined — with coroutines
    // disabled, set_connection_factory/set_sync_datagram_handler below are
    // the only acceptance strategy.
#ifdef SPAZNET_HAS_COROUTINES
    void set_connection_handler(ConnectionHandler handler);
    void set_datagram_handler(DatagramHandler handler);
#endif

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

    // Aggregated, lock-free read across every loop's IOContext.
    [[nodiscard]] auto get_statistics() const -> Statistics;

    // Number of independent event loops this Server owns (>= 1).
    [[nodiscard]] auto loop_count() const -> std::size_t {
        return io_contexts_.size();
    }
};

} // namespace spaznet
