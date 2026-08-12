#include <algorithm>
#include <cstring>
#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/reactor/buffered_connection.hpp>

namespace spaznet {

auto InputBuffer::prepare(std::size_t min_capacity) -> std::span<uint8_t> {
    // Compact first: if the already-consumed prefix alone makes enough
    // room, reuse it instead of growing the buffer.
    if (read_pos_ > 0 && (buf_.size() - write_pos_) < min_capacity) {
        std::size_t remaining = write_pos_ - read_pos_;
        if (remaining > 0) {
            std::memmove(buf_.data(), buf_.data() + read_pos_, remaining);
        }
        read_pos_ = 0;
        write_pos_ = remaining;
    }
    if (buf_.size() - write_pos_ < min_capacity) {
        buf_.resize(write_pos_ + min_capacity);
    }
    return {buf_.data() + write_pos_, buf_.size() - write_pos_};
}

void InputBuffer::commit(std::size_t n) {
    write_pos_ += n;
}

void InputBuffer::consume(std::size_t n) {
    read_pos_ += (std::min)(n, write_pos_ - read_pos_);
    if (read_pos_ == write_pos_) {
        // Fully drained: reset rather than let read_pos_/write_pos_ climb
        // forever on a long-lived connection.
        read_pos_ = 0;
        write_pos_ = 0;
    }
}

void OutputBuffer::append(const uint8_t* bytes, std::size_t len) {
    if (len == 0) {
        return;
    }
    if (read_pos_ >= buf_.size()) {
        buf_.clear();
        read_pos_ = 0;
    }
    // resize + copy rather than vector::insert's iterator-range overload:
    // functionally identical, but avoids a GCC 13 -Wstringop-overflow
    // false positive that fires on the inlined insert() path here.
    std::size_t old_size = buf_.size();
    buf_.resize(old_size + len);
    std::copy(bytes, bytes + len, buf_.begin() + static_cast<std::ptrdiff_t>(old_size));
}

void OutputBuffer::append(std::vector<uint8_t> bytes) {
    if (bytes.empty()) {
        return;
    }
    if (read_pos_ >= buf_.size()) {
        // Nothing pending: take ownership directly instead of copying.
        buf_ = std::move(bytes);
        read_pos_ = 0;
    } else {
        append(bytes.data(), bytes.size());
    }
}

auto OutputBuffer::try_flush(int fd) -> Result {
    while (read_pos_ < buf_.size()) {
        ssize_t n = detail::socket_send(fd, buf_.data() + read_pos_, buf_.size() - read_pos_,
                                        MSG_NOSIGNAL);
        if (n > 0) {
            read_pos_ += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            return Result::WouldBlock;
        }
        int err = detail::last_socket_error();
        if (detail::is_retryable_socket_error(err)) {
            return Result::WouldBlock;
        }
        return Result::Error;
    }
    // Fully drained: reset so a long-lived, mostly-idle connection doesn't
    // hold onto a buffer sized for its largest burst forever.
    buf_.clear();
    read_pos_ = 0;
    return Result::Flushed;
}

BufferedConnection::BufferedConnection(IOContext& ctx, int fd) : ctx_(ctx), fd_(fd) {
    detail::set_nonblocking(fd_);
}

BufferedConnection::~BufferedConnection() {
    // Deliberately do NOT invoke on_closed_ here even if it never ran:
    // this destructor only runs once every shared_ptr<BufferedConnection>
    // is gone, so a callback that calls shared_from_this() would hit
    // std::bad_weak_ptr. Callers that need the notification must call
    // close() explicitly (directly, or via IOContext::defer_destruction
    // followed by dropping their reference) before the last reference
    // goes away.
    if (!closed_) {
        ctx_.remove_io(fd_);
        detail::close_socket_fd(fd_);
    }
}

void BufferedConnection::start() {
    if (closed_) {
        return;
    }
    ctx_.set_io_handler(fd_, PlatformIO::EVENT_READ, shared_from_this());
}

void BufferedConnection::on_readable() {
    if (closed_) {
        return;
    }
    constexpr std::size_t kReadChunk = 4096;
    for (;;) {
        auto span = input_.prepare(kReadChunk);
        ssize_t n = detail::socket_recv(fd_, span.data(), span.size(), 0);
        if (n > 0) {
            input_.commit(static_cast<std::size_t>(n));
            if (on_data_) {
                on_data_();
            }
            if (closed_) {
                return; // on_data_ closed us (protocol error, etc).
            }
            continue;
        }
        if (n == 0) {
            fail(); // Orderly EOF.
            return;
        }
        int err = detail::last_socket_error();
        if (detail::is_retryable_socket_error(err)) {
            break; // Drained to EAGAIN; re-arm read interest below.
        }
        fail();
        return;
    }

    // Persistent read interest: re-register so the next readable event
    // still reaches on_readable(). Preserve write interest if it's
    // currently armed — set_io_handler only touches the bits present in
    // `events`, so passing just EVENT_READ here cannot accidentally
    // disarm a write registration made independently by write().
    uint32_t events = PlatformIO::EVENT_READ;
    if (write_interest_armed_) {
        events |= PlatformIO::EVENT_WRITE;
    }
    ctx_.set_io_handler(fd_, events, shared_from_this());
}

void BufferedConnection::on_writable() {
    if (closed_) {
        return;
    }
    std::size_t before = output_.pending();
    auto result = output_.try_flush(fd_);
    report_buffered_delta(before);
    if (result == OutputBuffer::Result::Error) {
        fail();
        return;
    }
    if (result == OutputBuffer::Result::Flushed) {
        write_interest_armed_ = false;
        ctx_.set_io_handler(fd_, PlatformIO::EVENT_READ, shared_from_this());
        return;
    }
    // WouldBlock: still bytes queued. Firing is one-shot per direction, so
    // re-arm both (read interest is independently tracked the same way).
    ctx_.set_io_handler(fd_, PlatformIO::EVENT_READ | PlatformIO::EVENT_WRITE, shared_from_this());
}

void BufferedConnection::on_error() {
    fail();
}

void BufferedConnection::write(std::vector<uint8_t> data) {
    if (closed_ || data.empty()) {
        return;
    }
    std::size_t before = output_.pending();
    bool was_empty = output_.empty();
    output_.append(std::move(data));

    if (was_empty) {
        // Optimistic write: try to drain immediately rather than waiting
        // for a writable event that may not even be necessary (the common
        // case for a socket with room in its send buffer).
        auto result = output_.try_flush(fd_);
        if (result == OutputBuffer::Result::Error) {
            report_buffered_delta(before);
            fail();
            return;
        }
        if (result == OutputBuffer::Result::Flushed) {
            report_buffered_delta(before);
            return; // Nothing left to do; no write interest needed.
        }
        // WouldBlock: fall through to arm write interest.
    }

    if (!write_interest_armed_) {
        write_interest_armed_ = true;
        ctx_.set_io_handler(fd_, PlatformIO::EVENT_READ | PlatformIO::EVENT_WRITE, shared_from_this());
    }
    report_buffered_delta(before);
}

void BufferedConnection::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (output_.pending() > 0) {
        // Going away with bytes still queued: they're never getting
        // flushed now, so drop them from the global gauge here rather
        // than leaking that count for the rest of the process lifetime.
        ctx_.adjust_bytes_buffered(-static_cast<std::int64_t>(output_.pending()));
    }
    ctx_.remove_io(fd_);
    detail::close_socket_fd(fd_);
    if (on_closed_) {
        on_closed_();
    }
}

void BufferedConnection::fail() {
    close();
}

void BufferedConnection::report_buffered_delta(std::size_t before) {
    auto after = static_cast<std::int64_t>(output_.pending());
    std::int64_t delta = after - static_cast<std::int64_t>(before);
    if (delta != 0) {
        ctx_.adjust_bytes_buffered(delta);
    }
}

} // namespace spaznet
