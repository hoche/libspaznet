#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <libspaznet/http_handler.hpp>
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

class ThroughputHTTPHandler : public HTTPHandler {
  public:
    std::atomic<uint64_t> request_count{0};
    std::atomic<uint64_t> total_bytes_sent{0};

    Task handle_request(const HTTPRequest& request, HTTPResponse& response,
                        Socket& socket) override {
        request_count.fetch_add(1);

        response.status_code = 200;
        response.status_message = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'O', 'K'};

        total_bytes_sent.fetch_add(response.body.size() + 100); // Approximate

        co_return;
    }
};

class ThroughputTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Ignore SIGPIPE to prevent crashes when writing to closed sockets
        signal(SIGPIPE, SIG_IGN);

        handler = std::make_unique<ThroughputHTTPHandler>();
        server = std::make_unique<Server>(4);
        server->set_http_handler(std::make_unique<ThroughputHTTPHandler>());
        server->listen_tcp(9000);

        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    void send_request(int sock) {
        std::ostringstream request;
        request << "GET /test HTTP/1.1\r\n";
        request << "Host: localhost\r\n";
        request << "\r\n";

        std::string req_str = request.str();
        // Use MSG_NOSIGNAL to avoid SIGPIPE if socket closes
        send(sock, req_str.c_str(), req_str.size(), MSG_NOSIGNAL);

        char buffer[4096];
        recv(sock, buffer, sizeof(buffer) - 1, 0);
    }

    int create_connection() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return -1;

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(9000);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return -1;
        }

        return sock;
    }

    std::unique_ptr<ThroughputHTTPHandler> handler;
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(ThroughputTest, SingleConnectionThroughput) {
    const int num_requests = 1000;
    auto start = std::chrono::high_resolution_clock::now();

    int sock = create_connection();
    ASSERT_GE(sock, 0);

    for (int i = 0; i < num_requests; ++i) {
        send_request(sock);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    close_socket(sock);

    double requests_per_second = (num_requests * 1000.0) / duration.count();

    std::cout << "\n[PERF] Single Connection Throughput:" << std::endl;
    std::cout << "  Requests: " << num_requests << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << requests_per_second
              << " req/s" << std::endl;

    // Should handle at least 100 req/s
    EXPECT_GT(requests_per_second, 100.0);
}

TEST_F(ThroughputTest, MultipleConnectionsThroughput) {
    const int num_connections = 10;
    const int requests_per_connection = 100;
    const int total_requests = num_connections * requests_per_connection;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < num_connections; ++i) {
        threads.emplace_back([this, requests_per_connection]() {
            int sock = create_connection();
            if (sock >= 0) {
                for (int j = 0; j < requests_per_connection; ++j) {
                    send_request(sock);
                }
                close_socket(sock);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double requests_per_second = (total_requests * 1000.0) / duration.count();

    std::cout << "\n[PERF] Multiple Connections Throughput:" << std::endl;
    std::cout << "  Connections: " << num_connections << std::endl;
    std::cout << "  Total Requests: " << total_requests << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << requests_per_second
              << " req/s" << std::endl;

    // Should handle at least 500 req/s with multiple connections
    EXPECT_GT(requests_per_second, 500.0);
}

TEST_F(ThroughputTest, SustainedLoadThroughput) {
    const int duration_seconds = 5;
    const int num_workers = 20;

    std::atomic<uint64_t> total_requests{0};
    std::atomic<bool> running{true};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> workers;
    for (int i = 0; i < num_workers; ++i) {
        workers.emplace_back([this, &total_requests, &running]() {
            while (running.load()) {
                int sock = create_connection();
                if (sock >= 0) {
                    send_request(sock);
                    total_requests.fetch_add(1);
                    close_socket(sock);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    running.store(false);

    for (auto& t : workers) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double requests_per_second = (total_requests.load() * 1000.0) / duration.count();

    std::cout << "\n[PERF] Sustained Load Throughput:" << std::endl;
    std::cout << "  Duration: " << duration_seconds << " seconds" << std::endl;
    std::cout << "  Total Requests: " << total_requests.load() << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << requests_per_second
              << " req/s" << std::endl;

    // Should maintain throughput over time
    EXPECT_GT(requests_per_second, 100.0);
}

TEST_F(ThroughputTest, LargeResponseThroughput) {
    class LargeResponseHandler : public HTTPHandler {
      public:
        Task handle_request(const HTTPRequest& request, HTTPResponse& response,
                            Socket& socket) override {
            response.status_code = 200;
            response.set_header("Content-Type", "application/octet-stream");
            response.body.resize(1024 * 10, 'X'); // 10KB response
            co_return;
        }
    };

    server->set_http_handler(std::make_unique<LargeResponseHandler>());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int num_requests = 100;
    auto start = std::chrono::high_resolution_clock::now();

    int sock = create_connection();
    ASSERT_GE(sock, 0);

    uint64_t total_bytes = 0;
    for (int i = 0; i < num_requests; ++i) {
        send_request(sock);
        total_bytes += 10 * 1024; // Approximate
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    close_socket(sock);

    double mbps = (total_bytes * 8.0) / (duration.count() * 1000.0); // Mbps

    std::cout << "\n[PERF] Large Response Throughput:" << std::endl;
    std::cout << "  Response Size: 10KB" << std::endl;
    std::cout << "  Requests: " << num_requests << std::endl;
    std::cout << "  Total Data: " << (total_bytes / 1024.0) << " KB" << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << mbps << " Mbps"
              << std::endl;
}
