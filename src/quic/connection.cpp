#include <libspaznet/quic/connection.hpp>

#include <algorithm>
#include <cstring>

#include <libspaznet/quic/varint.hpp>

namespace spaznet {
namespace quic {


namespace {

auto finalize_tp(TransportParameters tp, std::span<const uint8_t> od_cid,
                 std::span<const uint8_t> scid) -> TransportParameters {
    tp.original_destination_connection_id =
        std::vector<uint8_t>(od_cid.begin(), od_cid.end());
    tp.initial_source_connection_id =
        std::vector<uint8_t>(scid.begin(), scid.end());
    return tp;
}

} // namespace

Connection::Connection(std::shared_ptr<TlsContext> tls_ctx,
                       std::span<const uint8_t> client_dcid,
                       std::span<const uint8_t> client_scid,
                       std::span<const uint8_t> server_scid, TransportParameters tp,
                       SendFn send_fn, ClockFn clock_fn)
    // dcid_ holds the value we put in the DCID field on every outgoing
    // packet, which per RFC 9000 §7.2 is the peer's chosen SCID.
    : dcid_(client_scid.begin(), client_scid.end()),
      scid_(server_scid.begin(), server_scid.end()),
      original_dcid_(client_dcid.begin(), client_dcid.end()),
      send_fn_(std::move(send_fn)),
      clock_fn_(clock_fn ? std::move(clock_fn) : []() { return std::chrono::steady_clock::now(); }),
      tls_ctx_(std::move(tls_ctx)),
      // Initial-level keys are derived from the client's *original*
      // DCID (the value it put in its first Initial), not from the
      // peer's SCID — see RFC 9001 §5.2.
      tls_(tls_ctx_, client_dcid, finalize_tp(tp, client_dcid, server_scid)),
      local_tp_(finalize_tp(tp, client_dcid, server_scid)) {
    last_activity_ = clock_fn_();

    // Pre-install Initial-level keys for both directions.
    auto& init = space(EncryptionLevel::Initial);
    init.aead = Aead::Aes128Gcm;
    auto client_init_secret = derive_initial_secret(client_dcid, Direction::Client);
    auto server_init_secret = derive_initial_secret(client_dcid, Direction::Server);
    init.recv_keys = derive_packet_keys(init.aead, client_init_secret);
    init.send_keys = derive_packet_keys(init.aead, server_init_secret);
    init.recv_keys_ready = true;
    init.send_keys_ready = true;
}

auto Connection::install_new_keys() -> void {
    for (auto lvl :
         {EncryptionLevel::Handshake, EncryptionLevel::Application}) {
        auto& sp = space(lvl);
        const auto& rs = tls_.read_secret(lvl);
        const auto& ws = tls_.write_secret(lvl);
        Aead aead = tls_.negotiated_aead();
        if (!sp.recv_keys_ready && !rs.empty()) {
            sp.aead = aead;
            sp.recv_keys = derive_packet_keys(aead, rs);
            sp.recv_keys_ready = true;
        }
        if (!sp.send_keys_ready && !ws.empty()) {
            sp.aead = aead;
            sp.send_keys = derive_packet_keys(aead, ws);
            sp.send_keys_ready = true;
        }
    }
}

auto Connection::pull_crypto_from_tls() -> void {
    for (auto lvl :
         {EncryptionLevel::Initial, EncryptionLevel::Handshake, EncryptionLevel::Application}) {
        auto& buf = tls_.out_crypto(lvl);
        if (buf.empty()) continue;
        auto& sp = space(lvl);
        if (!sp.send_keys_ready) {
            // Can't ship yet; leave it pending in TLS.
            continue;
        }
        // Hand the whole buffer to a single CRYPTO frame; build_and_send
        // will fragment if needed.
        CryptoFrame cf;
        cf.offset = sp.crypto_send_offset;
        cf.data = buf;
        sp.crypto_send_offset += buf.size();
        // Stash on the Space's pending list via the SentRecord pathway:
        // we encode immediately in build_and_send. For simplicity we
        // attach to spaces_ as a pending list.
        crypto_pending_[static_cast<std::size_t>(lvl)].push_back(std::move(cf));
        buf.clear();
    }
}

auto Connection::on_datagram(std::span<const uint8_t> datagram) -> void {
    if (state_ == State::Closed || state_ == State::Draining) {
        return;
    }
    last_activity_ = clock_fn_();
    std::size_t off = 0;
    while (off < datagram.size()) {
        if ((datagram[off] & 0x80U) != 0) {
            std::size_t consumed = process_long_packet(datagram, off);
            if (consumed == 0) {
                return;
            }
        } else {
            // Short header — owning copy because decrypt mutates first
            // byte and PN bytes (header protection removal).
            std::vector<uint8_t> owned(datagram.begin() + static_cast<std::ptrdiff_t>(off),
                                       datagram.end());
            (void)process_short_packet(owned);
            return;
        }
    }
    install_new_keys();
    tls_.advance();
    install_new_keys();
    pull_crypto_from_tls();
    // Emit any outgoing packets at every level that has work to do.
    for (auto lvl :
         {EncryptionLevel::Initial, EncryptionLevel::Handshake, EncryptionLevel::Application}) {
        for (int safety = 0; safety < 64 && build_and_send(lvl); ++safety) {
        }
    }

    if (tls_.state() == TlsConnection::State::Established &&
        state_ == State::Handshaking) {
        state_ = State::Established;
    }
}

auto Connection::process_long_packet(std::span<const uint8_t> datagram, std::size_t& off)
    -> std::size_t {
    LongHeader hdr;
    std::size_t cursor = off;
    if (!parse_long_header(datagram, cursor, hdr)) {
        return 0;
    }
    EncryptionLevel level;
    switch (hdr.type) {
        case LongType::Initial:
            level = EncryptionLevel::Initial;
            break;
        case LongType::Handshake:
            level = EncryptionLevel::Handshake;
            break;
        case LongType::ZeroRtt:
            level = EncryptionLevel::EarlyData;
            break;
        case LongType::Retry:
            // Server doesn't receive Retry.
            off = cursor;
            return cursor - off;
    }
    auto& sp = space(level);
    if (!sp.recv_keys_ready) {
        off = cursor;
        return cursor - off;
    }

    // Carve the protected packet out of the datagram into an owning
    // buffer so HP/AEAD can mutate it.
    const std::size_t packet_start = off;
    const std::size_t packet_end = cursor;
    std::vector<uint8_t> pkt(datagram.begin() + static_cast<std::ptrdiff_t>(packet_start),
                             datagram.begin() + static_cast<std::ptrdiff_t>(packet_end));
    LongHeader local_hdr = hdr;
    local_hdr.pn_offset -= packet_start;

    uint64_t pn = 0;
    std::vector<uint8_t> plaintext;
    if (!decrypt_long_packet(sp.aead, sp.recv_keys, sp.any_recv ? sp.largest_recv_pn : 0, pkt,
                             local_hdr, pn, plaintext)) {
        off = cursor;
        return cursor - packet_start;
    }
    if (!sp.any_recv || pn > sp.largest_recv_pn) {
        sp.largest_recv_pn = pn;
    }
    sp.any_recv = true;

    deliver_frames(level, {plaintext.data(), plaintext.size()});
    sp.acks.on_received(pn, true);

    off = cursor;
    return cursor - packet_start;
}

auto Connection::process_short_packet(std::vector<uint8_t>& dg) -> bool {
    auto& sp = space(EncryptionLevel::Application);
    if (!sp.recv_keys_ready) {
        return false;
    }
    // Short header: first byte, then DCID (length known a priori — we
    // told the peer scid_.size()), then PN, then payload+tag.
    if (dg.size() < 1 + scid_.size() + 4 + 16) {
        return false;
    }
    const std::size_t pn_offset = 1 + scid_.size();
    if (pn_offset + 4 + kSampleLen > dg.size()) {
        return false;
    }
    auto mask = header_protection_mask(sp.aead, {sp.recv_keys.hp.data(), sp.recv_keys.hp.size()},
                                       {dg.data() + pn_offset + 4, kSampleLen});
    dg[0] ^= mask[0] & 0x1F; // short-header HP affects low 5 bits
    const std::size_t pn_len = (dg[0] & 0x03U) + 1;
    for (std::size_t i = 0; i < pn_len; ++i) {
        dg[pn_offset + i] ^= mask[1 + i];
    }
    uint64_t truncated = 0;
    for (std::size_t i = 0; i < pn_len; ++i) {
        truncated = (truncated << 8) | dg[pn_offset + i];
    }
    const uint64_t pn = decode_packet_number(sp.any_recv ? sp.largest_recv_pn : 0, truncated,
                                             pn_len * 8);

    const std::size_t header_len = pn_offset + pn_len;
    std::span<const uint8_t> aad{dg.data(), header_len};
    std::span<const uint8_t> ct{dg.data() + header_len, dg.size() - header_len};
    auto nonce = make_aead_nonce({sp.recv_keys.iv.data(), sp.recv_keys.iv.size()}, pn);
    std::vector<uint8_t> plaintext;
    if (!aead_open(sp.aead, {sp.recv_keys.key.data(), sp.recv_keys.key.size()},
                   {nonce.data(), nonce.size()}, aad, ct, plaintext)) {
        return false;
    }
    if (!sp.any_recv || pn > sp.largest_recv_pn) {
        sp.largest_recv_pn = pn;
    }
    sp.any_recv = true;

    deliver_frames(EncryptionLevel::Application, {plaintext.data(), plaintext.size()});
    sp.acks.on_received(pn, true);
    return true;
}

auto Connection::deliver_frames(EncryptionLevel level, std::span<const uint8_t> payload) -> void {
    std::vector<Frame> frames;
    if (!parse_frames(payload, frames)) {
        state_ = State::Closing;
        return;
    }
    for (auto& f : frames) {
        std::visit(
            [&](auto& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, CryptoFrame>) {
                    on_crypto_frame(level, x);
                } else if constexpr (std::is_same_v<T, StreamFrame>) {
                    on_stream_frame(x);
                } else if constexpr (std::is_same_v<T, AckFrame>) {
                    on_ack_frame(level, x);
                } else if constexpr (std::is_same_v<T, ConnectionCloseFrame>) {
                    state_ = State::Draining;
                }
                // PADDING, PING, HANDSHAKE_DONE, MAX_*, etc. — no-op for
                // this minimal first pass.
            },
            f);
    }
}

auto Connection::on_crypto_frame(EncryptionLevel level, const CryptoFrame& f) -> void {
    auto& sp = space(level);
    sp.crypto_recv_[f.offset] = f.data;
    feed_tls_crypto(level);
}

auto Connection::feed_tls_crypto(EncryptionLevel level) -> void {
    auto& sp = space(level);
    while (!sp.crypto_recv_.empty()) {
        auto it = sp.crypto_recv_.begin();
        if (it->first > sp.crypto_read_offset) {
            return; // hole
        }
        if (it->first + it->second.size() <= sp.crypto_read_offset) {
            sp.crypto_recv_.erase(it); // pure duplicate
            continue;
        }
        const std::size_t skip =
            static_cast<std::size_t>(sp.crypto_read_offset - it->first);
        std::span<const uint8_t> chunk{it->second.data() + skip, it->second.size() - skip};
        tls_.deliver_crypto(level, chunk);
        sp.crypto_read_offset += chunk.size();
        sp.crypto_recv_.erase(it);
    }
}

auto Connection::on_stream_frame(const StreamFrame& f) -> void {
    auto* s = ensure_stream(f.stream_id);
    if (s != nullptr) {
        (void)s->deliver(f.offset, f.data, f.fin);
    }
}

auto Connection::on_ack_frame(EncryptionLevel level, const AckFrame& f) -> void {
    auto& by_pn = sent_[static_cast<std::size_t>(level)];
    auto ack_one = [&](uint64_t pn) {
        auto it = by_pn.find(pn);
        if (it == by_pn.end()) return;
        // Notify streams of acked bytes.
        for (const auto& sf : it->second.streams) {
            if (auto* st = ensure_stream(sf.stream_id); st != nullptr) {
                st->on_acked(sf.offset, sf.data.size());
            }
        }
        congestion_.on_packets_acked(clock_fn_(), it->second.bytes);
        by_pn.erase(it);
    };
    if (f.first_range > f.largest_acked) {
        return;
    }
    uint64_t hi = f.largest_acked;
    uint64_t lo = hi - f.first_range;
    for (uint64_t pn = lo; pn <= hi; ++pn) ack_one(pn);
    for (const auto& r : f.ranges) {
        if (lo < r.gap + 2) break;
        hi = lo - r.gap - 2;
        if (r.length > hi) break;
        lo = hi - r.length;
        for (uint64_t pn = lo; pn <= hi; ++pn) ack_one(pn);
    }
    recovery_.reset_pto();
}

auto Connection::ensure_stream(uint64_t stream_id) -> Stream* {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) return it->second.get();
    // For bidi peer-initiated streams, our recv limit is from local TPs;
    // our send limit is from peer's TPs.
    const auto& peer = peer_transport_params();
    uint64_t recv_limit = 0;
    uint64_t send_limit = 0;
    if (is_bidi(stream_id)) {
        if (is_client_initiated(stream_id)) {
            recv_limit = local_tp_.initial_max_stream_data_bidi_remote;
            send_limit = peer.initial_max_stream_data_bidi_local;
        } else {
            recv_limit = local_tp_.initial_max_stream_data_bidi_local;
            send_limit = peer.initial_max_stream_data_bidi_remote;
        }
    } else {
        if (is_client_initiated(stream_id)) {
            recv_limit = local_tp_.initial_max_stream_data_uni;
            send_limit = 0;
        } else {
            recv_limit = 0;
            send_limit = peer.initial_max_stream_data_uni;
        }
    }
    auto stream = std::make_unique<Stream>(stream_id, recv_limit, send_limit);
    auto* raw = stream.get();
    streams_[stream_id] = std::move(stream);
    return raw;
}

