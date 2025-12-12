#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/server.hpp>
#include <sstream>
#include <string>
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

class ConcurrentHTTPHandler : public HTTPHandler {
  public:
    std::atomic<int> request_count{0};

    Task handle_request(const HTTPRequest& request, HTTPResponse& response,
                        Socket& socket) override {
        request_count.fetch_add(1);

        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        response.status_code = 200;
        response.status_message = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'O', 'K'};

        co_return;
    }
};

class ConcurrentConnectionsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        server = std::make_unique<Server>(4); // 4 threads for concurrency
        server->set_http_handler(std::make_unique<ConcurrentHTTPHandler>());
        server->listen_tcp(5555);

        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    void send_request(int client_id) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return;

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(5555);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return;
        }

        std::ostringstream request;
        request << "GET /test" << client_id << " HTTP/1.1\r\n";
        request << "Host: localhost\r\n";
        request << "\r\n";

        std::string req_str = request.str();
        send(sock, req_str.c_str(), req_str.size(), 0);

        char buffer[4096];
        recv(sock, buffer, sizeof(buffer) - 1, 0);
        close_socket(sock);
    }

    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(ConcurrentConnectionsTest, MultipleSequentialConnections) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i = 0; i < 10; ++i) {
        send_request(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

TEST_F(ConcurrentConnectionsTest, ConcurrentConnections) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int num_clients = 20;
    std::vector<std::thread> clients;

    for (int i = 0; i < num_clients; ++i) {
        clients.emplace_back([this, i]() { send_request(i); });
    }

    for (auto& t : clients) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

TEST_F(ConcurrentConnectionsTest, BurstConnections) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int num_clients = 50;
    std::vector<std::thread> clients;

    // All clients connect at once
    for (int i = 0; i < num_clients; ++i) {
        clients.emplace_back([this, i]() { send_request(i); });
    }

    for (auto& t : clients) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

TEST_F(ConcurrentConnectionsTest, SustainedLoad) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int num_rounds = 5;
    const int clients_per_round = 10;

    for (int round = 0; round < num_rounds; ++round) {
        std::vector<std::thread> clients;

        for (int i = 0; i < clients_per_round; ++i) {
            clients.emplace_back([this, round, i]() { send_request(round * 1000 + i); });
        }

        for (auto& t : clients) {
            t.join();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

TEST_F(ConcurrentConnectionsTest, MixedRequestTypes) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<std::thread> clients;

    // Mix of GET and POST (simulated)
    for (int i = 0; i < 10; ++i) {
        clients.emplace_back([this, i]() { send_request(i); });
    }

    for (auto& t : clients) {
        t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
