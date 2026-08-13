#pragma once

// Shared helper for parameterizing WebSocket integration tests over both
// dispatchers. Unlike example/http, example/udp, and example/quic-http3
// (where the exact same Handler subclass instance works for both
// dispatchers), the coroutine and reactor runtimes here have genuinely
// different Handler interfaces -- Task-returning vs. synchronous, see
// dispatcher.hpp's and reactor_handler.hpp's respective comments -- so
// there's no single install_dispatcher() helper to share the way the
// other protocols do. This header just carries the common DispatcherKind
// enum and name generator; each test file constructs the
// dispatcher-appropriate handler pair itself.

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace spaznet::websocket::testing_support {

enum class DispatcherKind { Coroutine, Reactor };

inline auto DispatcherKindName(const ::testing::TestParamInfo<DispatcherKind>& info) -> std::string {
    return info.param == DispatcherKind::Reactor ? "Reactor" : "Coroutine";
}

// The coroutine dispatcher only exists when this binary was built with
// SPAZNET_ENABLE_COROUTINES; instantiate test suites from this instead
// of a hardcoded {Coroutine, Reactor} list so they still build and run
// (Reactor-only) when it's off.
inline auto AllDispatcherKinds() -> std::vector<DispatcherKind> {
#ifdef SPAZNET_HAS_COROUTINES
    return {DispatcherKind::Coroutine, DispatcherKind::Reactor};
#else
    return {DispatcherKind::Reactor};
#endif
}

} // namespace spaznet::websocket::testing_support
