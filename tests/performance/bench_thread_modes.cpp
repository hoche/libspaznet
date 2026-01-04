#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <libspaznet/handlers/http_handler.hpp>
#include <libspaznet/server.hpp>

using namespace spaznet;

namespace {

using Clock = std::chrono::steady_clock;

static auto now() -> Clock::time_point {
    return Clock::now();
}

static auto ms_since(Clock::time_point start) -> double {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static auto nproc() -> std::size_t {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) {
        return static_cast<std::size_t>(n);
    }
#endif
    auto hc = std::thread::hardware_concurrency();
    return hc == 0 ? 1 : static_cast<std::size_t>(hc);
}

static auto build_thread_counts() -> std::vector<std::size_t> {
    const std::size_t max_threads = std::max<std::size_t>(2, nproc() * 16);
    // Default stepping is exponential (powers of two) plus the max cap.
    std::vector<std::size_t> out;
    out.push_back(0); // default / single-threaded (non-threaded IOContext mode)
    for (std::size_t t = 2; t <= max_threads; t *= 2) {
        out.push_back(t);
        if (t > max_threads / 2) {
            break;
        }
    }
    if (out.back() != max_threads) {
        out.push_back(max_threads);
    }
    // Dedup (in case max_threads already hit)
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static auto build_thread_counts(std::size_t max_threads, std::optional<std::size_t> linear_step)
    -> std::vector<std::size_t> {
    max_threads = std::max<std::size_t>(2, max_threads);
    std::vector<std::size_t> out;
    out.push_back(0);
    if (linear_step && *linear_step > 0) {
        for (std::size_t t = 2; t <= max_threads; t += *linear_step) {
            out.push_back(t);
        }
        if (out.back() != max_threads) {
            out.push_back(max_threads);
        }
    } else {
        for (std::size_t t = 2; t <= max_threads; t *= 2) {
            out.push_back(t);
            if (t > max_threads / 2) {
                break;
            }
        }
        if (out.back() != max_threads) {
            out.push_back(max_threads);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

static auto make_body(std::size_t n) -> std::vector<uint8_t> {
    std::vector<uint8_t> b(n);
    for (std::size_t i = 0; i < n; ++i) {
        b[i] = static_cast<uint8_t>('A' + (i % 26));
    }
    return b;
}

static auto parse_uint(const std::string& s) -> std::optional<std::size_t> {
    if (s.empty()) {
        return std::nullopt;
    }
    std::size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        std::size_t d = static_cast<std::size_t>(c - '0');
        if (v > (std::numeric_limits<std::size_t>::max() - d) / 10) {
            return std::nullopt;
        }
        v = v * 10 + d;
    }
    return v;
}

static auto extract_header_value(const std::string& headers,
                                 const std::string& name_lower) -> std::optional<std::string> {
    // Very small helper: find "name:" case-insensitively by lowercasing the headers copy.
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string needle = name_lower + ":";
    auto pos = lower.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto line_end = lower.find("\r\n", pos);
    if (line_end == std::string::npos) {
        return std::nullopt;
    }
    std::string line = headers.substr(pos, line_end - pos);
    auto colon = line.find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    std::string value = line.substr(colon + 1);
    // trim leading spaces
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    // trim trailing spaces
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

static auto recv_all(int fd, void* buf, std::size_t n) -> bool {
    auto* p = static_cast<uint8_t*>(buf);
    std::size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(r);
    }
    return true;
}

static auto send_all(int fd, const void* buf, std::size_t n) -> bool {
    auto* p = static_cast<const uint8_t*>(buf);
    std::size_t off = 0;
    while (off < n) {
        ssize_t r = send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (r <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(r);
    }
    return true;
}

static auto connect_ipv4_loopback(uint16_t port) -> int {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return -1;
    }
    return sock;
}

static auto read_http_response(int sock) -> std::optional<std::size_t> {
    // Read headers first
    std::string headers;
    headers.reserve(1024);
    char buf[4096];
    for (;;) {
        int r = recv(sock, buf, sizeof(buf), 0);
        if (r <= 0) {
            return std::nullopt;
        }
        headers.append(buf, buf + r);
        auto header_end = headers.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            // We may have some body bytes already in the buffer.
            std::size_t body_start = header_end + 4;
            auto cl_str = extract_header_value(headers.substr(0, header_end + 2), "content-length");
            std::size_t content_len = 0;
            if (cl_str) {
                auto cl = parse_uint(*cl_str);
                if (!cl) {
                    return std::nullopt;
                }
                content_len = *cl;
            }
            std::size_t have = headers.size() - body_start;
            if (have >= content_len) {
                return content_len;
            }
            std::size_t need = content_len - have;
            if (need == 0) {
                return content_len;
            }
            std::vector<uint8_t> body(need);
            if (!recv_all(sock, body.data(), need)) {
                return std::nullopt;
            }
            return content_len;
        }
        if (headers.size() > 64 * 1024) {
            return std::nullopt;
        }
    }
}

struct HttpCase {
    std::size_t req_body;
    std::size_t resp_body;
};

struct HttpResult {
    std::size_t threads = 0;
    HttpCase c{};
    std::size_t requests = 0;
    std::size_t errors = 0;
    double duration_s = 0.0;
    double rps = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double resp_mib_s = 0.0;
};

static auto percentile_ms(std::vector<double>& samples, double p) -> double {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    double idx = (p / 100.0) * (samples.size() - 1);
    std::size_t lo = static_cast<std::size_t>(std::floor(idx));
    std::size_t hi = static_cast<std::size_t>(std::ceil(idx));
    if (hi >= samples.size()) {
        hi = samples.size() - 1;
    }
    double frac = idx - static_cast<double>(lo);
    return samples[lo] * (1.0 - frac) + samples[hi] * frac;
}

class BenchHandler : public HTTPHandler {
  public:
    Task handle_request(const HTTPRequest& request, HTTPResponse& response, Socket&) override {
        std::size_t resp_size = 0;
        if (auto hdr = request.get_header("X-Resp-Size")) {
            if (auto v = parse_uint(*hdr)) {
                resp_size = *v;
            }
        }
        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "application/octet-stream");
        response.body = make_body(resp_size);
        co_return;
    }
};

static auto listen_on_random_port(Server& server) -> uint16_t {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(20000, 65000);
    for (int i = 0; i < 400; ++i) {
        uint16_t port = static_cast<uint16_t>(dist(gen));
        try {
            server.listen_tcp(port);
            return port;
        } catch (...) {
        }
    }
    return 0;
}

static auto run_http_case(std::size_t server_threads, uint16_t port, const HttpCase& c,
                          double duration_s, std::size_t client_threads) -> HttpResult {
    HttpResult out;
    out.threads = server_threads;
    out.c = c;

    std::atomic<std::size_t> requests{0};
    std::atomic<std::size_t> errors{0};
    std::atomic<std::size_t> resp_bytes{0};
    std::mutex samples_mu;
    std::vector<double> samples_ms;
    samples_ms.reserve(50000);

    auto start = now();
    auto deadline = start + std::chrono::duration_cast<Clock::duration>(
                                std::chrono::duration<double>(duration_s));

    auto body = make_body(c.req_body);

    auto worker = [&]() {
        // Each request uses a new connection (server does not currently implement request loops).
        while (Clock::now() < deadline) {
            int sock = connect_ipv4_loopback(port);
            if (sock < 0) {
                errors.fetch_add(1);
                continue;
            }

            std::ostringstream req;
            req << "POST /bench HTTP/1.1\r\n";
            req << "Host: localhost\r\n";
            req << "Connection: close\r\n";
            req << "Content-Length: " << body.size() << "\r\n";
            req << "X-Resp-Size: " << c.resp_body << "\r\n";
            req << "\r\n";
            std::string hdr = req.str();

            auto t0 = now();
            bool ok = send_all(sock, hdr.data(), hdr.size());
            if (ok && !body.empty()) {
                ok = send_all(sock, body.data(), body.size());
            }
            if (!ok) {
                errors.fetch_add(1);
                ::close(sock);
                continue;
            }

            auto resp_len = read_http_response(sock);
            ::close(sock);

            auto dt = std::chrono::duration<double, std::milli>(now() - t0).count();
            if (!resp_len) {
                errors.fetch_add(1);
                continue;
            }

            requests.fetch_add(1);
            resp_bytes.fetch_add(*resp_len);
            {
                std::lock_guard<std::mutex> lock(samples_mu);
                samples_ms.push_back(dt);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(client_threads);
    for (std::size_t i = 0; i < client_threads; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    out.duration_s = std::chrono::duration<double>(Clock::now() - start).count();
    out.requests = requests.load();
    out.errors = errors.load();
    if (out.duration_s > 0.0) {
        out.rps = static_cast<double>(out.requests) / out.duration_s;
        out.resp_mib_s =
            (static_cast<double>(resp_bytes.load()) / (1024.0 * 1024.0)) / out.duration_s;
    }
    {
        std::lock_guard<std::mutex> lock(samples_mu);
        out.p50_ms = percentile_ms(samples_ms, 50.0);
        out.p95_ms = percentile_ms(samples_ms, 95.0);
        out.p99_ms = percentile_ms(samples_ms, 99.0);
    }
    return out;
}

struct IperfResult {
    std::size_t streams = 0;
    std::size_t payload = 0;
    std::string proto;     // "tcp" or "udp"
    std::string direction; // "up" (client->server) or "down" (server->client)
    double mbps = 0.0;
    double jitter_ms = 0.0; // udp only
    double loss_pct = 0.0;  // udp only
    std::string raw;
};

static auto find_iperf() -> std::optional<std::string> {
    const char* candidates[] = {"iperf3", "iperf"};
    for (auto* c : candidates) {
        std::string cmd = std::string(c) + " --version > /dev/null 2>&1";
        if (system(cmd.c_str()) == 0) {
            return std::string(c);
        }
    }
    return std::nullopt;
}

static auto is_iperf3(const std::string& exe) -> bool {
    return exe.find("iperf3") != std::string::npos;
}

static auto run_cmd_capture(const std::string& cmd) -> std::string {
    std::string full = cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        return "";
    }
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
    }
    pclose(pipe);
    return out;
}

static auto extract_json_number(const std::string& json,
                                const std::string& key) -> std::optional<double> {
    auto pos = json.find(key);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos += key.size();
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == ':')) {
        ++pos;
    }
    std::size_t end = pos;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '.' ||
            json[end] == 'e' || json[end] == 'E' || json[end] == '-' || json[end] == '+')) {
        ++end;
    }
    if (end == pos) {
        return std::nullopt;
    }
    try {
        return std::stod(json.substr(pos, end - pos));
    } catch (...) {
        return std::nullopt;
    }
}

