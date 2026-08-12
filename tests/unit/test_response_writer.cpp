// ResponseWriter<Response>: the runtime-neutral completion token backing
// the synchronous handler API. Zero coroutine dependency, so (like
// test_reactor_primitives.cpp / test_buffered_connection.cpp) this file is
// built unconditionally regardless of SPAZNET_HAS_COROUTINES.

#include <gtest/gtest.h>

#include <atomic>
#include <libspaznet/reactor/response_writer.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace spaznet;

TEST(ResponseWriterTest, SynchronousCompleteRunsDeliverInline) {
    std::string delivered;
    ResponseWriter<std::string> writer([&](std::string r) { delivered = std::move(r); });

    EXPECT_FALSE(writer.is_completed());
    writer.complete("hello");
    EXPECT_TRUE(writer.is_completed());
    EXPECT_EQ(delivered, "hello");
}

TEST(ResponseWriterTest, SecondCompleteIsIgnored) {
    int deliveries = 0;
    std::string last;
    ResponseWriter<std::string> writer([&](std::string r) {
        ++deliveries;
        last = std::move(r);
    });

    writer.complete("first");
    writer.complete("second"); // must be a no-op: first writer wins
    EXPECT_EQ(deliveries, 1);
    EXPECT_EQ(last, "first");
}

TEST(ResponseWriterTest, CopiesShareCompletionState) {
    // A handler that stores a copy elsewhere (to answer later) and the
    // dispatcher's original both refer to the same underlying answer.
    std::string delivered;
    ResponseWriter<std::string> original([&](std::string r) { delivered = std::move(r); });
    ResponseWriter<std::string> stashed = original;

    stashed.complete("deferred answer");
    EXPECT_TRUE(original.is_completed());
    EXPECT_EQ(delivered, "deferred answer");

    // Completing through the other copy afterward is still a no-op.
    original.complete("too late");
    EXPECT_EQ(delivered, "deferred answer");
}

TEST(ResponseWriterTest, OnReadyFiresImmediatelyIfAlreadyCompleted) {
    ResponseWriter<int> writer([](int) {});
    writer.complete(42);

    bool fired = false;
    writer.on_ready([&] { fired = true; });
    EXPECT_TRUE(fired); // no waiting required: already done, so it runs inline
}

TEST(ResponseWriterTest, OnReadyFiresExactlyOnceWhenCompleteIsCalledLater) {
    ResponseWriter<int> writer([](int) {});

    int fire_count = 0;
    writer.on_ready([&] { ++fire_count; });
    EXPECT_EQ(fire_count, 0);

    writer.complete(7);
    EXPECT_EQ(fire_count, 1);
}

TEST(ResponseWriterTest, CompleteFromAnotherThreadStillDeliversAndNotifies) {
    ResponseWriter<int> writer([](int) {});
    std::atomic<bool> ready{false};
    writer.on_ready([&] { ready.store(true); });

    std::thread worker([writer]() mutable { writer.complete(99); });
    worker.join();

    EXPECT_TRUE(ready.load());
    EXPECT_TRUE(writer.is_completed());
}

TEST(ResponseWriterTest, MoveOnlyResponseTypeWorks) {
    // Response types in this codebase (HTTPResponse, etc.) are movable
    // aggregates; make sure the writer doesn't require copyability.
    struct MoveOnly {
        std::vector<int> data;
        MoveOnly() = default;
        MoveOnly(const MoveOnly&) = delete;
        auto operator=(const MoveOnly&) -> MoveOnly& = delete;
        MoveOnly(MoveOnly&&) = default;
        auto operator=(MoveOnly&&) -> MoveOnly& = default;
    };

    std::vector<int> delivered;
    ResponseWriter<MoveOnly> writer([&](MoveOnly r) { delivered = std::move(r.data); });

    MoveOnly response;
    response.data = {1, 2, 3};
    writer.complete(std::move(response));

    EXPECT_EQ(delivered, (std::vector<int>{1, 2, 3}));
}
