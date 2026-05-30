# Migration notes

What to do when a libspaznet bump breaks your build. One section per
incompatible change, newest first. Pair with [`CHANGELOG.md`](../CHANGELOG.md)
for the full list of what changed.

The library has not yet shipped versioned releases — pin a SHA, read
this page, and re-run your test suite when you bump.

## 2026-05-29 — toy QUIC/HTTP/3 types removed

Commit `5c1f39d`.

### What broke

The pre-rewrite "toy" QUIC and HTTP/3 types were deleted:

| Removed (file / type) | Replacement |
|---|---|
| `<libspaznet/handlers/quic_handler.hpp>` | `<libspaznet/handlers/http3_handler.hpp>` (the H3 abstraction handles QUIC underneath) |
| `<libspaznet/handlers/quic_server.hpp>` | `<libspaznet/http3/service.hpp>` (`http3::QuicHttp3Service`) |
| `<libspaznet/handlers/http3_handler.hpp>` | Stays at the same path, but the implementation file is now `src/http3/server.cpp`. Public interface (`HTTP3Handler`) is unchanged. |
| `class QUICHandler` | (no direct replacement — you don't write raw QUIC handlers anymore; you write `HTTP3Handler` and let `QuicHttp3Service` route through QUIC) |
| `class QUICServerEngine` | `http3::QuicHttp3Service` |
| `class QUICStream`, `class QUICConnection` | `quic::Stream`, `quic::Connection` under `<libspaznet/quic/*>` (internal — not user-facing) |
| `struct ConnectionID` | `std::vector<uint8_t>` everywhere; CIDs are just opaque bytes |
| `Server::set_quic_handler(...)` | **gone** — use `Server::set_quic_http3_service(...)` |
| `Server::set_http3_handler(...)` | **gone** — bundled into the service constructor |
| `enum QUICPacketType`, the toy state enums | Internal to `<libspaznet/quic/*>` now (`quic::LongType`, `quic::Connection::State`); not part of the public surface |

### How to migrate

If you had:

```cpp
class MyHandler : public spaznet::HTTP3Handler {
    spaznet::Task handle_request(...) override { ... }
};

int main() {
    spaznet::Server server;
    server.set_http3_handler(std::make_unique<MyHandler>());   // gone
    server.listen_udp(4433);
    server.run();
}
```

Change to:

```cpp
#include <libspaznet/quic/tls.hpp>
#include <libspaznet/quic/listener.hpp>
#include <libspaznet/http3/service.hpp>

int main() {
    using namespace spaznet;

    // 1. Build a TlsContext.  This is new — the toy stack had no TLS.
    quic::TlsServerConfig tls_cfg{cert_pem, key_pem, /*alpn*/ {"h3"}};
    auto tls = quic::TlsContext::make_server(tls_cfg);

    // 2. Configure the Listener.  These knobs are also new.
    quic::Listener::Config lcfg;
    lcfg.tls_ctx = tls;
    lcfg.server_tp.initial_max_data         = 1 << 20;
    lcfg.server_tp.initial_max_streams_bidi = 100;

    // 3. Build the service with your existing handler.
    auto service = std::make_unique<http3::QuicHttp3Service>(
        std::move(lcfg), std::make_unique<MyHandler>());

    Server server;
    server.set_quic_http3_service(std::move(service));        // new
    server.listen_udp(4433);
    server.run();
}
```

Walkthrough: [`quic-http3.md`](quic-http3.md).

### Why

The toy stack had no real TLS, no AEAD, hand-rolled non-RFC varints, no
ACKs / loss recovery / congestion control / flow control, no
Retry, no Version Negotiation, and no QPACK (HTTP/3 frames sent
headers as ASCII). It would interop with nothing. The rewrite (commits
`111fcd1` through `230681b`) replaced it with a from-scratch RFC 9000
/ 9001 / 9002 / 9114 / 9204 server that interops with our own client
end-to-end through the TLS handshake. Keeping the toy types around
would have meant maintaining two implementations of the same wire
protocol; they were deleted in one go.

### Sanity check

After updating, your code should compile only against
`<libspaznet/handlers/http3_handler.hpp>` (for `HTTP3Handler`,
`HTTP3Request`, `HTTP3Response`) and `<libspaznet/http3/service.hpp>`
+ `<libspaznet/quic/listener.hpp>` + `<libspaznet/quic/tls.hpp>` (for
the service wiring). If you're still `#include`-ing
`quic_handler.hpp` or `quic_server.hpp`, you'll get a "file not found"
error.

## 2026-05-28 — listen backlog bumped

Commit `477a21b`.

`Server::listen_tcp` now passes 4096 to `listen(2)` instead of
`SOMAXCONN` (128 on macOS). Linux honors the larger value; macOS
clamps to its sysctl ceiling (`kern.ipc.somaxconn`).

### What might break

If your application reasoned about a 128-deep accept queue (e.g. for
backpressure or rate-limiting), that assumption no longer holds on
Linux. Adjust whatever upstream rate-limit you'd planned to rely on.

In practice, nothing should break — the bigger backlog just lets the
kernel buffer more pending connections.

## Future breaks

A list of breaks that we've flagged but haven't made yet, so
downstreams can plan:

- **HTTP/2 dispatch wiring**: when `HTTP2Handler` is finally wired
  into `Server::handle_connection`, `set_http2_handler` will start
  *doing* something. Today it accepts a handler that's never invoked
  (see [`api-status.md`](api-status.md)). The compiled API won't
  change, but runtime behavior will.

- **QUIC client mode**: when added, `quic::Connection::Role` will
  gain a `Client` enumerator and several constructor parameters
  that today are server-only. Existing server code will continue to
  compile.

- **PTO retransmission**: today, dropped QUIC packets stay dropped
  (the bench passes on loopback). When PTO retransmit lands, the
  retransmit timer + congestion-control bookkeeping will fire,
  which may surface latency that you weren't seeing. No compile
  break — just expect more `Connection::on_timer` work per second.

These are *forward* breaks — they're called out so you don't pin a
SHA expecting a behavior that's about to change.

## Related

- [`CHANGELOG.md`](../CHANGELOG.md) — full chronological history
- [`api-status.md`](api-status.md) — stable vs experimental surface
- [TODO.md](../TODO.md) — what's coming next
