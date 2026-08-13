// Full-stack HTTP/3 loopback: client and server exchange real protected
// QUIC v1 datagrams carrying a complete TLS 1.3 handshake AND a follow-
// up HTTP/3 GET request/response over a request stream.
//
// The client reuses the same hand-built harness as test_quic_connection
// (because curl HTTP/3 isn't available on either CI host) but layered
// on top with QPACK-encoded HEADERS + DATA frames. The server uses
// quic::Connection + http3::Http3Server.

#include <gtest/gtest.h>

#include <libspaznet/http3/h3frame.hpp>
#include <libspaznet/http3/qpack.hpp>
#include <libspaznet/http3/server.hpp>
#include <libspaznet/quic/connection.hpp>
#include <libspaznet/quic/varint.hpp>

#include <cstring>
#include <span>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quic_test_tls.hpp"

using namespace spaznet::quic;
using namespace spaznet::quic::test;
namespace h3 = spaznet::http3;

namespace {

// Reusable client harness (slightly simplified from test_quic_connection).
struct QuicClient {
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};
    std::vector<uint8_t> dcid;
    std::vector<uint8_t> scid;
    std::array<std::vector<uint8_t>, 4> tls_out;
#if defined(SPAZNET_TLS_OPENSSL)
    std::array<std::vector<uint8_t>, 4> tls_in;
    std::array<std::size_t, 4> tls_in_cursor{0, 0, 0, 0};
    uint32_t send_level{OSSL_RECORD_PROTECTION_LEVEL_NONE};
#else
    std::size_t send_level{0};
#endif
    std::vector<uint8_t> own_tp_wire;
    bool got_peer_tp{false};
    std::vector<uint8_t> peer_tp_wire;
    std::array<PacketKeys, 4> send_keys{};
    std::array<PacketKeys, 4> recv_keys{};
    std::array<bool, 4> send_ready{false, false, false, false};
    std::array<bool, 4> recv_ready{false, false, false, false};
    std::array<Aead, 4> aead{Aead::Aes128Gcm, Aead::Aes128Gcm, Aead::Aes128Gcm,
                              Aead::Aes128Gcm};
    std::array<uint64_t, 4> next_pn{0, 0, 0, 0};
    std::array<uint64_t, 4> largest_recv_pn{0, 0, 0, 0};
    std::array<bool, 4> any_recv{false, false, false, false};
    std::array<uint64_t, 4> crypto_send_off{0, 0, 0, 0};
    std::array<uint64_t, 4> crypto_recv_off{0, 0, 0, 0};
    std::array<std::map<uint64_t, std::vector<uint8_t>>, 4> crypto_recv_chunks;
    std::array<PnSpace, 4> acks;
    std::deque<std::vector<uint8_t>> outbox;
    std::deque<std::vector<uint8_t>> pending_dgs;

    // For our HTTP/3 request stream (id 0 = client bidi):
    std::vector<uint8_t> req_stream_out;
    uint64_t req_stream_send_off{0};
    bool req_stream_fin_sent{false};
    // Inbound buffer for the response on the same stream:
    std::map<uint64_t, std::vector<uint8_t>> resp_chunks; // offset -> data
    std::vector<uint8_t> resp_assembled;
    bool resp_fin_seen{false};

    ~QuicClient() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
    }
};

auto make_client(std::vector<uint8_t> dcid, std::vector<uint8_t> scid)
    -> std::unique_ptr<QuicClient> {
    auto c = std::make_unique<QuicClient>();
    c->dcid = dcid;
    c->scid = scid;
    c->ctx = make_client_ssl_ctx();
    c->ssl = SSL_new(c->ctx);
    SSL_set_connect_state(c->ssl);
    SSL_set_tlsext_host_name(c->ssl, "localhost");

    auto secret_c = derive_initial_secret(dcid, Direction::Client);
    auto secret_s = derive_initial_secret(dcid, Direction::Server);
    c->aead[0] = Aead::Aes128Gcm;
    c->send_keys[0] = derive_packet_keys(Aead::Aes128Gcm, secret_c);
    c->recv_keys[0] = derive_packet_keys(Aead::Aes128Gcm, secret_s);
    c->send_ready[0] = true;
    c->recv_ready[0] = true;

    TransportParameters tp;
    tp.initial_source_connection_id = scid;
    tp.initial_max_data = 1 << 20;
    tp.initial_max_streams_bidi = 16;
    tp.initial_max_streams_uni = 16;
    tp.initial_max_stream_data_bidi_remote = 1 << 16;
    tp.initial_max_stream_data_bidi_local = 1 << 16;
    tp.initial_max_stream_data_uni = 1 << 16;
    c->own_tp_wire = encode_transport_params(tp);
    if (!install_packet_client_quic(c.get())) {
        throw std::runtime_error("install_packet_client_quic failed");
    }
    return c;
}

