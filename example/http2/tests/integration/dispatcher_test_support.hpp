#pragma once

// Shared helper for parameterizing HTTP/2 integration tests over both
// dispatchers: the coroutine one (dispatcher.cpp, Server::set_connection_handler)
// and the reactor one (dispatcher_reactor.cpp, Server::set_connection_factory).
// Both speak the exact same protocol against the exact same http2::Handler
// interface, so running the same test body against each is a differential
// check — any behavioral divergence is a bug in whichever one disagrees.
//
// Usage: `TEST_P` fixtures that install a handler via install_dispatcher()
// and read `GetParam()` where a DispatcherKind is needed, plus
// `INSTANTIATE_TEST_SUITE_P(Dispatchers, YourFixture,
// ::testing::Values(DispatcherKind::Coroutine, DispatcherKind::Reactor),
// DispatcherKindName)` — see test_rfc9113_compliance.cpp for worked
// examples.

#include <gtest/gtest.h>
#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>

#include <memory>
#include <string>
#include <vector>

namespace spaznet::http2::testing_support {

enum class DispatcherKind { Coroutine, Reactor };

// Installs `handler` on `server` using whichever of
// set_connection_handler/set_connection_factory matches `kind`. The
// Coroutine arm only exists when SPAZNET_HAS_COROUTINES is defined —
// AllDispatcherKinds() below never hands a test Coroutine otherwise, so
// this is unreachable rather than a runtime fallback.
inline void install_dispatcher(::spaznet::Server& server, std::unique_ptr<Handler> handler,
                               DispatcherKind kind) {
    if (kind == DispatcherKind::Reactor) {
        server.set_connection_factory(make_reactor_dispatcher(std::move(handler)));
        return;
    }
#ifdef SPAZNET_HAS_COROUTINES
    server.set_connection_handler(make_dispatcher(std::move(handler)));
#else
    (void)server;
    (void)handler;
#endif
}

// Name generator for INSTANTIATE_TEST_SUITE_P's optional 4th argument, so
// instances show up as e.g. "Dispatchers/RFC9113IntegrationTest.HeadersFrame/Reactor"
// instead of a numeric suffix.
inline auto DispatcherKindName(const ::testing::TestParamInfo<DispatcherKind>& info) -> std::string {
    return info.param == DispatcherKind::Reactor ? "Reactor" : "Coroutine";
}

// Every DispatcherKind this build actually supports — see http's
// dispatcher_test_support.hpp for the full rationale.
inline auto AllDispatcherKinds() -> std::vector<DispatcherKind> {
#ifdef SPAZNET_HAS_COROUTINES
    return {DispatcherKind::Coroutine, DispatcherKind::Reactor};
#else
    return {DispatcherKind::Reactor};
#endif
}

} // namespace spaznet::http2::testing_support
