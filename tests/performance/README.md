# Performance Tests

This directory contains performance benchmarks and tests for libspaznet.

## Test Suites

### Throughput Tests (`test_throughput.cpp`)
- **Single Connection Throughput**: Measures requests per second with a single persistent connection
- **Multiple Connections Throughput**: Tests throughput with multiple concurrent connections
- **Sustained Load Throughput**: Long-running test to verify consistent performance
- **Large Response Throughput**: Tests bandwidth with large response payloads

### Latency Tests (`test_latency.cpp`)
- **Single Request Latency**: Measures end-to-end request latency with statistics (min, max, mean, median, P95, P99)
- **Concurrent Request Latency**: Latency under concurrent load
- **Connection Establishment Latency**: Time to establish new connections

### Concurrent Performance Tests (`test_concurrent_performance.cpp`)
- **Scaling With Threads**: Tests how performance scales with number of worker threads
- **Peak Concurrent Connections**: Maximum number of simultaneous connections
- **Burst Traffic Handling**: Performance under sudden traffic spikes

### iperf Integration (`test_iperf_integration.cpp`)
- Integration tests for using iperf/iperf3 for bandwidth testing
- TCP and UDP bandwidth measurements

## Running Performance Tests

### All Performance Tests
```bash
cd build
./test_performance
```

### Specific Test
```bash
./test_performance --gtest_filter=ThroughputTest.SingleConnectionThroughput
```

### Verbose Output
```bash
./test_performance --gtest_color=yes
```

## Using iperf3 for Bandwidth Testing

### Quick Start
```bash
# Make script executable
chmod +x tests/performance/run_iperf_benchmark.sh

# Run benchmark (default: port 5201, 10 seconds)
./tests/performance/run_iperf_benchmark.sh

# Custom port and duration
./tests/performance/run_iperf_benchmark.sh 5201 30
```

### Manual iperf3 Testing

1. **Start iperf3 server:**
   ```bash
   iperf3 -s -p 5201
   ```

2. **Run TCP bandwidth test:**
   ```bash
   iperf3 -c 127.0.0.1 -p 5201 -t 10
   ```

3. **Run UDP bandwidth test:**
   ```bash
   iperf3 -c 127.0.0.1 -p 5201 -u -b 100M -t 10
   ```

4. **Run bidirectional test:**
   ```bash
   iperf3 -c 127.0.0.1 -p 5201 --bidir -t 10
   ```

5. **Run parallel streams:**
   ```bash
   iperf3 -c 127.0.0.1 -p 5201 -P 4 -t 10
   ```

### Installing iperf3

**Ubuntu/Debian:**
```bash
sudo apt-get install iperf3
```

**macOS:**
```bash
brew install iperf3
```

**Fedora/RHEL:**
```bash
sudo dnf install iperf3
```

**Windows:**
Download from: https://iperf.fr/iperf-download.php

## Performance Metrics

The tests measure and report:

- **Throughput**: Requests per second (req/s)
- **Bandwidth**: Megabytes per second (Mbps)
- **Latency**: Microseconds (μs) with percentiles
- **Concurrent Connections**: Maximum simultaneous connections
- **Success Rate**: Percentage of successful connections/requests

## Expected Performance

Typical performance on modern hardware:

- **Throughput**: 1000+ req/s (single connection), 5000+ req/s (multiple connections)
- **Latency**: < 1ms median, < 5ms P95
- **Concurrent Connections**: 500+ simultaneous connections
- **Bandwidth**: Depends on network, but should saturate localhost (10+ Gbps)

## Continuous Performance Monitoring

For CI/CD integration, performance tests can be run with thresholds:

```bash
# Run with failure on regression
./test_performance --gtest_filter=ThroughputTest.* --gtest_break_on_failure
```

## Notes

- Performance tests may take longer to run than unit/integration tests
- Some tests require network access
- Results vary based on system load and hardware
- For accurate measurements, run tests on dedicated hardware with minimal background processes

