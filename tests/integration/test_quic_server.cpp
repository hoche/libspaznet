#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/handlers/quic_handler.hpp>
#include <libspaznet/server.hpp>
#include <memory>
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

int create_udp_socket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    return fd;
}

bool send_udp(int fd, const std::vector<uint8_t>& data, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ssize_t sent =
        sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<ssize_t>(data.size());
}

std::vector<uint8_t> recv_udp(int fd, size_t max_size) {
    std::vector<uint8_t> buffer(max_size);
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);

    ssize_t received = recvfrom(fd, buffer.data(), buffer.size(), 0,
                                reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (received > 0) {
        buffer.resize(static_cast<size_t>(received));
        return buffer;
    }
    return {};
}

} // namespace

class TestQUICHandler : public QUICHandler {
  public:
    std::atomic<int> connection_count{0};
    std::atomic<int> stream_data_count{0};

    Task on_connection(std::shared_ptr<QUICConnection> connection) override {
        connection_count.fetch_add(1);
        co_return;
    }

    Task on_stream_data(std::shared_ptr<QUICConnection> connection,
                        std::shared_ptr<QUICStream> stream, const std::vector<uint8_t>& data,
                        bool fin) override {
        stream_data_count.fetch_add(1);
        co_return;
    }
};

class QUICServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        handler = std::make_unique<TestQUICHandler>();
        server = std::make_unique<Server>(2);
        server->set_quic_handler(std::make_unique<TestQUICHandler>());
        port = 8888;
        server->listen_udp(port);

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
    std::unique_ptr<TestQUICHandler> handler;
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(QUICServerTest, ServerStarts) {
    EXPECT_NE(server, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(QUICServerTest, ReceivesUDPPacket) {
    int client_fd = create_udp_socket();
    ASSERT_GE(client_fd, 0);

    std::vector<uint8_t> test_data = {0xC0, 0x00, 0x00, 0x00, 0x01}; // Simple QUIC-like packet
    ASSERT_TRUE(send_udp(client_fd, test_data, port));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close_socket(client_fd);
}
