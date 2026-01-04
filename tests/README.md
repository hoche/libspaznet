# Test Suite Documentation

This directory contains comprehensive unit and integration tests for libspaznet.

## Test Structure

### Unit Tests (`tests/unit/`)

Unit tests focus on individual components in isolation:

- **test_task_queue.cpp**: Tests for coroutine task scheduling and lock-free queues
  - Basic task execution
  - Task suspension and resumption
  - Thread-safe enqueue/dequeue operations
  - Concurrent access patterns

- **test_platform_io.cpp**: Tests for platform-specific I/O implementations
  - File descriptor registration
  - Event waiting and timeout handling
  - Multiple file descriptor management
  - Cross-platform compatibility

- **test_io_context.cpp**: Tests for the main event loop and context
  - Task scheduling
  - Concurrent task execution
  - Event loop lifecycle

- **test_http_handler.cpp**: Tests for HTTP protocol handling
  - Request/response serialization
  - Header management
  - Body handling
  - Status codes

- **test_websocket_handler.cpp**: Tests for WebSocket protocol
  - Frame serialization/parsing
  - Masked and unmasked frames
  - Extended length frames
  - Control frames (ping/pong/close)

- **test_http2_handler.cpp**: Tests for HTTP/2 protocol
  - Frame structure
  - Request/response conversion
  - Stream management

### Integration Tests (`tests/integration/`)

Integration tests verify end-to-end functionality:

- **test_tcp_server.cpp**: TCP server functionality
  - Server startup and shutdown
  - Port listening
  - Multiple port support

- **test_http_server.cpp**: HTTP server functionality
  - GET/POST request handling
  - Response generation
  - Multiple concurrent requests
  - Header and body handling

- **test_websocket_server.cpp**: WebSocket server functionality
  - Frame handling
  - Message echo
  - Connection lifecycle

- **test_udp_server.cpp**: UDP server functionality
  - Packet reception
  - Different packet sizes
  - Binary data handling

- **test_concurrent_connections.cpp**: Concurrency and performance
  - Sequential connections
  - Concurrent connections
  - Burst connections
  - Sustained load
  - Mixed request types

## Running Tests

### All Tests
```bash
cd build
ctest
```

### Unit Tests Only
```bash
./test_unit
```

### Integration Tests Only
```bash
./test_integration
```

### Specific Test
```bash
./test_unit --gtest_filter=TaskQueueTest.BasicEnqueueDequeue
```

### Verbose Output
```bash
./test_unit --gtest_color=yes
```

## Test Coverage Goals

- **Unit Tests**: >90% code coverage for core components
- **Integration Tests**: All major use cases and protocols
- **Concurrency Tests**: Verify thread safety and performance

## Adding New Tests

When adding new functionality:

1. Add unit tests for individual components
2. Add integration tests for end-to-end scenarios
3. Update this README with test descriptions
4. Ensure tests pass on all supported platforms

## Platform-Specific Notes

- **Linux**: Uses epoll for I/O multiplexing
- **macOS/BSD**: Uses kqueue for I/O multiplexing
- **Windows**: Uses IOCP for I/O multiplexing
- **Other Unix**: Falls back to poll()

Tests should work on all platforms, but some integration tests may require network access.






