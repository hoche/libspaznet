// Combined HTTP/1.1 + WebSocket CoroutineConnectionHandler.
//
// Sniffs the first ~2 KiB of each accepted connection: if the client
// is asking for a WebSocket upgrade (RFC 6455 §4.2), we complete the
// handshake and switch into the WS frame loop; otherwise we forward
// the already-read bytes plus the rest of the connection to the
// HTTP/1.1 keep-alive serve loop in example/http.

#include "handshake.hpp"

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/websocket/dispatcher.hpp>
#include <libspaznet/websocket/coroutine_handler.hpp>
#include <libspaznet/websocket/handler.hpp>
#include <libspaznet/websocket/send.hpp>

#include <coroutine>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace spaznet::websocket {

using detail::HandshakeRequest;
using detail::compute_accept;
using detail::is_upgrade;
using detail::parse_handshake;

namespace coroutine {

// Per-connection async write gate. Every write to a connection's socket is
// serialized through here: the dispatcher's reader coroutine (Pong/Close) and
// any application coroutine (e.g. a chat broadcaster) all acquire the gate
// before writing and release it after, so frames never interleave on the wire
// and two coroutines never both hold the IOContext's per-fd write slot.
//
// The gate is a fair (FIFO) async mutex. Acquisition never blocks a worker
// thread: an uncontended acquire completes inline; a contended one suspends
// the coroutine and is resumed by whoever releases the gate.
struct WriteGate {
    std::mutex m;
    bool held{false};
    std::vector<std::coroutine_handle<TaskPromise>> waiters; // FIFO

    struct AcquireAwaiter {
        WriteGate* gate;
        [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }
        // Returns false to continue immediately (gate acquired), true to
        // suspend until a release() hands this coroutine the gate. The decision
        // is made under the lock so a concurrent release can't be lost.
        auto await_suspend(std::coroutine_handle<TaskPromise> handle) -> bool {
            std::lock_guard<std::mutex> lock(gate->m);
            if (!gate->held) {
                gate->held = true;
                return false;
            }
            gate->waiters.push_back(handle);
            return true;
        }
        void await_resume() const noexcept {}
    };

    auto acquire() -> AcquireAwaiter { return AcquireAwaiter{this}; }

    // Hand the gate to the next waiter (resuming it via the scheduler so we
    // don't recurse into it on the releasing coroutine's stack), or mark the
    // gate free if nobody is waiting.
    void release(::spaznet::IOContext* ctx) {
        std::coroutine_handle<TaskPromise> next{};
        {
            std::lock_guard<std::mutex> lock(m);
            if (!waiters.empty()) {
                next = waiters.front();
                waiters.erase(waiters.begin());
                // held stays true: ownership transfers to `next`.
            } else {
                held = false;
            }
        }
        if (next) {
            ctx->schedule(Task::from_handle(next));
        }
    }
};

} // namespace coroutine

