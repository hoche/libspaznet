// End-to-end coverage for Server's reactor entry points (Milestone 5):
// ReactorConnectionFactory, ReactorSyncDatagramHandler, and destroy-based shutdown.
// Deliberately protocol-agnostic — BufferedConnection is the only
// dispatcher involved, so this exercises exactly the new Server plumbing
// without any HTTP/WS/etc. parsing in the way.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/reactor/buffered_connection.hpp>
#include <libspaznet/server.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define close_socket closesocket
#else
#define close_socket ::close
#endif

using namespace std::chrono_literals;
using namespace spaznet;

namespace {

template <typename Pred> auto wait_until(Pred pred, std::chrono::milliseconds timeout = 2000ms) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

// Echoes whatever it receives; hands `on_closed` straight through to
// BufferedConnection, exactly as ReactorConnectionFactory's contract expects.
auto make_echo_factory() -> ReactorConnectionFactory {
    return [](int fd, IOContext& ctx, std::function<void()> on_closed) -> std::shared_ptr<IoHandler> {
        auto conn = std::make_shared<BufferedConnection>(ctx, fd);
        conn->set_on_data([weak = std::weak_ptr<BufferedConnection>(conn)]() {
            auto c = weak.lock();
            if (!c) {
                return;
            }
            std::vector<uint8_t> echoed(c->input().data().begin(), c->input().data().end());
            c->input().consume(echoed.size());
            c->write(std::move(echoed));
        });
        conn->set_on_closed(std::move(on_closed));
        conn->start();
        return conn;
    };
}

int connect_to(uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(sock);
        return -1;
    }
    detail::setsockopt_rcvtimeo_ms(sock, 3000);
    return sock;
}

class ServerReactorTest : public ::testing::Test {
  protected:
    void TearDown() override {
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

    std::unique_ptr<Server> server;
    std::thread server_thread;
};

} // namespace

TEST_F(ServerReactorTest, ReactorConnectionFactoryEchoesEndToEnd) {
    server = std::make_unique<Server>(2);
    server->set_reactor_connection_factory(make_echo_factory());
    server->listen_tcp(19900);
    server_thread = std::thread([this]() { server->run(); });
    std::this_thread::sleep_for(100ms);

    int sock = connect_to(19900);
    ASSERT_GE(sock, 0);

    ASSERT_TRUE(wait_until([&] { return server->get_statistics().active_connections == 1; }));

    const char* msg = "ping";
    ASSERT_EQ(detail::socket_send(sock, msg, 4, 0), 4);

    std::array<char, 16> buf{};
    ssize_t n = detail::socket_recv(sock, buf.data(), buf.size(), 0);
    ASSERT_EQ(n, 4);
    EXPECT_EQ(std::string(buf.data(), 4), "ping");

    close_socket(sock);
    ASSERT_TRUE(wait_until([&] { return server->get_statistics().active_connections == 0; }));
}

// The core behavior server-reactor-entry adds: stop() must tear down
// reactor connections (no shutdown(2), no coroutine to unwind — just
// IoHandler::shutdown()) within its usual bounded deadline, exactly like
// it already does for coroutine connections.
TEST_F(ServerReactorTest, StopShutsDownReactorConnectionsPromptly) {
    server = std::make_unique<Server>(2);
    server->set_reactor_connection_factory(make_echo_factory());
    server->listen_tcp(19901);
    server_thread = std::thread([this]() { server->run(); });
    std::this_thread::sleep_for(100ms);

    int sock = connect_to(19901);
    ASSERT_GE(sock, 0);
    ASSERT_TRUE(wait_until([&] { return server->get_statistics().active_connections == 1; }));

    auto start = std::chrono::steady_clock::now();
    server->stop();
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 2s) << "stop() should not block anywhere near the full drain deadline "
                              "for an idle reactor connection";

    EXPECT_EQ(server->get_statistics().active_connections, 0u);

    // The peer should observe the connection going away (recv returns 0 or an error).
    std::array<char, 16> buf{};
    ssize_t n = detail::socket_recv(sock, buf.data(), buf.size(), 0);
    EXPECT_LE(n, 0);
    close_socket(sock);
}

TEST_F(ServerReactorTest, ReactorSyncDatagramHandlerReceivesAndRepliesSynchronously) {
    server = std::make_unique<Server>(1);
    std::atomic<int> packets_received{0};
    server->set_reactor_sync_datagram_handler([&](Datagram dg) {
        packets_received.fetch_add(1);
        std::string reply = "ack:" + std::string(dg.data.begin(), dg.data.end());
        sendto(dg.fd, reply.data(), reply.size(), 0,
              reinterpret_cast<const struct sockaddr*>(&dg.peer), dg.peer_len);
    });
    server->listen_udp(19902);
    server_thread = std::thread([this]() { server->run(); });
    std::this_thread::sleep_for(100ms);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sock, 0);
    detail::setsockopt_rcvtimeo_ms(sock, 3000);
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(19902);

    const char* msg = "hi";
    ASSERT_EQ(sendto(sock, msg, 2, 0, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 2);

    std::array<char, 32> buf{};
    ssize_t n = recv(sock, buf.data(), buf.size(), 0);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf.data(), n), "ack:hi");
    EXPECT_EQ(packets_received.load(), 1);

    close_socket(sock);
}

