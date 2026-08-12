#pragma once

// Reactor-side I/O layer: BufferedConnection wraps a raw, non-blocking fd
// with InputBuffer/OutputBuffer and implements IoHandler directly — no
// coroutine involved anywhere in this file. This is what a reactor
// dispatcher (e.g. the future Http1Connection) is built on, in the same
// way Socket::async_read/async_write is what a coroutine dispatcher is
// built on.
//
// See docs/concurrency-and-coroutines.md and the reactor-port plan for the
// execution model this fits into.

#include <cstdint>
#include <functional>
#include <libspaznet/platform/io_context.hpp>
#include <memory>
#include <span>
#include <vector>

namespace spaznet {

// Growable byte buffer for incoming data, written to directly by a raw
// read(2)/recv(2) via prepare()/commit(), and consumed incrementally by a
// parser via data()/consume(). Replaces the "read_exact" pattern coroutine
// dispatchers use (co_await enough bytes) with "are N bytes buffered? no
// -> return and wait for more on_readable()".
class InputBuffer {
  public:
    // Returns at least `min_capacity` contiguous writable bytes at the end
    // of the buffer, compacting the already-consumed prefix first if that
    // alone makes room (so a connection that's read from for a long time
    // doesn't grow this without bound). Read directly into the returned
    // span, then call commit() with the number of bytes actually read.
    auto prepare(std::size_t min_capacity) -> std::span<uint8_t>;

    // Record that `n` bytes of the span the last prepare() call returned
    // now hold real data.
    void commit(std::size_t n);

    // Drop the first `n` bytes (already consumed by a parser). Resets to
    // an empty buffer (freeing growth from a large single read) once
    // everything buffered has been consumed.
    void consume(std::size_t n);

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return write_pos_ - read_pos_;
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
        return size() == 0;
    }
    [[nodiscard]] auto data() const noexcept -> std::span<const uint8_t> {
        return {buf_.data() + read_pos_, size()};
    }

  private:
    std::vector<uint8_t> buf_;
    std::size_t read_pos_{0};
    std::size_t write_pos_{0};
};

// Growable byte buffer for outgoing data. append() queues bytes; try_flush()
// writes as much as the kernel will accept right now without blocking.
// Serializes writes by construction — replaces both HTTP/2's writer_loop +
// out_queue and WebSocket's WriteGate async mutex, neither of which have a
// reactor-side analogue because this subsumes them.
class OutputBuffer {
  public:
    enum class Result { Flushed, WouldBlock, Error };

    void append(const uint8_t* bytes, std::size_t len);
    void append(std::vector<uint8_t> bytes);

    [[nodiscard]] auto pending() const noexcept -> std::size_t {
        return buf_.size() - read_pos_;
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
        return pending() == 0;
    }

    // Write as much of the buffered bytes to `fd` as a single non-blocking
    // send(2) loop will accept. Flushed means the buffer is now empty;
    // WouldBlock means bytes remain (caller should ensure write interest
    // is armed); Error means a hard failure (caller should close the
    // connection).
    auto try_flush(int fd) -> Result;

  private:
    std::vector<uint8_t> buf_;
    std::size_t read_pos_{0};
};

// A single TCP (or other stream-socket) connection driven purely by
// IoHandler callbacks — no coroutine frame, no co_await. Owns the fd,
// applies backpressure via OutputBuffer, and manages its own read/write
// interest with IOContext::set_io_handler (persistent for reads: it
// re-arms itself after draining to EAGAIN, matching the "read to EAGAIN or
// drop read interest" rule for level-triggered persistent handlers).
//
// Lifetime: BufferedConnection is always held via shared_ptr (required by
// IoHandler registration, which stores shared_ptr<IoHandler>). A callback
// (on_data, on_closed) that wants to drop its own reference to the
// connection while still running from inside one of that connection's own
// callbacks should route the drop through
// IOContext::defer_destruction(shared_ptr) instead of destroying it
// synchronously — see that method's comment.
class BufferedConnection : public IoHandler, public std::enable_shared_from_this<BufferedConnection> {
  public:
    using DataCallback = std::function<void()>;
    using ClosedCallback = std::function<void()>;

    BufferedConnection(IOContext& ctx, int fd);
    ~BufferedConnection() override;

    BufferedConnection(const BufferedConnection&) = delete;
    auto operator=(const BufferedConnection&) -> BufferedConnection& = delete;
    BufferedConnection(BufferedConnection&&) = delete;
    auto operator=(BufferedConnection&&) -> BufferedConnection& = delete;

    // Fires whenever new bytes have landed in input(); the callback should
    // parse whatever it can and call input().consume() for what it used.
    // Not fired again until on_readable() next receives new data — i.e.
    // exactly the bytes currently in input() are all that's available.
    void set_on_data(DataCallback cb) {
        on_data_ = std::move(cb);
    }

    // Fires exactly once: on orderly EOF, a hard read/write error, or an
    // explicit close(). No other callback fires after this one.
    void set_on_closed(ClosedCallback cb) {
        on_closed_ = std::move(cb);
    }

    // Begin listening for readability. Call once, after installing the
    // callbacks above.
    void start();

    [[nodiscard]] auto input() noexcept -> InputBuffer& {
        return input_;
    }
    [[nodiscard]] auto fd() const noexcept -> int {
        return fd_;
    }
    [[nodiscard]] auto closed() const noexcept -> bool {
        return closed_;
    }
    [[nodiscard]] auto pending_write_bytes() const noexcept -> std::size_t {
        return output_.pending();
    }

    // Queue `data` for writing. If nothing else is queued, attempts an
    // immediate optimistic write; whatever doesn't fit is buffered and
    // write interest is armed so on_writable() drains the remainder.
    void write(std::vector<uint8_t> data);

    // Stop listening, close the fd, and fire on_closed() if it hasn't
    // already run. Safe to call from within one of this object's own
    // callbacks (on_data/on_closed/on_readable/on_writable): closing does
    // not destroy `this`, it just releases the fd and flips `closed_`.
    void close();

    void on_readable() override;
    void on_writable() override;
    void on_error() override;
    void shutdown() override {
        close();
    }

  private:
    void fail(); // shared "hard failure" path: same as close(), named for call sites' intent.
    // Reports the net change in output_.pending() (relative to `before`,
    // its value at the start of whichever operation just ran) to
    // IOContext's global bytes_buffered gauge. A no-op if unchanged.
    void report_buffered_delta(std::size_t before);

    IOContext& ctx_;
    int fd_;
    InputBuffer input_;
    OutputBuffer output_;
    DataCallback on_data_;
    ClosedCallback on_closed_;
    bool closed_{false};
    bool write_interest_armed_{false};
};

} // namespace spaznet
