#include <libspaznet/http3/server.hpp>

#include <algorithm>
#include <cstring>

#include <libspaznet/http3/h3frame.hpp>
#include <libspaznet/quic/varint.hpp>

namespace spaznet {
namespace http3 {

namespace {

using spaznet::quic::VarInt;

// Try to decode a leading varint from `buf`. Returns true and consumes
// the bytes from the front on success.
auto consume_leading_varint(std::vector<uint8_t>& buf, uint64_t& out) -> bool {
    std::size_t off = 0;
    if (!VarInt::decode(buf.data(), buf.size(), off, out)) {
        return false;
    }
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(off));
    return true;
}

// Try to decode a leading HTTP/3 frame from `buf`. On success, consumes
// the frame's bytes from the front of `buf` and returns true.
auto consume_leading_h3_frame(std::vector<uint8_t>& buf, H3Frame& out) -> bool {
    std::size_t off = 0;
    if (!parse_h3_frame({buf.data(), buf.size()}, off, out)) {
        return false;
    }
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(off));
    return true;
}

// Map a stream ID to its QUIC type (bidi/uni, client/server-initiated).
auto is_uni(uint64_t id) -> bool {
    return (id & 0x02U) != 0;
}
auto is_client_initiated(uint64_t id) -> bool {
    return (id & 0x01U) == 0;
}
auto is_request_stream(uint64_t id) -> bool {
    // Client-initiated bidirectional → request stream.
    return !is_uni(id) && is_client_initiated(id);
}

} // namespace

Http3Server::Http3Server(quic::Connection& conn, RequestFn on_request)
    : conn_(conn), on_request_(std::move(on_request)) {}

auto Http3Server::initialize_server_streams() -> void {
    if (initialized_) return;
    // Control stream: stream type 0x00 followed by a SETTINGS frame.
    std::vector<uint8_t> ctl;
    VarInt::append(ctl, static_cast<uint64_t>(H3UniStreamType::Control));
    // Settings: we set QPACK capacity = 0, blocked streams = 0 so the
    // peer is forced into static-only QPACK.
    H3Settings settings;
    settings.entries.emplace_back(static_cast<uint64_t>(H3SettingId::QpackMaxTableCapacity), 0);
    settings.entries.emplace_back(static_cast<uint64_t>(H3SettingId::QpackBlockedStreams), 0);
    encode_h3_frame(ctl, H3Frame{settings});
    conn_.write_stream(kServerControl, {ctl.data(), ctl.size()}, /*fin=*/false);

    // QPACK encoder + decoder streams: just emit the type byte. We won't
    // send any QPACK dynamic-table updates (capacity 0), but RFC 9204
    // requires the streams to exist.
    std::vector<uint8_t> qenc;
    VarInt::append(qenc, static_cast<uint64_t>(H3UniStreamType::QpackEncoder));
    conn_.write_stream(kServerQpackEncoder, {qenc.data(), qenc.size()}, false);
    std::vector<uint8_t> qdec;
    VarInt::append(qdec, static_cast<uint64_t>(H3UniStreamType::QpackDecoder));
    conn_.write_stream(kServerQpackDecoder, {qdec.data(), qdec.size()}, false);

    initialized_ = true;
}

auto Http3Server::pump() -> void {
    if (!conn_.handshake_complete()) {
        return;
    }
    initialize_server_streams();

    // Iterate streams and process anything new.
    conn_.for_each_stream([this](uint64_t id, quic::Stream& s) {
        // Ignore streams we created ourselves.
        if (id == kServerControl || id == kServerQpackEncoder || id == kServerQpackDecoder) {
            return;
        }
        process_stream(id, s);
    });
}

auto Http3Server::process_stream(uint64_t id, quic::Stream& s) -> void {
    std::vector<uint8_t> chunk;
    bool fin = false;
    s.read_contiguous(chunk, fin);
    if (chunk.empty() && !fin) {
        return;
    }
    if (is_uni(id) && is_client_initiated(id)) {
        auto& u = uni_[id];
        u.buf.insert(u.buf.end(), chunk.begin(), chunk.end());
        if (u.type == UINT64_MAX) {
            uint64_t t = 0;
            if (!consume_leading_varint(u.buf, t)) {
                return; // wait for more bytes
            }
            u.type = t;
        }
        // For Control streams: parse and discard incoming frames; we
        // only care about SETTINGS for now (and just ignore them).
        // For QPACK encoder/decoder streams: also discard (we set
        // capacity to 0 so the peer can't send meaningful instructions).
        if (u.type == static_cast<uint64_t>(H3UniStreamType::Control)) {
            H3Frame f;
            while (consume_leading_h3_frame(u.buf, f)) {
                // no-op
            }
        }
        return;
    }
    if (!is_request_stream(id)) {
        return; // ignore non-request bidi (shouldn't happen as a server)
    }
    auto& pr = pending_[id];
    pr.recv_buf.insert(pr.recv_buf.end(), chunk.begin(), chunk.end());
    if (fin) pr.fin_seen = true;

    // Drain framed bytes.
    while (true) {
        H3Frame f;
        if (!consume_leading_h3_frame(pr.recv_buf, f)) break;
        if (auto* h = std::get_if<H3Headers>(&f); h != nullptr) {
            HeaderList hl;
            if (!qpack_decode({h->encoded_field_section.data(),
                               h->encoded_field_section.size()},
                              hl)) {
                return;
            }
            for (auto& [name, value] : hl) {
                if (name == ":method")    pr.req.method = value;
                else if (name == ":scheme")   pr.req.scheme = value;
                else if (name == ":authority") pr.req.authority = value;
                else if (name == ":path")     pr.req.path = value;
                else pr.req.headers.emplace_back(std::move(name), std::move(value));
            }
            pr.headers_parsed = true;
        } else if (auto* d = std::get_if<H3Data>(&f); d != nullptr) {
            pr.req.body.insert(pr.req.body.end(), d->data.begin(), d->data.end());
        }
        // SETTINGS / GOAWAY / etc. on a request stream are protocol
        // errors per RFC 9114 §7.2; ignore for now.
    }

    if (pr.headers_parsed && pr.fin_seen && !pr.handled) {
        pr.handled = true;
        Http3Response resp = on_request_(pr.req);

        // Build response headers list with :status first.
        HeaderList resp_headers;
        resp_headers.emplace_back(":status", std::to_string(resp.status_code));
        for (auto& [n, v] : resp.headers) {
            resp_headers.emplace_back(n, v);
        }
        std::vector<uint8_t> encoded_headers;
        qpack_encode(resp_headers, encoded_headers);

        std::vector<uint8_t> wire;
        encode_h3_frame(wire, H3Frame{H3Headers{std::move(encoded_headers)}});
        if (!resp.body.empty()) {
            encode_h3_frame(wire, H3Frame{H3Data{std::move(resp.body)}});
        }
        conn_.write_stream(id, {wire.data(), wire.size()}, /*fin=*/true);
    }
}

} // namespace http3
} // namespace spaznet
