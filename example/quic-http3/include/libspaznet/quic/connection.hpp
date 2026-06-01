#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <libspaznet/quic/congestion.hpp>
#include <libspaznet/quic/crypto.hpp>
#include <libspaznet/quic/frame.hpp>
#include <libspaznet/quic/packet.hpp>
#include <libspaznet/quic/pn_space.hpp>
#include <libspaznet/quic/recovery.hpp>
#include <libspaznet/quic/stream.hpp>
#include <libspaznet/quic/tls.hpp>
#include <libspaznet/quic/transport_params.hpp>

namespace spaznet {
namespace quic {

// QUIC connection state machine. Drives the TLS handshake through CRYPTO
// frames in the appropriate PN spaces, manages per-direction keys, and
// (post-handshake) carries STREAM/MAX_*/etc. between application code and
// the peer. Server-side: the user-facing class for a single peer.
//
// All wire I/O is mediated by two callables supplied at construction:
//   - `send_datagram` is called every time we want to put bytes on the
//     wire. The body is a protected QUIC datagram ready for UDP send.
//   - `now` is the time source used by the recovery / congestion math.
//     Defaults to std::chrono::steady_clock::now.
//
// The caller is expected to:
//   - call on_datagram(bytes) for every UDP datagram received,
//   - call on_timer() periodically (or at next_timer_at()),
//   - call write_stream(...) to push data on a stream.
class Connection {
  public:
    enum class Role : uint8_t { Server };

    enum class State : uint8_t {
        Handshaking,
        Established,
        Closing,
        Draining,
        Closed,
    };

    using SendFn = std::function<void(std::span<const uint8_t>)>;
    using ClockFn = std::function<TimePoint()>;

    // Server constructor. `tls_ctx` is shared across connections;
    // `client_dcid` is the destination CID the client used in its first
    // Initial — we use it to derive Initial-level keys. `client_scid`
    // is the client's chosen source CID (extracted from the same first
    // Initial); per RFC 9000 §7.2 it becomes the DCID for all server
    // packets sent back to the client. `tp` is the server's transport
    // parameters (we'll set original_destination_connection_id and
    // initial_source_connection_id automatically).
    Connection(std::shared_ptr<TlsContext> tls_ctx, std::span<const uint8_t> client_dcid,
               std::span<const uint8_t> client_scid, std::span<const uint8_t> server_scid,
               TransportParameters tp, SendFn send_fn, ClockFn clock_fn = nullptr);

    Connection(const Connection&) = delete;
    auto operator=(const Connection&) -> Connection& = delete;
    Connection(Connection&&) = delete;
    auto operator=(Connection&&) -> Connection& = delete;
    ~Connection() = default;

    // Feed one received UDP datagram into the engine.
    auto on_datagram(std::span<const uint8_t> datagram) -> void;

    // Pump time-based machinery (loss detection, PTO, idle timeout,
    // and any pending sends).
    auto on_timer() -> void;

    // Application write to a bidirectional stream we're tracking. If the
    // stream doesn't yet exist on our side we create it.
    auto write_stream(uint64_t stream_id, std::span<const uint8_t> data, bool fin) -> void;

    // Application read from a stream. Returns bytes copied into `out`
    // and sets `fin_out` to true when the stream's FIN has been read.
    auto read_stream(uint64_t stream_id, std::vector<uint8_t>& out, bool& fin_out)
        -> std::size_t;

    // Iterate streams currently known on this connection.
    template <typename F>
    auto for_each_stream(F&& fn) -> void {
        for (auto& [id, st] : streams_) {
            fn(id, *st);
        }
    }

    // Mark the peer's address as validated.  RFC 9000 §8.1.2: this
    // disables the 3× anti-amplification cap on outbound bytes.  Called
    // by Listener after a Retry token round-trips successfully; the
    // path is otherwise validated automatically the first time a
    // Handshake-protected packet from the peer decrypts.
    auto mark_peer_address_validated() -> void {
        peer_address_validated_ = true;
    }

