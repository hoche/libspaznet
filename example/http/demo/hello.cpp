// Minimal HTTP/1.1 server using example/http.
//
//   $ ./http_hello           # coroutine dispatcher (default)
//   $ ./http_hello --reactor # coroutine-free reactor dispatcher
//   $ ./http_hello --tls     # also listen_tls on 8443 (ALPN http/1.1)
//   $ curl http://localhost:8080/
//   $ curl -k https://localhost:8443/
//   Hello, libspaznet!

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>
#ifdef SPAZNET_HAS_TLS
#include <libspaznet/detail/tls_self_signed.hpp>
#include <libspaznet/tls_config.hpp>
#include <iostream>
#endif

#include <cstring>
#include <memory>

class Hello : public spaznet::http::HTTPHandler {
  public:
    void handle_request(const spaznet::http::HTTPRequest&, spaznet::http::ResponseWriter writer) override {
        spaznet::http::HTTPResponse resp;
        resp.status_code = 200;
        resp.reason_phrase = "OK";
        resp.set_header("Content-Type", "text/plain");
        const char body[] = "Hello, libspaznet!\n";
        resp.body.assign(body, body + sizeof(body) - 1);
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
        if (std::strcmp(argv[i], "--reactor") == 0) {
            use_reactor = true;
        }
        if (std::strcmp(argv[i], "--tls") == 0) {
            use_tls = true;
        }
    }

    spaznet::Server server(4);
    // Same handler, same protocol, same port either way — the two
    // dispatchers are interchangeable from a client's point of view.
    if (use_reactor) {
        server.set_reactor_connection_factory(
            spaznet::http::make_reactor_dispatcher(std::make_unique<Hello>()));
    }
#ifdef SPAZNET_HAS_COROUTINES
    else {
        server.set_coroutine_connection_handler(
            spaznet::http::make_coroutine_dispatcher(std::make_unique<Hello>()));
    }
#endif
    server.listen_tcp(8080);
#ifdef SPAZNET_HAS_TLS
    if (use_tls) {
        auto [cert, key] = spaznet::detail::make_self_signed_pem("localhost");
        spaznet::TlsConfig cfg;
        cfg.cert_pem = std::move(cert);
        cfg.key_pem = std::move(key);
        cfg.alpn = {"http/1.1"};
        server.listen_tls(8443, std::move(cfg));
        std::cerr << "TLS listening on https://127.0.0.1:8443/ (self-signed; curl -k)\n";
    }
#else
    if (use_tls) {
        // Built without SPAZNET_ENABLE_TLS — ignore.
    }
#endif
    server.run();
}
