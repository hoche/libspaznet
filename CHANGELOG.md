# Changelog

Notable changes since the QUIC rewrite. SHAs are commit prefixes;
`git show <sha>` for full context. Newest first.

The library does not (yet) ship versioned releases — downstream
consumers should pin a SHA and re-test on bumps.

## 2026-05-31 — protocol handlers pulled out of core

Across `a7fab2d`, `aefbd64`, `e8f372f`, `d812849`, `63da693`,
`05f818f`, `2253437`.

### Added
- `Server::set_connection_handler(std::function<Task(Socket)>)` and
  `Server::set_datagram_handler(std::function<Task(Datagram)>)` —
  the low-level dispatch hooks every protocol example now plugs
  into.
- `spaznet::Datagram` struct (data + peer addr + raw sockaddr +
  listen fd).
- `spaznet::ConnectionHandler` / `spaznet::DatagramHandler`
  typedefs.
- `spaznet::codec::huffman_{encode,decode}` — shared RFC 7541 §B
  Huffman codec used by both HPACK (HTTP/2) and QPACK (HTTP/3).
- `example/http/` — HTTP/1.1, `spaznet::http::` namespace.
  `make_dispatcher(unique_ptr<HTTPHandler>) -> ConnectionHandler`.
- `example/http-websocket/` — combined HTTP/1.1 + WebSocket on the
  same port.  `spaznet::websocket::` namespace, names stripped of
  the `WebSocket` prefix (`Handler`, `Frame`, `Message`, `Opcode`).
  `make_dispatcher(http_handler, ws_handler)`.
  `spaznet::websocket::send_message()` replaces the old
  `Socket::send_websocket_message` method.
- `example/http2/` — HTTP/2 over h2c (RFC 9113 §3.4, prior-
  knowledge cleartext).  **First version that actually serves
  HTTP/2 requests** — pre-restructure `set_http2_handler` accepted
  a handler but the dispatch never ran.  Full SETTINGS exchange,
  multiplexed streams, HPACK with proper RFC 7541 varints +
  Huffman decode, per-stream and connection-level flow control.
  Verified against `curl --http2-prior-knowledge`.
- `example/udp/` — handler-interface idiom over `set_datagram_handler`.
  `spaznet::udp::Packet` carries `listen_fd` + raw `sockaddr_storage`
  so handlers `sendto()` directly.
- `example/quic-http3/` — full QUIC v1 + HTTP/3 + QPACK stack moved
  out of core into its own library (`spaznet_quic_http3`).
  Namespaces `spaznet::quic::` + `spaznet::http3::` unchanged.
  New `spaznet::http3::make_dispatcher(unique_ptr<QuicHttp3Service>)
  -> DatagramHandler` for symmetry with the other examples.
- Working demos under `example/<protocol>/demo/`:
  `http_hello`, `ws_echo`, `http2_hello`, `udp_echo`.

### Removed — **BREAKING**
- `Server::set_http_handler`, `set_websocket_handler`,
  `set_http2_handler`, `set_udp_handler`,
  `set_quic_http3_service` — all gone.  Replace with the
  per-protocol `make_dispatcher(...)` factory + the new
  low-level `set_connection_handler` / `set_datagram_handler`.
- `Socket::send_websocket_message` method —
  `spaznet::websocket::send_message(socket, ...)` free function
  is the replacement.
- All `<libspaznet/handlers/*.hpp>` headers (HTTP, WebSocket,
  HTTP/2, UDP) — moved to `<libspaznet/<protocol>/...>` under the
  example libraries.
- `<libspaznet/http3/huffman.hpp>` — moved to
  `<libspaznet/codec/huffman.hpp>` (and namespace shifted to
  `spaznet::codec::`).  The HuffTree codec stays in core because
  both HPACK and QPACK use it.

Migration: see [`docs/migration.md`](docs/migration.md).

### Changed
- HPACK rewritten to actually conform to RFC 7541.  The pre-
  restructure HPACK had broken varints, no Huffman decode, no
  dynamic-table-size-update handling, and only round-tripped its
  own (broken) output — it didn't interop with real HTTP/2
  clients.  The new implementation handles all four
  representations + prefix-N varints + Huffman decode via the
  shared `spaznet::codec` codec.
- Core builds with **no OpenSSL dependency**.  `SPAZNET_BUILD_QUIC`
  at the top level now gates `example/quic-http3` (rather than
  gating QUIC inside core).
- New top-level option `SPAZNET_BUILD_EXAMPLES` (default ON)
  controls whether the example libraries build.

### Result
- `src/handlers/` and `include/libspaznet/handlers/` directories
  deleted.  Core's `src/` and `include/libspaznet/` carry only
  `codec/`, `platform/`, `utils/`, `io_context.hpp`,
  `platform_io.hpp`, `logger.hpp`, `server.hpp`, plus
  `src/server_impl.cpp`.  The "only the low-level server should be
  left in src and include" goal is reached.
- 284 tests across core + 5 example libraries; same pass count on
  Mac and meep.

## 2026-05-30

### Added
- WebSocket frame loop now over-reads into a per-connection stash
  (`eb3ea04`). A 64-byte echo is one `recv()` syscall instead of
  three. Linux 64 B / 4-thread CPU is **−18% per echoed
  message**; large frames unchanged.

### Documentation
- Wrote 13 user-facing docs covering HTTP, WebSocket, QUIC/HTTP/3,
  threading tuning, security model, API status matrix, integration,
  migration, contributing, performance, CHANGELOG, and Doxygen.
  Replaced the orphan `lambda_cautions.txt` with a focused
  `docs/coroutine-pitfalls.md`.