static auto parse_iperf3_json_tcp(const std::string& json) -> std::optional<double> {
    // Prefer end.sum_received.bits_per_second if present
    auto end_pos = json.find("\"end\"");
    if (end_pos == std::string::npos) {
        return std::nullopt;
    }
    auto sum_pos = json.find("\"sum_received\"", end_pos);
    if (sum_pos == std::string::npos) {
        sum_pos = json.find("\"sum\"", end_pos);
    }
    if (sum_pos == std::string::npos) {
        return std::nullopt;
    }
    auto bps = extract_json_number(json.substr(sum_pos), "\"bits_per_second\"");
    return bps;
}

static auto parse_iperf3_json_udp(const std::string& json)
    -> std::optional<std::tuple<double, double, double>> {
    auto end_pos = json.find("\"end\"");
    if (end_pos == std::string::npos) {
        return std::nullopt;
    }
    auto sum_pos = json.find("\"sum\"", end_pos);
    if (sum_pos == std::string::npos) {
        return std::nullopt;
    }
    auto bps = extract_json_number(json.substr(sum_pos), "\"bits_per_second\"");
    auto jitter = extract_json_number(json.substr(sum_pos), "\"jitter_ms\"");
    auto lost = extract_json_number(json.substr(sum_pos), "\"lost_percent\"");
    if (!bps) {
        return std::nullopt;
    }
    return std::make_tuple(*bps, jitter.value_or(0.0), lost.value_or(0.0));
}

