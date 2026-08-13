// BufferedConnection and its InputBuffer/OutputBuffer: the reactor-side
// I/O layer with zero coroutine dependency. Covers buffer drain (grow,
// compact, partial write/flush), read/write interest toggling end to end
// over a real socket pair, and self-destruction from within a callback
// via IOContext::defer_destruction.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/platform/io_context.hpp>
#include <libspaznet/reactor/buffered_connection.hpp>
#include <memory>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;
using namespace spaznet;

namespace {

// A connected, non-blocking pair of stream-socket fds. AF_UNIX socketpair
// on POSIX; a loopback TCP pair on Windows (mirrors IOContext's own
// wakeup-pipe fallback in src/platform/io_context.cpp).
struct SocketPair {
    int a{-1};
    int b{-1};

    SocketPair() {
#ifdef _WIN32
        // Not exercised in this environment, but kept correct rather than
        // left as a silent gap: connect two Winsock TCP sockets over
        // loopback the same way IOContext's wakeup pipe does on Windows.
        detail::ensure_winsock();
        SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        int addr_len = sizeof(addr);
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len);
        ::listen(listener, 1);
        SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        SOCKET server = ::accept(listener, nullptr, nullptr);
        closesocket(listener);
        a = static_cast<int>(server);
        b = static_cast<int>(client);
#else
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            a = fds[0];
            b = fds[1];
        }
#endif
        if (a >= 0) {
            detail::set_nonblocking(a);
        }
        if (b >= 0) {
            detail::set_nonblocking(b);
        }
    }

    ~SocketPair() {
        // BufferedConnection tests close `a` themselves (it's the fd
        // handed to the connection under test); only close `b` here.
        if (b >= 0) {
            detail::close_socket_fd(b);
        }
    }

    SocketPair(const SocketPair&) = delete;
    auto operator=(const SocketPair&) -> SocketPair& = delete;
};

template <typename Pred> auto wait_until(Pred pred, std::chrono::milliseconds timeout = 1000ms) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

// Shrink both the writer's send buffer and the peer's receive buffer so a
// large write is forced to WouldBlock. SO_SNDBUF alone is unreliable:
// Darwin and Windows often clamp or ignore small values, and a large peer
// RCVBUF lets the kernel accept megabytes before backpressuring.
void shrink_pipe_buffers(int write_fd, int read_fd, int bytes = 2048) {
    detail::setsockopt_int(write_fd, SOL_SOCKET, SO_SNDBUF, bytes);
    detail::setsockopt_int(read_fd, SOL_SOCKET, SO_RCVBUF, bytes);
}

// Large enough to exceed a shrunk pipe, small enough to drain quickly on
// loaded CI runners (1 MiB + 5s was flaking on macOS ARM64 under load).
constexpr std::size_t kBackpressurePayload = 256 * 1024;
constexpr auto kBackpressureTimeout = 15000ms;

// BufferedConnection has no internal locking: write()/close()/
// close_after_flush()/pending_write_bytes()/closed() all assume the
// caller is on the same thread that drives this connection's
// on_readable()/on_writable() (see buffered_connection.hpp's class
// comment and IOContext::post_to_io_thread()'s rationale). These tests
// run IOContext::run() on a dedicated background thread (see SetUp()
// below) and need to touch the connection from the *test* thread, so
// every such touch is marshaled onto the IO thread through
// post_to_io_thread() — exactly what a real reactor dispatcher's
// ResponseWriter completion does — rather than calling BufferedConnection
// directly from here and racing the IO thread.
template <typename Fn> auto on_io_thread(IOContext& ctx, Fn fn) -> decltype(fn()) {
    using R = decltype(fn());
    std::atomic<bool> done{false};
    if constexpr (std::is_void_v<R>) {
        ctx.post_to_io_thread([&fn, &done]() {
            fn();
            done.store(true, std::memory_order_release);
        });
        while (!done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
    } else {
        R result{};
        ctx.post_to_io_thread([&fn, &done, &result]() {
            result = fn();
            done.store(true, std::memory_order_release);
        });
        while (!done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        return result;
    }
}

class BufferedConnectionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        context = std::make_unique<IOContext>();
        io_thread = std::thread([this]() { context->run(); });
        std::this_thread::sleep_for(20ms);
    }

    void TearDown() override {
        context->stop();
        if (io_thread.joinable()) {
            io_thread.join();
        }
        context.reset();
    }

    std::unique_ptr<IOContext> context;
    std::thread io_thread;
};

} // namespace

