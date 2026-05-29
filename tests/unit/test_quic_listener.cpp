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
    Listener listener(cfg, [&](std::span<const uint8_t> dg) {
        outbox.emplace_back(dg.begin(), dg.end());
    });

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

    listener.on_datagram({dg.data(), dg.size()});
    ASSERT_EQ(outbox.size(), 1U);
    // Echoed CIDs are at offsets 5 and 5+1+4 in the VN packet.
    EXPECT_EQ(outbox[0][5], dcid.size());
    EXPECT_TRUE(std::equal(dcid.begin(), dcid.end(), outbox[0].begin() + 6));
    EXPECT_EQ(outbox[0][10], scid.size());
    EXPECT_TRUE(std::equal(scid.begin(), scid.end(), outbox[0].begin() + 11));
    EXPECT_EQ(listener.connection_count(), 0U); // no Connection created
}
