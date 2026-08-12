#pragma once

// Shared helper for parameterizing UDP integration tests over both
// dispatchers: the coroutine one (dispatcher.cpp, Server::set_datagram_handler)
// and the reactor one (dispatcher_reactor.cpp, Server::set_sync_datagram_handler).
// See example/http/tests/integration/dispatcher_test_support.hpp for the
// pattern this mirrors.

#include <gtest/gtest.h>
#include <libspaznet/server.hpp>
#include <libspaznet/udp/dispatcher.hpp>
#include <libspaznet/udp/handler.hpp>

#include <memory>
#include <string>

namespace spaznet::udp::testing_support {

enum class DispatcherKind { Coroutine, Reactor };

inline void install_dispatcher(::spaznet::Server& server, std::unique_ptr<Handler> handler,
                               DispatcherKind kind) {
    if (kind == DispatcherKind::Reactor) {
        server.set_sync_datagram_handler(make_reactor_dispatcher(std::move(handler)));
    } else {
        server.set_datagram_handler(make_dispatcher(std::move(handler)));
    }
}

inline auto DispatcherKindName(const ::testing::TestParamInfo<DispatcherKind>& info) -> std::string {
    return info.param == DispatcherKind::Reactor ? "Reactor" : "Coroutine";
}

} // namespace spaznet::udp::testing_support
