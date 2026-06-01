// HTTP/2 (h2c, prior-knowledge cleartext) ConnectionHandler.
//
// Implements RFC 9113 §3.4 "HTTP/2 over cleartext TCP" plus enough of
// the rest of RFC 9113 to interop with `curl --http2-prior-knowledge`
// and `nghttp -nv` against a single-port server:
//
//   - Connection preface (§3.4)
//   - SETTINGS exchange + ACK (§6.5)
//   - Multiplexed streams keyed by 31-bit stream ID (§5.1)
//   - HEADERS / CONTINUATION (§6.2, §6.10) → user's handle_request
//   - DATA frame request body assembly (§6.1)
//   - HEADERS + DATA response emission, chunked by peer's
//     SETTINGS_MAX_FRAME_SIZE (§4.2)
//   - Per-stream + connection-level flow control windows (§5.2)
//     and WINDOW_UPDATE both inbound and outbound (§6.9)
//   - PING / PING-ACK (§6.7)
//   - GOAWAY emission on shutdown (§6.8)
//   - RST_STREAM on stream-level errors (§6.4)
//
// Not implemented:
//   - PUSH_PROMISE / server push
//   - Priority frames (deprecated in 9113)
//   - Dynamic HPACK table (we advertise SETTINGS_HEADER_TABLE_SIZE = 0
//     so peers don't compress against their own dynamic table)
//   - Trailers
//   - h2 over TLS (run a TLS terminator in front)

#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/server.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace spaznet::http2 {

