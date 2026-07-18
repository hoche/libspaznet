// Combined HTTP/1.1 + WebSocket ConnectionHandler.
//
// Sniffs the first ~2 KiB of each accepted connection: if the client
// is asking for a WebSocket upgrade (RFC 6455 §4.2), we complete the
// handshake and switch into the WS frame loop; otherwise we forward
// the already-read bytes plus the rest of the connection to the
// HTTP/1.1 keep-alive serve loop in example/http.

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/websocket/dispatcher.hpp>
#include <libspaznet/websocket/handler.hpp>
#include <libspaznet/websocket/send.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace spaznet::websocket {

namespace {

// ---- crypto helpers (moved from src/server_impl.cpp) -----------------

inline uint32_t rotl(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::array<uint8_t, 20> sha1(const uint8_t* data, std::size_t len) {
    uint64_t total_bits = static_cast<uint64_t>(len) * 8;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() + 8) % 64 != 0) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF));
    }
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (msg[chunk + i * 4] << 24) | (msg[chunk + i * 4 + 1] << 16) |
                   (msg[chunk + i * 4 + 2] << 8) | msg[chunk + i * 4 + 3];
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::array<uint8_t, 20> digest{};
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
    return digest;
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t triple = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t triple = data[i] << 16;
        if (i + 1 < data.size()) triple |= data[i + 1] << 8;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        if (i + 1 < data.size()) {
            out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool header_has_token(const std::string& value, const std::string& token) {
    std::string lower = to_lower(value);
    std::string lower_token = to_lower(token);
    std::istringstream iss(lower);
    std::string part;
    while (std::getline(iss, part, ',')) {
        part.erase(part.begin(),
                   std::find_if(part.begin(), part.end(),
                                [](unsigned char ch) { return !std::isspace(ch); }));
        part.erase(std::find_if(part.rbegin(), part.rend(),
                                [](unsigned char ch) { return !std::isspace(ch); })
                       .base(),
                   part.end());
        if (part == lower_token) return true;
    }
    return false;
}

// ---- upgrade-sniff helpers ------------------------------------------

struct HandshakeRequest {
    std::string method;
    std::map<std::string, std::string> headers;
};

std::optional<HandshakeRequest> parse_handshake(const std::string& request) {
    auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) return std::nullopt;
    std::istringstream iss(request.substr(0, header_end));
    std::string line;
    HandshakeRequest req;
    if (!std::getline(iss, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream start_line(line);
    start_line >> req.method;
    if (req.method.empty()) return std::nullopt;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = to_lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(),
                                 [](unsigned char ch) { return !std::isspace(ch); }));
        req.headers[name] = value;
    }
    return req;
}

std::string compute_accept(const std::string& key) {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = key + kGuid;
    auto digest = sha1(reinterpret_cast<const uint8_t*>(concat.data()), concat.size());
    return base64_encode(std::vector<uint8_t>(digest.begin(), digest.end()));
}

bool is_upgrade(const HandshakeRequest& req) {
    const auto& hdrs = req.headers;
    auto upgrade_it = hdrs.find("upgrade");
    auto conn_it = hdrs.find("connection");
    auto key_it = hdrs.find("sec-websocket-key");
    auto version_it = hdrs.find("sec-websocket-version");
    return upgrade_it != hdrs.end() && conn_it != hdrs.end() && key_it != hdrs.end() &&
           version_it != hdrs.end() && req.method == "GET" &&
           header_has_token(upgrade_it->second, "websocket") &&
           header_has_token(conn_it->second, "upgrade") &&
           to_lower(version_it->second) == "13";
}

// ---- WS frame loop --------------------------------------------------

