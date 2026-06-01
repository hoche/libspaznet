#pragma once

// Adapt a spaznet::udp::Handler into the core
// ::spaznet::DatagramHandler callback that the Server expects.

#include <libspaznet/server.hpp>
#include <libspaznet/udp/handler.hpp>

#include <memory>

namespace spaznet::udp {

auto make_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::DatagramHandler;

} // namespace spaznet::udp
