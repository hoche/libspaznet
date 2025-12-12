#include <gtest/gtest.h>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/server.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

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

// RFC 9112 Compliant Test Handler
class RFC9112TestHandler : public HTTPHandler {
public:
    std::atomic<int> request_count{0};
    HTTPRequest last_request;
    
    Task handle_request(const HTTPRequest& request, HTTPResponse& response,
                        Socket& socket) override {
        request_count.fetch_add(1);
        last_request = request;
        
        // RFC 9112 compliant response
        response.version = "1.1";
        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "text/plain");
        response.set_header("Server", "libspaznet/1.0");
        
        // Handle different methods
        if (request.method == "GET") {
            response.body = {'G', 'E', 'T', ' ', 'O', 'K'};
        } else if (request.method == "POST") {
            response.body = {'P', 'O', 'S', 'T', ' ', 'O', 'K'};
        } else {
            response.status_code = 405;
            response.reason_phrase = "Method Not Allowed";
            response.body.clear();
        }
        
        // Set Content-Length
        response.set_content_length(response.body.size());
        
        co_return;
    }
};

class RFC9112IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler = std::make_unique<RFC9112TestHandler>();
        server = std::make_unique<Server>(2);
        server->set_http_handler(std::make_unique<RFC9112TestHandler>());
        server->listen_tcp(9996);
        
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
    
    int connect_to_server() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(9996);
        
        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return -1;
        }
        
        return sock;
    }
    
    std::string send_request(const std::string& request) {
        int sock = connect_to_server();
        if (sock < 0) return "";
        
        send(sock, request.c_str(), request.size(), 0);
        
        char buffer[4096];
        int received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        close_socket(sock);
        
        if (received > 0) {
            buffer[received] = '\0';
            return std::string(buffer);
        }
        
        return "";
    }
    
    std::unique_ptr<RFC9112TestHandler> handler;
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

// Test RFC 9112 Section 3.1.1 - Request Line Format
TEST_F(RFC9112IntegrationTest, RequestLineFormat) {
    std::string request = 
        "GET /test HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "\r\n";
    
    std::string response = send_request(request);
    
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("Content-Length"), std::string::npos);
}

// Test RFC 9112 Section 5.5 - Header Fields
TEST_F(RFC9112IntegrationTest, HeaderFieldParsing) {
    std::string request = 
        "GET /test HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "User-Agent: test-agent/1.0\r\n"
        "Accept: text/plain, text/html\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    
    std::string response = send_request(request);
    
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    // Verify handler received headers
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GE(handler->request_count.load(), 1);
}

// Test RFC 9112 Section 8.6 - Content-Length
TEST_F(RFC9112IntegrationTest, ContentLengthHandling) {
    std::string request = 
        "POST /test HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Hello World";
    
    std::string response = send_request(request);
    
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("POST OK"), std::string::npos);
}

// Test RFC 9112 Section 7.1 - Chunked Transfer Encoding
TEST_F(RFC9112IntegrationTest, ChunkedTransferEncoding) {
    std::string request = 
        "POST /test HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "6\r\n"
        " World\r\n"
        "0\r\n"
        "\r\n";
    
    std::string response = send_request(request);
    
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

// Test RFC 9112 Section 9.6 - Connection Management
TEST_F(RFC9112IntegrationTest, ConnectionKeepAlive) {
    std::string request = 
        "GET /test HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    
    std::string response = send_request(request);
    
    EXPECT_NE(response.find("Connection: keep-alive"), std::string::npos);
}

TEST_F(RFC9112IntegrationTest, ConnectionClose) {
    std::string request = 
        "GET /test HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "Connection: close\r\n"
        "\r\n";
    
    std::string response = send_request(request);
    
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
}

// Test RFC 9112 Section 6.1 - Status Line
TEST_F(RFC9112IntegrationTest, StatusLineFormat) {
    std::string request = 
        "GET /test HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "\r\n";
    
    std::string response = send_request(request);
    
    // Verify status line format: HTTP-version SP status-code SP reason-phrase
    size_t status_pos = response.find("HTTP/1.1");
    EXPECT_NE(status_pos, std::string::npos);
    
    size_t code_pos = response.find("200", status_pos);
    EXPECT_NE(code_pos, std::string::npos);
    
    size_t reason_pos = response.find("OK", code_pos);
    EXPECT_NE(reason_pos, std::string::npos);
}

// Test RFC 9112 Section 9 - HTTP Methods
TEST_F(RFC9112IntegrationTest, HTTPMethods) {
    const char* methods[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS"};
    
    for (const char* method : methods) {
        std::string request = 
            std::string(method) + " /test HTTP/1.1\r\n"
            "Host: localhost:9996\r\n"
            "\r\n";
        
        std::string response = send_request(request);
        
        // Should get a response (may be 405 for unsupported methods)
        EXPECT_FALSE(response.empty());
        EXPECT_NE(response.find("HTTP/1.1"), std::string::npos);
    }
}

// Test RFC 9112 Section 3.2 - Request Target Forms
TEST_F(RFC9112IntegrationTest, RequestTargetOriginForm) {
    std::string request = 
        "GET /path/to/resource?query=value HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "\r\n";
    
    std::string response = send_request(request);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

// Test case-insensitive header field names per RFC 9112 Section 5.1
TEST_F(RFC9112IntegrationTest, CaseInsensitiveHeaders) {
    std::string request = 
        "GET /test HTTP/1.1\r\n"
        "HOST: localhost:9996\r\n"
        "user-agent: test-agent\r\n"
        "CONTENT-TYPE: application/json\r\n"
        "\r\n";
    
    std::string response = send_request(request);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

// Test large request body
TEST_F(RFC9112IntegrationTest, LargeRequestBody) {
    std::string body(10000, 'A');
    std::string request = 
        "POST /test HTTP/1.1\r\n"
        "Host: localhost:9996\r\n"
        "Content-Length: 10000\r\n"
        "\r\n" + body;
    
    std::string response = send_request(request);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

