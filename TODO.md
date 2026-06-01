# TODO

> Status as of 9630b58 (2026-05-29): unit suite is fully green on both
> macOS (AppleClang + OpenSSL 3.6.1) and Linux (gcc 13.3 + OpenSSL
> 3.5.4) — **225/225 unit tests**. asan+ubsan and tsan builds also
> clean on both platforms (tsan on meep needs `setarch -R` to dodge
> the kernel-ASLR-vs-tsan-shadow conflict; not a code issue). The two
> long-standing macOS PlatformIO failures fixed earlier in e9d66f6
> remain fixed.

## Active multi-session work

- [x] **Pull protocol handlers out of core into `example/<protocol>/`** — landed 2026-05-31 across commits a7fab2d..2253437. All ten phases done; src/ and include/libspaznet/ now carry only the low-level server. Same 284 tests pass on Mac and meep.
  - Goal: `src/` and `include/` contain only the low-level server
    (`Server`, `Socket`, `IOContext`, `Task`, `PlatformIO`). Protocol
    code (HTTP/1.1, WebSocket, HTTP/2, UDP, QUIC + HTTP/3) lives under
    `example/<protocol>/` with its own headers, sources, tests, and
    `CMakeLists.txt`.
  - **Phase 1 — ✅ shipped (a7fab2d)**: low-level
    `Server::set_connection_handler(std::function<Task(Socket)>)` and
    `Server::set_datagram_handler(std::function<Task(Datagram)>)`.
    `Datagram` struct added. Legacy `set_http_handler` /
    `set_websocket_handler` / etc. still work as compatibility
    wrappers while the move happens.
  - **Phase 2a — HTTP/1.1 → `example/http/`** (plain HTTP, no
    WebSocket). Types in `spaznet::http::` namespace, names
    verbatim (`HTTPHandler`, `HTTPRequest`, `HTTPResponse`,
    `HTTPParser`). Factory:
    `spaznet::http::make_dispatcher(unique_ptr<HTTPHandler>)
    -> ConnectionHandler`. Dispatcher reads requests, runs
    keep-alive loop, calls the user's handler — no WS upgrade
    detection. Demo: `example/http/demo/hello.cpp` (10-line
    "Hello, World" server).
  - **Phase 2b — HTTP/1.1 + WebSocket → `example/http-websocket/`**
    (combined stack, depends on `example/http/` for the HTTP/1.1
    parser used during upgrade). WS types live in
    `spaznet::websocket::` namespace, prefix dropped from names
    (`Handler`, `Frame`, `Message`, `Opcode`).
    `spaznet::websocket::send_message(Socket&, Opcode, span,
    bool fin=true)` is the free-function replacement for
    `Socket::send_websocket_message`. Factory:
    `spaznet::websocket::make_dispatcher(
        unique_ptr<spaznet::http::HTTPHandler>,
        unique_ptr<spaznet::websocket::Handler>)
        -> ConnectionHandler`. Dispatcher reads first request,
    detects WS upgrade, either runs the WS frame loop or hands
    to the HTTP dispatcher (composing example/http's
    machinery). Demo: `example/http-websocket/demo/echo.cpp`
    (WebSocket echo server). This phase is where the ~400-line
    WS frame loop currently inline in
    `src/server_impl.cpp::handle_connection` ends up — it moves
    to `example/http-websocket/src/dispatcher.cpp`.
  - **Phase 3 — HTTP/2 → `example/http2/`**. Status quo: dispatch
    isn't wired into `Server::handle_connection` today; move keeps
    the parser/HPACK code visible without changing behavior.
  - **Phase 4 — UDP → `example/udp/`**. Becomes a thin wrapper
    around `set_datagram_handler` for code that prefers the
    handler-interface idiom.
  - **Phase 5 — QUIC + HTTP/3 → `example/quic-http3/`**. The
    `quic::` and `http3::` namespaces stay; just relocate the
    files and update `add_subdirectory` wiring.
  - **Phase 6 — Strip dispatch from `src/server_impl.cpp`** and
    remove legacy setters. `handle_connection` collapses to "call
    `connection_handler_` if set, else close". `receive_udp`
    similarly.
  - **Phase 7 — CMake**: new top-level option
    `SPAZNET_BUILD_EXAMPLES` (default ON). Each example/ has its
    own CMakeLists adding its target + tests. SPAZNET_BUILD_QUIC
    moves to example/quic-http3 scope.
  - **Phase 8 — Docs**: update `api-status.md`, `http.md`,
    `websocket.md`, `quic-http3.md`, `integration.md`,
    `migration.md`, README. All `set_http_handler` references
    become `set_connection_handler + spaznet::http::make_dispatcher`.
  - **Phase 9 — netbench**: depend on `add_subdirectory(libspaznet/example/http)`
    and link against the example target.
  - **Phase 10 — Verify** on Mac + meep: all unit / integration /
    perf tests pass, netbench builds and runs, CI workflow doesn't
    reference the old layout.

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

