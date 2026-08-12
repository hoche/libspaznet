// Minimal HTTP/1.1 server using example/http.
//
//   $ ./http_hello           # coroutine dispatcher (default)
//   $ ./http_hello --reactor # coroutine-free reactor dispatcher
//   $ curl http://localhost:8080/
//   Hello, libspaznet!

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>

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
    bool use_reactor = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reactor") == 0) {
            use_reactor = true;
        }
    }

    spaznet::Server server(4);
    // Same handler, same protocol, same port either way — the two
    // dispatchers are interchangeable from a client's point of view.
    if (use_reactor) {
        server.set_connection_factory(
            spaznet::http::make_reactor_dispatcher(std::make_unique<Hello>()));
    } else {
        server.set_connection_handler(
            spaznet::http::make_dispatcher(std::make_unique<Hello>()));
    }
    server.listen_tcp(8080);
    server.run();
}
