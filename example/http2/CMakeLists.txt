# example/http2 — HTTP/2 (h2c, prior-knowledge cleartext).
# Depends on the core libspaznet target for Server/Socket, plus on
# spaznet::http3 (transitively via the core static lib) for the
# RFC 7541 §B Huffman codec shared with QPACK.

add_library(spaznet_http2 STATIC
    src/codec.cpp
    src/dispatcher.cpp
)
add_library(spaznet::http2 ALIAS spaznet_http2)

target_compile_features(spaznet_http2 PUBLIC cxx_std_20)

target_include_directories(spaznet_http2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(spaznet_http2 PUBLIC spaznet)

if(BUILD_TESTING OR TARGET test_unit)
    set(SPAZNET_HTTP2_UNIT_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/unit/test_codec.cpp
        tests/unit/test_parser.cpp
    )
    add_executable(test_http2_unit ${SPAZNET_HTTP2_UNIT_SOURCES})
    target_link_libraries(test_http2_unit PRIVATE spaznet_http2 GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_http2_unit)
    add_test(NAME Http2UnitTests COMMAND test_http2_unit)

    set(SPAZNET_HTTP2_INTEGRATION_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/integration/test_rfc9113_compliance.cpp
    )
    add_executable(test_http2_integration ${SPAZNET_HTTP2_INTEGRATION_SOURCES})
    target_link_libraries(test_http2_integration PRIVATE spaznet_http2 GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_http2_integration)
    add_test(NAME Http2IntegrationTests COMMAND test_http2_integration)

    set(SPAZNET_HTTP2_PERFORMANCE_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/performance/test_rfc9113_performance.cpp
    )
    add_executable(test_http2_performance ${SPAZNET_HTTP2_PERFORMANCE_SOURCES})
    target_link_libraries(test_http2_performance PRIVATE spaznet_http2 GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_http2_performance)
    add_test(NAME Http2PerformanceTests COMMAND test_http2_performance)
endif()

add_executable(http2_hello demo/hello.cpp)
target_link_libraries(http2_hello PRIVATE spaznet_http2)
