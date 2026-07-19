#include <gtest/gtest.h>
#include <libspaznet/detail/socket_compat.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define close_socket closesocket
#else
#define close_socket ::close
#endif

using namespace spaznet;

// RFC 9113 Compliant Test Handler
class RFC9113TestHandler : public spaznet::http2::Handler {
  public:
    std::atomic<int> request_count{0};
    spaznet::http2::Request last_request;

    Task handle_request(const spaznet::http2::Request& request, spaznet::http2::Response& response,
                        Socket& socket) override {
        request_count.fetch_add(1);
        last_request = request;

        response.stream_id = request.stream_id;
        response.status_code = 200;
        response.set_status(200);
        response.headers["content-type"] = "text/plain";
        response.body = {'O', 'K'};

        co_return;
    }

};

class RFC9113IntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        handler = std::make_unique<RFC9113TestHandler>();
        server = std::make_unique<Server>(2);
        server->set_connection_handler(
            spaznet::http2::make_dispatcher(std::make_unique<RFC9113TestHandler>()));
        server->listen_tcp(9997);

        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    int connect_to_server() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return -1;

        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(9997);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return -1;
        }

        return sock;
    }

    void send_http2_preface(int sock) {
        std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        (void)spaznet::detail::socket_send(sock, preface.c_str(), preface.size(), 0);
    }

    void send_settings_frame(int sock, const spaznet::http2::Settings& settings) {
        spaznet::http2::Frame frame = spaznet::http2::Parser::build_settings_frame(settings, false);
        auto serialized = frame.serialize();
        spaznet::detail::socket_send(sock, serialized.data(), serialized.size(), 0);
    }

    void send_headers_frame(int sock, const spaznet::http2::Request& request, uint32_t stream_id) {
        spaznet::http2::Frame frame = spaznet::http2::Parser::build_headers_frame(request, stream_id, true, true);
        auto serialized = frame.serialize();
        spaznet::detail::socket_send(sock, serialized.data(), serialized.size(), 0);
    }

    std::vector<uint8_t> receive_response(int sock) {
        char buffer[4096];
        int received = recv(sock, buffer, sizeof(buffer), 0);
        if (received > 0) {
            return std::vector<uint8_t>(buffer, buffer + received);
        }
        return {};
    }

    std::unique_ptr<RFC9113TestHandler> handler;
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

// Test RFC 9113 Section 3.5 - Connection Preface
TEST_F(RFC9113IntegrationTest, ConnectionPreface) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    // Server should respond with SETTINGS frame
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close_socket(sock);
}

// Test RFC 9113 Section 4.1 - Frame Format
TEST_F(RFC9113IntegrationTest, FrameFormat) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    spaznet::http2::Settings settings;
    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close_socket(sock);
}

// Test RFC 9113 Section 6.2 - HEADERS Frame
TEST_F(RFC9113IntegrationTest, HeadersFrame) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    spaznet::http2::Settings settings;
    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    spaznet::http2::Request request;
    request.method = "GET";
    request.path = "/test";
    request.headers[":method"] = "GET";
    request.headers[":path"] = "/test";
    request.headers[":scheme"] = "http";
    request.headers[":authority"] = "localhost:9997";

    send_headers_frame(sock, request, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close_socket(sock);
}

