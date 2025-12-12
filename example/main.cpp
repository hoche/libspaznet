#include <libspaznet/server.hpp>
#include <libspaznet/http_handler.hpp>
#include <libspaznet/udp_handler.hpp>
#include <libspaznet/websocket_handler.hpp>
#include <iostream>
#include <string>

// Example HTTP handler
class ExampleHTTPHandler : public spaznet::HTTPHandler {
public:
    spaznet::Task handle_request(
        const spaznet::HTTPRequest& request,
        spaznet::HTTPResponse& response,
        spaznet::Socket& socket
    ) override {
        std::cout << "HTTP Request: " << request.method << " " << request.path << std::endl;
        
        response.status_code = 200;
        response.status_message = "OK";
        response.set_header("Content-Type", "text/html");
        
        std::string body = "<html><body><h1>Hello from libspaznet!</h1>";
        body += "<p>Method: " + request.method + "</p>";
        body += "<p>Path: " + request.path + "</p>";
        body += "</body></html>";
        
        response.body.assign(body.begin(), body.end());
        
        co_return;
    }
};

// Example UDP handler
class ExampleUDPHandler : public spaznet::UDPHandler {
public:
    spaznet::Task handle_packet(
        const spaznet::UDPPacket& packet,
        spaznet::Socket& socket
    ) override {
        std::cout << "UDP Packet from " << packet.address << ":" << packet.port << std::endl;
        std::cout << "Data: " << std::string(packet.data.begin(), packet.data.end()) << std::endl;
        co_return;
    }
};

// Example WebSocket handler
class ExampleWebSocketHandler : public spaznet::WebSocketHandler {
public:
    spaznet::Task on_open(spaznet::Socket& socket) override {
        std::cout << "WebSocket connection opened" << std::endl;
        co_return;
    }
    
    spaznet::Task handle_message(
        const spaznet::WebSocketMessage& message,
        spaznet::Socket& socket
    ) override {
        if (message.opcode == spaznet::WebSocketOpcode::Text) {
            std::string text(message.data.begin(), message.data.end());
            std::cout << "WebSocket message: " << text << std::endl;
            
            // Echo back
            spaznet::WebSocketMessage echo;
            echo.opcode = spaznet::WebSocketOpcode::Text;
            echo.data = message.data;
            
            spaznet::WebSocketFrame frame;
            frame.fin = true;
            frame.opcode = echo.opcode;
            frame.masked = false;
            frame.payload = echo.data;
            frame.payload_length = frame.payload.size();
            
            auto frame_data = frame.serialize();
            co_await socket.async_write(frame_data);
        }
        co_return;
    }
    
    spaznet::Task on_close(spaznet::Socket& socket) override {
        std::cout << "WebSocket connection closed" << std::endl;
        co_return;
    }
};

int main() {
    try {
        spaznet::Server server(4);  // 4 worker threads
        
        // Set handlers
        server.set_http_handler(std::make_unique<ExampleHTTPHandler>());
        server.set_udp_handler(std::make_unique<ExampleUDPHandler>());
        server.set_websocket_handler(std::make_unique<ExampleWebSocketHandler>());
        
        // Start listening
        std::cout << "Starting server on port 8080..." << std::endl;
        server.listen_tcp(8080);
        server.listen_udp(8080);
        
        // Run the server
        server.run();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}

