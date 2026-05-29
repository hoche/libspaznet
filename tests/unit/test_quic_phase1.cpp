// Phase 1 foundation tests: variable-length integers, key derivation, and
// the full Initial-packet protection round trip against the test vectors
// in RFC 9001 Appendix A.2 (client) and A.3 (server).

#include <gtest/gtest.h>

#include <libspaznet/quic/crypto.hpp>
#include <libspaznet/quic/packet.hpp>
#include <libspaznet/quic/varint.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using spaznet::quic::Aead;
using spaznet::quic::Direction;
using spaznet::quic::Hash;
using spaznet::quic::PacketKeys;
using spaznet::quic::VarInt;

auto hex(std::string s) -> std::vector<uint8_t> {
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](unsigned char c) { return std::isspace(c) != 0; }),
            s.end());
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        auto nybble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        out.push_back(static_cast<uint8_t>((nybble(s[i]) << 4) | nybble(s[i + 1])));
    }
    return out;
}

} // namespace

TEST(QuicVarInt, Rfc9000AppendixA1Vectors) {
    // 8-byte form, value 151288809941952652.
    EXPECT_EQ(hex("c2197c5eff14e88c"), VarInt::encode(151288809941952652ULL));
    // 4-byte form, value 494878333.
    EXPECT_EQ(hex("9d7f3e7d"), VarInt::encode(494878333ULL));
    // 2-byte form, value 15293.
    EXPECT_EQ(hex("7bbd"), VarInt::encode(15293ULL));
    // 1-byte form, value 37 (encoded 0x25); also alt 2-byte 0x4025 — we
    // only emit the shortest form.
    EXPECT_EQ(hex("25"), VarInt::encode(37ULL));
}

TEST(QuicVarInt, RoundTrip) {
    for (uint64_t v : {0ULL, 1ULL, 63ULL, 64ULL, 16383ULL, 16384ULL, (1ULL << 30) - 1,
                       1ULL << 30, (1ULL << 62) - 1}) {
        auto enc = VarInt::encode(v);
        std::size_t off = 0;
        uint64_t decoded = 0;
        ASSERT_TRUE(VarInt::decode(enc.data(), enc.size(), off, decoded)) << v;
        EXPECT_EQ(off, enc.size()) << v;
        EXPECT_EQ(decoded, v);
    }
}

TEST(QuicVarInt, DecodeTruncated) {
    // 8-byte form needs 8 bytes; supply only 5.
    auto enc = VarInt::encode(151288809941952652ULL);
    std::size_t off = 0;
    uint64_t decoded = 0;
    EXPECT_FALSE(VarInt::decode(enc.data(), 5, off, decoded));
}

TEST(QuicCrypto, CipherCtxMatchesFreeFunction) {
    auto secret = spaznet::quic::derive_initial_secret(hex("8394c8f03e515708"),
                                                       Direction::Client);
    auto keys = spaznet::quic::derive_packet_keys(Aead::Aes128Gcm, secret);
    std::vector<uint8_t> aad = {0x11, 0x22, 0x33};
    std::vector<uint8_t> plaintext;
    for (int i = 0; i < 150; ++i) plaintext.push_back(static_cast<uint8_t>(i * 7));

    // Reference via the free function.
    std::vector<uint8_t> sealed_ref;
    ASSERT_TRUE(spaznet::quic::aead_seal(
        Aead::Aes128Gcm, {keys.key.data(), keys.key.size()},
        {keys.iv.data(), keys.iv.size()}, {aad.data(), aad.size()},
        {plaintext.data(), plaintext.size()}, sealed_ref));

    // Cached context.
    spaznet::quic::CipherCtx enc;
    ASSERT_TRUE(enc.init(Aead::Aes128Gcm, {keys.key.data(), keys.key.size()},
                         spaznet::quic::CipherCtx::Direction::Encrypt));
    std::vector<uint8_t> body = plaintext;
    std::array<uint8_t, 16> tag{};
    ASSERT_TRUE(enc.seal_inplace({keys.iv.data(), keys.iv.size()},
                                  {aad.data(), aad.size()},
                                  {body.data(), body.size()},
                                  {tag.data(), tag.size()}));
    EXPECT_EQ(std::vector<uint8_t>(sealed_ref.begin(), sealed_ref.begin() + plaintext.size()),
              body);
    EXPECT_EQ(std::vector<uint8_t>(sealed_ref.begin() + plaintext.size(), sealed_ref.end()),
              std::vector<uint8_t>(tag.begin(), tag.end()));

    // Re-use the same context for a second packet with a different IV
    // — that's the whole point of caching.
    std::vector<uint8_t> iv2(keys.iv.begin(), keys.iv.end());
    iv2.back() ^= 0xAB; // any different nonce
    std::vector<uint8_t> body2 = plaintext;
    std::array<uint8_t, 16> tag2{};
    ASSERT_TRUE(enc.seal_inplace({iv2.data(), iv2.size()},
                                  {aad.data(), aad.size()},
                                  {body2.data(), body2.size()},
                                  {tag2.data(), tag2.size()}));
    EXPECT_NE(body, body2); // different IV must yield different ciphertext

    // Decrypt cache works too.
    spaznet::quic::CipherCtx dec;
    ASSERT_TRUE(dec.init(Aead::Aes128Gcm, {keys.key.data(), keys.key.size()},
                         spaznet::quic::CipherCtx::Direction::Decrypt));
    ASSERT_TRUE(dec.open_inplace({iv2.data(), iv2.size()},
                                  {aad.data(), aad.size()},
                                  {body2.data(), body2.size()},
                                  {tag2.data(), tag2.size()}));
    EXPECT_EQ(body2, plaintext);
}