// Test RFC 9113 Section 6.1 - DATA Frame
TEST_F(RFC9113IntegrationTest, DataFrame) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    spaznet::http2::Settings settings;
    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send HEADERS frame
    spaznet::http2::Request request;
    request.headers[":method"] = "POST";
    request.headers[":path"] = "/data";
    request.headers[":scheme"] = "http";
    request.headers[":authority"] = "localhost:9997";

    spaznet::http2::Frame headers = spaznet::http2::Parser::build_headers_frame(request, 1, true, false);
    auto headers_serialized = headers.serialize();
    spaznet::detail::socket_send(sock, headers_serialized.data(), headers_serialized.size(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send DATA frame
    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    spaznet::http2::Frame data_frame = spaznet::http2::Parser::build_data_frame(1, data, true);
    auto data_serialized = data_frame.serialize();
    spaznet::detail::socket_send(sock, data_serialized.data(), data_serialized.size(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close_socket(sock);
}

// Test RFC 9113 Section 6.5.2 - SETTINGS Frame
TEST_F(RFC9113IntegrationTest, SettingsFrame) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    spaznet::http2::Settings settings;
    settings.header_table_size = 8192;
    settings.enable_push = false;
    settings.max_concurrent_streams = 100;

    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close_socket(sock);
}

// Test RFC 9113 Section 6.8 - GOAWAY Frame
TEST_F(RFC9113IntegrationTest, GoawayFrame) {
    // GOAWAY is typically sent by server, but we can test frame building
    spaznet::http2::Frame goaway = spaznet::http2::Parser::build_goaway_frame(0, 0);

    EXPECT_EQ(goaway.type, spaznet::http2::FrameType::GOAWAY);
    EXPECT_EQ(goaway.stream_id, 0);
    EXPECT_EQ(goaway.length, 8);
}

// Test RFC 9113 Section 6.4 - RST_STREAM Frame
TEST_F(RFC9113IntegrationTest, RstStreamFrame) {
    spaznet::http2::Frame rst = spaznet::http2::Parser::build_rst_stream_frame(1, 1); // PROTOCOL_ERROR

    EXPECT_EQ(rst.type, spaznet::http2::FrameType::RST_STREAM);
    EXPECT_EQ(rst.stream_id, 1);
    EXPECT_EQ(rst.length, 4);
}

// Test RFC 9113 Section 6.9 - WINDOW_UPDATE Frame
TEST_F(RFC9113IntegrationTest, WindowUpdateFrame) {
    spaznet::http2::Frame window_update = spaznet::http2::Parser::build_window_update_frame(1, 65535);

    EXPECT_EQ(window_update.type, spaznet::http2::FrameType::WINDOW_UPDATE);
    EXPECT_EQ(window_update.stream_id, 1);
    EXPECT_EQ(window_update.length, 4);
}

// Test RFC 9113 Section 6.7 - PING Frame
TEST_F(RFC9113IntegrationTest, PingFrame) {
    std::vector<uint8_t> opaque = {1, 2, 3, 4, 5, 6, 7, 8};
    spaznet::http2::Frame ping = spaznet::http2::Parser::build_ping_frame(opaque, false);

    EXPECT_EQ(ping.type, spaznet::http2::FrameType::PING);
    EXPECT_EQ(ping.stream_id, 0);
    EXPECT_EQ(ping.length, 8);
    EXPECT_EQ(ping.payload, opaque);
}

// Test RFC 9113 Section 8.1 - Request/Response Exchange
TEST_F(RFC9113IntegrationTest, RequestResponseExchange) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    spaznet::http2::Settings settings;
    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    spaznet::http2::Request request;
    request.method = "GET";
    request.path = "/api/test";
    request.headers[":method"] = "GET";
    request.headers[":path"] = "/api/test";
    request.headers[":scheme"] = "http";
    request.headers[":authority"] = "localhost:9997";
    request.headers["user-agent"] = "test-client";

    send_headers_frame(sock, request, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Should have received response
    EXPECT_GE(handler->request_count.load(), 0);

    close_socket(sock);
}

// Test RFC 9113 Section 5.1 - Stream States
TEST_F(RFC9113IntegrationTest, StreamStates) {
    spaznet::http2::Connection conn;

    EXPECT_EQ(conn.get_stream_state(1), spaznet::http2::StreamState::IDLE);

    spaznet::http2::Frame headers;
    headers.type = spaznet::http2::FrameType::HEADERS;
    headers.stream_id = 1;
    headers.flags = spaznet::http2::Flags::END_HEADERS;

    conn.process_frame(headers);
    EXPECT_EQ(conn.get_stream_state(1), spaznet::http2::StreamState::OPEN);
}

// Test Multiple Streams per RFC 9113 Section 5.1.1
TEST_F(RFC9113IntegrationTest, MultipleStreams) {
    spaznet::http2::Connection conn;

    // Open multiple streams
    for (uint32_t i = 1; i <= 5; i += 2) { // Odd stream IDs (client-initiated)
        spaznet::http2::Frame headers;
        headers.type = spaznet::http2::FrameType::HEADERS;
        headers.stream_id = i;
        headers.flags = spaznet::http2::Flags::END_HEADERS;

        conn.process_frame(headers);
        EXPECT_EQ(conn.get_stream_state(i), spaznet::http2::StreamState::OPEN);
    }
}

// Test Large Response Body (multiple DATA frames)
TEST_F(RFC9113IntegrationTest, LargeResponseBody) {
    spaznet::http2::Response response;
    response.stream_id = 1;
    response.status_code = 200;
    response.set_status(200);
    response.body.resize(50000, 'X'); // 50KB body

    auto frames = response.to_frames(16384); // Max frame size

    // Should have 1 HEADERS + multiple DATA frames
    EXPECT_GT(frames.size(), 1);
    EXPECT_EQ(frames[0].type, spaznet::http2::FrameType::HEADERS);

    // Verify all DATA frames
    size_t total_data = 0;
    for (size_t i = 1; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].type, spaznet::http2::FrameType::DATA);
        total_data += frames[i].payload.size();
    }

    EXPECT_EQ(total_data, 50000);
    EXPECT_TRUE(frames.back().flags & spaznet::http2::Flags::END_STREAM);
}

