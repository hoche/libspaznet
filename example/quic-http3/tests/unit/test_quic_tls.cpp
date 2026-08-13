// Phase 3 verification.
//
// 1. Transport-parameter round-trip (RFC 9000 §18).
//
// 2. An end-to-end TLS handshake driven entirely in-memory. We construct
//    a TlsConnection in server mode against a freshly-generated
//    self-signed cert, and we use a second SSL* in client mode (QUIC
//    callbacks / WOLFSSL_QUIC_METHOD) to feed it CRYPTO frames.

#include <gtest/gtest.h>

#include <libspaznet/quic/tls.hpp>
#include <libspaznet/quic/transport_params.hpp>

#include "quic_test_tls.hpp"

#include <memory>
#include <string>
#include <vector>

namespace {

using namespace spaznet::quic;
using namespace spaznet::quic::test;

auto make_test_client(const std::vector<uint8_t>& tp_wire) -> std::unique_ptr<TlsClientSide> {
    auto c = std::make_unique<TlsClientSide>();
    c->ctx = make_client_ssl_ctx();
    c->ssl = SSL_new(c->ctx);
    SSL_set_connect_state(c->ssl);
    SSL_set_tlsext_host_name(c->ssl, "localhost");
    c->own_tp_wire = tp_wire;
    if (!install_plain_client_quic(c.get())) {
        throw std::runtime_error("install_plain_client_quic failed");
    }
    return c;
}

} // namespace

TEST(QuicTransportParams, RoundTrip) {
    TransportParameters tp;
    tp.original_destination_connection_id = std::vector<uint8_t>{1, 2, 3, 4};
    tp.initial_source_connection_id = std::vector<uint8_t>{5, 6, 7, 8};
    tp.stateless_reset_token = std::array<uint8_t, 16>{};
    for (int i = 0; i < 16; ++i) (*tp.stateless_reset_token)[i] = static_cast<uint8_t>(i);
    tp.max_idle_timeout_ms = 30000;
    tp.initial_max_data = 1 << 20;
    tp.initial_max_stream_data_bidi_local = 65535;
    tp.initial_max_stream_data_bidi_remote = 65535;
    tp.initial_max_stream_data_uni = 65535;
    tp.initial_max_streams_bidi = 100;
    tp.initial_max_streams_uni = 3;
    tp.ack_delay_exponent = 7; // non-default
    tp.max_ack_delay_ms = 50;  // non-default
    tp.active_connection_id_limit = 4;
    tp.disable_active_migration = true;

    auto wire = encode_transport_params(tp);
    TransportParameters parsed;
    ASSERT_TRUE(decode_transport_params({wire.data(), wire.size()}, parsed));

    EXPECT_EQ(parsed.original_destination_connection_id,
              tp.original_destination_connection_id);
    EXPECT_EQ(parsed.initial_source_connection_id, tp.initial_source_connection_id);
    EXPECT_EQ(parsed.stateless_reset_token, tp.stateless_reset_token);
    EXPECT_EQ(parsed.max_idle_timeout_ms, 30000U);
    EXPECT_EQ(parsed.initial_max_data, 1U << 20);
    EXPECT_EQ(parsed.initial_max_streams_bidi, 100U);
    EXPECT_EQ(parsed.ack_delay_exponent, 7U);
    EXPECT_EQ(parsed.max_ack_delay_ms, 50U);
    EXPECT_EQ(parsed.active_connection_id_limit, 4U);
    EXPECT_TRUE(parsed.disable_active_migration);
}

TEST(QuicTransportParams, UnknownPreserved) {
    // Hand-build a wire string with id=0x1234 and value bytes 0xAA 0xBB.
    std::vector<uint8_t> wire;
    // id varint 0x1234 -> 0x52 0x34 (2-byte form).
    wire.insert(wire.end(), {0x52, 0x34, 0x02, 0xAA, 0xBB});
    TransportParameters parsed;
    ASSERT_TRUE(decode_transport_params({wire.data(), wire.size()}, parsed));
    ASSERT_EQ(parsed.unknown.size(), 1U);
    EXPECT_EQ(parsed.unknown[0].id, 0x1234U);
    EXPECT_EQ(parsed.unknown[0].value, (std::vector<uint8_t>{0xAA, 0xBB}));

    // Round-trip preserves the unknown.
    auto re = encode_transport_params(parsed);
    EXPECT_EQ(re, wire);
}

