#include <gtest/gtest.h>
#include <libspaznet/server.hpp>
#include <libspaznet/http_handler.hpp>
#include <thread>
#include <chrono>
#include <string>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#define inet_addr(x) inet_addr(x)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define close_socket ::close
#endif

using namespace spaznet;

class TestHTTPHandler : public HTTPHandler {
public:
    std::atomic<int> request_count{0};
    std::string last_method;
    std::string last_path;
    
    Task handle_request(
        const HTTPRequest& request,
        HTTPResponse& response,
        Socket& socket
    ) override {
        request_count.fetch_add(1);
        last_method = request.method;
        last_path = request.path;
        
        response.status_code = 200;
        response.status_message = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'H', 'e', 'l', 'l', 'o'};
        
        co_return;
    }
};

class HTTPServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler = std::make_unique<TestHTTPHandler>();
        server = std::make_unique<Server>(2);
        server->set_http_handler(std::make_unique<TestHTTPHandler>());
        server->listen_tcp(8888);
        
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
    
    std::string send_http_request(const std::string& method, const std::string& path) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return "";
        
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(8888);
        
        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return "";
        }
        
        std::ostringstream request;
        request << method << " " << path << " HTTP/1.1\r\n";
        request << "Host: localhost\r\n";
        request << "\r\n";
        
        std::string req_str = request.str();
        send(sock, req_str.c_str(), req_str.size(), 0);
        
        char buffer[4096];
        int received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        close_socket(sock);
        
        if (received > 0) {
            buffer[received] = '\0';
            return std::string(buffer);
        }
        
        return "";
    }
    
    std::unique_ptr<TestHTTPHandler> handler;
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

TEST_F(HTTPServerTest, HandleGETRequest) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::string response = send_http_request("GET", "/test");
    
    // Response should contain status line
    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("OK"), std::string::npos);
}

TEST_F(HTTPServerTest, HandlePOSTRequest) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::string response = send_http_request("POST", "/api/data");
    
    EXPECT_NE(response.find("200"), std::string::npos);
}

TEST_F(HTTPServerTest, MultipleRequests) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    for (int i = 0; i < 5; ++i) {
        std::string response = send_http_request("GET", "/test" + std::to_string(i));
        EXPECT_NE(response.find("200"), std::string::npos);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

TEST_F(HTTPServerTest, ResponseHeaders) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::string response = send_http_request("GET", "/");
    
    EXPECT_NE(response.find("Content-Type"), std::string::npos);
    EXPECT_NE(response.find("Content-Length"), std::string::npos);
}

TEST_F(HTTPServerTest, ResponseBody) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::string response = send_http_request("GET", "/");
    
    EXPECT_NE(response.find("Hello"), std::string::npos);
}

