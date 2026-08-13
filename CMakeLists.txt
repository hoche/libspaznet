cmake_minimum_required(VERSION 3.20)
project(libspaznet VERSION 1.0.0 LANGUAGES CXX)

# ---- Language settings --------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libstdc++ only got <format> in gcc 13.1; libc++ has it from clang 17.
# Probe explicitly so the build fails at configure time with an actionable
# error rather than deep in compilation of (e.g.) src/handlers/http_handler.cpp.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
check_cxx_source_compiles("
    #include <format>
    int main() { (void)std::format(\"{}\", 1); return 0; }
" SPAZNET_HAS_STD_FORMAT)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT SPAZNET_HAS_STD_FORMAT)
    message(FATAL_ERROR
        "libspaznet requires C++20 <format> support.\n"
        "  - gcc:   need 13.1 or newer (your toolchain: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})\n"
        "  - clang: need 17 or newer (libc++) or libstdc++ from gcc 13+\n"
        "On Debian/Ubuntu: 'apt install g++-13' and re-configure with\n"
        "  -DCMAKE_CXX_COMPILER=g++-13"
    )
endif()

# Emit compile_commands.json into the build directory; clangd (and other
# tooling) reads it to learn each translation unit's include paths,
# language standard, and definitions. Without it, the language server
# defaults to C++17 and misses our headers + system OpenSSL paths,
# producing spurious "file not found" / "no template named span"
# diagnostics that don't reflect what the real build sees.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Threads: pull pthread (or the platform equivalent) in via the modern
# imported target so we don't rely on the compiler implicitly linking
# libpthread.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# The QUIC v1 + HTTP/3 stack is an example (example/quic-http3), not
# part of the core library.  TLS for that stack is exclusive: OpenSSL
# 3.5+ (default, SSL_set_quic_tls_cbs) or wolfSSL built with
# --enable-quic (SPAZNET_USE_WOLFSSL=ON).  Core spaznet never links a
# TLS library; only spaznet_quic_http3 does.
option(SPAZNET_BUILD_QUIC "Build example/quic-http3 (requires OpenSSL 3.5+ or wolfSSL+QUIC)" ON)
option(SPAZNET_USE_WOLFSSL "Use wolfSSL instead of OpenSSL for QUIC TLS" OFF)
set(SPAZNET_TLS_BACKEND "")
if(SPAZNET_BUILD_QUIC)
    if(SPAZNET_USE_WOLFSSL)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            if(DEFINED WOLFSSL_ROOT)
                set(ENV{PKG_CONFIG_PATH}
                    "${WOLFSSL_ROOT}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
            endif()
            pkg_check_modules(WOLFSSL QUIET wolfssl)
        endif()
        if(NOT WOLFSSL_FOUND AND DEFINED WOLFSSL_ROOT)
            if(EXISTS "${WOLFSSL_ROOT}/include/wolfssl/ssl.h")
                set(WOLFSSL_FOUND TRUE)
                set(WOLFSSL_INCLUDE_DIRS "${WOLFSSL_ROOT}/include")
                set(WOLFSSL_LIBRARY_DIRS "${WOLFSSL_ROOT}/lib")
                set(WOLFSSL_LIBRARIES wolfssl)
            endif()
        endif()
        if(WOLFSSL_FOUND)
            include(CheckCXXSourceCompiles)
            set(_spaznet_wolf_cflags "")
            foreach(_inc IN LISTS WOLFSSL_INCLUDE_DIRS)
                string(APPEND _spaznet_wolf_cflags " -I${_inc}")
            endforeach()
            foreach(_libdir IN LISTS WOLFSSL_LIBRARY_DIRS)
                string(APPEND _spaznet_wolf_cflags " -L${_libdir}")
            endforeach()
            set(CMAKE_REQUIRED_FLAGS "${_spaznet_wolf_cflags}")
            set(CMAKE_REQUIRED_LIBRARIES ${WOLFSSL_LIBRARIES})
            # options.h must precede other wolfSSL headers for a
            # prefix-built install.
            check_cxx_source_compiles("
                #include <wolfssl/options.h>
                #include <wolfssl/ssl.h>
                #include <wolfssl/quic.h>
                int main() {
                    (void)&wolfSSL_set_quic_method;
                    return 0;
                }
            " SPAZNET_WOLFSSL_HAS_QUIC)
            unset(CMAKE_REQUIRED_FLAGS)
            unset(CMAKE_REQUIRED_LIBRARIES)
            unset(_spaznet_wolf_cflags)
        endif()
        if(NOT WOLFSSL_FOUND OR NOT SPAZNET_WOLFSSL_HAS_QUIC)
            message(WARNING
                "wolfSSL with QUIC support not found; disabling example/quic-http3. "
                "Build wolfSSL with --enable-quic --enable-opensslextra and pass "
                "-DWOLFSSL_ROOT=<prefix>, or set SPAZNET_USE_WOLFSSL=OFF to use OpenSSL 3.5+.")
            set(SPAZNET_BUILD_QUIC OFF CACHE BOOL "" FORCE)
        else()
            set(SPAZNET_TLS_BACKEND "wolfssl")
            add_library(spaznet_tls INTERFACE)
            target_include_directories(spaznet_tls INTERFACE ${WOLFSSL_INCLUDE_DIRS})
            target_link_directories(spaznet_tls INTERFACE ${WOLFSSL_LIBRARY_DIRS})
            target_link_libraries(spaznet_tls INTERFACE ${WOLFSSL_LIBRARIES})
            target_compile_definitions(spaznet_tls INTERFACE SPAZNET_TLS_WOLFSSL)
            add_library(spaznet::tls ALIAS spaznet_tls)
        endif()
    else()
        find_package(OpenSSL 3.5 QUIET)
        if(NOT OpenSSL_FOUND)
            message(WARNING
                "OpenSSL 3.5+ not found; disabling example/quic-http3 build. "
                "Pass -DSPAZNET_BUILD_QUIC=OFF to silence this warning, "
                "install OpenSSL 3.5+ and set OPENSSL_ROOT_DIR, "
                "or use -DSPAZNET_USE_WOLFSSL=ON with a QUIC-enabled wolfSSL.")
            set(SPAZNET_BUILD_QUIC OFF CACHE BOOL "" FORCE)
        else()
            set(SPAZNET_TLS_BACKEND "openssl")
            add_library(spaznet_tls INTERFACE)
            target_link_libraries(spaznet_tls INTERFACE OpenSSL::SSL OpenSSL::Crypto)
            target_compile_definitions(spaznet_tls INTERFACE SPAZNET_TLS_OPENSSL)
            add_library(spaznet::tls ALIAS spaznet_tls)
        endif()
    endif()
endif()

# ---- TCP TLS (HTTPS for HTTP/1.1 and HTTP/2) ------------------------------
# Independent of QUIC's OpenSSL 3.5+ / wolfSSL requirement. Uses plain
# SSL_read/SSL_write on accepted TCP fds via Server::listen_tls.
option(SPAZNET_ENABLE_TLS
    "TLS-over-TCP for HTTP/HTTP2 (requires OpenSSL 1.1.1+)" ON)
if(SPAZNET_ENABLE_TLS)
    find_package(OpenSSL 1.1.1 QUIET)
    if(NOT OpenSSL_FOUND)
        message(WARNING
            "OpenSSL 1.1.1+ not found; disabling SPAZNET_ENABLE_TLS. "
            "Pass -DSPAZNET_ENABLE_TLS=OFF to silence this warning, "
            "or install OpenSSL and set OPENSSL_ROOT_DIR.")
        set(SPAZNET_ENABLE_TLS OFF CACHE BOOL "" FORCE)
    endif()
endif()

# ---- Coroutine runtime toggle --------------------------------------------
# The reactor (IoHandler, the fd table, post(), timers) is always built and
# has no coroutine dependency. Task/ValueTask, CoroutineHandle, and
# IOContext::schedule()/register_io(CoroutineHandle) are gated behind
# SPAZNET_HAS_COROUTINES (set below from this option) so a toolchain that
# can't or doesn't want a C++20 coroutine dependency can build just the
# reactor kernel. Defaults ON: every current consumer (Server, all five
# protocol dispatchers) is still coroutine-only until they grow reactor
# counterparts, so turning this OFF today only builds the core reactor
# primitives — not Server or example/. See docs/concurrency-and-coroutines.md.
option(SPAZNET_ENABLE_COROUTINES
    "Build the C++20 coroutine runtime (Task/ValueTask) on top of the reactor core" ON)

# ---- Fuzzing ------------------------------------------------------------
# Optional libFuzzer harnesses for every untrusted-byte parser in the
# tree.  Requires a Clang-family compiler (libFuzzer ships with clang).
# When enabled, applies -fsanitize=fuzzer-no-link + ASan + UBSan to
# every target globally so the fuzzer binaries link cleanly against
# the libraries.  Don't enable for release builds — sanitizers slow
# everything down by ~2x.
option(SPAZNET_BUILD_FUZZ "Build libFuzzer harnesses for parser surfaces (requires Clang)" OFF)
if(SPAZNET_BUILD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "SPAZNET_BUILD_FUZZ requires a Clang-family compiler "
            "(libFuzzer ships with clang).  Re-run with "
            "-DCMAKE_CXX_COMPILER=clang++.")
    endif()
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=fuzzer-no-link,address,undefined)
endif()

# Helper: turn `target` into a libFuzzer binary.  Links the fuzzer
# runtime and applies the standard test-warning relaxations so harness
# scaffolding doesn't trip -Wunused.
function(spaznet_add_fuzzer name source)
    if(NOT SPAZNET_BUILD_FUZZ)
        return()
    endif()
    add_executable(${name} ${source})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    spaznet_apply_test_warnings(${name})
endfunction()

# CMake helpers for install(EXPORT) below.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- Platform demultiplexer selection -----------------------------------
# Windows defaults to the readiness-style IOCP backend. Force the portable
# poll backend with -DSPAZNET_FORCE_POLL=ON if needed.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(USE_EPOLL ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(USE_KQUEUE ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT SPAZNET_FORCE_POLL)
    set(USE_IOCP ON)
else()
    set(USE_POLL ON)
endif()

# ---- Source / header lists ----------------------------------------------
# Core sources — always built. OpenSSL is linked only when SPAZNET_ENABLE_TLS.
set(SOURCES
    src/platform/io_context.cpp
    src/platform/platform_io.cpp
    src/reactor/buffered_connection.cpp
    src/codec/huffman.cpp
    src/utils/string_utils.cpp
    src/utils/number_utils.cpp
    src/utils/binary_utils.cpp
    src/utils/header_utils.cpp
)

set(HEADERS
    include/libspaznet/io_context.hpp
    include/libspaznet/platform_io.hpp
    include/libspaznet/platform/io_context.hpp
    include/libspaznet/platform/platform_io.hpp
    include/libspaznet/reactor/buffered_connection.hpp
    include/libspaznet/reactor/response_writer.hpp
    include/libspaznet/logger.hpp
    include/libspaznet/codec/huffman.hpp
    include/libspaznet/utils/string_utils.hpp
    include/libspaznet/utils/number_utils.hpp
    include/libspaznet/utils/binary_utils.hpp
    include/libspaznet/utils/header_utils.hpp
)

if(SPAZNET_ENABLE_TLS)
    list(APPEND SOURCES src/tls/tls_stream.cpp)
    list(APPEND HEADERS
        include/libspaznet/tls_config.hpp
        include/libspaznet/detail/tls_stream.hpp
    )
endif()

# Server (and its Socket wrapper) is always built: Socket and the
# coroutine-only accept/datagram-dispatch paths are #ifdef
# SPAZNET_HAS_COROUTINES inside server.hpp/server_impl.cpp, but
# listen_tcp()/listen_udp() themselves drive a reactor-native
# ListenHandler/DatagramReadHandler (IoHandler, no coroutine) either way,
# so Server::set_connection_factory/set_sync_datagram_handler work with
# SPAZNET_ENABLE_COROUTINES=OFF. See the reactor-port plan's
# coro-free-build milestone note.
list(APPEND SOURCES src/server_impl.cpp)
list(APPEND HEADERS include/libspaznet/server.hpp)

# ---- Library target -----------------------------------------------------
add_library(spaznet STATIC ${SOURCES} ${HEADERS})
add_library(spaznet::spaznet ALIAS spaznet)

target_compile_features(spaznet PUBLIC cxx_std_20)

# Target-scoped include directories. PUBLIC for the in-tree build; the
# $<INSTALL_INTERFACE:> path is used after install() so downstream
# find_package consumers see the installed layout instead of the source
# tree.
target_include_directories(spaznet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(spaznet PUBLIC Threads::Threads)
if(SPAZNET_ENABLE_COROUTINES)
    # PUBLIC: downstream headers (io_context.hpp) branch on this too, so
    # consumers linking spaznet::spaznet must see the same definition the
    # library itself was built with.
    target_compile_definitions(spaznet PUBLIC SPAZNET_HAS_COROUTINES)
endif()
if(SPAZNET_ENABLE_TLS)
    target_link_libraries(spaznet PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(spaznet PUBLIC SPAZNET_HAS_TLS)
endif()
# QUIC TLS deps live on spaznet_quic_http3 only (see example/quic-http3).

# Platform-specific compile defs.
if(USE_EPOLL)
    target_compile_definitions(spaznet PRIVATE USE_EPOLL)
elseif(USE_KQUEUE)
    target_compile_definitions(spaznet PRIVATE USE_KQUEUE)
elseif(USE_IOCP)
    target_compile_definitions(spaznet PRIVATE USE_IOCP)
else()
    target_compile_definitions(spaznet PRIVATE USE_POLL)
endif()

# Winsock + IOCP support libraries.
if(WIN32)
    target_link_libraries(spaznet PUBLIC ws2_32 mswsock)
    # NOMINMAX: winsock2/windows.h otherwise #define min/max and break
    # std::min / std::max. WIN32_LEAN_AND_MEAN keeps the include set small.
    target_compile_definitions(spaznet PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Warnings. Applied to library + tests + example via the helper below.
#
#  -Wall / -Wextra / -Wpedantic:  the baseline catch-alls.
#  -Wno-unused-parameter:         -Wextra enables -Wunused-parameter, which
#    fires on every handler override that ignores its `Socket&` argument
#    (a common, valid pattern). Suppress it globally rather than
#    annotating every method with [[maybe_unused]].
#  -Wconversion / -Wshadow:       intentionally NOT enabled yet — both
#    fire dozens of times across existing code; cleaning them up is a
#    separate, bounded project.
set(spaznet_warning_flags "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(spaznet_warning_flags -Wall -Wextra -Wpedantic -Wno-unused-parameter)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(spaznet_warning_flags /W4 /permissive-)
endif()

function(spaznet_apply_warnings target)
    target_compile_options(${target} PRIVATE ${spaznet_warning_flags})
endfunction()

# Tests and benchmarks accumulate scratch — helper functions used by
# only some of the tests in a TU, lambda captures retained for clarity,
# variables held for breakpoints. Don't promote those to errors; keep
# -Wall/-Wextra signal-worthy bits only.
set(spaznet_test_warning_flags ${spaznet_warning_flags})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    list(APPEND spaznet_test_warning_flags
        -Wno-unused-function
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-unused-lambda-capture)
endif()

function(spaznet_apply_test_warnings target)
    target_compile_options(${target} PRIVATE ${spaznet_test_warning_flags})
    # So performance suites can `#include "perf_budget.hpp"`.
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
endfunction()

spaznet_apply_warnings(spaznet)

# ---- Install + find_package config --------------------------------------
install(TARGETS spaznet
    EXPORT spaznetTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/libspaznet
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# Export the target set into <prefix>/lib/cmake/spaznet/ so downstream
# CMake projects can `find_package(spaznet)` and link `spaznet::spaznet`.
install(EXPORT spaznetTargets
    FILE spaznetTargets.cmake
    NAMESPACE spaznet::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spaznet
)

# Core package needs Threads, plus OpenSSL when TCP TLS was enabled.
# QUIC TLS (OpenSSL 3.5 / wolfSSL) is pulled in by spaznet::quic_http3.
if(SPAZNET_ENABLE_TLS)
    set(SPAZNET_FIND_DEPENDENCY_OPENSSL "find_dependency(OpenSSL 1.1.1)")
else()
    set(SPAZNET_FIND_DEPENDENCY_OPENSSL "")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/spaznetConfig.cmake.in"
"@PACKAGE_INIT@

include(CMakeFindDependencyMacro)
find_dependency(Threads)
@SPAZNET_FIND_DEPENDENCY_OPENSSL@

include(\"\${CMAKE_CURRENT_LIST_DIR}/spaznetTargets.cmake\")

check_required_components(spaznet)
")

configure_package_config_file(
    "${CMAKE_CURRENT_BINARY_DIR}/spaznetConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/spaznetConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spaznet
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/spaznetConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/spaznetConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/spaznetConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/spaznet
)

# ---- Tests --------------------------------------------------------------
enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Treat googletest headers as SYSTEM so its own warnings (e.g.
# -Wcharacter-conversion in gtest-printers.h) don't surface every
# time we rebuild.
foreach(gtest_target gtest gtest_main gmock gmock_main)
    if(TARGET ${gtest_target})
        get_target_property(_gtest_inc ${gtest_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_gtest_inc)
            set_target_properties(${gtest_target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_gtest_inc}")
        endif()
    endif()
endforeach()

set(SPAZNET_UNIT_TEST_SOURCES
    tests/test_main.cpp
    tests/unit/test_platform_io.cpp
    tests/unit/test_logger.cpp
    tests/unit/test_number_utils.cpp
    tests/unit/test_reactor_primitives.cpp
    tests/unit/test_buffered_connection.cpp
    tests/unit/test_response_writer.cpp
)
if(SPAZNET_ENABLE_COROUTINES)
    # All of these drive Task/ValueTask/CoroutineHandle, IOContext::schedule(),
    # or ctx.sleep_for()/interval() (the TimerAwaiter-based coroutine timer
    # API) directly; every one is compiled out when coroutines are disabled.
    list(APPEND SPAZNET_UNIT_TEST_SOURCES
        tests/unit/test_task_queue.cpp
        tests/unit/test_io_context.cpp
        tests/unit/test_timer.cpp
    )
endif()
add_executable(test_unit ${SPAZNET_UNIT_TEST_SOURCES})
target_link_libraries(test_unit PRIVATE spaznet GTest::gtest GTest::gmock)
spaznet_apply_test_warnings(test_unit)

# Most core integration tests have moved to example/<protocol>/tests
# alongside their handler implementations. test_server_reactor is the
# exception: it drives Server's reactor entry points (ConnectionFactory /
# SyncDatagramHandler / destroy-based shutdown) directly, with no protocol
# parsing involved, so it belongs here rather than under any one example.

add_test(NAME UnitTests COMMAND test_unit)

# Shells out to an external iperf3/iperf process; doesn't touch Server or
# any coroutine API, so it builds either way.
add_executable(test_performance
    tests/test_main.cpp
    tests/performance/test_iperf_integration.cpp
)
target_link_libraries(test_performance PRIVATE spaznet GTest::gtest GTest::gmock)
spaznet_apply_test_warnings(test_performance)
add_test(NAME PerformanceTests COMMAND test_performance)

# Server is always built now (see the SOURCES/HEADERS comment above), and
# this test only exercises its reactor entry points (ConnectionFactory /
# SyncDatagramHandler / destroy-based shutdown), so it builds either way too.
add_executable(test_server_reactor
    tests/test_main.cpp
    tests/integration/test_server_reactor.cpp
)
target_link_libraries(test_server_reactor PRIVATE spaznet GTest::gtest GTest::gmock)
spaznet_apply_test_warnings(test_server_reactor)
add_test(NAME ServerReactorTests COMMAND test_server_reactor)

# ---- example/ subprojects ----
# Every protocol has a reactor-side dispatcher now (make_reactor_dispatcher),
# so example/ builds under SPAZNET_ENABLE_COROUTINES=OFF too — each
# protocol's own CMakeLists.txt excludes its coroutine-only dispatcher.cpp
# (and coroutine-only tests/demo code paths) in that case. See the
# reactor-port plan's coro-free-build milestone note.
option(SPAZNET_BUILD_EXAMPLES "Build example protocol libraries (HTTP, etc.)" ON)
if(SPAZNET_BUILD_EXAMPLES)
    add_subdirectory(example/http)
    add_subdirectory(example/http-websocket)
    add_subdirectory(example/http2)
    add_subdirectory(example/udp)
    if(SPAZNET_BUILD_QUIC)
        add_subdirectory(example/quic-http3)
    endif()
endif()
