#pragma once

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include <libspaznet/http3/server.hpp>
#include <libspaznet/quic/listener.hpp>

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
    QuicHttp3Service(quic::Listener::Config listener_cfg,
                     quic::Listener::SendFn send_fn, Http3Server::RequestFn on_request);

    QuicHttp3Service(const QuicHttp3Service&) = delete;
    auto operator=(const QuicHttp3Service&) -> QuicHttp3Service& = delete;
    QuicHttp3Service(QuicHttp3Service&&) = delete;
    auto operator=(QuicHttp3Service&&) -> QuicHttp3Service& = delete;
    ~QuicHttp3Service() = default;

    auto handle_datagram(const quic::PeerAddr& peer, std::span<const uint8_t> dg) -> void;

    // Drive every connection's timers + HTTP/3 layer once.
    auto pump_all() -> void;

    [[nodiscard]] auto listener() -> quic::Listener& {
        return listener_;
    }

  private:
    quic::Listener listener_;
    Http3Server::RequestFn on_request_;
    // Per-connection Http3Server instances, keyed by the server-chosen
    // SCID (the same key the Listener uses).
    std::map<std::vector<uint8_t>, std::unique_ptr<Http3Server>> servers_;

    auto ensure_http3_for(uint64_t /*placeholder*/) -> void {}
};

} // namespace http3
} // namespace spaznet