auto build_long(QuicClient& c, EncryptionLevel level, LongType type,
                const std::vector<uint8_t>& payload) -> std::vector<uint8_t> {
    auto& keys = c.send_keys[static_cast<std::size_t>(level)];
    Aead a = c.aead[static_cast<std::size_t>(level)];
    const std::size_t pn_len = 4;
    const std::size_t tag_len = aead_tag_length(a);
    std::vector<uint8_t> pkt;
    uint8_t first = static_cast<uint8_t>(0x80 | 0x40 | (static_cast<uint8_t>(type) << 4) |
                                         static_cast<uint8_t>(pn_len - 1));
    pkt.push_back(first);
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 24));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 16));
    pkt.push_back(static_cast<uint8_t>(kQuicV1 >> 8));
    pkt.push_back(static_cast<uint8_t>(kQuicV1));
    pkt.push_back(static_cast<uint8_t>(c.dcid.size()));
    pkt.insert(pkt.end(), c.dcid.begin(), c.dcid.end());
    pkt.push_back(static_cast<uint8_t>(c.scid.size()));
    pkt.insert(pkt.end(), c.scid.begin(), c.scid.end());
    if (type == LongType::Initial) VarInt::append(pkt, 0U);
    const uint64_t length = pn_len + payload.size() + tag_len;
    VarInt::append(pkt, length);
    const std::size_t pn_offset = pkt.size();
    const uint64_t pn = c.next_pn[static_cast<std::size_t>(level)]++;
    for (std::size_t i = 0; i < pn_len; ++i)
        pkt.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - i))) & 0xFF));
    const std::size_t payload_off = pkt.size();
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
    std::vector<uint8_t> pt(pkt.begin() + payload_off, pkt.end());
    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
    std::vector<uint8_t> sealed;
    aead_seal(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
              {aad.data(), aad.size()}, {pt.data(), pt.size()}, sealed);
    pkt.resize(payload_off);
    pkt.insert(pkt.end(), sealed.begin(), sealed.end());
    auto mask = header_protection_mask(a, {keys.hp.data(), keys.hp.size()},
                                       {pkt.data() + pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x0F;
    for (std::size_t i = 0; i < pn_len; ++i) pkt[pn_offset + i] ^= mask[1 + i];
    return pkt;
}

auto build_short(QuicClient& c, const std::vector<uint8_t>& payload) -> std::vector<uint8_t> {
    const auto i = static_cast<std::size_t>(EncryptionLevel::Application);
    auto& keys = c.send_keys[i];
    Aead a = c.aead[i];
    const std::size_t pn_len = 4;
    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>(0x40 | (pn_len - 1)));
    pkt.insert(pkt.end(), c.dcid.begin(), c.dcid.end());
    const std::size_t pn_offset = pkt.size();
    uint64_t pn = c.next_pn[i]++;
    for (std::size_t k = 0; k < pn_len; ++k)
        pkt.push_back(static_cast<uint8_t>((pn >> (8 * (pn_len - 1 - k))) & 0xFF));
    const std::size_t payload_off = pkt.size();
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    std::vector<uint8_t> aad(pkt.begin(), pkt.begin() + payload_off);
    std::vector<uint8_t> pt(pkt.begin() + payload_off, pkt.end());
    auto nonce = make_aead_nonce({keys.iv.data(), keys.iv.size()}, pn);
    std::vector<uint8_t> sealed;
    aead_seal(a, {keys.key.data(), keys.key.size()}, {nonce.data(), nonce.size()},
              {aad.data(), aad.size()}, {pt.data(), pt.size()}, sealed);
    pkt.resize(payload_off);
    pkt.insert(pkt.end(), sealed.begin(), sealed.end());
    auto mask = header_protection_mask(a, {keys.hp.data(), keys.hp.size()},
                                       {pkt.data() + pn_offset + 4, kSampleLen});
    pkt[0] ^= mask[0] & 0x1F;
    for (std::size_t k = 0; k < pn_len; ++k) pkt[pn_offset + k] ^= mask[1 + k];
    return pkt;
}

