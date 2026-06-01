#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace spaznet {
namespace quic {

// AEAD algorithms used by QUIC v1 (RFC 9001 §5.1).
enum class Aead : uint8_t {
    Aes128Gcm,   // TLS_AES_128_GCM_SHA256
    Aes256Gcm,   // TLS_AES_256_GCM_SHA384
    ChaCha20Poly1305, // TLS_CHACHA20_POLY1305_SHA256
};

// The TLS hash paired with the cipher suite.
enum class Hash : uint8_t {
    Sha256,
    Sha384,
};

// Hash output length in bytes.
auto hash_length(Hash hash) -> std::size_t;

// AEAD parameters for the given suite.
auto aead_key_length(Aead aead) -> std::size_t;
auto aead_iv_length(Aead aead) -> std::size_t;
auto aead_tag_length(Aead aead) -> std::size_t;
// Header-protection key length (matches the AEAD key size).
auto aead_hp_length(Aead aead) -> std::size_t;
// The hash paired with this AEAD by the TLS cipher suite.
auto aead_hash(Aead aead) -> Hash;

// HKDF-Extract per RFC 5869.
auto hkdf_extract(Hash hash, std::span<const uint8_t> salt, std::span<const uint8_t> ikm)
    -> std::vector<uint8_t>;

// HKDF-Expand per RFC 5869.
auto hkdf_expand(Hash hash, std::span<const uint8_t> prk, std::span<const uint8_t> info,
                 std::size_t out_len) -> std::vector<uint8_t>;

// HKDF-Expand-Label per RFC 8446 §7.1.
//
//   HkdfLabel = struct { uint16 length; opaque label<7..255>; opaque context<0..255>; }
//   label_prefix = "tls13 "
auto hkdf_expand_label(Hash hash, std::span<const uint8_t> secret, std::string_view label,
                       std::span<const uint8_t> context, std::size_t out_len)
    -> std::vector<uint8_t>;

// QUIC v1 Initial salt (RFC 9001 §5.2).
auto initial_salt_v1() -> std::span<const uint8_t>;

// Derive the per-direction Initial secret from the client's chosen DCID.
// Returns the Initial secret (PRK) for the requested direction.
//   client_initial_secret = HKDF-Expand-Label(initial_secret, "client in", "", Hash.length)
//   server_initial_secret = HKDF-Expand-Label(initial_secret, "server in", "", Hash.length)
enum class Direction : uint8_t { Client, Server };
auto derive_initial_secret(std::span<const uint8_t> client_dcid, Direction dir)
    -> std::vector<uint8_t>;

// Per-packet keys derived from a traffic secret (RFC 9001 §5.1).
struct PacketKeys {
    std::vector<uint8_t> key; // AEAD key
    std::vector<uint8_t> iv;  // 12-byte AEAD nonce base
    std::vector<uint8_t> hp;  // Header-protection key
};
auto derive_packet_keys(Aead aead, std::span<const uint8_t> secret) -> PacketKeys;

// RFC 9001 §6.1 — derive the next-generation 1-RTT traffic secret
// from the current one.  Used for QUIC key update:
//   next_secret = HKDF-Expand-Label(current_secret, "quic ku", "",
//                                   hash_length(hash))
// The corresponding packet keys are derived from `next_secret` via
// the usual `derive_packet_keys` (which uses the "quic key" / "quic
// iv" / "quic hp" labels) — note however that RFC 9001 §6 leaves
// the header-protection key unchanged across updates, so callers
// should reuse the old `PacketKeys::hp` rather than the value
// computed from the next secret.
auto derive_next_application_secret(Hash hash, std::span<const uint8_t> current_secret)
    -> std::vector<uint8_t>;

// AEAD encrypt/decrypt. `nonce` must be aead_iv_length(aead) bytes long; in
// QUIC this is the iv XOR packet-number, computed by the caller.
//
// On encrypt, `out` is resized to plaintext_len + tag_len and contains the
// ciphertext followed by the auth tag.
// On decrypt, `out` is resized to ciphertext_len - tag_len. Returns false on
// authentication failure (and `out` is left empty).
auto aead_seal(Aead aead, std::span<const uint8_t> key, std::span<const uint8_t> nonce,
               std::span<const uint8_t> aad, std::span<const uint8_t> plaintext,
               std::vector<uint8_t>& out) -> bool;

[[nodiscard]] auto aead_open(Aead aead, std::span<const uint8_t> key,
                             std::span<const uint8_t> nonce, std::span<const uint8_t> aad,
                             std::span<const uint8_t> ciphertext, std::vector<uint8_t>& out)
    -> bool;

// In-place AEAD variants. `body` is encrypted/decrypted into the same
// bytes; `tag_out` (16 bytes) receives the auth tag on seal, and
// `tag_in` (16 bytes) supplies it on open. Avoids the
// allocate-and-copy that `aead_seal`/`aead_open` do internally, so
// they're suitable for the per-packet hot path.
auto aead_seal_inplace(Aead aead, std::span<const uint8_t> key,
                       std::span<const uint8_t> nonce, std::span<const uint8_t> aad,
                       std::span<uint8_t> body, std::span<uint8_t> tag_out) -> bool;

[[nodiscard]] auto aead_open_inplace(Aead aead, std::span<const uint8_t> key,
                                     std::span<const uint8_t> nonce,
                                     std::span<const uint8_t> aad, std::span<uint8_t> body,
                                     std::span<const uint8_t> tag_in) -> bool;

// RAII wrapper around an EVP_CIPHER_CTX that caches the AEAD cipher
// binding and the per-direction key. Each call to seal_inplace /
// open_inplace then has to reset only the IV — saving an
// EVP_CIPHER_CTX_new + EVP_EncryptInit_ex(cipher, key) + free per
// packet, which the QUIC perf profile flagged as the dominant
// per-packet cost above the AEAD math itself.
class CipherCtx {
  public:
    enum class Direction : uint8_t { Encrypt, Decrypt };

