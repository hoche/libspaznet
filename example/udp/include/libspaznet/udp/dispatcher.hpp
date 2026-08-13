#pragma once

// Adapt a spaznet::udp::Handler into either of the core Server
// callbacks. Handler::handle_packet is a plain synchronous function, so
// both adapters are thin: the coroutine one just wraps the call in a
// Task that never suspends, and the reactor one calls it directly.

#include <libspaznet/server.hpp>
#include <libspaznet/udp/handler.hpp>

#include <memory>

namespace spaznet::udp {

#ifdef SPAZNET_HAS_COROUTINES
// Coroutine runtime: install via Server::set_datagram_handler.
auto make_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::DatagramHandler;
#endif // SPAZNET_HAS_COROUTINES

// Coroutine-free reactor runtime: install via
// Server::set_sync_datagram_handler. See dispatcher.hpp's HTTP/1.1
// counterpart (example/http) for the pattern this follows; UDP's is
// simpler since Handler has no completion token to bridge.
auto make_reactor_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::SyncDatagramHandler;

} // namespace spaznet::udp