TEST(QuicCrypto, AeadInplaceMatchesCopyVariant) {
    auto secret = spaznet::quic::derive_initial_secret(hex("8394c8f03e515708"),
                                                       Direction::Client);
    auto keys = spaznet::quic::derive_packet_keys(Aead::Aes128Gcm, secret);
    std::vector<uint8_t> aad = {0x01, 0x02, 0x03, 0x04, 0x05};
    std::vector<uint8_t> plaintext;
    for (int i = 0; i < 200; ++i) plaintext.push_back(static_cast<uint8_t>(i));
    std::vector<uint8_t> nonce(keys.iv.begin(), keys.iv.end());

    // Reference: copy variant.
    std::vector<uint8_t> sealed_ref;
    ASSERT_TRUE(spaznet::quic::aead_seal(
        Aead::Aes128Gcm, {keys.key.data(), keys.key.size()},
        {nonce.data(), nonce.size()}, {aad.data(), aad.size()},
        {plaintext.data(), plaintext.size()}, sealed_ref));
    ASSERT_EQ(sealed_ref.size(), plaintext.size() + 16U);

    // In-place: body holds plaintext on entry, ciphertext on exit;
    // tag goes into a 16-byte trailer.
    std::vector<uint8_t> body = plaintext;
    std::array<uint8_t, 16> tag{};
    ASSERT_TRUE(spaznet::quic::aead_seal_inplace(
        Aead::Aes128Gcm, {keys.key.data(), keys.key.size()},
        {nonce.data(), nonce.size()}, {aad.data(), aad.size()},
        {body.data(), body.size()}, {tag.data(), tag.size()}));
    EXPECT_EQ(std::vector<uint8_t>(sealed_ref.begin(), sealed_ref.begin() + plaintext.size()),
              body);
    EXPECT_EQ(std::vector<uint8_t>(sealed_ref.begin() + plaintext.size(), sealed_ref.end()),
              std::vector<uint8_t>(tag.begin(), tag.end()));

    // And the inverse decrypts back to the plaintext.
    ASSERT_TRUE(spaznet::quic::aead_open_inplace(
        Aead::Aes128Gcm, {keys.key.data(), keys.key.size()},
        {nonce.data(), nonce.size()}, {aad.data(), aad.size()},
        {body.data(), body.size()}, {tag.data(), tag.size()}));
    EXPECT_EQ(body, plaintext);
}

// RFC 9001 Appendix A.1: client DCID derives the Initial secrets.
TEST(QuicCrypto, Rfc9001AppA1_InitialSecrets) {
    auto client_dcid = hex("8394c8f03e515708");

    // initial_secret = HKDF-Extract(initial_salt, client_dcid)
    auto initial_secret = spaznet::quic::hkdf_extract(
        Hash::Sha256, spaznet::quic::initial_salt_v1(), client_dcid);
    EXPECT_EQ(hex("7db5df06e7a69e432496adedb0085192"
                  "3595221596ae2ae9fb8115c1e9ed0a44"),
              initial_secret);

    auto client_initial = spaznet::quic::derive_initial_secret(client_dcid, Direction::Client);
    EXPECT_EQ(hex("c00cf151ca5be075ed0ebfb5c80323c4"
                  "2d6b7db67881289af4008f1f6c357aea"),
              client_initial);

    auto server_initial = spaznet::quic::derive_initial_secret(client_dcid, Direction::Server);
    EXPECT_EQ(hex("3c199828fd139efd216c155ad844cc81"
                  "fb82fa8d7446fa7d78be803acdda951b"),
              server_initial);

    // Derived per-direction packet keys (RFC 9001 §A.1).
    PacketKeys ck = spaznet::quic::derive_packet_keys(Aead::Aes128Gcm, client_initial);
    EXPECT_EQ(hex("1f369613dd76d5467730efcbe3b1a22d"), ck.key);
    EXPECT_EQ(hex("fa044b2f42a3fd3b46fb255c"), ck.iv);
    EXPECT_EQ(hex("9f50449e04a0e810283a1e9933adedd2"), ck.hp);

    PacketKeys sk = spaznet::quic::derive_packet_keys(Aead::Aes128Gcm, server_initial);
    EXPECT_EQ(hex("cf3a5331653c364c88f0f379b6067e37"), sk.key);
    EXPECT_EQ(hex("0ac1493ca1905853b0bba03e"), sk.iv);
    EXPECT_EQ(hex("c206b8d9b9f0f37644430b490eeaa314"), sk.hp);
}

