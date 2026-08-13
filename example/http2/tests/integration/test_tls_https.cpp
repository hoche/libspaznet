// HTTPS (TLS-over-TCP) integration for HTTP/2 — both dispatchers.
// Gated on SPAZNET_HAS_TLS; skipped entirely when TLS was not built in.

#include <gtest/gtest.h>

#ifndef SPAZNET_HAS_TLS

TEST(Http2TlsSkipped, NoTlsInThisBuild) {
    GTEST_SKIP() << "SPAZNET_HAS_TLS not defined";
}

#else

#include <libspaznet/detail/tls_self_signed.hpp>
#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/tls_config.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "dispatcher_test_support.hpp"

using spaznet::http2::testing_support::AllDispatcherKinds;
using spaznet::http2::testing_support::DispatcherKind;
using spaznet::http2::testing_support::DispatcherKindName;
using spaznet::http2::testing_support::install_dispatcher;

namespace {

class TlsHello : public spaznet::http2::Handler {
  public:
    void handle_request(const spaznet::http2::Request&,
                        spaznet::http2::ResponseWriter writer) override {
        spaznet::http2::Response resp;
        resp.status_code = 200;
        resp.headers["content-type"] = "text/plain";
        const std::string body = "h2-tls-ok";
        resp.body.assign(body.begin(), body.end());
        writer.complete(std::move(resp));
    }
};

auto curl_http2_available() -> bool {
#if defined(_WIN32)
    auto* pipe = _popen("curl -V 2>NUL", "r");
#else
    auto* pipe = popen("curl -V 2>/dev/null", "r");
#endif
    if (pipe == nullptr) {
        return false;
    }
    std::string out;
    std::array<char, 256> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        out += buf.data();
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    // libcurl Features line lists HTTP2 when nghttp2 is linked.
    return out.find("HTTP2") != std::string::npos || out.find("http2") != std::string::npos;
}

auto curl_get_h2(const std::string& url) -> std::string {
    // -k: accept self-signed; --http2: negotiate ALPN h2.
    std::string cmd = "curl -sk --http2 --max-time 5 " + url + " 2>/dev/null";
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return {};
    }
    std::string out;
    std::array<char, 256> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        out += buf.data();
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

} // namespace

class Http2TlsTest : public ::testing::TestWithParam<DispatcherKind> {
  protected:
    static constexpr uint16_t kPort = 18444;

    void SetUp() override {
        auto [cert, key] = spaznet::detail::make_self_signed_pem("localhost");
        spaznet::TlsConfig cfg;
        cfg.cert_pem = std::move(cert);
        cfg.key_pem = std::move(key);
        cfg.alpn = {"h2"};

        server_ = std::make_unique<spaznet::Server>(
            GetParam() == DispatcherKind::Reactor ? 0 : 2);
        install_dispatcher(*server_, std::make_unique<TlsHello>(), GetParam());
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

TEST_P(Http2TlsTest, CurlHttpsHttp2Get) {
    if (!curl_http2_available()) {
        GTEST_SKIP() << "curl with HTTP2 support not available";
    }
    auto body = curl_get_h2("https://127.0.0.1:" + std::to_string(kPort) + "/");
    EXPECT_EQ(body, "h2-tls-ok");
}

INSTANTIATE_TEST_SUITE_P(Dispatchers, Http2TlsTest, ::testing::ValuesIn(AllDispatcherKinds()),
                         DispatcherKindName);

#endif // SPAZNET_HAS_TLS
