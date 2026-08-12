// Coroutine-free counterpart of dispatcher.cpp: adapts the exact same
// spaznet::udp::Handler into ::spaznet::SyncDatagramHandler instead of
// ::spaznet::DatagramHandler. Handler::handle_packet is already a plain
// synchronous function (see handler.hpp) with no completion token to
// bridge — unlike HTTP/1.1's ResponseWriter dance, there is nothing here
// for a "reactor" adapter to do beyond calling it directly.

#include <libspaznet/server.hpp>
#include <libspaznet/udp/dispatcher.hpp>
#include <libspaznet/udp/handler.hpp>

#include <memory>
#include <utility>

namespace spaznet::udp {

namespace {

auto to_packet(::spaznet::Datagram dg) -> Packet {
    Packet pkt;
    pkt.data = std::move(dg.data);
    pkt.address = std::move(dg.peer_addr);
    pkt.port = dg.peer_port;
    pkt.listen_fd = dg.fd;
    pkt.peer = dg.peer;
    pkt.peer_len = dg.peer_len;
    return pkt;
}

} // namespace

auto make_reactor_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::SyncDatagramHandler {
    std::shared_ptr<Handler> shared(handler.release());
    return [shared](::spaznet::Datagram dg) { shared->handle_packet(to_packet(std::move(dg))); };
}

} // namespace spaznet::udp
