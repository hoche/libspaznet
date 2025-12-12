#include <gtest/gtest.h>
#include <chrono>
#include <libspaznet/http_handler.hpp>
#include <libspaznet/server.hpp>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#define inet_addr(x) inet_addr(x)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define close_socket ::close
#endif

using namespace spaznet;

// Simple test HTTP handler
class TestHTTPHandler : public HTTPHandler {
  public:
    Task handle_request(const HTTPRequest& request, HTTPResponse& response,
                        Socket& socket) override {
        response.status_code = 200;
        response.status_message = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'O', 'K'};
        co_return;
    }
};

class TCPServerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        server = std::make_unique<Server>(2);
        // Set up a simple handler to handle connections
        server->set_http_handler(std::make_unique<TestHTTPHandler>());
        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    int connect_to_server(uint16_t port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return -1;

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return -1;
        }

        return sock;
    }

    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(TCPServerTest, ServerStartup) {
    // Server should start without errors
    EXPECT_NE(server, nullptr);
}

TEST_F(TCPServerTest, ListenOnPort) {
    EXPECT_NO_THROW(server->listen_tcp(9999));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Try to connect
    int client = connect_to_server(9999);
    if (client >= 0) {
        close_socket(client);
    }
    // Connection may succeed or fail depending on handler, but shouldn't crash
}

TEST_F(TCPServerTest, MultiplePorts) {
    EXPECT_NO_THROW(server->listen_tcp(9998));
    EXPECT_NO_THROW(server->listen_tcp(9997));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Both ports should be listening
    int client1 = connect_to_server(9998);
    int client2 = connect_to_server(9997);

    if (client1 >= 0)
        close_socket(client1);
    if (client2 >= 0)
        close_socket(client2);
}

TEST_F(TCPServerTest, ServerShutdown) {
    EXPECT_NO_THROW(server->stop());
}