auto Connection::write_stream(uint64_t stream_id, std::span<const uint8_t> data, bool fin)
    -> void {
    auto* s = ensure_stream(stream_id);
    std::vector<uint8_t> bytes(data.begin(), data.end());
    s->write(bytes, fin);
}

auto Connection::read_stream(uint64_t stream_id, std::vector<uint8_t>& out, bool& fin_out)
    -> std::size_t {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        fin_out = false;
        return 0;
    }
    return it->second->read_contiguous(out, fin_out);
}

namespace {

// Build the inside-protection plaintext payload for an outgoing packet.
auto build_payload(std::span<const CryptoFrame> crypto_frames,
                   std::span<const StreamFrame> stream_frames, bool include_ack,
                   const AckFrame& ack, bool include_hs_done, bool pad_to,
                   std::vector<uint8_t>& out) -> void {
    out.clear();
    if (include_ack) {
        encode_frame(out, Frame{ack});
    }
    for (const auto& cf : crypto_frames) {
        encode_frame(out, Frame{cf});
    }
    for (const auto& sf : stream_frames) {
        encode_frame(out, Frame{sf});
    }
    if (include_hs_done) {
        encode_frame(out, Frame{HandshakeDoneFrame{}});
    }
    if (pad_to && out.size() < 1) {
        encode_frame(out, Frame{PingFrame{}});
    }
}

} // namespace

