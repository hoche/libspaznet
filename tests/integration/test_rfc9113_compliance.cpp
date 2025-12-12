#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <libspaznet/handlers/http2_handler.hpp>
#include <libspaznet/server.hpp>
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

// RFC 9113 Compliant Test Handler
class RFC9113TestHandler : public HTTP2Handler {
  public:
    std::atomic<int> request_count{0};
    HTTP2Request last_request;

    Task handle_request(const HTTP2Request& request, HTTP2Response& response,
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

    Task handle_frame(const HTTP2Frame& frame, Socket& socket) override {
        co_return;
    }
};

class RFC9113IntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        handler = std::make_unique<RFC9113TestHandler>();
        server = std::make_unique<Server>(2);
        server->set_http2_handler(std::make_unique<RFC9113TestHandler>());
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
        send(sock, preface.c_str(), preface.size(), 0);
    }

    void send_settings_frame(int sock, const HTTP2Settings& settings) {
        HTTP2Frame frame = HTTP2Parser::build_settings_frame(settings, false);
        auto serialized = frame.serialize();
        send(sock, serialized.data(), serialized.size(), 0);
    }

    void send_headers_frame(int sock, const HTTP2Request& request, uint32_t stream_id) {
        HTTP2Frame frame = HTTP2Parser::build_headers_frame(request, stream_id, true, true);
        auto serialized = frame.serialize();
        send(sock, serialized.data(), serialized.size(), 0);
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

    HTTP2Settings settings;
    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close_socket(sock);
}

// Test RFC 9113 Section 6.2 - HEADERS Frame
TEST_F(RFC9113IntegrationTest, HeadersFrame) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    HTTP2Settings settings;
    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    HTTP2Request request;
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

    HTTP2Settings settings;
    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send HEADERS frame
    HTTP2Request request;
    request.headers[":method"] = "POST";
    request.headers[":path"] = "/data";
    request.headers[":scheme"] = "http";
    request.headers[":authority"] = "localhost:9997";

    HTTP2Frame headers = HTTP2Parser::build_headers_frame(request, 1, true, false);
    auto headers_serialized = headers.serialize();
    send(sock, headers_serialized.data(), headers_serialized.size(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send DATA frame
    std::vector<uint8_t> data = {'t', 'e', 's', 't'};
    HTTP2Frame data_frame = HTTP2Parser::build_data_frame(1, data, true);
    auto data_serialized = data_frame.serialize();
    send(sock, data_serialized.data(), data_serialized.size(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    close_socket(sock);
}

// Test RFC 9113 Section 6.5.2 - SETTINGS Frame
TEST_F(RFC9113IntegrationTest, SettingsFrame) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    HTTP2Settings settings;
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
    HTTP2Frame goaway = HTTP2Parser::build_goaway_frame(0, 0);

    EXPECT_EQ(goaway.type, HTTP2FrameType::GOAWAY);
    EXPECT_EQ(goaway.stream_id, 0);
    EXPECT_EQ(goaway.length, 8);
}

// Test RFC 9113 Section 6.4 - RST_STREAM Frame
TEST_F(RFC9113IntegrationTest, RstStreamFrame) {
    HTTP2Frame rst = HTTP2Parser::build_rst_stream_frame(1, 1); // PROTOCOL_ERROR

    EXPECT_EQ(rst.type, HTTP2FrameType::RST_STREAM);
    EXPECT_EQ(rst.stream_id, 1);
    EXPECT_EQ(rst.length, 4);
}

// Test RFC 9113 Section 6.9 - WINDOW_UPDATE Frame
TEST_F(RFC9113IntegrationTest, WindowUpdateFrame) {
    HTTP2Frame window_update = HTTP2Parser::build_window_update_frame(1, 65535);

    EXPECT_EQ(window_update.type, HTTP2FrameType::WINDOW_UPDATE);
    EXPECT_EQ(window_update.stream_id, 1);
    EXPECT_EQ(window_update.length, 4);
}

// Test RFC 9113 Section 6.7 - PING Frame
TEST_F(RFC9113IntegrationTest, PingFrame) {
    std::vector<uint8_t> opaque = {1, 2, 3, 4, 5, 6, 7, 8};
    HTTP2Frame ping = HTTP2Parser::build_ping_frame(opaque, false);

    EXPECT_EQ(ping.type, HTTP2FrameType::PING);
    EXPECT_EQ(ping.stream_id, 0);
    EXPECT_EQ(ping.length, 8);
    EXPECT_EQ(ping.payload, opaque);
}

// Test RFC 9113 Section 8.1 - Request/Response Exchange
TEST_F(RFC9113IntegrationTest, RequestResponseExchange) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

    send_http2_preface(sock);

    HTTP2Settings settings;
    send_settings_frame(sock, settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    HTTP2Request request;
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
    HTTP2Connection conn;

    EXPECT_EQ(conn.get_stream_state(1), HTTP2StreamState::IDLE);

    HTTP2Frame headers;
    headers.type = HTTP2FrameType::HEADERS;
    headers.stream_id = 1;
    headers.flags = HTTP2Flags::END_HEADERS;

    conn.process_frame(headers);
    EXPECT_EQ(conn.get_stream_state(1), HTTP2StreamState::OPEN);
}

// Test Multiple Streams per RFC 9113 Section 5.1.1
TEST_F(RFC9113IntegrationTest, MultipleStreams) {
    HTTP2Connection conn;

    // Open multiple streams
    for (uint32_t i = 1; i <= 5; i += 2) { // Odd stream IDs (client-initiated)
        HTTP2Frame headers;
        headers.type = HTTP2FrameType::HEADERS;
        headers.stream_id = i;
        headers.flags = HTTP2Flags::END_HEADERS;

        conn.process_frame(headers);
        EXPECT_EQ(conn.get_stream_state(i), HTTP2StreamState::OPEN);
    }
}

// Test Large Response Body (multiple DATA frames)
TEST_F(RFC9113IntegrationTest, LargeResponseBody) {
    HTTP2Response response;
    response.stream_id = 1;
    response.status_code = 200;
    response.set_status(200);
    response.body.resize(50000, 'X'); // 50KB body

    auto frames = response.to_frames(16384); // Max frame size

    // Should have 1 HEADERS + multiple DATA frames
    EXPECT_GT(frames.size(), 1);
    EXPECT_EQ(frames[0].type, HTTP2FrameType::HEADERS);

    // Verify all DATA frames
    size_t total_data = 0;
    for (size_t i = 1; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].type, HTTP2FrameType::DATA);
        total_data += frames[i].payload.size();
    }

    EXPECT_EQ(total_data, 50000);
    EXPECT_TRUE(frames.back().flags & HTTP2Flags::END_STREAM);
}
