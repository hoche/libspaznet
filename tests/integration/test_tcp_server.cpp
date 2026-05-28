#include <gtest/gtest.h>
#include <chrono>
#include <libspaznet/handlers/http_handler.hpp>
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
        response.reason_phrase = "OK";
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

// Server::stop() must drain in-flight connection coroutines before
// returning so the IOContext isn't torn down with suspended awaiters
// still pointing into it. We open an idle keep-alive connection (server
// suspended on recv), then call stop() and assert it returns inside the
// drain deadline.
TEST_F(TCPServerTest, StopDrainsIdleConnection) {
    server->listen_tcp(9996);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int client = connect_to_server(9996);
    ASSERT_GE(client, 0);

    // Send one full request and read the response so the server-side
    // coroutine has cycled back to async_read for the next request — i.e.,
    // it is now suspended on recv waiting for keep-alive bytes that will
    // never come.
    std::string req = "GET /x HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(send(client, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
    char buf[512]{};
    ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
    ASSERT_GT(n, 0);

    // The server coroutine is now parked on async_read. stop() must
    // shutdown() the client fd, wake the coroutine with an error, and
    // wait for it to unwind. Must return well inside the 1s deadline.
    auto t0 = std::chrono::steady_clock::now();
    server->stop();
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1500);

    close_socket(client);
}