// ---- InputBuffer -----------------------------------------------------

TEST(InputBufferTest, PrepareCommitDataRoundTrip) {
    InputBuffer buf;
    auto span = buf.prepare(16);
    ASSERT_GE(span.size(), 16u);
    std::memcpy(span.data(), "hello world", 11);
    buf.commit(11);

    EXPECT_EQ(buf.size(), 11u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data().data()), buf.data().size()),
             "hello world");
}

TEST(InputBufferTest, ConsumePartialThenRemaining) {
    InputBuffer buf;
    auto span = buf.prepare(16);
    std::memcpy(span.data(), "0123456789", 10);
    buf.commit(10);

    buf.consume(4);
    EXPECT_EQ(buf.size(), 6u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data().data()), buf.data().size()),
             "456789");

    buf.consume(6);
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_TRUE(buf.empty());
}

TEST(InputBufferTest, CompactsInsteadOfGrowingUnnecessarily) {
    InputBuffer buf;
    // Fill, consume most of it, then ask for more than the tail-room but
    // less than what compaction should free up.
    auto span = buf.prepare(100);
    std::memset(span.data(), 'a', 100);
    buf.commit(100);
    buf.consume(90); // 10 bytes remain, ~90 bytes of consumed prefix to reclaim

    auto span2 = buf.prepare(50);
    // After compaction, remaining 10 bytes should sit at the front, and
    // there should be no need to grow the backing buffer beyond its
    // current 100-byte capacity to satisfy a 50-byte request.
    EXPECT_GE(span2.size(), 50u);
    EXPECT_EQ(buf.size(), 10u);
}

TEST(InputBufferTest, GrowsWhenCompactionIsNotEnough) {
    InputBuffer buf;
    auto span = buf.prepare(10);
    std::memset(span.data(), 'x', 10);
    buf.commit(10);

    // Nothing consumed, so compaction can't help; must grow.
    auto span2 = buf.prepare(1000);
    EXPECT_GE(span2.size(), 1000u);
    EXPECT_EQ(buf.size(), 10u); // still-buffered bytes untouched
}

// ---- OutputBuffer ------------------------------------------------------

TEST(OutputBufferTest, FlushesSmallPayloadImmediately) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);

    OutputBuffer out;
    std::vector<uint8_t> payload{'h', 'i'};
    out.append(payload);
    EXPECT_EQ(out.try_flush(pair.a), OutputBuffer::Result::Flushed);
    EXPECT_TRUE(out.empty());

    std::array<char, 16> recv_buf{};
    ssize_t n = detail::socket_recv(pair.b, recv_buf.data(), recv_buf.size(), 0);
    ASSERT_EQ(n, 2);
    EXPECT_EQ(std::string(recv_buf.data(), n), "hi");

    detail::close_socket_fd(pair.a);
    pair.a = -1;
}

