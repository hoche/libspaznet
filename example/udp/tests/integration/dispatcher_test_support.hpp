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
#include <vector>

namespace spaznet::udp::testing_support {

enum class DispatcherKind { Coroutine, Reactor };

// The Coroutine arm only exists when SPAZNET_HAS_COROUTINES is defined —
// AllDispatcherKinds() below never hands a test Coroutine otherwise.
inline void install_dispatcher(::spaznet::Server& server, std::unique_ptr<Handler> handler,
                               DispatcherKind kind) {
    if (kind == DispatcherKind::Reactor) {
        server.set_sync_datagram_handler(make_reactor_dispatcher(std::move(handler)));
        return;
    }
#ifdef SPAZNET_HAS_COROUTINES
    server.set_datagram_handler(make_dispatcher(std::move(handler)));
#else
    (void)server;
    (void)handler;
#endif
}

inline auto DispatcherKindName(const ::testing::TestParamInfo<DispatcherKind>& info) -> std::string {
    return info.param == DispatcherKind::Reactor ? "Reactor" : "Coroutine";
}

// Every DispatcherKind this build actually supports — see
// example/http/tests/integration/dispatcher_test_support.hpp for the full
// rationale.
inline auto AllDispatcherKinds() -> std::vector<DispatcherKind> {
#ifdef SPAZNET_HAS_COROUTINES
    return {DispatcherKind::Coroutine, DispatcherKind::Reactor};
#else
    return {DispatcherKind::Reactor};
#endif
}

} // namespace spaznet::udp::testing_support
