// Combined HTTP/1.1 + WebSocket reactor dispatcher: the coroutine-free
// counterpart of dispatcher.cpp's make_coroutine_dispatcher/serve_websocket. Same
// upgrade-sniffing rules, same handshake computation (handshake.hpp,
// shared with the coroutine dispatcher), same on-the-wire frame format
// (handler.cpp's Frame::serialize/parse, unchanged) -- only the
// execution model differs.
//
// Phase state machine (WsConnection):
//   Sniffing -> accumulating bytes via on_data(), looking for a complete
//     "\r\n\r\n"-terminated header block to decide HTTP vs. WS upgrade.
//     Once decided: either hand off to http::attach_reactor_dispatcher
//     (plain HTTP -- example/http's Http1Connection takes over this
//     connection's callbacks entirely from here) or send the 101
//     response and fall into the WS frame loop below.
//   ReadingHeader -> accumulating the 2-14 byte frame header (base 2
//     bytes, then possibly a 2- or 8-byte extended length once the
//     length code is known, then a 4-byte mask key).
//   ReadingPayload -> accumulating exactly the number of bytes the just-
//     parsed header declared, then unmasking and dispatching.
// There's no separate "Closing" phase in the enum for the same reason
// Http1Connection doesn't have one: closing is a terminal action
// (close()/close_after_flush()), not a state machine waits in.
//
// Sending: reactor::Connection::send() (reactor_handler.hpp) writes
// straight into the BufferedConnection's OutputBuffer, which already
// serializes writes by construction -- there is no WriteGate on this
// side, and no analogue needed (see docs/concurrency-and-coroutines.md).
// Control frames the dispatcher itself originates (Pong, Close) go
// through the same BufferedConnection::write(), via a small local helper
// below.

#include "handshake.hpp"

#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/io_context.hpp>
#include <libspaznet/reactor/buffered_connection.hpp>
#include <libspaznet/server.hpp>
#include <libspaznet/websocket/dispatcher.hpp>
#include <libspaznet/websocket/handler.hpp>
#include <libspaznet/websocket/reactor_handler.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace spaznet::websocket {

using detail::HandshakeRequest;
using detail::compute_accept;
using detail::is_upgrade;
using detail::parse_handshake;

namespace {

bool opcode_known(Opcode op) {
    switch (op) {
        case Opcode::Continuation:
        case Opcode::Text:
        case Opcode::Binary:
        case Opcode::Close:
        case Opcode::Ping:
        case Opcode::Pong:
            return true;
    }
    return false;
}

bool is_control_opcode(Opcode op) {
    return op == Opcode::Close || op == Opcode::Ping || op == Opcode::Pong;
}

// Dispatcher-originated control frame (Pong reply, Close ack/failure).
// Same unmasked-frame shape as reactor::Connection::send(); duplicated
// rather than shared because this one also needs the close-code-prefix
// special case, which application code never needs.
void send_frame(::spaznet::BufferedConnection& conn, Opcode opcode,
                std::span<const std::uint8_t> payload, uint16_t close_code = 0) {
    Frame frame;
    frame.fin = true;
    frame.opcode = opcode;
    frame.masked = false;
    if (opcode == Opcode::Close && close_code != 0) {
        frame.payload.reserve(2 + payload.size());
        frame.payload.push_back(static_cast<std::uint8_t>((close_code >> 8) & 0xFF));
        frame.payload.push_back(static_cast<std::uint8_t>(close_code & 0xFF));
        frame.payload.insert(frame.payload.end(), payload.begin(), payload.end());
    } else {
        frame.payload.assign(payload.begin(), payload.end());
    }
    frame.payload_length = frame.payload.size();
    conn.write(frame.serialize());
}

class WsConnection : public std::enable_shared_from_this<WsConnection> {
  public:
    WsConnection(::spaznet::IOContext& ctx, std::shared_ptr<::spaznet::BufferedConnection> conn,
                std::shared_ptr<::spaznet::http::HTTPHandler> http_handler,
                std::shared_ptr<reactor::Handler> ws_handler)
        : ctx_(ctx), conn_(conn), http_handler_(std::move(http_handler)),
          ws_handler_(std::move(ws_handler)) {}