auto emit(QuicClient& c) -> void {
    // Handshake-stage CRYPTO emission (Initial / Handshake).
    for (auto lvl :
         {EncryptionLevel::Initial, EncryptionLevel::Handshake, EncryptionLevel::Application}) {
        const auto i = static_cast<std::size_t>(lvl);
        if (!c.send_ready[i]) continue;
        std::vector<uint8_t> payload;
        if (c.acks[i].needs_ack()) {
            AckFrame ack;
            c.acks[i].build_ack_frame(0, ack);
            encode_frame(payload, Frame{ack});
        }
        if (!c.tls_out[i].empty()) {
            CryptoFrame cf;
            cf.offset = c.crypto_send_off[i];
            cf.data = c.tls_out[i];
            c.crypto_send_off[i] += c.tls_out[i].size();
            encode_frame(payload, Frame{cf});
            c.tls_out[i].clear();
        }
        // In 1-RTT, also flush any pending HTTP/3 stream bytes for our
        // request stream (id 0). We use a STREAM frame with LEN and
        // optionally FIN flags.
        if (lvl == EncryptionLevel::Application && !c.req_stream_out.empty()) {
            StreamFrame sf;
            sf.stream_id = 0; // client-initiated bidi #0
            sf.offset = c.req_stream_send_off;
            sf.has_length = true;
            sf.fin = !c.req_stream_fin_sent ? true : false;
            sf.data = c.req_stream_out;
            c.req_stream_send_off += c.req_stream_out.size();
            c.req_stream_out.clear();
            c.req_stream_fin_sent = true;
            encode_frame(payload, Frame{sf});
        }
        if (payload.empty()) continue;
        if (lvl == EncryptionLevel::Initial) {
            while (payload.size() < 1100) payload.push_back(0);
            auto pkt = build_long(c, lvl, LongType::Initial, payload);
            if (pkt.size() < 1200) pkt.resize(1200, 0);
            c.outbox.push_back(std::move(pkt));
        } else if (lvl == EncryptionLevel::Handshake) {
            c.outbox.push_back(build_long(c, lvl, LongType::Handshake, payload));
        } else {
            c.outbox.push_back(build_short(c, payload));
        }
    }
}

