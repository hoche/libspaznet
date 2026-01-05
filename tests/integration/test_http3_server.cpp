#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/handlers/http3_handler.hpp>
#include <libspaznet/handlers/quic_handler.hpp>
#include <libspaznet/server.hpp>
#include <memory>
#include <thread>
#include <vector>

using namespace spaznet;

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

namespace {

int create_udp_socket() {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    return fd;
}

bool send_udp(int fd, const std::vector<uint8_t>& data, uint16_t port) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_loopback;

    ssize_t sent =
        sendto(fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<ssize_t>(data.size());
}

std::vector<uint8_t> recv_udp(int fd, size_t max_size) {
    std::vector<uint8_t> buffer(max_size);
    sockaddr_in6 addr{};
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

class TestHTTP3Handler : public HTTP3Handler {
  public:
    std::atomic<int> request_count{0};

    Task handle_request(const HTTP3Request& request, HTTP3Response& response,
                        std::shared_ptr<QUICStream> stream) override {
        request_count.fetch_add(1);

        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body.assign(request.method.begin(), request.method.end());

        co_return;
    }
};

class HTTP3ServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto handler_unique = std::make_unique<TestHTTP3Handler>();
        handler = handler_unique.get();

        server = std::make_unique<Server>(2);
        server->set_http3_handler(std::move(handler_unique));
        port = 9999;
        server->listen_udp(port);

        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    uint16_t port{};
    TestHTTP3Handler* handler{};
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(HTTP3ServerTest, HandlerCreation) {
    EXPECT_NE(handler, nullptr);
}

TEST_F(HTTP3ServerTest, RequestStructure) {
    HTTP3Request request;
    request.method = "GET";
    request.request_target = "/";
    request.scheme = "https";
    request.authority = "example.com";

    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.request_target, "/");
    EXPECT_EQ(request.scheme, "https");
    EXPECT_EQ(request.authority, "example.com");
}

TEST_F(HTTP3ServerTest, ResponseStructure) {
    HTTP3Response response;
    response.status_code = 200;
    response.reason_phrase = "OK";
    response.set_header("Content-Type", "text/plain");
    response.body = {'H', 'e', 'l', 'l', 'o'};

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.reason_phrase, "OK");
    EXPECT_FALSE(response.body.empty());
}

TEST_F(HTTP3ServerTest, EndToEndRequestResponse) {
    int client_fd = create_udp_socket();
    ASSERT_GE(client_fd, 0);

#ifndef _WIN32
    // Timeout so the test won't hang.
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Toy HTTP/3 HEADERS frame:
    // - first byte is frame type (1 = Headers)
    // - payload is text; first line is "METHOD PATH SCHEME AUTHORITY"
    std::vector<uint8_t> h3;
    h3.push_back(0x01); // HEADERS frame type
    const std::string headers = "GET / https localhost\r\n"
                                "user-agent: test\r\n"
                                "\r\n";
    h3.insert(h3.end(), headers.begin(), headers.end());

    ConnectionID dest;
    dest.bytes = {0x01, 0x02, 0x03, 0x04};
    ConnectionID src;
    src.bytes = {0x05, 0x06, 0x07, 0x08};
    auto conn = std::make_shared<QUICConnection>(dest, src);
    QUICStreamFrame frame;
    frame.stream_id = 0;
    frame.offset = 0;
    frame.data = std::move(h3);
    frame.fin = true;

    std::vector<uint8_t> packet = conn->build_packet(QUICPacketType::Initial, {frame});
    ASSERT_TRUE(send_udp(client_fd, packet, port));

    // Give server time to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_NE(handler, nullptr);
    EXPECT_GE(handler->request_count.load(), 1);

    auto resp = recv_udp(client_fd, 64 * 1024);
    close_socket(client_fd);

    ASSERT_FALSE(resp.empty());
    std::string resp_str(resp.begin(), resp.end());
    EXPECT_NE(resp_str.find(":status 200"), std::string::npos);
    EXPECT_NE(resp_str.find("GET"), std::string::npos); // body echoes method in handler

    EXPECT_GE(handler->request_count.load(), 1);
}
