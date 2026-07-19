#include <libspaznet/http3/service.hpp>

#include <cstring>
#include <memory>
#include <utility>

#include <libspaznet/detail/socket_compat.hpp>

namespace spaznet {
namespace http3 {

QuicHttp3Service::QuicHttp3Service(quic::Listener::Config listener_cfg,
                                   quic::Listener::SendFn send_fn,
                                   Http3Server::RequestFn on_request)
    : listener_(std::move(listener_cfg), std::move(send_fn)),
      on_request_(std::move(on_request)) {}

QuicHttp3Service::QuicHttp3Service(quic::Listener::Config listener_cfg,
                                   Http3Server::RequestFn on_request)
    : listener_(std::move(listener_cfg),
                [this](const quic::PeerAddr& peer, std::span<const uint8_t> bytes) {
                    const int f = fd_.load(std::memory_order_relaxed);
                    if (f < 0) return;
                    // Best-effort; a UDP send failure here is silently
                    // dropped — the peer will retransmit or the
                    // connection will idle out.  We don't have a
                    // notification channel back into application
                    // code from a SendFn.
                    (void)detail::socket_sendto(f, bytes.data(), bytes.size(), 0, peer.data(),
                                                peer.length);
                }),
      on_request_(std::move(on_request)) {}

auto QuicHttp3Service::handle_datagram(const quic::PeerAddr& peer,
                                       std::span<const uint8_t> dg) -> void {
    listener_.on_datagram(peer, dg);
    pump_all();
}

auto QuicHttp3Service::pump_all() -> void {
    listener_.on_timer();
    listener_.for_each_connection([this](const std::vector<uint8_t>& scid,
                                         quic::Connection& conn) {
        auto it = servers_.find(scid);
        if (it == servers_.end()) {
            it = servers_.emplace(scid, std::make_unique<Http3Server>(conn, on_request_))
                     .first;
        }
        it->second->pump();
    });
}

auto make_dispatcher(std::unique_ptr<QuicHttp3Service> service)
    -> ::spaznet::DatagramHandler {
    // shared_ptr so the std::function payload stays copyable.
    std::shared_ptr<QuicHttp3Service> shared(service.release());
    return [shared](::spaznet::Datagram dg) -> ::spaznet::Task {
        // First-datagram bind: the listening UDP fd lives in the
        // incoming Datagram, so we install it here.  The service's
        // self-routing SendFn reads `fd_` atomically on every call.
        shared->bind_fd(dg.fd);
        ::spaznet::quic::PeerAddr peer{};
        peer.length = dg.peer_len;
        std::memcpy(&peer.storage, &dg.peer, dg.peer_len);
        shared->handle_datagram(peer, {dg.data.data(), dg.data.size()});
        co_return;
    };
}

} // namespace http3
} // namespace spaznet
