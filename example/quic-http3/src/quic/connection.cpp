#include <libspaznet/quic/connection.hpp>

#include <algorithm>
#include <cstring>

#include <libspaznet/quic/varint.hpp>

namespace spaznet {
namespace quic {


namespace {

auto finalize_tp(TransportParameters tp, std::span<const uint8_t> od_cid,
                 std::span<const uint8_t> scid) -> TransportParameters {
    // The Listener pre-fills original_destination_connection_id on the
    // Retry path (where the ODCID came out of the verified token, not
    // from the current Initial's DCID).  Honor it if set; otherwise
    // default to the caller-provided od_cid.
    if (!tp.original_destination_connection_id) {
        tp.original_destination_connection_id =
            std::vector<uint8_t>(od_cid.begin(), od_cid.end());
    }
    if (!tp.initial_source_connection_id) {
        tp.initial_source_connection_id =
            std::vector<uint8_t>(scid.begin(), scid.end());
    }
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
    // Hot-path scratch buffer; reserved once and reused across every
    // build_and_send invocation. 1500 bytes covers any IPv4/IPv6 path
    // MTU we'd actually send.
    scratch_packet_.reserve(1500);

    // Pre-install Initial-level keys for both directions.
    auto& init = space(EncryptionLevel::Initial);
    init.aead = Aead::Aes128Gcm;
    auto client_init_secret = derive_initial_secret(client_dcid, Direction::Client);
    auto server_init_secret = derive_initial_secret(client_dcid, Direction::Server);
    init.recv_keys = derive_packet_keys(init.aead, client_init_secret);
    init.send_keys = derive_packet_keys(init.aead, server_init_secret);
    (void)init.send_ctx.init(init.aead, {init.send_keys.key.data(), init.send_keys.key.size()},
                             CipherCtx::Direction::Encrypt);
    (void)init.recv_ctx.init(init.aead, {init.recv_keys.key.data(), init.recv_keys.key.size()},
                             CipherCtx::Direction::Decrypt);
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
            (void)sp.recv_ctx.init(
                aead, {sp.recv_keys.key.data(), sp.recv_keys.key.size()},
                CipherCtx::Direction::Decrypt);
            sp.recv_keys_ready = true;
        }
        if (!sp.send_keys_ready && !ws.empty()) {
            sp.aead = aead;
            sp.send_keys = derive_packet_keys(aead, ws);
            (void)sp.send_ctx.init(
                aead, {sp.send_keys.key.data(), sp.send_keys.key.size()},
                CipherCtx::Direction::Encrypt);
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
    // RFC 9000 §8.1.2: counted bytes for anti-amplification are ALL
    // received UDP payload bytes — including bytes that ultimately fail
    // to decrypt.  Bump before per-packet processing so the send budget
    // expands as soon as the datagram lands.
    recv_bytes_total_ += datagram.size();
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

    // Inline header-protection removal + cached-context in-place
    // AEAD open. Matches what decrypt_long_packet does but uses the
    // per-Space CipherCtx so we skip per-packet EVP_CIPHER_CTX_new.
    if (local_hdr.pn_offset + 4 + kSampleLen > pkt.size()) {
        off = cursor;
        return cursor - packet_start;
    }
    auto mask = header_protection_mask(sp.aead,
                                       {sp.recv_keys.hp.data(), sp.recv_keys.hp.size()},
                                       {pkt.data() + local_hdr.pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x0F;
    const std::size_t pn_len = (pkt[0] & 0x03U) + 1;
    for (std::size_t i = 0; i < pn_len; ++i) pkt[local_hdr.pn_offset + i] ^= mask[1 + i];

    uint64_t trunc = 0;
    for (std::size_t i = 0; i < pn_len; ++i) {
        trunc = (trunc << 8) | pkt[local_hdr.pn_offset + i];
    }
    const uint64_t pn = decode_packet_number(sp.any_recv ? sp.largest_recv_pn : 0, trunc,
                                              pn_len * 8);
    const std::size_t header_len = local_hdr.pn_offset + pn_len;
    const std::size_t ct_len = local_hdr.length - pn_len;
    if (header_len + ct_len > pkt.size() || ct_len < aead_tag_length(sp.aead)) {
        off = cursor;
        return cursor - packet_start;
    }
    const std::size_t tag_len = aead_tag_length(sp.aead);
    const std::size_t pt_len = ct_len - tag_len;
    auto nonce = make_aead_nonce({sp.recv_keys.iv.data(), sp.recv_keys.iv.size()}, pn);
    if (!sp.recv_ctx.open_inplace({nonce.data(), nonce.size()},
                                  {pkt.data(), header_len},
                                  {pkt.data() + header_len, pt_len},
                                  {pkt.data() + header_len + pt_len, tag_len})) {
        off = cursor;
        return cursor - packet_start;
    }
    if (!sp.any_recv || pn > sp.largest_recv_pn) {
        sp.largest_recv_pn = pn;
    }
    sp.any_recv = true;
    // RFC 9000 §8.1.2: successful decryption of a Handshake-protected
    // packet proves the peer received our Initial response and thus
    // owns this address — disable the anti-amp cap.
    if (level == EncryptionLevel::Handshake) {
        peer_address_validated_ = true;
    }

    deliver_frames(level, {pkt.data() + header_len, pt_len});
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
    const std::size_t ct_len = dg.size() - header_len;
    const std::size_t tag_len = aead_tag_length(sp.aead);
    if (ct_len < tag_len) {
        return false;
    }
    const std::size_t pt_len = ct_len - tag_len;
    auto nonce = make_aead_nonce({sp.recv_keys.iv.data(), sp.recv_keys.iv.size()}, pn);
    if (!sp.recv_ctx.open_inplace({nonce.data(), nonce.size()},
                                  {dg.data(), header_len},
                                  {dg.data() + header_len, pt_len},
                                  {dg.data() + header_len + pt_len, tag_len})) {
        return false;
    }
    if (!sp.any_recv || pn > sp.largest_recv_pn) {
        sp.largest_recv_pn = pn;
    }
    sp.any_recv = true;

    deliver_frames(EncryptionLevel::Application, {dg.data() + header_len, pt_len});
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
    auto& sp = space(level);
    auto& by_pn = sent_[static_cast<std::size_t>(level)];

    // RFC 9002 §5.1: take an RTT sample iff the largest-acked PN is
    // newly acked by this frame AND that record is ack-eliciting.
    // We snapshot whether it's "new" before the ack-erase loop.
    const auto largest_it = by_pn.find(f.largest_acked);
    const bool sample_rtt =
        largest_it != by_pn.end() && largest_it->second.ack_eliciting;
    TimePoint largest_sent_at{};
    if (sample_rtt) {
        largest_sent_at = largest_it->second.sent_at;
    }
    if (!sp.any_acked_ || f.largest_acked > sp.largest_acked_pn_) {
        sp.largest_acked_pn_ = f.largest_acked;
        sp.any_acked_ = true;
    }

    auto ack_entry = [&](typename std::map<uint64_t, SentRecord>::iterator it) {
        // Notify streams of acked bytes.
        for (const auto& sf : it->second.streams) {
            if (auto* st = ensure_stream(sf.stream_id); st != nullptr) {
                st->on_acked(sf.offset, sf.data.size());
            }
        }
        congestion_.on_packets_acked(clock_fn_(), it->second.bytes);
        by_pn.erase(it);
    };
    // Process each ACK range by walking sent_'s map (via lower_bound)
    // instead of iterating every PN in the range. Wire ACK frames
    // legitimately cover wide PN ranges that are mostly already-acked-
    // and-released entries; iterating every PN was O(N) per ACK and
    // dominated steady-state throughput at large queue depths.
    auto ack_range = [&](uint64_t lo, uint64_t hi) {
        if (lo > hi) return;
        auto it = by_pn.lower_bound(lo);
        while (it != by_pn.end() && it->first <= hi) {
            // ack_entry erases; capture next first.
            auto next = std::next(it);
            ack_entry(it);
            it = next;
        }
    };
    if (f.first_range > f.largest_acked) {
        return;
    }
    uint64_t hi = f.largest_acked;
    uint64_t lo = hi - f.first_range;
    ack_range(lo, hi);
    for (const auto& r : f.ranges) {
        if (lo < r.gap + 2) break;
        hi = lo - r.gap - 2;
        if (r.length > hi) break;
        lo = hi - r.length;
        ack_range(lo, hi);
    }
    recovery_.reset_pto();

    // RFC 9002 §5.3: sample RTT now that the largest-acked record is
    // confirmed gone from in-flight (the ack-eliciting check is from
    // before the erase loop, so it still tells us whether we should
    // sample).  ack_delay on the wire is microseconds scaled by
    // 2^peer.ack_delay_exponent; decode against the peer's parameter.
    if (sample_rtt) {
        const auto now = clock_fn_();
        Duration latest = std::chrono::duration_cast<Duration>(now - largest_sent_at);
        const auto& peer_tp = tls_.peer_transport_params();
        uint64_t scaled = f.ack_delay << peer_tp.ack_delay_exponent;
        // Cap against peer's max_ack_delay to defend against a
        // pathological peer.
        const uint64_t cap_us = peer_tp.max_ack_delay_ms * 1000U;
        if (cap_us > 0 && scaled > cap_us) {
            scaled = cap_us;
        }
        Duration ack_delay = std::chrono::microseconds(scaled);
        recovery_.on_rtt_sample(latest, ack_delay);
    }

    // RFC 9002 §6.1: declare any unacked packet older than the
    // largest-acked-minus-threshold (or older than the time threshold)
    // lost, and re-queue its frames.
    detect_and_handle_loss(level);
}

auto Connection::declare_lost(SentRecord& rec, EncryptionLevel level) -> void {
    // Re-queue stream frames via the Stream's lost-ranges machinery.
    for (auto& sf : rec.streams) {
        if (auto* st = ensure_stream(sf.stream_id); st != nullptr) {
            st->on_lost(sf.offset, sf.data.size());
        }
    }
    // Re-queue CRYPTO frames into the pending list for `level`.  The
    // frame carries its original offset, so the peer will reassemble
    // by offset; no separate retransmit-offset bookkeeping needed.
    auto& pending = crypto_pending_[static_cast<std::size_t>(level)];
    for (auto& cf : rec.crypto) {
        pending.push_back(std::move(cf));
    }
    rec.crypto.clear();
    rec.streams.clear();
    if (rec.ack_eliciting) {
        congestion_.on_packets_lost(clock_fn_(), rec.bytes, rec.sent_at);
    }
}

auto Connection::detect_and_handle_loss(EncryptionLevel level) -> void {
    auto& sp = space(level);
    if (!sp.any_acked_) return;
    auto& by_pn = sent_[static_cast<std::size_t>(level)];
    if (by_pn.empty()) return;

    const auto now = clock_fn_();
    const Duration time_threshold = recovery_.loss_time_threshold();
    const TimePoint time_cutoff = now - time_threshold;

    // Iterate while erasing — capture next before declare_lost+erase.
    auto it = by_pn.begin();
    while (it != by_pn.end()) {
        const uint64_t pn = it->first;
        // Stop once we hit packets at or above largest_acked — those
        // can't be considered lost (they may not have been acked yet
        // but the threshold rules don't apply).
        if (pn >= sp.largest_acked_pn_) break;
        const bool packet_threshold =
            sp.largest_acked_pn_ - pn >= Recovery::kPacketThreshold;
        const bool time_threshold_hit = it->second.sent_at <= time_cutoff;
        if (!packet_threshold && !time_threshold_hit) {
            ++it;
            continue;
        }
        auto next = std::next(it);
        declare_lost(it->second, level);
        by_pn.erase(it);
        it = next;
    }
}

auto Connection::check_pto() -> void {
    // PTO uses the peer's max_ack_delay (in ms) per RFC 9002 §6.2.1.
    const auto& peer_tp = tls_.peer_transport_params();
    const Duration max_ack_delay =
        std::chrono::milliseconds(peer_tp.max_ack_delay_ms);
    const Duration pto = recovery_.pto_timeout(max_ack_delay);
    const auto now = clock_fn_();

    bool fired = false;
    for (auto lvl : {EncryptionLevel::Initial, EncryptionLevel::Handshake,
                     EncryptionLevel::Application}) {
        auto& sp = space(lvl);
        if (!sp.any_ack_eliciting_sent_) continue;
        auto& by_pn = sent_[static_cast<std::size_t>(lvl)];
        if (by_pn.empty()) continue;
        if (now - sp.last_ack_eliciting_send_time_ < pto) continue;

        // PTO has expired for this space.  Per RFC 9002 §6.2.4 we
        // ought to send 1-2 probe packets.  Simplest correct behavior:
        // declare every in-flight ack-eliciting record at this level
        // lost so build_and_send re-emits the contents in a fresh
        // packet.  The peer will eventually ACK whichever copy
        // arrives; spurious "loss" of a slow-but-not-lost packet
        // costs us a duplicate transmission, which is acceptable.
        fired = true;
        auto it = by_pn.begin();
        while (it != by_pn.end()) {
            if (!it->second.ack_eliciting) {
                ++it;
                continue;
            }
            auto next = std::next(it);
            declare_lost(it->second, lvl);
            by_pn.erase(it);
            it = next;
        }
        // After draining, mark "no longer any ack-eliciting in flight"
        // so we don't keep firing every tick until the retransmits
        // land.  The retransmit itself will set it back to true.
        sp.any_ack_eliciting_sent_ = false;
    }
    if (fired) {
        recovery_.on_pto_fired();
    }
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

    // RFC 9000 §8.1.2 anti-amplification: until the peer's address is
    // validated, total sent bytes MUST NOT exceed 3× total received
    // bytes.  Estimate the worst-case datagram we'd build at ~1500
    // bytes (full path MTU); if that estimate would push us over, bail
    // out without consuming any pending CRYPTO/STREAM bytes.  The send
    // will fire again on the next received datagram (which bumps the
    // budget) or the next on_timer tick.
    if (!peer_address_validated_) {
        constexpr uint64_t kMaxDatagramEstimate = 1500;
        if (sent_bytes_unvalidated_ + kMaxDatagramEstimate > 3 * recv_bytes_total_) {
            return false;
        }
    }

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

    const uint64_t pn = sp.next_send_pn++;
    // Initial path keeps the original `build_initial_packet` helper
    // because it has special padding rules and is hand-shake only
    // (one or two calls per connection). The hot path is Application
    // / Handshake, which we build directly into the reused scratch
    // buffer with in-place AEAD.
    if (level == EncryptionLevel::Initial) {
        std::vector<uint8_t> datagram = build_initial_packet(
            sp.aead, sp.send_keys, {dcid_.data(), dcid_.size()},
            {scid_.data(), scid_.size()}, {}, {payload.data(), payload.size()}, pn);
        // Track + send.
        SentRecord rec;
        rec.level = level;
        rec.ack_eliciting =
            !crypto_pending.empty() || !stream_pending.empty() || include_hs_done;
        rec.bytes = datagram.size();
        rec.sent_at = clock_fn_();
        rec.crypto = std::move(crypto_pending);
        rec.streams = std::move(stream_pending);
        const bool was_ack_eliciting = rec.ack_eliciting;
        const auto sent_at = rec.sent_at;
        const auto sent_bytes = datagram.size();
        sent_[static_cast<std::size_t>(level)].emplace(pn, std::move(rec));
        if (was_ack_eliciting) {
            congestion_.on_packet_sent(sent_at, sent_bytes);
            sp.last_ack_eliciting_send_time_ = sent_at;
            sp.any_ack_eliciting_sent_ = true;
        }
        if (!peer_address_validated_) {
            sent_bytes_unvalidated_ += sent_bytes;
        }
        send_fn_({datagram.data(), datagram.size()});
        return true;
    }

    // Application + Handshake share an "encode header into scratch,
    // append payload, append tag space, AEAD-seal in place, apply HP"
    // pattern; the only differences are the header layout and which
    // bits of the first byte are HP-masked.
    auto& buf = scratch_packet_;
    buf.clear();
    const std::size_t pn_len = 4;
    const std::size_t tag_len = aead_tag_length(sp.aead);
    std::size_t pn_offset = 0;
    uint8_t hp_first_mask = 0;

    if (level == EncryptionLevel::Handshake) {
        const uint8_t first =
            static_cast<uint8_t>(0x80 | 0x40 |
                                 (static_cast<uint8_t>(LongType::Handshake) << 4) |
                                 static_cast<uint8_t>(pn_len - 1));
        buf.push_back(first);
        buf.push_back(static_cast<uint8_t>(kQuicV1 >> 24));
        buf.push_back(static_cast<uint8_t>(kQuicV1 >> 16));
        buf.push_back(static_cast<uint8_t>(kQuicV1 >> 8));
        buf.push_back(static_cast<uint8_t>(kQuicV1));
        buf.push_back(static_cast<uint8_t>(dcid_.size()));
        buf.insert(buf.end(), dcid_.begin(), dcid_.end());
        buf.push_back(static_cast<uint8_t>(scid_.size()));
        buf.insert(buf.end(), scid_.begin(), scid_.end());
        const uint64_t length = pn_len + payload.size() + tag_len;
        VarInt::append(buf, length);
        hp_first_mask = 0x0F;
    } else {
        // Application — short header.
        const uint8_t first = static_cast<uint8_t>(0x40 | static_cast<uint8_t>(pn_len - 1));
        buf.push_back(first);
        buf.insert(buf.end(), dcid_.begin(), dcid_.end());
        hp_first_mask = 0x1F;
    }
    pn_offset = buf.size();
    for (std::size_t i = 0; i < pn_len; ++i) {
        buf.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - i))) & 0xFF));
    }
    const std::size_t header_end = buf.size();
    buf.insert(buf.end(), payload.begin(), payload.end());
    const std::size_t body_end = buf.size();
    // Make room for the AEAD auth tag at the end of the scratch buffer.
    buf.resize(body_end + tag_len);

