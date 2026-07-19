// HTTP/1.0 vs HTTP/1.1 feature showcase using example/http.
//
// The point of this demo is to make the version-dependent differences
// between HTTP/1.0 and HTTP/1.1 *observable* by hitting the same routes
// with each protocol version:
//
//   $ ./http_showcase
//   $ curl -v --http1.0 http://localhost:8080/info   # Connection: close by default
//   $ curl -v --http1.1 http://localhost:8080/info   # Connection: keep-alive by default
//   $ curl -v --http1.1 http://localhost:8080/chunked   # Transfer-Encoding: chunked
//   $ curl -v --http1.0 http://localhost:8080/chunked   # falls back to Content-Length
//   $ curl --data-binary 'hello' http://localhost:8080/echo
//   $ curl -H 'Transfer-Encoding: chunked' --data-binary 'hello' http://localhost:8080/echo
//   $ curl -v http://localhost:8080/status/404
//
// Everything this handler needs is already decoded by example/http's
// parser (HTTPRequest::version, should_keep_alive(), is_chunked(),
// get_content_length()) — the handler itself never touches sockets or
// wire bytes directly. Features the stack doesn't implement (byte
// ranges, content negotiation/compression, 100-continue) are
// intentionally left out of this showcase.

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/utils/number_utils.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using spaznet::http::HTTPRequest;
using spaznet::http::HTTPResponse;

constexpr std::string_view kIndexPage =
    "<!DOCTYPE html><html><head><title>libspaznet HTTP/1.x showcase</title></head>\n"
    "<body style=\"font-family: monospace; max-width: 60em; margin: 2em auto;\">\n"
    "<h1>libspaznet HTTP/1.x showcase</h1>\n"
    "<p>These routes are designed to be hit with both "
    "<code>curl --http1.0</code> and <code>curl --http1.1</code> so you can "
    "see how the two versions differ.</p>\n"
    "<ul>\n"
    "<li><code>GET /info</code> — echoes the request line, headers, and "
    "the negotiated keep-alive decision. Compare "
    "<code>curl -v --http1.0 .../info</code> (closes) against "
    "<code>curl -v --http1.1 .../info</code> (keeps the connection open, "
    "so a second curl request over <code>--next</code> reuses it).</li>\n"
    "<li><code>POST /echo</code> — echoes the request body. Works with "
    "either a <code>Content-Length</code> upload (the default) or a "
    "<code>Transfer-Encoding: chunked</code> upload — try "
    "<code>curl -H 'Transfer-Encoding: chunked' --data-binary @file ...</code>.</li>\n"
    "<li><code>GET /chunked</code> — HTTP/1.1 clients get a "
    "<code>Transfer-Encoding: chunked</code> response; HTTP/1.0 has no "
    "chunked encoding, so 1.0 clients get the same body framed with "
    "<code>Content-Length</code> instead.</li>\n"
    "<li><code>GET /status/&lt;code&gt;</code> — returns that status code "
    "with a matching reason phrase, e.g. <code>/status/404</code>.</li>\n"
    "</ul>\n"
    "</body></html>\n";

// Small reason-phrase table for the /status/<code> route. Real servers
// know far more of these; this is intentionally just enough to make the
// demo legible.
std::string reason_phrase_for(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 418:
            return "I'm a teapot";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Unknown Status";
    }
}

void set_text(HTTPResponse& resp, std::string body) {
    resp.set_header("Content-Type", "text/plain");
    resp.body.assign(body.begin(), body.end());
}

void handle_info(const HTTPRequest& req, HTTPResponse& resp) {
    std::ostringstream out;
    out << "HTTP version : " << req.version << "\n";
    out << "Method       : " << req.method << "\n";
    out << "Target       : " << req.request_target << "\n";
    out << "Keep-alive   : " << (req.should_keep_alive() ? "yes" : "no")
        << " (default for HTTP/" << req.version
        << (req.get_header("Connection") ? ", overridden by Connection header)" : ", no Connection header)")
        << "\n";
    out << "Body bytes   : " << req.body.size() << "\n";
    out << "Headers:\n";
    for (const auto& [name, value] : req.headers) {
        out << "  " << name << ": " << value << "\n";
    }
    set_text(resp, out.str());
}

void handle_echo(const HTTPRequest& req, HTTPResponse& resp) {
    // Whether the client used Content-Length or chunked framing, by the
    // time handle_request runs the body is fully reassembled here —
    // example/http's parser hides that distinction from the handler.
    auto content_type = req.get_header("Content-Type");
    resp.set_header("Content-Type", content_type ? *content_type : "application/octet-stream");
    resp.set_header("X-Request-Framing", req.is_chunked() ? "chunked" : "content-length");
    resp.body = req.body;
}

void handle_chunked(const HTTPRequest& req, HTTPResponse& resp) {
    std::string body =
        "This response was generated as three logical pieces on the server.\n"
        "HTTP/1.1 has chunked transfer-encoding, so it doesn't need to know\n"
        "the total length up front.\n";
    resp.set_header("Content-Type", "text/plain");
    if (req.version == "1.1") {
        // Dispatcher notices Transfer-Encoding: chunked (via set_chunked())
        // and calls HTTPResponse::serialize_chunked() instead of serialize().
        resp.set_chunked();
        resp.body.assign(body.begin(), body.end());
    } else {
        // HTTP/1.0 predates chunked transfer-encoding (RFC 1945 has no
        // Transfer-Encoding header at all) — fall back to a plain
        // Content-Length-framed body so 1.0 clients still get the content.
        resp.body.assign(body.begin(), body.end());
    }
}

void handle_status(const std::string& target, HTTPResponse& resp) {
    auto slash = target.rfind('/');
    auto code_opt = spaznet::NumberUtils::parse_int(target.substr(slash + 1));
    int code = code_opt.value_or(400);
    resp.status_code = code;
    resp.reason_phrase = reason_phrase_for(code);
    set_text(resp, "Responded with status " + std::to_string(code) + " " + resp.reason_phrase + "\n");
}

} // namespace

class Showcase : public spaznet::http::HTTPHandler {
  public:
    spaznet::Task handle_request(const spaznet::http::HTTPRequest& req,
                                 spaznet::http::HTTPResponse& resp,
                                 spaznet::Socket&) override {
        resp.status_code = 200;
        resp.reason_phrase = "OK";

        if (req.method == "GET" && req.request_target == "/") {
            resp.set_header("Content-Type", "text/html");
            resp.body.assign(kIndexPage.begin(), kIndexPage.end());
        } else if (req.method == "GET" && req.request_target == "/info") {
            handle_info(req, resp);
        } else if (req.method == "POST" && req.request_target == "/echo") {
            handle_echo(req, resp);
        } else if (req.method == "GET" && req.request_target == "/chunked") {
            handle_chunked(req, resp);
        } else if (req.method == "GET" && req.request_target.rfind("/status/", 0) == 0) {
            handle_status(req.request_target, resp);
        } else {
            resp.status_code = 404;
            resp.reason_phrase = "Not Found";
            set_text(resp, "No such route: " + req.method + " " + req.request_target + "\n");
        }
        co_return;
    }
};

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(
        spaznet::http::make_dispatcher(std::make_unique<Showcase>()));
    server.listen_tcp(8080);
    server.run();
}
