// End-to-end interop check: spin up the library's QUIC + HTTP/3 server
// on a real UDP socket and aim `curl --http3-only` at it.  Asserts that
// curl receives the response body we registered and exits cleanly.
//
// The test self-skips on platforms whose `curl` doesn't carry HTTP/3
// (the macOS system curl is a common example).  When SSLKEYLOGFILE is
// set in the environment, the server-side SSL_CTX wires
// SSL_CTX_set_keylog_callback so Wireshark can decrypt the traffic for
// post-mortem debugging.

#include <gtest/gtest.h>

#include "dispatcher_test_support.hpp"

#include <libspaznet/http3/service.hpp>
#include <libspaznet/quic/listener.hpp>
#include <libspaznet/quic/tls.hpp>
#include <libspaznet/server.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <libspaznet/detail/socket_compat.hpp>

#ifdef _WIN32
#include <stdio.h>
#define popen _popen
#define pclose _pclose
#endif

#include "quic_test_tls.hpp"

using namespace spaznet;
using namespace spaznet::quic::test;

namespace {

// `popen` + read-to-end + wait wrapper.  Returns (stdout, exit_status).
auto run_capture(const std::string& cmd) -> std::pair<std::string, int> {
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) return {"", -1};
    std::string out;
    std::array<char, 4096> buf{};
    while (auto n = std::fread(buf.data(), 1, buf.size(), p)) {
        out.append(buf.data(), n);
    }
    const int rc = pclose(p);
    return {std::move(out), rc};
}

// Probe the host's `curl` for HTTP/3 support.  Returns true iff
// `curl -V` lists "HTTP3" in its Features line.
auto curl_supports_http3() -> bool {
#ifdef _WIN32
    // `_popen` runs via cmd.exe; use NUL, not /dev/null.
    auto [out, rc] = run_capture("curl -V 2>NUL");
#else
    auto [out, rc] = run_capture("curl -V 2>/dev/null");
#endif
    if (rc != 0) return false;
    return out.find("HTTP3") != std::string::npos ||
           out.find("http3") != std::string::npos;
}

// Optional SSLKEYLOGFILE hook so Wireshark can decrypt the traffic
// for a failed interop run.  Append-only; we never truncate so
// developers can run several tests against the same file.
void install_keylog(quic::TlsSslCtx* ctx) {
#if defined(SPAZNET_TLS_OPENSSL)
    const char* path = std::getenv("SSLKEYLOGFILE");
    if (path == nullptr || *path == '\0') return;
    static std::string keylog_path; // captured by the lambda
    keylog_path = path;
    SSL_CTX_set_keylog_callback(ctx, [](const SSL* /*ssl*/, const char* line) {
        if (keylog_path.empty()) return;
        std::ofstream f(keylog_path, std::ios::app);
        if (f) {
            f << line << '\n';
        }
    });
#else
    (void)ctx;
#endif
}

// Bind a UDP socket to 127.0.0.1:0 just to learn the kernel-assigned
// port, then close it.  Caller passes the returned port to
// `listen_udp`.  This is a TOCTOU window — another process could grab
// the port between probe and listen — but for a unit test it's
// adequate.
auto pick_free_udp_port() -> uint16_t {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        detail::close_socket_fd(fd);
        return 0;
    }
    socklen_t slen = sizeof(sin);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sin), &slen) < 0) {
        detail::close_socket_fd(fd);
        return 0;
    }
    const uint16_t port = ntohs(sin.sin_port);
    detail::close_socket_fd(fd);
    return port;
}

} // namespace

using ::spaznet::http3::testing_support::DispatcherKind;
using ::spaznet::http3::testing_support::DispatcherKindName;
using ::spaznet::http3::testing_support::install_dispatcher;
using ::spaznet::http3::testing_support::AllDispatcherKinds;

class QuicHttp3CurlInterop : public ::testing::TestWithParam<DispatcherKind> {};

TEST_P(QuicHttp3CurlInterop, RealCurlReceivesResponseBody) {
    if (!curl_supports_http3()) {
        GTEST_SKIP() << "host curl lacks HTTP/3 support — install curl built "
                        "against an HTTP/3-capable libcurl (e.g. on Linux: "
                        "apt install --reinstall curl on a release with "
                        "experimental HTTP/3, or use `nghttp2`'s curl bundle).";
    }

    auto [cert, key] = make_test_cert_pem("localhost");
    quic::TlsServerConfig tcfg{cert, key, {"h3"}};
    auto tls_ctx = quic::TlsContext::make_server(tcfg);
    ASSERT_NE(tls_ctx, nullptr);
    install_keylog(tls_ctx->ssl_ctx());

    quic::Listener::Config lcfg;
    lcfg.tls_ctx = tls_ctx;
    lcfg.server_tp.initial_max_data = 1 << 20;
    lcfg.server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    lcfg.server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    lcfg.server_tp.initial_max_stream_data_uni = 1 << 16;
    lcfg.server_tp.initial_max_streams_bidi = 16;
    lcfg.server_tp.initial_max_streams_uni = 16;
    lcfg.server_tp.max_idle_timeout_ms = 10'000;

    const std::string body = "Hello, libspaznet!";
    auto on_request = [&](const http3::Http3Request& req) -> http3::Http3Response {
        http3::Http3Response resp;
        resp.status_code = 200;
        resp.headers.emplace_back("content-type", "text/plain");
        resp.headers.emplace_back("server", "libspaznet-interop");
        resp.body.assign(body.begin(), body.end());
        (void)req;
        return resp;
    };

    auto service = std::make_unique<http3::QuicHttp3Service>(std::move(lcfg), on_request);

    // Construct Server first so Winsock is initialised before we probe
    // for a free UDP port with raw socket()/bind()/getsockname().
    Server srv;
    const uint16_t port = pick_free_udp_port();
    ASSERT_NE(port, 0U);

    install_dispatcher(srv, std::move(service), GetParam());
    srv.listen_udp(port);

    std::thread server_thread([&]() { srv.run(); });

    // Give the server a tick to enter its run loop before launching
    // curl — without this, curl can occasionally land its first
    // datagram before the kernel has the bind / read loop wired.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::string cmd = "curl --http3-only -k -sS --max-time 8 ";
    cmd += "--resolve localhost:" + std::to_string(port) + ":127.0.0.1 ";
    cmd += "https://localhost:" + std::to_string(port) + "/";
    cmd += " 2>&1";

    auto [out, exit_status] = run_capture(cmd);
    srv.stop();
    if (server_thread.joinable()) server_thread.join();

    EXPECT_EQ(exit_status, 0)
        << "curl failed (exit=" << exit_status << "). stdout/stderr was:\n"
        << out;
    EXPECT_EQ(out, body)
        << "response body mismatch. curl emitted:\n[" << out << "]\nexpected:\n["
        << body << "]";
}

INSTANTIATE_TEST_SUITE_P(Dispatchers, QuicHttp3CurlInterop,
                         ::testing::ValuesIn(AllDispatcherKinds()),
                         DispatcherKindName);
