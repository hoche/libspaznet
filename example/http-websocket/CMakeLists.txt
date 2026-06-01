# example/http-websocket — HTTP/1.1 + WebSocket combined dispatcher.
# Depends on example/http for the HTTP/1.1 serve loop used when an
# incoming request turns out not to be a WS upgrade.

add_library(spaznet_http_websocket STATIC
    src/handler.cpp
    src/dispatcher.cpp
)
add_library(spaznet::http_websocket ALIAS spaznet_http_websocket)

target_compile_features(spaznet_http_websocket PUBLIC cxx_std_20)

target_include_directories(spaznet_http_websocket
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(spaznet_http_websocket PUBLIC spaznet spaznet_http)

if(BUILD_TESTING OR TARGET test_unit)
    set(SPAZNET_WS_UNIT_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/unit/test_handler.cpp
    )
    add_executable(test_ws_unit ${SPAZNET_WS_UNIT_SOURCES})
    target_link_libraries(test_ws_unit PRIVATE spaznet_http_websocket GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_ws_unit)
    add_test(NAME WebSocketUnitTests COMMAND test_ws_unit)

    set(SPAZNET_WS_INTEGRATION_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/integration/test_websocket_server.cpp
    )
    add_executable(test_ws_integration ${SPAZNET_WS_INTEGRATION_SOURCES})
    target_link_libraries(test_ws_integration PRIVATE spaznet_http_websocket GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_ws_integration)
    add_test(NAME WebSocketIntegrationTests COMMAND test_ws_integration)

    set(SPAZNET_WS_PERFORMANCE_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/performance/test_websocket_rfc6455_performance.cpp
    )
    add_executable(test_ws_performance ${SPAZNET_WS_PERFORMANCE_SOURCES})
    target_link_libraries(test_ws_performance PRIVATE spaznet_http_websocket GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_ws_performance)
    add_test(NAME WebSocketPerformanceTests COMMAND test_ws_performance)
endif()

add_executable(ws_echo demo/echo.cpp)
target_link_libraries(ws_echo PRIVATE spaznet_http_websocket)
