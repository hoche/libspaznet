#pragma once

// Shared helper for parameterizing QUIC/HTTP3 integration tests over both
// dispatchers: the coroutine one (service.cpp's make_dispatcher,
// Server::set_datagram_handler) and the reactor one (service.cpp's
// make_reactor_dispatcher, Server::set_sync_datagram_handler). See
// example/http/tests/integration/dispatcher_test_support.hpp for the
// pattern this mirrors.

#include <gtest/gtest.h>
#include <libspaznet/http3/service.hpp>
#include <libspaznet/server.hpp>

#include <memory>
#include <string>

namespace spaznet::http3::testing_support {

enum class DispatcherKind { Coroutine, Reactor };

inline void install_dispatcher(::spaznet::Server& server,
                               std::unique_ptr<QuicHttp3Service> service, DispatcherKind kind) {
    if (kind == DispatcherKind::Reactor) {
        server.set_sync_datagram_handler(make_reactor_dispatcher(std::move(service)));
    } else {
        server.set_datagram_handler(make_dispatcher(std::move(service)));
    }
}

inline auto DispatcherKindName(const ::testing::TestParamInfo<DispatcherKind>& info) -> std::string {
    return info.param == DispatcherKind::Reactor ? "Reactor" : "Coroutine";
}

} // namespace spaznet::http3::testing_support