// RFC 9001 Appendix A.2 — client Initial packet (with single CRYPTO frame
// carrying the ClientHello, PADDING-frame stuffed to 1200 bytes).
TEST(QuicPacket, Rfc9001AppA2_ClientInitial_BuildMatches) {
    auto client_dcid = hex("8394c8f03e515708");
    auto secret = spaznet::quic::derive_initial_secret(client_dcid, Direction::Client);
    auto keys = spaznet::quic::derive_packet_keys(Aead::Aes128Gcm, secret);

    // Frames payload: CRYPTO frame containing the example ClientHello,
    // followed by PADDING frames out to 1162 bytes (RFC 9001 §A.2 text).
    auto crypto_frame = hex(
        "060040f1010000ed0303ebf8fa56f129"
        "39b9584a3896472ec40bb863cfd3e868"
        "04fe3a47f06a2b69484c000004130113"
        "02010000c000000010000e00000b6578"
        "616d706c652e636f6dff01000100000a"
        "00080006001d00170018001000070005"
        "04616c706e0005000501000000000033"
        "00260024001d00209370b2c9caa47fba"
        "baf4559fedba753de171fa71f50f1ce1"
        "5d43e994ec74d748002b000302030400"
        "0d0010000e0403050306030203080408"
        "05080604010002001000000039003200"
        "0408ffffffffffffffff050480008000"
        "0604800080000704800080000801100a"
        "010c0d0080240a00000100");
    std::vector<uint8_t> payload = crypto_frame;
    payload.resize(1162, 0x00); // PADDING frames are 0x00.

    auto pkt = spaznet::quic::build_initial_packet(
        Aead::Aes128Gcm, keys,
        /*dcid=*/{client_dcid.data(), client_dcid.size()},
        /*scid=*/{}, /*token=*/{},
        {payload.data(), payload.size()}, /*packet_number=*/2);

    // Expected ciphertext + protected header from RFC 9001 §A.2.
    auto expected_first_bytes =
        hex("c000000001088394c8f03e5157080000449e7b9aec34");
    ASSERT_GE(pkt.size(), expected_first_bytes.size());
    EXPECT_EQ(std::vector<uint8_t>(pkt.begin(),
                                   pkt.begin() + static_cast<std::ptrdiff_t>(
                                                     expected_first_bytes.size())),
              expected_first_bytes);
    // The full datagram is 1200 bytes.
    EXPECT_EQ(pkt.size(), 1200U);
}

// Round-trip: build a server Initial and decrypt it back to the same
// payload using the server-direction keys.
TEST(QuicPacket, ServerInitialRoundTrip) {
    auto client_dcid = hex("8394c8f03e515708");
    auto secret = spaznet::quic::derive_initial_secret(client_dcid, Direction::Server);
    auto keys = spaznet::quic::derive_packet_keys(Aead::Aes128Gcm, secret);

    auto server_scid = hex("f067a5502a4262b5");
    std::vector<uint8_t> payload =
        hex("02000000000600405a020000560303ee"
            "fce7f7b37ba1d1632e96677825ddf739"
            "88cfc79825df566dc5430b9a045a1200"
            "130100002e00330024001d00209d3c94"
            "0d89690b84d08a60993c144eca684d10"
            "81287c834d5311bcf32bb9da1a002b00"
            "020304");

    auto pkt = spaznet::quic::build_initial_packet(Aead::Aes128Gcm, keys,
                                                   /*dcid=*/{}, // server picks empty
                                                   {server_scid.data(), server_scid.size()},
                                                   /*token=*/{},
                                                   {payload.data(), payload.size()},
                                                   /*packet_number=*/1);

    // Parse the long header back.
    std::vector<uint8_t> datagram = pkt;
    std::size_t off = 0;
    spaznet::quic::LongHeader hdr;
    ASSERT_TRUE(spaznet::quic::parse_long_header({datagram.data(), datagram.size()}, off, hdr));
    EXPECT_EQ(hdr.type, spaznet::quic::LongType::Initial);
    EXPECT_EQ(hdr.version, spaznet::quic::kQuicV1);
    EXPECT_EQ(hdr.scid, server_scid);

    uint64_t pn = 0;
    std::vector<uint8_t> plaintext;
    ASSERT_TRUE(spaznet::quic::decrypt_long_packet(Aead::Aes128Gcm, keys, 0, datagram, hdr, pn,
                                                   plaintext));
    EXPECT_EQ(pn, 1U);
    EXPECT_EQ(plaintext, payload);
}
