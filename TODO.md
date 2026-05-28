# TODO

> Status as of aeb1bc5 (2026-05-27): unit + integration suites are
> fully green on both macOS (excluding two long-standing PlatformIO
> failures that don't reproduce on Linux) and Linux.

## From the 2026-05-27 audit

- [ ] Replace QUIC + HPACK with a real library
  - Current QUIC is non-interoperable plaintext (no Initial keys, no header
    protection, no AEAD, inverted long-header bit, non-RFC varint).
  - Current HPACK silently corrupts headers (no RFC 7541 varint, no Huffman,
    no dynamic table).
  - Candidates: ngtcp2 / quiche / msquic for QUIC; nghttp2 for HPACK.

- [x] Make the WebSocket layer RFC 6455 compliant
  - Shipped in e1db51e and f87cdb8 (pre-rewrite SHAs e860986, 3fa7788)
    on the merged main; integration + unit tests cover each rejection
    path. Sec-WebSocket-Accept handshake already existed and was
    confirmed correct.

- [x] Re-architect Socket::async_read / async_write
  - Shipped in dbd9ded and 607a2c9, merged via aca9198. async_read /
    async_write loop on EAGAIN via re-await instead of sleep_for;
    read_exact has no sleep_for; Server::stop drains in-flight
    handle_connection coroutines before tearing down the IOContext.
  - Still open as a follow-up: change async_read's return type to
    Task<ssize_t> so callers can distinguish bytes-read from
    EOF/error. Not done because Task<T> doesn't exist yet and the
    callsites all infer success from buffer.size().

- [x] Fix platform I/O hazards
  - Shipped in 4b3f3e6, 77a8fc5, d64bce1, 5d53adb (EINTR + HUP/EOF,
    TaskQueue UAF, user_data + generation token, IOCP gate). Docs
    refreshed in e179a02.

- [x] Sanitize CR/LF in outgoing header values
  - Shipped in 0fbe399. HTTP/1.1 and HTTP/2 response builders now
    drop entries whose name isn't a valid token or whose value
    contains CR/LF/NUL. 7 unit tests cover the rejection paths.

- [x] Build hygiene
  - Shipped on branch fix/build-hygiene, merged via 5b5a1b9.
    -Wall/-Wextra/-Wpedantic on the library (tests run a slightly
    looser set); googletest treated as SYSTEM; find_package(Threads)
    + Threads::Threads; install(EXPORT) + spaznetConfig.cmake so
    downstream find_package(spaznet) works; WSAStartup via call_once
    on Windows; verified ASan + UBSan suites are still clean.
  - -Wconversion / -Wshadow intentionally deferred — both fire across
    existing code; cleaning them up is a separate, bounded project.

## Other


- [ ] Add support for TLS
  - Currently, TLS is not supported.
  - Probably use OpenSSL as an optional config.
  - Need to be able to get cert information from the connection for auth





