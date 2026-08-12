// Adapter from spaznet::udp::Handler (the handler-interface idiom)
// to the core ::spaznet::DatagramHandler callback.  Trivial — the
// core already parses peer addr/port and preserves the raw sockaddr
// in the Datagram.

#include <libspaznet/io_context.hpp>
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

auto make_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::DatagramHandler {
    // std::shared_ptr makes the std::function payload copyable.
    std::shared_ptr<Handler> shared(handler.release());
    return [shared](::spaznet::Datagram dg) -> ::spaznet::Task {
        // handle_packet is a plain synchronous call — no co_await — so
        // this Task never actually suspends; it exists purely to satisfy
        // DatagramHandler's signature for callers still on the coroutine
        // runtime. See dispatcher_reactor.cpp's make_reactor_dispatcher
        // for the coroutine-free equivalent.
        shared->handle_packet(to_packet(std::move(dg)));
        co_return;
    };
}

} // namespace spaznet::udp
