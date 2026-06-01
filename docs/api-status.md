# API status

What's stable, what's experimental, what's a stub, and what's missing
entirely. Update this table when the wiring changes.

Following the protocol-handler restructure (commits a7fab2d through
05f818f, 2026-05-31), the protocol implementations live under
`example/<protocol>/` and are linked as separate libraries
(`spaznet_http`, `spaznet_http_websocket`, `spaznet_http2`,
`spaznet_udp`, `spaznet_quic_http3`).  The core `spaznet` library
holds only the low-level server / IOContext / Socket / Task /
PlatformIO.

| Subsystem | Public type / entry point | Status | Notes |
|---|---|---|---|
| Core | `spaznet::Server` | **Stable** | Public API. `set_connection_handler(ConnectionHandler)` + `set_datagram_handler(DatagramHandler)` are the dispatch hooks; the example libraries provide `make_dispatcher(...)` factories. |
| Core | `spaznet::Socket` | **Stable** | Move-only. `async_read` / `async_write` / `close`. Not safe to share across coroutines on different threads. |
| Core | `spaznet::Datagram` | **Stable** | Plain struct: bytes + peer addr + raw sockaddr_storage + listen fd. Passed to `DatagramHandler` callbacks. |
| Core | `spaznet::ConnectionHandler` / `spaznet::DatagramHandler` | **Stable** | `std::function<Task(Socket)>` / `std::function<Task(Datagram)>` typedefs. |
| Core | `spaznet::IOContext` | **Stable** | Owned by `Server`; advanced users can construct one directly for unit tests. |
| Core | `spaznet::Task` | **Stable** | C++20 coroutine return type. Symmetric transfer + ref-counted control block. |
| Core | `spaznet::codec::huffman_{encode,decode}` | **Stable** | RFC 7541 §B codec. Shared by HPACK (example/http2) and QPACK (example/quic-http3). |
| Logger | `spaznet::Logger` | **Stable** | Optional; no I/O on the hot path unless enabled. |
| **example/http** | `spaznet::http::HTTPHandler` + `make_dispatcher(...)` | **Stable** | Full HTTP/1.1 keep-alive, chunked **request** body, header CR/LF sanitization on responses. See `docs/http.md`. |
| example/http | Chunked **trailer** parsing | **Not implemented** | RFC 9112 §7.1.2 trailers stall the parser in `Incomplete`. Tracked in [TODO.md](../TODO.md). |
| example/http | Chunk-extension line length | **Limited** | 64-byte cap; real-world integrity-tag extensions can exceed this. Tracked in [TODO.md](../TODO.md). |
| **example/http-websocket** | `spaznet::websocket::Handler` + `make_dispatcher(http_handler, ws_handler)` | **Stable** | RFC 6455 compliant. Combined dispatcher: sniffs each connection for a WS upgrade and routes to the WS frame loop, otherwise hands off to example/http. Both `handle_message` overloads (`const&` and `&&`) supported. See `docs/websocket.md`. |
| example/http-websocket | `spaznet::websocket::send_message` | **Stable** | Free function (was `Socket::send_websocket_message`). One-allocation frame builder. |
| **example/http2** | `spaznet::http2::Handler` + `make_dispatcher(...)` | **Stable** | h2c (RFC 9113 §3.4, prior-knowledge cleartext). Connection preface, SETTINGS exchange, HEADERS+DATA dispatch through HPACK with Huffman decode (via `spaznet::codec`), per-stream flow control, WINDOW_UPDATE / PING / GOAWAY / RST_STREAM. Verified against `curl --http2-prior-knowledge`. See `docs/http.md`. |
| example/http2 | h2 over TLS (the `h2` ALPN) | **Not implemented** | Run a TLS terminator in front. |
| example/http2 | HPACK dynamic table | **Not implemented (intentional)** | Advertises `SETTINGS_HEADER_TABLE_SIZE=0` to peers; encode emits literal-without-Huffman. |
| example/http2 | PUSH_PROMISE / server push | **Not implemented** | Disabled via SETTINGS. |
| example/http2 | Trailers, priority frames | **Not implemented** | Priority frames are silently dropped per RFC 9113 §4.1. |
| example/http2 | Multiplexing | **Limited** | Handlers run serially per connection — a connection processes streams one at a time. A coroutine-per-stream model is a follow-up. |
| **example/udp** | `spaznet::udp::Handler` + `make_dispatcher(...)` | **Stable** | Handler-interface idiom over `set_datagram_handler`. `Packet` carries raw sockaddr + listen fd so handlers can `sendto()` directly. |
| **example/quic-http3** | `spaznet::quic::TlsContext`, `quic::Connection`, `quic::Listener` | **Experimental — server-only, partial** | Handshake + 1-RTT data work; interops with our own client. See `docs/quic-http3.md` and `docs/quic-security.md` for what's in and what's out. |
| example/quic-http3 | QUIC client mode | **Not implemented** | `Connection::Role::Server` is the only enumerator. |
| example/quic-http3 | Key update (RFC 9001 §6) | **Not implemented** | Long-lived 1-RTT connections will eventually exceed the AES-128-GCM 2²³ packet limit. |
| example/quic-http3 | Connection migration / path validation | **Not implemented** | `Listener` blindly updates `last_peer` on every received datagram. |
| example/quic-http3 | PTO retransmission / idle timeout | **Not implemented** | `Recovery` computes the math but `Connection::on_timer` never consults it. Loss recovery doesn't actually work today. |
| example/quic-http3 | CONNECTION_CLOSE emission on protocol errors | **Not implemented** | We parse incoming CONNECTION_CLOSE; we never emit one. |
| example/quic-http3 | Anti-amplification (RFC 9000 §8.1.2) | **Stable** | Enforced in `Connection::build_and_send`. Validation flips automatically on first decrypted Handshake packet, or via `mark_peer_address_validated()`. |
| example/quic-http3 | Retry token validation | **Stable** | Enabled by `Listener::Config::require_retry`. Token is peer-address-bound (HMAC-SHA256-trunc-128). See `docs/quic-security.md`. |
| example/quic-http3 | 0-RTT / session resumption | **Not implemented** | Deferred. |
| example/quic-http3 | `http3::QuicHttp3Service`, `http3::Http3Server` | **Experimental** | Static-table QPACK only (server advertises `SETTINGS_QPACK_MAX_TABLE_CAPACITY=0`). HEADERS + DATA frames work end-to-end. Server push not implemented. |
| example/quic-http3 | Dynamic QPACK table | **Not implemented** | Compression ratios suffer on repeated headers. |
| Platform I/O | `PlatformIO` epoll backend (Linux) | **Stable** | |
| Platform I/O | `PlatformIO` kqueue backend (BSD/macOS) | **Stable** | |
| Platform I/O | `PlatformIO` poll backend (other Unix) | **Stable** | Slower than epoll/kqueue; only used when nothing better is available. Currently the default on Windows (see below). |
| Platform I/O | `PlatformIO` IOCP backend (Windows) | **Disabled** | Gated behind `SPAZNET_ENABLE_BROKEN_IOCP`; the code has known UAF + WSAStartup + 0-byte-recv issues from the 2026-05-27 audit. Default Windows builds use the portable poll backend. Tracked in [TODO.md](../TODO.md). |
| Build | `find_package(spaznet)` | **Stable** | Install target ships `spaznetConfig.cmake`. See `docs/integration.md`. |
| Build | `SPAZNET_BUILD_QUIC` CMake option | **Stable** | Gates `example/quic-http3`. Defaults `ON` if OpenSSL 3.5+ is detected; warns + disables otherwise. Core builds with no OpenSSL dependency when this is `OFF`. |
| Build | `SPAZNET_BUILD_EXAMPLES` CMake option | **Stable** | Defaults `ON`. Pulls in `example/http`, `example/http-websocket`, `example/http2`, `example/udp`, and (gated on `SPAZNET_BUILD_QUIC`) `example/quic-http3`. |

## Reading the status column

- **Stable**: under test, documented, no known design-level bugs. Safe to depend on. Source-level breakage is reserved for security or correctness fixes and is called out in `CHANGELOG.md`.
- **Experimental**: works for the cases the tests cover, but the API surface or wire-level behavior may shift. Pin a SHA and re-test on bumps.
- **Limited**: the feature works for common cases but has an explicit cap (size, count, scope) that real-world peers can exceed.
- **Stub**: the type exists; the method body is a no-op.
- **Not implemented**: the feature isn't here. Calling for it will fail at compile time or at runtime depending on the surface.
- **Not implemented (intentional)**: the feature is deliberately not provided, and the absence is part of the design — e.g. HPACK dynamic table is off because we advertise `HEADER_TABLE_SIZE=0` to peers.
- **Disabled**: code exists and is in tree, but the build excludes it. Re-enabling requires a CMake flag and accepting known defects.

## What this isn't

This isn't a feature roadmap. For "what's planned next", see [TODO.md](../TODO.md). For ground-truth on whether a specific piece of code actually runs end-to-end, the test suites in `tests/` and `example/*/tests/` are authoritative — every "Stable" row above corresponds to at least one test that exercises that path.
