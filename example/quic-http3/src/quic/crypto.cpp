#include <libspaznet/quic/crypto.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

namespace spaznet {
namespace quic {

namespace {

auto evp_md(Hash hash) -> const EVP_MD* {
    switch (hash) {
        case Hash::Sha256:
            return EVP_sha256();
        case Hash::Sha384:
            return EVP_sha384();
    }
    return nullptr;
}

auto aead_cipher(Aead aead) -> const EVP_CIPHER* {
    switch (aead) {
        case Aead::Aes128Gcm:
            return EVP_aes_128_gcm();
        case Aead::Aes256Gcm:
            return EVP_aes_256_gcm();
        case Aead::ChaCha20Poly1305:
            return EVP_chacha20_poly1305();
    }
    return nullptr;
}

[[noreturn]] auto throw_ossl(const char* what) -> void {
    char err_buf[256];
    unsigned long err = ERR_get_error();
    ERR_error_string_n(err, err_buf, sizeof(err_buf));
    throw std::runtime_error(std::string(what) + ": " + err_buf);
}

} // namespace

auto hash_length(Hash hash) -> std::size_t {
    switch (hash) {
        case Hash::Sha256:
            return 32;
        case Hash::Sha384:
            return 48;
    }
    return 0;
}

auto aead_key_length(Aead aead) -> std::size_t {
    switch (aead) {
        case Aead::Aes128Gcm:
            return 16;
        case Aead::Aes256Gcm:
            return 32;
        case Aead::ChaCha20Poly1305:
            return 32;
    }
    return 0;
}

auto aead_iv_length(Aead /*aead*/) -> std::size_t {
    return 12;
}

auto aead_tag_length(Aead /*aead*/) -> std::size_t {
    return 16;
}

auto aead_hp_length(Aead aead) -> std::size_t {
    return aead_key_length(aead);
}

auto aead_hash(Aead aead) -> Hash {
    switch (aead) {
        case Aead::Aes128Gcm:
        case Aead::ChaCha20Poly1305:
            return Hash::Sha256;
        case Aead::Aes256Gcm:
            return Hash::Sha384;
    }
    return Hash::Sha256;
}

auto hkdf_extract(Hash hash, std::span<const uint8_t> salt, std::span<const uint8_t> ikm)
    -> std::vector<uint8_t> {
    const EVP_MD* md = evp_md(hash);
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (kdf == nullptr) {
        throw_ossl("EVP_KDF_fetch(HKDF)");
    }
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (ctx == nullptr) {
        throw_ossl("EVP_KDF_CTX_new");
    }

    int mode = EVP_KDF_HKDF_MODE_EXTRACT_ONLY;
    std::string md_name = (hash == Hash::Sha256) ? "SHA256" : "SHA384";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &mode),
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, md_name.data(), 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                          const_cast<uint8_t*>(salt.data()), salt.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                          const_cast<uint8_t*>(ikm.data()), ikm.size()),
        OSSL_PARAM_construct_end()};

    std::vector<uint8_t> out(EVP_MD_size(md));
    if (EVP_KDF_derive(ctx, out.data(), out.size(), params) <= 0) {
        EVP_KDF_CTX_free(ctx);
        throw_ossl("HKDF-Extract");
    }
    EVP_KDF_CTX_free(ctx);
    return out;
}

auto hkdf_expand(Hash hash, std::span<const uint8_t> prk, std::span<const uint8_t> info,
                 std::size_t out_len) -> std::vector<uint8_t> {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (kdf == nullptr) {
        throw_ossl("EVP_KDF_fetch(HKDF)");
    }
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (ctx == nullptr) {
        throw_ossl("EVP_KDF_CTX_new");
    }

    int mode = EVP_KDF_HKDF_MODE_EXPAND_ONLY;
    std::string md_name = (hash == Hash::Sha256) ? "SHA256" : "SHA384";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &mode),
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, md_name.data(), 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                          const_cast<uint8_t*>(prk.data()), prk.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                                          const_cast<uint8_t*>(info.data()), info.size()),
        OSSL_PARAM_construct_end()};

    std::vector<uint8_t> out(out_len);
    if (EVP_KDF_derive(ctx, out.data(), out.size(), params) <= 0) {
        EVP_KDF_CTX_free(ctx);
        throw_ossl("HKDF-Expand");
    }
    EVP_KDF_CTX_free(ctx);
    return out;
}

