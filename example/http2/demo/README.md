# HTTP/2 demos

Two small servers built on `spaznet::http2::make_dispatcher`, both
serving h2c (prior-knowledge cleartext HTTP/2) on the same port (8080).

## `hello.cpp` — minimal server

```bash
./http2_hello
curl --http2-prior-knowledge http://localhost:8080/
```

A single route that always returns `200 OK` with a fixed body. This is
the "hello world" of the `Handler` interface — enough to show how a
handler is wired into `Server::set_connection_handler`, but nothing
about what HTTP/2 actually adds over HTTP/1.1.

## `showcase.cpp` — stream multiplexing showcase

```bash
./http2_showcase
curl --http2-prior-knowledge http://localhost:8080/

# The multiplexing demo — 8 requests that each sleep 1s. Over HTTP/1.1
# keep-alive this would take ~8s serialized; over one multiplexed
# HTTP/2 connection it takes ~1s:
h2load -n8 -c1 -m8 'http://localhost:8080/slow?ms=1000'

curl --http2-prior-knowledge http://localhost:8080/stream-info
curl --http2-prior-knowledge --data-binary 'hello' http://localhost:8080/echo
curl --http2-prior-knowledge http://localhost:8080/status/404
```

- **`GET /slow?ms=N`** — the centerpiece. The handler
  `co_await`s a timer for `N` milliseconds before responding. While one
  stream's handler coroutine is suspended there, the dispatcher's frame
  loop and every other stream's handler coroutine on that same
  connection keep making progress — fire several `/slow` requests at
  once (`h2load -c1 -m8`, or several browser tabs against the same
  origin) and they complete together in ~N ms instead of serializing to
  N × (request count) ms the way HTTP/1.1 keep-alive would.
- **`GET /stream-info`** — echoes the stream id and both pseudo-headers
  (`:scheme`, `:authority`) and regular headers, all HPACK-decoded by
  the dispatcher before the handler sees them.
- **`POST /echo`** — echoes the request body, showing DATA-frame
  reassembly (the body may arrive as several DATA frames; the handler
  only ever sees the finished result).
- **`GET /status/<code>`** — returns an arbitrary status code, using
  `Response::set_status()` since HTTP/2 has no reason-phrase field.

This build of `example/http2` does not implement server push
(`PUSH_PROMISE`), stream `PRIORITY`, or trailers (see the header comment
in `src/dispatcher.cpp`), so the showcase sticks to what's actually
there: multiplexed streams, HPACK-decoded headers, DATA-frame bodies,
and dispatcher-enforced `SETTINGS` / `PING` / flow control /
`MAX_CONCURRENT_STREAMS`.

### Why this is a better HTTP/2 demonstration

`hello.cpp`'s single fixed response can't show anything HTTP/2-specific
— a client could send it a hundred requests and they'd all just look
like independent HTTP/1.1 exchanges that happen to share a wire format.
Multiplexing is the feature that actually distinguishes HTTP/2 from
HTTP/1.1 keep-alive: many requests in flight *concurrently* on one TCP
connection, rather than one at a time. `/slow?ms=N` is deliberately built
so its handler blocks (via a real suspend point, not a busy loop) long
enough to make concurrent-vs-serial behavior visible with a stopwatch —
that's only interesting to demonstrate if there's more than one stream
in flight at once, which is exactly the property `hello.cpp` never
exercises.
