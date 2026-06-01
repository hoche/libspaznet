#include <libspaznet/http3/service.hpp>

#include <cstring>
#include <memory>
#include <utility>

namespace spaznet {
namespace http3 {

QuicHttp3Service::QuicHttp3Service(quic::Listener::Config listener_cfg,
                                   quic::Listener::SendFn send_fn,
                                   Http3Server::RequestFn on_request)
    : listener_(std::move(listener_cfg), std::move(send_fn)),
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
        ::spaznet::quic::PeerAddr peer{};
        peer.length = dg.peer_len;
        std::memcpy(&peer.storage, &dg.peer, dg.peer_len);
        shared->handle_datagram(peer, {dg.data.data(), dg.data.size()});
        shared->pump_all();
        co_return;
    };
}

} // namespace http3
} // namespace spaznet
