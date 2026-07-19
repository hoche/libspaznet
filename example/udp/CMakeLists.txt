# example/udp — handler-interface idiom over the core
# Server::set_datagram_handler callback.  Tiny.

add_library(spaznet_udp STATIC
    src/dispatcher.cpp
)
add_library(spaznet::udp ALIAS spaznet_udp)

target_compile_features(spaznet_udp PUBLIC cxx_std_20)
target_include_directories(spaznet_udp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(spaznet_udp PUBLIC spaznet)

if(BUILD_TESTING OR TARGET test_unit)
    set(SPAZNET_UDP_INTEGRATION_SOURCES
        ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
        tests/integration/test_udp_server.cpp
    )
    add_executable(test_udp_integration ${SPAZNET_UDP_INTEGRATION_SOURCES})
    target_link_libraries(test_udp_integration PRIVATE spaznet_udp GTest::gtest GTest::gmock)
    spaznet_apply_test_warnings(test_udp_integration)
    add_test(NAME UdpIntegrationTests COMMAND test_udp_integration)
endif()

add_executable(udp_echo demo/echo.cpp)
target_link_libraries(udp_echo PRIVATE spaznet_udp)

add_executable(udp_relay demo/relay.cpp)
target_link_libraries(udp_relay PRIVATE spaznet_udp)
