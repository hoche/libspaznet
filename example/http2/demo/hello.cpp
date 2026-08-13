// Minimal HTTP/2 server.
//
//   $ ./http2_hello           # coroutine dispatcher (default), h2c on 8080
//   $ ./http2_hello --reactor # coroutine-free reactor dispatcher
//   $ ./http2_hello --tls     # also listen_tls on 8443 (ALPN h2)
//   $ curl --http2-prior-knowledge http://localhost:8080/
//   $ curl -k --http2 https://localhost:8443/
//   Hello, HTTP/2!

#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>
#ifdef SPAZNET_HAS_TLS
#include <libspaznet/detail/tls_self_signed.hpp>
#include <libspaznet/tls_config.hpp>
#include <iostream>
#endif

#include <memory>
#include <string>

// Same Handler under both dispatchers — no Task, no Socket, just build a
// Response and complete() it. See handler.hpp's ResponseWriter comment.
class Hello : public spaznet::http2::Handler {
  public:
    void handle_request(const spaznet::http2::Request&,
                        spaznet::http2::ResponseWriter writer) override {
        spaznet::http2::Response resp;
        resp.status_code = 200;
        resp.headers["content-type"] = "text/plain";
        const std::string body = "Hello, HTTP/2!\n";
        resp.body.assign(body.begin(), body.end());
        writer.complete(std::move(resp));
    }
};

int main(int argc, char** argv) {
#ifdef SPAZNET_HAS_COROUTINES
    bool use_reactor = false;
#else
    bool use_reactor = true; // Coroutine dispatcher isn't built in this configuration.
#endif
    bool use_tls = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--reactor") {
            use_reactor = true;
        }
        if (std::string(argv[i]) == "--tls") {
            use_tls = true;
        }
    }

    spaznet::Server server(4);
    if (use_reactor) {
        server.set_connection_factory(
            spaznet::http2::make_reactor_dispatcher(std::make_unique<Hello>()));
    }
#ifdef SPAZNET_HAS_COROUTINES
    else {
        server.set_connection_handler(
            spaznet::http2::make_dispatcher(std::make_unique<Hello>()));
    }
#endif
    server.listen_tcp(8080);
#ifdef SPAZNET_HAS_TLS
    if (use_tls) {
        auto [cert, key] = spaznet::detail::make_self_signed_pem("localhost");
        spaznet::TlsConfig cfg;
        cfg.cert_pem = std::move(cert);
        cfg.key_pem = std::move(key);
        cfg.alpn = {"h2"};
        server.listen_tls(8443, std::move(cfg));
        std::cerr << "TLS listening on https://127.0.0.1:8443/ (ALPN h2; curl -k --http2)\n";
    }
#else
    if (use_tls) {
        // Built without SPAZNET_ENABLE_TLS — ignore.
    }
#endif
    server.run();
}