- [x] parse_chunked_body should accept trailer headers — landed
  2026-05-31 in example/http/src/handler.cpp; trailers are
  consumed and dropped (not surfaced on HTTPRequest).

- [x] Bump the chunk-size line max — landed 2026-05-31;
  chunk_line_max is now 4 KiB (was 64).

- [x] Don't hold the pending_io_ spinlock across add_fd / modify_fd
  - map_lock_ is now a std::mutex instead of an atomic_flag spinlock
    (register_io / remove_io / process_io_events). The platform-IO
    syscall stays inside the critical section on purpose — it must
    remain mutually exclusive with remove_io's remove_fd to keep the
    side-table fd-reuse fix (d64bce1) intact — but waiters now park on
    the mutex instead of burning CPU spinning behind a blocked syscall.

- [x] Skip generation 0 on register_io counter wrap
  - register_io re-rolls when fetch_add returns 0 (the wakeup-pipe
    sentinel), so a real fd never inherits generation 0 and has its
    events dropped as wakeup-pipe traffic after the counter wraps.

- [x] async_read return type → Task<ssize_t>
  - Landed as ValueTask<ssize_t>: async_read co_returns the byte count
    (>0), 0 on orderly EOF, or -1 on hard error, so callers can tell
    EOF from error instead of inferring from buffer.size(). Named
    ValueTask<T> rather than Task<T> to avoid renaming the 100+ void
    Task callsites. The coroutine runtime was refactored so Task and
    ValueTask<T> share a TaskPromiseBase; the control block carries a
    base-promise pointer so the I/O layer never reinterpret-casts
    between promise types. Existing `co_await socket.async_read(...)`
    callsites are unchanged (the ssize_t is simply discarded). ASan +
    TSan suites stay clean.

## QUIC + HTTP/3 follow-ups (2026-05-29)

The seven-phase rewrite landed (see "From the 2026-05-27 audit" above),
but a number of items were explicitly deferred or left as cleanup work.
Ordered by priority.

### Cleanup we deferred but should land before anyone uses this

- [x] Delete the toy QUIC/HTTP/3 code
  - Deleted `src/handlers/quic_handler.cpp`,
    `src/handlers/quic_server.cpp`, `src/handlers/http3_handler.cpp`,
    their public headers, and the obsolete tests
    (`tests/unit/test_quic_handler.cpp`,
    `tests/integration/test_quic_server.cpp`,
    `tests/integration/test_http3_server.cpp`,
    `tests/performance/test_quic_performance.cpp`).  Removed
    `set_quic_handler` / `set_http3_handler` from `Server` and the
    `QUICServerEngine` branch in `Server::receive_udp`.
    `Server::set_quic_http3_service` is the only QUIC/HTTP/3 entry
    point now.

- [x] Wire `Listener::Config::require_retry` into the dispatch path
  - When `require_retry` is on, the listener now full-parses each
    Initial; without a token it emits a Retry (containing a fresh
    SCID and a peer-address-bound token) and creates no state.
    Initials carrying a valid token are accepted, the OD-CID is
    decoded out of the token, the connection is constructed with
    `original_destination_connection_id` and
    `retry_source_connection_id` pre-filled, and
    `Connection::mark_peer_address_validated()` skips the anti-amp
    cap.  Tokens are bound to the peer's IP+port via HMAC-SHA256
    truncated to 128 bits over `nonce||addr_len||addr||odcid_len||
    odcid`, so a replay from a different source fails validation.

- [x] Implement the 3× anti-amplification budget in `Connection`
  - RFC 9000 §8.1.2.  `Connection` now tracks `recv_bytes_total_`
    (bumped on every received datagram regardless of decrypt
    success) and `sent_bytes_unvalidated_`.  `build_and_send` bails
    early when `sent + 1500 > 3 × recv` while still unvalidated.
    The cap is released either by `mark_peer_address_validated()`
    (called by Listener after Retry token verification) or
    automatically when a Handshake-protected packet decrypts (proof
    the peer received our Initial response).

