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

std::vector<uint8_t> build_quic_stream_packet(uint64_t stream_id, const std::vector<uint8_t>& data,
                                              bool fin) {
    ConnectionID dest;
    dest.bytes = {0x01, 0x02, 0x03, 0x04};
    ConnectionID src;
    src.bytes = {0x05, 0x06, 0x07, 0x08};
    auto conn = std::make_shared<QUICConnection>(dest, src);

    QUICStreamFrame frame;
    frame.stream_id = stream_id;
    frame.offset = 0;
    frame.data = data;
    frame.fin = fin;

    return conn->build_packet(QUICPacketType::Initial, {frame});
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
        auto handler_unique = std::make_unique<TestQUICHandler>();
        handler = handler_unique.get();
        server = std::make_unique<Server>(2);
        server->set_quic_handler(std::move(handler_unique));
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
    TestQUICHandler* handler{};
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

    std::vector<uint8_t> test_data =
        build_quic_stream_packet(0, std::vector<uint8_t>{'p', 'i', 'n', 'g'}, true);

    ASSERT_TRUE(send_udp(client_fd, test_data, port));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_NE(handler, nullptr);
    EXPECT_GE(handler->connection_count.load(), 1);
    EXPECT_GE(handler->stream_data_count.load(), 1);

    close_socket(client_fd);
}

TEST_F(QUICServerTest, ConnectionCallbackOncePerRemote) {
    int client_fd = create_udp_socket();
    ASSERT_GE(client_fd, 0);

    // Send two packets from the same socket/remote endpoint.
    ASSERT_TRUE(send_udp(
        client_fd, build_quic_stream_packet(0, std::vector<uint8_t>{'a'}, /*fin=*/true), port));
    ASSERT_TRUE(send_udp(
        client_fd, build_quic_stream_packet(4, std::vector<uint8_t>{'b'}, /*fin=*/true), port));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->connection_count.load(), 1);
    EXPECT_GE(handler->stream_data_count.load(), 2);

    close_socket(client_fd);
}

TEST_F(QUICServerTest, ConnectionCallbackPerRemote) {
    int client_fd1 = create_udp_socket();
    int client_fd2 = create_udp_socket();
    ASSERT_GE(client_fd1, 0);
    ASSERT_GE(client_fd2, 0);

    // Send one packet from each socket (different local ports => different remote endpoints).
    ASSERT_TRUE(send_udp(
        client_fd1, build_quic_stream_packet(0, std::vector<uint8_t>{'x'}, /*fin=*/true), port));
    ASSERT_TRUE(send_udp(
        client_fd2, build_quic_stream_packet(0, std::vector<uint8_t>{'y'}, /*fin=*/true), port));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_NE(handler, nullptr);
    EXPECT_GE(handler->connection_count.load(), 2);
    EXPECT_GE(handler->stream_data_count.load(), 2);

    close_socket(client_fd1);
    close_socket(client_fd2);
}
