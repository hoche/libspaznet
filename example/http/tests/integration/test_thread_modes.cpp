#include <gtest/gtest.h>

#include <libspaznet/detail/socket_compat.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/server.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "dispatcher_test_support.hpp"

using namespace spaznet;
using spaznet::http::testing_support::DispatcherKind;
using spaznet::http::testing_support::install_dispatcher;
using spaznet::http::testing_support::listen_on_random_port;
using spaznet::http::testing_support::AllDispatcherKinds;

namespace {

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
        detail::close_socket_fd(sock);
        return "";
    }

    detail::setsockopt_rcvtimeo_ms(sock, 3000);

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: localhost\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";

    std::string req_str = request.str();
    ssize_t sent = detail::socket_send(sock, req_str.c_str(), req_str.size(), MSG_NOSIGNAL);
    if (sent < 0 || static_cast<size_t>(sent) != req_str.size()) {
        detail::close_socket_fd(sock);
        return "";
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        ssize_t received = detail::socket_recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            break;
        }
        buffer[received] = '\0';
        response.append(buffer, static_cast<size_t>(received));
    }

    detail::close_socket_fd(sock);
    return response;
}

class SimpleOKHandler : public spaznet::http::HTTPHandler {
  public:
    std::atomic<int> requests{0};

    void handle_request(const spaznet::http::HTTPRequest&, spaznet::http::ResponseWriter writer) override {
        requests.fetch_add(1, std::memory_order_relaxed);
        spaznet::http::HTTPResponse response;
        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'O', 'K'};
        writer.complete(std::move(response));
    }
};

} // namespace

// Parameterized over both {0, 4} worker threads AND both HTTP/1.1
// dispatchers, so every combination of threading mode x execution model
// gets the same correctness check.
class ServerThreadModeTest : public ::testing::TestWithParam<std::tuple<std::size_t, DispatcherKind>> {};

TEST_P(ServerThreadModeTest, HandlesRequestsInBothModes) {
    const std::size_t worker_threads = std::get<0>(GetParam()); // 0 = non-threaded, >0 = multi-threaded
    const DispatcherKind dispatcher_kind = std::get<1>(GetParam());
    auto handler = std::make_unique<SimpleOKHandler>();
    auto* handler_ptr = handler.get();

    Server server(worker_threads);
    install_dispatcher(server, std::move(handler), dispatcher_kind);
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

INSTANTIATE_TEST_SUITE_P(
    ThreadModes, ServerThreadModeTest,
    ::testing::Combine(::testing::Values(std::size_t{0}, std::size_t{4}),
                       ::testing::ValuesIn(AllDispatcherKinds())),
    [](const ::testing::TestParamInfo<std::tuple<std::size_t, DispatcherKind>>& info) {
        std::string name = std::get<0>(info.param) == 0 ? "NonThreaded" : "Threaded";
        name += (std::get<1>(info.param) == DispatcherKind::Reactor) ? "_Reactor" : "_Coroutine";
        return name;
    });

class ServerDefaultModeTest : public ::testing::TestWithParam<DispatcherKind> {};

TEST_P(ServerDefaultModeTest, DefaultIsNonThreadedAndWorks) {
    Server server; // default should be non-threaded
    install_dispatcher(server, std::make_unique<SimpleOKHandler>(), GetParam());
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

INSTANTIATE_TEST_SUITE_P(Dispatchers, ServerDefaultModeTest,
                        ::testing::ValuesIn(AllDispatcherKinds()),
                        spaznet::http::testing_support::DispatcherKindName);
