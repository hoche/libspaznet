// Minimal HTTP/2 (h2c, prior-knowledge cleartext) server.
//
//   $ ./http2_hello
//   $ curl --http2-prior-knowledge http://localhost:8080/
//   Hello, HTTP/2!

#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>

#include <memory>
#include <string>

class Hello : public spaznet::http2::Handler {
  public:
    spaznet::Task handle_request(const spaznet::http2::Request&,
                                 spaznet::http2::Response& resp,
                                 spaznet::Socket&) override {
        resp.status_code = 200;
        resp.headers["content-type"] = "text/plain";
        const std::string body = "Hello, HTTP/2!\n";
        resp.body.assign(body.begin(), body.end());
        co_return;
    }
};

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(
        spaznet::http2::make_dispatcher(std::make_unique<Hello>()));
    server.listen_tcp(8080);
    server.run();
}
