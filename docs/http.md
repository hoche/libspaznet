# HTTP/1.1 handlers

`HTTPHandler` is the entry point for serving HTTP/1.1 requests. Subclass
it, implement `handle_request`, and register the handler on a `Server`:

```cpp
#include <libspaznet/server.hpp>
#include <libspaznet/handlers/http_handler.hpp>

class MyHandler : public spaznet::HTTPHandler {
public:
    spaznet::Task handle_request(
        const spaznet::HTTPRequest& request,
        spaznet::HTTPResponse& response,
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
    spaznet::Server server(4);                 // 4 IO worker threads
    server.set_http_handler(std::make_unique<MyHandler>());
    server.listen_tcp(8080);
    server.run();
}
```

## `HTTPRequest`

The handler receives a fully-parsed request:

| Field | Type | Notes |
|---|---|---|
| `method` | `std::string` | Uppercase verb (`GET`, `POST`, ...). |
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

## `HTTPResponse`

Build the response in place. The server serializes and writes it for you
after `handle_request` returns:

```cpp
response.status_code = 200;
response.reason_phrase = "OK";
response.set_header("Content-Type", "application/json");
response.set_header("Cache-Control", "no-store");
response.body = {/* bytes */};
```

Defaults: `version = "1.1"`, `status_code = 200`, `reason_phrase = "OK"`,
empty headers, empty body.

`Content-Length` is **set automatically** to `body.size()` if you don't
set it yourself. `Connection` is set automatically based on whether the
request asked for keep-alive (and whether the server is willing — it
always is, unless you set `Connection: close` yourself).

## The `Socket&` parameter

You usually don't need to touch the `Socket&`. It's exposed so that
handlers that need to send raw bytes (e.g. SSE streaming, manual chunked
output) can. If you do touch it:

- `socket.async_write(...)` writes raw bytes. Anything you write here
  will be prepended to the response the server sends after
  `handle_request` returns, which is almost never what you want.
- `socket.close()` ends the connection. The server will skip the response
  serialize/write step if you call this.
- It is **not safe** to capture `socket` and use it from another
  coroutine running on a different IOContext thread.

## Lifecycle

`handle_request` is a `Task` — a C++20 coroutine. You can `co_await`
inside it (database calls, downstream HTTP fetches, file I/O) and the
connection won't block other connections on the same IOContext thread:

```cpp
spaznet::Task handle_request(
    const spaznet::HTTPRequest&, spaznet::HTTPResponse& resp,
    spaznet::Socket& sock
) override {
    auto rows = co_await db_query(...);   // suspends; doesn't block thread
    resp.body = serialize(rows);
    co_return;
}
```

The handler instance is shared across all connections (a single
`unique_ptr` lives in `Server`). Don't store per-connection state on
`this` — use a local in the coroutine, or capture into a member that's
keyed by `socket.fd()`.

Keep-alive is automatic: after `handle_request` returns and the response
is written, the server reads the next request on the same TCP
connection. The handler is invoked again with a fresh `HTTPRequest`.

## Header sanitization

To prevent header-injection from leaking through your handler, the
response serializer silently **drops** any header entry whose:

- name isn't a valid token per RFC 9112 §5.6.2 (e.g. contains spaces, control bytes, or non-ASCII), or
- value contains `\r`, `\n`, or `\0`.

If your `Content-Type` value disappears, this is why — check for stray
newlines.

## Protocol limits

These are baked in to defend against Slowloris and oversized requests:

| Limit | Value | Where |
|---|---:|---|
| Maximum request size (headers + body) | 1 MiB | `server_impl.cpp` (`kMaxRequestBytes`) |
| Read chunk per `async_read` | 8 KiB | `server_impl.cpp` (`kReadChunk`) |
| Maximum number of header fields | 100 | `http_handler.cpp` (`kMaxHeaders`) |
| Chunked-encoding chunk-size line | 64 bytes | `http_handler.cpp` |

A request that exceeds the size cap gets a `400 Bad Request` with
`Connection: close`; the parser does not try to recover.

## Chunked requests

`Transfer-Encoding: chunked` requests are decoded for you — `body` holds
the concatenated chunks by the time `handle_request` runs. Two known
gaps you may hit:

- **Trailers** (RFC 9112 §7.1.2) are not parsed. A peer that emits any
  trailer field after the last chunk stalls in `Incomplete` until the
  read timeout closes the connection. Tracked in [TODO.md](../TODO.md).
- **Chunk-extension lines** are capped at 64 bytes. Real-world
  integrity-tag extensions exceed this. Same tracking issue.

If your peer is well-behaved (no trailers, no extensions), neither
limit fires.

## Errors

There's no explicit error path from `handle_request`. To return an
error response, set `response.status_code` and `response.body` as
usual:

```cpp
if (!authenticated(request)) {
    response.status_code = 401;
    response.reason_phrase = "Unauthorized";
    response.set_header("WWW-Authenticate", "Basic realm=\"app\"");
    co_return;
}
```

If `handle_request` throws, the connection is closed (the exception is
swallowed by the connection coroutine's catch-all). The peer sees a
truncated read or EOF mid-response.

## What's not here

- **HTTPS / TLS termination**: terminate in front (nginx, Envoy) or
  build a TLS shim using OpenSSL. The HTTP path itself is plaintext.
- **HTTP/2**: types exist (`HTTP2Handler`, `HTTP2Parser`, `HPACK`) but
  `Server::handle_connection` doesn't actually dispatch to them yet.
  See [api-status.md](api-status.md).
- **WebSocket upgrade**: handled separately via `set_websocket_handler`
  on the same `Server`. See [websocket.md](websocket.md).

## Related

- [api-status.md](api-status.md) — overall feature matrix
- [threading.md](threading.md) — when to use `Server(0)` vs `Server(N)`
- [websocket.md](websocket.md) — WebSocket handler API
