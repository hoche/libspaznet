# example/quic-http3 — full QUIC v1 + HTTP/3 + QPACK stack.
# Requires OpenSSL 3.5+ (for SSL_set_quic_tls_cbs).  The top-level
# CMakeLists.txt guards `add_subdirectory(example/quic-http3)` on
# SPAZNET_BUILD_QUIC + the OpenSSL probe, so by the time we get here
# both are satisfied.

add_library(spaznet_quic_http3 STATIC
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
    src/http3/qpack.cpp
    src/http3/h3frame.cpp
    src/http3/server.cpp
    src/http3/service.cpp
)
add_library(spaznet::quic_http3 ALIAS spaznet_quic_http3)

target_compile_features(spaznet_quic_http3 PUBLIC cxx_std_20)

target_include_directories(spaznet_quic_http3
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(spaznet_quic_http3 PUBLIC spaznet OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(spaznet_quic_http3 PUBLIC SPAZNET_HAS_QUIC)

if(BUILD_TESTING OR TARGET test_unit)
    set(SPAZNET_QH3_UNIT_SOURCES
        ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
        tests/unit/test_quic_phase1.cpp
        tests/unit/test_quic_frame.cpp
        tests/unit/test_quic_tls.cpp
        tests/unit/test_quic_phase4.cpp
        tests/unit/test_quic_connection.cpp
        tests/unit/test_quic_listener.cpp
        tests/unit/test_http3_huffman.cpp
        tests/unit/test_http3_qpack.cpp
        tests/unit/test_http3_frame.cpp
        tests/unit/test_http3_end_to_end.cpp
    )
    add_executable(test_quic_http3_unit ${SPAZNET_QH3_UNIT_SOURCES})
    target_link_libraries(test_quic_http3_unit
        PRIVATE spaznet_quic_http3 GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_quic_http3_unit)
    add_test(NAME QuicHttp3UnitTests COMMAND test_quic_http3_unit)
endif()

add_executable(bench_quic_steady_state tests/performance/bench_quic_steady_state.cpp)
target_link_libraries(bench_quic_steady_state PRIVATE spaznet_quic_http3)
spaznet_apply_test_warnings(bench_quic_steady_state)