auto serve_websocket(::spaznet::Socket socket, Handler& handler,
                     const HandshakeRequest& req) -> ::spaznet::Task {
    std::string client_key = req.headers.at("sec-websocket-key");
    std::string accept_key = compute_accept(client_key);

    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << accept_key << "\r\n\r\n";
    std::string resp_str = resp.str();
    co_await socket.async_write({resp_str.begin(), resp_str.end()});

    co_await handler.on_open(socket);

    // Per-connection scratch buffers reused across every read.  See
    // server_impl.cpp's pre-extraction comments for the rationale: this
    // is the WS hot path and the stash-buffered recv collapses
    // header/mask/payload into one syscall on small frames.
    std::vector<uint8_t> read_chunk;
    std::vector<uint8_t> ws_recv_stash;
    std::size_t ws_stash_off = 0;
    constexpr std::size_t kWsRecvHint = 4096;

    auto consume_from_stash =
        [&](std::size_t n, std::vector<uint8_t>& out) -> std::size_t {
        const std::size_t avail = ws_recv_stash.size() - ws_stash_off;
        const std::size_t take = std::min(n, avail);
        out.insert(out.end(), ws_recv_stash.begin() + ws_stash_off,
                   ws_recv_stash.begin() + ws_stash_off + take);
        ws_stash_off += take;
        if (ws_stash_off == ws_recv_stash.size()) {
            ws_recv_stash.clear();
            ws_stash_off = 0;
        }
        return take;
    };

    auto read_exact = [&](std::size_t n, std::vector<uint8_t>& out) -> ::spaznet::Task {
        out.clear();
        out.reserve(n);
        (void)consume_from_stash(n, out);
        while (out.size() < n) {
            const std::size_t need = n - out.size();
            const std::size_t want = std::max(need, kWsRecvHint);
            co_await socket.async_read(read_chunk, want);
            if (read_chunk.empty()) co_return;
            const std::size_t take = std::min(need, read_chunk.size());
            out.insert(out.end(), read_chunk.begin(),
                       read_chunk.begin() + static_cast<std::ptrdiff_t>(take));
            if (read_chunk.size() > take) {
                ws_recv_stash.clear();
                ws_stash_off = 0;
                ws_recv_stash.insert(
                    ws_recv_stash.end(),
                    read_chunk.begin() + static_cast<std::ptrdiff_t>(take),
                    read_chunk.end());
            }
        }
    };

    auto send_frame = [&](Opcode opcode, std::span<const std::uint8_t> payload,
                          uint16_t close_code = 0) -> ::spaznet::Task {
        if (opcode == Opcode::Close && close_code != 0) {
            std::vector<std::uint8_t> body;
            body.reserve(2 + payload.size());
            body.push_back(static_cast<std::uint8_t>((close_code >> 8) & 0xFF));
            body.push_back(static_cast<std::uint8_t>(close_code & 0xFF));
            body.insert(body.end(), payload.begin(), payload.end());
            co_await send_message(socket, opcode, body);
        } else {
            co_await send_message(socket, opcode, payload);
        }
    };

    bool sent_close = false;
    auto fail_close = [&](uint16_t code) -> ::spaznet::Task {
        if (!sent_close) {
            sent_close = true;
            co_await send_frame(Opcode::Close, {}, code);
        }
    };

    std::vector<uint8_t> message_buffer;
    Opcode current_message_opcode = Opcode::Continuation;
    bool fragmented = false;

    std::vector<uint8_t> header;
    std::vector<uint8_t> ext;
    std::vector<uint8_t> mask_key_buf;
    std::vector<uint8_t> payload;

    while (true) {
        co_await read_exact(2, header);
        if (header.size() < 2) break;

        const bool fin = (header[0] & 0x80) != 0;
        const bool rsv1 = (header[0] & 0x40) != 0;
        const bool rsv2 = (header[0] & 0x20) != 0;
        const bool rsv3 = (header[0] & 0x10) != 0;
        const Opcode opcode = static_cast<Opcode>(header[0] & 0x0F);
        const bool masked = (header[1] & 0x80) != 0;
        uint64_t payload_len = header[1] & 0x7F;

        if (rsv1 || rsv2 || rsv3) {
            co_await fail_close(1002);
            break;
        }
        if (!masked) {
            co_await fail_close(1002);
            break;
        }
        const bool opcode_known =
            opcode == Opcode::Continuation || opcode == Opcode::Text ||
            opcode == Opcode::Binary || opcode == Opcode::Close ||
            opcode == Opcode::Ping || opcode == Opcode::Pong;
        if (!opcode_known) {
            co_await fail_close(1002);
            break;
        }

        if (payload_len == 126) {
            co_await read_exact(2, ext);
            if (ext.size() != 2) break;
            payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
            if (payload_len < 126) {
                co_await fail_close(1002);
                break;
            }
        } else if (payload_len == 127) {
            co_await read_exact(8, ext);
            if (ext.size() != 8) break;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) payload_len = (payload_len << 8) | ext[i];
            if (payload_len & (1ULL << 63)) {
                co_await fail_close(1002);
                break;
            }
            if (payload_len < 65536) {
                co_await fail_close(1002);
                break;
            }
        }

        if (payload_len > Frame::kMaxPayloadBytes) {
            co_await fail_close(1009);
            break;
        }

        co_await read_exact(4, mask_key_buf);
        if (mask_key_buf.size() != 4) break;
        const uint32_t masking_key =
            (static_cast<uint32_t>(mask_key_buf[0]) << 24) |
            (static_cast<uint32_t>(mask_key_buf[1]) << 16) |
            (static_cast<uint32_t>(mask_key_buf[2]) << 8) |
            static_cast<uint32_t>(mask_key_buf[3]);

        co_await read_exact(static_cast<std::size_t>(payload_len), payload);
        if (payload.size() != payload_len) break;
        for (std::size_t i = 0; i < payload_len; ++i) {
            payload[i] ^= ((masking_key >> ((3 - (i % 4)) * 8)) & 0xFF);
        }

        const bool is_control =
            opcode == Opcode::Close || opcode == Opcode::Ping || opcode == Opcode::Pong;
        if (is_control) {
            if (!fin || payload_len > 125) {
                co_await fail_close(1002);
                break;
            }
            if (opcode == Opcode::Close) {
                if (!sent_close) {
                    sent_close = true;
                    co_await send_message(socket, Opcode::Close, payload);
                }
                break;
            } else if (opcode == Opcode::Ping) {
                co_await send_frame(Opcode::Pong, payload);
                continue;
            } else if (opcode == Opcode::Pong) {
                continue;
            }
        } else {
            if (opcode != Opcode::Continuation) {
                if (fragmented) {
                    co_await fail_close(1002);
                    break;
                }
                current_message_opcode = opcode;
                std::swap(message_buffer, payload);
                fragmented = !fin;
            } else {
                if (!fragmented) {
                    co_await fail_close(1002);
                    break;
                }
                // Bound the reassembled message. Frame::kMaxPayloadBytes only
                // caps a single frame; without this a peer can stream
                // unlimited fin=0 continuation frames and grow message_buffer
                // without limit (remote OOM). 1009 = Message Too Big.
                if (message_buffer.size() + payload.size() > Frame::kMaxPayloadBytes) {
                    co_await fail_close(1009);
                    break;
                }
                message_buffer.insert(message_buffer.end(), payload.begin(), payload.end());
            }
            if (fin) {
                Message msg;
                msg.opcode = current_message_opcode;
                std::swap(msg.data, message_buffer);
                fragmented = false;
                co_await handler.handle_message(std::move(msg), socket);
                std::swap(message_buffer, msg.data);
                message_buffer.clear();
            }
        }
    }

    co_await handler.on_close(socket);
}

} // namespace

