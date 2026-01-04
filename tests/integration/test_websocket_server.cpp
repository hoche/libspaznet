#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <libspaznet/handlers/websocket_handler.hpp>
#include <libspaznet/server.hpp>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#define close_socket ::close
#endif

using namespace spaznet;

namespace {

int connect_client(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return -1;
    }
    return fd;
}

bool send_all(int fd, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        ssize_t n = send(fd, reinterpret_cast<const char*>(data.data() + sent),
                         static_cast<int>(data.size() - sent), 0);
#else
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_exact(int fd, std::vector<uint8_t>& out, size_t n) {
    out.clear();
    out.reserve(n);
    size_t total_received = 0;
    const int max_attempts = 50;
    int attempts = 0;

    while (total_received < n && attempts < max_attempts) {
        uint8_t buf[256];
        size_t want = std::min(n - total_received, sizeof(buf));

#ifdef _WIN32
        ssize_t r = recv(fd, reinterpret_cast<char*>(buf), static_cast<int>(want), 0);
#else
        ssize_t r = recv(fd, buf, want, 0);
#endif
        if (r > 0) {
            out.insert(out.end(), buf, buf + r);
            total_received += static_cast<size_t>(r);
            attempts = 0; // Reset attempts on successful read
        } else if (r == 0) {
            // Connection closed
            return false;
        } else {
            // Error - wait and retry (might be EAGAIN on non-blocking socket or async delay)
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
                attempts++;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            // Other error
            return false;
        }
    }

    return total_received == n;
}

std::vector<uint8_t> make_masked_frame(WebSocketOpcode opcode, const std::vector<uint8_t>& payload,
                                       uint32_t masking_key) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.rsv1 = frame.rsv2 = frame.rsv3 = false;
    frame.opcode = opcode;
    frame.masked = true;
    frame.masking_key = masking_key;
    frame.payload = payload;
    frame.payload_length = payload.size();
    return frame.serialize();
}

WebSocketFrame read_frame(int fd) {
    std::vector<uint8_t> header;
    if (!recv_exact(fd, header, 2)) {
        throw std::runtime_error("failed to read header");
    }
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
        std::vector<uint8_t> ext;
        if (!recv_exact(fd, ext, 2)) {
            throw std::runtime_error("failed ext");
        }
        len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
        std::vector<uint8_t> ext;
        if (!recv_exact(fd, ext, 8)) {
            throw std::runtime_error("failed ext");
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | ext[i];
        }
    }
    uint32_t mask = 0;
    if (masked) {
        std::vector<uint8_t> mask_bytes;
        if (!recv_exact(fd, mask_bytes, 4)) {
            throw std::runtime_error("failed mask");
        }
        mask = (mask_bytes[0] << 24) | (mask_bytes[1] << 16) | (mask_bytes[2] << 8) | mask_bytes[3];
    }
    std::vector<uint8_t> payload;
    if (len > 0) {
        if (!recv_exact(fd, payload, static_cast<size_t>(len))) {
            throw std::runtime_error("failed payload");
        }
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= (mask >> ((3 - (i % 4)) * 8)) & 0xFF;
            }
        }
    }
    WebSocketFrame frame;
    frame.fin = (header[0] & 0x80) != 0;
    frame.rsv1 = (header[0] & 0x40) != 0;
    frame.rsv2 = (header[0] & 0x20) != 0;
    frame.rsv3 = (header[0] & 0x10) != 0;
    frame.opcode = static_cast<WebSocketOpcode>(header[0] & 0x0F);
    frame.masked = masked;
    frame.payload_length = payload.size();
    frame.payload = payload;
    frame.masking_key = mask;
    return frame;
}

std::string handshake_request(const std::string& key) {
    std::ostringstream oss;
    oss << "GET /chat HTTP/1.1\r\n";
    oss << "Host: localhost\r\n";
    oss << "Upgrade: websocket\r\n";
    oss << "Connection: Upgrade\r\n";
    oss << "Sec-WebSocket-Key: " << key << "\r\n";
    oss << "Sec-WebSocket-Version: 13\r\n\r\n";
    return oss.str();
}

} // namespace

class EchoWebSocketHandler : public WebSocketHandler {
  public:
    std::atomic<int> open_count{0};
    std::atomic<int> close_count{0};
    Task on_open(Socket& socket) override {
        open_count.fetch_add(1);
        co_return;
    }
    Task handle_message(const WebSocketMessage& message, Socket& socket) override {
        WebSocketFrame frame;
        frame.fin = true;
        frame.rsv1 = frame.rsv2 = frame.rsv3 = false;
        frame.opcode = message.opcode;
        frame.masked = false;
        frame.payload = message.data;
        frame.payload_length = frame.payload.size();
        auto bytes = frame.serialize();
        co_await socket.async_write(std::move(bytes));
    }
    Task on_close(Socket& socket) override {
        close_count.fetch_add(1);
        co_return;
    }
};

class WebSocketServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        handler = std::make_unique<EchoWebSocketHandler>();
        server = std::make_unique<Server>(2);
        server->set_websocket_handler(std::make_unique<EchoWebSocketHandler>());
        port = 7877;
        server->listen_tcp(port);
        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    uint16_t port{};
    std::unique_ptr<EchoWebSocketHandler> handler;
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(WebSocketServerTest, PerformsRFC6455Handshake) {
    int fd = connect_client(port);
    ASSERT_GE(fd, 0);
    std::string key = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC6455 example key
    auto req = handshake_request(key);
    ASSERT_TRUE(send_all(fd, std::vector<uint8_t>(req.begin(), req.end())));

    char resp[512]{};
    ssize_t r = recv(fd, resp, sizeof(resp) - 1, 0);
    ASSERT_GT(r, 0);
    std::string resp_str(resp, resp + r);
    EXPECT_NE(resp_str.find("101 Switching Protocols"), std::string::npos);
    EXPECT_NE(resp_str.find("Upgrade: websocket"), std::string::npos);
    EXPECT_NE(resp_str.find("Connection: Upgrade"), std::string::npos);
    EXPECT_NE(resp_str.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
    close_socket(fd);
}

TEST_F(WebSocketServerTest, EchoesMaskedTextFrame) {
    int fd = connect_client(port);
    ASSERT_GE(fd, 0);
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    auto req = handshake_request(key);
    ASSERT_TRUE(send_all(fd, std::vector<uint8_t>(req.begin(), req.end())));

    // Wait for handshake response
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    char resp[256]{};
    recv(fd, resp, sizeof(resp), 0); // ignore handshake body

    std::string message = "hello-rfc6455";
    auto frame = make_masked_frame(
        WebSocketOpcode::Text, std::vector<uint8_t>(message.begin(), message.end()), 0x11223344);
    ASSERT_TRUE(send_all(fd, frame));

    // Wait for echo
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto echoed = read_frame(fd);
    EXPECT_EQ(echoed.opcode, WebSocketOpcode::Text);
    std::string echoed_text(echoed.payload.begin(), echoed.payload.end());
    EXPECT_EQ(echoed_text, message);
    close_socket(fd);
}

TEST_F(WebSocketServerTest, RespondsToPingWithPong) {
    int fd = connect_client(port);
    ASSERT_GE(fd, 0);
    auto req = handshake_request("dGhlIHNhbXBsZSBub25jZQ==");
    ASSERT_TRUE(send_all(fd, std::vector<uint8_t>(req.begin(), req.end())));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char resp[256]{};
    recv(fd, resp, sizeof(resp), 0);

    std::vector<uint8_t> payload = {'p', 'i', 'n', 'g'};
    auto ping = make_masked_frame(WebSocketOpcode::Ping, payload, 0xA1B2C3D4);
    ASSERT_TRUE(send_all(fd, ping));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto frame = read_frame(fd);
    EXPECT_EQ(frame.opcode, WebSocketOpcode::Pong);
    EXPECT_EQ(frame.payload, payload);
    close_socket(fd);
}

TEST_F(WebSocketServerTest, HandlesFragmentedMessage) {
    int fd = connect_client(port);
    ASSERT_GE(fd, 0);
    auto req = handshake_request("dGhlIHNhbXBsZSBub25jZQ==");
    ASSERT_TRUE(send_all(fd, std::vector<uint8_t>(req.begin(), req.end())));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char resp[256]{};
    recv(fd, resp, sizeof(resp), 0);

    std::string part1 = "hello ";
    std::string part2 = "fragmented world";

    // First fragment (FIN = 0)
    WebSocketFrame f1;
    f1.fin = false;
    f1.rsv1 = f1.rsv2 = f1.rsv3 = false;
    f1.opcode = WebSocketOpcode::Text;
    f1.masked = true;
    f1.masking_key = 0xCAFEBABE;
    f1.payload.assign(part1.begin(), part1.end());
    f1.payload_length = f1.payload.size();
    auto bytes1 = f1.serialize();

    // Continuation (FIN = 1)
    WebSocketFrame f2;
    f2.fin = true;
    f2.rsv1 = f2.rsv2 = f2.rsv3 = false;
    f2.opcode = WebSocketOpcode::Continuation;
    f2.masked = true;
    f2.masking_key = 0xCAFEBABE;
    f2.payload.assign(part2.begin(), part2.end());
    f2.payload_length = f2.payload.size();
    auto bytes2 = f2.serialize();

    ASSERT_TRUE(send_all(fd, bytes1));
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Small delay between frames
    ASSERT_TRUE(send_all(fd, bytes2));

    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Wait for server to process

    auto echoed = read_frame(fd);
    EXPECT_EQ(echoed.opcode, WebSocketOpcode::Text);
    std::string combined(echoed.payload.begin(), echoed.payload.end());
    EXPECT_EQ(combined, part1 + part2);
    close_socket(fd);
}

TEST_F(WebSocketServerTest, RejectsUnmaskedClientFrame) {
    int fd = connect_client(port);
    ASSERT_GE(fd, 0);
    auto req = handshake_request("dGhlIHNhbXBsZSBub25jZQ==");
    ASSERT_TRUE(send_all(fd, std::vector<uint8_t>(req.begin(), req.end())));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char resp[256]{};
    recv(fd, resp, sizeof(resp), 0);

    // Send unmasked frame (protocol error)
    WebSocketFrame bad;
    bad.fin = true;
    bad.rsv1 = bad.rsv2 = bad.rsv3 = false;
    bad.opcode = WebSocketOpcode::Text;
    bad.masked = false;
    bad.payload = {'b', 'a', 'd'};
    bad.payload_length = bad.payload.size();
    auto bytes = bad.serialize();
    ASSERT_TRUE(send_all(fd, bytes));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Server should respond with Close
    auto close_frame = read_frame(fd);
    EXPECT_EQ(close_frame.opcode, WebSocketOpcode::Close);
    close_socket(fd);
}