auto Connection::build_and_send(EncryptionLevel level) -> bool {
    auto& sp = space(level);
    if (!sp.send_keys_ready) return false;

    // 1. Collect pending CRYPTO frames for this level.
    std::vector<CryptoFrame> crypto_pending;
    crypto_pending.swap(crypto_pending_[static_cast<std::size_t>(level)]);

    // 2. Collect STREAM frames (1-RTT only).
    std::vector<StreamFrame> stream_pending;
    if (level == EncryptionLevel::Application) {
        for (auto& [id, st] : streams_) {
            std::size_t budget = 1100; // leave room for header/tag/ACK
            while (budget > 0) {
                uint64_t off = 0;
                std::vector<uint8_t> data;
                bool fin = false;
                std::size_t n = st->pull_send(budget, off, data, fin);
                if (n == 0 && !fin) break;
                StreamFrame sf;
                sf.stream_id = id;
                sf.offset = off;
                sf.has_length = true;
                sf.fin = fin;
                sf.data = std::move(data);
                stream_pending.push_back(std::move(sf));
                if (n == 0) break; // FIN-only chunk
                budget -= n;
            }
        }
    }

    // 3. ACK frame, if we owe one.
    bool include_ack = sp.acks.needs_ack();
    AckFrame ack;
    if (include_ack) {
        sp.acks.build_ack_frame(0, ack);
    }

    // 4. HANDSHAKE_DONE — server emits once after handshake completes.
    bool include_hs_done = (level == EncryptionLevel::Application &&
                            tls_.state() == TlsConnection::State::Established &&
                            !sent_handshake_done_);

    if (crypto_pending.empty() && stream_pending.empty() && !include_ack && !include_hs_done) {
        return false;
    }

    std::vector<uint8_t> payload;
    build_payload({crypto_pending.data(), crypto_pending.size()},
                  {stream_pending.data(), stream_pending.size()}, include_ack, ack,
                  include_hs_done, false, payload);

    std::vector<uint8_t> datagram;
    const uint64_t pn = sp.next_send_pn++;
    if (level == EncryptionLevel::Initial) {
        // Pad Initial to 1200 if it contains a CRYPTO frame and we are
        // the server's first response (RFC 9000 §14.1 applies to client;
        // server side: we just emit whatever we have).
        datagram =
            build_initial_packet(sp.aead, sp.send_keys, {dcid_.data(), dcid_.size()},
                                 {scid_.data(), scid_.size()}, {}, {payload.data(), payload.size()},
                                 pn);
    } else {
        // Handshake or Application long/short header. For this first
        // pass we build a Handshake packet as a minimal long header,
        // and Application as short header.
        if (level == EncryptionLevel::Handshake) {
            // Hand-build a Handshake long header.
            const std::size_t pn_len = 4;
            const std::size_t tag_len = aead_tag_length(sp.aead);
            datagram.reserve(64 + payload.size() + tag_len);
            uint8_t first = static_cast<uint8_t>(0x80 | 0x40 |
                                                 (static_cast<uint8_t>(LongType::Handshake) << 4) |
                                                 static_cast<uint8_t>(pn_len - 1));
            datagram.push_back(first);
            datagram.push_back(static_cast<uint8_t>(kQuicV1 >> 24));
            datagram.push_back(static_cast<uint8_t>(kQuicV1 >> 16));
            datagram.push_back(static_cast<uint8_t>(kQuicV1 >> 8));
            datagram.push_back(static_cast<uint8_t>(kQuicV1));
            datagram.push_back(static_cast<uint8_t>(dcid_.size()));
            datagram.insert(datagram.end(), dcid_.begin(), dcid_.end());
            datagram.push_back(static_cast<uint8_t>(scid_.size()));
            datagram.insert(datagram.end(), scid_.begin(), scid_.end());
            // Length (varint) = pn_len + payload + tag.
            const uint64_t length = pn_len + payload.size() + tag_len;
            VarInt::append(datagram, length);
            const std::size_t pn_offset = datagram.size();
            for (std::size_t i = 0; i < pn_len; ++i) {
                datagram.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - i))) & 0xFF));
            }
            const std::size_t payload_off = datagram.size();
            datagram.insert(datagram.end(), payload.begin(), payload.end());
            std::vector<uint8_t> aad(datagram.begin(), datagram.begin() + payload_off);
            std::vector<uint8_t> plaintext(datagram.begin() + payload_off, datagram.end());
            auto nonce = make_aead_nonce({sp.send_keys.iv.data(), sp.send_keys.iv.size()}, pn);
            std::vector<uint8_t> sealed;
            aead_seal(sp.aead, {sp.send_keys.key.data(), sp.send_keys.key.size()},
                      {nonce.data(), nonce.size()}, {aad.data(), aad.size()},
                      {plaintext.data(), plaintext.size()}, sealed);
            datagram.resize(payload_off);
            datagram.insert(datagram.end(), sealed.begin(), sealed.end());
            auto mask = header_protection_mask(
                sp.aead, {sp.send_keys.hp.data(), sp.send_keys.hp.size()},
                {datagram.data() + pn_offset + 4, kSampleLen});
            datagram[0] ^= mask[0] & 0x0F;
            for (std::size_t i = 0; i < pn_len; ++i) {
                datagram[pn_offset + i] ^= mask[1 + i];
            }
        } else {
            // Application — short header.
            const std::size_t pn_len = 4;
            const std::size_t tag_len = aead_tag_length(sp.aead);
            datagram.reserve(1 + dcid_.size() + pn_len + payload.size() + tag_len);
            // First byte: 01 (Header Form=0, Fixed Bit=1), Spin=0,
            // reserved bits 4-3=0, Key Phase=0, PN length-1 in low 2 bits.
            uint8_t first = static_cast<uint8_t>(0x40 | static_cast<uint8_t>(pn_len - 1));
            datagram.push_back(first);
            datagram.insert(datagram.end(), dcid_.begin(), dcid_.end());
            const std::size_t pn_offset = datagram.size();
            for (std::size_t i = 0; i < pn_len; ++i) {
                datagram.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - i))) & 0xFF));
            }
            const std::size_t payload_off = datagram.size();
            datagram.insert(datagram.end(), payload.begin(), payload.end());
            std::vector<uint8_t> aad(datagram.begin(), datagram.begin() + payload_off);
            std::vector<uint8_t> plaintext(datagram.begin() + payload_off, datagram.end());
            auto nonce = make_aead_nonce({sp.send_keys.iv.data(), sp.send_keys.iv.size()}, pn);
            std::vector<uint8_t> sealed;
            aead_seal(sp.aead, {sp.send_keys.key.data(), sp.send_keys.key.size()},
                      {nonce.data(), nonce.size()}, {aad.data(), aad.size()},
                      {plaintext.data(), plaintext.size()}, sealed);
            datagram.resize(payload_off);
            datagram.insert(datagram.end(), sealed.begin(), sealed.end());
            auto mask = header_protection_mask(
                sp.aead, {sp.send_keys.hp.data(), sp.send_keys.hp.size()},
                {datagram.data() + pn_offset + 4, kSampleLen});
            datagram[0] ^= mask[0] & 0x1F;
            for (std::size_t i = 0; i < pn_len; ++i) {
                datagram[pn_offset + i] ^= mask[1 + i];
            }
        }
    }

    // Track for ack/loss.
    SentRecord rec;
    rec.level = level;
    rec.ack_eliciting = !crypto_pending.empty() || !stream_pending.empty() || include_hs_done;
    rec.bytes = datagram.size();
    rec.sent_at = clock_fn_();
    rec.crypto = std::move(crypto_pending);
    rec.streams = std::move(stream_pending);
    sent_[static_cast<std::size_t>(level)].emplace(pn, std::move(rec));
    if (rec.ack_eliciting) {
        congestion_.on_packet_sent(rec.sent_at, datagram.size());
    }

    if (include_hs_done) sent_handshake_done_ = true;

    send_fn_({datagram.data(), datagram.size()});
    return true;
}

auto Connection::on_timer() -> void {
    install_new_keys();
    tls_.advance();
    install_new_keys();
    pull_crypto_from_tls();
    for (auto lvl :
         {EncryptionLevel::Initial, EncryptionLevel::Handshake, EncryptionLevel::Application}) {
        while (build_and_send(lvl)) {
        }
    }
    if (tls_.state() == TlsConnection::State::Established && state_ == State::Handshaking) {
        state_ = State::Established;
    }
}

} // namespace quic
} // namespace spaznet
