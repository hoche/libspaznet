# libspaznet

A high-performance, cross-platform network server library written in C++20 that supports two interchangeable execution models on the same event loop: C++20 coroutines and a plain callback/state-machine reactor. Every protocol dispatcher (HTTP/1.1, HTTP/2, WebSocket, UDP, QUIC + HTTP/3) ships in both flavors, and the reactor model needs no coroutine support at all — `-DSPAZNET_ENABLE_COROUTINES=OFF` builds the entire library, every demo, and the full test suite with zero `<coroutine>` in the compiled output.

## Features

- **Cross-platform I/O multiplexing:**
  - kqueue on BSD/macOS
  - epoll on Linux
  - poll on other Unix systems
  - readiness-style IOCP on Windows (force poll with `-DSPAZNET_FORCE_POLL=ON`)
- **Two execution models, your choice per connection:**
  - **Coroutines** (`make_coroutine_dispatcher(...)`) — `Task`/`co_await`, the original model. Connection state lives implicitly in the coroutine frame, which can migrate freely between worker threads.
  - **Reactor** (`make_reactor_dispatcher(...)`) — plain callbacks over an explicit state machine (`BufferedConnection` + phase enum), no `<coroutine>` dependency, no coroutine-frame allocation. Works with `-DSPAZNET_ENABLE_COROUTINES=OFF`.
  - Both hit the identical protocol codec and the identical handler object where the handler shape allows it (HTTP/1.1, HTTP/2, UDP, QUIC/HTTP3); only the execution model differs. See *Execution Models* below.
- **Thread-safe:** Single-threaded by default, or multi-threaded with a small fixed set of mutexes (see *Concurrency Primitives* below and `docs/mutex-vs-atomics.md`). Reactor TLS I/O takes no per-connection mutex (IO-thread affinity); coroutine TLS serializes only when needed.
- **Optional TCP TLS (HTTPS / WSS):** `Server::listen_tls` + `TlsConfig` when OpenSSL 1.1.1+ is found (`SPAZNET_ENABLE_TLS`, default ON). Memory-BIO `TlsStream` under `Socket` / `BufferedConnection`; per-listener ALPN (`http/1.1` or `h2`). Independent of QUIC's OpenSSL 3.5+ / wolfSSL stack. See `docs/integration.md`.
- **Protocol support:**
  - UDP
  - HTTP/1.1 (plain and HTTPS)
  - HTTP/2 (h2c and TLS `h2`)
  - QUIC & HTTP/3 (partial)
  - WebSockets (plain and WSS)



## Execution Models: Coroutines vs. Reactor

Every protocol under `example/<protocol>/` exposes two dispatcher factories that speak the exact same wire protocol:

```cpp
// Coroutine dispatcher: Task/co_await under the hood.
server.set_coroutine_connection_handler(
    spaznet::http::make_coroutine_dispatcher(std::make_unique<MyHTTPHandler>()));

// Reactor dispatcher: plain callbacks, no coroutine dependency.
server.set_reactor_connection_factory(
    spaznet::http::make_reactor_dispatcher(std::make_unique<MyHTTPHandler>()));
```

| | Coroutine (`make_coroutine_dispatcher`) | Reactor (`make_reactor_dispatcher`) |
|---|---|---|
| Connection state lives in | the coroutine frame (implicit, across `co_await`) | ordinary member variables of a phase state machine (explicit) |
| Requires C++20 `<coroutine>` | Yes | No |
| Builds with `-DSPAZNET_ENABLE_COROUTINES=OFF` | No | Yes |
| Can migrate between `Server(N)` worker threads | Yes, at every `co_await` | No — see below |
| Registered via | `Server::set_coroutine_connection_handler` | `Server::set_reactor_connection_factory` |

Pick coroutines when you want `co_await`-style linear code and don't mind the C++20 coroutine dependency. Pick the reactor model when you need a coroutine-free build (older toolchains, embedded targets, or code that just prefers explicit state machines), or when you're calling into `libspaznet` from a context where `<coroutine>` isn't available at all.

