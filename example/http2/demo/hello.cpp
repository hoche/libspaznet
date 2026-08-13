// Minimal HTTP/2 (h2c, prior-knowledge cleartext) server.
//
//   $ ./http2_hello           # coroutine dispatcher (default)
//   $ ./http2_hello --reactor # coroutine-free reactor dispatcher
//   $ curl --http2-prior-knowledge http://localhost:8080/
//   Hello, HTTP/2!

#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>

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
    bool use_reactor = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--reactor") {
            use_reactor = true;
        }
    }

    spaznet::Server server(4);
    if (use_reactor) {
        server.set_connection_factory(
            spaznet::http2::make_reactor_dispatcher(std::make_unique<Hello>()));
    } else {
        server.set_connection_handler(
            spaznet::http2::make_dispatcher(std::make_unique<Hello>()));
    }
    server.listen_tcp(8080);
    server.run();
}
