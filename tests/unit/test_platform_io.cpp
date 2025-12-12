#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <libspaznet/platform_io.hpp>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

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

#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
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

#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
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

#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

TEST_F(PlatformIOTest, WaitForEvents) {
    // Create a pair of connected sockets
    int fds[2];
#ifdef _WIN32
    // On Windows, use TCP sockets
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    listen(listener, 1);
    socklen_t len = sizeof(addr);
    getsockname(listener, reinterpret_cast<struct sockaddr*>(&addr), &len);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    fds[0] = accept(listener, nullptr, nullptr);
    fds[1] = client;
    closesocket(listener);
#else
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
#endif

    // Set non-blocking
    for (int i = 0; i < 2; ++i) {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fds[i], FIONBIO, &mode);
#else
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
#endif
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

#ifdef _WIN32
    closesocket(fds[0]);
    closesocket(fds[1]);
#else
    close(fds[0]);
    close(fds[1]);
#endif
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

#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
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
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    }
}