    // Wires this dispatcher onto `conn` and starts the read loop. Call
    // exactly once, immediately after construction. Mirrors
    // Http1Connection::start's weak/strong split (example/http's
    // dispatcher_reactor.cpp): conn_ is a weak_ptr here, while conn's own
    // on_data/on_closed callbacks hold a strong shared_ptr<WsConnection>,
    // avoiding a reference cycle.
    void start(std::function<void()> on_closed) {
        notify_closed_ = std::move(on_closed);
        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        auto self = shared_from_this();
        conn->set_on_data([self] { self->on_data(); });
        conn->set_on_closed([self] { self->on_closed(); });
        conn->start();
    }

  private:
    enum class Phase : std::uint8_t { Sniffing, ReadingHeader, ReadingPayload };
    static constexpr std::size_t kMaxHandshakeBytes = 8192;

    void on_data() {
        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        auto data = conn->input().data();
        buffer_.insert(buffer_.end(), data.begin(), data.end());
        conn->input().consume(data.size());

        if (closing_ || handed_off_) {
            return;
        }
        process();
    }

    // Fires exactly once: orderly EOF, a hard error, or our own
    // close()/close_after_flush() completing. Not fired at all once
    // hand_off_to_http() has run -- from that point on, Http1Connection
    // owns this BufferedConnection's callbacks (including on_closed) and
    // its own `on_closed` chain fires the same `notify_closed_` we would
    // have.
    void on_closed() {
        closing_ = true;
        if (ws_open_) {
            ws_open_ = false;
            if (ws_handler_) {
                reactor::Connection view(conn_, fd_, ctx_);
                ws_handler_->on_close(view);
            }
        }
        if (notify_closed_) {
            auto cb = std::move(notify_closed_);
            notify_closed_ = nullptr;
            cb();
        }
    }

    // Drains as much of `buffer_` as currently possible, one step (one
    // handshake, one frame header, one payload) per iteration, stopping
    // as soon as a step can't complete with what's buffered so far (it
    // waits for the next on_data()) or the connection has been handed off
    // / is closing.
    void process() {
        while (!closing_ && !handed_off_) {
            bool progressed = false;
            switch (phase_) {
                case Phase::Sniffing:
                    progressed = try_sniff();
                    break;
                case Phase::ReadingHeader:
                    progressed = try_read_header();
                    break;
                case Phase::ReadingPayload:
                    progressed = try_read_payload();
                    break;
            }
            if (!progressed) {
                return;
            }
        }
    }

    bool try_sniff() {
        auto conn = conn_.lock();
        if (!conn) {
            closing_ = true;
            return false;
        }

        std::string request_str(buffer_.begin(), buffer_.end());
        auto hs_end = request_str.find("\r\n\r\n");
        if (hs_end == std::string::npos) {
            if (buffer_.size() >= kMaxHandshakeBytes) {
                // Mirrors make_coroutine_dispatcher: an over-long handshake attempt
                // just falls through to the HTTP path below (whatever
                // the HTTPHandler makes of the still-incomplete buffer is
                // its business), rather than a dedicated WS-layer error.
                hand_off_to_http();
                return false;
            }
            return false; // wait for more bytes.
        }

        auto handshake = ws_handler_ ? parse_handshake(request_str) : std::nullopt;
        if (handshake && is_upgrade(*handshake) && ws_handler_) {
            // Preserve any bytes pipelined after the upgrade request's
            // header terminator (e.g. an immediate WS frame in the same
            // TCP segment) -- otherwise that first frame would be lost.
            const std::size_t start = hs_end + 4;
            std::vector<std::uint8_t> leftover;
            if (start < buffer_.size()) {
                leftover.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(start),
                                buffer_.end());
            }
            complete_ws_handshake(*conn, *handshake, std::move(leftover));
            return true;
        }

        hand_off_to_http();
        return false;
    }

    // Hands this connection's remaining lifetime to example/http's
    // reactor dispatcher, seeded with whatever we've already buffered.
    // From this call onward BufferedConnection's on_data/on_closed
    // belong to the Http1Connection it creates; this WsConnection has
    // nothing left to do and is dropped once its own shared_ptr refs
    // (held only by the callbacks we're about to overwrite) go away.
    void hand_off_to_http() {
        auto conn = conn_.lock();
        if (!conn) {
            closing_ = true;
            return;
        }
        handed_off_ = true;
        if (!http_handler_) {
            closing_ = true;
            conn->close();
            return;
        }
        auto initial = std::move(buffer_);
        buffer_.clear();
        auto notify = std::move(notify_closed_);
        notify_closed_ = nullptr;
        ::spaznet::http::attach_reactor_dispatcher(ctx_, conn, http_handler_, std::move(initial),
                                                   std::move(notify));
    }