One threading caveat: reactor connection state is only safe to touch from the IO thread of the `IOContext` it is pinned to. `Server(N)`'s worker threads add parallelism for coroutine `Task`s but **not** for reactor I/O — use `Server(ServerConfig{.loops = N})` for accept-and-shard across N independent loops. See [`docs/reactor-threading.md`](docs/reactor-threading.md).

For the full picture — how coroutines schedule, why reactor state can't migrate, the CMake option and header guards, and a state-machine authoring guide for a new reactor dispatcher — see:

- [`docs/concurrency-and-coroutines.md`](docs/concurrency-and-coroutines.md) — coroutine scheduling model and the "Reactor Threading Model" section
- [`docs/coro-free-build.md`](docs/coro-free-build.md) — the coroutine-free build matrix and reactor authoring rules
- [`docs/threading.md`](docs/threading.md) — workers vs loops tuning (with SVG diagrams)
- [`docs/reactor-threading.md`](docs/reactor-threading.md) — accept-and-shard multi-loop design



## Quick Start



### Building

```bash
# Using Make (recommended)
make

# Or using CMake directly
mkdir build && cd build
cmake ..
make
```



### Running Tests

```bash
# Run all tests
make test

# Run specific test suites
make test-unit
make test-integration
make test-performance
```



### Code Quality

```bash
# Format code
make format

# Check formatting
make check-format

# Run static analysis
make check-tidy      # clang-tidy
make check-cppcheck  # cppcheck

# Run all checks
make lint
```



## Threading Modes

The two execution models want opposite shapes of parallelism, so `Server` exposes both axes via `ServerConfig`:

```cpp
struct ServerConfig {
    std::size_t loops = 1;            // independent IOContext instances
    std::size_t workers_per_loop = 0; // coroutine worker threads per loop
};
```

- `Server(0)` / `ServerConfig{1, 0}` — single loop, no workers
- `Server(N)` / `ServerConfig{1, N}` — one loop + N coroutine workers (historical meaning)
- `Server(ServerConfig{.loops = N})` — N loops, accept-and-shard for reactor TCP

**Coroutine** dispatchers: coroutines can migrate between worker threads as they await I/O. The scheduler uses a small, fixed set of `std::mutex`es (see *Concurrency Primitives* below); everything else on the hot path is `std::atomic<…>`.

**Reactor** dispatchers: each connection is pinned to one loop at accept and never moves. Extra `Server(N)` workers do not raise reactor I/O throughput — pass `ServerConfig{.loops = N}` instead. UDP stays on loop 0. See [`docs/reactor-threading.md`](docs/reactor-threading.md).

For performance characteristics across both axes, see `thread_mode_report.md` (generated by `./bench_thread_modes`).

## Testing

The project includes extensive unit, integration, and performance tests using Google Test.

### Running Tests

```bash
cd build
ctest
```

Or run tests individually:

```bash
./test_unit          # Run unit tests
./test_integration    # Run integration tests
./test_performance    # Run performance benchmarks
```



### Test Coverage

**Unit Tests:**

- Task and TaskQueue (coroutine scheduling, thread safety)
- PlatformIO implementations (epoll/kqueue/poll/IOCP)
- IOContext (event loop, task scheduling)
- HTTP handler (request/response serialization)
- WebSocket handler (frame parsing and serialization)
- HTTP/2 handler (frame structure)
- QUIC handler
- HTTP/3 handler (partial)

**Integration Tests:**

- TCP server (connection handling, multiple ports)
- HTTP server (request/response cycle, multiple requests)
- WebSocket server (frame handling, ping/pong)
- UDP server (packet handling, different sizes)
- Concurrent connections (load testing, burst connections)

**Performance Tests:**

- Throughput benchmarks (requests per second)
- Latency measurements (min, max, mean, median, P95, P99)
- Concurrent connection performance
- iperf/iperf3 integration for bandwidth testing



