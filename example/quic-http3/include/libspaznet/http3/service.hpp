#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include <libspaznet/http3/server.hpp>
#include <libspaznet/quic/listener.hpp>
#include <libspaznet/server.hpp>

namespace spaznet {
namespace http3 {

// Convenience object that wires a quic::Listener to per-connection
// Http3Server instances. The lifetime model:
//   - Construct once per UDP socket.
//   - For every received datagram call handle_datagram(peer, bytes).
//   - The service routes the datagram into the Listener, then walks the
//     active set of QUIC Connections and pumps each connection's
//     Http3Server (creating one lazily on first sighting). Outgoing
//     datagrams flow back through the SendFn the caller installed on
//     the Listener via its config.
//
// The user-provided `RequestFn` is invoked synchronously when a full
// HTTP/3 request has arrived on a request stream.
class QuicHttp3Service {
  public:
    // Explicit-SendFn constructor — useful for tests that route
    // outbound bytes through an in-memory queue.
    QuicHttp3Service(quic::Listener::Config listener_cfg,
                     quic::Listener::SendFn send_fn, Http3Server::RequestFn on_request);

    // Self-routing constructor — the service installs its own
    // `SendFn` that calls `::sendto(fd, ...)` on the listening UDP
    // socket.  The fd is supplied later via `bind_fd()` (typically
    // by `make_dispatcher`, which pulls it out of the first
    // incoming `Datagram`).  Until `bind_fd` runs the SendFn is a
    // no-op, which is fine: the very first datagram from a peer
    // triggers the bind before any outbound packet is built.
    QuicHttp3Service(quic::Listener::Config listener_cfg, Http3Server::RequestFn on_request);

    QuicHttp3Service(const QuicHttp3Service&) = delete;
    auto operator=(const QuicHttp3Service&) -> QuicHttp3Service& = delete;
    QuicHttp3Service(QuicHttp3Service&&) = delete;
    auto operator=(QuicHttp3Service&&) -> QuicHttp3Service& = delete;
    ~QuicHttp3Service() = default;

    auto handle_datagram(const quic::PeerAddr& peer, std::span<const uint8_t> dg) -> void;

    // Drive every connection's timers + HTTP/3 layer once.
    auto pump_all() -> void;

    // Late-binding for the listening UDP fd.  Required when the
    // self-routing constructor was used.  No-op for the explicit
    // SendFn variant.  Subsequent calls overwrite the prior value
    // (which lets a fresh `listen_udp` after `stop()` rebind).
    auto bind_fd(int fd) -> void {
        fd_.store(fd, std::memory_order_relaxed);
    }

    [[nodiscard]] auto listener() -> quic::Listener& {
        return listener_;
    }

  private:
    std::atomic<int> fd_{-1};
    quic::Listener listener_;
    Http3Server::RequestFn on_request_;
    // Per-connection Http3Server instances, keyed by the server-chosen
    // SCID (the same key the Listener uses).
    std::map<std::vector<uint8_t>, std::unique_ptr<Http3Server>> servers_;

    auto ensure_http3_for(uint64_t /*placeholder*/) -> void {}
};

// Build a ::spaznet::DatagramHandler that owns `service` and forwards
// every received datagram into it.  Hand the result to
// Server::set_datagram_handler:
//
//   auto svc = std::make_unique<QuicHttp3Service>(...);
//   server.set_datagram_handler(spaznet::http3::make_dispatcher(std::move(svc)));
//   server.listen_udp(4433);
//
// The Listener's SendFn (configured inside QuicHttp3Service) should
// route outbound datagrams via `::sendto(datagram.fd, ...)` so the
// reply goes back through the same listening socket.
auto make_dispatcher(std::unique_ptr<QuicHttp3Service> service) -> ::spaznet::DatagramHandler;

} // namespace http3
} // namespace spaznet
