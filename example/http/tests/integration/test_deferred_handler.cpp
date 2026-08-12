// Exercises the one HTTPHandler contract path that no other test hits:
// a handler that does NOT complete its ResponseWriter before
// handle_request() returns. Every other example/http handler answers
// synchronously, so without this test the AwaitResponseReady bridge in
// dispatcher.cpp (the suspend to a later completion) would have zero
// coverage.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/server.hpp>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define close_socket closesocket
#else
#define close_socket ::close
#endif

using namespace spaznet;

namespace {

// Defers every response: hands the ResponseWriter off to a detached
// background thread that sleeps briefly and *then* calls complete() —
// well after handle_request() itself has returned. Proves completion can
// come from an arbitrary thread, at an arbitrary later time, and still
// correctly resume the suspended dispatcher coroutine and get the
// response onto the wire.
class DeferredHandler : public spaznet::http::HTTPHandler {
  public:
    std::atomic<int> requests_started{0};
    std::atomic<int> requests_completed{0};

    void handle_request(const spaznet::http::HTTPRequest& request, spaznet::http::ResponseWriter writer) override {
        requests_started.fetch_add(1);
        std::string path = request.request_target;
        std::thread([this, writer, path]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            spaznet::http::HTTPResponse response;
            response.status_code = 200;
            response.reason_phrase = "OK";
            response.set_header("Content-Type", "text/plain");
            std::string body = "deferred:" + path;
            response.body.assign(body.begin(), body.end());
            requests_completed.fetch_add(1);
            writer.complete(std::move(response));
        }).detach();
    }
};

} // namespace

class DeferredHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto handler_unique = std::make_unique<DeferredHandler>();
        handler = handler_unique.get();
        server = std::make_unique<Server>(2);
        server->set_connection_handler(spaznet::http::make_dispatcher(std::move(handler_unique)));
        server->listen_tcp(8891);
        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    std::string send_get(const std::string& path) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return "";
        }
        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(8891);
        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return "";
        }

        std::ostringstream request;
        request << "GET " << path << " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        std::string req_str = request.str();
        spaznet::detail::socket_send(sock, req_str.c_str(), req_str.size(), MSG_NOSIGNAL);
        spaznet::detail::setsockopt_rcvtimeo_ms(sock, 3000);

        std::string response;
        char buffer[4096];
        for (;;) {
            int received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (received <= 0) {
                break;
            }
            response.append(buffer, static_cast<std::size_t>(received));
        }
        close_socket(sock);
        return response;
    }

    DeferredHandler* handler{nullptr};
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(DeferredHandlerTest, ResponseArrivesAfterHandlerDefersCompletion) {
    std::string response = send_get("/hello");

    ASSERT_NE(response.find("HTTP/1.1 200"), std::string::npos) << response;
    EXPECT_NE(response.find("deferred:/hello"), std::string::npos) << response;
    EXPECT_EQ(handler->requests_started.load(), 1);
    EXPECT_EQ(handler->requests_completed.load(), 1);
}

TEST_F(DeferredHandlerTest, MultipleSequentialDeferredRequestsAllComplete) {
    for (int i = 0; i < 3; ++i) {
        std::string path = "/req" + std::to_string(i);
        std::string response = send_get(path);
        ASSERT_NE(response.find("HTTP/1.1 200"), std::string::npos) << response;
        EXPECT_NE(response.find("deferred:" + path), std::string::npos) << response;
    }
    EXPECT_EQ(handler->requests_started.load(), 3);
    EXPECT_EQ(handler->requests_completed.load(), 3);
}
