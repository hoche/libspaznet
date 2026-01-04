#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/server.hpp>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace spaznet;

namespace {

static uint16_t listen_on_random_port(Server& server) {
    // Avoid "pick free port, close it, then bind later" races by binding directly and retrying.
    //
    // We intentionally avoid port 0 because Server currently doesn't expose the chosen port back to
    // the caller.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(20000, 65000);

    constexpr int kMaxAttempts = 200;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        uint16_t port = static_cast<uint16_t>(dist(gen));
        try {
            server.listen_tcp(port);
            return port;
        } catch (...) {
            // Most likely EADDRINUSE; retry with another port.
        }
    }
    return 0;
}

static std::string send_http_request(uint16_t port, const std::string& path = "/") {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return "";
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return "";
    }

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: localhost\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";

    std::string req_str = request.str();
    ssize_t sent = send(sock, req_str.c_str(), req_str.size(), MSG_NOSIGNAL);
    if (sent < 0 || static_cast<size_t>(sent) != req_str.size()) {
        ::close(sock);
        return "";
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        int received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            break;
        }
        buffer[received] = '\0';
        response.append(buffer, received);
    }

    ::close(sock);
    return response;
}

class SimpleOKHandler : public HTTPHandler {
  public:
    std::atomic<int> requests{0};

    Task handle_request(const HTTPRequest&, HTTPResponse& response, Socket&) override {
        requests.fetch_add(1, std::memory_order_relaxed);
        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'O', 'K'};
        co_return;
    }
};

} // namespace

class ServerThreadModeTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(ServerThreadModeTest, HandlesRequestsInBothModes) {
    const std::size_t worker_threads = GetParam(); // 0 = non-threaded, >0 = multi-threaded
    auto handler = std::make_unique<SimpleOKHandler>();
    auto* handler_ptr = handler.get();

    Server server(worker_threads);
    server.set_http_handler(std::move(handler));
    uint16_t port = listen_on_random_port(server);
    ASSERT_NE(port, 0) << "Failed to bind any test port";

    std::thread server_thread([&]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // A few sequential requests
    for (int i = 0; i < 5; ++i) {
        std::string resp = send_http_request(port, "/test");
        EXPECT_NE(resp.find("200"), std::string::npos);
        EXPECT_NE(resp.find("OK"), std::string::npos);
    }

    // Light concurrency: correctness should hold in both modes.
    std::vector<std::thread> clients;
    for (int i = 0; i < 10; ++i) {
        clients.emplace_back([&]() {
            std::string resp = send_http_request(port, "/c");
            EXPECT_NE(resp.find("200"), std::string::npos);
            EXPECT_NE(resp.find("OK"), std::string::npos);
        });
    }
    for (auto& t : clients) {
        t.join();
    }

    server.stop();
    server_thread.join();

    EXPECT_GE(handler_ptr->requests.load(), 15);
}

INSTANTIATE_TEST_SUITE_P(ThreadModes, ServerThreadModeTest,
                         ::testing::Values(std::size_t{0}, std::size_t{4}));

TEST(ServerDefaultModeTest, DefaultIsNonThreadedAndWorks) {
    Server server; // default should be non-threaded
    server.set_http_handler(std::make_unique<SimpleOKHandler>());
    uint16_t port = listen_on_random_port(server);
    ASSERT_NE(port, 0) << "Failed to bind any test port";

    std::thread server_thread([&]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::string resp = send_http_request(port, "/");
    EXPECT_NE(resp.find("200"), std::string::npos);
    EXPECT_NE(resp.find("OK"), std::string::npos);

    server.stop();
    server_thread.join();
}