    // RFC 9000 §10.2 — proactively close the connection with a
    // CONNECTION_CLOSE frame.  `application=true` selects the 0x1d
    // (application-error) variant, otherwise 0x1c (transport).
    // `frame_type` is the offending frame type for transport closes
    // and ignored for application closes.  Subsequent send attempts
    // become no-ops; the next on_timer (or on_datagram) will emit
    // the close to the peer.  Safe to call from anywhere on the
    // owning thread; first close wins, subsequent calls are no-ops.
    auto close_with_error(uint64_t error_code, bool application,
                          std::string reason = "", uint64_t frame_type = 0) -> void {
        initiate_close(error_code, application, std::move(reason), frame_type);
    }
    [[nodiscard]] auto peer_address_validated() const -> bool {
        return peer_address_validated_;
    }

    // RFC 9001 §6 — locally initiate a 1-RTT key update.  Rotates
    // our send-side application keys to the next phase: subsequent
    // outbound packets are encrypted with the newly-derived keys
    // and carry KEY_PHASE = !current.  The receive side stays on
    // the current phase until a packet from the peer arrives at
    // the toggled KEY_PHASE — at which point we commit the recv-
    // side update too.  No-op if Application keys aren't installed
    // yet or if `next_keys_ready` is false (a previous update is
    // still in flight).  Returns true if the update was started.
    auto initiate_key_update() -> bool;

    // Test / observability hook: which 1-RTT key phase is our
    // current send direction on?  0 immediately after handshake;
    // toggles to 1 after the first key update completes.
    [[nodiscard]] auto send_key_phase() const -> uint8_t {
        return spaces_[static_cast<std::size_t>(EncryptionLevel::Application)]
            .send_key_phase;
    }
    [[nodiscard]] auto recv_key_phase() const -> uint8_t {
        return spaces_[static_cast<std::size_t>(EncryptionLevel::Application)]
            .recv_key_phase;
    }

    // ---- Accessors -----------------------------------------------------
    [[nodiscard]] auto state() const -> State {
        return state_;
    }
    [[nodiscard]] auto handshake_complete() const -> bool {
        return state_ != State::Handshaking;
    }
    [[nodiscard]] auto tls() -> TlsConnection& {
        return tls_;
    }
    [[nodiscard]] auto peer_transport_params() const -> const TransportParameters& {
        return tls_.peer_transport_params();
    }
    [[nodiscard]] auto destination_connection_id() const -> const std::vector<uint8_t>& {
        return dcid_;
    }
    [[nodiscard]] auto source_connection_id() const -> const std::vector<uint8_t>& {
        return scid_;
    }

  private:
    // For lightweight in-flight tracking: per outgoing PN, the frames
    // we'd need to (selectively) replay on loss. We don't replay ACK or
    // PADDING; CRYPTO and STREAM frames are the main retransmit targets.
    struct SentRecord {
        EncryptionLevel level;
        bool ack_eliciting;
        std::size_t bytes;
        TimePoint sent_at;
        std::vector<CryptoFrame> crypto;
        std::vector<StreamFrame> streams;
    };

    // Per-PN-space derived keys + ACK bookkeeping.
    struct Space {
        Aead aead{Aead::Aes128Gcm};
        PacketKeys send_keys{};
        PacketKeys recv_keys{};
        // Cached AEAD contexts. Allocated once when keys are installed
        // and reused for every packet at this level — only the IV gets
        // reset per call. Avoids the per-packet EVP_CIPHER_CTX_new +
        // EVP_EncryptInit_ex(cipher, key) + free that dominated the
        // wallclock cost of the previous in-place-AEAD attempt.
        CipherCtx send_ctx{};
        CipherCtx recv_ctx{};
        bool send_keys_ready{false};
        bool recv_keys_ready{false};
        // Largest PN we've decrypted (for PN reconstruction).
        uint64_t largest_recv_pn{0};
        bool any_recv{false};
        // Next outgoing PN.
        uint64_t next_send_pn{0};
        PnSpace acks{};
        // CRYPTO frame reassembly buffer for this space, keyed by offset.
        std::map<uint64_t, std::vector<uint8_t>> crypto_recv_;
        uint64_t crypto_read_offset{0};
        uint64_t crypto_send_offset{0};
        // RFC 9002 loss + PTO bookkeeping.  We record the most-recent
        // ack-eliciting send so on_timer can compare against now +
        // pto_timeout, and the largest PN we've seen acked so on_ack
        // can run the packet-threshold lost-packet rule.
        TimePoint last_ack_eliciting_send_time_{};
        bool any_ack_eliciting_sent_{false};
        uint64_t largest_acked_pn_{0};
        bool any_acked_{false};

