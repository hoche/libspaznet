// Minimal UDP echo server.
//
//   $ ./udp_echo &
//   $ echo -n hi | nc -u -w1 127.0.0.1 8080
//   hi

#include <libspaznet/server.hpp>
#include <libspaznet/udp/dispatcher.hpp>
#include <libspaznet/udp/handler.hpp>

#include <libspaznet/detail/socket_compat.hpp>

#include <memory>

class Echo : public spaznet::udp::Handler {
  public:
    spaznet::Task handle_packet(const spaznet::udp::Packet& pkt) override {
        spaznet::detail::socket_sendto(pkt.listen_fd, pkt.data.data(), pkt.data.size(), 0,
                                       reinterpret_cast<const sockaddr*>(&pkt.peer), pkt.peer_len);
        co_return;
    }
};

int main() {
    spaznet::Server server(2);
    server.set_datagram_handler(spaznet::udp::make_dispatcher(std::make_unique<Echo>()));
    server.listen_udp(8080);
    server.run();
}
