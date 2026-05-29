# TODO

> Status as of 9630b58 (2026-05-29): unit suite is fully green on both
> macOS (AppleClang + OpenSSL 3.6.1) and Linux (gcc 13.3 + OpenSSL
> 3.5.4) — **225/225 unit tests**. asan+ubsan and tsan builds also
> clean on both platforms (tsan on meep needs `setarch -R` to dodge
> the kernel-ASLR-vs-tsan-shadow conflict; not a code issue). The two
> long-standing macOS PlatformIO failures fixed earlier in e9d66f6
> remain fixed.

## From the 2026-05-27 audit

- [x] Replace QUIC + HPACK with a real implementation
  - Shipped across commits 111fcd1 (Phase 1+2 — RFC 9000 §16 varints,
    RFC 9001 §A.2 Initial-packet KAT, AES-128/256-GCM + ChaCha20-Poly1305
    AEAD, header protection, full frame codec under std::variant),
    3da5cbb (Phase 3 — TLS 1.3 via SSL_set_quic_tls_cbs, RFC 9000 §18
    transport-parameter codec), aa756d6 + 982be0a (Phase 4 — PN-space
    ACK bookkeeping, stream state machine with out-of-order reassembly,
    RFC 9002 RTT/PTO, NewReno congestion, Connection orchestrator),
    1adf39b (Phase 5 — Listener with CID demux, RFC 9001 §A.4 Retry
    integrity tag KAT, Version Negotiation), 5e3a4ba + 7c90120 +
    eb91300 (Phase 6 — RFC 7541 §B Huffman with §C.4 vectors,
    RFC 9204 QPACK static-only, RFC 9114 HTTP/3 frames, Http3Server
    request dispatcher), 230681b (Phase 7 — peer-addr routing,
    Server::set_quic_http3_service, end-to-end protected-datagram GET
    test).
  - Server-side QUIC v1 + HTTP/3 only. TLS handshake driven by
    OpenSSL 3.5+ via the upstream QUIC TLS callback dispatch; all
    transport / recovery / congestion / HTTP/3 / QPACK code is
    in-tree, no other third-party deps.
  - End-to-end verification: an OpenSSL client SSL* and our
    quic::Connection complete a TLS 1.3 handshake through three rounds
    of real protected datagrams, exchange transport parameters,
    install Application-level secrets, and the client's QPACK-encoded
    GET on stream 0 round-trips through Http3Server to a HEADERS+DATA
    response (`:status 200`, body "Hello, h3!").

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

- [ ] async_read return type → Task<ssize_t>
  - Deferred follow-up from #3 of the original audit. Requires the
    Task<T> template that doesn't exist yet.

## QUIC + HTTP/3 follow-ups (2026-05-29)

The seven-phase rewrite landed (see "From the 2026-05-27 audit" above),
but a number of items were explicitly deferred or left as cleanup work.
Ordered by priority.

### Cleanup we deferred but should land before anyone uses this

- [ ] Delete the toy QUIC/HTTP/3 code
  - `src/handlers/quic_handler.cpp`, `src/handlers/quic_server.cpp`,
    `src/handlers/http3_handler.cpp`, plus their declarations in
    `include/libspaznet/handlers/quic_handler.hpp`,
    `include/libspaznet/handlers/quic_server.hpp`,
    `include/libspaznet/handlers/http3_handler.hpp`. Remove the
    `set_quic_handler`/`set_http3_handler` setters and the old
    `QUICServerEngine` branch in `Server::receive_udp`. Also drop
    `tests/unit/test_quic_handler.cpp`. ~900 lines.
  - Keep `Server::set_quic_http3_service` (the new entry point).

- [ ] Wire `Listener::Config::require_retry` into the dispatch path
  - The flag exists, the Retry packet builder + RFC 9001 §A.4 KAT
    pass, but `Listener::on_datagram` never branches on
    `cfg_.require_retry`. With it off (the default), the server hands
    out connection state to anyone who can synthesize a 1200-byte
    Initial — DDoS-amplification fodder.

- [ ] Implement the 3× anti-amplification budget in `Connection`
  - RFC 9000 §8.1.2. Until the peer's address is validated (Initial
    ACKed or Retry token verified), the server MUST cap total bytes
    sent at 3× total bytes received. Today the budget isn't tracked
    at all.

### Spec MUSTs we deferred

- [ ] Key update (RFC 9001 §6)
  - AEAD usage limits (RFC 9001 §6.6) require key rotation before
    2^23 packets at AES-128-GCM. A long-lived 1-RTT connection without
    key update is a MUST violation.

- [ ] Connection migration / path validation past first datagram
  - Today `Listener::on_datagram` updates `state.last_peer = peer`
    on every received datagram and the response goes wherever the
    most recent inbound came from. RFC 9000 §9 requires PATH_CHALLENGE
    / PATH_RESPONSE on a new path before sending non-probing data.