TEST(OutputBufferTest, PartialFlushLeavesRemainderPendingUntilDrained) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);

    // Shrink both ends so a payload larger than the pipe forces a partial
    // (WouldBlock) write without needing tens of megabytes of data.
    shrink_pipe_buffers(pair.a, pair.b);

    OutputBuffer out;
    std::vector<uint8_t> payload(kBackpressurePayload, 'z');
    out.append(payload);

    auto first = out.try_flush(pair.a);
    // Either it all fit (unlikely with a shrunk pipe and no reader
    // draining) or it didn't; either way pending() must reflect reality.
    if (first == OutputBuffer::Result::Flushed) {
        EXPECT_TRUE(out.empty());
    } else {
        ASSERT_EQ(first, OutputBuffer::Result::WouldBlock) << "errno=" << detail::last_socket_error();
        EXPECT_GT(out.pending(), 0u);
    }

    // Drain the reader in a loop, retrying try_flush, until everything's
    // through — proves try_flush is safe to call repeatedly and eventually
    // converges.
    std::size_t total_received = 0;
    std::array<char, 65536> recv_buf{};
    bool flushed = (first == OutputBuffer::Result::Flushed);
    bool saw_error = false;
    ASSERT_TRUE(wait_until(
        [&]() {
            if (!flushed) {
                auto r = out.try_flush(pair.a);
                if (r == OutputBuffer::Result::Flushed) {
                    flushed = true;
                } else if (r == OutputBuffer::Result::Error) {
                    saw_error = true;
                    return true;
                }
            }
            for (;;) {
                ssize_t n = detail::socket_recv(pair.b, recv_buf.data(), recv_buf.size(), 0);
                if (n <= 0) {
                    break;
                }
                total_received += static_cast<std::size_t>(n);
            }
            return flushed && total_received == payload.size();
        },
        kBackpressureTimeout));
    ASSERT_FALSE(saw_error) << "try_flush hard-failed with errno=" << detail::last_socket_error();

    EXPECT_EQ(total_received, payload.size());
    detail::close_socket_fd(pair.a);
    pair.a = -1;
}

// ---- BufferedConnection --------------------------------------------------

TEST_F(BufferedConnectionTest, EchoesReceivedDataBack) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);

    auto conn = std::make_shared<BufferedConnection>(*context, pair.a);
    pair.a = -1; // ownership transferred to the connection
    conn->set_on_data([conn_ptr = conn.get()]() {
        std::vector<uint8_t> echoed(conn_ptr->input().data().begin(), conn_ptr->input().data().end());
        conn_ptr->input().consume(echoed.size());
        conn_ptr->write(std::move(echoed));
    });
    conn->start();

    const char* msg = "ping";
    detail::socket_send(pair.b, msg, 4, 0);

    std::array<char, 16> recv_buf{};
    ASSERT_TRUE(wait_until([&] {
        ssize_t n = detail::socket_recv(pair.b, recv_buf.data(), recv_buf.size(), 0);
        return n == 4 && std::string(recv_buf.data(), 4) == "ping";
    }));
}

TEST_F(BufferedConnectionTest, LargeWriteTogglesWriteInterestThenDrains) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);
    shrink_pipe_buffers(pair.a, pair.b);

    auto conn = std::make_shared<BufferedConnection>(*context, pair.a);
    pair.a = -1;
    conn->start();

    std::vector<uint8_t> payload(kBackpressurePayload, 'y');
    // Keep writing until the kernel pipe backpressures (pending > 0) or we
    // have pushed several payloads — Darwin sometimes absorbs one chunk
    // despite SO_*BUF shrinks.
    std::size_t bytes_written = 0;
    for (int i = 0; i < 64; ++i) {
        on_io_thread(*context, [&]() { conn->write(payload); });
        bytes_written += payload.size();
        if (on_io_thread(*context, [&]() { return conn->pending_write_bytes(); }) > 0) {
            break;
        }
    }

    std::size_t total_received = 0;
    std::array<char, 65536> recv_buf{};
    ASSERT_TRUE(wait_until(
        [&]() {
            for (;;) {
                ssize_t n = detail::socket_recv(pair.b, recv_buf.data(), recv_buf.size(), 0);
                if (n <= 0) {
                    break;
                }
                total_received += static_cast<std::size_t>(n);
            }
            return total_received == bytes_written &&
                   on_io_thread(*context, [&]() { return conn->pending_write_bytes(); }) == 0;
        },
        kBackpressureTimeout));

    EXPECT_EQ(total_received, bytes_written);
    EXPECT_EQ(on_io_thread(*context, [&]() { return conn->pending_write_bytes(); }), 0u);
}