auto hkdf_expand_label(Hash hash, std::span<const uint8_t> secret, std::string_view label,
                       std::span<const uint8_t> context, std::size_t out_len)
    -> std::vector<uint8_t> {
    // HkdfLabel = struct {
    //   uint16 length;
    //   opaque label<7..255>;   // "tls13 " + label, length-prefixed (uint8)
    //   opaque context<0..255>; // length-prefixed (uint8)
    // }
    static constexpr std::string_view kPrefix = "tls13 ";
    const std::size_t label_len = kPrefix.size() + label.size();
    if (label_len > 255 || context.size() > 255 || out_len > 0xFFFF) {
        throw std::out_of_range("hkdf_expand_label argument too large");
    }

    std::vector<uint8_t> info;
    info.reserve(2 + 1 + label_len + 1 + context.size());
    info.push_back(static_cast<uint8_t>(out_len >> 8));
    info.push_back(static_cast<uint8_t>(out_len & 0xFF));
    info.push_back(static_cast<uint8_t>(label_len));
    info.insert(info.end(), kPrefix.begin(), kPrefix.end());
    info.insert(info.end(), label.begin(), label.end());
    info.push_back(static_cast<uint8_t>(context.size()));
    info.insert(info.end(), context.begin(), context.end());

    return hkdf_expand(hash, secret, info, out_len);
}

namespace {
// RFC 9001 §5.2 — QUIC v1 Initial salt.
constexpr std::array<uint8_t, 20> kInitialSaltV1 = {0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34,
                                                    0xb3, 0x4d, 0x17, 0x9a, 0xe6, 0xa4, 0xc8,
                                                    0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a};
} // namespace

auto initial_salt_v1() -> std::span<const uint8_t> {
    return {kInitialSaltV1.data(), kInitialSaltV1.size()};
}

auto derive_initial_secret(std::span<const uint8_t> client_dcid, Direction dir)
    -> std::vector<uint8_t> {
    auto initial_secret = hkdf_extract(Hash::Sha256, initial_salt_v1(), client_dcid);
    const std::string_view label = (dir == Direction::Client) ? "client in" : "server in";
    return hkdf_expand_label(Hash::Sha256, initial_secret, label, {}, hash_length(Hash::Sha256));
}

auto derive_packet_keys(Aead aead, std::span<const uint8_t> secret) -> PacketKeys {
    PacketKeys out;
    Hash hash = aead_hash(aead);
    out.key = hkdf_expand_label(hash, secret, "quic key", {}, aead_key_length(aead));
    out.iv = hkdf_expand_label(hash, secret, "quic iv", {}, aead_iv_length(aead));
    out.hp = hkdf_expand_label(hash, secret, "quic hp", {}, aead_hp_length(aead));
    return out;
}

auto aead_seal(Aead aead, std::span<const uint8_t> key, std::span<const uint8_t> nonce,
               std::span<const uint8_t> aad, std::span<const uint8_t> plaintext,
               std::vector<uint8_t>& out) -> bool {
    const EVP_CIPHER* cipher = aead_cipher(aead);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return false;
    }
    const std::size_t tag_len = aead_tag_length(aead);
    out.assign(plaintext.size() + tag_len, 0);

    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()),
                                nullptr) != 1) {
            break;
        }
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
            break;
        }
        int outlen = 0;
        if (!aad.empty()) {
            if (EVP_EncryptUpdate(ctx, nullptr, &outlen, aad.data(),
                                  static_cast<int>(aad.size())) != 1) {
                break;
            }
        }
        if (EVP_EncryptUpdate(ctx, out.data(), &outlen, plaintext.data(),
                              static_cast<int>(plaintext.size())) != 1) {
            break;
        }
        int finlen = 0;
        if (EVP_EncryptFinal_ex(ctx, out.data() + outlen, &finlen) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(tag_len),
                                out.data() + plaintext.size()) != 1) {
            break;
        }
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        out.clear();
    }
    return ok;
}

