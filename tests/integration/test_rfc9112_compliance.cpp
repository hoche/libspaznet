#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/server.hpp>
#include <string>
#include <thread>
#include <vector>

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
        // Create handler first and keep a raw pointer for test assertions
        auto handler_unique = std::make_unique<RFC9112TestHandler>();
        handler = handler_unique.get();

        server = std::make_unique<Server>(2);
        // Transfer ownership to server
        server->set_http_handler(std::move(handler_unique));

        server->listen_tcp(9996);

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
        addr.sin_port = htons(9996);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock);
            return -1;
        }

        return sock;
    }

    std::string send_request(const std::string& request) {
        int sock = connect_to_server();
        if (sock < 0)
            return "";

#ifdef _WIN32
        int sent = send(sock, request.c_str(), static_cast<int>(request.size()), 0);
        if (sent < 0) {
            close_socket(sock);
            return "";
        }
#else
        ssize_t sent = send(sock, request.c_str(), request.size(), MSG_NOSIGNAL);
        if (sent < 0 || static_cast<size_t>(sent) != request.size()) {
            close_socket(sock);
            return "";
        }

        // Set receive timeout
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        // Give server time to process
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        char buffer[4096];
        int received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        close_socket(sock);

        if (received > 0) {
            buffer[received] = '\0';
            return std::string(buffer);
        }

        return "";
    }

    std::string read_response(int sock) {
        std::string response;
        char buffer[4096];

        auto start_time = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(2);
        bool got_headers = false;
        size_t headers_end = std::string::npos;
        size_t expected_len = 0;
        bool has_content_length = false;

        while (std::chrono::steady_clock::now() - start_time < timeout) {
            int received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (received > 0) {
                buffer[received] = '\0';
                response.append(buffer, received);

                if (!got_headers) {
                    headers_end = response.find("\r\n\r\n");
                    if (headers_end != std::string::npos) {
                        got_headers = true;
                        size_t content_length_pos = response.find("Content-Length:");
                        if (content_length_pos != std::string::npos) {
                            size_t len_start = response.find(": ", content_length_pos);
                            if (len_start != std::string::npos) {
                                len_start += 2;
                                size_t len_end = response.find("\r\n", len_start);
                                if (len_end != std::string::npos) {
                                    try {
                                        expected_len = static_cast<size_t>(std::stoul(
                                            response.substr(len_start, len_end - len_start)));
                                        has_content_length = true;
                                    } catch (...) {
                                        has_content_length = false;
                                    }
                                }
                            }
                        }
                    }
                }

                if (got_headers && has_content_length) {
                    const size_t body_start = headers_end + 4;
                    if (response.size() >= body_start + expected_len) {
                        break;
                    }
                } else if (got_headers) {
                    // No Content-Length: treat headers-only as complete for these tests.
                    break;
                }
            } else if (received == 0) {
                // Connection closed by peer.
                break;
            } else {
                // Timeout or transient error.
                break;
            }
        }

        return response;
    }

    RFC9112TestHandler* handler; // Raw pointer since server owns the unique_ptr
    std::unique_ptr<Server> server;
    std::thread server_thread;
};

// Test RFC 9112 Section 3.1.1 - Request Line Format
TEST_F(RFC9112IntegrationTest, RequestLineFormat) {
    std::string request = "GET /test HTTP/1.1\r\n"
                          "Host: localhost:9996\r\n"
                          "\r\n";

    std::string response = send_request(request);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("Content-Length"), std::string::npos);
}

// Test RFC 9112 Section 5.5 - Header Fields
TEST_F(RFC9112IntegrationTest, HeaderFieldParsing) {
    std::string request = "GET /test HTTP/1.1\r\n"
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
    std::string request = "POST /test HTTP/1.1\r\n"
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
    std::string request = "POST /test HTTP/1.1\r\n"
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
    std::string request = "GET /test HTTP/1.1\r\n"
                          "Host: localhost:9996\r\n"
                          "Connection: keep-alive\r\n"
                          "\r\n";

    std::string response = send_request(request);

    EXPECT_NE(response.find("Connection: keep-alive"), std::string::npos);
}

TEST_F(RFC9112IntegrationTest, KeepAliveAllowsMultipleRequestsOnSameConnection) {
    int sock = connect_to_server();
    ASSERT_GE(sock, 0);

#ifndef _WIN32
    // Set receive timeout so tests don't hang.
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    std::string req1 = "GET /one HTTP/1.1\r\n"
                       "Host: localhost:9996\r\n"
                       "Connection: keep-alive\r\n"
                       "\r\n";
#ifdef _WIN32
    ASSERT_GT(send(sock, req1.c_str(), static_cast<int>(req1.size()), 0), 0);
#else
    ASSERT_EQ(send(sock, req1.c_str(), req1.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(req1.size()));
#endif

    std::string resp1 = read_response(sock);
    EXPECT_NE(resp1.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp1.find("Connection: keep-alive"), std::string::npos);

    std::string req2 = "GET /two HTTP/1.1\r\n"
                       "Host: localhost:9996\r\n"
                       "Connection: close\r\n"
                       "\r\n";
#ifdef _WIN32
    ASSERT_GT(send(sock, req2.c_str(), static_cast<int>(req2.size()), 0), 0);
#else
    ASSERT_EQ(send(sock, req2.c_str(), req2.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(req2.size()));
#endif

    std::string resp2 = read_response(sock);
    EXPECT_NE(resp2.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp2.find("Connection: close"), std::string::npos);

    close_socket(sock);

    // Give server a moment to record both requests.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GE(handler->request_count.load(), 2);
}

TEST_F(RFC9112IntegrationTest, ConnectionClose) {
    std::string request = "GET /test HTTP/1.1\r\n"
                          "Host: localhost:9996\r\n"
                          "Connection: close\r\n"
                          "\r\n";

    std::string response = send_request(request);

    EXPECT_NE(response.find("Connection: close"), std::string::npos);
}

// Test RFC 9112 Section 6.1 - Status Line
TEST_F(RFC9112IntegrationTest, StatusLineFormat) {
    std::string request = "GET /test HTTP/1.1\r\n"
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
        std::string request = std::string(method) + " /test HTTP/1.1\r\n"
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
    std::string request = "GET /path/to/resource?query=value HTTP/1.1\r\n"
                          "Host: localhost:9996\r\n"
                          "\r\n";

    std::string response = send_request(request);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

// Test case-insensitive header field names per RFC 9112 Section 5.1
TEST_F(RFC9112IntegrationTest, CaseInsensitiveHeaders) {
    std::string request = "GET /test HTTP/1.1\r\n"
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
    std::string request = "POST /test HTTP/1.1\r\n"
                          "Host: localhost:9996\r\n"
                          "Content-Length: 10000\r\n"
                          "\r\n" +
                          body;

    std::string response = send_request(request);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}
