#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <libspaznet/handlers/quic_server.hpp>
#include <sstream>

namespace spaznet {

namespace {

auto endpoint_key(const sockaddr_storage& addr, socklen_t addr_len) -> std::string {
    char host[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;

    if (addr.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
        port = ntohs(a->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        inet_ntop(AF_INET6, &a6->sin6_addr, host, sizeof(host));
        port = ntohs(a6->sin6_port);
    } else {
        std::snprintf(host, sizeof(host), "unknown");
    }

    std::ostringstream oss;
    oss << host << ":" << port << ":" << static_cast<int>(addr.ss_family) << ":" << addr_len;
    return oss.str();
}

// Variable-length integer encoding (RFC9000 Section 16) - toy.
uint64_t read_varint(const std::vector<uint8_t>& data, size_t& offset, int prefix_bits) {
    if (offset >= data.size()) {
        return 0;
    }
    uint64_t mask = (1ULL << prefix_bits) - 1;
    uint64_t value = data[offset] & mask;
    offset++;
    if (value < (1ULL << (prefix_bits - 2))) {
        return value;
    }
    int bytes = 1 << (value - (1ULL << (prefix_bits - 2)));
    for (int i = 0; i < bytes; ++i) {
        if (offset >= data.size()) {
            return 0;
        }
        value = (value << 8) | data[offset++];
    }
    return value;
}

void write_varint(std::vector<uint8_t>& out, uint64_t value, int prefix_bits) {
    uint64_t max_prefix = (1ULL << (prefix_bits - 2)) - 1;
    if (value < max_prefix) {
        out.push_back(static_cast<uint8_t>(value));
        return;
    }

    int bytes = 1;
    uint64_t v = value;
    while (v > 0) {
        bytes++;
        v >>= 8;
    }

    uint64_t prefix = (1ULL << (prefix_bits - 2)) + (bytes - 1);
    out.push_back(static_cast<uint8_t>((prefix << (8 - prefix_bits)) |
                                       (value >> (bytes * 8 - 8 + prefix_bits))));
    for (int i = bytes - 2; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

bool parse_http3_headers_frame(const std::vector<uint8_t>& data, HTTP3Request& request) {
    if (data.empty()) {
        return false;
    }

    size_t offset = 0;
    uint64_t frame_type = read_varint(data, offset, 8);
    if (frame_type != static_cast<uint64_t>(HTTP3FrameType::Headers)) {
        return false;
    }

    std::string headers_str(data.begin() + offset, data.end());
    std::istringstream iss(headers_str);
    std::string line;

    bool first_line = true;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        if (first_line) {
            std::istringstream first_iss(line);
            std::string method, path, scheme, authority;
            first_iss >> method >> path >> scheme >> authority;
            request.method = method;
            request.request_target = path;
            request.scheme = scheme;
            request.authority = authority;
            first_line = false;
        } else {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                value.erase(value.begin(),
                            std::find_if(value.begin(), value.end(),
                                         [](unsigned char ch) { return !std::isspace(ch); }));
                request.headers[name] = value;
            }
        }
    }

    return !request.method.empty();
}

std::vector<uint8_t> serialize_http3_headers_frame(const HTTP3Response& response) {
    std::vector<uint8_t> frame;
    write_varint(frame, static_cast<uint64_t>(HTTP3FrameType::Headers), 8);

    std::ostringstream oss;
    oss << ":status " << response.status_code << " " << response.reason_phrase << "\r\n";
    for (const auto& [name, value] : response.headers) {
        oss << name << ": " << value << "\r\n";
    }
    oss << "\r\n";

    std::string headers_str = oss.str();
    frame.insert(frame.end(), headers_str.begin(), headers_str.end());
    return frame;
}

std::vector<uint8_t> serialize_http3_data_frame(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> frame;
    write_varint(frame, static_cast<uint64_t>(HTTP3FrameType::Data), 8);
    frame.insert(frame.end(), data.begin(), data.end());
    return frame;
}

void send_reply(int udp_fd, const sockaddr_storage& addr, socklen_t addr_len,
                const std::vector<uint8_t>& data) {
    // Handle IPv4-mapped address case for IPv4 clients.
    const struct sockaddr* send_addr = reinterpret_cast<const struct sockaddr*>(&addr);
    socklen_t send_len = addr_len;
    sockaddr_in a4_reply{};
    if (addr.ss_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            a4_reply.sin_family = AF_INET;
            a4_reply.sin_port = a6->sin6_port;
            std::memcpy(&a4_reply.sin_addr, &a6->sin6_addr.s6_addr[12], 4);
            send_addr = reinterpret_cast<const struct sockaddr*>(&a4_reply);
            send_len = sizeof(a4_reply);
        }
    }
    (void)sendto(udp_fd, data.data(), data.size(), 0, send_addr, send_len);
}

} // namespace

QUICServerEngine::QUICServerEngine(QUICHandler* quic_handler, HTTP3Handler* http3_handler)
    : quic_handler_(quic_handler), http3_handler_(http3_handler) {}

Task QUICServerEngine::handle_datagram(int udp_fd, const sockaddr_storage& addr, socklen_t addr_len,
                                       const std::vector<uint8_t>& datagram) {
    if (!quic_handler_ && !http3_handler_) {
        co_return;
    }

    const std::string key = endpoint_key(addr, addr_len);
    auto it = conns_.find(key);
    if (it == conns_.end()) {
        it = conns_.emplace(key, std::make_shared<QUICConnection>(ConnectionID{}, ConnectionID{}))
                 .first;
    }
    auto conn = it->second;

    if (quic_handler_ && notified_.insert(key).second) {
        co_await quic_handler_->on_connection(conn);
    }

    std::vector<QUICStreamFrame> frames;
    if (!conn->process_packet(datagram, frames)) {
        co_return;
    }

    for (const auto& frame : frames) {
        auto stream = conn->get_stream(frame.stream_id);

        if (quic_handler_) {
            co_await quic_handler_->on_stream_data(conn, stream, frame.data, frame.fin);
        }

        if (!http3_handler_) {
            continue;
        }

        // Reassemble per stream until FIN.
        auto& buf = h3_stream_buffers_[key][frame.stream_id];
        buf.insert(buf.end(), frame.data.begin(), frame.data.end());
        if (!frame.fin) {
            continue;
        }

        std::vector<uint8_t> payload;
        payload.swap(buf);

        HTTP3Request h3req;
        if (!parse_http3_headers_frame(payload, h3req)) {
            continue;
        }

        HTTP3Response h3resp;
        co_await http3_handler_->handle_request(h3req, h3resp, stream);

        std::vector<uint8_t> out_payload;
        auto hdr = serialize_http3_headers_frame(h3resp);
        out_payload.insert(out_payload.end(), hdr.begin(), hdr.end());
        if (!h3resp.body.empty()) {
            auto data_frame = serialize_http3_data_frame(h3resp.body);
            out_payload.insert(out_payload.end(), data_frame.begin(), data_frame.end());
        }

        QUICStreamFrame out_frame;
        out_frame.stream_id = frame.stream_id;
        out_frame.offset = 0;
        out_frame.data = std::move(out_payload);
        out_frame.fin = true;

        auto out_packet = conn->build_packet(QUICPacketType::OneRTT, {out_frame});
        send_reply(udp_fd, addr, addr_len, out_packet);
    }

    co_return;
}

} // namespace spaznet
