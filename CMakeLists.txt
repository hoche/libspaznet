cmake_minimum_required(VERSION 3.20)
project(libspaznet VERSION 1.0.0 LANGUAGES CXX)

# ---- Language settings --------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Threads: pull pthread (or the platform equivalent) in via the modern
# imported target so we don't rely on the compiler implicitly linking
# libpthread.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# OpenSSL 3.5+ provides the SSL_set_quic_tls_cbs interface that the
# QUIC stack uses to drive the TLS 1.3 handshake (RFC 9001). Everything
# above the TLS layer (transport, recovery, HTTP/3, QPACK) is implemented
# in this repo with no other third-party dependencies.
find_package(OpenSSL 3.5 REQUIRED)

# CMake helpers for install(EXPORT) below.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- Platform demultiplexer selection -----------------------------------
# The IOCP backend is incomplete (see src/platform/platform_io_iocp.cpp);
# it is opt-in via SPAZNET_ENABLE_BROKEN_IOCP. Default Windows builds
# fall through to the portable poll backend so the library is usable
# out of the box.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(USE_EPOLL ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(USE_KQUEUE ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND SPAZNET_ENABLE_BROKEN_IOCP)
    set(USE_IOCP ON)
else()
    set(USE_POLL ON)
endif()

# ---- Source / header lists ----------------------------------------------
set(SOURCES
    src/platform/io_context.cpp
    src/platform/platform_io.cpp
    src/handlers/udp_handler.cpp
    src/handlers/http_handler.cpp
    src/handlers/http2_handler.cpp
    src/handlers/websocket_handler.cpp
    src/handlers/quic_handler.cpp
    src/handlers/quic_server.cpp
    src/handlers/http3_handler.cpp
    src/quic/varint.cpp
    src/quic/crypto.cpp
    src/quic/packet.cpp
    src/quic/frame.cpp
    src/quic/transport_params.cpp
    src/quic/tls.cpp
    src/quic/pn_space.cpp
    src/quic/stream.cpp
    src/quic/recovery.cpp
    src/quic/congestion.cpp
    src/quic/connection.cpp
    src/quic/listener.cpp
    src/http3/huffman.cpp
    src/http3/qpack.cpp
    src/http3/h3frame.cpp
    src/utils/string_utils.cpp
    src/utils/number_utils.cpp
    src/utils/binary_utils.cpp
    src/utils/header_utils.cpp
    src/server_impl.cpp
)

set(HEADERS
    include/libspaznet/io_context.hpp
    include/libspaznet/platform_io.hpp
    include/libspaznet/platform/io_context.hpp
    include/libspaznet/platform/platform_io.hpp
    include/libspaznet/logger.hpp
    include/libspaznet/handlers/udp_handler.hpp
    include/libspaznet/handlers/http_handler.hpp
    include/libspaznet/handlers/http2_handler.hpp
    include/libspaznet/handlers/websocket_handler.hpp
    include/libspaznet/handlers/quic_handler.hpp
    include/libspaznet/handlers/quic_server.hpp
    include/libspaznet/handlers/http3_handler.hpp
    include/libspaznet/quic/varint.hpp
    include/libspaznet/quic/crypto.hpp
    include/libspaznet/quic/packet.hpp
    include/libspaznet/quic/frame.hpp
    include/libspaznet/quic/transport_params.hpp
    include/libspaznet/quic/tls.hpp
    include/libspaznet/quic/pn_space.hpp
    include/libspaznet/quic/stream.hpp
    include/libspaznet/quic/recovery.hpp
    include/libspaznet/quic/congestion.hpp
    include/libspaznet/quic/connection.hpp
    include/libspaznet/quic/listener.hpp
    include/libspaznet/http3/huffman.hpp
    include/libspaznet/http3/qpack.hpp
    include/libspaznet/http3/h3frame.hpp
    include/libspaznet/utils/string_utils.hpp
    include/libspaznet/utils/number_utils.hpp
    include/libspaznet/utils/binary_utils.hpp
    include/libspaznet/utils/header_utils.hpp
    include/libspaznet/server.hpp
)

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

