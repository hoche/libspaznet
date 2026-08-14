// HTTP/2 feature showcase using example/http2.
//
// The centerpiece is multiplexing: both dispatchers below run every
// stream's handler independently of the others on a single TCP
// connection, so several concurrent requests make progress in parallel
// instead of queueing behind each other the way HTTP/1.1 keep-alive
// requests do — the coroutine dispatcher via a detached per-stream
// coroutine, the reactor dispatcher via a synchronous call per stream
// that never blocks the frame-reading loop (see
// libspaznet/http2/handler.hpp and dispatcher_reactor.cpp).
//
//   $ ./http2_showcase           # coroutine dispatcher (default)
//   $ ./http2_showcase --reactor # coroutine-free reactor dispatcher
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
// comment in src/dispatcher_coroutine.cpp — so this showcase sticks to features
// that are actually there: multiplexed streams, HPACK-decoded headers,
// DATA-frame request bodies, and dispatcher-enforced
// MAX_CONCURRENT_STREAMS / SETTINGS / PING / flow control.
//
// Per Handler's contract, this code only ever completes a
// ResponseWriter — it never touches a socket directly (under the
// coroutine dispatcher that would race with other streams' frames on
// the shared connection writer; the reactor dispatcher has no
// per-handler socket access at all).

#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/utils/number_utils.hpp>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

using spaznet::http2::Request;
using spaznet::http2::Response;
using spaznet::http2::ResponseWriter;

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

// Defers completion to a detached background thread that sleeps, then
// calls writer.complete() — proving neither dispatcher's frame loop (nor,
// under the coroutine dispatcher, any other stream's handler) is blocked
// while this one "sleeps". See handler.hpp's ResponseWriter comment and
// example/http/tests/integration/test_deferred_handler.cpp, which
// exercises the exact same pattern.
void handle_slow(const Request& req, const ParsedPath& parsed, ResponseWriter writer) {
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

    const std::uint32_t stream_id = req.stream_id;
    const std::string start = now_iso_ms();
    std::thread([stream_id, delay_ms, start, writer]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

        std::ostringstream out;
        out << "stream " << stream_id << ": slept " << delay_ms << " ms\n";
        out << "  start: " << start << "\n";
        out << "  end:   " << now_iso_ms() << "\n";

        Response resp;
        set_text(resp, out.str());
        writer.complete(std::move(resp));
    }).detach();
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
    void handle_request(const Request& req, ResponseWriter writer) override {
        ParsedPath parsed = parse_path(req.path);

        if (req.method == "GET" && parsed.path == "/slow") {
            // Deferred — handle_slow completes `writer` itself, later,
            // from a background thread.
            handle_slow(req, parsed, writer);
            return;
        }

        Response resp;
        resp.stream_id = req.stream_id;
        resp.status_code = 200;

        if (req.method == "GET" && parsed.path == "/") {
            set_text(resp, std::string(kIndexPage));
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
        writer.complete(std::move(resp));
    }
};

int main(int argc, char** argv) {
#ifdef SPAZNET_HAS_COROUTINES
    bool use_reactor = false;
#else
    bool use_reactor = true; // Coroutine dispatcher isn't built in this configuration.
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--reactor") {
            use_reactor = true;
        }
    }

    spaznet::Server server(4);
    if (use_reactor) {
        server.set_reactor_connection_factory(
            spaznet::http2::make_reactor_dispatcher(std::make_unique<Showcase>()));
    }
#ifdef SPAZNET_HAS_COROUTINES
    else {
        server.set_coroutine_connection_handler(
            spaznet::http2::make_coroutine_dispatcher(std::make_unique<Showcase>()));
    }
#endif
    server.listen_tcp(8080);
    server.run();
}