        // RFC 9001 §6 — 1-RTT key update.  Used at
        // EncryptionLevel::Application only.  When `next_keys_ready`
        // is true we hold the "phase + 1" keys precomputed so an
        // inbound packet with the toggled KEY_PHASE bit can be
        // decrypted in one pass; on success we install the new
        // packet keys + secrets into the `send_*` / `recv_*` slots,
        // ratchet the local phase, and derive a fresh `next_*` set
        // for the cycle after that.
        std::vector<uint8_t> send_secret{};
        std::vector<uint8_t> recv_secret{};
        PacketKeys next_send_keys{};
        PacketKeys next_recv_keys{};
        CipherCtx next_send_ctx{};
        CipherCtx next_recv_ctx{};
        uint8_t send_key_phase{0};
        uint8_t recv_key_phase{0};
        bool next_keys_ready{false};
    };

    auto space(EncryptionLevel l) -> Space& {
        return spaces_[static_cast<std::size_t>(l)];
    }

    // Try to install Handshake / Application keys from TLS if newly
    // available.
    auto install_new_keys() -> void;

    // Drain any TLS-produced CRYPTO bytes; queue them for sending.
    auto pull_crypto_from_tls() -> void;

    // Process a single packet from a datagram. Returns bytes consumed
    // from `datagram` (caller advances by that amount for coalesced
    // packets). 0 = unparseable, abandon datagram.
    auto process_long_packet(std::span<const uint8_t> datagram, std::size_t& off) -> std::size_t;
    auto process_short_packet(std::vector<uint8_t>& datagram_owned) -> bool;

    auto deliver_frames(EncryptionLevel level, std::span<const uint8_t> payload) -> void;
    auto on_crypto_frame(EncryptionLevel level, const CryptoFrame& f) -> void;
    auto on_stream_frame(const StreamFrame& f) -> void;
    auto on_ack_frame(EncryptionLevel level, const AckFrame& f) -> void;

    auto build_and_send(EncryptionLevel level) -> bool;

    // Reassemble in-order CRYPTO bytes for the level and feed them to TLS.
    auto feed_tls_crypto(EncryptionLevel level) -> void;

    // Get-or-create stream.
    auto ensure_stream(uint64_t stream_id) -> Stream*;

    // Re-queue the CRYPTO + STREAM frames carried by `rec` for
    // retransmission, and notify congestion control of the loss.
    // After this, build_and_send will emit those frames in a fresh
    // packet on its next call.  The caller is responsible for erasing
    // `rec` from sent_[level] (it's typically the iterator they're
    // walking).
    auto declare_lost(SentRecord& rec, EncryptionLevel level) -> void;

    // RFC 9002 §6.1.1+§6.1.2: walk sent_[level] for packets sent
    // before `largest_acked_pn - kPacketThreshold` OR sent more than
    // loss_time_threshold ago, declare them lost.  Called after each
    // ACK is processed.
    auto detect_and_handle_loss(EncryptionLevel level) -> void;

    // RFC 9002 §6.2: if any in-flight ack-eliciting packet is older
    // than the PTO timeout, fire — bump pto_count, declare the
    // most-recent ack-eliciting record(s) lost so the next
    // build_and_send re-emits them as a probe.
    auto check_pto() -> void;

