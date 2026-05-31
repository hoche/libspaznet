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

// Debug trace logging (lock-free per-thread)
void trace_log(const std::string& msg) {
    static thread_local std::ofstream trace_file;
    static thread_local bool initialized = false;

    if (!initialized) {
        std::ostringstream filename;
        filename << "/tmp/websocket_trace_" << std::this_thread::get_id() << ".log";
        trace_file.open(filename.str(), std::ios::app);
        initialized = true;
    }

    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    trace_file << "[" << us << "] " << msg << "\n";
    trace_file.flush(); // Ensure it's written immediately for debugging
}

inline uint32_t rotl(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::array<uint8_t, 20> sha1(const uint8_t* data, std::size_t len) {
    uint64_t total_bits = static_cast<uint64_t>(len) * 8;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() + 8) % 64 != 0) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF));
    }

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) |
                   (msg[chunk + i * 4 + 2] << 8) | (msg[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> digest{};
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
    return digest;
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t triple = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        i += 3;
    }

    if (i < data.size()) {
        uint32_t triple = data[i] << 16;
        if (i + 1 < data.size()) {
            triple |= data[i + 1] << 8;
        }
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        if (i + 1 < data.size()) {
            out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }

    return out;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool header_has_token(const std::string& value, const std::string& token) {
    std::string lower = to_lower(value);
    std::string lower_token = to_lower(token);
    std::istringstream iss(lower);
    std::string part;
    while (std::getline(iss, part, ',')) {
        // trim spaces
        part.erase(part.begin(), std::find_if(part.begin(), part.end(),
                                              [](unsigned char ch) { return !std::isspace(ch); }));
        part.erase(std::find_if(part.rbegin(), part.rend(),
                                [](unsigned char ch) { return !std::isspace(ch); })
                       .base(),
                   part.end());
        if (part == lower_token) {
            return true;
        }
    }
    return false;
}

struct WebSocketHandshakeRequest {
    std::string method;
    std::map<std::string, std::string> headers;
};

std::optional<WebSocketHandshakeRequest> parse_websocket_request(const std::string& request) {
    auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return std::nullopt;
    }

    std::istringstream iss(request.substr(0, header_end));
    std::string line;
    WebSocketHandshakeRequest req;

    if (!std::getline(iss, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::istringstream start_line(line);
    start_line >> req.method;
    if (req.method.empty()) {
        return std::nullopt;
    }

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string name = to_lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                        return !std::isspace(ch);
                    }));
        req.headers[name] = value;
    }

    return req;
}

std::string compute_websocket_accept(const std::string& key) {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = key + kGuid;
    auto digest = sha1(reinterpret_cast<const uint8_t*>(concat.data()), concat.size());
    return base64_encode(std::vector<uint8_t>(digest.begin(), digest.end()));
}

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

Task Socket::send_websocket_message(WebSocketOpcode opcode,
                                    std::span<const std::uint8_t> payload, bool fin) {
    const std::size_t len = payload.size();

    // RFC 6455 §5.2: 2-byte minimum header; +2 bytes for the 16-bit
    // length form (126..65535) or +8 bytes for the 64-bit form.
    std::size_t header_size = 2;
    if (len > 65535) {
        header_size += 8;
    } else if (len > 125) {
        header_size += 2;
    }

    std::vector<std::uint8_t> buf;
    buf.resize(header_size + len);

    buf[0] = static_cast<std::uint8_t>((fin ? 0x80 : 0x00) |
                                       (static_cast<std::uint8_t>(opcode) & 0x0F));

    if (len > 65535) {
        buf[1] = 127; // server-origin frame: mask bit always 0
        buf[2] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(len) >> 56) & 0xFF);
        buf[3] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(len) >> 48) & 0xFF);
        buf[4] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(len) >> 40) & 0xFF);
        buf[5] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(len) >> 32) & 0xFF);
        buf[6] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(len) >> 24) & 0xFF);
        buf[7] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(len) >> 16) & 0xFF);
        buf[8] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(len) >> 8) & 0xFF);
        buf[9] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(len) & 0xFF);
    } else if (len > 125) {
        buf[1] = 126;
        buf[2] = static_cast<std::uint8_t>((len >> 8) & 0xFF);
        buf[3] = static_cast<std::uint8_t>(len & 0xFF);
    } else {
        buf[1] = static_cast<std::uint8_t>(len);
    }

    if (len > 0) {
        std::memcpy(buf.data() + header_size, payload.data(), len);
    }

    co_await async_write(std::move(buf));
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

