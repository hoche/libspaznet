# API status

What's stable, what's experimental, what's a stub, and what's missing
entirely. Update this table when the wiring changes.

| Subsystem | Public type / entry point | Status | Notes |
|---|---|---|---|
| Core | `spaznet::Server` | **Stable** | Public API. `Server(N)` thread sweep tested in CI. |
| Core | `spaznet::Socket` | **Stable** | Move-only. `async_read` / `async_write` / `send_websocket_message` / `close` are the supported operations. Not safe to share across coroutines on different threads. |
| Core | `spaznet::IOContext` | **Stable** | Owned by `Server`; advanced users can construct one directly for unit tests. |
| Core | `spaznet::Task` | **Stable** | C++20 coroutine return type. Symmetric transfer + ref-counted control block. |
| Logger | `spaznet::Logger` | **Stable** | Optional; no I/O on the hot path unless enabled. |
| UDP | `UDPHandler::handle_packet` | **Stable** | One callback per datagram with peer addr/port. |
| HTTP/1.1 | `HTTPHandler::handle_request` | **Stable** | Full keep-alive, chunked **request** body, header CR/LF sanitization on responses. See `docs/http.md`. |
| HTTP/1.1 | Chunked **trailer** parsing | **Not implemented** | RFC 9112 §7.1.2 trailers stall the parser in `Incomplete`. Tracked in [TODO.md](../TODO.md). |
| HTTP/1.1 | Chunk-extension line length | **Limited** | 64-byte cap; real-world integrity-tag extensions can exceed this. Tracked in [TODO.md](../TODO.md). |
| WebSocket | `WebSocketHandler` + `Socket::send_websocket_message` | **Stable** | RFC 6455 compliant. Both `handle_message` overloads (`const&` and `&&`) supported. See `docs/websocket.md`. |
| HTTP/2 | `HTTP2Parser`, `HPACK`, `HTTP2Frame`, etc. | **Implementation present, dispatch not wired** | Parser and codec exist (`include/libspaznet/handlers/http2_handler.hpp`) and have unit-test coverage, but `Server::handle_connection` never reads HTTP/2 frames. `set_http2_handler` accepts a handler that's never invoked. Don't rely on it from production code. |
| QUIC v1 | `quic::TlsContext`, `quic::Connection`, `quic::Listener` | **Experimental — server-only, partial** | Handshake + 1-RTT data work; interops with our own client. See `docs/quic-http3.md` and `docs/quic-security.md` for what's in and what's out. |
| QUIC v1 | Client mode | **Not implemented** | `Connection::Role::Server` is the only enumerator. |
| QUIC v1 | Key update (RFC 9001 §6) | **Not implemented** | Long-lived 1-RTT connections will eventually exceed the AES-128-GCM 2²³ packet limit. |
| QUIC v1 | Connection migration / path validation | **Not implemented** | `Listener` blindly updates `last_peer` on every received datagram. |
| QUIC v1 | PTO retransmission / idle timeout | **Not implemented** | `Recovery` computes the math but `Connection::on_timer` never consults it. Loss recovery doesn't actually work today. |
| QUIC v1 | CONNECTION_CLOSE emission on protocol errors | **Not implemented** | We parse incoming CONNECTION_CLOSE; we never emit one. |
| QUIC v1 | Anti-amplification (RFC 9000 §8.1.2) | **Stable** | Enforced in `Connection::build_and_send`. Validation flips automatically on first decrypted Handshake packet, or via `mark_peer_address_validated()`. |
| QUIC v1 | Retry token validation | **Stable** | Enabled by `Listener::Config::require_retry`. Token is peer-address-bound (HMAC-SHA256-trunc-128). See `docs/quic-security.md`. |
| QUIC v1 | 0-RTT / session resumption | **Not implemented** | Deferred. |
| HTTP/3 | `http3::QuicHttp3Service`, `http3::Http3Server` | **Experimental** | Static-table QPACK only (server advertises `SETTINGS_QPACK_MAX_TABLE_CAPACITY=0`). HEADERS + DATA frames work end-to-end. Server push not implemented. |
| HTTP/3 | Dynamic QPACK table | **Not implemented** | Compression ratios suffer on repeated headers. |
| Platform I/O | `PlatformIO` epoll backend (Linux) | **Stable** | |
| Platform I/O | `PlatformIO` kqueue backend (BSD/macOS) | **Stable** | |
| Platform I/O | `PlatformIO` poll backend (other Unix) | **Stable** | Slower than epoll/kqueue; only used when nothing better is available. Currently the default on Windows (see below). |
| Platform I/O | `PlatformIO` IOCP backend (Windows) | **Disabled** | Gated behind `SPAZNET_ENABLE_BROKEN_IOCP`; the code has known UAF + WSAStartup + 0-byte-recv issues from the 2026-05-27 audit. Default Windows builds use the portable poll backend. Tracked in [TODO.md](../TODO.md). |
| Build | `find_package(spaznet)` | **Stable** | Install target ships `spaznetConfig.cmake`. See `docs/integration.md`. |
| Build | `SPAZNET_BUILD_QUIC` CMake option | **Stable** | Defaults `ON` if OpenSSL 3.5+ is detected; warns + disables otherwise. |

## Reading the status column

- **Stable**: under test, documented, no known design-level bugs. Safe to depend on. Source-level breakage is reserved for security or correctness fixes and is called out in `CHANGELOG.md`.
- **Experimental**: works for the cases the tests cover, but the API surface or wire-level behavior may shift. Pin a SHA and re-test on bumps.
- **Implementation present, dispatch not wired**: the building blocks exist and are unit-tested, but `Server` doesn't actually route to them. Building against these types compiles fine but won't do anything useful at runtime.
- **Stub**: the type exists; the method body is a no-op.
- **Not implemented**: the feature isn't here. Calling for it will fail at compile time or at runtime depending on the surface.
- **Disabled**: code exists and is in tree, but the build excludes it. Re-enabling requires a CMake flag and accepting known defects.

## What this isn't

This isn't a feature roadmap. For "what's planned next", see [TODO.md](../TODO.md). For ground-truth on whether a specific piece of code actually runs end-to-end, the test suite in `tests/` is authoritative — every "Stable" row above corresponds to at least one test that exercises that path.
