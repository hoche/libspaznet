#pragma once

// Header-only helper to mint a short-lived self-signed P-256 cert+key PEM
// pair for demos and tests. Requires SPAZNET_HAS_TLS (OpenSSL linked).

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace spaznet::detail {

inline auto make_self_signed_pem(const char* cn = "localhost")
    -> std::pair<std::string, std::string> {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (pkey == nullptr) {
        throw std::runtime_error("EVP_EC_gen failed");
    }
#else
    EC_KEY* ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (ec == nullptr || EC_KEY_generate_key(ec) != 1) {
        if (ec != nullptr) {
            EC_KEY_free(ec);
        }
        throw std::runtime_error("EC_KEY_generate_key failed");
    }
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (pkey == nullptr || EVP_PKEY_assign_EC_KEY(pkey, ec) != 1) {
        EC_KEY_free(ec);
        if (pkey != nullptr) {
            EVP_PKEY_free(pkey);
        }
        throw std::runtime_error("EVP_PKEY_assign_EC_KEY failed");
    }
#endif

    X509* x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600 * 24);
    X509_set_pubkey(x, pkey);
    X509_NAME* nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(x, nm);
    X509_EXTENSION* san_ext = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_subject_alt_name,
        const_cast<char*>("DNS:localhost,IP:127.0.0.1"));
    if (san_ext != nullptr) {
        X509_add_ext(x, san_ext, -1);
        X509_EXTENSION_free(san_ext);
    }
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
    return {std::move(cpem), std::move(kpem)};
}

} // namespace spaznet::detail
