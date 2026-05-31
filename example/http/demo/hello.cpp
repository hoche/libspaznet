// Minimal HTTP/1.1 server using example/http.
//
//   $ ./http_hello
//   $ curl http://localhost:8080/
//   Hello, libspaznet!

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>

#include <memory>

class Hello : public spaznet::http::HTTPHandler {
  public:
    spaznet::Task handle_request(const spaznet::http::HTTPRequest&,
                                 spaznet::http::HTTPResponse& resp,
                                 spaznet::Socket&) override {
        resp.status_code = 200;
        resp.reason_phrase = "OK";
        resp.set_header("Content-Type", "text/plain");
        const char body[] = "Hello, libspaznet!\n";
        resp.body.assign(body, body + sizeof(body) - 1);
        co_return;
    }
};

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(
        spaznet::http::make_dispatcher(std::make_unique<Hello>()));
    server.listen_tcp(8080);
    server.run();
}