    auto nonce = make_aead_nonce({sp.send_keys.iv.data(), sp.send_keys.iv.size()}, pn);
    if (!sp.send_ctx.seal_inplace({nonce.data(), nonce.size()},
                                  {buf.data(), header_end},
                                  {buf.data() + header_end, body_end - header_end},
                                  {buf.data() + body_end, tag_len})) {
        return false;
    }
    auto mask = header_protection_mask(
        sp.aead, {sp.send_keys.hp.data(), sp.send_keys.hp.size()},
        {buf.data() + pn_offset + 4, kSampleLen});
    buf[0] ^= mask[0] & hp_first_mask;
    for (std::size_t i = 0; i < pn_len; ++i) {
        buf[pn_offset + i] ^= mask[1 + i];
    }

    // Track for ack/loss + emit the freshly-built Application or
    // Handshake datagram from the scratch buffer.
    SentRecord rec;
    rec.level = level;
    rec.ack_eliciting = !crypto_pending.empty() || !stream_pending.empty() || include_hs_done;
    rec.bytes = buf.size();
    rec.sent_at = clock_fn_();
    rec.crypto = std::move(crypto_pending);
    rec.streams = std::move(stream_pending);
    const bool was_ack_eliciting2 = rec.ack_eliciting;
    const auto sent_at2 = rec.sent_at;
    const auto sent_bytes2 = buf.size();
    sent_[static_cast<std::size_t>(level)].emplace(pn, std::move(rec));
    if (was_ack_eliciting2) {
        congestion_.on_packet_sent(sent_at2, sent_bytes2);
        sp.last_ack_eliciting_send_time_ = sent_at2;
        sp.any_ack_eliciting_sent_ = true;
    }

    if (include_hs_done) sent_handshake_done_ = true;

    if (!peer_address_validated_) {
        sent_bytes_unvalidated_ += buf.size();
    }
    send_fn_({buf.data(), buf.size()});
    return true;
}

auto Connection::on_timer() -> void {
    install_new_keys();
    tls_.advance();
    install_new_keys();
    // RFC 9002 §6.2: PTO expiry re-queues any in-flight ack-eliciting
    // frames so the subsequent build_and_send pass re-emits them as
    // probe packets.  Check before pulling new TLS data so the probe
    // includes the actual lost-and-retransmitted payload rather than
    // freshly-arrived bytes.
    check_pto();
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