target_link_libraries(spaznet PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

# Platform-specific compile defs.
if(USE_EPOLL)
    target_compile_definitions(spaznet PRIVATE USE_EPOLL)
elseif(USE_KQUEUE)
    target_compile_definitions(spaznet PRIVATE USE_KQUEUE)
elseif(USE_IOCP)
    target_compile_definitions(spaznet PRIVATE USE_IOCP SPAZNET_ENABLE_BROKEN_IOCP)
else()
    target_compile_definitions(spaznet PRIVATE USE_POLL)
endif()

# Winsock + IOCP support libraries.
if(WIN32)
    target_link_libraries(spaznet PUBLIC ws2_32 mswsock)
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
endfunction()

spaznet_apply_warnings(spaznet)

# ---- Example ------------------------------------------------------------
add_executable(example example/server.cpp)
target_link_libraries(example PRIVATE spaznet)
spaznet_apply_warnings(example)

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

# Generate a config file that pulls in spaznetTargets.cmake and re-runs
# find_package(Threads) (so downstream sees the same pthread linkage).
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/spaznetConfig.cmake.in"
"@PACKAGE_INIT@

include(CMakeFindDependencyMacro)
find_dependency(Threads)
find_dependency(OpenSSL 3.5)

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

add_executable(test_unit
    tests/test_main.cpp
    tests/unit/test_task_queue.cpp
    tests/unit/test_platform_io.cpp
    tests/unit/test_io_context.cpp
    tests/unit/test_timer.cpp
    tests/unit/test_http_handler.cpp
    tests/unit/test_websocket_handler.cpp
    tests/unit/test_http2_handler.cpp
    tests/unit/test_quic_handler.cpp
    tests/unit/test_quic_phase1.cpp
    tests/unit/test_quic_frame.cpp
    tests/unit/test_quic_tls.cpp
    tests/unit/test_quic_phase4.cpp
    tests/unit/test_quic_connection.cpp
    tests/unit/test_quic_listener.cpp
    tests/unit/test_http3_huffman.cpp
    tests/unit/test_http3_qpack.cpp
    tests/unit/test_http3_frame.cpp
    tests/unit/test_rfc9112_parser.cpp
    tests/unit/test_rfc9113_parser.cpp
    tests/unit/test_logger.cpp
)
target_link_libraries(test_unit PRIVATE spaznet GTest::gtest GTest::gmock)
spaznet_apply_test_warnings(test_unit)

add_executable(test_integration
    tests/test_main.cpp
    tests/integration/test_tcp_server.cpp
    tests/integration/test_http_server.cpp
    tests/integration/test_thread_modes.cpp
    tests/integration/test_websocket_server.cpp
    tests/integration/test_udp_server.cpp
    tests/integration/test_concurrent_connections.cpp
    tests/integration/test_rfc9112_compliance.cpp
    tests/integration/test_rfc9113_compliance.cpp
    tests/integration/test_quic_server.cpp
    tests/integration/test_http3_server.cpp
)
target_link_libraries(test_integration PRIVATE spaznet GTest::gtest GTest::gmock)
spaznet_apply_test_warnings(test_integration)

add_executable(test_performance
    tests/test_main.cpp
    tests/performance/test_throughput.cpp
    tests/performance/test_latency.cpp
    tests/performance/test_concurrent_performance.cpp
    tests/performance/test_iperf_integration.cpp
    tests/performance/test_rfc9112_performance.cpp
    tests/performance/test_rfc9113_performance.cpp
    tests/performance/test_websocket_rfc6455_performance.cpp
    tests/performance/test_quic_performance.cpp
)
target_link_libraries(test_performance PRIVATE spaznet GTest::gtest GTest::gmock)
spaznet_apply_test_warnings(test_performance)

add_executable(bench_thread_modes
    tests/performance/bench_thread_modes.cpp
)
target_link_libraries(bench_thread_modes PRIVATE spaznet)
spaznet_apply_test_warnings(bench_thread_modes)

add_test(NAME UnitTests COMMAND test_unit)
add_test(NAME IntegrationTests COMMAND test_integration)
add_test(NAME PerformanceTests COMMAND test_performance)
