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
    spaznet::Task handle_request(
        const spaznet::http::HTTPRequest& request,
        spaznet::http::HTTPResponse& response,
        spaznet::Socket& socket
    ) override {
        response.status_code = 200;
        response.reason_phrase = "OK";
        response.set_header("Content-Type", "text/plain");
        response.body = {'H', 'i', '\n'};
        co_return;
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

`handle_request` is a `Task` — a C++20 coroutine.  You can `co_await`
inside it (database calls, downstream HTTP fetches, file I/O) and the
connection won't block other connections on the same IOContext thread.

The handler instance is shared across all connections (a single
`unique_ptr` is wrapped in a `shared_ptr` inside `make_dispatcher`).
Don't store per-connection state on `this` — use a coroutine local,
or a member keyed by `socket.fd()`.

Keep-alive is automatic: after `handle_request` returns and the
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
Two known gaps:

- **Trailers** (RFC 9112 §7.1.2) are not parsed. A peer that emits
  any trailer field after the last chunk stalls in `Incomplete` until
  the read timeout closes the connection.
- **Chunk-extension lines** are capped at 64 bytes. Real-world
  integrity-tag extensions exceed this.

If your peer is well-behaved (no trailers, no extensions), neither
limit fires.

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
- Coroutine-per-stream multiplexing — request handlers run serially
  per connection.  Correct for request/response but lower throughput
  than a true multiplexed implementation.  Tracked in
  [`api-status.md`](api-status.md).

## The `Socket&` parameter (both protocols)

You usually don't need to touch the `Socket&`. It's exposed so that
handlers needing to send raw bytes (SSE streaming, manual chunked
output) can.  If you do touch it:

- `socket.async_write(...)` writes raw bytes.  Anything you write
  here will be prepended to the response the server sends after
  `handle_request` returns, which is almost never what you want.
- `socket.close()` ends the connection.  The dispatcher will skip
  the response serialize/write step if you call this.
- It is **not safe** to capture `socket` and use it from another
  coroutine running on a different IOContext thread.

## Errors

There's no explicit error path from `handle_request`.  To return an
error response, set `response.status_code` and `response.body` as
usual:

```cpp
if (!authenticated(request)) {
    response.status_code = 401;
    response.reason_phrase = "Unauthorized";        // HTTP/1.1
    response.set_header("WWW-Authenticate", "Basic realm=\"app\"");
    co_return;
}
```

If `handle_request` throws, HTTP/1.1 closes the connection and
HTTP/2 sends `RST_STREAM(INTERNAL_ERROR)` and continues serving
other streams on the same connection.

## Related

- [api-status.md](api-status.md) — overall feature matrix
- [threading.md](threading.md) — when to use `Server(0)` vs `Server(N)`
- [websocket.md](websocket.md) — WebSocket handler API
- [quic-http3.md](quic-http3.md) — HTTP/3 via QUIC
- [migration.md](migration.md) — breaking changes from the pre-restructure API