static auto start_iperf_server(const std::string& exe, int port, bool udp) -> pid_t {
    pid_t pid = fork();
    if (pid == 0) {
        // Child: exec server, silence output.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::ostringstream cmd;
        if (is_iperf3(exe)) {
            cmd << exe << " -s -p " << port;
        } else {
            cmd << exe << " -s -p " << port;
            if (udp) {
                cmd << " -u";
            }
        }
        execlp("/bin/sh", "sh", "-c", cmd.str().c_str(), nullptr);
        _exit(1);
    }
    if (pid > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return pid;
    }
    return -1;
}

static void stop_iperf_server(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    // best-effort wait + kill
    for (int i = 0; i < 10; ++i) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

static auto run_iperf_case(const std::string& exe, const std::string& host, int port, int seconds,
                           bool udp, bool reverse, std::size_t streams,
                           std::size_t payload) -> IperfResult {
    IperfResult r;
    r.proto = udp ? "udp" : "tcp";
    r.streams = streams;
    r.payload = payload;
    r.direction = reverse ? "down" : "up";

    std::ostringstream cmd;
    if (is_iperf3(exe)) {
        cmd << exe << " -c " << host << " -p " << port << " -t " << seconds;
        cmd << " -P " << streams;
        cmd << " -l " << payload;
        if (reverse) {
            cmd << " -R";
        }
        if (udp) {
            cmd << " -u -b 0"; // "as fast as possible" on loopback; we'll read achieved rate
        }
        cmd << " -J";
        std::string out = run_cmd_capture(cmd.str());
        r.raw = out;
        if (udp) {
            auto tup = parse_iperf3_json_udp(out);
            if (tup) {
                auto [bps, jitter, loss] = *tup;
                r.mbps = bps / 1e6;
                r.jitter_ms = jitter;
                r.loss_pct = loss;
            }
        } else {
            auto bps = parse_iperf3_json_tcp(out);
            if (bps) {
                r.mbps = *bps / 1e6;
            }
        }
        return r;
    }

    // iperf2 (text output)
    cmd << exe << " -c " << host << " -p " << port << " -t " << seconds;
    cmd << " -P " << streams;
    cmd << " -l " << payload;
    if (reverse) {
        // iperf2 uses -R? It does not; use -r (bidirectional sequential) as a best-effort.
        // We'll leave reverse unsupported for iperf2 and report the "up" direction only.
        r.raw = "reverse mode not supported for iperf2";
        return r;
    }
    if (udp) {
        cmd << " -u -b 1000M";
    }
    std::string out = run_cmd_capture(cmd.str());
    r.raw = out;

    // Parse last reported bandwidth line. iperf2 can print Kbits/sec, Mbits/sec, or Gbits/sec.
    {
        std::istringstream iss(out);
        std::string line;
        double last_mbps = 0.0;
        while (std::getline(iss, line)) {
            if (line.find("bits/sec") == std::string::npos) {
                continue;
            }
            std::istringstream ls(line);
            std::vector<std::string> toks;
            std::string t;
            while (ls >> t) {
                toks.push_back(t);
            }
            for (std::size_t i = 1; i < toks.size(); ++i) {
                if (toks[i] == "Kbits/sec" || toks[i] == "Mbits/sec" || toks[i] == "Gbits/sec") {
                    // previous token should be the numeric value
                    try {
                        double v = std::stod(toks[i - 1]);
                        if (toks[i] == "Kbits/sec") {
                            v /= 1000.0;
                        } else if (toks[i] == "Gbits/sec") {
                            v *= 1000.0;
                        }
                        last_mbps = v;
                    } catch (...) {
                    }
                }
            }
        }
        r.mbps = last_mbps;
    }
    // UDP iperf2 jitter/loss parsing is intentionally omitted; report will show throughput only.
    return r;
}

static void print_markdown_report(const std::vector<HttpResult>& http,
                                  const std::vector<IperfResult>& iperf_tcp,
                                  const std::vector<IperfResult>& iperf_udp,
                                  const std::vector<std::size_t>& thread_counts,
                                  double http_duration_s, int iperf_seconds,
                                  std::size_t client_threads) {
    std::cout << "\n";
    std::cout << "## Thread-mode performance report\n\n";
    std::cout << "- **Host nproc**: " << nproc() << "\n";
    std::cout << "- **Thread counts**: ";
    for (std::size_t i = 0; i < thread_counts.size(); ++i) {
        if (i) {
            std::cout << ", ";
        }
        std::cout << thread_counts[i];
    }
    std::cout << "\n";
    std::cout << "- **HTTP duration per case**: " << http_duration_s << "s\n";
    std::cout << "- **HTTP client threads**: " << client_threads << "\n";
    std::cout << "- **iperf duration per case**: " << iperf_seconds << "s\n\n";

    std::cout << "### HTTP (libspaznet) — throughput + latency\n\n";
    // Group results by (req,resp) for easier per-thread comparisons.
    std::map<std::pair<std::size_t, std::size_t>, std::vector<HttpResult>> by_case;
    for (const auto& r : http) {
        by_case[{r.c.req_body, r.c.resp_body}].push_back(r);
    }
    for (auto& [k, rows] : by_case) {
        std::sort(rows.begin(), rows.end(),
                  [](const HttpResult& a, const HttpResult& b) { return a.threads < b.threads; });
        std::cout << "**Case**: req_body=" << k.first << "B, resp_body=" << k.second << "B\n\n";
        std::cout << "| threads | rps | resp MiB/s | p50 ms | p95 ms | p99 ms | errors |\n";
        std::cout << "|---:|---:|---:|---:|---:|---:|---:|\n";
        for (const auto& r : rows) {
            std::cout << "| " << r.threads << " | " << std::fixed << std::setprecision(1) << r.rps
                      << " | " << std::setprecision(2) << r.resp_mib_s << " | "
                      << std::setprecision(2) << r.p50_ms << " | " << std::setprecision(2)
                      << r.p95_ms << " | " << std::setprecision(2) << r.p99_ms << " | " << r.errors
                      << " |\n";
        }
        std::cout << "\n";
    }

    if (!iperf_tcp.empty()) {
        std::cout << "### Raw TCP (iperf) — throughput\n\n";
        std::map<std::pair<std::size_t, std::string>, std::vector<IperfResult>> by_payload_dir;
        for (const auto& r : iperf_tcp) {
            by_payload_dir[{r.payload, r.direction}].push_back(r);
        }
        for (auto& [key, rows] : by_payload_dir) {
            std::sort(rows.begin(), rows.end(), [](const IperfResult& a, const IperfResult& b) {
                return a.streams < b.streams;
            });
            std::cout << "**payload**: " << key.first << "B" << ", **direction**: " << key.second
                      << "\n\n";
            std::cout << "| streams | mbps |\n";
            std::cout << "|---:|---:|\n";
            for (const auto& r : rows) {
                std::cout << "| " << r.streams << " | " << std::fixed << std::setprecision(2)
                          << r.mbps << " |\n";
            }
            std::cout << "\n";
        }
        std::cout << "\n";
    } else {
        std::cout << "### Raw TCP (iperf)\n\n";
        std::cout << "_Skipped (iperf/iperf3 not found)_\n\n";
    }

    if (!iperf_udp.empty()) {
        std::cout << "### UDP (iperf) — throughput + loss/jitter (if available)\n\n";
        std::map<std::pair<std::size_t, std::string>, std::vector<IperfResult>> by_payload_dir;
        for (const auto& r : iperf_udp) {
            by_payload_dir[{r.payload, r.direction}].push_back(r);
        }
        for (auto& [key, rows] : by_payload_dir) {
            std::sort(rows.begin(), rows.end(), [](const IperfResult& a, const IperfResult& b) {
                return a.streams < b.streams;
            });
            std::cout << "**payload**: " << key.first << "B" << ", **direction**: " << key.second
                      << "\n\n";
            std::cout << "| streams | mbps | jitter ms | loss % |\n";
            std::cout << "|---:|---:|---:|---:|\n";
            for (const auto& r : rows) {
                std::cout << "| " << r.streams << " | " << std::fixed << std::setprecision(2)
                          << r.mbps << " | " << std::setprecision(3) << r.jitter_ms << " | "
                          << std::setprecision(3) << r.loss_pct << " |\n";
            }
            std::cout << "\n";
        }
        std::cout << "\n";
    } else {
        std::cout << "### UDP (iperf)\n\n";
        std::cout << "_Skipped (iperf/iperf3 not found)_\n\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    double http_duration_s = 1.5;
    int iperf_seconds = 2;
    std::size_t client_threads = std::min<std::size_t>(32, nproc() * 2);
    std::size_t max_threads = std::max<std::size_t>(2, nproc() * 16);
    std::optional<std::size_t> linear_step;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--http-seconds" && i + 1 < argc) {
            http_duration_s = std::stod(argv[++i]);
        } else if (a == "--iperf-seconds" && i + 1 < argc) {
            iperf_seconds = std::stoi(argv[++i]);
        } else if (a == "--client-threads" && i + 1 < argc) {
            client_threads = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (a == "--max-threads" && i + 1 < argc) {
            max_threads = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (a == "--linear-step" && i + 1 < argc) {
            linear_step = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (a == "--help") {
            std::cout << "Usage: bench_thread_modes [--http-seconds S] [--iperf-seconds S] "
                         "[--client-threads N] [--max-threads N] [--linear-step N]\n";
            return 0;
        }
    }

    std::vector<std::size_t> thread_counts = build_thread_counts(max_threads, linear_step);

    std::vector<HttpCase> http_cases = {
        {0, 0}, {0, 256}, {256, 256}, {4096, 4096}, {65536, 65536}, {65536, 262144},
    };

    std::vector<HttpResult> http_results;
    http_results.reserve(thread_counts.size() * http_cases.size());

    // HTTP benchmarks per thread count.
    for (std::size_t threads : thread_counts) {
        Server server(threads);
        server.set_http_handler(std::make_unique<BenchHandler>());
        uint16_t port = listen_on_random_port(server);
        if (port == 0) {
            std::cerr << "Failed to bind server for threads=" << threads << "\n";
            continue;
        }
        std::thread server_thread([&]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        for (const auto& c : http_cases) {
            auto r = run_http_case(threads, port, c, http_duration_s, client_threads);
            http_results.push_back(r);
        }

        server.stop();
        server_thread.join();
    }

    // iperf benchmarks (raw TCP + UDP) with stream counts aligned to thread counts (excluding 0).
    std::vector<IperfResult> iperf_tcp;
    std::vector<IperfResult> iperf_udp;

    auto iperf_exe = find_iperf();
    if (iperf_exe) {
        const int iperf_port = 5201;
        const std::vector<std::size_t> payloads = {64, 1024, 8192, 65536};

        // TCP
        {
            pid_t pid = start_iperf_server(*iperf_exe, iperf_port, /*udp=*/false);
            if (pid > 0) {
                for (std::size_t streams : thread_counts) {
                    if (streams == 0) {
                        continue;
                    }
                    for (std::size_t p : payloads) {
                        iperf_tcp.push_back(
                            run_iperf_case(*iperf_exe, "127.0.0.1", iperf_port, iperf_seconds,
                                           /*udp=*/false, /*reverse=*/false, streams, p));
                        // If iperf3 is present, also measure reverse direction (server -> client).
                        if (is_iperf3(*iperf_exe)) {
                            iperf_tcp.push_back(
                                run_iperf_case(*iperf_exe, "127.0.0.1", iperf_port, iperf_seconds,
                                               /*udp=*/false, /*reverse=*/true, streams, p));
                        }
                    }
                }
                stop_iperf_server(pid);
            }
        }

        // UDP
        {
            pid_t pid = start_iperf_server(*iperf_exe, iperf_port, /*udp=*/true);
            if (pid > 0) {
                for (std::size_t streams : thread_counts) {
                    if (streams == 0) {
                        continue;
                    }
                    for (std::size_t p : payloads) {
                        iperf_udp.push_back(
                            run_iperf_case(*iperf_exe, "127.0.0.1", iperf_port, iperf_seconds,
                                           /*udp=*/true, /*reverse=*/false, streams, p));
                        if (is_iperf3(*iperf_exe)) {
                            iperf_udp.push_back(
                                run_iperf_case(*iperf_exe, "127.0.0.1", iperf_port, iperf_seconds,
                                               /*udp=*/true, /*reverse=*/true, streams, p));
                        }
                    }
                }
                stop_iperf_server(pid);
            }
        }
    }

    print_markdown_report(http_results, iperf_tcp, iperf_udp, thread_counts, http_duration_s,
                          iperf_seconds, client_threads);
    return 0;
}