TEST_F(BufferedConnectionTest, BytesBufferedStatDrainsToZeroOnceFlushed) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);
    shrink_pipe_buffers(pair.a, pair.b);

    auto conn = std::make_shared<BufferedConnection>(*context, pair.a);
    pair.a = -1;
    conn->start();

    std::vector<uint8_t> payload(kBackpressurePayload, 'z');
    for (int i = 0; i < 64; ++i) {
        on_io_thread(*context, [&]() { conn->write(payload); });
        if (on_io_thread(*context, [&]() { return conn->pending_write_bytes(); }) > 0) {
            break;
        }
    }
    EXPECT_EQ(context->get_statistics().bytes_buffered,
              on_io_thread(*context, [&]() { return conn->pending_write_bytes(); }));

    std::array<char, 65536> recv_buf{};
    ASSERT_TRUE(wait_until(
        [&]() {
            while (detail::socket_recv(pair.b, recv_buf.data(), recv_buf.size(), 0) > 0) {
                // draining
            }
            return on_io_thread(*context, [&]() { return conn->pending_write_bytes(); }) == 0;
        },
        kBackpressureTimeout));

    EXPECT_EQ(context->get_statistics().bytes_buffered, 0u);
}

TEST_F(BufferedConnectionTest, BytesBufferedStatDroppedOnCloseWithPendingData) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);
    shrink_pipe_buffers(pair.a, pair.b);

    auto conn = std::make_shared<BufferedConnection>(*context, pair.a);
    pair.a = -1;
    conn->start();

    // Peer never reads. Keep writing until OutputBuffer still holds bytes
    // (kernel pipe full). A single 1 MiB write is usually enough once both
    // ends are shrunk, but Windows/Darwin can still accept more than the
    // requested SO_*BUF, so loop.
    std::vector<uint8_t> payload(kBackpressurePayload, 'z');
    std::size_t pending = 0;
    for (int i = 0; i < 64 && pending == 0; ++i) {
        on_io_thread(*context, [&]() { conn->write(payload); });
        pending = on_io_thread(*context, [&]() { return conn->pending_write_bytes(); });
    }
    ASSERT_GT(pending, 0u) << "could not create send backpressure on this platform";
    ASSERT_EQ(context->get_statistics().bytes_buffered, pending);

    on_io_thread(*context, [&]() { conn->close(); });
    EXPECT_EQ(context->get_statistics().bytes_buffered, 0u);
}

TEST_F(BufferedConnectionTest, CloseAfterFlushClosesImmediatelyWhenNothingQueued) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);

    auto conn = std::make_shared<BufferedConnection>(*context, pair.a);
    pair.a = -1;
    conn->start();

    on_io_thread(*context, [&]() { conn->close_after_flush(); });
    EXPECT_TRUE(on_io_thread(*context, [&]() { return conn->closed(); }));
}

TEST_F(BufferedConnectionTest, CloseAfterFlushWaitsForPendingWriteToDrainFirst) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);
    shrink_pipe_buffers(pair.a, pair.b);

    auto conn = std::make_shared<BufferedConnection>(*context, pair.a);
    pair.a = -1;
    conn->start();

    // Fill until something is still queued, then arm close_after_flush.
    // Peer drains below; until then the connection must stay open.
    std::vector<uint8_t> payload(kBackpressurePayload, 'q');
    std::size_t bytes_written = 0;
    std::size_t pending = 0;
    for (int i = 0; i < 64 && pending == 0; ++i) {
        on_io_thread(*context, [&]() { conn->write(payload); });
        bytes_written += payload.size();
        pending = on_io_thread(*context, [&]() { return conn->pending_write_bytes(); });
    }
    ASSERT_GT(pending, 0u) << "could not create send backpressure on this platform";
    on_io_thread(*context, [&]() { conn->close_after_flush(); });
    // Must not have closed yet — bytes are still queued and the peer
    // hasn't read anything.
    EXPECT_FALSE(on_io_thread(*context, [&]() { return conn->closed(); }));

    std::size_t total_received = 0;
    std::array<char, 65536> recv_buf{};
    ASSERT_TRUE(wait_until(
        [&]() {
            // Check closed() FIRST, then always drain whatever's arrived
            // afterward: by the time close_after_flush() actually closes,
            // every byte it flushed has already been handed to the
            // kernel via send() (and is therefore already available to
            // recv()), so draining after the check — rather than before
            // it — can't miss a tail that a slower closed() round trip
            // (now a post_to_io_thread() call, not a plain field read)
            // would otherwise let slip past an EAGAIN-terminated drain
            // loop that ran just before the connection finished closing.
            bool was_closed = on_io_thread(*context, [&]() { return conn->closed(); });
            for (;;) {
                ssize_t n = detail::socket_recv(pair.b, recv_buf.data(), recv_buf.size(), 0);
                if (n <= 0) {
                    break;
                }
                total_received += static_cast<std::size_t>(n);
            }
            return was_closed;
        },
        kBackpressureTimeout));

    EXPECT_EQ(total_received, bytes_written)
        << "close_after_flush() must not truncate data queued before it was called";
    EXPECT_TRUE(on_io_thread(*context, [&]() { return conn->closed(); }));
}

