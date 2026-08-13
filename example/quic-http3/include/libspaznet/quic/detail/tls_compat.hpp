#pragma once

// Internal TLS-backend include shim for example/quic-http3.
// Public headers stay free of OpenSSL/wolfSSL types; .cpp and tests
// include this instead of raw openssl/*.h / wolfssl/*.h.

#if defined(SPAZNET_TLS_WOLFSSL)
#include <wolfssl/options.h>
#include <wolfssl/openssl/bio.h>
#include <wolfssl/openssl/ec.h>
#include <wolfssl/openssl/err.h>
#include <wolfssl/openssl/evp.h>
#include <wolfssl/openssl/pem.h>
#include <wolfssl/openssl/ssl.h>
#include <wolfssl/openssl/x509.h>
#include <wolfssl/quic.h>
#include <wolfssl/ssl.h>
// wolfcrypt defines Sha256/Sha384 as macros for wc_Sha256/wc_Sha384,
// which breaks spaznet::quic::Hash::Sha256 enumerators after include.
#ifdef Sha256
#undef Sha256
#endif
#ifdef Sha384
#undef Sha384
#endif
#elif defined(SPAZNET_TLS_OPENSSL)
#include <openssl/bio.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#else
#error "QUIC build requires SPAZNET_TLS_OPENSSL or SPAZNET_TLS_WOLFSSL"
#endif