    CipherCtx() = default;
    ~CipherCtx();
    CipherCtx(const CipherCtx&) = delete;
    auto operator=(const CipherCtx&) -> CipherCtx& = delete;
    CipherCtx(CipherCtx&& other) noexcept;
    auto operator=(CipherCtx&& other) noexcept -> CipherCtx&;

    // Allocate the context and bind cipher + IV length + key. May be
    // called more than once on the same object; each call discards the
    // previous binding.
    [[nodiscard]] auto init(Aead aead, std::span<const uint8_t> key, Direction dir) -> bool;

    // True once init() has succeeded.
    [[nodiscard]] auto ready() const -> bool {
        return ctx_ != nullptr;
    }
    [[nodiscard]] auto aead() const -> Aead {
        return aead_;
    }

    // Per-packet seal/open. Layout: `aad` ranges over the AAD bytes;
    // `body` is encrypted/decrypted in place; `tag_out`/`tag_in` is
    // the 16-byte AEAD auth tag in/out.
    [[nodiscard]] auto seal_inplace(std::span<const uint8_t> nonce,
                                    std::span<const uint8_t> aad, std::span<uint8_t> body,
                                    std::span<uint8_t> tag_out) -> bool;
    [[nodiscard]] auto open_inplace(std::span<const uint8_t> nonce,
                                    std::span<const uint8_t> aad, std::span<uint8_t> body,
                                    std::span<const uint8_t> tag_in) -> bool;

  private:
    auto reset() -> void;

    // Held as `void*` to keep this header free of OpenSSL includes;
    // crypto.cpp casts back to `EVP_CIPHER_CTX*` when using it.
    void* ctx_{nullptr};
    Aead aead_{Aead::Aes128Gcm};
    Direction dir_{Direction::Encrypt};
};

// Header-protection mask (RFC 9001 §5.4).
//
//   AES-128/256: mask = AES-ECB(hp_key, sample)[0..4]
//   ChaCha20:    counter = u32_le(sample[0..3]); nonce = sample[4..15];
//                mask = ChaCha20(hp_key, counter, nonce, zeros[0..4])
//
// `sample` must be exactly 16 bytes; the function returns the first 5 mask
// bytes (1 byte for the header byte + 4 for the longest packet number).
auto header_protection_mask(Aead aead, std::span<const uint8_t> hp_key,
                            std::span<const uint8_t> sample) -> std::array<uint8_t, 5>;

} // namespace quic
} // namespace spaznet