auto aead_open(Aead aead, std::span<const uint8_t> key, std::span<const uint8_t> nonce,
               std::span<const uint8_t> aad, std::span<const uint8_t> ciphertext,
               std::vector<uint8_t>& out) -> bool {
    const std::size_t tag_len = aead_tag_length(aead);
    if (ciphertext.size() < tag_len) {
        out.clear();
        return false;
    }
    const std::size_t pt_len = ciphertext.size() - tag_len;

    const EVP_CIPHER* cipher = aead_cipher(aead);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        out.clear();
        return false;
    }
    out.assign(pt_len, 0);

    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()),
                                nullptr) != 1) {
            break;
        }
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
            break;
        }
        int outlen = 0;
        if (!aad.empty()) {
            if (EVP_DecryptUpdate(ctx, nullptr, &outlen, aad.data(),
                                  static_cast<int>(aad.size())) != 1) {
                break;
            }
        }
        if (EVP_DecryptUpdate(ctx, out.data(), &outlen, ciphertext.data(),
                              static_cast<int>(pt_len)) != 1) {
            break;
        }
        // The auth tag is at the end of the ciphertext.
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tag_len),
                                const_cast<uint8_t*>(ciphertext.data() + pt_len)) != 1) {
            break;
        }
        int finlen = 0;
        if (EVP_DecryptFinal_ex(ctx, out.data() + outlen, &finlen) != 1) {
            break;
        }
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        out.clear();
    }
    return ok;
}

auto aead_seal_inplace(Aead aead, std::span<const uint8_t> key, std::span<const uint8_t> nonce,
                       std::span<const uint8_t> aad, std::span<uint8_t> body,
                       std::span<uint8_t> tag_out) -> bool {
    const std::size_t tag_len = aead_tag_length(aead);
    if (tag_out.size() != tag_len) {
        return false;
    }
    const EVP_CIPHER* cipher = aead_cipher(aead);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return false;
    }
    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()),
                                nullptr) != 1) {
            break;
        }
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
            break;
        }
        int outlen = 0;
        if (!aad.empty()) {
            if (EVP_EncryptUpdate(ctx, nullptr, &outlen, aad.data(),
                                  static_cast<int>(aad.size())) != 1) {
                break;
            }
        }
        // AES-GCM permits in-place encryption: same buffer as input
        // and output is legal because the cipher operates in counter
        // mode XOR-ing keystream over the plaintext byte by byte.
        if (!body.empty()) {
            if (EVP_EncryptUpdate(ctx, body.data(), &outlen, body.data(),
                                  static_cast<int>(body.size())) != 1) {
                break;
            }
        }
        int finlen = 0;
        // Tag-only ciphers (GCM) have no remaining output here; pass a
        // throwaway 16-byte buffer so EVP_EncryptFinal_ex never writes
        // past the end of the caller's body span. AES-GCM and
        // ChaCha20-Poly1305 both emit zero bytes here.
        std::array<uint8_t, 16> finish_pad{};
        if (EVP_EncryptFinal_ex(ctx, finish_pad.data(), &finlen) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(tag_len),
                                tag_out.data()) != 1) {
            break;
        }
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