    void complete_ws_handshake(::spaznet::BufferedConnection& conn, const HandshakeRequest& req,
                               std::vector<std::uint8_t> leftover) {
        fd_ = conn.fd();
        std::string accept_key = compute_accept(req.headers.at("sec-websocket-key"));
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " +
                           accept_key + "\r\n\r\n";
        conn.write({resp.begin(), resp.end()});

        ws_open_ = true;
        buffer_ = std::move(leftover);
        phase_ = Phase::ReadingHeader;

        reactor::Connection view(conn_, fd_, ctx_);
        ws_handler_->on_open(view);
    }

    bool try_read_header() {
        if (buffer_.size() < 2) {
            return false;
        }
        const std::uint8_t payload_len_byte = buffer_[1] & 0x7F;
        std::size_t base_header_size = 2;
        if (payload_len_byte == 126) {
            base_header_size = 4;
        } else if (payload_len_byte == 127) {
            base_header_size = 10;
        }
        const bool masked = (buffer_[1] & 0x80) != 0;
        const std::size_t full_header_size = base_header_size + (masked ? 4 : 0);
        if (buffer_.size() < full_header_size) {
            return false;
        }

        const bool fin = (buffer_[0] & 0x80) != 0;
        const bool rsv1 = (buffer_[0] & 0x40) != 0;
        const bool rsv2 = (buffer_[0] & 0x20) != 0;
        const bool rsv3 = (buffer_[0] & 0x10) != 0;
        const auto opcode = static_cast<Opcode>(buffer_[0] & 0x0F);

        if (rsv1 || rsv2 || rsv3) {
            fail_close(1002);
            return false;
        }
        if (!masked) {
            fail_close(1002);
            return false;
        }
        if (!opcode_known(opcode)) {
            fail_close(1002);
            return false;
        }

        uint64_t payload_len = payload_len_byte;
        if (base_header_size == 4) {
            payload_len = (static_cast<uint64_t>(buffer_[2]) << 8) | buffer_[3];
            if (payload_len < 126) {
                fail_close(1002);
                return false;
            }
        } else if (base_header_size == 10) {
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | buffer_[2 + i];
            }
            if (payload_len & (1ULL << 63)) {
                fail_close(1002);
                return false;
            }
            if (payload_len < 65536) {
                fail_close(1002);
                return false;
            }
        }

        const bool is_control = is_control_opcode(opcode);
        // RFC 6455 §5.5: control frames MUST NOT be fragmented and carry
        // at most 125 bytes. Reject before reading the payload so an
        // oversized/fragmented control frame can't make us buffer and
        // XOR up to kMaxPayloadBytes of attacker data.
        if (is_control && (!fin || payload_len > 125)) {
            fail_close(1002);
            return false;
        }
        if (payload_len > Frame::kMaxPayloadBytes) {
            fail_close(1009);
            return false;
        }