auto receive(QuicClient& c, std::vector<uint8_t> dg) -> void {
    std::size_t off = 0;
    while (off < dg.size()) {
        if ((dg[off] & 0x80U) != 0) {
            LongHeader hdr;
            std::size_t cursor = off;
            if (!parse_long_header({dg.data(), dg.size()}, cursor, hdr)) return;
            EncryptionLevel lvl = hdr.type == LongType::Initial
                                      ? EncryptionLevel::Initial
                                      : hdr.type == LongType::Handshake
                                            ? EncryptionLevel::Handshake
                                            : EncryptionLevel::EarlyData;
            const auto i = static_cast<std::size_t>(lvl);
            if (!c.recv_ready[i]) {
                c.pending_dgs.emplace_back(dg.begin() + static_cast<std::ptrdiff_t>(off),
                                           dg.end());
                return;
            }
            std::vector<uint8_t> pkt(dg.begin() + static_cast<std::ptrdiff_t>(off),
                                     dg.begin() + static_cast<std::ptrdiff_t>(cursor));
            LongHeader local = hdr;
            local.pn_offset -= off;
            uint64_t pn = 0;
            std::vector<uint8_t> plain;
            if (!decrypt_long_packet(c.aead[i], c.recv_keys[i],
                                     c.any_recv[i] ? c.largest_recv_pn[i] : 0, pkt, local,
                                     pn, plain)) {
                return;
            }
            c.any_recv[i] = true;
            if (pn > c.largest_recv_pn[i]) c.largest_recv_pn[i] = pn;
            c.acks[i].on_received(pn, true);
            std::vector<Frame> frames;
            if (!parse_frames({plain.data(), plain.size()}, frames)) return;
            for (auto& f : frames) {
                std::visit(
                    [&](auto& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, CryptoFrame>) {
                            c.crypto_recv_chunks[i][x.offset] = x.data;
                        }
                    },
                    f);
            }
            while (!c.crypto_recv_chunks[i].empty()) {
                auto it = c.crypto_recv_chunks[i].begin();
                if (it->first > c.crypto_recv_off[i]) break;
                if (it->first + it->second.size() <= c.crypto_recv_off[i]) {
                    c.crypto_recv_chunks[i].erase(it);
                    continue;
                }
                std::size_t skip =
                    static_cast<std::size_t>(c.crypto_recv_off[i] - it->first);
                std::span<const uint8_t> chunk{it->second.data() + skip,
                                               it->second.size() - skip};
                client_feed_crypto(c.ssl, static_cast<EncryptionLevel>(i), chunk,
#if defined(SPAZNET_TLS_OPENSSL)
                                   &c.tls_in
#else
                                   nullptr
#endif
                );
                c.crypto_recv_off[i] += it->second.size() - skip;
                c.crypto_recv_chunks[i].erase(it);
            }
            off = cursor;
        } else {
            const auto i = static_cast<std::size_t>(EncryptionLevel::Application);
            if (!c.recv_ready[i]) return;
            std::vector<uint8_t> pkt(dg.begin() + static_cast<std::ptrdiff_t>(off), dg.end());
            const std::size_t pn_offset = 1 + c.scid.size();
            if (pn_offset + 4 + kSampleLen > pkt.size()) return;
            auto mask = header_protection_mask(c.aead[i], {c.recv_keys[i].hp.data(),
                                                            c.recv_keys[i].hp.size()},
                                                {pkt.data() + pn_offset + 4, kSampleLen});
            pkt[0] ^= mask[0] & 0x1F;
            const std::size_t pn_len = (pkt[0] & 0x03U) + 1;
            for (std::size_t k = 0; k < pn_len; ++k) pkt[pn_offset + k] ^= mask[1 + k];
            uint64_t trunc = 0;
            for (std::size_t k = 0; k < pn_len; ++k) trunc = (trunc << 8) | pkt[pn_offset + k];
            uint64_t pn = decode_packet_number(c.any_recv[i] ? c.largest_recv_pn[i] : 0,
                                               trunc, pn_len * 8);
            const std::size_t header_len = pn_offset + pn_len;
            std::span<const uint8_t> aad{pkt.data(), header_len};
            std::span<const uint8_t> ct{pkt.data() + header_len, pkt.size() - header_len};
            auto nonce = make_aead_nonce(
                {c.recv_keys[i].iv.data(), c.recv_keys[i].iv.size()}, pn);
            std::vector<uint8_t> plain;
            if (!aead_open(c.aead[i],
                           {c.recv_keys[i].key.data(), c.recv_keys[i].key.size()},
                           {nonce.data(), nonce.size()}, aad, ct, plain)) {
                return;
            }
            c.any_recv[i] = true;
            if (pn > c.largest_recv_pn[i]) c.largest_recv_pn[i] = pn;
            c.acks[i].on_received(pn, true);
            std::vector<Frame> frames;
            if (!parse_frames({plain.data(), plain.size()}, frames)) return;
            for (auto& f : frames) {
                std::visit(
                    [&](auto& x) {
                        using T = std::decay_t<decltype(x)>;
                        if constexpr (std::is_same_v<T, StreamFrame>) {
                            if (x.stream_id == 0) {
                                c.resp_chunks[x.offset] = x.data;
                                if (x.fin) c.resp_fin_seen = true;
                            }
                        }
                    },
                    f);
            }
            // Reassemble response in order.
            while (!c.resp_chunks.empty()) {
                auto it = c.resp_chunks.begin();
                const uint64_t expected = c.resp_assembled.size();
                if (it->first > expected) break;
                if (it->first + it->second.size() <= expected) {
                    c.resp_chunks.erase(it);
                    continue;
                }
                std::size_t skip = static_cast<std::size_t>(expected - it->first);
                c.resp_assembled.insert(c.resp_assembled.end(),
                                        it->second.begin() + skip, it->second.end());
                c.resp_chunks.erase(it);
            }
            return;
        }
    }
}

} // namespace

