#include <gtest/gtest.h>

#include <libspaznet/detail/socket_compat.hpp>

#ifndef _WIN32
#include <csignal>
#endif

auto main(int argc, char** argv) -> int {
#ifndef _WIN32
    // Under ctest, stdout/stderr are pipes; some network writes can still trigger SIGPIPE
    // (or libraries may use plain write()). Ignore SIGPIPE so tests fail via return codes
    // instead of aborting the whole process.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // PlatformIO / raw socket helpers in unit tests create sockets before any
    // Server/IOContext exists; bootstrap Winsock here so those paths work.
    spaznet::detail::ensure_winsock();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
