#!/bin/bash

# iperf Benchmark Script for libspaznet
# This script helps benchmark the network server using iperf3

set -e

PORT=${1:-5201}
DURATION=${2:-10}
SERVER_HOST=${3:-127.0.0.1}

echo "=========================================="
echo "libspaznet iperf Benchmark"
echo "=========================================="
echo "Port: $PORT"
echo "Duration: ${DURATION}s"
echo "Server: $SERVER_HOST"
echo ""

# Check if iperf3 is available
if ! command -v iperf3 &> /dev/null; then
    echo "Error: iperf3 not found. Please install iperf3."
    echo "  Ubuntu/Debian: sudo apt-get install iperf3"
    echo "  macOS: brew install iperf3"
    echo "  Fedora: sudo dnf install iperf3"
    exit 1
fi

echo "Starting iperf3 server in background..."
iperf3 -s -p $PORT &
SERVER_PID=$!

# Wait for server to start
sleep 2

# Function to cleanup
cleanup() {
    echo ""
    echo "Stopping iperf3 server..."
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
}

trap cleanup EXIT

echo "Running TCP bandwidth test..."
echo "----------------------------------------"
iperf3 -c $SERVER_HOST -p $PORT -t $DURATION -f m

echo ""
echo "Running UDP bandwidth test..."
echo "----------------------------------------"
iperf3 -c $SERVER_HOST -p $PORT -t $DURATION -u -b 100M -f m

echo ""
echo "Running bidirectional test..."
echo "----------------------------------------"
iperf3 -c $SERVER_HOST -p $PORT -t $DURATION --bidir -f m

echo ""
echo "Running parallel streams test..."
echo "----------------------------------------"
iperf3 -c $SERVER_HOST -p $PORT -t $DURATION -P 4 -f m

echo ""
echo "Benchmark complete!"

