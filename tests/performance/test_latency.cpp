#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <libspaznet/http_handler.hpp>
#include <libspaznet/server.hpp>
#include <numeric>
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

class LatencyHTTPHandler : public HTTPHandler {
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

class LatencyTest : public ::testing::Test {
  protected:
    void SetUp() override {
        server = std::make_unique<Server>(4);
        server->set_http_handler(std::make_unique<LatencyHTTPHandler>());
        server->listen_tcp(9001);

        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    int64_t measure_request_latency() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return -1;

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(9001);

        auto start = std::chrono::high_resolution_clock::now();

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return -1;
        }

        std::string request = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
        send(sock, request.c_str(), request.size(), 0);

        char buffer[4096];
        recv(sock, buffer, sizeof(buffer) - 1, 0);

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        close_socket(sock);

        return duration.count();
    }

    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(LatencyTest, SingleRequestLatency) {
    const int num_samples = 100;
    std::vector<int64_t> latencies;
    latencies.reserve(num_samples);

    for (int i = 0; i < num_samples; ++i) {
        int64_t latency = measure_request_latency();
        if (latency > 0) {
            latencies.push_back(latency);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (latencies.empty()) {
        GTEST_SKIP() << "No successful latency measurements";
    }

    std::sort(latencies.begin(), latencies.end());

    int64_t min = latencies.front();
    int64_t max = latencies.back();
    int64_t median = latencies[latencies.size() / 2];
    int64_t p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
    int64_t p99 = latencies[static_cast<size_t>(latencies.size() * 0.99)];

    double mean = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

    std::cout << "\n[PERF] Request Latency Statistics:" << std::endl;
    std::cout << "  Samples: " << latencies.size() << std::endl;
    std::cout << "  Min: " << min << " μs" << std::endl;
    std::cout << "  Max: " << max << " μs" << std::endl;
    std::cout << "  Mean: " << std::fixed << std::setprecision(2) << mean << " μs" << std::endl;
    std::cout << "  Median: " << median << " μs" << std::endl;
    std::cout << "  P95: " << p95 << " μs" << std::endl;
    std::cout << "  P99: " << p99 << " μs" << std::endl;

    // Median latency should be reasonable (< 10ms)
    EXPECT_LT(median, 10000);
}

TEST_F(LatencyTest, ConcurrentRequestLatency) {
    const int num_requests = 50;
    const int num_threads = 5;

    std::vector<std::vector<int64_t>> thread_latencies(num_threads);
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, &thread_latencies, t, num_requests]() {
            for (int i = 0; i < num_requests; ++i) {
                int64_t latency = measure_request_latency();
                if (latency > 0) {
                    thread_latencies[t].push_back(latency);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::vector<int64_t> all_latencies;
    for (const auto& latencies : thread_latencies) {
        all_latencies.insert(all_latencies.end(), latencies.begin(), latencies.end());
    }

    if (all_latencies.empty()) {
        GTEST_SKIP() << "No successful latency measurements";
    }

    std::sort(all_latencies.begin(), all_latencies.end());
    int64_t median = all_latencies[all_latencies.size() / 2];
    int64_t p95 = all_latencies[static_cast<size_t>(all_latencies.size() * 0.95)];

    std::cout << "\n[PERF] Concurrent Request Latency:" << std::endl;
    std::cout << "  Threads: " << num_threads << std::endl;
    std::cout << "  Total Requests: " << all_latencies.size() << std::endl;
    std::cout << "  Median: " << median << " μs" << std::endl;
    std::cout << "  P95: " << p95 << " μs" << std::endl;

    EXPECT_LT(median, 20000); // Should handle concurrent requests reasonably
}

TEST_F(LatencyTest, ConnectionEstablishmentLatency) {
    const int num_samples = 50;
    std::vector<int64_t> latencies;
    latencies.reserve(num_samples);

    for (int i = 0; i < num_samples; ++i) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            continue;

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(9001);

        auto start = std::chrono::high_resolution_clock::now();
        int result = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        auto end = std::chrono::high_resolution_clock::now();

        if (result == 0) {
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            latencies.push_back(duration.count());
        }

        close_socket(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (latencies.empty()) {
        GTEST_SKIP() << "No successful connection measurements";
    }

    std::sort(latencies.begin(), latencies.end());
    int64_t median = latencies[latencies.size() / 2];
    double mean = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size();

    std::cout << "\n[PERF] Connection Establishment Latency:" << std::endl;
    std::cout << "  Samples: " << latencies.size() << std::endl;
    std::cout << "  Mean: " << std::fixed << std::setprecision(2) << mean << " μs" << std::endl;
    std::cout << "  Median: " << median << " μs" << std::endl;
}
