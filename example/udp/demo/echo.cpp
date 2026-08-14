// Minimal UDP echo server.
//
//   $ ./udp_echo            # coroutine dispatcher (default)
//   $ ./udp_echo --reactor  # coroutine-free reactor dispatcher
//   $ echo -n hi | nc -u -w1 127.0.0.1 8080
//   hi

#include <libspaznet/server.hpp>
#include <libspaznet/udp/dispatcher.hpp>
#include <libspaznet/udp/handler.hpp>

#include <libspaznet/detail/socket_compat.hpp>

#include <cstring>
#include <memory>

class Echo : public spaznet::udp::Handler {
  public:
    void handle_packet(const spaznet::udp::Packet& pkt) override {
        spaznet::detail::socket_sendto(pkt.listen_fd, pkt.data.data(), pkt.data.size(), 0,
                                       reinterpret_cast<const sockaddr*>(&pkt.peer), pkt.peer_len);
    }
};

int main(int argc, char** argv) {
#ifdef SPAZNET_HAS_COROUTINES
    bool use_reactor = false;
#else
    bool use_reactor = true; // Coroutine dispatcher isn't built in this configuration.
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reactor") == 0) {
            use_reactor = true;
        }
    }

    spaznet::Server server(2);
    if (use_reactor) {
        server.set_reactor_sync_datagram_handler(spaznet::udp::make_reactor_dispatcher(std::make_unique<Echo>()));
    }
#ifdef SPAZNET_HAS_COROUTINES
    else {
        server.set_coroutine_datagram_handler(spaznet::udp::make_coroutine_dispatcher(std::make_unique<Echo>()));
    }
#endif
    server.listen_udp(8080);
    server.run();
}
