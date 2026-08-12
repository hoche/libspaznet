#pragma once

#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <string>
#include <libspaznet/detail/socket_compat.hpp>
#include <vector>

namespace spaznet {
class Socket;
}

namespace spaznet::udp {

// One received UDP datagram, with the peer address parsed into a
// human-readable dotted-quad / colon-hex string and the raw kernel
// sockaddr preserved so handlers can sendto() a reply without
// re-resolving.  `listen_fd` is the UDP socket the packet arrived
// on — pass it (with `peer`/`peer_len`) to sendto() to respond.
struct Packet {
    std::vector<uint8_t> data;
    std::string address;
    uint16_t port{};
    int listen_fd{-1};
    sockaddr_storage peer{};
    socklen_t peer_len{0};
};

// Handler-interface idiom over the low-level Server callbacks.  Subclass
// + override handle_packet, then wrap with either
// spaznet::udp::make_dispatcher (coroutine runtime,
// Server::set_datagram_handler) or spaznet::udp::make_reactor_dispatcher
// (coroutine-free, Server::set_sync_datagram_handler) — same Handler,
// same protocol behavior, pick a runtime.
//
// Runtime-neutral by design: plain synchronous function, no Task, no
// co_await, no completion token. Every UDP interaction in this codebase
// (echo, fire-and-forget reply, broadcast relay, telemetry aggregation)
// is answered — or not answered at all — entirely within one
// handle_packet call, unlike HTTP/1.1's HTTPHandler there's no
// request/response pairing that could need to defer; there is simply
// nothing to defer. If a future handler ever needs to do that, it can
// stash whatever it needs and reply asynchronously via
// packet.listen_fd/peer/peer_len on its own schedule — handle_packet
// itself never blocks on it either way.
class Handler {
  public:
    virtual ~Handler() = default;

    Handler() = default;
    Handler(const Handler&) = delete;
    auto operator=(const Handler&) -> Handler& = delete;
    Handler(Handler&&) = delete;
    auto operator=(Handler&&) -> Handler& = delete;

    // Handle one incoming UDP packet.  To reply, call
    //   ::sendto(packet.listen_fd, body, body_len, 0,
    //            reinterpret_cast<sockaddr*>(&packet.peer),
    //            packet.peer_len);
    virtual void handle_packet(const Packet& packet) = 0;
};

} // namespace spaznet::udp