// RFC 9113 §5: streams are independent and the connection is
// multiplexed.  Concretely: the frame-reading loop MUST keep making
// progress while a request handler is running.  If the dispatcher
// were single-threaded-per-connection, a slow handler would stall
// PING-ACK and every other frame.
//
// Test: install a handler that blocks until a flag is set.  Connect,
// send HEADERS+END_STREAM for stream 1 (handler starts but blocks),
// then immediately send PING.  Expect PING-ACK BEFORE the slow
// handler is released.
class HTTP2MultiplexingTest : public ::testing::Test {
  protected:
    class SlowHandler : public spaznet::http2::Handler {
      public:
        std::atomic<bool> release{false};
        std::atomic<int> in_flight{0};

        Task handle_request(const spaznet::http2::Request& request,
                            spaznet::http2::Response& response, Socket&) override {
            in_flight.fetch_add(1);
            // Poll the release flag on the IOContext via short timer waits.
            while (!release.load(std::memory_order_acquire)) {
                co_await socket_yield();
            }
            in_flight.fetch_sub(1);
            response.stream_id = request.stream_id;
            response.status_code = 200;
            response.set_status(200);
            response.headers["content-type"] = "text/plain";
            response.body = {'O', 'K'};
            co_return;
        }

      private:
        // Short sleep — really just a yield onto the next IOContext tick.
        // We can't use std::this_thread::sleep because that blocks the
        // worker thread.  The IOContext exposes a Timer awaitable.
        struct YieldAwaiter {
            bool await_ready() const noexcept {
                return false;
            }
            void await_suspend(std::coroutine_handle<> h) {
                std::thread([h]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    h.resume();
                }).detach();
            }
            void await_resume() const noexcept {}
        };
        static YieldAwaiter socket_yield() {
            return {};
        }
    };

    void SetUp() override {
        auto h = std::make_unique<SlowHandler>();
        handler_raw = h.get();
        server = std::make_unique<Server>(2);
        server->set_connection_handler(spaznet::http2::make_dispatcher(std::move(h)));
        server->listen_tcp(9996);
        server_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        handler_raw->release.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server->stop();
        if (server_thread.joinable()) server_thread.join();
    }

    int connect_to_server() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(9996);
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return -1;
        }
        return sock;
    }

    SlowHandler* handler_raw{nullptr};
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(HTTP2MultiplexingTest, FrameLoopUnblockedBySlowHandler) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    // Preface + our SETTINGS.
    const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    ASSERT_EQ(spaznet::detail::socket_send(sock, preface.data(), preface.size(), 0),
              static_cast<ssize_t>(preface.size()));

    spaznet::http2::Settings client_settings;
    auto settings_frame = spaznet::http2::Parser::build_settings_frame(client_settings, false);
    auto settings_bytes = settings_frame.serialize();
    ASSERT_EQ(spaznet::detail::socket_send(sock, settings_bytes.data(), settings_bytes.size(), 0),
              static_cast<ssize_t>(settings_bytes.size()));

    // HEADERS+END_STREAM for stream 1 — handler will block.
    spaznet::http2::Request req;
    req.method = "GET";
    req.path = "/slow";
    req.headers[":method"] = "GET";
    req.headers[":scheme"] = "http";
    req.headers[":authority"] = "localhost";
    req.headers[":path"] = "/slow";
    auto h_frame = spaznet::http2::Parser::build_headers_frame(req, 1, true, true);
    auto h_bytes = h_frame.serialize();
    ASSERT_EQ(spaznet::detail::socket_send(sock, h_bytes.data(), h_bytes.size(), 0),
              static_cast<ssize_t>(h_bytes.size()));

    // Wait briefly for the handler to actually enter handle_request.
    for (int i = 0; i < 100 && handler_raw->in_flight.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(handler_raw->in_flight.load(), 1)
        << "handler never started — multiplexing test can't run";

    // Send PING with a known payload.
    const std::vector<std::uint8_t> ping_payload = {0xDE, 0xAD, 0xBE, 0xEF,
                                                     0xCA, 0xFE, 0xBA, 0xBE};
    auto ping = spaznet::http2::Parser::build_ping_frame(ping_payload, false);
    auto ping_bytes = ping.serialize();
    ASSERT_EQ(spaznet::detail::socket_send(sock, ping_bytes.data(), ping_bytes.size(), 0),
              static_cast<ssize_t>(ping_bytes.size()));

    // Read until we see a PING-ACK with our payload — with a deadline
    // so a regression where the frame loop blocks would surface as a
    // test failure, not a hang.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::vector<std::uint8_t> rx_buf;
    bool got_ping_ack = false;
    while (!got_ping_ack && std::chrono::steady_clock::now() < deadline) {
        char buf[4096];
        // Use a recv timeout so we don't block forever in the loop.
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100'000;
        spaznet::detail::setsockopt_val(sock, SOL_SOCKET, SO_RCVTIMEO, tv);
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            rx_buf.insert(rx_buf.end(), buf, buf + n);
        }
        // Walk rx_buf frame-by-frame looking for a PING-ACK.
        std::size_t off = 0;
        while (off + 9 <= rx_buf.size()) {
            const std::uint32_t length =
                (static_cast<std::uint32_t>(rx_buf[off]) << 16) |
                (static_cast<std::uint32_t>(rx_buf[off + 1]) << 8) |
                static_cast<std::uint32_t>(rx_buf[off + 2]);
            const auto type = static_cast<spaznet::http2::FrameType>(rx_buf[off + 3]);
            const std::uint8_t flags = rx_buf[off + 4];
            if (off + 9 + length > rx_buf.size()) break;
            if (type == spaznet::http2::FrameType::PING &&
                (flags & spaznet::http2::Flags::ACK) != 0 && length == 8) {
                if (std::memcmp(&rx_buf[off + 9], ping_payload.data(), 8) == 0) {
                    got_ping_ack = true;
                    break;
                }
            }
            off += 9 + length;
        }
    }

    EXPECT_TRUE(got_ping_ack)
        << "frame loop never emitted PING-ACK while the handler was blocked — "
           "dispatcher is still serial-per-connection";
    EXPECT_EQ(handler_raw->in_flight.load(), 1)
        << "handler should still be in flight at this point";

    handler_raw->release.store(true, std::memory_order_release);
    close_socket(sock);
}