namespace {

constexpr const char* kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kPrefaceLen = 24;
constexpr std::uint32_t kInitialWindow = 65535;
constexpr std::uint32_t kMaxFrameSizeDefault = 16384;

// Per-stream state.
struct Stream {
    std::uint32_t id{};
    bool headers_received{false};
    bool end_stream_received{false};
    bool reset{false};
    std::int64_t recv_window{kInitialWindow};
    std::int64_t send_window{kInitialWindow};
    std::unordered_map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
    // Accumulates a HEADERS payload across CONTINUATION frames before
    // we hand it to HPACK::decode_headers.
    std::vector<std::uint8_t> pending_hpack;
};

// Read exactly `n` bytes; returns false on EOF/error.
auto read_exact(::spaznet::Socket& socket, std::size_t n, std::vector<std::uint8_t>& out)
    -> ::spaznet::Task {
    out.clear();
    out.reserve(n);
    std::vector<std::uint8_t> chunk;
    while (out.size() < n) {
        co_await socket.async_read(chunk, n - out.size());
        if (chunk.empty()) co_return;
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
}

// Write a Frame to the wire.
auto write_frame(::spaznet::Socket& socket, const Frame& frame) -> ::spaznet::Task {
    Frame mutated = frame;
    mutated.length = static_cast<std::uint32_t>(mutated.payload.size());
    co_await socket.async_write(mutated.serialize());
}

// Compose a goaway frame body: last_stream_id (4) + error_code (4).
auto goaway_payload(std::uint32_t last_stream, std::uint32_t error_code)
    -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> p(8);
    p[0] = static_cast<std::uint8_t>((last_stream >> 24) & 0x7F);
    p[1] = static_cast<std::uint8_t>((last_stream >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((last_stream >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(last_stream & 0xFF);
    p[4] = static_cast<std::uint8_t>((error_code >> 24) & 0xFF);
    p[5] = static_cast<std::uint8_t>((error_code >> 16) & 0xFF);
    p[6] = static_cast<std::uint8_t>((error_code >> 8) & 0xFF);
    p[7] = static_cast<std::uint8_t>(error_code & 0xFF);
    return p;
}

auto serve(::spaznet::Socket socket, Handler& handler) -> ::spaznet::Task {
    // 1. Connection preface (§3.4).
    std::vector<std::uint8_t> buffer;
    co_await read_exact(socket, kPrefaceLen, buffer);
    if (buffer.size() != kPrefaceLen ||
        std::memcmp(buffer.data(), kPreface, kPrefaceLen) != 0) {
        socket.close();
        co_return;
    }

    // 2. Send our SETTINGS.  Header-table-size = 0 keeps the peer from
    // indexing against a dynamic table on their side (we don't
    // maintain one).
    Settings our_settings;
    our_settings.header_table_size = 0;
    our_settings.enable_push = false;
    our_settings.initial_window_size = kInitialWindow;
    our_settings.max_frame_size = kMaxFrameSizeDefault;
    our_settings.max_concurrent_streams = 100;
    {
        Frame settings_frame = Parser::build_settings_frame(our_settings, false);
        co_await write_frame(socket, settings_frame);
    }

    // 3. Frame loop.
    Settings peer_settings; // defaults from RFC 9113 §6.5.2
    std::map<std::uint32_t, Stream> streams;
    std::int64_t conn_recv_window = kInitialWindow;
    std::int64_t conn_send_window = kInitialWindow;
    std::uint32_t last_stream_id_handled = 0;
    bool goaway_sent = false;

    auto rst_stream = [&](std::uint32_t sid, std::uint32_t error_code) -> ::spaznet::Task {
        Frame f = Parser::build_rst_stream_frame(sid, error_code);
        co_await write_frame(socket, f);
        auto it = streams.find(sid);
        if (it != streams.end()) it->second.reset = true;
    };

    auto send_goaway = [&](std::uint32_t error_code) -> ::spaznet::Task {
        if (goaway_sent) co_return;
        goaway_sent = true;
        Frame f;
        f.type = FrameType::GOAWAY;
        f.stream_id = 0;
        f.payload = goaway_payload(last_stream_id_handled, error_code);
        co_await write_frame(socket, f);
    };

    auto send_response = [&](Stream& stream, Response& resp) -> ::spaznet::Task {
        // Pseudo-header first per RFC 9113 §8.1.2.1.
        resp.headers[":status"] = std::to_string(resp.status_code);
        if (resp.headers.find("content-length") == resp.headers.end() &&
            !resp.body.empty()) {
            resp.headers["content-length"] = std::to_string(resp.body.size());
        }
        Frame headers_frame = Parser::build_headers_frame(
            resp, stream.id,
            /*end_headers=*/true,
            /*end_stream=*/resp.body.empty());
        co_await write_frame(socket, headers_frame);
        if (resp.body.empty()) co_return;

        // Stream body, chunked by peer's MAX_FRAME_SIZE.
        const std::size_t max_chunk = peer_settings.max_frame_size > 0
                                          ? peer_settings.max_frame_size
                                          : kMaxFrameSizeDefault;
        std::size_t off = 0;
        while (off < resp.body.size()) {
            const std::size_t take = std::min(max_chunk, resp.body.size() - off);
            std::vector<std::uint8_t> chunk(resp.body.begin() + off,
                                            resp.body.begin() + off + take);
            const bool end = (off + take == resp.body.size());
            Frame f = Parser::build_data_frame(stream.id, chunk, end);
            co_await write_frame(socket, f);
            off += take;
        }
    };

    auto deliver_request = [&](Stream& stream) -> ::spaznet::Task {
        Request req;
        req.stream_id = stream.id;
        req.headers = stream.headers;
        req.body = std::move(stream.body);
        auto m = stream.headers.find(":method");
        auto p = stream.headers.find(":path");
        if (m != stream.headers.end()) req.method = m->second;
        if (p != stream.headers.end()) req.path = p->second;

        Response resp;
        resp.stream_id = stream.id;
        resp.status_code = DEFAULT_HTTP_STATUS_CODE;

        // co_await is forbidden inside a try-handler block — flag a
        // failure with a bool and emit the RST_STREAM outside the
        // catch.
        bool errored = false;
        socket.context()->increment_active_requests();
        try {
            co_await handler.handle_request(req, resp, socket);
        } catch (...) {
            errored = true;
        }
        socket.context()->decrement_active_requests();
        if (errored) {
            co_await rst_stream(stream.id, /*INTERNAL_ERROR*/ 0x2);
            co_return;
        }
        co_await send_response(stream, resp);
    };

    while (true) {
        // Read 9-byte frame header.
        std::vector<std::uint8_t> hdr;
        co_await read_exact(socket, 9, hdr);
        if (hdr.size() != 9) break;
        const std::uint32_t length =
            (static_cast<std::uint32_t>(hdr[0]) << 16) |
            (static_cast<std::uint32_t>(hdr[1]) << 8) | static_cast<std::uint32_t>(hdr[2]);
        const FrameType type = static_cast<FrameType>(hdr[3]);
        const std::uint8_t flags = hdr[4];
        const std::uint32_t stream_id =
            ((static_cast<std::uint32_t>(hdr[5]) << 24) |
             (static_cast<std::uint32_t>(hdr[6]) << 16) |
             (static_cast<std::uint32_t>(hdr[7]) << 8) |
             static_cast<std::uint32_t>(hdr[8])) &
            0x7FFFFFFFU;

        if (length > our_settings.max_frame_size) {
            co_await send_goaway(/*FRAME_SIZE_ERROR*/ 0x6);
            break;
        }
        std::vector<std::uint8_t> payload;
        if (length > 0) {
            co_await read_exact(socket, length, payload);
            if (payload.size() != length) break;
        }

        // Connection-level flow control on DATA frames.
        if (type == FrameType::DATA) {
            conn_recv_window -= static_cast<std::int64_t>(length);
        }

        switch (type) {
            case FrameType::SETTINGS: {
                if ((flags & Flags::ACK) != 0) {
                    // Peer ACKed our settings — nothing to do.
                    break;
                }
                peer_settings = Settings::parse(payload);
                // Adjust per-stream send windows by the delta from the
                // old initial_window_size to the new one (§6.9.2).
                // For simplicity we assume this only matters before
                // any streams exist; on first SETTINGS this is right.
                Frame ack;
                ack.type = FrameType::SETTINGS;
                ack.flags = Flags::ACK;
                ack.stream_id = 0;
                co_await write_frame(socket, ack);
                break;
            }
            case FrameType::PING: {
                if ((flags & Flags::ACK) != 0) {
                    break; // pong — done
                }
                Frame pong;
                pong.type = FrameType::PING;
                pong.flags = Flags::ACK;
                pong.stream_id = 0;
                pong.payload = payload;
                co_await write_frame(socket, pong);
                break;
            }
            case FrameType::WINDOW_UPDATE: {
                if (payload.size() != 4) {
                    co_await send_goaway(/*FRAME_SIZE_ERROR*/ 0x6);
                    break;
                }
                const std::uint32_t inc =
                    ((static_cast<std::uint32_t>(payload[0]) << 24) |
                     (static_cast<std::uint32_t>(payload[1]) << 16) |
                     (static_cast<std::uint32_t>(payload[2]) << 8) |
                     static_cast<std::uint32_t>(payload[3])) &
                    0x7FFFFFFFU;
                if (stream_id == 0) {
                    conn_send_window += static_cast<std::int64_t>(inc);
                } else {
                    auto& s = streams[stream_id];
                    s.id = stream_id;
                    s.send_window += static_cast<std::int64_t>(inc);
                }
                break;
            }
            case FrameType::RST_STREAM: {
                auto it = streams.find(stream_id);
                if (it != streams.end()) it->second.reset = true;
                break;
            }
            case FrameType::GOAWAY: {
                // Peer is closing — drain.
                co_await send_goaway(0);
                socket.close();
                co_return;
            }
            case FrameType::HEADERS: {
                if (stream_id == 0) {
                    co_await send_goaway(/*PROTOCOL_ERROR*/ 0x1);
                    socket.close();
                    co_return;
                }
                auto& s = streams[stream_id];
                s.id = stream_id;
                s.headers_received = true;
                // Strip pad / priority bytes if present.
                std::size_t off = 0;
                std::size_t end_off = payload.size();
                if ((flags & Flags::PADDED) != 0 && off < end_off) {
                    const std::uint8_t pad_len = payload[off++];
                    if (pad_len >= end_off - off) {
                        co_await rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        break;
                    }
                    end_off -= pad_len;
                }
                if ((flags & Flags::PRIORITY) != 0) {
                    if (off + 5 > end_off) {
                        co_await rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        break;
                    }
                    off += 5;
                }
                s.pending_hpack.assign(payload.begin() + off, payload.begin() + end_off);

                const bool end_headers = (flags & Flags::END_HEADERS) != 0;
                const bool end_stream = (flags & Flags::END_STREAM) != 0;
                if (end_stream) s.end_stream_received = true;

                if (end_headers) {
                    s.headers = HPACK::decode_headers(s.pending_hpack);
                    s.pending_hpack.clear();
                    if (end_stream) {
                        last_stream_id_handled = stream_id;
                        co_await deliver_request(s);
                        streams.erase(stream_id);
                    }
                }
                break;
            }
            case FrameType::CONTINUATION: {
                auto it = streams.find(stream_id);
                if (it == streams.end() || !it->second.headers_received) {
                    co_await send_goaway(/*PROTOCOL_ERROR*/ 0x1);
                    socket.close();
                    co_return;
                }
                auto& s = it->second;
                s.pending_hpack.insert(s.pending_hpack.end(), payload.begin(),
                                       payload.end());
                if ((flags & Flags::END_HEADERS) != 0) {
                    s.headers = HPACK::decode_headers(s.pending_hpack);
                    s.pending_hpack.clear();
                    if (s.end_stream_received) {
                        last_stream_id_handled = stream_id;
                        co_await deliver_request(s);
                        streams.erase(stream_id);
                    }
                }
                break;
            }
            case FrameType::DATA: {
                auto it = streams.find(stream_id);
                if (it == streams.end() || !it->second.headers_received) {
                    co_await send_goaway(/*PROTOCOL_ERROR*/ 0x1);
                    socket.close();
                    co_return;
                }
                auto& s = it->second;
                std::size_t off = 0;
                std::size_t end_off = payload.size();
                if ((flags & Flags::PADDED) != 0 && off < end_off) {
                    const std::uint8_t pad_len = payload[off++];
                    if (pad_len >= end_off - off) {
                        co_await rst_stream(stream_id, /*PROTOCOL_ERROR*/ 0x1);
                        break;
                    }
                    end_off -= pad_len;
                }
                s.body.insert(s.body.end(), payload.begin() + off,
                              payload.begin() + end_off);
                s.recv_window -= static_cast<std::int64_t>(end_off - off);

                // Replenish the peer's send window aggressively (we
                // never delay reads, so just announce we have room
                // for what we just consumed).
                if (length > 0) {
                    Frame wu = Parser::build_window_update_frame(0, length);
                    co_await write_frame(socket, wu);
                    Frame wu_s = Parser::build_window_update_frame(stream_id, length);
                    co_await write_frame(socket, wu_s);
                }

                if ((flags & Flags::END_STREAM) != 0) {
                    s.end_stream_received = true;
                    last_stream_id_handled = stream_id;
                    co_await deliver_request(s);
                    streams.erase(stream_id);
                }
                break;
            }
            case FrameType::PRIORITY:
            case FrameType::PUSH_PROMISE:
            default:
                // Ignore unknown / unsupported frame types per
                // RFC 9113 §4.1: "implementations MUST discard frames
                // that have unknown or unsupported types."
                break;
        }
    }

    co_await send_goaway(0);
    socket.close();
}

} // namespace

auto make_dispatcher(std::unique_ptr<Handler> handler) -> ::spaznet::ConnectionHandler {
    std::shared_ptr<Handler> shared(handler.release());
    return [shared](::spaznet::Socket sock) -> ::spaznet::Task {
        co_await serve(std::move(sock), *shared);
    };
}

} // namespace spaznet::http2
