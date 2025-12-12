#include <gtest/gtest.h>
#include <chrono>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/server.hpp>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_socket closesocket
#define inet_addr(x) inet_addr(x)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define close_socket ::close
#endif

using namespace spaznet;

class TestHTTPHandler : public HTTPHandler {
  public:
    std::atomic<int> request_count{0};
    std::string last_method;
    std::string last_path;

    Task handle_request(const HTTPRequest& request, HTTPResponse& response,
                        Socket& socket) override {
        request_count.fetch_add(1);
        last_method = request.method;
        last_path = request.request_target;

        response.status_code = 200;
        response.reason_phrase = "OK";
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

        server_thread = std::thread([this]() { server->run(); });
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
        if (sock < 0)
            return "";

        struct sockaddr_in addr {};
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
#ifdef _WIN32
        int sent = send(sock, req_str.c_str(), static_cast<int>(req_str.size()), 0);
        if (sent < 0) {
            close_socket(sock);
            return "";
        }
#else
        // Use MSG_NOSIGNAL to prevent SIGPIPE on closed sockets
        ssize_t sent = send(sock, req_str.c_str(), req_str.size(), MSG_NOSIGNAL);
        if (sent < 0 || static_cast<size_t>(sent) != req_str.size()) {
            close_socket(sock);
            return "";
        }

        // Set receive timeout
        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        // Give the server time to process async coroutines and send response
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Read response - keep reading until we get complete response or timeout
        std::string response;
        char buffer[4096];

        // Use a loop with timeout to handle async server delays
        auto start_time = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(3);
        bool got_headers = false;

        while (std::chrono::steady_clock::now() - start_time < timeout) {
            int received = recv(sock, buffer, sizeof(buffer) - 1, 0);

            if (received > 0) {
                buffer[received] = '\0';
                response.append(buffer, received);

                // Check if we have headers
                if (!got_headers && response.find("\r\n\r\n") != std::string::npos) {
                    got_headers = true;
                }

                // If we have headers, check if we need more body
                if (got_headers) {
                    size_t headers_end = response.find("\r\n\r\n");
                    size_t content_length_pos = response.find("Content-Length:");

                    if (content_length_pos != std::string::npos) {
                        size_t len_start = response.find(": ", content_length_pos) + 2;
                        size_t len_end = response.find("\r\n", len_start);
                        if (len_end != std::string::npos) {
                            try {
                                int expected_len =
                                    std::stoi(response.substr(len_start, len_end - len_start));
                                size_t body_start = headers_end + 4;
                                int body_received = static_cast<int>(response.size() - body_start);

                                if (body_received >= expected_len) {
                                    // Complete response received
                                    break;
                                }
                            } catch (...) {
                                // Invalid Content-Length, assume complete
                                break;
                            }
                        } else {
                            break;
                        }
                    } else {
                        // No Content-Length, assume response is complete after headers
                        break;
                    }
                }
            } else if (received == 0) {
                // Connection closed - use what we have
                break;
            } else {
                // Error or would block - wait a bit and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        close_socket(sock);
        return response;
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

    // Debug output
    std::cout << "Response length: " << response.length() << std::endl;
    if (response.length() > 0) {
        std::cout << "Response (first 500 chars): "
                  << response.substr(0, std::min(500UL, response.length())) << std::endl;
    } else {
        std::cout << "Response is empty!" << std::endl;
    }

    EXPECT_NE(response.find("Hello"), std::string::npos);
}
