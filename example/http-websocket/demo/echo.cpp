// Minimal WebSocket echo server (with an HTTP/1.1 fallback for plain
// requests on the same port).
//
//   $ ./ws_echo [--reactor]
//   $ ./ws_echo --tls           # also listen_tls on 8443 (ALPN http/1.1)
//   $ wscat -c ws://localhost:8080/
//   $ wscat -c wss://localhost:8443/ --no-check
//   > hi
//   < hi
//
// --reactor selects the coroutine-free dispatcher (EchoReactor) instead
// of the default coroutine one (Echo); both speak the identical protocol.

#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/websocket/dispatcher.hpp>
#include <libspaznet/websocket/handler.hpp>
#include <libspaznet/websocket/reactor_handler.hpp>
#include <libspaznet/websocket/send.hpp>
#ifdef SPAZNET_HAS_TLS
#include <libspaznet/detail/tls_self_signed.hpp>
#include <libspaznet/tls_config.hpp>
#include <iostream>
#endif

#include <cstring>
#include <memory>

class HttpFallback : public spaznet::http::HTTPHandler {
  public:
    void handle_request(const spaznet::http::HTTPRequest&, spaznet::http::ResponseWriter writer) override {
        spaznet::http::HTTPResponse resp;
        resp.status_code = 200;
        resp.reason_phrase = "OK";
        resp.set_header("Content-Type", "text/plain");
        const char body[] = "Try a WebSocket upgrade.\n";
        resp.body.assign(body, body + sizeof(body) - 1);
        writer.complete(std::move(resp));
    }
};

#ifdef SPAZNET_HAS_COROUTINES
class Echo : public spaznet::websocket::coroutine::Handler {
  public:
    spaznet::Task on_open(spaznet::websocket::coroutine::Connection&) override { co_return; }
    spaznet::Task on_close(spaznet::websocket::coroutine::Connection&) override { co_return; }

    spaznet::Task handle_message(const spaznet::websocket::Message& m,
                                 spaznet::websocket::coroutine::Connection& conn) override {
        co_await conn.send(m.opcode, m.data);
    }
};
#endif // SPAZNET_HAS_COROUTINES

// Coroutine-free counterpart of Echo: same behavior, no co_await --
// conn.send() writes straight into the connection's OutputBuffer, so
// there's nothing to suspend on.
class EchoReactor : public spaznet::websocket::reactor::Handler {
  public:
    void on_open(spaznet::websocket::reactor::Connection&) override {}
    void on_close(spaznet::websocket::reactor::Connection&) override {}

    void handle_message(const spaznet::websocket::Message& m,
                        spaznet::websocket::reactor::Connection& conn) override {
        conn.send(m.opcode, m.data);
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
    if (use_reactor) {
        server.set_reactor_connection_factory(spaznet::websocket::make_reactor_dispatcher(
            std::make_unique<HttpFallback>(), std::make_unique<EchoReactor>()));
    }
#ifdef SPAZNET_HAS_COROUTINES
    else {
        server.set_coroutine_connection_handler(spaznet::websocket::make_coroutine_dispatcher(
            std::make_unique<HttpFallback>(), std::make_unique<Echo>()));
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
        std::cerr << "TLS listening on wss://127.0.0.1:8443/ (self-signed)\n";
    }
#else
    (void)use_tls;
#endif
    server.run();
}
