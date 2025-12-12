# libspaznet

A high-performance, cross-platform network server library written in C++20 using coroutines.

## Features

- **Cross-platform I/O multiplexing:**
  - kqueue on BSD/macOS
  - epoll on Linux
  - poll on other Unix systems
  - IOCP on Windows

- **Coroutine-based async I/O:** Uses C++20 coroutines as the primary execution model
- **Thread-safe:** Multi-threaded with lock-free task queues (minimal mutex usage)
- **Protocol support:**
  - UDP
  - HTTP/1.1
  - HTTP/2
  - WebSockets

## Building

```bash
mkdir build
cd build
cmake ..
make
```

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
- iperf3 integration for bandwidth testing

### Performance Benchmarking

For detailed bandwidth testing using iperf3:

```bash
# Make script executable
chmod +x tests/performance/run_iperf_benchmark.sh

# Run benchmark
./tests/performance/run_iperf_benchmark.sh
```

See `tests/performance/README.md` for detailed performance testing documentation.

## Example Usage

```cpp
#include <libspaznet/server.hpp>
#include <libspaznet/http_handler.hpp>

class MyHTTPHandler : public spaznet::HTTPHandler {
public:
    spaznet::Task handle_request(
        const spaznet::HTTPRequest& request,
        spaznet::HTTPResponse& response,
        spaznet::Socket& socket
    ) override {
        response.status_code = 200;
        response.set_header("Content-Type", "text/plain");
        response.body = {'H', 'e', 'l', 'l', 'o'};
        co_return;
    }
};

int main() {
    spaznet::Server server(4);  // 4 worker threads
    server.set_http_handler(std::make_unique<MyHTTPHandler>());
    server.listen_tcp(8080);
    server.run();
    return 0;
}
```

## Architecture

- **IOContext:** Manages the event loop and coroutine scheduling
- **PlatformIO:** Platform-specific I/O multiplexing abstraction
- **Server:** High-level server interface
- **Handlers:** Protocol-specific request handlers (UDP, HTTP, HTTP/2, WebSocket)

The library uses C++20 coroutines for async operations, with threads only used to run multiple coroutines in parallel. Task scheduling is lock-free using atomic operations.

## Requirements

- C++20 compiler (GCC 10+, Clang 10+, MSVC 2019+)
- CMake 3.20+

## License

This project is provided as-is for educational and development purposes.

