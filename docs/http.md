# HTTP/1.1 + HTTP/2

The protocol implementations live in `example/<protocol>/` libraries
on top of the core `spaznet` target.  They register with `Server` via
the low-level `set_connection_handler` callback that
`make_dispatcher(...)` produces.

## HTTP/1.1 — `example/http`

```cpp
#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>
#include <libspaznet/server.hpp>

class MyHandler : public spaznet::http::HTTPHandler {
public:
    void handle_request(
        const spaznet::http::HTTPRequest& request,
        spaznet::http::ResponseWriter writer
    ) override {
        spaznet::http::HTTPResponse response;
        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'H', 'i', '\n'};
        writer.complete(std::move(response));
    }
};

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(
        spaznet::http::make_dispatcher(std::make_unique<MyHandler>()));
    server.listen_tcp(8080);
    server.run();
}
```

Build a downstream program against it by linking the `spaznet_http`
library (alongside `spaznet`).  With CMake:

```cmake
find_package(spaznet REQUIRED)        # or add_subdirectory(libspaznet)
target_link_libraries(myapp PRIVATE spaznet::spaznet spaznet::http)
```

### Two dispatchers, one handler

`example/http` ships two interchangeable dispatchers for the same
`HTTPHandler` interface — pick one, or run both side by side:

- **`make_dispatcher(...)`** (above) — coroutine-based, registered via
  `Server::set_connection_handler`. Each connection runs as a `Task`
  (`serve_keep_alive` in `dispatcher.cpp`).
- **`make_reactor_dispatcher(...)`** — coroutine-free, registered via
  `Server::set_connection_factory`. Each connection is a small explicit
  state machine (`Http1Connection` in `dispatcher_reactor.cpp`) built on
  `BufferedConnection` instead of a suspended coroutine frame.

```cpp
server.set_connection_factory(
    spaznet::http::make_reactor_dispatcher(std::make_unique<MyHandler>()));
```

Both parse requests with the same `HTTPParser`, serialize responses with
the same `HTTPResponse::serialize()`/`serialize_chunked()`, and answer
through the same `ResponseWriter` — from a client's point of view they
are indistinguishable. `example/http/demo/hello.cpp` and `showcase.cpp`
both accept a `--reactor` flag to switch, and the integration test suite
(`example/http/tests/integration/`) runs every scenario against both via
`dispatcher_test_support.hpp`'s `DispatcherKind` parameterization — any
behavioral divergence between them is treated as a bug. See
`docs/concurrency-and-coroutines.md` for how the two execution models
differ under the hood.

### `HTTPRequest`

The handler receives a fully-parsed request:

| Field | Type | Notes |
|---|---|---|
| `method` | `std::string` | Uppercase verb (`GET`, `POST`, …). |
| `request_target` | `std::string` | The raw target as it appeared on the wire (`/foo?bar=1`). No normalization. |
| `version` | `std::string` | `"1.1"` for HTTP/1.1; `"1.0"` for HTTP/1.0. |
| `headers` | `unordered_map<string, string>` | Header field names are stored **lowercased** so `get_header("Content-Type")` and `get_header("content-type")` both work. Multi-value headers are concatenated with `, `. |
| `body` | `vector<uint8_t>` | Decoded body. For chunked encoding the chunks have already been concatenated. |

Convenience accessors:

```cpp
auto ct = request.get_header("Content-Type");      // std::optional<std::string>
bool keep = request.should_keep_alive();           // Honors Connection: close
auto len  = request.get_content_length();          // std::optional<size_t>
bool chunked = request.is_chunked();               // Transfer-Encoding: chunked
```

### `HTTPResponse`

Build the response in place. The server serializes and writes it for
you after `handle_request` returns:

```cpp
response.status_code = 200;
response.reason_phrase = "OK";
response.set_header("Content-Type", "application/json");
response.set_header("Cache-Control", "no-store");
response.body = {/* bytes */};
```

Defaults: `version = "1.1"`, `status_code = 200`,
`reason_phrase = "OK"`, empty headers, empty body.

`Content-Length` is **set automatically** to `body.size()` if you
don't set it yourself. `Connection` is set automatically based on
whether the request asked for keep-alive (and whether the server is
willing — it always is, unless you set `Connection: close` yourself).

### Lifecycle

`handle_request` is a plain, synchronous, non-coroutine virtual
function — no `co_await`, no `Task`. Answer immediately by building an
`HTTPResponse` and calling `writer.complete(std::move(response))`
before returning; that's the entire handler for the common case, and
it's indistinguishable from any other synchronous function call.

