#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <libspaznet/io_context.hpp>
#include <libspaznet/platform_io.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Server/public API header: suppress noisy style checks.
// NOLINTBEGIN

namespace spaznet {

// Forward declarations
class UDPHandler;
#ifdef SPAZNET_HAS_QUIC
namespace http3 {
class QuicHttp3Service;
}
#endif

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

    // Async read
    Task async_read(std::vector<uint8_t>& buffer, std::size_t size);

    // Async write
    Task async_write(std::vector<uint8_t> data);

    void close();
};

} // namespace spaznet

// NOLINTEND

// Include handlers after Socket is defined
#include <libspaznet/handlers/udp_handler.hpp>

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

// Server class
class Server {
  private:
    std::unique_ptr<IOContext> io_context_;
    ConnectionHandler connection_handler_;
    DatagramHandler datagram_handler_;
    std::unique_ptr<UDPHandler> udp_handler_;
#ifdef SPAZNET_HAS_QUIC
    std::unique_ptr<http3::QuicHttp3Service> quic_http3_service_;
#endif

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
    std::atomic<bool> running_;

    Task handle_connection(Socket socket);
    Task accept_connections(int listen_fd);
    Task receive_udp(int udp_fd);

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

    // ---- Legacy handler-pattern setters (deprecated). ----
    // These remain as compatibility wrappers around
    // set_connection_handler / set_datagram_handler while the
    // protocol-specific handlers are moved out of the core library.
    // New code should depend on the example/<protocol> libraries
    // and use the low-level setters above instead.
    void set_udp_handler(std::unique_ptr<UDPHandler> handler);
#ifdef SPAZNET_HAS_QUIC
    // QUIC v1 + HTTP/3 entry point. The service object owns the
    // Listener + per-connection Http3Server instances; the Server just
    // routes UDP datagrams to it and drives its timer.  Only available
    // when libspaznet was built with -DSPAZNET_BUILD_QUIC=ON (default;
    // requires OpenSSL 3.5+).
    void set_quic_http3_service(std::unique_ptr<http3::QuicHttp3Service> service);
#endif

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