TEST(QuicTls, EndToEndHandshake) {
    auto [cert_pem, key_pem] = make_test_cert_pem("libspaznet-test");
    TlsServerConfig cfg;
    cfg.cert_pem = cert_pem;
    cfg.key_pem = key_pem;
    cfg.alpn = {"h3"};
    auto ctx = TlsContext::make_server(cfg);
    ASSERT_NE(ctx, nullptr);

    TransportParameters server_tp;
    server_tp.original_destination_connection_id = std::vector<uint8_t>{0xCA, 0xFE, 0xBA, 0xBE};
    server_tp.initial_source_connection_id = std::vector<uint8_t>{0x01, 0x02};
    server_tp.initial_max_data = 1 << 20;
    server_tp.initial_max_stream_data_bidi_remote = 1 << 16;
    server_tp.initial_max_streams_bidi = 10;
    server_tp.initial_max_streams_uni = 3;

    std::vector<uint8_t> client_dcid = {0xCA, 0xFE, 0xBA, 0xBE};
    TlsConnection server(ctx, {client_dcid.data(), client_dcid.size()}, server_tp);

    TransportParameters client_tp;
    client_tp.initial_source_connection_id = std::vector<uint8_t>{0xDD};
    client_tp.initial_max_data = 1 << 20;
    client_tp.initial_max_streams_bidi = 100;
    auto client_tp_wire = encode_transport_params(client_tp);
    auto client = make_test_client(client_tp_wire);

    // Pump until both sides finish or we hit a safety cap.
    for (int round = 0; round < 16; ++round) {
        int crc = SSL_do_handshake(client->ssl);
        if (crc != 1) {
            int err = SSL_get_error(client->ssl, crc);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                FAIL() << "client SSL_do_handshake failed err=" << err;
            }
        }
        client_poll_peer_tp(client->ssl, client->peer_tp_wire, client->got_peer_tp);
        for (std::size_t i = 0; i < 4; ++i) {
            if (!client->tls_out[i].empty()) {
                server.deliver_crypto(static_cast<EncryptionLevel>(i),
                                      {client->tls_out[i].data(), client->tls_out[i].size()});
                client->tls_out[i].clear();
            }
        }

        auto srv_state = server.advance();
        for (std::size_t i = 0; i < 4; ++i) {
            auto& buf = server.out_crypto(static_cast<EncryptionLevel>(i));
            if (!buf.empty()) {
                client_feed_crypto(client->ssl, static_cast<EncryptionLevel>(i),
                                   {buf.data(), buf.size()},
#if defined(SPAZNET_TLS_OPENSSL)
                                   &client->tls_in
#else
                                   nullptr
#endif
                );
                buf.clear();
            }
        }

        bool client_done = SSL_is_init_finished(client->ssl) != 0;
        bool server_done = srv_state == TlsConnection::State::Established;
        if (client_done && server_done) {
            break;
        }
        ASSERT_NE(srv_state, TlsConnection::State::Failed)
            << "server failed at round " << round
            << " alert=" << static_cast<int>(server.alert_code());
    }

    client_poll_peer_tp(client->ssl, client->peer_tp_wire, client->got_peer_tp);

    EXPECT_EQ(server.state(), TlsConnection::State::Established);
    EXPECT_TRUE(SSL_is_init_finished(client->ssl));
    EXPECT_EQ(server.negotiated_alpn(), "h3");
    EXPECT_TRUE(server.have_peer_transport_params());
    EXPECT_EQ(server.peer_transport_params().initial_max_streams_bidi, 100U);
    EXPECT_TRUE(client->got_peer_tp);
    TransportParameters parsed_server_tp;
    ASSERT_TRUE(decode_transport_params(
        {client->peer_tp_wire.data(), client->peer_tp_wire.size()}, parsed_server_tp));
    EXPECT_EQ(parsed_server_tp.original_destination_connection_id,
              std::vector<uint8_t>{client_dcid});
    EXPECT_FALSE(server.read_secret(EncryptionLevel::Application).empty());
    EXPECT_FALSE(server.write_secret(EncryptionLevel::Application).empty());
}