If you need to defer — background work, a downstream call that
finishes later, a timer — move or copy `writer` (it's cheap: a
`shared_ptr` under the hood) into whatever will eventually have the
answer, and call `.complete()` from there instead, on whatever thread
or callback that ends up being. Only the first `complete()` call
across all copies of a given `writer` takes effect; later ones are
silently ignored, so a handler racing a timeout against real work
never double-answers. Internally, the dispatcher suspends its
coroutine until your `writer.complete()` runs — but that's dispatcher
plumbing, not something your handler needs to know about.

The handler instance is shared across all connections (a single
`unique_ptr` is wrapped in a `shared_ptr` inside `make_dispatcher`).
Don't store per-connection state on `this` — use a local in
`handle_request`, capture it into whatever you hand `writer` off to,
or keep a member map keyed by something request-derived.

Keep-alive is automatic: after `writer.complete()` runs and the
response is written, the server reads the next request on the same
TCP connection.  The handler is invoked again with a fresh
`HTTPRequest`.

### Header sanitization

To prevent header-injection from leaking through your handler, the
response serializer silently **drops** any header entry whose:

- name isn't a valid token per RFC 9112 §5.6.2 (e.g. contains spaces,
  control bytes, or non-ASCII), or
- value contains `\r`, `\n`, or `\0`.

If your `Content-Type` value disappears, this is why — check for
stray newlines.

### Protocol limits

These are baked in to defend against Slowloris and oversized requests:

| Limit | Value | Where |
|---|---:|---|
| Maximum request size (headers + body) | 1 MiB | `example/http/src/dispatcher.cpp` (`kMaxRequestBytes`) |
| Read chunk per `async_read` | 8 KiB | `example/http/src/dispatcher.cpp` (`kReadChunk`) |
| Maximum number of header fields | 100 | `example/http/src/handler.cpp` (`kMaxHeaders`) |
| Chunked-encoding chunk-size line | 64 bytes | `example/http/src/handler.cpp` |

A request that exceeds the size cap gets a `400 Bad Request` with
`Connection: close`; the parser does not try to recover.

### Chunked requests

`Transfer-Encoding: chunked` requests are decoded for you — `body`
holds the concatenated chunks by the time `handle_request` runs.

- **Trailers** (RFC 9112 §7.1.2) between the last-chunk (`0\r\n`)
  and the final CRLF are consumed and dropped. We don't expose
  trailer fields on `HTTPRequest` today; if you need them, the
  hook is in `parse_chunked_body` in
  `example/http/src/handler.cpp`.
- **Chunk-extension lines** (the optional `;key=value` after a
  chunk size) are capped at 4 KiB so a peer can't make us scan an
  unbounded buffer. That's well above any real-world integrity-tag
  extension.

## HTTP/1.1 + WebSocket — `example/http-websocket`

If the same TCP port should serve both HTTP/1.1 and WebSocket
upgrades, use `example/http-websocket` instead.  Its
`make_dispatcher(http_handler, ws_handler)` accepts both an
`http::HTTPHandler` and a `websocket::Handler`; it sniffs each
accepted connection and either runs the WS frame loop or hands
the buffer off to the HTTP dispatcher.  See
[`websocket.md`](websocket.md) for the WS-specific API.

## HTTP/2 (h2c) — `example/http2`

```cpp
#include <libspaznet/http2/dispatcher.hpp>
#include <libspaznet/http2/handler.hpp>
#include <libspaznet/server.hpp>

class MyHandler : public spaznet::http2::Handler {
public:
    spaznet::Task handle_request(
        const spaznet::http2::Request& request,
        spaznet::http2::Response& response,
        spaznet::Socket& socket
    ) override {
        response.status_code = 200;
        response.headers["content-type"] = "text/plain";
        const char body[] = "Hello, HTTP/2!\n";
        response.body.assign(body, body + sizeof(body) - 1);
        co_return;
    }
};

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(
        spaznet::http2::make_dispatcher(std::make_unique<MyHandler>()));
    server.listen_tcp(8080);
    server.run();
}
```

```cmake
target_link_libraries(myapp PRIVATE spaznet::spaznet spaznet::http2)
```

This is **h2c, prior-knowledge cleartext** (RFC 9113 §3.4).  Test it
with:

```bash
curl --http2-prior-knowledge -s -i http://127.0.0.1:8080/
```

