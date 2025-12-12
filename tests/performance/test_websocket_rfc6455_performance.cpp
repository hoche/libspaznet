#include <gtest/gtest.h>
#include <chrono>
#include <libspaznet/handlers/websocket_handler.hpp>
#include <libspaznet/server.hpp>
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
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_exact(int fd, std::vector<uint8_t>& out, size_t n) {
    out.clear();
    while (out.size() < n) {
        uint8_t buf[512];
        size_t want = std::min(n - out.size(), sizeof(buf));
        ssize_t r = recv(fd, buf, want, 0);
        if (r <= 0) {
            return false;
        }
        out.insert(out.end(), buf, buf + r);
    }
    return true;
}

std::vector<uint8_t> make_masked_frame(const std::vector<uint8_t>& payload, uint32_t mask) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.rsv1 = frame.rsv2 = frame.rsv3 = false;
    frame.opcode = WebSocketOpcode::Text;
    frame.masked = true;
    frame.masking_key = mask;
    frame.payload = payload;
    frame.payload_length = payload.size();
    return frame.serialize();
}

std::vector<uint8_t> read_frame_payload(int fd) {
    std::vector<uint8_t> header;
    if (!recv_exact(fd, header, 2)) {
        return {};
    }
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
        std::vector<uint8_t> ext;
        if (!recv_exact(fd, ext, 2)) {
            return {};
        }
        len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
        std::vector<uint8_t> ext;
        if (!recv_exact(fd, ext, 8)) {
            return {};
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | ext[i];
        }
    }
    if (masked) {
        std::vector<uint8_t> mask;
        if (!recv_exact(fd, mask, 4)) {
            return {};
        }
    }
    std::vector<uint8_t> payload;
    if (!recv_exact(fd, payload, static_cast<size_t>(len))) {
        return {};
    }
    return payload;
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

class EchoWebSocketHandler : public WebSocketHandler {
  public:
    Task on_open(Socket&) override {
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
        co_await socket.async_write(bytes);
    }
    Task on_close(Socket&) override {
        co_return;
    }
};

} // namespace

TEST(WebSocketPerformance, EchoesHundredsOfFramesQuickly) {
    const uint16_t port = 7999;
    Server server(4);
    server.set_websocket_handler(std::make_unique<EchoWebSocketHandler>());
    server.listen_tcp(port);
    std::thread t([&]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int fd = connect_client(port);
    ASSERT_GE(fd, 0);
    auto req = handshake_request("dGhlIHNhbXBsZSBub25jZQ==");
    ASSERT_TRUE(send_all(fd, std::vector<uint8_t>(req.begin(), req.end())));
    char resp[256]{};
    recv(fd, resp, sizeof(resp), 0);

    const int frame_count = 300;
    const std::vector<uint8_t> payload(64, 'x');
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < frame_count; ++i) {
        uint32_t mask = 0x11111111 ^ static_cast<uint32_t>(i);
        auto frame = make_masked_frame(payload, mask);
        ASSERT_TRUE(send_all(fd, frame));
        auto echoed = read_frame_payload(fd);
        ASSERT_EQ(echoed.size(), payload.size());
    }
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_LT(ms, 2000); // simple performance guard

    close_socket(fd);
    server.stop();
    if (t.joinable()) {
        t.join();
    }
}
