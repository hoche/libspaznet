#include <libspaznet/http3/service.hpp>

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

} // namespace http3
} // namespace spaznet