// A ReactorConnectionFactory that returns nullptr for every connection: the
// listening accept loop must close the raw fd itself rather than leaking
// it, since nothing else took ownership. (Per ReactorConnectionFactory's
// contract, the factory must NOT close the fd itself in this case — that
// would race Server's own close_socket() call against whatever fd number
// the kernel reassigns in between.)
TEST_F(ServerReactorTest, FactoryDecliningConnectionDoesNotLeakOrHang) {
    server = std::make_unique<Server>(1);
    server->set_reactor_connection_factory(
        [](int, IOContext&, std::function<void()>) -> std::shared_ptr<IoHandler> { return nullptr; });
    server->listen_tcp(19903);
    server_thread = std::thread([this]() { server->run(); });
    std::this_thread::sleep_for(100ms);

    int sock = connect_to(19903);
    ASSERT_GE(sock, 0);

    std::array<char, 16> buf{};
    ssize_t n = detail::socket_recv(sock, buf.data(), buf.size(), 0);
    EXPECT_LE(n, 0); // connection should be closed immediately, not hung

    close_socket(sock);
    EXPECT_EQ(server->get_statistics().active_connections, 0u);
}

// Accept-and-shard: with loops > 1, each accepted connection is pinned to
// one of N independent IOContexts. Concurrent echoes must still work, and
// stop() must drain connections on every loop.
TEST_F(ServerReactorTest, MultiLoopAcceptAndShardEchoesAndStops) {
    constexpr std::size_t kLoops = 4;
    constexpr int kClients = 8;
    server = std::make_unique<Server>(ServerConfig{.loops = kLoops, .workers_per_loop = 0});
    EXPECT_EQ(server->loop_count(), kLoops);

    std::mutex seen_mu;
    std::unordered_set<IOContext*> seen_contexts;
    server->set_reactor_connection_factory(
        [&](int fd, IOContext& ctx,
            std::function<void()> on_closed) -> std::shared_ptr<IoHandler> {
            {
                std::lock_guard<std::mutex> lock(seen_mu);
                seen_contexts.insert(&ctx);
            }
            auto conn = std::make_shared<BufferedConnection>(ctx, fd);
            conn->set_on_data([weak = std::weak_ptr<BufferedConnection>(conn)]() {
                auto c = weak.lock();
                if (!c) {
                    return;
                }
                std::vector<uint8_t> echoed(c->input().data().begin(), c->input().data().end());
                c->input().consume(echoed.size());
                c->write(std::move(echoed));
            });
            conn->set_on_closed(std::move(on_closed));
            conn->start();
            return conn;
        });

    server->listen_tcp(19904);
    server_thread = std::thread([this]() { server->run(); });
    std::this_thread::sleep_for(100ms);

    std::vector<int> socks;
    socks.reserve(kClients);
    for (int i = 0; i < kClients; ++i) {
        int sock = connect_to(19904);
        ASSERT_GE(sock, 0);
        socks.push_back(sock);
    }
    ASSERT_TRUE(wait_until(
        [&] { return server->get_statistics().active_connections == static_cast<std::size_t>(kClients); }));

    for (int i = 0; i < kClients; ++i) {
        std::string msg = "p" + std::to_string(i);
        ASSERT_EQ(detail::socket_send(socks[static_cast<std::size_t>(i)], msg.data(), msg.size(), 0),
                  static_cast<ssize_t>(msg.size()));
        std::array<char, 16> buf{};
        ssize_t n = detail::socket_recv(socks[static_cast<std::size_t>(i)], buf.data(), buf.size(), 0);
        ASSERT_EQ(n, static_cast<ssize_t>(msg.size()));
        EXPECT_EQ(std::string(buf.data(), static_cast<std::size_t>(n)), msg);
    }

    {
        std::lock_guard<std::mutex> lock(seen_mu);
        // Round-robin across 8 accepts onto 4 loops should touch more than one
        // context; ideally all four, but at least prove sharding happened.
        EXPECT_GT(seen_contexts.size(), 1u);
    }

    server->stop();
    EXPECT_EQ(server->get_statistics().active_connections, 0u);

    for (int sock : socks) {
        std::array<char, 16> buf{};
        ssize_t n = detail::socket_recv(sock, buf.data(), buf.size(), 0);
        EXPECT_LE(n, 0);
        close_socket(sock);
    }
}
