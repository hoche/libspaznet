# Changelog

Notable changes since the QUIC rewrite. SHAs are commit prefixes;
`git show <sha>` for full context. Newest first.

The library does not (yet) ship versioned releases — downstream
consumers should pin a SHA and re-test on bumps.

## 2026-08-12 — WebSocket reactor dispatcher

Eighth milestone of the reactor port (see the "Milestone websocket-reactor
progress note" in the reactor-port plan). Unlike HTTP/1.1, UDP, and
QUIC/HTTP3, WebSocket's coroutine `Handler` couldn't be reused as-is —
`conn.send()` is a real `co_await`'d socket write, not an already-synchronous
call wearing a `Task` wrapper — so this milestone adds a parallel
synchronous handler interface rather than changing the existing one.

### Added
- `spaznet::websocket::reactor::Handler` / `Connection`
  (`example/http-websocket/include/libspaznet/websocket/reactor_handler.hpp`)
  — the coroutine-free counterpart of `websocket::Handler`/`Connection`.
  `Connection::send()` writes a `Frame` (handler.cpp's `Frame::serialize()`,
  unchanged) straight into the target's `BufferedConnection::OutputBuffer` —
  no suspension possible or needed, so there's no `WriteGate` equivalent on
  this side. Unlike the coroutine `Connection`, this one is copyable (a
  `weak_ptr` + fd + `IOContext*`), so a handler can stash one for later
  (e.g. a broadcast) without risking a dangling pointer.
- `spaznet::websocket::make_reactor_dispatcher(std::unique_ptr<HTTPHandler>,
  std::unique_ptr<reactor::Handler>) -> ConnectionFactory`
  (`example/http-websocket/src/dispatcher_reactor.cpp`) — same
  upgrade-sniffing rules and on-the-wire framing as `make_dispatcher`, built
  on a `WsConnection` state machine (`Sniffing` / `ReadingHeader` /
  `ReadingPayload`) instead of a suspended coroutine frame. Falls through to
  `http::attach_reactor_dispatcher` (new, see below) for the non-WS-upgrade
  case instead of duplicating the HTTP/1.1 keep-alive loop.
- `spaznet::http::attach_reactor_dispatcher(ctx, conn, handler,
  initial_buffer, on_closed)`
  (`example/http/include/libspaznet/http/dispatcher.hpp`) — the reactor-side
  counterpart of the already-public `http::serve_keep_alive`: attaches the
  HTTP/1.1 reactor loop to an already-constructed `BufferedConnection`,
  seeded with bytes a caller already peeked at. `make_reactor_dispatcher`
  now calls this internally; behavior is unchanged.
- `example/http-websocket/src/handshake.hpp`/`.cpp` — RFC 6455 §4.2
  handshake parsing and `Sec-WebSocket-Accept` computation, extracted
  verbatim out of `dispatcher.cpp`'s anonymous namespace so both dispatchers
  share one implementation instead of risking drift.
- `ws_echo --reactor` / `ws_chat --reactor` — opt-in CLI flag on both demos.
  `ws_chat --reactor`'s `ChatRoomReactor` is notably simpler than the
  coroutine `ChatRoom`: no outbox queue, no `writer_loop`, no interval poll
  — broadcasting is a direct `send()` per target connection, posted via
  `IOContext::post()` since another connection's `BufferedConnection` isn't
  safe to touch from an arbitrary thread without going through the event
  loop (see the plan note for the exact reasoning and its relationship to
  the pending `threading` milestone).
- `example/http-websocket/tests/integration/dispatcher_test_support.hpp` —
  carries the shared `DispatcherKind`/name-generator (no `install_dispatcher`
  helper here, since the two runtimes use different `Handler` classes,
  unlike the other three protocols). `test_websocket_server.cpp` is now
  `TEST_P`-parameterized over both dispatchers (22 tests total, including
  the full RFC 6455 malformed-frame compliance suite).

Verified with the full suite (13 ctest targets) under
`-DSPAZNET_ENABLE_COROUTINES=ON`, again with `-DSPAZNET_BUILD_QUIC=ON` (15
targets, against a local OpenSSL 3.5 build), and confirmed
`-DSPAZNET_ENABLE_COROUTINES=OFF` still configures/builds/passes
`UnitTests` unchanged. Manually smoke-tested both demos in both modes,
including a two-client `ws_chat --reactor` session confirming join/message/
leave notifications actually cross connections via `post()`.

## 2026-08-12 — UDP + QUIC/HTTP3 reactor dispatchers

Seventh milestone of the reactor port (see the "Milestone udp-quic-reactor
progress note" in the reactor-port plan). Both protocols reuse the exact
transport/codec code the coroutine dispatchers already had — no `quic/` or
`http3/` source changed — because both were already synchronous under the
hood; this milestone is purely about exposing that as a coroutine-free
entry point.

### Changed
- `spaznet::udp::Handler::handle_packet` is now `void handle_packet(const
  Packet&)` instead of `Task handle_packet(const Packet&)`
  (`example/udp/include/libspaznet/udp/handler.hpp`). Every existing
  implementation (`echo`, `relay`, `statsd`, the test harness) already ran
  to completion synchronously with no `co_await`, so this is a signature
  change with no behavioral change. The coroutine dispatcher
  (`example/udp/src/dispatcher.cpp`) now calls the synchronous method and
  `co_return`s immediately afterward. See `docs/migration.md`.

### Added
- `spaznet::udp::make_reactor_dispatcher(std::unique_ptr<Handler>) ->
  SyncDatagramHandler` (`example/udp/src/dispatcher_reactor.cpp`) — calls
  `handle_packet` directly, no `Task` involved. Install via
  `Server::set_sync_datagram_handler()`.
- `spaznet::http3::make_reactor_dispatcher(std::unique_ptr<QuicHttp3Service>)
  -> SyncDatagramHandler` (`example/quic-http3/src/http3/service.cpp`) —
  the coroutine-free counterpart of `make_dispatcher`. `make_dispatcher`'s
  `Task` never actually suspends (the whole QUIC/HTTP3 transport underneath
  is already a synchronous pump), so this is a mechanical swap: both now
  share a `dispatch_one()` helper, one wrapped in a no-op-suspend `Task`,
  the other called directly.
- `udp_echo --reactor` / `udp_relay --reactor` / `udp_statsd --reactor` —
  opt-in CLI flag on all three UDP demos to run the reactor dispatcher
  instead of the default coroutine one.
- `example/udp/tests/integration/dispatcher_test_support.hpp` and
  `example/quic-http3/tests/integration/dispatcher_test_support.hpp` — the
  same `DispatcherKind {Coroutine, Reactor}` / `install_dispatcher()`
  pattern `example/http` established, applied to these two protocols.
  `test_udp_server.cpp` and `test_curl_http3_interop.cpp` are now
  `TEST_P`-parameterized over both dispatchers.

Verified with the full suite (15 ctest targets) under
`-DSPAZNET_ENABLE_COROUTINES=ON` plus `-DSPAZNET_BUILD_QUIC=ON` (against a
local OpenSSL 3.5 build), and `bench_quic_steady_state` as a regression
sanity check (~87k pkts/sec, unchanged from prior runs — it drives the
QUIC/HTTP3 codec directly and doesn't exercise either dispatcher). The
curl-HTTP/3 interop test self-skips on hosts whose `curl` lacks HTTP/3
support (true in this dev environment) but now does so for both
parameterizations rather than only covering the coroutine path.

## 2026-08-12 — HTTP/1.1 reactor dispatcher (coroutine-free Http1Connection)

Sixth milestone of the reactor port (see the "Milestone http1-reactor
progress note" in the reactor-port plan). `example/http` gains a second,
coroutine-free dispatcher that plugs into `Server::set_connection_factory`
from Milestone 5, speaking the exact same HTTP/1.1 protocol against the
exact same `HTTPHandler`/`HTTPParser`/`HTTPRequest`/`HTTPResponse` as the
existing coroutine dispatcher — unchanged, not forked. Non-breaking:
`make_dispatcher`/`serve_keep_alive` and every existing caller are
untouched.

### Added
- `spaznet::http::make_reactor_dispatcher(std::unique_ptr<HTTPHandler>) -> ConnectionFactory`
  (`example/http/src/dispatcher_reactor.cpp`) — the reactor counterpart of
  `make_dispatcher`. Internally, `Http1Connection` is a small phase state
  machine (`ReadingRequest` / `Dispatching`) built on `BufferedConnection`;
  handles keep-alive, pipelining (answered in place, without recursion —
  see the plan note), chunked responses, and the 400-Bad-Request/parse-error
  path identically to the coroutine dispatcher.
- `BufferedConnection::close_after_flush()` — closes immediately if
  nothing is queued in `output()`, otherwise waits for `on_writable()` to
  finish draining it first. Needed anywhere a dispatcher wants to send a
  final response and then close: `write()` immediately followed by
  `close()` would race the write against the fd closing.
- `http_hello --reactor` / `http_showcase --reactor` — opt-in CLI flag on
  both HTTP/1.1 demos to run the new dispatcher instead of the default
  coroutine one, same handler either way.
- `example/http/tests/integration/dispatcher_test_support.hpp` — a
  `DispatcherKind {Coroutine, Reactor}` enum plus an `install_dispatcher()`
  helper, establishing the differential-testing harness later protocol
  milestones will reuse.

### Changed
- `test_tcp_server.cpp`, `test_http_server.cpp`,
  `test_rfc9112_compliance.cpp`, `test_concurrent_connections.cpp`,
  `test_thread_modes.cpp`, and `test_deferred_handler.cpp` are now
  parameterized (`TEST_P`/`INSTANTIATE_TEST_SUITE_P`) over
  `DispatcherKind`, so every scenario they already covered — RFC 9112
  protocol details, chunked encoding, keep-alive, concurrent/burst
  connections, both threading modes, `stop()`-drain timing, and the
  async-completion (`ResponseWriter` deferred) path — now runs against
  both dispatchers. Added `RFC9112IntegrationTest.PipelinedRequestsAnsweredInOrderOnOneConnection`
  and `RFC9112IntegrationTest.MalformedRequestLineGetsFullBadRequestResponseThenCloses`
  specifically to exercise the reactor dispatcher's pipelining loop and
  `close_after_flush()` path (both pass identically on the coroutine side
  too). Performance tests are unchanged (coroutine-only) for this
  milestone.

Verified under both `-DSPAZNET_ENABLE_COROUTINES=ON` (full suite, 13
ctest targets, `HttpIntegrationTests` now covering 66 tests) and `=OFF`
(core `UnitTests`, including the new `close_after_flush()` coverage).

## 2026-08-12 — reactor entry points on Server (ConnectionFactory, sync UDP, destroy-based shutdown)

Fifth milestone of the reactor port (see the "Milestone 5 progress note"
in the reactor-port plan for full design rationale). `Server` gains a
coroutine-free way to accept TCP connections and receive UDP datagrams,
alongside — not instead of — its existing `Task`-based API. Non-breaking:
nothing changes for existing `set_connection_handler`/`set_datagram_handler`
users.

### Added
- `Server::set_connection_factory(ConnectionFactory)` —
  `ConnectionFactory = std::function<std::shared_ptr<IoHandler>(int fd, IOContext&, std::function<void()> on_closed)>`.
  Takes precedence over `set_connection_handler` for newly accepted
  connections. The factory mints whatever `IoHandler` drives the
  connection (typically a `BufferedConnection`) and must arrange for
  `on_closed` to fire exactly once when it's done; returning `nullptr`
  declines the connection (`Server` closes the fd itself in that case).
- `Server::set_sync_datagram_handler(SyncDatagramHandler)` —
  `SyncDatagramHandler = std::function<void(Datagram)>`, called as a
  plain function with no coroutine involved. Takes precedence over
  `set_datagram_handler` for datagrams received afterward.
- `IoHandler::shutdown()` — new virtual hook (default no-op) letting
  callers holding a `shared_ptr<IoHandler>` tear it down generically.
  `BufferedConnection::shutdown()` overrides it to call `close()`.
- `Statistics::active_connections` and `Statistics::bytes_buffered`
  are now populated: `active_connections` by both runtimes (the
  coroutine path's `ConnGuard` and the reactor path's
  factory/`finish_reactor_connection` hooks feed the same counter);
  `bytes_buffered` by `BufferedConnection` (always 0 for coroutine
  connections, which have no equivalent buffer).
- `tests/integration/test_server_reactor.cpp` — core-level,
  protocol-agnostic coverage of the above: an echoing
  `ConnectionFactory` end to end over a real socket,
  `active_connections` tracking, `stop()` tearing down a live reactor
  connection within its bounded deadline, a `SyncDatagramHandler`
  round trip, and a declined-connection fd-leak check.
  `BufferedConnectionTest.BytesBufferedStat*` unit tests cover the new
  gauge.

### Changed
- `Server::stop()` gained a step, between closing listening sockets
  and draining in-flight coroutines, that force-closes every
  registered reactor connection: posted to an IO thread (via
  `IOContext::post()`, avoiding a race between `stop()`'s caller
  thread and live `on_readable()`/`on_writable()` calls) and bounded
  to the same 1s deadline the coroutine drain already uses. No
  `shutdown(2)` involved — these connections have no suspended call
  stack to unwind, so their `IoHandler::shutdown()` is called directly.

## 2026-08-12 — synchronous handler API + ResponseWriter (HTTP/1.1)

Fourth milestone of the reactor port (see the "Milestone 4 progress
note" in the reactor-port plan for the full design rationale).
**Breaking change for HTTP/1.1 handlers** — `example/http`'s
`HTTPHandler::handle_request` is no longer a coroutine. HTTP/2,
WebSocket, and UDP are untouched and still `Task`-based; each gets
this same treatment alongside its own reactor-dispatcher milestone.

### Added
- `spaznet::ResponseWriter<Response>`
  (`include/libspaznet/reactor/response_writer.hpp`) — a movable/
  copyable completion token. A handler answers by calling
  `writer.complete(response)`, either inline (indistinguishable from
  a plain synchronous function call) or later, from anywhere — a
  stashed copy, a different thread, a timer. Only the first
  `complete()` call across all copies takes effect. Core, header-only,
  zero coroutine dependency; unit-tested standalone
  (`test_response_writer.cpp`) and confirmed to build/link under
  `-DSPAZNET_ENABLE_COROUTINES=OFF`.

### Changed
- `spaznet::http::HTTPHandler::handle_request` is now
  `void handle_request(const HTTPRequest&, ResponseWriter)` — no
  `Task`, no `co_await`, no `Socket&`. All demos (`http_hello`,
  `http_showcase`, `ws_echo`'s/`ws_chat`'s HTTP fallback) and tests
  updated. See `docs/http.md` for the new contract.
- `example/http/src/dispatcher.cpp`'s `serve_keep_alive` (still a
  coroutine itself — its own reactor dispatcher is a later milestone)
  now calls `handle_request()` synchronously and only suspends
  (`co_await`s a small internal `AwaitResponseReady` bridge) if the
  handler actually deferred completion. Added
  `test_deferred_handler.cpp` — a handler that completes from a
  detached background thread after a delay — since no pre-existing
  handler ever exercised that suspend/resume path.

## 2026-08-12 — reactor core extracted from IOContext; coroutines now optional

First three milestones of the reactor port (see
`docs/concurrency-and-coroutines.md` for the execution model). No
public coroutine API changed; `Server`/`Socket`/every protocol
dispatcher are unaffected and still coroutine-only pending their own
reactor-side dispatchers.

### Added
- `spaznet::IoHandler` — readiness callback interface (`on_readable`/
  `on_writable`/`on_error`). `IOContext`'s fd table now stores
  `shared_ptr<IoHandler>` instead of a raw `CoroutineHandle`;
  `CoroutineResumeHandler` is the coroutine runtime's implementation
  of it, and `IOContext::set_io_handler()` is the generic primitive
  `register_io(fd, events, CoroutineHandle)` now wraps.
- `IOContext::post(std::function<void()>)` — always-queueing callback
  primitive (never resumes/invokes inline, unlike `schedule()`'s
  non-threaded fast path), backed by a new `CallbackQueue` sibling to
  `TaskQueue`.
- `IOContext::add_timer_callback(...)` — callback-flavored timer,
  dispatched via `post()`, alongside the existing `Task`-flavored
  `add_timer()` used by `sleep_for`/`sleep_until`/`interval`.
- `IOContext::defer_destruction(shared_ptr<void>)` / `drain_reap_list()`
  — a reap list for the standard reactor re-entrancy hazard: a
  connection dropping its own last owning reference from inside a
  callback `IOContext` is currently invoking on it. Objects handed to
  `defer_destruction` are kept alive until the current run()/worker
  loop iteration finishes, instead of being destroyed synchronously
  mid-callback.
- `spaznet::InputBuffer` / `OutputBuffer` / `BufferedConnection`
  (`include/libspaznet/reactor/buffered_connection.hpp`) — the
  reactor-side buffered I/O layer: a growable/compacting read buffer,
  a write buffer with optimistic-write-then-queue semantics and
  automatic write-interest toggling, and an `IoHandler` that ties them
  to a raw non-blocking fd via `set_io_handler`/`defer_destruction`.
  Operates on a raw fd directly rather than through `Socket` (which
  remains coroutine-only pending its own reactor entry point), so it
  has no coroutine dependency at all.
- `SPAZNET_ENABLE_COROUTINES` CMake option (default `ON`). When
  `OFF`, `SPAZNET_HAS_COROUTINES` is not defined, `Task`/`ValueTask`/
  `CoroutineHandle`/`schedule()`/`register_io(CoroutineHandle)` and
  all coroutine-only `IOContext` state are compiled out entirely, and
  the core library (fd table, timers, `post()`, `PlatformIO`,
  `BufferedConnection`) builds and links with zero coroutine code.
  `Server` and `SPAZNET_BUILD_EXAMPLES` still require coroutines today
  (forced `OFF` with a warning otherwise) — their reactor counterparts
  are later milestones.

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