- [ ] PTO retransmission + idle timeout
  - `Recovery` tracks the math (`pto_timeout`, `loss_time_threshold`)
    but `Connection::on_timer()` never consults it. Packets dropped on
    the wire never get re-sent today; the existing loopback test only
    passes because there's no loss. Plumb PTO into `on_timer`, and
    enforce the negotiated `max_idle_timeout`.

- [ ] CONNECTION_CLOSE emission on protocol errors
  - We parse incoming CONNECTION_CLOSE and flip to `Draining`, but on
    our side we never emit one — on a parse error we just transition
    to `Closing` silently. Build and ship a CONNECTION_CLOSE frame
    with the appropriate transport / application error code.

### Hardening

- [ ] Fuzz the parsers
  - `parse_frame`, `parse_long_header`, `parse_h3_frame`, `qpack_decode`,
    `decode_transport_params` all take attacker-controlled bytes. Even
    a 10-second libFuzzer pass per parser would find any obvious
    crashers. Wire under `tests/fuzz/` behind `-DBUILD_FUZZERS=ON`.

- [ ] UDP-socket integration test for `Server::set_quic_http3_service`
  - We have the wiring (`server_impl.cpp` calls
    `quic_http3_service_->handle_datagram`) but no test actually opens
    a UDP socket. The end-to-end loopback test (`test_http3_end_to_end`)
    goes through the `Connection` API directly. A real-socket test
    that binds, uses our hand-rolled client over `sendto`/`recvfrom`,
    and validates the GET round-trips would catch any plumbing bugs
    in the receive_udp coroutine.

- [ ] Real curl `--http3` interop
  - Neither CI host has an HTTP/3-enabled curl today. Build curl with
    `--with-quiche` or `--with-ngtcp2` on meep (or pull a pre-built
    container) and add a test that pipes through it. Almost certainly
    finds at least one issue the in-memory test misses (cipher-suite
    preference ordering, exact ACK timing, GREASE frame handling).

### Features that round out the implementation

- [ ] Client mode for `Connection`
  - The `Role role_{Role::Server}` field is a placeholder. Client mode
    needs: SCID/DCID generation in reverse, Initial-secret derivation
    with `Direction::Client`, transport-parameter encode-without-OD-CID,
    handling of Retry on the client side, and the dispatch through TLS
    `SSL_set_connect_state` instead of `SSL_set_accept_state`.

- [ ] 0-RTT / session resumption
  - TLS layer can already do it (OpenSSL exposes the early-data API).
    QUIC plumbing for the EarlyData PN space, the early-data CRYPTO
    buffer, and the 0-RTT key install path is missing.

- [ ] Dynamic QPACK table
  - Today the server advertises `SETTINGS_QPACK_MAX_TABLE_CAPACITY = 0`
    and rejects any non-zero Required-Insert-Count in incoming field
    sections. A dynamic-table encoder + decoder (RFC 9204 §3.2) lifts
    compression ratios significantly for repeated headers.

### Operational polish

- [ ] qlog tracing
  - RFC 9001 §A.1 / IETF draft-ietf-quic-qlog. Emit one JSON line per
    packet/frame to a file pointed to by the `QLOGDIR` env var. Cheap
    to add (the encoder/decoder for each frame already exists) and
    indispensable when something breaks in the field.

- [ ] Performance benchmark for the QUIC path
  - Comparable to the existing `bench_thread_modes` for TCP. Right
    now we have no idea what requests/sec this stack does. Useful
    even before the spec-MUST gaps are closed.
  - First callgrind profile (2026-05-29, see
    `docs/quic-profile-2026-05-29.md`): under the QUIC + HTTP/3 unit
    tests the workload is **handshake-bound** — ~50% of cycles in
    libcrypto crypto math (X25519, P-256, SHA-2, ML-KEM), ~9% in
    glibc malloc/free, ~5% in glibc memcpy/memset, and **only ~1%
    in libspaznet's own code**. No path to a meaningful win from
    micro-optimizing varint/frame parsing/huffman; the big realistic
    wins are session resumption / 0-RTT (skips cert + KEM) and
    reducing `std::vector` churn on the packet-build path. Need a
    steady-state benchmark next to see what AEAD-bound code looks
    like.

- [ ] API documentation
  - The five new public types (`TlsContext`, `Connection`, `Listener`,
    `QuicHttp3Service`, `Http3Server`) have header comments but no
    consolidated docs. A `docs/quic.md` walking through how the layers
    compose, with the loopback test as a worked example, would help
    anyone trying to use this.

## Other

- [x] Add support for TLS
  - Shipped as part of the QUIC rewrite (commit 3da5cbb). The
    `TlsContext` / `TlsConnection` wrappers around OpenSSL 3.5+ live
    in `include/libspaznet/quic/tls.hpp`. They're QUIC-specific
    today — using them for vanilla TLS-over-TCP would need a thin
    `SSL_read`/`SSL_write`-driven path that isn't there yet, but the
    SSL_CTX construction (cert/key loading, ALPN) is reusable.