        const std::size_t mask_off = base_header_size;
        pending_masking_key_ = (static_cast<uint32_t>(buffer_[mask_off]) << 24) |
                               (static_cast<uint32_t>(buffer_[mask_off + 1]) << 16) |
                               (static_cast<uint32_t>(buffer_[mask_off + 2]) << 8) |
                               static_cast<uint32_t>(buffer_[mask_off + 3]);
        pending_fin_ = fin;
        pending_opcode_ = opcode;
        pending_payload_len_ = payload_len;

        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(full_header_size));
        phase_ = Phase::ReadingPayload;
        return true;
    }

    bool try_read_payload() {
        if (buffer_.size() < pending_payload_len_) {
            return false;
        }
        const auto n = static_cast<std::ptrdiff_t>(pending_payload_len_);
        std::vector<std::uint8_t> payload(buffer_.begin(), buffer_.begin() + n);
        buffer_.erase(buffer_.begin(), buffer_.begin() + n);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] ^= static_cast<std::uint8_t>(
                (pending_masking_key_ >> ((3 - (i % 4)) * 8)) & 0xFF);
        }
        phase_ = Phase::ReadingHeader;
        return handle_frame(pending_opcode_, pending_fin_, std::move(payload));
    }

    // Returns whether process()'s loop should keep going (false either
    // means "wait for more input" would be wrong here -- it always
    // means "stop for now", e.g. because we just closed).
    bool handle_frame(Opcode opcode, bool fin, std::vector<std::uint8_t> payload) {
        auto conn = conn_.lock();
        if (!conn) {
            closing_ = true;
            return false;
        }

        if (is_control_opcode(opcode)) {
            if (opcode == Opcode::Close) {
                if (!sent_close_) {
                    sent_close_ = true;
                    send_frame(*conn, Opcode::Close, payload);
                }
                closing_ = true;
                conn->close_after_flush();
                return false;
            }
            if (opcode == Opcode::Ping) {
                send_frame(*conn, Opcode::Pong, payload);
                return true;
            }
            return true; // Pong: nothing to do.
        }

        if (opcode != Opcode::Continuation) {
            if (fragmented_) {
                fail_close(1002);
                return false;
            }
            current_message_opcode_ = opcode;
            message_buffer_ = std::move(payload);
            fragmented_ = !fin;
        } else {
            if (!fragmented_) {
                fail_close(1002);
                return false;
            }
            // Bound the reassembled message the same way as the
            // coroutine dispatcher: kMaxPayloadBytes alone only caps a
            // single frame; without this a peer streaming unlimited
            // fin=0 continuation frames could grow message_buffer_
            // without bound.
            if (message_buffer_.size() + payload.size() > Frame::kMaxPayloadBytes) {
                fail_close(1009);
                return false;
            }
            message_buffer_.insert(message_buffer_.end(), payload.begin(), payload.end());
        }

        if (fin) {
            fragmented_ = false;
            Message msg;
            msg.opcode = current_message_opcode_;
            msg.data = std::move(message_buffer_);
            message_buffer_.clear();
            if (ws_handler_) {
                reactor::Connection view(conn_, fd_, ctx_);
                ws_handler_->handle_message(std::move(msg), view);
            }
        }
        return !closing_;
    }

    void fail_close(uint16_t code) {
        closing_ = true;
        auto conn = conn_.lock();
        if (!conn) {
            return;
        }
        if (!sent_close_) {
            sent_close_ = true;
            send_frame(*conn, Opcode::Close, {}, code);
        }
        conn->close_after_flush();
    }

    ::spaznet::IOContext& ctx_;
    std::weak_ptr<::spaznet::BufferedConnection> conn_;
    std::shared_ptr<::spaznet::http::HTTPHandler> http_handler_;
    std::shared_ptr<reactor::Handler> ws_handler_;
    int fd_{-1};

    std::vector<std::uint8_t> buffer_;
    Phase phase_ = Phase::Sniffing;
    bool closing_ = false;
    bool handed_off_ = false;
    bool ws_open_ = false;
    bool sent_close_ = false;

    bool pending_fin_ = false;
    Opcode pending_opcode_ = Opcode::Continuation;
    uint32_t pending_masking_key_ = 0;
    uint64_t pending_payload_len_ = 0;

    std::vector<std::uint8_t> message_buffer_;
    Opcode current_message_opcode_ = Opcode::Continuation;
    bool fragmented_ = false;

    std::function<void()> notify_closed_;
};

} // namespace

auto make_reactor_dispatcher(std::unique_ptr<::spaznet::http::HTTPHandler> http_handler,
                             std::unique_ptr<reactor::Handler> ws_handler)
    -> ::spaznet::ReactorConnectionFactory {
    std::shared_ptr<::spaznet::http::HTTPHandler> http_shared(http_handler.release());
    std::shared_ptr<reactor::Handler> ws_shared(ws_handler.release());

    return [http_shared, ws_shared](int fd, ::spaznet::IOContext& ctx,
                                    std::function<void()> on_closed)
               -> std::shared_ptr<::spaznet::IoHandler> {
        auto conn = std::make_shared<::spaznet::BufferedConnection>(ctx, fd);
        auto dispatcher =
            std::make_shared<WsConnection>(ctx, conn, http_shared, ws_shared);
        dispatcher->start(std::move(on_closed));
        return conn;
    };
}

} // namespace spaznet::websocket
