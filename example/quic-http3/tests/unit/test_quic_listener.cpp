// Phase 5 verification:
// - RFC 9001 §A.4 Retry integrity tag KAT.
// - Version Negotiation packet layout.
// - Listener routes long-header packets by DCID; new Initials create
//   Connection objects, repeated Initials route to the same one,
//   non-v1 versions get Version Negotiation, short-header packets
//   route by server-chosen SCID.

#include <gtest/gtest.h>

#include <libspaznet/quic/listener.hpp>
#include <libspaznet/quic/packet.hpp>
#include <libspaznet/quic/varint.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

using namespace spaznet::quic;

namespace {

auto hex(std::string s) -> std::vector<uint8_t> {
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](unsigned char c) { return std::isspace(c) != 0; }),
            s.end());
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    auto nyb = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return 10 + (c - 'A');
    };
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nyb(s[i]) << 4) | nyb(s[i + 1])));
    }
    return out;
}

auto make_self_signed() -> std::pair<std::string, std::string> {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME* nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("ls-test"), -1, -1, 0);
    X509_set_issuer_name(x, nm);
    X509_sign(x, pkey, EVP_sha256());

    BIO* cb = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cb, x);
    char* cdata = nullptr;
    long clen = BIO_get_mem_data(cb, &cdata);
    std::string cpem(cdata, static_cast<std::size_t>(clen));
    BIO* kb = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* kdata = nullptr;
    long klen = BIO_get_mem_data(kb, &kdata);
    std::string kpem(kdata, static_cast<std::size_t>(klen));
    BIO_free(cb);
    BIO_free(kb);
    X509_free(x);
    EVP_PKEY_free(pkey);
    return {cpem, kpem};
}

} // namespace

// RFC 9001 Appendix A.4: example Retry packet.
//   ODCID = 0x8394c8f03e515708
//   Retry token = "token" (ASCII bytes — see the RFC's table)
//   The full Retry packet, integrity tag included, is:
//   ff000000010008f067a5502a4262b5746f6b656e04a265ba2eff4d829058fb3f0f2496ba
TEST(QuicRetry, Rfc9001AppA4_IntegrityTagKnownAnswer) {
    auto odcid = hex("8394c8f03e515708");
    // The expected packet, minus the 16-byte integrity tag at the end.
    auto retry_packet_without_tag = hex(
        "ff000000010008f067a5502a4262b5746f6b656e");
    auto expected_tag_bytes = hex("04a265ba2eff4d829058fb3f0f2496ba");
    std::array<uint8_t, 16> tag{};
    ASSERT_TRUE(compute_retry_integrity_tag(
        {odcid.data(), odcid.size()},
        {retry_packet_without_tag.data(), retry_packet_without_tag.size()}, tag));
    EXPECT_EQ(std::vector<uint8_t>(tag.begin(), tag.end()), expected_tag_bytes);
}

TEST(QuicRetry, BuildPacketMatchesRfcExample) {
    // Inputs:
    //   client SCID (= peer's chosen SCID echoed back, but for the RFC
    //                 example the client picked nothing — empty DCID),
    //   server SCID = f067a5502a4262b5,
    //   token = "token" (5 bytes),
    //   odcid = 8394c8f03e515708.
    auto server_scid = hex("f067a5502a4262b5");
    auto token = std::vector<uint8_t>{'t', 'o', 'k', 'e', 'n'};
    auto odcid = hex("8394c8f03e515708");
    auto pkt = build_retry_packet({}, {server_scid.data(), server_scid.size()},
                                  {token.data(), token.size()},
                                  {odcid.data(), odcid.size()});
    auto expected =
        hex("ff000000010008f067a5502a4262b5746f6b656e04a265ba2eff4d829058fb3f0f2496ba");
    EXPECT_EQ(pkt, expected);
}