void Server::set_http2_handler(std::unique_ptr<HTTP2Handler> handler) {
    http2_handler_ = std::move(handler);
}

void Server::set_websocket_handler(std::unique_ptr<WebSocketHandler> handler) {
    websocket_handler_ = std::move(handler);
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

    if (!http2_handler_ && !websocket_handler_) {
        socket.close();
        co_return;
    }

    // RAII guard: track this connection from entry to exit (including
    // exception unwinds and every co_return path below). Server::stop()
    // waits on active_connections_ reaching zero so it can drain
    // in-flight work before the IOContext is destroyed, and walks
    // active_client_fds_ to shutdown() each fd so any suspended
    // recv/send fails out.
    //
    // The release() method exists to close a TOCTOU window: several
    // paths below call `socket.close()` mid-coroutine. Without an
    // explicit release, the fd would be closed but its number would
    // still sit in active_client_fds_ until the guard destructs at
    // co_return — and between those two events the kernel could
    // hand the same number back to a fresh accept(), at which point
    // Server::stop() iterating the set would shutdown(2) the wrong
    // (newly-accepted) socket. We therefore drop the fd from the set
    // BEFORE we close the socket, in every path.
    struct ConnectionGuard {
        Server* server;
        int fd;
        bool released = false;
        ConnectionGuard(Server* s, int f) : server(s), fd(f) {
            {
                std::lock_guard<std::mutex> lock(server->client_fds_mutex_);
                server->active_client_fds_.insert(fd);
            }
            server->active_connections_.fetch_add(1, std::memory_order_acq_rel);
        }
        // Idempotent: safe to call multiple times. Drops the fd from
        // active_client_fds_ and decrements active_connections_ so a
        // subsequent Server::stop() drain doesn't shutdown(2) the fd
        // after it's closed and (possibly) recycled.
        void release() {
            if (released) {
                return;
            }
            released = true;
            {
                std::lock_guard<std::mutex> lock(server->client_fds_mutex_);
                server->active_client_fds_.erase(fd);
            }
            server->active_connections_.fetch_sub(1, std::memory_order_acq_rel);
        }
        ~ConnectionGuard() {
            release();
        }
        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;
        ConnectionGuard(ConnectionGuard&&) = delete;
        ConnectionGuard& operator=(ConnectionGuard&&) = delete;
    };
    ConnectionGuard guard(this, socket.fd());


    try {
        std::vector<uint8_t> buffer;
        co_await socket.async_read(buffer, 2048);

        if (buffer.empty()) {
            guard.release();
            socket.close();
            co_return;
        }

        std::string request_str(buffer.begin(), buffer.end());
        auto ws_request = websocket_handler_ ? parse_websocket_request(request_str)
                                             : std::optional<WebSocketHandshakeRequest>{};
        // If the WebSocket request is incomplete, read more until headers finish or limit hit.
        if (websocket_handler_ && !ws_request) {
            const std::size_t kMaxHandshakeBytes = 8192;
            while (!ws_request && buffer.size() < kMaxHandshakeBytes) {
                std::vector<uint8_t> more_data;
                co_await socket.async_read(more_data, 2048);
                if (more_data.empty()) {
                    break;
                }
                buffer.insert(buffer.end(), more_data.begin(), more_data.end());
                request_str.assign(buffer.begin(), buffer.end());
                ws_request = parse_websocket_request(request_str);
            }
        }
        bool websocket_upgrade = false;

        if (ws_request) {
            const auto& hdrs = ws_request->headers;
            auto upgrade_it = hdrs.find("upgrade");
            auto conn_it = hdrs.find("connection");
            auto key_it = hdrs.find("sec-websocket-key");
            auto version_it = hdrs.find("sec-websocket-version");

            if (upgrade_it != hdrs.end() && conn_it != hdrs.end() && key_it != hdrs.end() &&
                version_it != hdrs.end() && ws_request->method == "GET" &&
                header_has_token(upgrade_it->second, "websocket") &&
                header_has_token(conn_it->second, "upgrade") &&
                to_lower(version_it->second) == "13") {
                websocket_upgrade = true;
            }
        }

        // [HTTP/1.1 keep-alive loop removed — moved to
        // example/http/src/dispatcher.cpp.  Users get plain HTTP via
        // Server::set_connection_handler(
        //     spaznet::http::make_dispatcher(...)).  The WebSocket
        // branch below stays in core until Phase 2b extracts it to
        // example/http-websocket.]
        if (websocket_upgrade && websocket_handler_) {
            auto& hdrs = ws_request->headers;
            std::string client_key = hdrs.at("sec-websocket-key");
            std::string accept_key = compute_websocket_accept(client_key);

            std::ostringstream resp;
            resp << "HTTP/1.1 101 Switching Protocols\r\n";
            resp << "Upgrade: websocket\r\n";
            resp << "Connection: Upgrade\r\n";
            resp << "Sec-WebSocket-Accept: " << accept_key << "\r\n\r\n";
            std::string resp_str = resp.str();
            std::vector<uint8_t> resp_bytes(resp_str.begin(), resp_str.end());
            trace_log("WS: Sending handshake response, fd=" + std::to_string(socket.fd()));
            co_await socket.async_write(std::move(resp_bytes));
            trace_log("WS: Handshake sent, calling on_open, fd=" + std::to_string(socket.fd()));

            co_await websocket_handler_->on_open(socket);
            trace_log("WS: on_open complete, starting frame loop, fd=" +
                      std::to_string(socket.fd()));

            // Per-connection scratch buffer reused by every read_exact call.
            // Without this each read_exact would default-construct a fresh
            // `tmp` vector and async_read would resize it from zero,
            // causing 4 small heap allocations per WS frame (header + ext
            // length + mask + payload). Hoisting it amortizes the capacity
            // grow to the first frame on the connection.
            std::vector<uint8_t> read_chunk;

            // Stash for over-read bytes.  A small WS frame is at most 14
            // bytes of header+mask plus payload; under load the kernel
            // typically has multiple frames pending in the receive
            // queue.  The pre-existing version asked recv() for the
            // EXACT number of bytes it needed at each step (2 header,
            // 4 mask, N payload) — three syscalls per frame, even
            // though one recv with a generous size would have grabbed
            // them all at once.  Buffering an over-read here collapses
            // a frame's worth of syscalls into typically one.
            std::vector<uint8_t> ws_recv_stash;
            std::size_t ws_stash_off = 0;
            constexpr std::size_t kWsRecvHint = 4096;

            auto consume_from_stash =
                [&](std::size_t n, std::vector<uint8_t>& out) -> std::size_t {
                const std::size_t avail = ws_recv_stash.size() - ws_stash_off;
                const std::size_t take = std::min(n, avail);
                out.insert(out.end(), ws_recv_stash.begin() + ws_stash_off,
                           ws_recv_stash.begin() + ws_stash_off + take);
                ws_stash_off += take;
                if (ws_stash_off == ws_recv_stash.size()) {
                    ws_recv_stash.clear();
                    ws_stash_off = 0;
                }
                return take;
            };

            auto read_exact = [&](std::size_t n, std::vector<uint8_t>& out) -> Task {
                // async_read returns an empty buffer only on orderly EOF
                // or a hard error — in either case we stop and let the
                // caller see a short read via out.size() < n. Spurious
                // EAGAIN wakeups are already absorbed inside async_read's
                // own re-await loop, so no sleep_for is needed here.
                out.clear();
                out.reserve(n);
                // First, drain whatever is already stashed from a
                // previous over-read.  This is the fast path for the
                // header/mask reads when the kernel had a full frame
                // ready.
                std::size_t taken = consume_from_stash(n, out);
                while (out.size() < n) {
                    // Ask for at least the remainder, but request more
                    // when the caller's `n` is small so we soak up
                    // header + mask + small payload in one syscall.
                    const std::size_t need = n - out.size();
                    const std::size_t want = std::max(need, kWsRecvHint);
                    co_await socket.async_read(read_chunk, want);
                    if (read_chunk.empty()) {
                        co_return;
                    }
                    // Take only `need` bytes for this call; the rest
                    // (if any) goes into the stash for the next
                    // read_exact.
                    const std::size_t take = std::min(need, read_chunk.size());
                    out.insert(out.end(), read_chunk.begin(),
                               read_chunk.begin() + static_cast<std::ptrdiff_t>(take));
                    if (read_chunk.size() > take) {
                        // Stash the overflow.  We expect ws_recv_stash
                        // to be empty here (the prior pass drained it),
                        // so this is a single append + offset reset.
                        ws_recv_stash.clear();
                        ws_stash_off = 0;
                        ws_recv_stash.insert(
                            ws_recv_stash.end(),
                            read_chunk.begin() + static_cast<std::ptrdiff_t>(take),
                            read_chunk.end());
                    }
                }
                (void)taken;
            };

            auto send_frame = [&](WebSocketOpcode opcode, std::span<const std::uint8_t> payload,
                                  uint16_t close_code = 0) -> Task {
                if (opcode == WebSocketOpcode::Close && close_code != 0) {
                    // Close frame with a 2-byte status code prefix (RFC 6455 §5.5.1).
                    std::vector<std::uint8_t> body;
                    body.reserve(2 + payload.size());
                    body.push_back(static_cast<std::uint8_t>((close_code >> 8) & 0xFF));
                    body.push_back(static_cast<std::uint8_t>(close_code & 0xFF));
                    body.insert(body.end(), payload.begin(), payload.end());
                    co_await socket.send_websocket_message(opcode, body);
                } else {
                    co_await socket.send_websocket_message(opcode, payload);
                }
            };

            bool sent_close = false;
            auto fail_close = [&](uint16_t code) -> Task {
                if (!sent_close) {
                    sent_close = true;
                    co_await send_frame(WebSocketOpcode::Close, {}, code);
                }
            };

            std::vector<uint8_t> message_buffer;
            WebSocketOpcode current_message_opcode = WebSocketOpcode::Continuation;
            bool fragmented = false;

            // Hoisted per-frame scratch. Capacity persists across loop
            // iterations, so a steady-state connection allocates none of
            // these once each has been sized for the largest frame it sees.
            std::vector<uint8_t> header;
            std::vector<uint8_t> ext;
            std::vector<uint8_t> mask_key_buf;
            std::vector<uint8_t> payload;

            while (true) {
                trace_log("WS: Reading frame header, fd=" + std::to_string(socket.fd()));
                co_await read_exact(2, header);
                trace_log("WS: Got header bytes=" + std::to_string(header.size()) +
                          ", fd=" + std::to_string(socket.fd()));
                if (header.size() < 2) {
                    trace_log("WS: Header too short, closing, fd=" + std::to_string(socket.fd()));
                    break;
                }

                bool fin = (header[0] & 0x80) != 0;
                bool rsv1 = (header[0] & 0x40) != 0;
                bool rsv2 = (header[0] & 0x20) != 0;
                bool rsv3 = (header[0] & 0x10) != 0;
                WebSocketOpcode opcode = static_cast<WebSocketOpcode>(header[0] & 0x0F);
                bool masked = (header[1] & 0x80) != 0;
                uint64_t payload_len = header[1] & 0x7F;

                if (rsv1 || rsv2 || rsv3) {
                    co_await fail_close(1002);
                    break;
                }
                if (!masked) {
                    // RFC 6455 §5.1: every client→server frame MUST be masked.
                    co_await fail_close(1002);
                    break;
                }
                // RFC 6455 §5.2: only 0x0..0x2 (data) and 0x8..0xA (control)
                // are defined; everything else is reserved and MUST fail.
                const bool opcode_known =
                    opcode == WebSocketOpcode::Continuation || opcode == WebSocketOpcode::Text ||
                    opcode == WebSocketOpcode::Binary || opcode == WebSocketOpcode::Close ||
                    opcode == WebSocketOpcode::Ping || opcode == WebSocketOpcode::Pong;
                if (!opcode_known) {
                    co_await fail_close(1002);
                    break;
                }

                if (payload_len == 126) {
                    co_await read_exact(2, ext);
                    if (ext.size() != 2) {
                        break;
                    }
                    payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
                    // RFC 6455 §5.2: the 16-bit form must be used only when
                    // length >= 126; otherwise the peer should have used the
                    // 7-bit form.
                    if (payload_len < 126) {
                        co_await fail_close(1002);
                        break;
                    }
                } else if (payload_len == 127) {
                    co_await read_exact(8, ext);
                    if (ext.size() != 8) {
                        break;
                    }
                    payload_len = 0;
                    for (int i = 0; i < 8; ++i) {
                        payload_len = (payload_len << 8) | ext[i];
                    }
                    if (payload_len & (1ULL << 63)) {
                        co_await fail_close(1002);
                        break;
                    }
                    // RFC 6455 §5.2: the 64-bit form must be used only when
                    // length >= 65536.
                    if (payload_len < 65536) {
                        co_await fail_close(1002);
                        break;
                    }
                }

                // Application-level cap: refuse any frame whose declared
                // payload exceeds kMaxPayloadBytes BEFORE allocating, then
                // close with 1009 (message too big). Without this, a peer
                // could declare a payload_len of (2^63 - 1) and trigger an
                // arbitrarily large allocation in the std::vector below.
                if (payload_len > WebSocketFrame::kMaxPayloadBytes) {
                    co_await fail_close(1009);
                    break;
                }

                co_await read_exact(4, mask_key_buf);
                if (mask_key_buf.size() != 4) {
                    break;
                }
                // Promote each byte to uint32_t before shifting — shifting an
                // unsigned-char-promoted-to-int by 24 puts the value bit in
                // the int's sign bit, which is undefined behavior when bit 7
                // of mask_key_buf[0] is set.
                uint32_t masking_key = (static_cast<uint32_t>(mask_key_buf[0]) << 24) |
                                       (static_cast<uint32_t>(mask_key_buf[1]) << 16) |
                                       (static_cast<uint32_t>(mask_key_buf[2]) << 8) |
                                       static_cast<uint32_t>(mask_key_buf[3]);

                // Read directly into `payload` (resized in place, capacity
                // preserved across frames) and unmask in place. The previous
                // version allocated a separate payload_buf, read into that,
                // then copied + XORed into a freshly-default-constructed
                // `payload` of equal size — two allocations and a full copy
                // per frame that we can collapse to one allocation and
                // one in-place pass.
                co_await read_exact(static_cast<std::size_t>(payload_len), payload);
                if (payload.size() != payload_len) {
                    break;
                }
                for (std::size_t i = 0; i < payload_len; ++i) {
                    payload[i] ^= ((masking_key >> ((3 - (i % 4)) * 8)) & 0xFF);
                }

                bool is_control = opcode == WebSocketOpcode::Close ||
                                  opcode == WebSocketOpcode::Ping ||
                                  opcode == WebSocketOpcode::Pong;
                if (is_control) {
                    if (!fin || payload_len > 125) {
                        co_await fail_close(1002);
                        break;
                    }
                    if (opcode == WebSocketOpcode::Close) {
                        if (!sent_close) {
                            sent_close = true;
                            // Echo the peer's close payload back unmodified
                            // (this preserves the code + reason if present).
                            co_await socket.send_websocket_message(WebSocketOpcode::Close, payload);
                        }
                        break;
                    } else if (opcode == WebSocketOpcode::Ping) {
                        co_await send_frame(WebSocketOpcode::Pong, payload);
                        continue;
                    } else if (opcode == WebSocketOpcode::Pong) {
                        continue;
                    }
                } else {
                    if (opcode != WebSocketOpcode::Continuation) {
                        if (fragmented) {
                            co_await fail_close(1002);
                            break;
                        }
                        current_message_opcode = opcode;
                        // Swap the freshly-read payload into message_buffer
                        // instead of copy-assigning. After the swap, `payload`
                        // holds message_buffer's previous storage (empty,
                        // but with retained capacity from prior frames) so
                        // the next read_exact reuses it.
                        std::swap(message_buffer, payload);
                        fragmented = !fin;
                    } else {
                        if (!fragmented) {
                            co_await fail_close(1002);
                            break;
                        }
                        message_buffer.insert(message_buffer.end(), payload.begin(), payload.end());
                    }

                    if (fin) {
                        // Message is complete (either single frame or last
                        // fragment). Swap message_buffer into msg.data so
                        // the handler sees the assembled message without a
                        // copy. Dispatch through the rvalue overload of
                        // handle_message so a handler that wants to move
                        // m.data into its response can do so; the default
                        // overload forwards to the const& form for legacy
                        // handlers. On return we swap back and reclaim
                        // the capacity for the next frame (no-op if the
                        // handler moved out — that's fine, payload-sized
                        // alloc next frame either way).
                        WebSocketMessage msg;
                        msg.opcode = current_message_opcode;
                        std::swap(msg.data, message_buffer);
                        fragmented = false;
                        co_await websocket_handler_->handle_message(std::move(msg), socket);
                        std::swap(message_buffer, msg.data);
                        message_buffer.clear();
                    }
                }
            }

            co_await websocket_handler_->on_close(socket);
        }
    } catch (...) {
        // Catch any exceptions to prevent coroutine crashes
    }

    // Drop the fd from active_client_fds_ before closing the socket —
    // see the TOCTOU note on ConnectionGuard above.
    guard.release();
    socket.close();
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