auto aead_open_inplace(Aead aead, std::span<const uint8_t> key, std::span<const uint8_t> nonce,
                       std::span<const uint8_t> aad, std::span<uint8_t> body,
                       std::span<const uint8_t> tag_in) -> bool {
    const std::size_t tag_len = aead_tag_length(aead);
    if (tag_in.size() != tag_len) {
        return false;
    }
    const EVP_CIPHER* cipher = aead_cipher(aead);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return false;
    }
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce.size()),
                                nullptr) != 1) {
            break;
        }
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1) {
            break;
        }
        int outlen = 0;
        if (!aad.empty()) {
            if (EVP_DecryptUpdate(ctx, nullptr, &outlen, aad.data(),
                                  static_cast<int>(aad.size())) != 1) {
                break;
            }
        }
        if (!body.empty()) {
            if (EVP_DecryptUpdate(ctx, body.data(), &outlen, body.data(),
                                  static_cast<int>(body.size())) != 1) {
                break;
            }
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tag_len),
                                const_cast<uint8_t*>(tag_in.data())) != 1) {
            break;
        }
        int finlen = 0;
        std::array<uint8_t, 16> finish_pad{};
        if (EVP_DecryptFinal_ex(ctx, finish_pad.data(), &finlen) != 1) {
            break;
        }
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

// ---- CipherCtx ----------------------------------------------------------

CipherCtx::~CipherCtx() {
    reset();
}

CipherCtx::CipherCtx(CipherCtx&& other) noexcept
    : ctx_(other.ctx_), aead_(other.aead_), dir_(other.dir_) {
    other.ctx_ = nullptr;
}

auto CipherCtx::operator=(CipherCtx&& other) noexcept -> CipherCtx& {
    if (this != &other) {
        reset();
        ctx_ = other.ctx_;
        aead_ = other.aead_;
        dir_ = other.dir_;
        other.ctx_ = nullptr;
    }
    return *this;
}

auto CipherCtx::reset() -> void {
    if (ctx_ != nullptr) {
        EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX*>(ctx_));
        ctx_ = nullptr;
    }
}

auto CipherCtx::init(Aead aead, std::span<const uint8_t> key, Direction dir) -> bool {
    reset();
    const EVP_CIPHER* cipher = aead_cipher(aead);
    if (cipher == nullptr) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return false;
    bool ok = false;
    if (dir == Direction::Encrypt) {
        ok = EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                 static_cast<int>(aead_iv_length(aead)), nullptr) == 1 &&
             EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nullptr) == 1;
    } else {
        ok = EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                                 static_cast<int>(aead_iv_length(aead)), nullptr) == 1 &&
             EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nullptr) == 1;
    }
    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    ctx_ = ctx;
    aead_ = aead;
    dir_ = dir;
    return true;
}

auto CipherCtx::seal_inplace(std::span<const uint8_t> nonce, std::span<const uint8_t> aad,
                             std::span<uint8_t> body, std::span<uint8_t> tag_out) -> bool {
    if (ctx_ == nullptr || dir_ != Direction::Encrypt) return false;
    const std::size_t tag_len = aead_tag_length(aead_);
    if (tag_out.size() != tag_len) return false;
    auto* ctx = static_cast<EVP_CIPHER_CTX*>(ctx_);
    // Reset only the IV; cipher + key are already bound.
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, nullptr, nonce.data()) != 1) {
        return false;
    }
    int outlen = 0;
    if (!aad.empty()) {
        if (EVP_EncryptUpdate(ctx, nullptr, &outlen, aad.data(),
                              static_cast<int>(aad.size())) != 1) {
            return false;
        }
    }
    if (!body.empty()) {
        if (EVP_EncryptUpdate(ctx, body.data(), &outlen, body.data(),
                              static_cast<int>(body.size())) != 1) {
            return false;
        }
    }
    int finlen = 0;
    std::array<uint8_t, 16> finish_pad{};
    if (EVP_EncryptFinal_ex(ctx, finish_pad.data(), &finlen) != 1) {
        return false;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(tag_len),
                            tag_out.data()) != 1) {
        return false;
    }
    return true;
}

