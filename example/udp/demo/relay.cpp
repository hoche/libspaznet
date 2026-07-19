// UDP broadcast relay ("chat" over UDP).
//
//   $ ./udp_relay
//   $ nc -u 127.0.0.1 8080     # terminal A
//   $ nc -u 127.0.0.1 8080     # terminal B
//   (type a line in A, it shows up in B, and vice versa)
//
// This is the UDP counterpart to the WebSocket chat demo
// (example/http-websocket/demo/chat.cpp), and the contrast is the
// point: over TCP/WebSocket, "who is connected" and "how do I reply to
// them" come for free from the Socket/Connection object tied to the
// accepted connection. UDP is connectionless — there is no connection
// object, no on_open/on_close, and no guarantee a "sender" is still
// listening. A UDP server that wants to fan messages out to a group of
// peers has to reconstruct that membership itself from the datagrams
// it happens to receive, and remember each peer's raw sockaddr so it
// can sendto() them later.
//
// Peers are "known" only after they've sent at least one datagram
// (there's no other way to learn their address), and this demo never
// evicts stale peers — a production relay would time them out. Kept
// simple on purpose.

#include <libspaznet/server.hpp>
#include <libspaznet/udp/dispatcher.hpp>
#include <libspaznet/udp/handler.hpp>

#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unordered_map>

namespace {

// One remembered peer: its raw sockaddr (needed to sendto() it) plus
// the human-readable "address:port" used as both the map key and the
// join-notice text.
struct Peer {
    sockaddr_storage addr{};
    socklen_t addr_len{0};
};

std::string peer_key(const spaznet::udp::Packet& pkt) {
    return pkt.address + ":" + std::to_string(pkt.port);
}

} // namespace

class Relay : public spaznet::udp::Handler {
  public:
    spaznet::Task handle_packet(const spaznet::udp::Packet& pkt) override {
        const std::string key = peer_key(pkt);
        std::string join_notice;

        // The dispatcher (example/udp/src/dispatcher.cpp) shares this one
        // Handler instance across every datagram on every worker thread,
        // so the peer table needs its own lock — nothing upstream
        // serializes access to it the way a per-connection object would.
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto [it, inserted] = peers_.try_emplace(key);
            if (inserted) {
                it->second.addr = pkt.peer;
                it->second.addr_len = pkt.peer_len;
                join_notice = "* " + key + " joined\n";
            }
        }

        const std::string message(pkt.data.begin(), pkt.data.end());

        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [other_key, peer] : peers_) {
            if (other_key == key) {
                continue; // never echo back to the sender
            }
            if (!join_notice.empty()) {
                ::sendto(pkt.listen_fd, join_notice.data(), join_notice.size(), 0,
                         reinterpret_cast<const sockaddr*>(&peer.addr), peer.addr_len);
            }
            ::sendto(pkt.listen_fd, message.data(), message.size(), 0,
                     reinterpret_cast<const sockaddr*>(&peer.addr), peer.addr_len);
        }
        co_return;
    }

  private:
    std::mutex peers_mutex_;
    std::unordered_map<std::string, Peer> peers_;
};

int main() {
    spaznet::Server server(2);
    server.set_datagram_handler(spaznet::udp::make_dispatcher(std::make_unique<Relay>()));
    server.listen_udp(8080);
    server.run();
}
