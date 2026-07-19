# HTTP/1.x demos

Two small servers built on `spaznet::http::make_dispatcher`, both
serving plain HTTP/1.1 on the same port (8080).

## `hello.cpp` — minimal server

```bash
./http_hello
curl http://localhost:8080/
```

A single route that always returns `200 OK` with a fixed body. This is
the "hello world" of the `HTTPHandler` interface — enough to show how a
handler is wired into `Server::set_connection_handler`, but nothing about
the protocol itself.

## `showcase.cpp` — HTTP/1.0 vs HTTP/1.1 feature showcase

```bash
./http_showcase
curl -v --http1.0 http://localhost:8080/info   # Connection: close by default
curl -v --http1.1 http://localhost:8080/info   # Connection: keep-alive by default
curl -v --http1.1 http://localhost:8080/chunked   # Transfer-Encoding: chunked
curl -v --http1.0 http://localhost:8080/chunked   # falls back to Content-Length
curl --data-binary 'hello' http://localhost:8080/echo
curl -H 'Transfer-Encoding: chunked' --data-binary 'hello' http://localhost:8080/echo
curl -v http://localhost:8080/status/404
```

The same routes are hit with `--http1.0` and `--http1.1` on purpose, so
the version-dependent differences are visible side by side instead of
buried in a spec:

- **`GET /info`** — echoes the request line, headers, and the
  negotiated keep-alive decision. HTTP/1.0 defaults to closing the
  connection after one response; HTTP/1.1 defaults to keeping it open.
  That decision is made by `HTTPRequest::should_keep_alive()` and fed
  straight into the response's `Connection` header by the dispatcher.
- **`POST /echo`** — echoes the request body back. Works identically
  whether the client uploaded it with `Content-Length` framing or
  `Transfer-Encoding: chunked` — `example/http`'s parser reassembles
  either into `HTTPRequest::body` before the handler ever runs, so the
  handler code can't tell (and doesn't need to) which one was used.
- **`GET /chunked`** — HTTP/1.1 clients get the response framed with
  `Transfer-Encoding: chunked`. HTTP/1.0 predates chunked encoding
  entirely (RFC 1945 has no `Transfer-Encoding` header), so 1.0 clients
  get the identical body framed with `Content-Length` instead.
- **`GET /status/<code>`** — returns that status code with a matching
  reason phrase, showing arbitrary status/reason handling.

### Why this is a better HTTP/1.x demonstration

`hello.cpp` returns the same response no matter what's sent to it, so it
can't show anything version-specific. The 1.0/1.1 split only shows up in
things a single fixed response can't exercise: how the *request*
declares (or omits) framing and connection-persistence information, and
how the *response* has to adapt to what the request version allows. Both
directions — decoding an incoming chunked or Content-Length body, and
choosing whether the outgoing body may use chunked encoding — depend on
`req.version`, which is exactly what this showcase is built to surface.

Features the stack doesn't implement (byte ranges, content
negotiation/compression, `100-continue`) are intentionally left out.
