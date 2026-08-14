#pragma once

// Shared helper for parameterizing QUIC/HTTP3 integration tests over both
// dispatchers: the coroutine one (service.cpp's make_coroutine_dispatcher,
// Server::set_coroutine_datagram_handler) and the reactor one (service.cpp's
// make_reactor_dispatcher, Server::set_reactor_sync_datagram_handler). See
// example/http/tests/integration/dispatcher_test_support.hpp for the
// pattern this mirrors.

#include <gtest/gtest.h>
#include <libspaznet/http3/service.hpp>
#include <libspaznet/server.hpp>

#include <memory>
#include <string>
#include <vector>

namespace spaznet::http3::testing_support {

enum class DispatcherKind { Coroutine, Reactor };

// The Coroutine arm only exists when SPAZNET_HAS_COROUTINES is defined —
// AllDispatcherKinds() below never hands a test Coroutine otherwise.
inline void install_dispatcher(::spaznet::Server& server,
                               std::unique_ptr<QuicHttp3Service> service, DispatcherKind kind) {
    if (kind == DispatcherKind::Reactor) {
        server.set_reactor_sync_datagram_handler(make_reactor_dispatcher(std::move(service)));
        return;
    }
#ifdef SPAZNET_HAS_COROUTINES
    server.set_coroutine_datagram_handler(make_coroutine_dispatcher(std::move(service)));
#else
    (void)server;
    (void)service;
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

} // namespace spaznet::http3::testing_support