For h2-over-TLS (the `h2` ALPN), terminate TLS in front; libspaznet
does not include a TLS server for TCP today.  The QUIC stack
(`example/quic-http3`) is a separate path that does include TLS via
OpenSSL 3.5+.

### What's implemented (RFC 9113)

- Connection preface (§3.4)
- SETTINGS exchange + ACK (§6.5)
- Multiplexed streams keyed by 31-bit stream ID (§5.1)
- HEADERS / CONTINUATION → user's `handle_request` (§6.2, §6.10)
- DATA frame request-body assembly (§6.1)
- HEADERS + DATA response emission, chunked by peer's
  `SETTINGS_MAX_FRAME_SIZE` (§4.2)
- Per-stream + connection-level flow control with WINDOW_UPDATE
  emission after every consumed byte (§5.2, §6.9)
- PING / PING-ACK (§6.7)
- GOAWAY emission on shutdown (§6.8)
- RST_STREAM on stream-level errors (§6.4)
- HPACK with prefix-N varint integers, all 4 representations, and
  Huffman decode (RFC 7541 §5 + §B, decode-only; we emit literals
  without Huffman, which is RFC-conformant)
- Static-table HPACK only (we advertise `SETTINGS_HEADER_TABLE_SIZE = 0`
  so peers don't index against a dynamic table either)

### What's not in `example/http2`

- HTTP/2 over TLS (no TLS terminator in the core; terminate in front).
- HPACK dynamic table (intentional — see above).
- PUSH_PROMISE / server push (disabled via SETTINGS).
- Trailers, priority frames (priority frames are silently dropped
  per RFC 9113 §4.1).
- True per-frame priority weighting.  We don't reorder pending
  writes by stream priority; the queue is FIFO across all streams.
  Frames are still allowed to interleave across streams because
  every HEADERS we emit carries `END_HEADERS` (so RFC 9113 §6.10's
  "no other frames between HEADERS and CONTINUATION" doesn't bind
  us).  Tracked in [`api-status.md`](api-status.md).

> Concurrent multiplexing **is** wired (2026-05-31): each fully-
> arrived request dispatches as a detached coroutine, so a slow
> handler on stream A no longer stalls PING-ACK, WINDOW_UPDATE, or
> handlers for streams B / C / D.  Wire writes funnel through a
> single per-connection writer coroutine to keep individual
> frames atomic on the wire.  Handlers MUST NOT call
> `socket.async_write` directly when running under the multiplexed
> dispatcher — that bypasses the writer and races with other
> handlers' frames.  Use the `Response` object instead.

## The `Socket&` parameter (HTTP/2 only)

HTTP/1.1's `handle_request` has no `Socket&` parameter — writes are
entirely mediated by `ResponseWriter`, so there's nothing to expose.
HTTP/2's `handle_request` still takes one (its handler is still
coroutine-based; see below). You usually don't need to touch it —
it's exposed so that handlers needing to send raw bytes can — and if
you do:

- `socket.async_write(...)` writes raw bytes, bypassing the
  per-connection writer. **Never do this under the HTTP/2
  dispatcher** — see the multiplexing warning above.
- It is **not safe** to capture `socket` and use it from another
  coroutine running on a different IOContext thread.

## Errors

HTTP/1.1's `handle_request` has no explicit error path: build the
response you want to send (including error responses) and complete
`writer` with it.

```cpp
void handle_request(const HTTPRequest& request, ResponseWriter writer) override {
    if (!authenticated(request)) {
        HTTPResponse response;
        response.status_code = 401;
        response.reason_phrase = "Unauthorized";
        response.set_header("WWW-Authenticate", "Basic realm=\"app\"");
        writer.complete(std::move(response));
        return;
    }
    // ...
}
```

HTTP/2's `handle_request` is still a coroutine (`Task`); the
equivalent there is `co_return` after setting `response.status_code`.

If HTTP/1.1's `handle_request` throws (synchronously, before
returning) the connection is closed. HTTP/2 sends
`RST_STREAM(INTERNAL_ERROR)` and continues serving other streams on
the same connection.

## Related

- [api-status.md](api-status.md) — overall feature matrix
- [threading.md](threading.md) — when to use `Server(0)` vs `Server(N)`
- [websocket.md](websocket.md) — WebSocket handler API
- [quic-http3.md](quic-http3.md) — HTTP/3 via QUIC
- [migration.md](migration.md) — breaking changes from the pre-restructure API
