# example/http — HTTP/1.1 only.  Built on top of the core libspaznet
# target via Server::set_connection_handler.  See docs/http.md.

add_library(spaznet_http STATIC
    src/handler.cpp
    src/dispatcher.cpp
)
add_library(spaznet::http ALIAS spaznet_http)

target_compile_features(spaznet_http PUBLIC cxx_std_20)

target_include_directories(spaznet_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(spaznet_http PUBLIC spaznet)

# Tests
if(BUILD_TESTING OR TARGET test_unit)
    set(SPAZNET_HTTP_UNIT_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/unit/test_handler.cpp
        tests/unit/test_parser.cpp
    )
    add_executable(test_http_unit ${SPAZNET_HTTP_UNIT_SOURCES})
    target_link_libraries(test_http_unit PRIVATE spaznet_http GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_http_unit)
    add_test(NAME HttpUnitTests COMMAND test_http_unit)

    set(SPAZNET_HTTP_INTEGRATION_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/integration/test_tcp_server.cpp
        tests/integration/test_http_server.cpp
        tests/integration/test_thread_modes.cpp
        tests/integration/test_concurrent_connections.cpp
        tests/integration/test_rfc9112_compliance.cpp
    )
    add_executable(test_http_integration ${SPAZNET_HTTP_INTEGRATION_SOURCES})
    target_link_libraries(test_http_integration PRIVATE spaznet_http GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_http_integration)
    add_test(NAME HttpIntegrationTests COMMAND test_http_integration)

    set(SPAZNET_HTTP_PERFORMANCE_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/performance/test_throughput.cpp
        tests/performance/test_latency.cpp
        tests/performance/test_concurrent_performance.cpp
        tests/performance/test_rfc9112_performance.cpp
    )
    add_executable(test_http_performance ${SPAZNET_HTTP_PERFORMANCE_SOURCES})
    target_link_libraries(test_http_performance PRIVATE spaznet_http GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_http_performance)
    add_test(NAME HttpPerformanceTests COMMAND test_http_performance)

    add_executable(bench_thread_modes tests/performance/bench_thread_modes.cpp)
    target_link_libraries(bench_thread_modes PRIVATE spaznet_http)
    spaznet_apply_test_warnings(bench_thread_modes)
endif()

# Demo
add_executable(http_hello demo/hello.cpp)
target_link_libraries(http_hello PRIVATE spaznet_http)

# Fuzz harnesses (only built under SPAZNET_BUILD_FUZZ).
spaznet_add_fuzzer(fuzz_http_request   fuzz/fuzz_http_request.cpp)
spaznet_add_fuzzer(fuzz_chunked_body   fuzz/fuzz_chunked_body.cpp)
if(SPAZNET_BUILD_FUZZ)
    target_link_libraries(fuzz_http_request PRIVATE spaznet_http)
    target_link_libraries(fuzz_chunked_body PRIVATE spaznet_http)
endif()
