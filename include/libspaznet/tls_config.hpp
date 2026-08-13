#pragma once

// TLS-over-TCP configuration for Server::listen_tls (HTTP/1.1 and HTTP/2).
// Only available when the library is built with SPAZNET_ENABLE_TLS
// (defines SPAZNET_HAS_TLS). Independent of QUIC's OpenSSL 3.5 / wolfSSL
// requirement — this path uses plain SSL_read/SSL_write on accepted TCP fds.

#include <string>
#include <vector>

namespace spaznet {

struct TlsConfig {
    // PEM-encoded certificate (and optional chain after the leaf) and
    // matching private key. Prefer in-memory PEM strings for tests/demos.
    std::string cert_pem;
    std::string key_pem;

    // ALPN protocol list advertised by this listener. Per-protocol
    // listeners: HTTP/1.1 uses {"http/1.1"}; HTTP/2 uses {"h2"}. There is
    // no cross-protocol ALPN mux on a single port.
    std::vector<std::string> alpn;
};

} // namespace spaznet
