// Minimal WebSocket echo server (with an HTTP/1.1 fallback for plain
// requests on the same port).
//
//   $ ./ws_echo
//   $ wscat -c ws://localhost:8080/
//   > hi
//   < hi

#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/websocket/dispatcher.hpp>
#include <libspaznet/websocket/handler.hpp>
#include <libspaznet/websocket/send.hpp>

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

class Echo : public spaznet::websocket::Handler {
  public:
    spaznet::Task on_open(spaznet::websocket::Connection&) override { co_return; }
    spaznet::Task on_close(spaznet::websocket::Connection&) override { co_return; }

    spaznet::Task handle_message(const spaznet::websocket::Message& m,
                                 spaznet::websocket::Connection& conn) override {
        co_await conn.send(m.opcode, m.data);
    }
};

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(spaznet::websocket::make_dispatcher(
        std::make_unique<HttpFallback>(), std::make_unique<Echo>()));
    server.listen_tcp(8080);
    server.run();
}
