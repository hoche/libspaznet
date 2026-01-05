#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <libspaznet/handlers/quic_server.hpp>
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
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define close_socket ::close
#endif

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
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
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
                // Try reading again after resume - data should be available
                // A few retries handle spurious wakeups and data arriving in chunks
                for (int i = 0; i < 3; ++i) {
                    result = recv(socket->fd(), buffer->data(), size, 0);
                    trace_log("async_read fd=" + std::to_string(socket->fd()) + " attempt=" +
                              std::to_string(i) + " result=" + std::to_string(result) +
                              " errno=" + std::to_string(errno));
                    if (result >= 0 ||
                        (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                        break;
                    }
                    // Brief delay for data to arrive
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            }
            if (result > 0) {
                buffer->resize(result);
            } else {
                // Treat zero (connection closed) or negative (errors) as no data read.
                buffer->clear();
            }
            trace_log("async_read fd=" + std::to_string(socket->fd()) +
                      " final_result=" + std::to_string(result));
            return result;
        }
    };

    ReadAwaiter awaiter{this, &buffer, size};
    ssize_t result = co_await awaiter;

    if (result < 0) {
        buffer.clear();
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
            mutable bool ready = false;

            bool await_ready() const noexcept {
                // Try to write immediately
                result = send(socket->fd(), data_ptr, remaining, MSG_NOSIGNAL);
                if (result >= 0) {
                    ready = true;
                    return true; // Wrote some data
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
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
                    // Try writing again after resume - socket should be writable
                    for (int i = 0; i < 3; ++i) {
                        result = send(socket->fd(), data_ptr, remaining, MSG_NOSIGNAL);
                        if (result >= 0 ||
                            (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                            break;
                        }
                        // Brief delay for socket to become writable
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                    }
                }
                // On error or connection closed, report zero to break the write loop gracefully.
                if (result < 0) {
                    return 0;
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

void Server::set_quic_handler(std::unique_ptr<QUICHandler> handler) {
    quic_handler_ = std::move(handler);
}

void Server::set_http3_handler(std::unique_ptr<HTTP3Handler> handler) {
    http3_handler_ = std::move(handler);
}

void Server::listen_tcp(uint16_t port) {
    // Use getaddrinfo for IPv4/IPv6 compatibility
    struct addrinfo hints {
    }, *result = nullptr;
    hints.ai_family = AF_INET6; // IPv6 socket (can accept IPv4 via IPv4-mapped addresses)
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // For wildcard bind address

    std::string port_str = std::format("{}", port);
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

    // Listen
    if (listen(listen_fd, SOMAXCONN) < 0) {
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

    std::string port_str = std::format("{}", port);
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
    std::optional<QUICServerEngine> quic_engine;
    if (quic_handler_ || http3_handler_) {
        quic_engine.emplace(quic_handler_.get(), http3_handler_.get());
    }

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

        if (udp_handler_) {
            UDPPacket pkt;
            pkt.data = buffer;
            // Best-effort address/port fill for diagnostics.
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
            pkt.address = host;
            pkt.port = port;
            co_await udp_handler_->handle_packet(pkt, udp_socket);
        }

        if (quic_engine) {
            co_await quic_engine->handle_datagram(udp_fd, addr, addr_len, buffer);
        }
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
        io_context_->platform_io().remove_fd(listen_fd);
        close_socket(listen_fd);
    }
    co_return;
}

Task Server::handle_connection(Socket socket) {
    if (!http_handler_ && !http2_handler_ && !websocket_handler_) {
        socket.close();
        co_return;
    }

    try {
        std::vector<uint8_t> buffer;
        co_await socket.async_read(buffer, 2048);

        if (buffer.empty()) {
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

        if (!websocket_upgrade && http_handler_) {
            constexpr std::size_t kReadChunk = 8192;
            constexpr std::size_t kMaxRequestBytes = 1024 * 1024; // 1 MiB safety cap

            // HTTP/1.1 keep-alive: serve multiple requests per TCP connection.
            while (true) {
                HTTPRequest request;
                size_t bytes_consumed = 0;
                HTTPParser::ParseResult parse_result =
                    HTTPParser::parse_request(buffer, request, bytes_consumed);

                // Read until we have a full request (headers + body) or hit a safety limit.
                while (parse_result == HTTPParser::ParseResult::Incomplete &&
                       buffer.size() < kMaxRequestBytes) {
                    std::vector<uint8_t> more_data;
                    co_await socket.async_read(more_data, kReadChunk);
                    if (more_data.empty()) {
                        // Client closed connection (or read error).
                        socket.close();
                        co_return;
                    }
                    buffer.insert(buffer.end(), more_data.begin(), more_data.end());
                    parse_result = HTTPParser::parse_request(buffer, request, bytes_consumed);
                }

                if (parse_result != HTTPParser::ParseResult::Success) {
                    HTTPResponse error_response;
                    error_response.version = "1.1";
                    error_response.status_code = 400;
                    error_response.reason_phrase = "Bad Request";
                    error_response.set_header("Connection", "close");
                    error_response.set_header("Content-Length", "0");

                    auto error_data = error_response.serialize();
                    co_await socket.async_write(std::move(error_data));
                    socket.close();
                    co_return;
                }

                if (bytes_consumed > buffer.size()) {
                    socket.close();
                    co_return;
                }

                // Consume request bytes so we can parse the next request (pipelined or later).
                buffer.erase(buffer.begin(), buffer.begin() + bytes_consumed);

                // Track request start (lock-free)
                socket.context()->increment_active_requests();

                try {
                    HTTPResponse response;
                    response.version = "1.1";

                    co_await http_handler_->handle_request(request, response, socket);

                    const bool keep_alive = request.should_keep_alive();
                    response.set_header("Connection", keep_alive ? "keep-alive" : "close");

                    std::vector<uint8_t> response_data;
                    auto te = response.get_header("Transfer-Encoding");
                    if (te) {
                        std::string te_lower = *te;
                        std::transform(te_lower.begin(), te_lower.end(), te_lower.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (te_lower.find("chunked") != std::string::npos) {
                            response_data = response.serialize_chunked();
                        } else {
                            response_data = response.serialize();
                        }
                    } else {
                        response_data = response.serialize();
                    }

                    co_await socket.async_write(std::move(response_data));

                    // Track request finish (lock-free) - after response is sent successfully
                    socket.context()->decrement_active_requests();

                    if (!keep_alive) {
                        socket.close();
                        co_return;
                    }
                } catch (...) {
                    // Ensure request counter is decremented even on exception
                    socket.context()->decrement_active_requests();
                    throw;
                }
            }
        } else if (websocket_upgrade && websocket_handler_) {
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

            auto read_exact = [&](std::size_t n, std::vector<uint8_t>& out) -> Task {
                out.clear();
                std::size_t remaining = n;
                int empty_attempts = 0;
                const int max_empty = 20; // Allow retries for small frames arriving slowly
                trace_log("read_exact: want=" + std::to_string(n) +
                          " bytes, fd=" + std::to_string(socket.fd()));
                while (remaining > 0) {
                    std::vector<uint8_t> tmp;
                    co_await socket.async_read(tmp, remaining);
                    trace_log("read_exact: got " + std::to_string(tmp.size()) + " bytes, " +
                              "remaining=" + std::to_string(remaining) +
                              ", fd=" + std::to_string(socket.fd()));
                    if (tmp.empty()) {
                        // Empty read might be transient - retry with backoff
                        if (++empty_attempts > max_empty) {
                            trace_log("read_exact: too many empty reads, giving up, fd=" +
                                      std::to_string(socket.fd()));
                            out.clear();
                            co_return;
                        }
                        // Exponential backoff: 100μs, 200μs, 400μs, 800μs, then 1ms
                        auto delay_us = std::min(100 << std::min(empty_attempts - 1, 3), 1000);
                        trace_log("read_exact: empty read #" + std::to_string(empty_attempts) +
                                  ", delaying " + std::to_string(delay_us) +
                                  "μs, fd=" + std::to_string(socket.fd()));
                        std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
                        continue;
                    }
                    empty_attempts = 0;
                    out.insert(out.end(), tmp.begin(), tmp.end());
                    remaining -= tmp.size();
                }
                trace_log("read_exact: complete, got " + std::to_string(out.size()) +
                          " bytes, fd=" + std::to_string(socket.fd()));
            };

            auto send_frame = [&](WebSocketOpcode opcode, const std::vector<uint8_t>& payload,
                                  uint16_t close_code = 0) -> Task {
                WebSocketFrame frame;
                frame.fin = true;
                frame.rsv1 = frame.rsv2 = frame.rsv3 = false;
                frame.opcode = opcode;
                frame.masked = false;
                frame.payload = payload;
                if (opcode == WebSocketOpcode::Close && close_code != 0) {
                    std::vector<uint8_t> body;
                    body.push_back(static_cast<uint8_t>((close_code >> 8) & 0xFF));
                    body.push_back(static_cast<uint8_t>(close_code & 0xFF));
                    body.insert(body.end(), payload.begin(), payload.end());
                    frame.payload.swap(body);
                }
                frame.payload_length = frame.payload.size();
                auto data = frame.serialize();
                co_await socket.async_write(std::move(data));
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

            while (true) {
                trace_log("WS: Reading frame header, fd=" + std::to_string(socket.fd()));
                std::vector<uint8_t> header;
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
                    co_await fail_close(1002);
                    break;
                }

                if (payload_len == 126) {
                    std::vector<uint8_t> ext;
                    co_await read_exact(2, ext);
                    if (ext.size() != 2) {
                        break;
                    }
                    payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
                } else if (payload_len == 127) {
                    std::vector<uint8_t> ext;
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
                }

                std::vector<uint8_t> mask_key_buf;
                co_await read_exact(4, mask_key_buf);
                if (mask_key_buf.size() != 4) {
                    break;
                }
                uint32_t masking_key = (mask_key_buf[0] << 24) | (mask_key_buf[1] << 16) |
                                       (mask_key_buf[2] << 8) | mask_key_buf[3];

                std::vector<uint8_t> payload(static_cast<std::size_t>(payload_len));
                if (payload_len > 0) {
                    std::vector<uint8_t> payload_buf;
                    co_await read_exact(static_cast<std::size_t>(payload_len), payload_buf);
                    if (payload_buf.size() != payload_len) {
                        break;
                    }
                    for (std::size_t i = 0; i < payload_len; ++i) {
                        payload[i] = payload_buf[i] ^ ((masking_key >> ((3 - (i % 4)) * 8)) & 0xFF);
                    }
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
                            WebSocketFrame close_frame;
                            close_frame.fin = true;
                            close_frame.rsv1 = close_frame.rsv2 = close_frame.rsv3 = false;
                            close_frame.opcode = WebSocketOpcode::Close;
                            close_frame.masked = false;
                            close_frame.payload = payload;
                            close_frame.payload_length = close_frame.payload.size();
                            auto data = close_frame.serialize();
                            co_await socket.async_write(std::move(data));
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
                        message_buffer = payload;
                        fragmented = !fin;
                    } else {
                        if (!fragmented) {
                            co_await fail_close(1002);
                            break;
                        }
                        message_buffer.insert(message_buffer.end(), payload.begin(), payload.end());
                    }

                    if (fin) {
                        // Message is complete (either single frame or last fragment)
                        WebSocketMessage msg;
                        msg.opcode = current_message_opcode;
                        msg.data = message_buffer;
                        fragmented = false;
                        message_buffer.clear();
                        co_await websocket_handler_->handle_message(msg, socket);
                    }
                }
            }

            co_await websocket_handler_->on_close(socket);
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

    // Close all listening sockets. This also releases any accept coroutines suspended on them.
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
        io_context_->platform_io().remove_fd(fd);
        close_socket(fd);
    }
}

} // namespace spaznet
