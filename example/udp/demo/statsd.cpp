// Fire-and-forget UDP metrics aggregator (a tiny statsd).
//
//   $ ./udp_statsd
//   $ printf 'requests:1|c\ntemp:21.5|g' | nc -u -w0 127.0.0.1 8081
//   $ echo -n 'requests:1|c' | nc -u -w0 127.0.0.1 8081
//   # every ~2s, a report like this prints to stdout:
//   [statsd] counters: requests=2
//            gauges:   temp=21.5
//
// This demo shows the other classic UDP usage pattern, distinct from
// the request/reply style of udp_echo and the peer-to-peer relay of
// udp_relay: one-way, fire-and-forget telemetry. Callers never expect
// (and this handler never sends) a reply — that's the entire appeal of
// UDP for metrics: a slow or unreachable collector can never make a
// caller block. Each datagram is one or more complete metric lines
// (UDP preserves message boundaries, unlike a TCP byte stream, so
// there's no framing to do here), parsed and folded into shared
// counters/gauges under a mutex, then discarded.
//
// Line protocol: "name:value|type", one metric per line, optionally
// several newline-separated metrics per datagram.
//   type 'c' — counter; value is added to a running total, reset to 0
//              after each report (classic statsd flush semantics).
//   type 'g' — gauge; value replaces the last-reported value.
// Malformed lines are dropped silently.

#include <libspaznet/server.hpp>
#include <libspaznet/udp/dispatcher.hpp>
#include <libspaznet/udp/handler.hpp>

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

// Renders a double without a pile of trailing zeros for whole numbers,
// so counter reports read "requests=2" rather than "requests=2.000000".
std::string format_value(double value) {
    std::ostringstream out;
    if (value == static_cast<double>(static_cast<long long>(value))) {
        out << static_cast<long long>(value);
    } else {
        out << value;
    }
    return out.str();
}

} // namespace

class StatsdAggregator : public spaznet::udp::Handler {
  public:
    spaznet::Task handle_packet(const spaznet::udp::Packet& pkt) override {
        const std::string text(pkt.data.begin(), pkt.data.end());
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            apply_line(line);
        }
        // No sendto() here at all — this handler only ever consumes
        // datagrams. That's the whole point of the demo.
        co_return;
    }

    // Prints the current snapshot and resets counters (statsd-style
    // flush). Called from the reporter thread in main(), so the same
    // mutex that guards handle_packet's writes protects this read.
    void report() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "[statsd] counters: ";
        for (const auto& [name, value] : counters_) {
            std::cout << name << "=" << format_value(value) << " ";
        }
        std::cout << "\n         gauges:   ";
        for (const auto& [name, value] : gauges_) {
            std::cout << name << "=" << format_value(value) << " ";
        }
        std::cout << std::endl;
        counters_.clear();
    }

  private:
    void apply_line(const std::string& line) {
        const auto colon = line.find(':');
        const auto pipe = line.find('|', colon == std::string::npos ? 0 : colon);
        if (colon == std::string::npos || pipe == std::string::npos || pipe < colon) {
            return;
        }
        const std::string name = line.substr(0, colon);
        const std::string value_str = line.substr(colon + 1, pipe - colon - 1);
        const std::string type = line.substr(pipe + 1);
        if (name.empty() || (type != "c" && type != "g")) {
            return;
        }

        double value = 0.0;
        try {
            std::size_t consumed = 0;
            value = std::stod(value_str, &consumed);
            if (consumed != value_str.size()) {
                return; // trailing garbage after the number
            }
        } catch (const std::exception&) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (type == "c") {
            counters_[name] += value;
        } else {
            gauges_[name] = value;
        }
    }

    std::mutex mutex_;
    std::unordered_map<std::string, double> counters_;
    std::unordered_map<std::string, double> gauges_;
};

int main() {
    spaznet::Server server(2);
    auto handler = std::make_unique<StatsdAggregator>();
    StatsdAggregator* handler_raw = handler.get();
    server.set_datagram_handler(spaznet::udp::make_dispatcher(std::move(handler)));
    server.listen_udp(8081);

    // udp::Handler is a plain callback with no IOContext handed to it,
    // so a periodic flush/report is simplest as its own thread rather
    // than a coroutine timer. handler_raw stays valid for the process
    // lifetime — make_dispatcher holds the Handler in a shared_ptr that
    // outlives this thread.
    std::thread reporter([handler_raw]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            handler_raw->report();
        }
    });
    reporter.detach();

    server.run();
}
