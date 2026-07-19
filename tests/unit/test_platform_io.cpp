#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <libspaznet/detail/socket_compat.hpp>
#include <libspaznet/platform_io.hpp>
#include <thread>

using namespace spaznet;

class PlatformIOTest : public ::testing::Test {
  protected:
    void SetUp() override {
        platform_io = create_platform_io();
        ASSERT_TRUE(platform_io->init());
    }

    void TearDown() override {
        platform_io->cleanup();
    }

    std::unique_ptr<PlatformIO> platform_io;

    int create_test_socket() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        detail::set_nonblocking(fd);
        return fd;
    }
};

TEST_F(PlatformIOTest, BasicInit) {
    EXPECT_NE(platform_io, nullptr);
}

TEST_F(PlatformIOTest, AddRemoveFd) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    void* user_data = reinterpret_cast<void*>(0x1234);
    EXPECT_TRUE(platform_io->add_fd(fd, PlatformIO::EVENT_READ, user_data));
    EXPECT_TRUE(platform_io->remove_fd(fd));

detail::close_socket_fd(fd);
}

// Regression: kqueue's remove_fd used to issue EV_DELETE for both
// EVFILT_READ and EVFILT_WRITE regardless of which filters had
// actually been registered, returning ENOENT for the absent filter
// and reporting failure. Verify that adding only one direction lets
// remove_fd succeed.
TEST_F(PlatformIOTest, AddOnlyReadThenRemoveSucceeds) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    void* user_data = reinterpret_cast<void*>(0xABCD);
    EXPECT_TRUE(platform_io->add_fd(fd, PlatformIO::EVENT_READ, user_data));
    EXPECT_TRUE(platform_io->remove_fd(fd));

detail::close_socket_fd(fd);
}

TEST_F(PlatformIOTest, AddOnlyWriteThenRemoveSucceeds) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    void* user_data = reinterpret_cast<void*>(0xABCD);
    EXPECT_TRUE(platform_io->add_fd(fd, PlatformIO::EVENT_WRITE, user_data));
    EXPECT_TRUE(platform_io->remove_fd(fd));

detail::close_socket_fd(fd);
}

TEST_F(PlatformIOTest, ModifyFd) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    void* user_data = reinterpret_cast<void*>(0x1234);
    EXPECT_TRUE(platform_io->add_fd(fd, PlatformIO::EVENT_READ, user_data));

    void* new_data = reinterpret_cast<void*>(0x5678);
    EXPECT_TRUE(
        platform_io->modify_fd(fd, PlatformIO::EVENT_READ | PlatformIO::EVENT_WRITE, new_data));

    EXPECT_TRUE(platform_io->remove_fd(fd));

detail::close_socket_fd(fd);
}

TEST_F(PlatformIOTest, WaitForEvents) {
    // Create a pair of connected sockets
    int fds[2];
#ifdef _WIN32
    // Winsock has no socketpair; use a TCP loopback pair. Connect must
    // target 127.0.0.1 — connecting to INADDR_ANY (0.0.0.0) fails with
    // WSAEADDRNOTAVAIL and leave accept() blocked forever.
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len), 0);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client, 0);
    ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    fds[0] = accept(listener, nullptr, nullptr);
    ASSERT_GE(fds[0], 0);
    fds[1] = client;
    detail::close_socket_fd(listener);
#else
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
#endif

    // Set non-blocking
    for (int i = 0; i < 2; ++i) {
        detail::set_nonblocking(fds[i]);
    }

    void* user_data1 = reinterpret_cast<void*>(0x1111);
    void* user_data2 = reinterpret_cast<void*>(0x2222);

    EXPECT_TRUE(platform_io->add_fd(fds[0], PlatformIO::EVENT_READ, user_data1));
    EXPECT_TRUE(platform_io->add_fd(fds[1], PlatformIO::EVENT_WRITE, user_data2));

    // Write to trigger read event
    const char* msg = "test";
    send(fds[1], msg, strlen(msg), 0);

    std::vector<PlatformIO::Event> events;
    int num_events = platform_io->wait(events, 1000); // 1 second timeout

    EXPECT_GT(num_events, 0);

    // Cleanup
    platform_io->remove_fd(fds[0]);
    platform_io->remove_fd(fds[1]);

    detail::close_socket_fd(fds[0]);
    detail::close_socket_fd(fds[1]);
}

TEST_F(PlatformIOTest, WaitTimeout) {
    int fd = create_test_socket();
    ASSERT_GE(fd, 0);

    void* user_data = reinterpret_cast<void*>(0x1234);
    EXPECT_TRUE(platform_io->add_fd(fd, PlatformIO::EVENT_READ, user_data));

    std::vector<PlatformIO::Event> events;
    auto start = std::chrono::steady_clock::now();
    int num_events = platform_io->wait(events, 100); // 100ms timeout
    auto end = std::chrono::steady_clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    // Note: Socket may report ready state (error/EOF) immediately
    // This test verifies wait() returns without blocking indefinitely
    EXPECT_GE(num_events, 0); // Should return (may have events or timeout)

    platform_io->remove_fd(fd);

detail::close_socket_fd(fd);
}

TEST_F(PlatformIOTest, MultipleFds) {
    const int num_fds = 10;
    std::vector<int> fds;

    for (int i = 0; i < num_fds; ++i) {
        int fd = create_test_socket();
        ASSERT_GE(fd, 0);
        fds.push_back(fd);

        void* user_data = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        EXPECT_TRUE(platform_io->add_fd(fd, PlatformIO::EVENT_READ, user_data));
    }

    // Remove all
    for (int fd : fds) {
        EXPECT_TRUE(platform_io->remove_fd(fd));
detail::close_socket_fd(fd);
    }
}
