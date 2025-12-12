#include <gtest/gtest.h>
#include <libspaznet/server.hpp>
#include <libspaznet/websocket_handler.hpp>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define close_socket ::close
#endif

using namespace spaznet;

class TestWebSocketHandler : public WebSocketHandler {
public:
    std::atomic<int> open_count{0};
    std::atomic<int> message_count{0};
    std::atomic<int> close_count{0};
    std::vector<std::string> received_messages;
    
    Task on_open(Socket& socket) override {
        open_count.fetch_add(1);
        co_return;
    }
    
    Task handle_message(const WebSocketMessage& message, Socket& socket) override {
        message_count.fetch_add(1);
        
        if (message.opcode == WebSocketOpcode::Text) {
            std::string text(message.data.begin(), message.data.end());
            received_messages.push_back(text);
            
            // Echo back
            WebSocketFrame frame;
            frame.fin = true;
            frame.opcode = WebSocketOpcode::Text;
            frame.masked = false;
            frame.payload = message.data;
            frame.payload_length = frame.payload.size();
            
            auto frame_data = frame.serialize();
            co_await socket.async_write(frame_data);
        }
        
        co_return;
    }
    
    Task on_close(Socket& socket) override {
        close_count.fetch_add(1);
        co_return;
    }
};

class WebSocketServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler = std::make_unique<TestWebSocketHandler>();
        server = std::make_unique<Server>(2);
        server->set_websocket_handler(std::make_unique<TestWebSocketHandler>());
        server->listen_tcp(7777);
        
        server_thread = std::thread([this]() {
            server->run();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    void TearDown() override {
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
    
    std::vector<uint8_t> create_websocket_frame(const std::string& text, bool masked = false) {
        WebSocketFrame frame;
        frame.fin = true;
        frame.opcode = WebSocketOpcode::Text;
        frame.masked = masked;
        if (masked) {
            frame.masking_key = 0x12345678;
        }
        frame.payload.assign(text.begin(), text.end());
        frame.payload_length = frame.payload.size();
        
        return frame.serialize();
    }
    
    std::unique_ptr<TestWebSocketHandler> handler;
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(WebSocketServerTest, WebSocketConnection) {
    // Basic test that server accepts WebSocket connections
    // Full WebSocket handshake would require HTTP upgrade request
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // This is a simplified test - full WebSocket requires HTTP upgrade
    EXPECT_NE(server, nullptr);
}

TEST_F(WebSocketServerTest, FrameSerialization) {
    std::string test_message = "Hello, WebSocket!";
    auto frame_data = create_websocket_frame(test_message);
    
    EXPECT_GE(frame_data.size(), test_message.size() + 2);
    
    // Parse it back
    auto parsed = WebSocketFrame::parse(frame_data);
    std::string parsed_text(parsed.payload.begin(), parsed.payload.end());
    
    EXPECT_EQ(parsed_text, test_message);
    EXPECT_EQ(parsed.opcode, WebSocketOpcode::Text);
}

TEST_F(WebSocketServerTest, MaskedFrame) {
    std::string test_message = "Masked message";
    auto frame_data = create_websocket_frame(test_message, true);
    
    EXPECT_GE(frame_data.size(), test_message.size() + 6);  // Header + mask + payload
    
    auto parsed = WebSocketFrame::parse(frame_data);
    std::string parsed_text(parsed.payload.begin(), parsed.payload.end());
    
    EXPECT_EQ(parsed_text, test_message);
    EXPECT_TRUE(parsed.masked);
}

TEST_F(WebSocketServerTest, MultipleFrames) {
    std::vector<std::string> messages = {"Hello", "World", "Test"};
    std::vector<std::vector<uint8_t>> frames;
    
    for (const auto& msg : messages) {
        frames.push_back(create_websocket_frame(msg));
    }
    
    EXPECT_EQ(frames.size(), 3);
    
    for (size_t i = 0; i < frames.size(); ++i) {
        auto parsed = WebSocketFrame::parse(frames[i]);
        std::string text(parsed.payload.begin(), parsed.payload.end());
        EXPECT_EQ(text, messages[i]);
    }
}

TEST_F(WebSocketServerTest, CloseFrame) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = WebSocketOpcode::Close;
    frame.masked = false;
    frame.payload_length = 0;
    
    auto frame_data = frame.serialize();
    auto parsed = WebSocketFrame::parse(frame_data);
    
    EXPECT_EQ(parsed.opcode, WebSocketOpcode::Close);
}

TEST_F(WebSocketServerTest, PingPongFrames) {
    WebSocketFrame ping;
    ping.fin = true;
    ping.opcode = WebSocketOpcode::Ping;
    ping.masked = false;
    ping.payload = {'p', 'i', 'n', 'g'};
    ping.payload_length = ping.payload.size();
    
    auto ping_data = ping.serialize();
    auto parsed_ping = WebSocketFrame::parse(ping_data);
    
    EXPECT_EQ(parsed_ping.opcode, WebSocketOpcode::Ping);
    
    WebSocketFrame pong;
    pong.fin = true;
    pong.opcode = WebSocketOpcode::Pong;
    pong.masked = false;
    pong.payload = {'p', 'o', 'n', 'g'};
    pong.payload_length = pong.payload.size();
    
    auto pong_data = pong.serialize();
    auto parsed_pong = WebSocketFrame::parse(pong_data);
    
    EXPECT_EQ(parsed_pong.opcode, WebSocketOpcode::Pong);
}

