#include <gtest/gtest.h>
#include <libspaznet/detail/socket_compat.hpp>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/server.hpp>
#include <thread>
#include <vector>

#ifdef _WIN32
#define close_socket closesocket
#else
#define close_socket ::close
#endif

using namespace spaznet;

class ConcurrentPerformanceHandler : public spaznet::http::HTTPHandler {
  public:
    std::atomic<uint64_t> request_count{0};

    Task handle_request(const spaznet::http::HTTPRequest& request, spaznet::http::HTTPResponse& response,
                        Socket& socket) override {
        request_count.fetch_add(1);

        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'O', 'K'};

        co_return;
    }
};

class ConcurrentPerformanceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        server = std::make_unique<Server>(8); // More threads for concurrency
        server->set_connection_handler(spaznet::http::make_dispatcher(std::make_unique<ConcurrentPerformanceHandler>()));
        server->listen_tcp(9002);

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
        std::string request = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
        (void)spaznet::detail::socket_send(sock, request.c_str(), request.size(), MSG_NOSIGNAL);

        char buffer[4096];
        (void)spaznet::detail::socket_recv(sock, buffer, sizeof(buffer) - 1, 0);
    }

    int create_connection() {
        // CI runners with few cores can saturate the server's SYN queue when
        // hundreds of threads call connect() simultaneously; retry transient
        // ECONNREFUSED with a short exponential backoff before giving up.
        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(9002);

        int delay_us = 200;
        for (int attempt = 0; attempt < 8; ++attempt) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0)
                return -1;
            // SO_LINGER {1, 0} forces close() to send RST instead of FIN
            // so the client-side ephemeral port skips TIME_WAIT.  This
            // matters when prior tests in the run have already churned
            // through many ports: macOS's ephemeral range is only
            // ~16K, and otherwise connect() starts failing with
            // EADDRNOTAVAIL.
            struct linger lin {1, 0};
            spaznet::detail::setsockopt_val(sock, SOL_SOCKET, SO_LINGER, lin);
            spaznet::detail::setsockopt_rcvtimeo_ms(sock, 2000);
            if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                return sock;
            }
            close_socket(sock);
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
            delay_us *= 2;
        }
        return -1;
    }

    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(ConcurrentPerformanceTest, ScalingWithThreads) {
    const int num_connections = 100;
    const int requests_per_connection = 10;

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

    double requests_per_second =
        (num_connections * requests_per_connection * 1000.0) / duration.count();

    std::cout << "\n[PERF] Concurrent Connection Scaling:" << std::endl;
    std::cout << "  Connections: " << num_connections << std::endl;
    std::cout << "  Requests per Connection: " << requests_per_connection << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << requests_per_second
              << " req/s" << std::endl;
}

TEST_F(ConcurrentPerformanceTest, PeakConcurrentConnections) {
    const int target_connections = 500;
    std::atomic<int> successful_connections{0};
    std::atomic<int> failed_connections{0};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < target_connections; ++i) {
        threads.emplace_back([this, &successful_connections, &failed_connections]() {
            int sock = create_connection();
            if (sock >= 0) {
                successful_connections.fetch_add(1);
                send_request(sock);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                close_socket(sock);
            } else {
                failed_connections.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "\n[PERF] Peak Concurrent Connections:" << std::endl;
    std::cout << "  Target: " << target_connections << std::endl;
    std::cout << "  Successful: " << successful_connections.load() << std::endl;
    std::cout << "  Failed: " << failed_connections.load() << std::endl;
    std::cout << "  Success Rate: " << std::fixed << std::setprecision(2)
              << (100.0 * successful_connections.load() / target_connections) << "%" << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;

    // Should handle at least 80% of connections
    EXPECT_GT(successful_connections.load(), target_connections * 0.8);
}

TEST_F(ConcurrentPerformanceTest, BurstTrafficHandling) {
    const int burst_size = 200;
    const int num_bursts = 5;

    std::atomic<uint64_t> total_requests{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int burst = 0; burst < num_bursts; ++burst) {
        std::vector<std::thread> threads;

        for (int i = 0; i < burst_size; ++i) {
            threads.emplace_back([this, &total_requests]() {
                int sock = create_connection();
                if (sock >= 0) {
                    send_request(sock);
                    total_requests.fetch_add(1);
                    close_socket(sock);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double requests_per_second = (total_requests.load() * 1000.0) / duration.count();

    std::cout << "\n[PERF] Burst Traffic Handling:" << std::endl;
    std::cout << "  Burst Size: " << burst_size << std::endl;
    std::cout << "  Number of Bursts: " << num_bursts << std::endl;
    std::cout << "  Total Requests: " << total_requests.load() << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Average Throughput: " << std::fixed << std::setprecision(2)
              << requests_per_second << " req/s" << std::endl;
}