### Spec MUSTs we deferred

- [x] Key update (RFC 9001 §6) — landed 2026-05-31.
  Both directions of 1-RTT key updates are wired:
  `Connection::install_new_keys` pre-derives "phase + 1" packet
  keys from the Application traffic secrets via `quic ku`;
  `process_short_packet` detects the toggled `KEY_PHASE` bit and
  ratchets recv (and, per §6.1, send) on a successful decrypt;
  `Connection::initiate_key_update()` exposes the server-initiated
  path. Header-protection keys deliberately don't rotate. Tests:
  `QuicConnection.AcceptsPeerInitiatedKeyUpdate`,
  `QuicConnection.ServerInitiatedKeyUpdate`. Open follow-up:
  automatic update once a per-direction packet count approaches the
  AEAD usage ceiling (RFC 9001 §6.6); applications must currently
  call `initiate_key_update()` themselves.

- [x] Connection migration / path validation, partial — landed 2026-05-31.
  `Listener::on_datagram` now freezes `state.last_peer` once
  `conn->handshake_complete()` returns true, so a forged short-header
  datagram from a spoofed source can't redirect our outbound traffic.
  Inbound `PATH_CHALLENGE` is echoed as `PATH_RESPONSE` on the
  validated path. We do not initiate path validation toward
  alternate paths ourselves; legitimate NAT rebinding mid-connection
  is unsupported and the peer must re-handshake. Tests:
  `QuicConnection.RespondsToPathChallenge`,
  `QuicListener.FreezesPathPostHandshake`.

- [x] PTO retransmission — landed 2026-05-31.
  `Connection::on_timer` now calls `check_pto()`, ACK
  processing samples RTT and runs the packet-threshold +
  time-threshold loss-detection rules from RFC 9002 §6.1, and
  losses re-queue via `Stream::on_lost` / `crypto_pending_`.
  Test: `QuicConnection.PtoRetransmitsDroppedStream`.

- [x] Idle timeout enforcement — landed 2026-05-31.
  `Connection::on_timer` calls `check_idle_timeout()` which flips
  state to Draining when `now - last_activity_` exceeds the lesser
  of our and the peer's `max_idle_timeout_ms`.  Test:
  `QuicConnection.IdleTimeoutFlipsToDraining`.

- [x] CONNECTION_CLOSE emission on protocol errors — landed
  2026-05-31.  Frame-parse failure now triggers a transport
  CONNECTION_CLOSE with FRAME_ENCODING_ERROR (0x07).  Application
  code can drive a graceful close via
  `Connection::close_with_error(code, application, reason)`.
  Test: `QuicConnection.InitiateCloseEmitsConnectionCloseFrame`.

### Hardening

- [x] Fuzz the parsers — landed 2026-05-31.  11 libFuzzer harnesses
  cover every untrusted-byte parser (HTTP/1.1 request + chunked-body;
  HPACK + HTTP/2 frame; QUIC frame + long-header + transport-params +
  varint; QPACK + HTTP/3 frame; shared RFC 7541 Huffman).  Gated by
  `-DSPAZNET_BUILD_FUZZ=ON` (requires Clang); applies
  `-fsanitize=fuzzer,address,undefined` to every target globally so
  fuzzer binaries link cleanly.  5-second pass per harness on meep
  (clang 18, Linux x86_64) found zero crashers and accumulates
  coverage continuously — good baseline for longer-running CI runs.
  Run with e.g. `./build-fuzz/example/quic-http3/fuzz_quic_frame
  -max_total_time=10 -print_final_stats=1`.
- [ ] UDP-socket integration test for `Server::set_quic_http3_service`
  - We have the wiring (`server_impl.cpp` calls
    `quic_http3_service_->handle_datagram`) but no test actually opens
    a UDP socket. The end-to-end loopback test (`test_http3_end_to_end`)
    goes through the `Connection` API directly. A real-socket test
    that binds, uses our hand-rolled client over `sendto`/`recvfrom`,
    and validates the GET round-trips would catch any plumbing bugs
    in the receive_udp coroutine.

