# TODO

> Status as of c5a830d (2026-05-27): unit + integration suites are
> fully green on both macOS and Linux — **143/143 unit, 70/70 integration**.
> The two long-standing macOS PlatformIO failures are fixed (kqueue
> remove_fd bug, e9d66f6).

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

## From the post-cleanup re-audit (2026-05-27)

Five fixes shipped on branch fix/audit-round-2 (merge c5a830d):
duplicate-CL-with-identical-values, default-Windows-to-poll, kqueue
remove_fd ENOENT, ConnectionGuard / Socket::close TOCTOU, and the
integration tests that asserted on a dead handler instance. Open
items from the same re-audit, none critical:

- [ ] Rewrite the IOCP backend
  - Currently gated off (#error unless SPAZNET_ENABLE_BROKEN_IOCP),
    so default Windows builds fall through to the portable poll
    backend. The IOCP code itself still has the audit-flagged defects:
    no WSAStartup of its own (the WSAStartup added in 02f77d0 lives
    in io_context.cpp and is independent), zero-byte WSARecv probes
    that confuse 0-byte completions with EOF, no CancelIoEx on
    remove_fd (heap-allocated OverlappedContext blocks leak per
    cancelled op), ERROR_OPERATION_ABORTED treated as fatal,
    fd-based dispatch in process_io_events that predates the
    per-registration token (d64bce1) so the fd-reuse race is wide
    open here.
  - When picking this back up: build on top of the per-registration
    token (encode the token into the OVERLAPPED's hEvent/Internal
    slot or into a side-table keyed by overlapped*), call WSAStartup
    via the io_context.cpp helper, use AcceptEx + WSARecv with a
    real buffer instead of zero-byte probes, and cancel via
    CancelIoEx with proper bookkeeping. Default Windows builds
    should be opt-in to IOCP via -DSPAZNET_ENABLE_BROKEN_IOCP=ON
    (rename the flag once the backend is actually finished).

- [ ] parse_chunked_body should accept trailer headers
  - RFC 9112 §7.1.2 allows zero-or-more trailer-field lines between
    the last-chunk (`0\r\n`) and the final `\r\n`. The current parser
    requires the final CRLF immediately, so a compliant peer that
    emits any trailer field stalls in Incomplete forever (until read
    timeout closes the connection).

- [ ] Bump the chunk-size line max (currently 64 bytes)
  - http_handler.cpp's parse_chunked_body uses chunk_line_max = 64.
    RFC 9112 places no limit on chunk-extensions, and real-world
    extensions (e.g. trailer-style integrity tags) routinely exceed
    that. Bump to ~4 KiB to remain DoS-safe while accommodating
    real usage.

- [ ] Don't hold the pending_io_ spinlock across add_fd / modify_fd
  - register_io and process_io_events hold map_lock_ (an atomic_flag
    spinlock) across the platform-IO syscall. A blocked syscall under
    a spinlock can starve every other thread that needs the map.
    Either switch to std::mutex (other call sites already do) or
    drop the syscall outside the critical section.

- [ ] Skip generation 0 on register_io counter wrap
  - PendingIO::generation is uint32_t incremented via fetch_add. 0 is
    reserved as the wakeup-pipe sentinel; after 2^32 - 1 registrations
    the counter wraps back to 0 and collides. Realistic concern only
    on decades-long uptime under extreme churn, but the fix is one
    line: re-roll on wrap.

- [ ] Replace QUIC + HPACK with a real library (still the big one)
  - See above section for the full description.

- [ ] async_read return type → Task<ssize_t>
  - Deferred follow-up from #3 of the original audit. Requires the
    Task<T> template that doesn't exist yet.

## Other

- [ ] Add support for TLS
  - Currently, TLS is not supported.
  - Probably use OpenSSL as an optional config.
  - Need to be able to get cert information from the connection for auth