TEST(Http3EndToEnd, GetReturnsResponseBody) {
    auto [cert, key] = make_test_cert_pem("ls-test");
    TlsServerConfig tcfg{cert, key, {"h3"}};
    auto tls_ctx = TlsContext::make_server(tcfg);
    ASSERT_NE(tls_ctx, nullptr);

    std::vector<uint8_t> dcid = {0xCA, 0xFE, 0xBA, 0xBE};
    std::vector<uint8_t> client_scid = {0x77};
    std::vector<uint8_t> server_scid = {0x11, 0x22, 0x33, 0x44};

    std::deque<std::vector<uint8_t>> server_outbox;
    TransportParameters server_tp;
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_stream_data_bidi_local = 1 << 16;
    server_tp.initial_max_stream_data_uni = 1 << 16;
    server_tp.initial_max_streams_bidi = 16;
    server_tp.initial_max_streams_uni = 16;

    Connection server(tls_ctx, {dcid.data(), dcid.size()},
                      {client_scid.data(), client_scid.size()},
                      {server_scid.data(), server_scid.size()}, server_tp,
                      [&](std::span<const uint8_t> dg) {
                          server_outbox.emplace_back(dg.begin(), dg.end());
                      });

    bool got_request = false;
    h3::Http3Server h3srv(server, [&](const h3::Http3Request& req) {
        got_request = true;
        EXPECT_EQ(req.method, "GET");
        EXPECT_EQ(req.path, "/hello");
        h3::Http3Response resp;
        resp.status_code = 200;
        resp.headers.emplace_back("content-type", "text/plain");
        const std::string body = "Hello, h3!";
        resp.body.assign(body.begin(), body.end());
        return resp;
    });

    auto client = make_client(dcid, client_scid);
    bool sent_request = false;

    (void)sent_request;
    for (int round = 0; round < 30; ++round) {
        int rc = SSL_do_handshake(client->ssl);
        if (rc != 1) {
            int err = SSL_get_error(client->ssl, rc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                FAIL() << "client SSL_do_handshake err=" << err;
            }
        }
        // Once handshake is finished from the client's perspective and
        // we haven't sent the request yet, queue HEADERS+DATA bytes onto
        // stream 0.
        if (!sent_request && SSL_is_init_finished(client->ssl)) {
            h3::HeaderList headers = {{":method", "GET"},
                                      {":scheme", "https"},
                                      {":authority", "localhost"},
                                      {":path", "/hello"}};
            std::vector<uint8_t> encoded_headers;
            h3::qpack_encode(headers, encoded_headers);
            std::vector<uint8_t> framed;
            h3::encode_h3_frame(framed, h3::H3Frame{h3::H3Headers{encoded_headers}});
            client->req_stream_out = std::move(framed);
            client->req_stream_fin_sent = false;
            sent_request = true;
        }

        emit(*client);
        while (!client->outbox.empty()) {
            auto dg = std::move(client->outbox.front());
            client->outbox.pop_front();
            server.on_datagram({dg.data(), dg.size()});
        }
        server.on_timer();
        h3srv.pump();
        // Pump again — the response we wrote in pump() needs to be
        // packetized by the connection.
        server.on_timer();

        while (!server_outbox.empty()) {
            auto dg = std::move(server_outbox.front());
            server_outbox.pop_front();
            receive(*client, std::move(dg));
        }
        if (!client->pending_dgs.empty()) {
            auto pending = std::move(client->pending_dgs);
            client->pending_dgs.clear();
            for (auto& dg : pending) receive(*client, std::move(dg));
        }
        if (got_request && client->resp_fin_seen) break;
    }

    EXPECT_TRUE(got_request);
    EXPECT_TRUE(client->resp_fin_seen);

    // Parse the response as HTTP/3 HEADERS + DATA frames.
    std::size_t off = 0;
    h3::H3Frame f;
    ASSERT_TRUE(h3::parse_h3_frame({client->resp_assembled.data(),
                                    client->resp_assembled.size()},
                                   off, f));
    auto* hf = std::get_if<h3::H3Headers>(&f);
    ASSERT_NE(hf, nullptr);
    h3::HeaderList parsed;
    ASSERT_TRUE(h3::qpack_decode(
        {hf->encoded_field_section.data(), hf->encoded_field_section.size()}, parsed));
    bool saw_status_200 = false;
    for (auto const& [n, v] : parsed) {
        if (n == ":status" && v == "200") saw_status_200 = true;
    }
    EXPECT_TRUE(saw_status_200);

    ASSERT_TRUE(h3::parse_h3_frame({client->resp_assembled.data(),
                                    client->resp_assembled.size()},
                                   off, f));
    auto* df = std::get_if<h3::H3Data>(&f);
    ASSERT_NE(df, nullptr);
    std::string body(df->data.begin(), df->data.end());
    EXPECT_EQ(body, "Hello, h3!");
}
