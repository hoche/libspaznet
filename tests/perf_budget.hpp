#pragma once

// Helpers for performance-test budgets. Native CI (x64/macOS) keeps the
// tight regression guards; under QEMU (Linux ARM64 workflow) the same
// microbenchmarks routinely run several times slower, so ceilings are
// loosened and floors are lowered.

#include <cstdlib>
#include <fstream>
#include <string>

namespace spaznet::test {

inline auto under_qemu() -> bool {
    // Explicit override — set by the Linux ARM64 workflow.
    if (const char* env = std::getenv("SPAZNET_UNDER_QEMU")) {
        if (env[0] == '1' || env[0] == 'y' || env[0] == 'Y' || env[0] == 't' ||
            env[0] == 'T') {
            return true;
        }
        if (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' ||
            env[0] == 'F') {
            return false;
        }
    }

#if defined(__linux__)
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        // User-mode and system QEMU both advertise themselves here.
        if (line.find("QEMU") != std::string::npos) {
            return true;
        }
    }
#endif
    return false;
}

// Multiplier applied to upper-bound budgets (EXPECT_LT / EXPECT_LE).
inline auto qemu_slowdown() -> double {
    return under_qemu() ? 10.0 : 1.0;
}

// Ceiling for "must finish faster than X" asserts.
inline auto perf_ceiling(double native_budget) -> double {
    return native_budget * qemu_slowdown();
}

// Floor for "must exceed X throughput" asserts.
inline auto perf_floor(double native_budget) -> double {
    return native_budget / qemu_slowdown();
}

} // namespace spaznet::test
