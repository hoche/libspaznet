#include <gtest/gtest.h>

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

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