### Performance Benchmarking

For detailed bandwidth testing using iperf3:

```bash
# Make script executable
chmod +x tests/performance/run_iperf_benchmark.sh

# Run benchmark
./tests/performance/run_iperf_benchmark.sh
```

To generate a comprehensive performance report comparing thread modes:

```bash
cd build
./bench_thread_modes > thread_mode_report.md
```

This generates a report showing HTTP throughput and latency across different thread counts, as well as raw TCP/UDP bandwidth measurements. See `tests/performance/README.md` for detailed performance testing documentation.

## Code Quality Tools

The project includes support for various code quality tools:

### clang-format

Format code according to the project style:

```bash
make format        # Format all files
make check-format  # Check formatting without modifying files
```



### clang-tidy

Run static analysis:

```bash
make check-tidy
```



### cppcheck

Run additional static analysis:

```bash
make check-cppcheck
```



### Combined Checks

Run all code quality checks:

```bash
make lint
```



## Example Usage

```cpp
#include <libspaznet/server.hpp>
#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>

class MyHTTPHandler : public spaznet::http::HTTPHandler {
public:
    void handle_request(
        const spaznet::http::HTTPRequest& request,
        spaznet::http::ResponseWriter writer
    ) override {
        spaznet::http::HTTPResponse response;
        response.status_code = 200;
        response.set_header("Content-Type", "text/plain");
        response.body = {'H', 'e', 'l', 'l', 'o'};
        writer.complete(std::move(response));
    }
};

int main() {
    spaznet::Server server(4);  // 4 worker threads (0 = single-threaded)
    server.set_coroutine_connection_handler(
        spaznet::http::make_coroutine_dispatcher(std::make_unique<MyHTTPHandler>()));
    server.listen_tcp(8080);

    // Optional: Monitor server statistics
    auto stats = server.get_statistics();
    std::cout << "Active coroutines: " << stats.active_coroutines << std::endl;

    server.run();
    return 0;
}
```

`MyHTTPHandler` above works unchanged with the reactor dispatcher too — only how the connection replays it differs:

```cpp
int main() {
    // loops=N scales reactor I/O via accept-and-shard; workers_per_loop is unused here.
    spaznet::Server server(spaznet::ServerConfig{.loops = 4, .workers_per_loop = 0});
    server.set_reactor_connection_factory(
        spaznet::http::make_reactor_dispatcher(std::make_unique<MyHTTPHandler>()));
    server.listen_tcp(8080);
    server.run();
    return 0;
}
```

This variant has no coroutine dependency and compiles under `-DSPAZNET_ENABLE_COROUTINES=OFF`.

