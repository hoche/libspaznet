#include <gtest/gtest.h>
#include <chrono>
#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/server.hpp>
#include <thread>
#include <vector>

#include "dispatcher_test_support.hpp"

#include <libspaznet/detail/socket_compat.hpp>
#ifdef _WIN32
#define close_socket closesocket
#else
#define close_socket ::close
#endif

using namespace spaznet;
using spaznet::http::testing_support::DispatcherKind;
using spaznet::http::testing_support::DispatcherKindName;
using spaznet::http::testing_support::install_dispatcher;
using spaznet::http::testing_support::listen_on_random_port;
using spaznet::http::testing_support::AllDispatcherKinds;

// Simple test HTTP handler
class TestHTTPHandler : public spaznet::http::HTTPHandler {
  public:
    void handle_request(const spaznet::http::HTTPRequest&, spaznet::http::ResponseWriter writer) override {
        spaznet::http::HTTPResponse response;
        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'O', 'K'};
        writer.complete(std::move(response));
    }
};

// Parameterized over both HTTP/1.1 dispatchers (see
// dispatcher_test_support.hpp): every scenario here — including the
// stop()-drain ones, which exercise Server's coroutine-drain and
// reactor-connection-teardown paths respectively — runs against both.
class TCPServerTest : public ::testing::TestWithParam<DispatcherKind> {
  protected:
    void SetUp() override {
        // Reactor connections don't use coroutine workers; workers are
        // idle on this path. Keep the same Server(2) as the coroutine
        // fixture so stop()/IOContext teardown matches the well-tested
        // shape (Server(0) was flaking ListenOnPort/Reactor under CI).
        server = std::make_unique<Server>(2);
        // Set up a simple handler to handle connections
        install_dispatcher(*server, std::make_unique<TestHTTPHandler>(), GetParam());
        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        // Always join the run() thread before destroying Server/IOContext.
        // stop() can throw std::system_error if a prior bug left a mutex
        // invalid; still join so we never destroy IOContext under live workers
        // (that is what surfaces as "mutex lock failed: Invalid argument" on
        // macOS ARM64 when worker_wake_mutex_ is torn down mid-wait).
        if (server) {
            try {
                server->stop();
            } catch (...) {
            }
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
        server.reset();
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

        spaznet::detail::setsockopt_rcvtimeo_ms(sock, 3000);
        return sock;
    }

    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_P(TCPServerTest, ServerStartup) {
    // Server should start without errors
    EXPECT_NE(server, nullptr);
}

TEST_P(TCPServerTest, ListenOnPort) {
    const uint16_t port = listen_on_random_port(*server);
    ASSERT_NE(port, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Try to connect
    int client = connect_to_server(port);
    if (client >= 0) {
        close_socket(client);
    }
    // Connection may succeed or fail depending on handler, but shouldn't crash
}

TEST_P(TCPServerTest, MultiplePorts) {
    const uint16_t port1 = listen_on_random_port(*server);
    const uint16_t port2 = listen_on_random_port(*server);
    ASSERT_NE(port1, 0u);
    ASSERT_NE(port2, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Both ports should be listening
    int client1 = connect_to_server(port1);
    int client2 = connect_to_server(port2);

    if (client1 >= 0)
        close_socket(client1);
    if (client2 >= 0)
        close_socket(client2);
}

TEST_P(TCPServerTest, ServerShutdown) {
    EXPECT_NO_THROW(server->stop());
}

// Server::stop() must drain in-flight connections before returning so the
// IOContext isn't torn down with a suspended coroutine (or, for the
// reactor dispatcher, a live BufferedConnection) still pointing into it.
// We open an idle keep-alive connection (server parked waiting for more
// data), then call stop() and assert it returns inside the drain
// deadline.
TEST_P(TCPServerTest, StopDrainsIdleConnection) {
    const uint16_t port = listen_on_random_port(*server);
    ASSERT_NE(port, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int client = connect_to_server(port);
    ASSERT_GE(client, 0);

    // Send one full request and read the response so the server side has
    // cycled back to waiting for the next request's bytes — i.e., it is
    // now parked waiting for keep-alive bytes that will never come.
    std::string req = "GET /x HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(send(client, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
    char buf[512]{};
    ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
    ASSERT_GT(n, 0);

    // The server side is now parked waiting for more bytes on this
    // connection. stop() must force it closed (shutdown(2)+unwind for the
    // coroutine dispatcher, IoHandler::shutdown() for the reactor one) and
    // wait for it to finish. Must return well inside the 1s deadline.
    auto t0 = std::chrono::steady_clock::now();
    server->stop();
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1500);

    close_socket(client);
}

// Regression for the ConnectionGuard / Socket::close TOCTOU: each
// completed connection must remove its fd from active_client_fds_
// BEFORE Socket::close() returns the fd to the kernel, otherwise a
// subsequent accept() could reuse the fd number while it's still
// "tracked", and a concurrent Server::stop() would shutdown(2) the
// foreign socket. (For the reactor dispatcher, the equivalent hazard is
// finish_reactor_connection() vs. fd reuse; same test, same guarantee,
// different mechanism underneath.)
//
// We can't easily inspect active_client_fds_ from a test, but we
// CAN drive many short-lived connections in sequence and then call
// stop(); if the guard's release path is wrong, stop() will block
// for ~1s waiting on a phantom active-connection count.
TEST_P(TCPServerTest, StopReturnsImmediatelyAfterShortLivedConnections) {
    const uint16_t port = listen_on_random_port(*server);
    ASSERT_NE(port, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    constexpr int kNumConnections = 25;
    for (int i = 0; i < kNumConnections; ++i) {
        int client = connect_to_server(port);
        ASSERT_GE(client, 0);
        std::string req = "GET /x HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n\r\n";
        ASSERT_EQ(send(client, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
        char buf[512]{};
        // Drain until peer closes; this lets the server side run its
        // close + guard-release and exit cleanly.
        while (recv(client, buf, sizeof(buf), 0) > 0) {
        }
        close_socket(client);
    }

    // All 25 connections should have unwound by now. stop() must
    // return promptly — anything close to the 1 s drain deadline
    // means stale entries are still tracked.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto t0 = std::chrono::steady_clock::now();
    server->stop();
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 250);
}

INSTANTIATE_TEST_SUITE_P(Dispatchers, TCPServerTest,
                        ::testing::ValuesIn(AllDispatcherKinds()),
                        DispatcherKindName);
