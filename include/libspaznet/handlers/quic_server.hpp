#pragma once

#include <sys/socket.h>
#include <cstdint>
#include <libspaznet/handlers/http3_handler.hpp>
#include <libspaznet/handlers/quic_handler.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spaznet {

// QUIC/HTTP3 server-side engine for driving QUIC over UDP.
// This encapsulates QUIC connection tracking, stream reassembly, and (toy) HTTP/3 request handling.
class QUICServerEngine {
  public:
    QUICServerEngine(QUICHandler* quic_handler, HTTP3Handler* http3_handler);

    // Process one UDP datagram received on `udp_fd` from `addr` and optionally send responses
    // back via sendto() on the same socket.
    auto handle_datagram(int udp_fd, const sockaddr_storage& addr, socklen_t addr_len,
                         const std::vector<uint8_t>& datagram) -> Task;

  private:
    QUICHandler* quic_handler_{nullptr};
    HTTP3Handler* http3_handler_{nullptr};

    // Per-remote QUIC state (keyed by remote endpoint string).
    std::unordered_map<std::string, std::shared_ptr<QUICConnection>> conns_;
    std::unordered_set<std::string> notified_;
    std::unordered_map<std::string, std::unordered_map<uint64_t, std::vector<uint8_t>>>
        h3_stream_buffers_;
};

} // namespace spaznet