namespace {

// Build a server-origin (unmasked) WebSocket frame in one allocation.
// Shared by send_message() and Connection::send().
auto build_frame(Opcode opcode, std::span<const std::uint8_t> payload, bool fin)
    -> std::vector<std::uint8_t> {
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
    return buf;
}

// ---- WS frame loop --------------------------------------------------

auto serve_websocket(::spaznet::Socket socket, coroutine::Handler& handler,
                     const HandshakeRequest& req, std::vector<uint8_t> initial)
    -> ::spaznet::Task {
    std::string client_key = req.headers.at("sec-websocket-key");
    std::string accept_key = compute_accept(client_key);

    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << accept_key << "\r\n\r\n";
    std::string resp_str = resp.str();
    // The handshake response is written before on_open and before any
    // application writer coroutine can exist, so this raw write is
    // uncontended and doesn't need the gate.
    co_await socket.async_write({resp_str.begin(), resp_str.end()});

    // From here on every write to this socket — the dispatcher's own control
    // frames below AND anything the handler sends (including from a separate
    // writer coroutine on another thread) — funnels through `conn`, which
    // serializes them via `gate`. gate/conn live on this coroutine frame for
    // the whole connection, so handlers that stash `&conn` must not use it
    // after on_close returns (the frame is destroyed right after).
    coroutine::WriteGate gate;
    coroutine::Connection conn{socket, gate};

    co_await handler.on_open(conn);

    // Per-connection scratch buffers reused across every read.  See
    // server_impl.cpp's pre-extraction comments for the rationale: this
    // is the WS hot path and the stash-buffered recv collapses
    // header/mask/payload into one syscall on small frames.
    std::vector<uint8_t> read_chunk;
    // Seed the stash with any bytes the client pipelined right after the
    // upgrade request so the first WS frame isn't lost.
    std::vector<uint8_t> ws_recv_stash = std::move(initial);
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
            co_await conn.send(opcode, body);
        } else {
            co_await conn.send(opcode, payload);
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

        const bool is_control =
            opcode == Opcode::Close || opcode == Opcode::Ping || opcode == Opcode::Pong;
        // RFC 6455 §5.5: control frames MUST NOT be fragmented and carry at
        // most 125 bytes. Reject *before* reading and unmasking the payload
        // so an oversized/fragmented control frame can't make us buffer and
        // XOR up to kMaxPayloadBytes of attacker data.
        if (is_control && (!fin || payload_len > 125)) {
            co_await fail_close(1002);
            break;
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

        if (is_control) {
            // Fragmentation / size already validated above, before the read.
            if (opcode == Opcode::Close) {
                if (!sent_close) {
                    sent_close = true;
                    co_await conn.send(Opcode::Close, payload);
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
                co_await handler.handle_message(std::move(msg), conn);
                std::swap(message_buffer, msg.data);
                message_buffer.clear();
            }
        }
    }

    co_await handler.on_close(conn);
}

} // namespace

// ---- send_message (was Socket::send_websocket_message) ---------------

auto send_message(::spaznet::Socket& socket, Opcode opcode,
                  std::span<const std::uint8_t> payload, bool fin) -> ::spaznet::Task {
    // Low-level, UNSERIALIZED write. Safe only when the caller guarantees it
    // is the sole writer of `socket` (e.g. a handler that writes exclusively
    // from its own inline handle_message). Handlers with an independent writer
    // coroutine must use coroutine::Connection::send instead, which serializes writes.
    co_await socket.async_write(build_frame(opcode, payload, fin));
}

// ---- Connection (serialized per-connection sender) ------------------

auto coroutine::Connection::id() const -> int {
    return socket_->fd();
}

auto coroutine::Connection::context() const -> ::spaznet::IOContext* {
    return socket_->context();
}

auto coroutine::Connection::send(Opcode opcode, std::span<const std::uint8_t> payload, bool fin)
    -> ::spaznet::Task {
    // Build first (no shared state touched), then serialize the actual write
    // through the per-connection gate so this never interleaves with the
    // dispatcher's control frames or another coroutine's send.
    std::vector<std::uint8_t> buf = build_frame(opcode, payload, fin);
    co_await gate_->acquire();
    co_await socket_->async_write(std::move(buf));
    gate_->release(socket_->context());
}

// ---- combined dispatcher --------------------------------------------

auto make_coroutine_dispatcher(std::unique_ptr<::spaznet::http::HTTPHandler> http_handler,
                     std::unique_ptr<coroutine::Handler> ws_handler)
    -> ::spaznet::CoroutineConnectionHandler {
    // Wrap into shared_ptr so the std::function payload stays copyable.
    std::shared_ptr<::spaznet::http::HTTPHandler> http_shared(http_handler.release());
    std::shared_ptr<coroutine::Handler> ws_shared(ws_handler.release());

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
            // Preserve any bytes pipelined after the upgrade request's
            // header terminator (e.g. an immediate WS frame in the same
            // TCP segment) — otherwise that first frame would be dropped.
            std::vector<uint8_t> leftover;
            auto hs_end = request_str.find("\r\n\r\n");
            if (hs_end != std::string::npos) {
                const std::size_t start = hs_end + 4;
                if (start < buffer.size()) {
                    leftover.assign(buffer.begin() + static_cast<std::ptrdiff_t>(start),
                                    buffer.end());
                }
            }
            co_await serve_websocket(std::move(sock), *ws_shared, *handshake,
                                     std::move(leftover));
            co_return;
        }

        if (http_shared) {
            co_await ::spaznet::http::serve_coroutine_keep_alive(std::move(sock), *http_shared,
                                                      std::move(buffer));
            co_return;
        }

        // Neither handler — close.
        sock.close();
    };
}

} // namespace spaznet::websocket
