# Changelog

Notable changes since the QUIC rewrite. SHAs are commit prefixes;
`git show <sha>` for full context. Newest first.

The library does not (yet) ship versioned releases — downstream
consumers should pin a SHA and re-test on bumps.

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
