#pragma once

#include <cstdint>
#include <libspaznet/io_context.hpp>
#include <string>
#include <sys/socket.h>
#include <vector>

namespace spaznet {
class Socket;
}

namespace spaznet::udp {

using ::spaznet::Task;

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

// Handler-interface idiom over the low-level
// Server::set_datagram_handler callback.  Subclass + override
// handle_packet, then wrap with spaznet::udp::make_dispatcher and
// install via set_datagram_handler.
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
    virtual auto handle_packet(const Packet& packet) -> Task = 0;
};

} // namespace spaznet::udp