Core ships only the low-level server.  Each protocol — HTTP/1.1,
WebSocket, HTTP/2, UDP, QUIC + HTTP/3 — is an `example/<protocol>/`
library you link in addition to `spaznet::spaznet`.  Working demos
under `example/*/demo/` (see each directory's `README.md` for details):


| Protocol        | Binary           | Description                                                               |
| --------------- | ---------------- | ------------------------------------------------------------------------- |
| **HTTP/1.x**    | `http_hello`     | Minimal `HTTPHandler`; `--tls` for HTTPS on 8443                          |
|                 | `http_showcase`  | HTTP/1.0 vs 1.1 differences (keep-alive, chunked framing, request bodies) |
| **WebSocket**   | `ws_echo`        | Minimal echo; plain HTTP on the same port; `--tls` for WSS                |
|                 | `ws_chat`        | Multi-client broadcast chat with a browser HTML+JS page; `--tls` for WSS  |
| **HTTP/2**      | `http2_hello`    | Minimal h2c (prior-knowledge); `--tls` for ALPN `h2`                      |
|                 | `http2_showcase` | Stream multiplexing via `/slow?ms=N`, plus HPACK/DATA routes              |
| **QUIC-HTTP/3** | ---              | None yet                                                                  |
| **UDP**         | `udp_echo`       | Minimal datagram echo                                                     |
|                 | `udp_relay`      | Connectionless peer table + fan-out ("chat" over UDP)                     |
|                 | `udp_statsd`     | Fire-and-forget metrics aggregator (statsd-style line protocol)           |


```bash
./build/example/http/http_hello
./build/example/http/http_hello --tls          # HTTPS (self-signed) on 8443
./build/example/http/http_showcase
./build/example/http-websocket/ws_echo
./build/example/http-websocket/ws_echo --tls   # WSS on 8443
./build/example/http-websocket/ws_chat
./build/example/http2/http2_hello
./build/example/http2/http2_hello --tls
./build/example/http2/http2_showcase
./build/example/udp/udp_echo
./build/example/udp/udp_relay
./build/example/udp/udp_statsd
```

QUIC + HTTP/3 (`example/quic-http3/`) has no interactive demo yet; exercise
it via the unit/integration tests and `bench_quic_steady_state`.

See [docs/integration.md](docs/integration.md) for the CMake
linkage and [docs/migration.md](docs/migration.md) if you're
porting code from before the restructure.

## Architecture

- **IOContext:** Manages the event loop, the fd readiness table, timers, and — for the coroutine model — coroutine scheduling across worker threads
- **PlatformIO:** Platform-specific I/O multiplexing abstraction (epoll/kqueue/poll/IOCP), shared by both execution models
- **IoHandler:** The reactor primitive underneath both models — a plain `on_readable`/`on_writable`/`on_error` interface. Coroutine resumption is implemented as one `IoHandler` (`CoroutineResumeHandler`); a reactor connection's `BufferedConnection` is another. Neither is a privileged special case.
- **Server:** High-level server interface; `set_coroutine_connection_handler` wires up a coroutine dispatcher, `set_reactor_connection_factory` wires up a reactor one
- **Handlers:** Protocol-specific request handlers (UDP, HTTP, HTTP/2, WebSocket, QUIC, HTTP/3), each with a coroutine dispatcher and a reactor dispatcher sharing the same codec

Coroutines are the optional layer, not the foundation: the reactor core (`IoHandler`, `BufferedConnection`, `post()`/timers) builds and runs with zero coroutine dependency, and coroutine support is an adapter on top of it, gated by the `SPAZNET_ENABLE_COROUTINES` CMake option (default `ON`). See *Execution Models* above. Most state on the hot path is held in `std::atomic<…>`; structural mutations are guarded by a small set of mutexes (see *Concurrency Primitives* below). TCP TLS follows the same split: reactor connections never take `TlsStream::io_mu_`; coroutine `Socket::attach_tls` enables it only when reader and writer coroutines can share one SSL*.

### Concurrency Primitives

**Backend** means which I/O demultiplexer the lock is tied to. `all` = present regardless of demux (epoll / kqueue / poll / IOCP). epoll and kqueue keep interest sets in the kernel and add **no** mutex of their own. A given binary includes at most one of the poll / IOCP table locks.

**Runtime** means which execution model exercises the lock: `both` (present and used no matter which dispatcher a connection uses), `coroutine` (only reachable through a `Task`-returning `Handler`; compiled out entirely under `-DSPAZNET_ENABLE_COROUTINES=OFF`), or `reactor` (only reachable through an `IoHandler` / `make_reactor_dispatcher` path; present in every build, coroutines or not). See *Execution Models* above for what distinguishes the two.

#### Core library (`src/`, `include/libspaznet/`)


| Location                               | File                                              | Backend        | Runtime   | Guards                                                                                                                                                 |
| --------------------------------------- | -------------------------------------------------- | --------------- | --------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `TaskQueue::mutex_`                    | `include/libspaznet/platform/io_context.hpp`      | all            | coroutine | both ends of each worker's coroutine task queue (`thread_queues_`)                                                                                     |
| `CallbackQueue::mutex_`                | `include/libspaznet/platform/io_context.hpp`      | all            | both      | both ends of `post()`'s round-robin callback queues (`callback_queues_`) and the single IO-thread-affine queue behind `post_to_io_thread()` (`io_thread_queue_`) |
| `IOContext::timer_mutex_`              | `include/libspaznet/platform/io_context.hpp`      | all            | both      | timer min-heap + cancelled-set + (coroutine-only) suspended-tasks map                                                                                  |
| `IOContext::map_lock_`                 | `include/libspaznet/platform/io_context.hpp`      | all            | both      | `pending_io_` map and platform `add_fd` / `modify_fd` / `remove_fd` side-table updates                                                                 |
| `IOContext::reap_mutex_`               | `include/libspaznet/platform/io_context.hpp`      | all            | reactor   | the reap list backing `defer_destruction()` — lets a reactor state machine tear itself down safely from inside its own callback                       |
| `IOContext::worker_wake_mutex_`        | `include/libspaznet/platform/io_context.hpp`      | all            | both      | pairs with `worker_wake_cv_`; idle workers park here instead of busy-yielding                                                                          |
| `IOContext::worker_join_mutex_`        | `include/libspaznet/platform/io_context.hpp`      | all            | both      | serializes `join_workers()` between `run()`'s exit path and `~IOContext`                                                                               |
| `Server::listen_fds_mutex_`            | `include/libspaznet/server.hpp`                   | all            | both      | listening-socket vector                                                                                                                                |
| `Server::client_fds_mutex_`            | `include/libspaznet/server.hpp`                   | all            | coroutine | active-client-fd set used by `Server::stop()` to drain in-flight coroutine connections                                                                |
| `Server::reactor_conns_mutex_`         | `include/libspaznet/server.hpp`                   | all            | reactor   | `reactor_connections_` (fd → `{IoHandler, IOContext*}`) registry backing `set_reactor_connection_factory`; under multi-loop accept-and-shard the `IOContext*` is the loop the connection was pinned to, so `stop()` can `post_to_io_thread` shutdown on the right loop |
| `ResponseWriter<Response>::State::mu`  | `include/libspaznet/reactor/response_writer.hpp`  | —              | both      | the shared completion state (`deliver`, `on_ready`, `completed`) so `complete()` can race safely against `on_ready()` registration from any thread    |
| `PlatformIOPoll::mutex_`               | `src/platform/platform_io_poll.cpp`               | poll / WSAPoll | both      | interest-set tables (`pollfds_` / `fd_info_`); compiled when `USE_POLL` is selected (non-Windows default fallback, or `-DSPAZNET_FORCE_POLL=ON` on Windows) |
| `PlatformIOIOCP::mutex_`               | `src/platform/platform_io_iocp.cpp`               | IOCP (Winsock) | both      | IOCP fd/probe tables; compiled when `USE_IOCP` is selected (default on Windows)                                                                        |
| `detail::TlsStream::io_mu_` (optional) | `include/libspaznet/detail/tls_stream.hpp`        | all            | coroutine | taken only after `Socket::attach_tls` → `enable_serialized_io()`; reactor `BufferedConnection` leaves it off (IO-thread affinity). Protects SSL* + memory-BIO pump when HTTP/2/WS coroutines share a connection |
| `detail::ensure_winsock()` `once_flag` | `include/libspaznet/detail/socket_compat.hpp`     | Winsock        | both      | process-wide `WSAStartup` / `WSACleanup`; fires at most once (not a `std::mutex`)                                                                      |


Everything else in core — coroutine ref-counts, the per-fd generation counter that defeats fd-reuse, statistics, timer ids, `running_` / `active_connections_` flags, etc. — lives in `std::atomic<…>` and never reaches for a mutex. Reactor connection state itself (`BufferedConnection` and everything built on it, including TLS under memory BIOs) needs no lock at all: exactly one thread per `IOContext` ever calls `on_readable()`/`on_writable()` — see [`docs/reactor-threading.md`](docs/reactor-threading.md) and `docs/concurrency-and-coroutines.md`'s "Reactor Threading Model". TLS accept→factory handoff is `thread_local` (no global stash map).

See `docs/mutex-vs-atomics.md` for why the core locks are mutexes rather than atomics.

#### Protocol example libraries (`example/*/src`)

These ship with the optional protocol stacks under `example/`; they are not part of the core `spaznet` target and are not demos. Backend is `—` because they sit above the demux.


| Location        | File                                        | Backend | Runtime   | Guards                                                                                                                                                                                                             |
| --------------- | ------------------------------------------- | ------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `WriteGate::m`  | `example/http-websocket/src/dispatcher_coroutine.cpp` | —       | coroutine | fair async write gate so dispatcher control frames and application `send()` cannot interleave on one connection — the reactor dispatcher's `OutputBuffer` serializes writes by construction, so it needs no gate |
| `ConnState::mu` | `example/http2/src/dispatcher_coroutine.cpp`          | —       | coroutine | per-connection HTTP/2 state and the serialized outbound frame queue — the reactor dispatcher's `Http2Connection` needs no lock, since every mutating entry point is provably reached from the IO thread alone via `post_to_io_thread()` (`assert(ctx_.is_io_thread())` instead) |


Neither of these has a reactor-side counterpart lock: the reactor dispatchers for WebSocket and HTTP/2 achieve the same single-writer/single-mutator guarantee through `OutputBuffer` and IO-thread affinity instead of a mutex.


#### Demos only (`example/*/demo`)

Demo binaries may take locks for application-level shared state. None of these are linked into the core library or the protocol example libraries.


| Demo         | Location                   | File                                     | Backend | Runtime   | Guards                                                                                                                                                        |
| ------------ | -------------------------- | ----------------------------------------- | ------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `ws_chat`    | `ChatRoom::mu_`            | `example/http-websocket/demo/chat.cpp`   | —       | coroutine | room session map (`sessions_`) across `on_open` / `on_close` / `broadcast`                                                                                   |
| `ws_chat`    | `Session::mu`              | `example/http-websocket/demo/chat.cpp`   | —       | coroutine | that session's outbound message outbox                                                                                                                       |
| `ws_chat`    | `ChatRoomReactor::mu_`     | `example/http-websocket/demo/chat.cpp`   | —       | reactor   | same room session map, coroutine-free counterpart of `ChatRoom::mu_` — no per-session outbox needed since `Connection::send()` is a direct synchronous write |
| `udp_relay`  | `Relay::peers_mutex_`      | `example/udp/demo/relay.cpp`             | —       | both      | peer table shared across datagram worker threads; the same `Relay` instance backs both `--reactor` and the default coroutine dispatcher                     |
| `udp_statsd` | `StatsdAggregator::mutex_` | `example/udp/demo/statsd.cpp`            | —       | both      | counter/gauge maps between the handler and the reporter thread; the same `StatsdAggregator` instance backs both dispatchers                                 |


Other demos (`http_hello`, `http_showcase`, `http2_*`, `ws_echo`, `udp_echo`, etc.) do not introduce their own mutexes.

### Monitoring and Statistics

The `Server` class provides a `get_statistics()` method that returns lock-free statistics:

```cpp
auto stats = server.get_statistics();
// stats.active_requests - Currently active HTTP requests
// stats.total_coroutines_created - Total coroutines created
// stats.active_coroutines - Currently active coroutines
// stats.total_memory_bytes - Estimated memory in use (bytes)
```

Statistics are updated atomically and can be read from any thread without blocking.

For detailed information about the coroutine execution model, thread scheduling, and I/O handling, see `docs/concurrency-and-coroutines.md`.

## Documentation


| Topic                                         | File                                                                       |
| --------------------------------------------- | -------------------------------------------------------------------------- |
| **What's stable, what's not**                 | `[docs/api-status.md](docs/api-status.md)`                                 |
| HTTP/1.1 handler API                          | `[docs/http.md](docs/http.md)`                                             |
| WebSocket handler API                         | `[docs/websocket.md](docs/websocket.md)`                                   |
| QUIC + HTTP/3 walkthrough                     | `[docs/quic-http3.md](docs/quic-http3.md)`                                 |
| QUIC security model                           | `[docs/quic-security.md](docs/quic-security.md)`                           |
| Threading & tuning                            | `[docs/threading.md](docs/threading.md)`                                   |
| Reactor multi-loop (accept-and-shard)         | `[docs/reactor-threading.md](docs/reactor-threading.md)`                   |
| Coroutine model                               | `[docs/concurrency-and-coroutines.md](docs/concurrency-and-coroutines.md)` |
| Building without coroutines (reactor runtime) | `[docs/coro-free-build.md](docs/coro-free-build.md)`                       |
| Coroutine pitfalls (don't do these)           | `[docs/coroutine-pitfalls.md](docs/coroutine-pitfalls.md)`                 |
| Mutex vs. atomic posture                      | `[docs/mutex-vs-atomics.md](docs/mutex-vs-atomics.md)`                     |
| Performance numbers                           | `[docs/performance.md](docs/performance.md)`                               |
| Integrating libspaznet into your project      | `[docs/integration.md](docs/integration.md)`                               |
| TCP TLS / HTTPS / WSS build notes             | `[docs/integration.md](docs/integration.md)` (TLS sections)                |
| Migration / breaking changes                  | `[docs/migration.md](docs/migration.md)`                                   |
| API reference (Doxygen)                       | `[docs/doxygen.md](docs/doxygen.md)`                                       |
| Contributing                                  | `[CONTRIBUTING.md](CONTRIBUTING.md)`                                       |
| Changelog                                     | `[CHANGELOG.md](CHANGELOG.md)`                                             |




## Requirements

- C++20 compiler with `<format>` support: **GCC 13.1+** or **Clang 17+**
  (with libstdc++ from gcc 13+, or libc++).
- CMake 3.20+
- Make (optional, for convenience targets)
- The `SPAZNET_ENABLE_COROUTINES` CMake option (default `ON`) gates the
  coroutine execution model and its `<coroutine>` dependency; the reactor
  model has no such dependency. Set `-DSPAZNET_ENABLE_COROUTINES=OFF` to
  build the core library, every protocol, every demo, and the full test
  suite with zero coroutine code compiled in — see
  [docs/coro-free-build.md](docs/coro-free-build.md).
- **OpenSSL 1.1.1+** — optional for TCP TLS (`listen_tls` / HTTPS / WSS).
  `SPAZNET_ENABLE_TLS` defaults `ON` when found; otherwise CMake warns and
  builds without `SPAZNET_HAS_TLS`. Independent of QUIC.
- **OpenSSL 3.5+** (or wolfSSL with QUIC) — only required when building
  the QUIC v1 + HTTP/3 stack. `SPAZNET_BUILD_QUIC` (default `ON`) gates
  this: if no backend is available, CMake warns and disables QUIC; UDP /
  HTTP/1.1 / HTTP/2 / WebSocket still build. Explicit off:
  ```
  cmake -B build -DSPAZNET_BUILD_QUIC=OFF
  ```

If your distribution ships an older gcc, `apt install g++-13` and
configure with `-DCMAKE_CXX_COMPILER=g++-13`. CMake checks for `<format>`
at configure time and fails fast with an actionable message rather than
deep in compilation.

### Optional Tools

- clang-format (for code formatting)
- clang-tidy (for static analysis)
- cppcheck (for additional static analysis)
- iperf2 or iperf3 (for performance benchmarking)



## Development Workflow

```bash
# Set up development environment
make dev-setup

# Make changes, then:
make format      # Format code
make lint        # Run all checks
make test        # Run tests
make build       # Build
```