## 2026-05-29

### Removed — **BREAKING**
- Deleted the pre-rewrite toy QUIC/HTTP/3 code (`5c1f39d`):
  `src/handlers/{quic_handler,quic_server,http3_handler}.cpp` and
  their public headers, the `set_quic_handler` /
  `set_http3_handler` setters on `Server`, and the
  `QUICServerEngine` dispatch branch. ~900 lines.
  Migration: `Server::set_quic_http3_service` is the new entry
  point — see [`docs/migration.md`](docs/migration.md).

### Added
- `Listener::Config::require_retry` now actually emits a Retry
  packet on the first Initial from each peer and validates the
  echoed token before allocating connection state. Tokens are
  peer-address-bound via HMAC-SHA256-trunc-128.
- RFC 9000 §8.1.2 anti-amplification budget in `quic::Connection`.
  Total outbound bytes ≤ 3 × received bytes until validation flips
  (Retry token verified, or Handshake-protected packet decrypted).
- `Connection::mark_peer_address_validated()` public setter for
  Listener-side override.

### Fixed
- `finalize_tp` now honors pre-filled `original_destination_connection_id`
  / `initial_source_connection_id` so the Retry path's transport
  parameters aren't clobbered.

## 2026-05-28

### Added
- CI installs iperf3 on every runner so `IperfIntegrationTest` cases
  don't self-skip (`2413ff2`).
- Performance tests' client sockets set `SO_LINGER {1, 0}`
  (`477a21b`) so `close()` sends RST and skips TIME_WAIT. Fixes
  spurious `EADDRNOTAVAIL` on the macOS CI runner once the ephemeral
  port range drains.
- Listen backlog bumped from `SOMAXCONN` (128 on macOS) to a literal
  4096. Linux honors the larger value; macOS clamps to its sysctl
  ceiling.

### Changed
- Windows CI installs OpenSSL via vcpkg (`4e6af09`); chocolatey's
  package was 404'ing from upstream CDN.

### Fixed
- `IOContext` bounds coroutine resume-chain stack growth via a
  thread-local pending-resume queue (`63b0874`). Previously, deeply
  nested `co_await` chains in the WS echo path could blow a 512 KiB
  stack.

## 2026-05-27

### Added
- FreeBSD x86_64 + aarch64 CI jobs via `vmactions/freebsd-vm@v1`
  (`5b1d8b7`).
- `SPAZNET_BUILD_QUIC` CMake option, default ON. If OpenSSL 3.5+
  isn't found, the build warns and disables QUIC automatically; the
  rest of the library still builds with no OpenSSL dependency
  (`126e102`).

### Fixed
- PlatformIO kqueue: UAF on `fd_records_` under concurrent close
  (`de129bf`). `IOContext::remove_io` now also calls
  `platform_io_->remove_fd` under the same spinlock so the kqueue
  registration and map deletion happen atomically.

## 2026-05-26 — QUIC + HTTP/3 rewrite landed

A from-scratch, server-side QUIC v1 + HTTP/3 + QPACK stack replaces
the previous toy. TLS 1.3 is driven by OpenSSL 3.5+ via
`SSL_set_quic_tls_cbs`; everything else (transport, recovery,
congestion, HTTP/3 framing, QPACK static table, RFC 7541 Huffman) is
in-tree.

Shipped across:

- `111fcd1` — varints, AEAD wrappers, RFC 9001 §A Initial-packet KAT.
- `3da5cbb` — TLS 1.3 integration, RFC 9000 §18 transport-parameter codec.
- `aa756d6` + `982be0a` — ACK bookkeeping, stream state machine,
  RFC 9002 RTT/PTO math, NewReno congestion, `Connection` orchestrator.
- `1adf39b` — `Listener` with CID demux, RFC 9001 §A.4 Retry integrity
  tag KAT, Version Negotiation.
- `5e3a4ba` + `7c90120` + `eb91300` — Huffman, QPACK, HTTP/3 frame
  codec, `Http3Server`.
- `230681b` — peer-addr routing, `Server::set_quic_http3_service`,
  end-to-end test.

### Performance optimizations on top of the rewrite
- `fc8a185` — in-place AEAD seal/open + reused scratch buffer.
- `549107d` — cached `EVP_CIPHER_CTX` per Space. +8–23% steady-state
  throughput.
- `bcac7b4` — killed O(N²) ACK processing. `Connection::on_ack_frame`
  now walks `sent_` via `lower_bound` instead of iterating every PN;
  `Stream::on_acked` uses a head offset with lazy compaction. Bench
  ns/pkt is flat at any N. End-to-end +231% throughput vs the
  pre-optimization baseline.

## Earlier history

See `git log` for the pre-QUIC-rewrite commit history. Major
milestones from that era:

- RFC 6455 WebSocket compliance work.
- Re-architecting `Socket::async_read` / `async_write` to re-await
  on EAGAIN instead of `sleep_for`.
- The five-mutex / one-spinlock concurrency primitive minimization
  documented in [`docs/mutex-vs-atomics.md`](docs/mutex-vs-atomics.md).
- Platform-I/O hazard fixes: EINTR + HUP/EOF handling, TaskQueue
  UAF, per-fd generation token to defeat fd reuse races.
- HTTP response header CR/LF sanitization (RFC 9112 §5.6.2).
