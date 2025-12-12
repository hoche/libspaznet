cmake_minimum_required(VERSION 3.20)
project(libspaznet VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(USE_EPOLL ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(USE_KQUEUE ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(USE_IOCP ON)
else()
    set(USE_POLL ON)
endif()

# Include directories
include_directories(include)

# Source files
set(SOURCES
    src/io_context.cpp
    src/platform_io.cpp
    src/handlers/udp_handler.cpp
    src/handlers/http_handler.cpp
    src/handlers/http2_handler.cpp
    src/handlers/websocket_handler.cpp
    src/utils/string_utils.cpp
    src/utils/number_utils.cpp
    src/utils/binary_utils.cpp
    src/utils/header_utils.cpp
    src/server.cpp
)

# Header files
set(HEADERS
    include/libspaznet/io_context.hpp
    include/libspaznet/platform_io.hpp
    include/libspaznet/handlers/udp_handler.hpp
    include/libspaznet/handlers/http_handler.hpp
    include/libspaznet/handlers/http2_handler.hpp
    include/libspaznet/handlers/websocket_handler.hpp
    include/libspaznet/utils/string_utils.hpp
    include/libspaznet/utils/number_utils.hpp
    include/libspaznet/utils/binary_utils.hpp
    include/libspaznet/utils/header_utils.hpp
    include/libspaznet/server.hpp
)

# Create library
add_library(spaznet STATIC ${SOURCES} ${HEADERS})

# Platform-specific sources
if(USE_EPOLL)
    target_sources(spaznet PRIVATE src/platform_io_epoll.cpp)
    target_compile_definitions(spaznet PRIVATE USE_EPOLL)
elseif(USE_KQUEUE)
    target_sources(spaznet PRIVATE src/platform_io_kqueue.cpp)
    target_compile_definitions(spaznet PRIVATE USE_KQUEUE)
elseif(USE_IOCP)
    target_sources(spaznet PRIVATE src/platform_io_iocp.cpp)
    target_compile_definitions(spaznet PRIVATE USE_IOCP)
else()
    target_sources(spaznet PRIVATE src/platform_io_poll.cpp)
    target_compile_definitions(spaznet PRIVATE USE_POLL)
endif()

# Windows-specific
if(WIN32)
    target_link_libraries(spaznet ws2_32 mswsock)
endif()

# Example executable
add_executable(example example/main.cpp)
target_link_libraries(example spaznet)

# Installation
install(TARGETS spaznet
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/libspaznet
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

# Testing
enable_testing()

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Unit tests
add_executable(test_unit
    tests/unit/test_task_queue.cpp
    tests/unit/test_platform_io.cpp
    tests/unit/test_io_context.cpp
    tests/unit/test_timer.cpp
    tests/unit/test_http_handler.cpp
    tests/unit/test_websocket_handler.cpp
    tests/unit/test_http2_handler.cpp
    tests/unit/test_rfc9112_parser.cpp
    tests/unit/test_rfc9113_parser.cpp
)

target_link_libraries(test_unit
    spaznet
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
)

# Integration tests
add_executable(test_integration
    tests/integration/test_tcp_server.cpp
    tests/integration/test_http_server.cpp
    tests/integration/test_websocket_server.cpp
    tests/integration/test_udp_server.cpp
    tests/integration/test_concurrent_connections.cpp
    tests/integration/test_rfc9112_compliance.cpp
    tests/integration/test_rfc9113_compliance.cpp
)

target_link_libraries(test_integration
    spaznet
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
)

# Performance tests
add_executable(test_performance
    tests/performance/test_throughput.cpp
    tests/performance/test_latency.cpp
    tests/performance/test_concurrent_performance.cpp
    tests/performance/test_iperf_integration.cpp
    tests/performance/test_rfc9112_performance.cpp
    tests/performance/test_rfc9113_performance.cpp
)

target_link_libraries(test_performance
    spaznet
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
)

# Add tests to CTest
add_test(NAME UnitTests COMMAND test_unit)
add_test(NAME IntegrationTests COMMAND test_integration)
add_test(NAME PerformanceTests COMMAND test_performance)