    // RFC 9000 §10.2: queue a CONNECTION_CLOSE frame for emission
    // and flip state_ to Closing.  The next build_and_send pass
    // packs the frame into a single packet at the highest available
    // encryption level and sends it.  `application` selects between
    // 0x1c (transport error) and 0x1d (application error);
    // `frame_type` is the offending wire-frame type for transport
    // closes, ignored for application closes.  Subsequent attempts
    // to send are silently dropped.
    auto initiate_close(uint64_t error_code, bool application,
                        std::string reason, uint64_t frame_type = 0) -> void;

    // RFC 9000 §10.1: tear the connection down on inactivity.
    // Called from on_timer; flips state_ to Draining when
    // now - last_activity_ exceeds the negotiated idle timeout
    // (the lesser of our and the peer's max_idle_timeout_ms).
    auto check_idle_timeout() -> void;

    // RFC 9000 §8.2: handle an inbound PATH_CHALLENGE by queuing a
    // matching PATH_RESPONSE for the next 1-RTT send.  We respond on
    // the existing (validated) path — the Listener freezes the peer
    // address post-handshake, so PATH_CHALLENGE arriving from a forged
    // off-path source still gets its response routed to the legitimate
    // peer.  That's RFC-conformant: §8.2.2 says PATH_RESPONSE goes on
    // the path PATH_CHALLENGE arrived on, but we don't currently
    // support multiple paths, so all responses go on the one path we
    // know.  Cheap liveness probes from the real peer continue to
    // work; forged challenges from off-path attackers waste 30-ish
    // bytes on the legitimate path.

    // ---- Members --------------------------------------------------------
    Role role_{Role::Server};
    State state_{State::Handshaking};

    std::vector<uint8_t> dcid_; // destination CID (peer's chosen SCID)
    std::vector<uint8_t> scid_; // source CID (ours)
    std::vector<uint8_t> original_dcid_; // for the OD-CID transport param

    SendFn send_fn_;
    ClockFn clock_fn_;
    TimePoint last_activity_{};

    std::shared_ptr<TlsContext> tls_ctx_;
    TlsConnection tls_;
    TransportParameters local_tp_{};

    std::array<Space, 4> spaces_{};

    Recovery recovery_{};
    Congestion congestion_{};

    // Streams indexed by ID.
    std::map<uint64_t, std::unique_ptr<Stream>> streams_;

    std::array<std::map<uint64_t, SentRecord>, 4> sent_{};
    // CRYPTO frames pulled from TLS but not yet packetized. One bucket
    // per encryption level (Initial/0-RTT/Handshake/Application).
    std::array<std::vector<CryptoFrame>, 4> crypto_pending_{};

    // Scratch buffer reused across every build_and_send call to avoid
    // per-packet std::vector reallocation churn. Reserved at
    // construction time to a comfortable max-datagram-size headroom;
    // each call clear()s it (which preserves capacity) and writes the
    // outgoing datagram directly into it in place.
    std::vector<uint8_t> scratch_packet_{};

    // Have we sent HANDSHAKE_DONE yet?
    bool sent_handshake_done_{false};

    // RFC 9000 §10.2 — pending CONNECTION_CLOSE.  Set by
    // initiate_close, drained by build_and_send (emits in a single
    // packet, then leaves state_ in Closing with no more sends).
    std::optional<ConnectionCloseFrame> pending_close_{};

    // RFC 9000 §8.2.2 — PATH_RESPONSE frames we owe in reply to
    // inbound PATH_CHALLENGE.  Drained into the next 1-RTT packet.
    std::vector<std::array<uint8_t, 8>> pending_path_responses_{};

    // RFC 9000 §8.1.2 anti-amplification bookkeeping.  Until the peer's
    // address is validated (either by a Retry token round-trip handled
    // at Listener level, or by successfully decrypting a Handshake-
    // protected packet from the peer), the server MUST NOT send more
    // than 3× the bytes it has received from that address.
    bool peer_address_validated_{false};
    uint64_t recv_bytes_total_{0};
    uint64_t sent_bytes_unvalidated_{0};
};

} // namespace quic
} // namespace spaznet