TEST(QuicVersionNegotiation, EchoesCidsAndAdvertisesVersions) {
    auto client_dcid = hex("aabbccdd");
    auto client_scid = hex("11223344");
    std::array<uint8_t, 4> v1_be = {0x00, 0x00, 0x00, 0x01};
    auto pkt = build_version_negotiation_packet({client_dcid.data(), client_dcid.size()},
                                                {client_scid.data(), client_scid.size()},
                                                {v1_be.data(), v1_be.size()});
    // Header: 1 byte + 4-byte zero version + 1+4 + 1+4 + 4 = 19 bytes.
    ASSERT_EQ(pkt.size(), 1U + 4U + 1U + 4U + 1U + 4U + 4U);
    EXPECT_NE(pkt[0] & 0x80, 0);
    EXPECT_EQ(pkt[1], 0); EXPECT_EQ(pkt[2], 0); EXPECT_EQ(pkt[3], 0); EXPECT_EQ(pkt[4], 0);
    EXPECT_EQ(pkt[5], 4); // dcid length
    EXPECT_TRUE(std::equal(client_dcid.begin(), client_dcid.end(), pkt.begin() + 6));
    EXPECT_EQ(pkt[10], 4); // scid length
    EXPECT_TRUE(std::equal(client_scid.begin(), client_scid.end(), pkt.begin() + 11));
    EXPECT_EQ(pkt[15], 0x00);
    EXPECT_EQ(pkt[16], 0x00);
    EXPECT_EQ(pkt[17], 0x00);
    EXPECT_EQ(pkt[18], 0x01);
}

TEST(QuicListener, UnsupportedVersionTriggersVersionNegotiation) {
    auto [cert, key] = make_self_signed();
    TlsServerConfig tcfg{cert, key, {"h3"}};
    auto tls_ctx = TlsContext::make_server(tcfg);
    ASSERT_NE(tls_ctx, nullptr);

    Listener::Config cfg;
    cfg.tls_ctx = tls_ctx;
    cfg.server_tp.initial_max_data = 1 << 20;
    cfg.server_tp.initial_max_streams_bidi = 4;
    cfg.random_seed = 0xDEADBEEFULL;

    std::vector<std::vector<uint8_t>> outbox;
    Listener listener(cfg,
                      [&](const PeerAddr&, std::span<const uint8_t> dg) {
                          outbox.emplace_back(dg.begin(), dg.end());
                      });
    PeerAddr peer{};
    peer.length = sizeof(sockaddr_in);
    reinterpret_cast<sockaddr_in*>(&peer.storage)->sin_family = AF_INET;

    // Hand-build an Initial-like packet with version 0xCAFEBABE.
    std::vector<uint8_t> dg;
    dg.push_back(0xC0); // long form + fixed + Initial(0)
    dg.push_back(0xCA);
    dg.push_back(0xFE);
    dg.push_back(0xBA);
    dg.push_back(0xBE);
    std::vector<uint8_t> dcid = {1, 2, 3, 4};
    dg.push_back(static_cast<uint8_t>(dcid.size()));
    dg.insert(dg.end(), dcid.begin(), dcid.end());
    std::vector<uint8_t> scid = {9, 8, 7};
    dg.push_back(static_cast<uint8_t>(scid.size()));
    dg.insert(dg.end(), scid.begin(), scid.end());
    // Pad out so the listener doesn't reject as too small.
    dg.resize(64, 0);

    listener.on_datagram(peer, {dg.data(), dg.size()});
    ASSERT_EQ(outbox.size(), 1U);
    // Echoed CIDs are at offsets 5 and 5+1+4 in the VN packet.
    EXPECT_EQ(outbox[0][5], dcid.size());
    EXPECT_TRUE(std::equal(dcid.begin(), dcid.end(), outbox[0].begin() + 6));
    EXPECT_EQ(outbox[0][10], scid.size());
    EXPECT_TRUE(std::equal(scid.begin(), scid.end(), outbox[0].begin() + 11));
    EXPECT_EQ(listener.connection_count(), 0U); // no Connection created
}

