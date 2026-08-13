// HTTPS (TLS-over-TCP) integration for HTTP/1.1 — both dispatchers.
// Gated on SPAZNET_HAS_TLS; skipped entirely when TLS was not built in.

#include <gtest/gtest.h>

#ifndef SPAZNET_HAS_TLS

TEST(HttpTlsSkipped, NoTlsInThisBuild) {
    GTEST_SKIP() << "SPAZNET_HAS_TLS not defined";
}

#else

#include <libspaznet/detail/tls_self_signed.hpp>
#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/tls_config.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "dispatcher_test_support.hpp"

using spaznet::http::testing_support::AllDispatcherKinds;
using spaznet::http::testing_support::DispatcherKind;
using spaznet::http::testing_support::DispatcherKindName;
using spaznet::http::testing_support::install_dispatcher;

namespace {

class TlsHello : public spaznet::http::HTTPHandler {
  public:
    void handle_request(const spaznet::http::HTTPRequest&,
                        spaznet::http::ResponseWriter writer) override {
        spaznet::http::HTTPResponse resp;
        resp.status_code = 200;
        resp.reason_phrase = "OK";
        resp.set_header("Content-Type", "text/plain");
        const char body[] = "tls-ok";
        resp.body.assign(body, body + sizeof(body) - 1);
        writer.complete(std::move(resp));
    }
};

auto curl_available() -> bool {
#if defined(_WIN32)
    auto* pipe = _popen("curl -V 2>NUL", "r");
#else
    auto* pipe = popen("curl -V 2>/dev/null", "r");
#endif
    if (pipe == nullptr) {
        return false;
    }
    std::array<char, 256> buf{};
    bool ok = false;
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        ok = true;
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return ok;
}

auto curl_get(const std::string& url) -> std::string {
    // -k: accept self-signed; --http1.1: force ALPN http/1.1 path.
    std::string cmd = "curl -sk --http1.1 --max-time 5 " + url + " 2>/dev/null";
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

class HttpTlsTest : public ::testing::TestWithParam<DispatcherKind> {
  protected:
    static constexpr uint16_t kPort = 18443;

    void SetUp() override {
        auto [cert, key] = spaznet::detail::make_self_signed_pem("localhost");
        spaznet::TlsConfig cfg;
        cfg.cert_pem = std::move(cert);
        cfg.key_pem = std::move(key);
        cfg.alpn = {"http/1.1"};

        server_ = std::make_unique<spaznet::Server>(2);
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

TEST_P(HttpTlsTest, CurlHttpsGet) {
    if (!curl_available()) {
        GTEST_SKIP() << "curl not available";
    }
    auto body = curl_get("https://127.0.0.1:" + std::to_string(kPort) + "/");
    EXPECT_EQ(body, "tls-ok");
}

INSTANTIATE_TEST_SUITE_P(Dispatchers, HttpTlsTest, ::testing::ValuesIn(AllDispatcherKinds()),
                         DispatcherKindName);

#endif // SPAZNET_HAS_TLS