- [x] Real curl `--http3` interop test scaffolding — landed 2026-05-31.
  `QuicHttp3CurlInterop` (example/quic-http3/tests/integration/test_curl_http3_interop.cpp)
  spins the server up on a real loopback UDP port, picks a free
  port, generates a self-signed P-256 cert, and runs
  `curl --http3-only -k -sS` against it. Self-skips when the host
  curl lacks HTTP/3 (macOS system curl, stock Ubuntu 24.04 curl).
  Service grows a self-routing constructor so callers don't have
  to know the UDP fd before `listen_udp` picks one.  Honors
  `SSLKEYLOGFILE` for Wireshark debugging.

  Still open follow-up: actually running it against a real
  HTTP/3-capable curl in CI. Both meep and the macOS dev box ship
  curl without HTTP/3 today.  Easiest path: install
  `libnghttp3-dev` + `libngtcp2-dev` (both on Ubuntu 24.04
  universe) and build curl from source with `--with-nghttp3`
  `--with-ngtcp2`.  Once that's wired the test will catch the
  cipher-suite ordering / ACK timing / GREASE drift the in-memory
  client glosses over.

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

- [x] Performance benchmark for the QUIC path
  - Shipped as `tests/performance/bench_quic_steady_state.cpp`
    (commit 27320da). Baseline numbers: ~117 K full-MTU
    packets/sec, ~675 Mbps, ~8.5 µs/packet end-to-end on a single
    thread (meep, Intel x86_64). See
    `docs/quic-profile-2026-05-29.md`.
  - The first profile under that bench is also captured in the same
    doc. The in-place-AEAD optimization that the profile recommended
    (commit fc8a185) turned out to be within benchmark noise here
    (callgrind saw −7% on the changed sub-tree, but the bench
    exercises both server send + client receive plus the per-call
    EVP context churn, which dwarfs vector allocations).

- [x] QUIC perf: cache EVP_CIPHER_CTX per Space
  - Shipped as `quic::CipherCtx` (commit pending). One Encrypt + one
    Decrypt context per encryption level, allocated when keys are
    installed via `install_new_keys` and reused for every packet —
    only the IV gets reset per call via
    `EVP_EncryptInit_ex(ctx, NULL, NULL, NULL, nonce)`. Eliminates
    per-packet EVP_CIPHER_CTX_new + Init(cipher,key) + free. Bench
    confirms +8–23% throughput on the steady-state QUIC bench
    (smaller-N runs see the bigger win because at higher N
    `Stream::on_acked`'s O(N) erase eats the wallclock).

- [x] QUIC perf: fix O(N) erase in Stream::on_acked + O(N²) ACK loop
  - Shipped in bcac7b4. Two fixes, both required:
    1. `Connection::on_ack_frame` now walks `sent_` via `lower_bound`
       instead of iterating every PN in each acked range. The bench
       client doesn't trim its PnSpace.ranges_, so each ACK frame
       covers PNs 0..largest — most of which are already acked. The
       old code did O(largest_pn) by_pn.find calls per ACK; the new
       one visits only the actually-still-in-flight entries.
    2. `Stream::on_acked` now advances a logical head offset
       (`send_buf_head_`) on ack rather than `vector::erase(begin,
       ...)`, compacting only when the head exceeds 8 KiB or half
       the buffer.
  - Result: bench ns/pkt is flat across N (was growing ~linearly
    with N, i.e. total time was O(N²)). Headline: ~388 K pps /
    2.23 Gbps / 2,575 ns/pkt on meep, holds steady up to 500 K
    packets. End-to-end +231% throughput vs the pre-optimization
    baseline (461645b).

- [ ] QUIC perf: PnSpace should trim received-PN ranges over time
  - The companion problem to the fix above. RFC 9000 §13.2
    "Limiting Ranges by Tracking ACK Frames" describes the actual
    fix: once the peer has acknowledged a frame that itself
    acknowledged some PNs, the receiver no longer needs to keep
    those PNs in its received-PN set. We currently never call
    PnSpace::trim_below, so the ranges_ vector includes every PN
    ever seen — bloating every emitted ACK frame.
  - Today the workaround is on the server side (on_ack_frame
    handles the bloated ACK efficiently). But the bloated ACKs
    themselves are wire-inefficient (more varint bytes than
    needed) and the test harness/client would benefit from
    proper trimming too.

- [ ] QUIC perf: hand back a span from Stream::pull_send
  - Today pull_send does data_out.assign(...), copying bytes from
    the stream's send buffer into a vector that lives in a
    StreamFrame's `data` field until the packet is acked. That's
    one allocation + one memcpy per packet emitted. The stream's
    send buffer is the source of truth and doesn't shrink until
    on_acked, so handing back a span<const uint8_t> into it would
    let build_and_send reference the bytes without copying.

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

