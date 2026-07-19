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

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

using namespace spaznet;

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

// Generate a self-signed P-256 cert + private key as PEM.  No third-party
// CLI — straight OpenSSL EVP / X509 calls.
auto make_self_signed() -> std::pair<std::string, std::string> {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME* nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x, nm);
    // SAN: DNS:localhost + IP:127.0.0.1 — curl-on-h3 checks SAN even
    // with -k for the SNI/destination match in some libcurl builds.
    X509_EXTENSION* san_ext = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_subject_alt_name,
        const_cast<char*>("DNS:localhost,IP:127.0.0.1"));
    if (san_ext != nullptr) {
        X509_add_ext(x, san_ext, -1);
        X509_EXTENSION_free(san_ext);
    }
    X509_sign(x, pkey, EVP_sha256());

    BIO* cb = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cb, x);
    char* cdata = nullptr;
    long clen = BIO_get_mem_data(cb, &cdata);
    std::string cpem(cdata, static_cast<std::size_t>(clen));
    BIO* kb = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* kdata = nullptr;
    long klen = BIO_get_mem_data(kb, &kdata);
    std::string kpem(kdata, static_cast<std::size_t>(klen));
    BIO_free(cb);
    BIO_free(kb);
    X509_free(x);
    EVP_PKEY_free(pkey);
    return {cpem, kpem};
}

// Optional SSLKEYLOGFILE hook so Wireshark can decrypt the traffic
// for a failed interop run.  Append-only; we never truncate so
// developers can run several tests against the same file.
void install_keylog(SSL_CTX* ctx) {
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

TEST(QuicHttp3CurlInterop, RealCurlReceivesResponseBody) {
    if (!curl_supports_http3()) {
        GTEST_SKIP() << "host curl lacks HTTP/3 support — install curl built "
                        "against an HTTP/3-capable libcurl (e.g. on Linux: "
                        "apt install --reinstall curl on a release with "
                        "experimental HTTP/3, or use `nghttp2`'s curl bundle).";
    }

    auto [cert, key] = make_self_signed();
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
    auto dispatcher = http3::make_dispatcher(std::move(service));

    // Construct Server first so Winsock is initialised before we probe
    // for a free UDP port with raw socket()/bind()/getsockname().
    Server srv;
    const uint16_t port = pick_free_udp_port();
    ASSERT_NE(port, 0U);

    srv.set_datagram_handler(std::move(dispatcher));
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