namespace {

// Build a syntactically-valid v1 Initial we can hand to the listener
// for require_retry path testing.  The payload bytes won't decrypt, but
// the listener inspects the Token field before decryption.
auto build_initial_with_token(std::span<const uint8_t> dcid,
                              std::span<const uint8_t> scid,
                              std::span<const uint8_t> token) -> std::vector<uint8_t> {
    std::vector<uint8_t> dg;
    dg.push_back(0xC0); // long + fixed + Initial(0) + pn_len-1 = 0
    dg.push_back(0x00); dg.push_back(0x00); dg.push_back(0x00); dg.push_back(0x01);
    dg.push_back(static_cast<uint8_t>(dcid.size()));
    dg.insert(dg.end(), dcid.begin(), dcid.end());
    dg.push_back(static_cast<uint8_t>(scid.size()));
    dg.insert(dg.end(), scid.begin(), scid.end());
    // Token length (varint) + token bytes.
    VarInt::append(dg, token.size());
    dg.insert(dg.end(), token.begin(), token.end());
    // Length (varint, 2-byte form) covering PN(1) + payload + AEAD tag(16).
    // Pad payload so the total datagram is >=1200 bytes (RFC 9000 §14.1
    // — clients MUST pad to 1200; we exercise the realistic shape).
    const std::size_t header_through_token = dg.size();
    const std::size_t pn_len = 1;
    const std::size_t tag_len = 16;
    const std::size_t min_total = 1200;
    // After length(2) + PN + payload + tag must reach min_total.
    const std::size_t after_length = min_total - header_through_token - 2;
    const std::size_t payload_plus_tag = after_length - pn_len;
    const std::size_t length_field = pn_len + payload_plus_tag;
    // 2-byte varint header: high two bits = 01.
    dg.push_back(static_cast<uint8_t>(0x40 | ((length_field >> 8) & 0x3FU)));
    dg.push_back(static_cast<uint8_t>(length_field & 0xFFU));
    dg.push_back(0x00); // PN
    dg.resize(min_total, 0x00); // payload + space for tag
    return dg;
}

auto make_v4_peer(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) -> PeerAddr {
    PeerAddr p{};
    p.length = sizeof(sockaddr_in);
    auto* sin = reinterpret_cast<sockaddr_in*>(&p.storage);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    uint8_t bytes[4] = {a, b, c, d};
    std::memcpy(&sin->sin_addr, bytes, 4);
    return p;
}

} // namespace

TEST(QuicRequireRetry, InitialWithoutTokenEmitsRetryAndCreatesNoConnection) {
    auto [cert, key] = make_self_signed();
    TlsServerConfig tcfg{cert, key, {"h3"}};
    auto tls_ctx = TlsContext::make_server(tcfg);
    ASSERT_NE(tls_ctx, nullptr);

    Listener::Config cfg;
    cfg.tls_ctx = tls_ctx;
    cfg.server_tp.initial_max_data = 1 << 20;
    cfg.server_tp.initial_max_streams_bidi = 4;
    cfg.random_seed = 0xABCD1234ULL;
    cfg.require_retry = true;

    std::vector<std::vector<uint8_t>> outbox;
    Listener listener(cfg,
                      [&](const PeerAddr&, std::span<const uint8_t> dg) {
                          outbox.emplace_back(dg.begin(), dg.end());
                      });

    auto peer = make_v4_peer(127, 0, 0, 1, 12345);
    std::vector<uint8_t> dcid = {0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08};
    std::vector<uint8_t> scid = {0x11, 0x22, 0x33, 0x44};
    auto dg = build_initial_with_token({dcid.data(), dcid.size()},
                                       {scid.data(), scid.size()}, {});
    listener.on_datagram(peer, {dg.data(), dg.size()});

    ASSERT_EQ(outbox.size(), 1U);
    // Retry packet: long+fixed+type=Retry → top nibble bits set, type==3
    // (lives in bits 4-5).  Our build_retry_packet emits 0xFF.
    EXPECT_EQ((outbox[0][0] & 0xF0U), 0xF0U);
    EXPECT_EQ(listener.connection_count(), 0U);
}