auto CipherCtx::open_inplace(std::span<const uint8_t> nonce, std::span<const uint8_t> aad,
                             std::span<uint8_t> body, std::span<const uint8_t> tag_in) -> bool {
    if (ctx_ == nullptr || dir_ != Direction::Decrypt) return false;
    const std::size_t tag_len = aead_tag_length(aead_);
    if (tag_in.size() != tag_len) return false;
    auto* ctx = static_cast<EVP_CIPHER_CTX*>(ctx_);
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, nullptr, nonce.data()) != 1) {
        return false;
    }
    int outlen = 0;
    if (!aad.empty()) {
        if (EVP_DecryptUpdate(ctx, nullptr, &outlen, aad.data(),
                              static_cast<int>(aad.size())) != 1) {
            return false;
        }
    }
    if (!body.empty()) {
        if (EVP_DecryptUpdate(ctx, body.data(), &outlen, body.data(),
                              static_cast<int>(body.size())) != 1) {
            return false;
        }
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tag_len),
                            const_cast<uint8_t*>(tag_in.data())) != 1) {
        return false;
    }
    int finlen = 0;
    std::array<uint8_t, 16> finish_pad{};
    if (EVP_DecryptFinal_ex(ctx, finish_pad.data(), &finlen) != 1) {
        return false;
    }
    return true;
}

auto header_protection_mask(Aead aead, std::span<const uint8_t> hp_key,
                            std::span<const uint8_t> sample) -> std::array<uint8_t, 5> {
    if (sample.size() != 16) {
        throw std::invalid_argument("header_protection_mask: sample must be 16 bytes");
    }
    std::array<uint8_t, 5> mask{};

    if (aead == Aead::Aes128Gcm || aead == Aead::Aes256Gcm) {
        // mask = AES-ECB(hp_key, sample)[0..4]
        const EVP_CIPHER* cipher =
            (aead == Aead::Aes128Gcm) ? EVP_aes_128_ecb() : EVP_aes_256_ecb();
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (ctx == nullptr) {
            throw_ossl("EVP_CIPHER_CTX_new");
        }
        std::array<uint8_t, 16> out{};
        int outlen = 0;
        if (EVP_EncryptInit_ex(ctx, cipher, nullptr, hp_key.data(), nullptr) != 1 ||
            EVP_CIPHER_CTX_set_padding(ctx, 0) != 1 ||
            EVP_EncryptUpdate(ctx, out.data(), &outlen, sample.data(),
                              static_cast<int>(sample.size())) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            throw_ossl("AES-ECB header protection");
        }
        EVP_CIPHER_CTX_free(ctx);
        std::memcpy(mask.data(), out.data(), 5);
        return mask;
    }

    // ChaCha20: counter = u32_le(sample[0..3]); nonce = sample[4..15];
    //          mask = ChaCha20(hp_key, counter, nonce, zeros[0..4])
    std::array<uint8_t, 16> iv{};
    std::memcpy(iv.data(), sample.data(), 16);

    const EVP_CIPHER* cipher = EVP_chacha20();
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        throw_ossl("EVP_CIPHER_CTX_new");
    }
    std::array<uint8_t, 5> zeros{};
    std::array<uint8_t, 16> tmp{};
    int outlen = 0;
    if (EVP_EncryptInit_ex(ctx, cipher, nullptr, hp_key.data(), iv.data()) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx, 0) != 1 ||
        EVP_EncryptUpdate(ctx, tmp.data(), &outlen, zeros.data(),
                          static_cast<int>(zeros.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw_ossl("ChaCha20 header protection");
    }
    EVP_CIPHER_CTX_free(ctx);
    std::memcpy(mask.data(), tmp.data(), 5);
    return mask;
}

} // namespace quic
} // namespace spaznet
