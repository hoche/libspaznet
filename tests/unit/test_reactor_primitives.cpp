// Reactor primitives that have no coroutine dependency: IOContext::post(),
// IOContext::add_timer_callback(), and IOContext::defer_destruction() (the
// reap list). These must all work identically whether or not
// SPAZNET_HAS_COROUTINES is defined, so this file (unlike
// test_io_context.cpp / test_timer.cpp / test_task_queue.cpp) is built
// unconditionally.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <libspaznet/platform/io_context.hpp>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace spaznet;

namespace {

class ReactorPrimitivesTest : public ::testing::Test {
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

// Poll a condition with a deadline instead of a fixed sleep, since post()
// and timers run asynchronously on the run() thread.
template <typename Pred> auto wait_until(Pred pred, std::chrono::milliseconds timeout = 500ms) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

} // namespace

TEST_F(ReactorPrimitivesTest, PostRunsOnEventLoop) {
    std::atomic<bool> ran{false};
    context->post([&ran]() { ran.store(true); });
    EXPECT_TRUE(wait_until([&] { return ran.load(); }));
}

TEST_F(ReactorPrimitivesTest, PostAlwaysQueuesEvenNonThreaded) {
    // post() must not run inline on the calling thread the way schedule()'s
    // non-threaded fast path resumes a coroutine inline. Verify the
    // callback runs on the IOContext's run() thread, not this test thread.
    std::atomic<std::thread::id> ran_on{};
    context->post([&ran_on]() { ran_on.store(std::this_thread::get_id()); });
    EXPECT_TRUE(wait_until([&] { return ran_on.load() != std::thread::id{}; }));
    EXPECT_EQ(ran_on.load(), io_thread.get_id());
    EXPECT_NE(ran_on.load(), std::this_thread::get_id());
}

TEST_F(ReactorPrimitivesTest, PostFromMultipleThreadsAllRun) {
    constexpr int kPosters = 8;
    constexpr int kPerThread = 50;
    std::atomic<int> counter{0};

    std::vector<std::thread> posters;
    posters.reserve(kPosters);
    for (int t = 0; t < kPosters; ++t) {
        posters.emplace_back([this, &counter]() {
            for (int i = 0; i < kPerThread; ++i) {
                context->post([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    for (auto& t : posters) {
        t.join();
    }

    EXPECT_TRUE(wait_until([&] { return counter.load() == kPosters * kPerThread; }, 2000ms));
}

TEST_F(ReactorPrimitivesTest, TimerCallbackFiresOnce) {
    std::atomic<int> hits{0};
    context->add_timer_callback(std::chrono::steady_clock::now() + 20ms, {}, /*repeat=*/false,
                                [&hits]() { hits.fetch_add(1); });

    EXPECT_TRUE(wait_until([&] { return hits.load() >= 1; }));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(hits.load(), 1); // one-shot: must not fire again.
}

TEST_F(ReactorPrimitivesTest, TimerCallbackRepeats) {
    std::atomic<int> hits{0};
    context->add_timer_callback(std::chrono::steady_clock::now() + 10ms, 15ms, /*repeat=*/true,
                                [&hits]() { hits.fetch_add(1); });

    EXPECT_TRUE(wait_until([&] { return hits.load() >= 3; }, 1000ms));
}

TEST_F(ReactorPrimitivesTest, TimerCallbackCancelStopsFiring) {
    std::atomic<int> hits{0};
    uint64_t id = context->add_timer_callback(std::chrono::steady_clock::now() + 15ms, 15ms,
                                              /*repeat=*/true, [&hits]() { hits.fetch_add(1); });

    EXPECT_TRUE(wait_until([&] { return hits.load() >= 1; }));
    context->cancel_timer(id);
    int hits_at_cancel = hits.load();
    std::this_thread::sleep_for(60ms);
    // Allow at most one more in-flight fire race, but it must not keep going.
    EXPECT_LE(hits.load(), hits_at_cancel + 1);
}

// The core scenario defer_destruction exists for: an object drops its own
// last owning reference from inside a callback that IOContext invoked on
// it. Without the reap list, the object would be destroyed synchronously
// right there, mid-callback. With it, destruction is deferred until the
// loop iteration finishes.
TEST_F(ReactorPrimitivesTest, DeferDestructionSurvivesUntilAfterCallback) {
    auto destroyed = std::make_shared<std::atomic<bool>>(false);

    struct Sentinel {
        std::shared_ptr<std::atomic<bool>> flag;
        explicit Sentinel(std::shared_ptr<std::atomic<bool>> f) : flag(std::move(f)) {}
        ~Sentinel() {
            flag->store(true);
        }
    };

    auto sentinel = std::make_shared<Sentinel>(destroyed);
    std::atomic<bool> still_alive_inside_callback{false};
    std::atomic<bool> callback_ran{false};

    // Post a callback that holds the only remaining shared_ptr to
    // `sentinel`, hands it to defer_destruction, then drops its own copy.
    // If defer_destruction worked correctly, the object must still be
    // alive at the point we check inside this very callback. Moved into
    // the capture (rather than captured by value) so the local `sentinel`
    // is not itself a second owning reference that outlives the test body.
    context->post([sentinel = std::move(sentinel), &still_alive_inside_callback, &callback_ran, this]() mutable {
        auto* raw = sentinel.get();
        context->defer_destruction(std::move(sentinel));
        // sentinel is now moved-from (empty); the reap list is the only
        // owner. Check the flag through the still-valid raw pointer,
        // which is fine to read because nothing has run its destructor.
        still_alive_inside_callback.store(!raw->flag->load());
        callback_ran.store(true);
    });

    EXPECT_TRUE(wait_until([&] { return callback_ran.load(); }));
    EXPECT_TRUE(still_alive_inside_callback.load()) << "object was destroyed before its callback returned";

    // After the loop iteration that ran the callback finishes, the reap
    // list should have been drained and the object destroyed.
    EXPECT_TRUE(wait_until([&] { return destroyed->load(); }))
        << "object was never reaped after its callback returned";
}

TEST_F(ReactorPrimitivesTest, DeferDestructionIgnoresNull) {
    // Should not crash or otherwise misbehave on a null shared_ptr.
    EXPECT_NO_THROW(context->defer_destruction(nullptr));
}