// ---- send_message (was Socket::send_websocket_message) ---------------

auto send_message(::spaznet::Socket& socket, Opcode opcode,
                  std::span<const std::uint8_t> payload, bool fin) -> ::spaznet::Task {
    const std::size_t len = payload.size();

    std::size_t header_size = 2;
    if (len > 65535) {
        header_size += 8;
    } else if (len > 125) {
        header_size += 2;
    }

    std::vector<std::uint8_t> buf;
    buf.resize(header_size + len);

    buf[0] = static_cast<std::uint8_t>((fin ? 0x80 : 0x00) |
                                       (static_cast<std::uint8_t>(opcode) & 0x0F));

    if (len > 65535) {
        buf[1] = 127;
        for (int i = 0; i < 8; ++i) {
            buf[2 + i] = static_cast<std::uint8_t>((len >> (56 - i * 8)) & 0xFF);
        }
    } else if (len > 125) {
        buf[1] = 126;
        buf[2] = static_cast<std::uint8_t>((len >> 8) & 0xFF);
        buf[3] = static_cast<std::uint8_t>(len & 0xFF);
    } else {
        buf[1] = static_cast<std::uint8_t>(len);
    }

    if (len > 0) {
        std::memcpy(buf.data() + header_size, payload.data(), len);
    }

    co_await socket.async_write(std::move(buf));
}

// ---- combined dispatcher --------------------------------------------

auto make_dispatcher(std::unique_ptr<::spaznet::http::HTTPHandler> http_handler,
                     std::unique_ptr<Handler> ws_handler) -> ::spaznet::ConnectionHandler {
    // Wrap into shared_ptr so the std::function payload stays copyable.
    std::shared_ptr<::spaznet::http::HTTPHandler> http_shared(http_handler.release());
    std::shared_ptr<Handler> ws_shared(ws_handler.release());

    return [http_shared, ws_shared](::spaznet::Socket sock) -> ::spaznet::Task {
        // Read the first chunk and decide HTTP vs WS upgrade.
        std::vector<uint8_t> buffer;
        co_await sock.async_read(buffer, 2048);
        if (buffer.empty()) {
            sock.close();
            co_return;
        }

        // Read more if the handshake headers aren't complete yet.
        std::string request_str(buffer.begin(), buffer.end());
        auto handshake = ws_shared ? parse_handshake(request_str)
                                   : std::optional<HandshakeRequest>{};
        if (ws_shared && !handshake) {
            constexpr std::size_t kMaxHandshakeBytes = 8192;
            while (!handshake && buffer.size() < kMaxHandshakeBytes) {
                std::vector<uint8_t> more;
                co_await sock.async_read(more, 2048);
                if (more.empty()) break;
                buffer.insert(buffer.end(), more.begin(), more.end());
                request_str.assign(buffer.begin(), buffer.end());
                handshake = parse_handshake(request_str);
            }
        }

        if (handshake && is_upgrade(*handshake) && ws_shared) {
            co_await serve_websocket(std::move(sock), *ws_shared, *handshake);
            co_return;
        }

        if (http_shared) {
            co_await ::spaznet::http::serve_keep_alive(std::move(sock), *http_shared,
                                                      std::move(buffer));
            co_return;
        }

        // Neither handler — close.
        sock.close();
    };
}

} // namespace spaznet::websocket