TEST_F(BufferedConnectionTest, PeerCloseFiresOnClosedExactlyOnce) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);

    auto conn = std::make_shared<BufferedConnection>(*context, pair.a);
    pair.a = -1;
    std::atomic<int> closed_count{0};
    conn->set_on_closed([&closed_count]() { closed_count.fetch_add(1); });
    conn->start();

    detail::close_socket_fd(pair.b);
    pair.b = -1;

    ASSERT_TRUE(wait_until([&] { return on_io_thread(*context, [&]() { return conn->closed(); }); }));
    std::this_thread::sleep_for(30ms); // let any spurious extra events settle
    EXPECT_EQ(closed_count.load(), 1);
}

// The scenario defer_destruction exists for, exercised through the actual
// object it was built for: a dispatcher owns connections in a map keyed by
// fd, and one connection's own on_closed_ callback erases it from that map
// (dropping what would otherwise be the last owning reference) while still
// running from inside that connection's on_readable() -> fail() -> close()
// -> on_closed_() call chain.
TEST_F(BufferedConnectionTest, SelfErasureFromOwningMapViaDeferDestruction) {
    SocketPair pair;
    ASSERT_GE(pair.a, 0);
    ASSERT_GE(pair.b, 0);

    auto destroyed = std::make_shared<std::atomic<bool>>(false);

    struct TrackedConnection : BufferedConnection {
        std::shared_ptr<std::atomic<bool>> destroyed_flag;
        TrackedConnection(IOContext& ctx, int fd, std::shared_ptr<std::atomic<bool>> flag)
            : BufferedConnection(ctx, fd), destroyed_flag(std::move(flag)) {}
        ~TrackedConnection() override {
            destroyed_flag->store(true);
        }
    };

    std::unordered_map<int, std::shared_ptr<BufferedConnection>> connections;
    int fd = pair.a;
    auto conn = std::make_shared<TrackedConnection>(*context, fd, destroyed);
    connections[fd] = conn;
    pair.a = -1;

    std::atomic<bool> still_alive_when_on_closed_ran{false};
    conn->set_on_closed([&connections, &still_alive_when_on_closed_ran, this, fd]() {
        auto it = connections.find(fd);
        ASSERT_NE(it, connections.end());
        std::shared_ptr<BufferedConnection> self = it->second; // last external owner
        connections.erase(it);
        // Hand the last reference to the reap list instead of just letting
        // `self` go out of scope here (which would run TrackedConnection's
        // destructor synchronously, mid-callback, on the io thread's stack
        // several frames below where the object's own IoHandler methods
        // are still executing).
        still_alive_when_on_closed_ran.store(true);
        context->defer_destruction(std::move(self));
    });
    conn->start();
    conn.reset(); // test no longer holds a reference either; only the map did.

    detail::close_socket_fd(pair.b); // triggers EOF -> fail() -> close() -> on_closed_
    pair.b = -1;

    ASSERT_TRUE(wait_until([&] { return still_alive_when_on_closed_ran.load(); }));
    ASSERT_TRUE(wait_until([&] { return destroyed->load(); }))
        << "TrackedConnection was never destroyed after being reaped";
    EXPECT_TRUE(connections.empty());
}