TEST(QuicRequireRetry, ValidTokenCreatesValidatedConnection) {
    auto [cert, key] = make_self_signed();
    TlsServerConfig tcfg{cert, key, {"h3"}};
    auto tls_ctx = TlsContext::make_server(tcfg);
    ASSERT_NE(tls_ctx, nullptr);

    Listener::Config cfg;
    cfg.tls_ctx = tls_ctx;
    cfg.server_tp.initial_max_data = 1 << 20;
    cfg.server_tp.initial_max_streams_bidi = 4;
    cfg.random_seed = 0xABCD1234ULL;
    cfg.require_retry = true;

    std::vector<std::vector<uint8_t>> outbox;
    Listener listener(cfg,
                      [&](const PeerAddr&, std::span<const uint8_t> dg) {
                          outbox.emplace_back(dg.begin(), dg.end());
                      });

    auto peer = make_v4_peer(10, 1, 2, 3, 4444);
    std::vector<uint8_t> original_dcid = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02};
    std::vector<uint8_t> client_scid = {0x11, 0x22, 0x33, 0x44};

    // Step 1: first Initial — no token — listener emits Retry.
    auto dg1 = build_initial_with_token({original_dcid.data(), original_dcid.size()},
                                        {client_scid.data(), client_scid.size()}, {});
    listener.on_datagram(peer, {dg1.data(), dg1.size()});
    ASSERT_EQ(outbox.size(), 1U);
    auto retry = outbox.front();
    outbox.clear();
    ASSERT_EQ(listener.connection_count(), 0U);

    // Step 2: extract the Retry's SCID (becomes new DCID) + token.
    // Layout: 1 byte | 4 bytes version | dcid_len | dcid | scid_len | scid | token | 16-byte tag
    ASSERT_GE(retry.size(), 1U + 4U + 1U + 1U + 16U);
    std::size_t off = 5;
    const std::size_t r_dcid_len = retry[off++];
    off += r_dcid_len;
    const std::size_t r_scid_len = retry[off++];
    std::vector<uint8_t> retry_scid(retry.begin() + static_cast<std::ptrdiff_t>(off),
                                    retry.begin() + static_cast<std::ptrdiff_t>(off + r_scid_len));
    off += r_scid_len;
    std::vector<uint8_t> token(retry.begin() + static_cast<std::ptrdiff_t>(off),
                               retry.end() - 16); // last 16 bytes are integrity tag

    // Step 3: client sends new Initial with DCID = retry_scid and the token.
    auto dg2 = build_initial_with_token({retry_scid.data(), retry_scid.size()},
                                        {client_scid.data(), client_scid.size()},
                                        {token.data(), token.size()});
    listener.on_datagram(peer, {dg2.data(), dg2.size()});
    EXPECT_EQ(listener.connection_count(), 1U);
    auto* conn = listener.find_connection({retry_scid.data(), retry_scid.size()});
    ASSERT_NE(conn, nullptr);
    EXPECT_TRUE(conn->peer_address_validated());
}

TEST(QuicRequireRetry, TokenFromDifferentPeerIsRejected) {
    auto [cert, key] = make_self_signed();
    TlsServerConfig tcfg{cert, key, {"h3"}};
    auto tls_ctx = TlsContext::make_server(tcfg);
    ASSERT_NE(tls_ctx, nullptr);

    Listener::Config cfg;
    cfg.tls_ctx = tls_ctx;
    cfg.server_tp.initial_max_data = 1 << 20;
    cfg.server_tp.initial_max_streams_bidi = 4;
    cfg.random_seed = 0xFEEDFACEULL;
    cfg.require_retry = true;

    std::vector<std::vector<uint8_t>> outbox;
    Listener listener(cfg,
                      [&](const PeerAddr&, std::span<const uint8_t> dg) {
                          outbox.emplace_back(dg.begin(), dg.end());
                      });

    auto peer_legit = make_v4_peer(10, 1, 2, 3, 4444);
    auto peer_attacker = make_v4_peer(10, 1, 2, 4, 4444);
    std::vector<uint8_t> dcid = {0xaa, 0xbb, 0xcc, 0xdd};
    std::vector<uint8_t> scid = {0x11, 0x22, 0x33, 0x44};

    // Issue Retry to legitimate peer.
    auto dg1 = build_initial_with_token({dcid.data(), dcid.size()},
                                        {scid.data(), scid.size()}, {});
    listener.on_datagram(peer_legit, {dg1.data(), dg1.size()});
    ASSERT_EQ(outbox.size(), 1U);
    auto retry = std::move(outbox.front());
    outbox.clear();

    // Extract the SCID + token from the Retry packet.
    std::size_t off = 5;
    const std::size_t r_dcid_len = retry[off++];
    off += r_dcid_len;
    const std::size_t r_scid_len = retry[off++];
    std::vector<uint8_t> retry_scid(retry.begin() + static_cast<std::ptrdiff_t>(off),
                                    retry.begin() + static_cast<std::ptrdiff_t>(off + r_scid_len));
    off += r_scid_len;
    std::vector<uint8_t> token(retry.begin() + static_cast<std::ptrdiff_t>(off),
                               retry.end() - 16);

    // Attacker replays the token from a DIFFERENT peer address.
    auto dg2 = build_initial_with_token({retry_scid.data(), retry_scid.size()},
                                        {scid.data(), scid.size()},
                                        {token.data(), token.size()});
    listener.on_datagram(peer_attacker, {dg2.data(), dg2.size()});
    EXPECT_EQ(listener.connection_count(), 0U);
    EXPECT_TRUE(outbox.empty());
}
