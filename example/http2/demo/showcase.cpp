// HTTP/2 feature showcase using example/http2.
//
// The centerpiece is multiplexing: the dispatcher runs each stream as
// its own detached coroutine on a single TCP connection, so several
// concurrent requests make progress in parallel instead of queueing
// behind each other the way HTTP/1.1 keep-alive requests do.
//
//   $ ./http2_showcase
//   $ curl --http2-prior-knowledge http://localhost:8080/
//
//   # The multiplexing demo — 8 requests that each sleep 1s. Over
//   # HTTP/1.1 keep-alive this would take ~8s serialized; over one
//   # multiplexed HTTP/2 connection it takes ~1s:
//   $ h2load -n8 -c1 -m8 'http://localhost:8080/slow?ms=1000'
//
//   $ curl --http2-prior-knowledge http://localhost:8080/stream-info
//   $ curl --http2-prior-knowledge --data-binary 'hello' http://localhost:8080/echo
//   $ curl --http2-prior-knowledge http://localhost:8080/status/404
//
// This build of example/http2 does NOT implement server push
// (PUSH_PROMISE), stream PRIORITY, or trailers — see the header
// comment in src/dispatcher.cpp — so this showcase sticks to features
// that are actually there: multiplexed streams, HPACK-decoded headers,
// DATA-frame request bodies, and dispatcher-enforced
// MAX_CONCURRENT_STREAMS / SETTINGS / PING / flow control.
//
// Per Handler's contract, this code only ever populates `response` —
// it never calls socket.async_write() directly (that would race with
// other streams' frames on the shared connection writer).

#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/utils/number_utils.hpp>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

using spaznet::http2::Request;
using spaznet::http2::Response;

constexpr std::string_view kIndexPage =
    "libspaznet HTTP/2 showcase\n"
    "===========================\n"
    "\n"
    "GET  /slow?ms=N     sleep N ms server-side, then report this stream's\n"
    "                    id and start/end time. Fire several of these at\n"
    "                    once on one connection (e.g. h2load -n8 -c1 -m8)\n"
    "                    to see them overlap instead of serializing.\n"
    "GET  /stream-info   echo this request's stream id, pseudo-headers,\n"
    "                    and regular headers (HPACK-decoded).\n"
    "POST /echo          echo the request body (DATA-frame reassembly).\n"
    "GET  /status/<code> respond with an arbitrary status code.\n";

// Splits an HTTP/2 ":path" pseudo-header value (which, per RFC 9113,
// carries the full path *and* query string verbatim) into the path
// component and a simple flat key=value query map. No percent-decoding
// or repeated-key handling — this is a demo, not a URL library.
struct ParsedPath {
    std::string path;
    std::unordered_map<std::string, std::string> query;
};

ParsedPath parse_path(const std::string& raw) {
    ParsedPath result;
    auto qpos = raw.find('?');
    result.path = (qpos == std::string::npos) ? raw : raw.substr(0, qpos);
    if (qpos == std::string::npos) {
        return result;
    }
    std::string query_str = raw.substr(qpos + 1);
    std::istringstream iss(query_str);
    std::string pair;
    while (std::getline(iss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            result.query[pair] = "";
        } else {
            result.query[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
    }
    return result;
}

std::string reason_phrase_for(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 418:
            return "I'm a teapot";
        case 500:
            return "Internal Server Error";
        default:
            return "Unknown Status";
    }
}

void set_text(Response& resp, std::string body) {
    resp.headers["content-type"] = "text/plain";
    resp.body.assign(body.begin(), body.end());
}

std::string now_iso_ms() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    return std::to_string(ms) + " ms since epoch";
}

spaznet::Task handle_slow(const Request& req, const ParsedPath& parsed, Response& resp,
                          spaznet::Socket& socket) {
    int delay_ms = 100;
    auto it = parsed.query.find("ms");
    if (it != parsed.query.end()) {
        if (auto parsed_ms = spaznet::NumberUtils::parse_int(it->second)) {
            delay_ms = *parsed_ms;
        }
    }
    // Clamp so a mistyped ?ms= doesn't let one stream hog the connection
    // (or the demo box) indefinitely.
    if (delay_ms < 0) {
        delay_ms = 0;
    }
    if (delay_ms > 60000) {
        delay_ms = 60000;
    }

    std::ostringstream out;
    out << "stream " << req.stream_id << ": sleeping " << delay_ms << " ms\n";
    out << "  start: " << now_iso_ms() << "\n";

    // While this stream's coroutine is suspended here, the dispatcher's
    // frame-reading loop and the other streams' handler coroutines on
    // this same connection keep running — that's the multiplexing this
    // route exists to demonstrate.
    co_await socket.context()->sleep_for(std::chrono::milliseconds(delay_ms));

    out << "  end:   " << now_iso_ms() << "\n";
    set_text(resp, out.str());
}

void handle_stream_info(const Request& req, Response& resp) {
    std::ostringstream out;
    out << "stream_id : " << req.stream_id << "\n";
    out << "method    : " << req.method << "\n";
    out << "path      : " << req.path << "\n";
    for (const char* pseudo : {":scheme", ":authority"}) {
        if (auto v = req.get_pseudo_header(pseudo)) {
            out << pseudo << "     : " << *v << "\n";
        }
    }
    out << "headers (HPACK-decoded, non-pseudo):\n";
    for (const auto& [name, value] : req.get_regular_headers()) {
        out << "  " << name << ": " << value << "\n";
    }
    set_text(resp, out.str());
}

void handle_echo(const Request& req, Response& resp) {
    resp.headers["content-type"] = "application/octet-stream";
    resp.body = req.body;
}

void handle_status(const std::string& path, Response& resp) {
    auto slash = path.rfind('/');
    auto code_opt = spaznet::NumberUtils::parse_int(path.substr(slash + 1));
    int code = code_opt.value_or(400);
    resp.set_status(code, reason_phrase_for(code));
    set_text(resp, "Responded with status " + std::to_string(code) + " " +
                       reason_phrase_for(code) + "\n");
}

} // namespace

class Showcase : public spaznet::http2::Handler {
  public:
    spaznet::Task handle_request(const spaznet::http2::Request& req,
                                 spaznet::http2::Response& resp,
                                 spaznet::Socket& socket) override {
        resp.status_code = 200;
        ParsedPath parsed = parse_path(req.path);

        if (req.method == "GET" && parsed.path == "/") {
            set_text(resp, std::string(kIndexPage));
        } else if (req.method == "GET" && parsed.path == "/slow") {
            co_await handle_slow(req, parsed, resp, socket);
        } else if (req.method == "GET" && parsed.path == "/stream-info") {
            handle_stream_info(req, resp);
        } else if (req.method == "POST" && parsed.path == "/echo") {
            handle_echo(req, resp);
        } else if (req.method == "GET" && parsed.path.rfind("/status/", 0) == 0) {
            handle_status(parsed.path, resp);
        } else {
            resp.set_status(404, "Not Found");
            set_text(resp, "No such route: " + req.method + " " + parsed.path + "\n");
        }
    }
};

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(
        spaznet::http2::make_dispatcher(std::make_unique<Showcase>()));
    server.listen_tcp(8080);
    server.run();
}
