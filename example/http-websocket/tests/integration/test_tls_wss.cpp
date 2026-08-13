// WSS (WebSocket over TLS) integration — both dispatchers.
// Gated on SPAZNET_HAS_TLS; skipped entirely when TLS was not built in.
// Client is a small OpenSSL SSL_connect + RFC 6455 handshake/echo.

#include <gtest/gtest.h>

#ifndef SPAZNET_HAS_TLS

TEST(WsTlsSkipped, NoTlsInThisBuild) {
    GTEST_SKIP() << "SPAZNET_HAS_TLS not defined";
}

#else

#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/detail/tls_self_signed.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/tls_config.hpp>
#include <libspaznet/websocket/dispatcher.hpp>
#include <libspaznet/websocket/handler.hpp>
#include <libspaznet/websocket/reactor_handler.hpp>
#include <libspaznet/websocket/send.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dispatcher_test_support.hpp"

#ifdef _WIN32
#define close_socket closesocket
#else
#define close_socket ::close
#endif

using spaznet::websocket::testing_support::AllDispatcherKinds;
using spaznet::websocket::testing_support::DispatcherKind;
using spaznet::websocket::testing_support::DispatcherKindName;

namespace {

#ifdef SPAZNET_HAS_COROUTINES
class EchoWS : public spaznet::websocket::Handler {
  public:
    spaznet::Task on_open(spaznet::websocket::Connection&) override {
        co_return;
    }
    spaznet::Task on_close(spaznet::websocket::Connection&) override {
        co_return;
    }
    spaznet::Task handle_message(const spaznet::websocket::Message& m,
                                 spaznet::websocket::Connection& conn) override {
        co_await conn.send(m.opcode, m.data);
    }
};
#endif

class EchoWSReactor : public spaznet::websocket::reactor::Handler {
  public:
    void on_open(spaznet::websocket::reactor::Connection&) override {}
    void on_close(spaznet::websocket::reactor::Connection&) override {}
    void handle_message(const spaznet::websocket::Message& m,
                        spaznet::websocket::reactor::Connection& conn) override {
        conn.send(m.opcode, m.data);
    }
};

struct SslClient {
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    int fd{-1};

    ~SslClient() {
        close();
    }

    void close() {
        if (ssl != nullptr) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        if (fd >= 0) {
            close_socket(fd);
            fd = -1;
        }
    }

    auto connect_tls(uint16_t port) -> bool {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            return false;
        }
        spaznet::detail::setsockopt_rcvtimeo_ms(fd, 3000);

        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx == nullptr) {
            return false;
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn));

        ssl = SSL_new(ctx);
        if (ssl == nullptr) {
            return false;
        }
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) != 1) {
            return false;
        }
        return true;
    }

    auto write_all(const void* data, std::size_t len) -> bool {
        const auto* p = static_cast<const uint8_t*>(data);
        std::size_t sent = 0;
        while (sent < len) {
            int n = SSL_write(ssl, p + sent, static_cast<int>(len - sent));
            if (n <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    auto read_some(void* buf, int len) -> int {
        return SSL_read(ssl, buf, len);
    }

    auto read_exact(std::vector<uint8_t>& out, std::size_t n) -> bool {
        out.assign(n, 0);
        std::size_t got = 0;
        while (got < n) {
            int r = SSL_read(ssl, out.data() + got, static_cast<int>(n - got));
            if (r <= 0) {
                return false;
            }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }
};

auto handshake_request(const std::string& key) -> std::string {
    std::ostringstream oss;
    oss << "GET / HTTP/1.1\r\n";
    oss << "Host: localhost\r\n";
    oss << "Upgrade: websocket\r\n";
    oss << "Connection: Upgrade\r\n";
    oss << "Sec-WebSocket-Key: " << key << "\r\n";
    oss << "Sec-WebSocket-Version: 13\r\n\r\n";
    return oss.str();
}

auto make_masked_text_frame(const std::string& text, uint32_t mask) -> std::vector<uint8_t> {
    spaznet::websocket::Frame frame;
    frame.fin = true;
    frame.opcode = spaznet::websocket::Opcode::Text;
    frame.masked = true;
    frame.masking_key = mask;
    frame.payload.assign(text.begin(), text.end());
    frame.payload_length = frame.payload.size();
    return frame.serialize();
}

auto read_unmasked_text_payload(SslClient& c) -> std::string {
    std::vector<uint8_t> header;
    if (!c.read_exact(header, 2)) {
        throw std::runtime_error("header");
    }
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
        std::vector<uint8_t> ext;
        if (!c.read_exact(ext, 2)) {
            throw std::runtime_error("ext16");
        }
        len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        std::vector<uint8_t> ext;
        if (!c.read_exact(ext, 8)) {
            throw std::runtime_error("ext64");
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | ext[static_cast<std::size_t>(i)];
        }
    }
    if (masked) {
        std::vector<uint8_t> mask_bytes;
        if (!c.read_exact(mask_bytes, 4)) {
            throw std::runtime_error("mask");
        }
    }
    std::vector<uint8_t> payload;
    if (len > 0 && !c.read_exact(payload, static_cast<std::size_t>(len))) {
        throw std::runtime_error("payload");
    }
    return std::string(payload.begin(), payload.end());
}

} // namespace

class WsTlsTest : public ::testing::TestWithParam<DispatcherKind> {
  protected:
    static constexpr uint16_t kPort = 18445;

    void SetUp() override {
        auto [cert, key] = spaznet::detail::make_self_signed_pem("localhost");
        spaznet::TlsConfig cfg;
        cfg.cert_pem = std::move(cert);
        cfg.key_pem = std::move(key);
        cfg.alpn = {"http/1.1"};

        server_ = std::make_unique<spaznet::Server>(
            GetParam() == DispatcherKind::Reactor ? 0 : 2);
        if (GetParam() == DispatcherKind::Reactor) {
            server_->set_connection_factory(spaznet::websocket::make_reactor_dispatcher(
                nullptr, std::make_unique<EchoWSReactor>()));
        }
#ifdef SPAZNET_HAS_COROUTINES
        else {
            server_->set_connection_handler(spaznet::websocket::make_dispatcher(
                nullptr, std::make_unique<EchoWS>()));
        }
#endif
        server_->listen_tls(kPort, std::move(cfg));
        thread_ = std::thread([this]() { server_->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::unique_ptr<spaznet::Server> server_;
    std::thread thread_;
};

TEST_P(WsTlsTest, HandshakeAndEcho) {
    SslClient client;
    ASSERT_TRUE(client.connect_tls(kPort));

    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    auto req = handshake_request(key);
    ASSERT_TRUE(client.write_all(req.data(), req.size()));

    std::string resp_str;
    char buf[256]{};
    for (int i = 0; i < 32 && resp_str.find("\r\n\r\n") == std::string::npos; ++i) {
        int r = client.read_some(buf, sizeof(buf));
        ASSERT_GT(r, 0);
        resp_str.append(buf, buf + r);
    }
    EXPECT_NE(resp_str.find("101 Switching Protocols"), std::string::npos);
    EXPECT_NE(resp_str.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);

    auto frame = make_masked_text_frame("wss-ok", 0x01020304);
    ASSERT_TRUE(client.write_all(frame.data(), frame.size()));

    std::string echoed = read_unmasked_text_payload(client);
    EXPECT_EQ(echoed, "wss-ok");
}

INSTANTIATE_TEST_SUITE_P(Dispatchers, WsTlsTest, ::testing::ValuesIn(AllDispatcherKinds()),
                         DispatcherKindName);

#endif // SPAZNET_HAS_TLS
