# WebSocket handlers

`WebSocketHandler` is the entry point for RFC 6455 WebSocket connections.
The server handles the HTTP/1.1 → WebSocket upgrade handshake; your
handler sees only the post-handshake message stream.

```cpp
#include <libspaznet/server.hpp>
#include <libspaznet/handlers/websocket_handler.hpp>

class EchoHandler : public spaznet::WebSocketHandler {
public:
    spaznet::Task on_open(spaznet::Socket&) override { co_return; }
    spaznet::Task on_close(spaznet::Socket&) override { co_return; }

    // Echo back every message we receive. Take by rvalue so we can
    // move the payload into the outgoing frame without copying.
    spaznet::Task handle_message(spaznet::WebSocketMessage&& m,
                                 spaznet::Socket& s) override {
        co_await s.send_websocket_message(m.opcode, m.data);
    }

    // The const& overload is still required (pure-virtual in the base);
    // forward to the rvalue one so both paths agree.
    spaznet::Task handle_message(const spaznet::WebSocketMessage& m,
                                 spaznet::Socket& s) override {
        co_await s.send_websocket_message(m.opcode, m.data);
    }
};

int main() {
    spaznet::Server server(4);
    server.set_websocket_handler(std::make_unique<EchoHandler>());
    server.listen_tcp(8080);
    server.run();
}
```

## Connection lifecycle

```
client                            server
  |                                 |
  |    HTTP/1.1 Upgrade: websocket  |
  |-------------------------------->|
  |    HTTP/1.1 101 Switching       |
  |<--------------------------------|
  |                                 |  on_open(socket) fires
  |    frame (masked, client→srv)   |
  |-------------------------------->|  handle_message(msg, socket)
  |                                 |
  |    frame (unmasked, srv→client) |
  |<--------------------------------|
  |             ...                 |
  |                                 |
  |    Close frame                  |
  |<------------------------------->|  on_close(socket) fires
```

- `on_open` runs once, after the handshake response has been written.
  Suspend here if you need to do per-connection setup before accepting
  messages.
- `handle_message` runs once per fully-reassembled message (RFC 6455
  fragments are joined for you).
- `on_close` runs once, after the connection is torn down. The `Socket&`
  is still alive at this point but writing to it is a no-op.

All three are coroutines — `co_await` freely inside them.

## The two `handle_message` overloads

```cpp
virtual Task handle_message(const WebSocketMessage&, Socket&) = 0;
virtual Task handle_message(WebSocketMessage&&,      Socket&);
```

The dispatcher always calls the rvalue overload first. The default
rvalue body forwards to the `const&` form — so existing handlers that
override only `const&` continue to work.

Override the rvalue form when you want to **consume** the payload (move
it into a buffer, into a response, into a parser). Override the `const&`
form when you only need to read it.

If you override both — like the echo example above — make sure they agree.

## Sending messages

Two send paths exist; prefer the first.

### `Socket::send_websocket_message` (preferred)

```cpp
co_await socket.send_websocket_message(WebSocketOpcode::Binary, payload);
co_await socket.send_websocket_message(WebSocketOpcode::Text,   bytes_of("hi"));
co_await socket.send_websocket_message(WebSocketOpcode::Pong,   ping_payload);
```

Builds the WebSocket frame header (FIN=1, no mask) directly into a
single buffer with one allocation, copies the payload once, and hands
it to `async_write`. This is the fast path; bench_websocket shows it
running ~30% lower CPU/msg than the older `WebSocketFrame::serialize()`
path on small payloads.

Pass `false` for `fin` to send a non-final fragment (rare; only useful
for streaming a huge payload as multiple frames).

### `WebSocketFrame::serialize()` (legacy, still works)

```cpp
WebSocketFrame f;
f.fin = true;
f.opcode = WebSocketOpcode::Binary;
f.masked = false;            // server frames are never masked
f.payload = std::move(data);
f.payload_length = f.payload.size();
co_await socket.async_write(f.serialize());
```

This path allocates twice (once for the value type, once for the
serialized output) and copies the payload twice. Functional but slow.

## Opcodes

| Opcode | Hex | Meaning |
|---|---|---|
| `Continuation` | `0x0` | Fragment of a previous message. You never see this — the server joins fragments for you. |
| `Text` | `0x1` | UTF-8 payload. The server doesn't validate UTF-8; that's your responsibility per RFC 6455 §8.1 if it matters. |
| `Binary` | `0x2` | Opaque bytes. |
| `Close` | `0x8` | Connection-close. The server handles the Close handshake; you'll see `on_close` shortly. |
| `Ping` | `0x9` | The server auto-Pongs and does not deliver Ping to your handler. |
| `Pong` | `0xA` | Server-side ping replies. Delivered to your handler only if you initiated a Ping yourself. |

Control frames (`Close`, `Ping`, `Pong`) MUST have payload ≤125 bytes
per RFC 6455 §5.5; the server rejects oversized control frames with a
`1002 protocol error` close.

## Close codes

To initiate a close yourself:

```cpp
co_await socket.send_websocket_message(WebSocketOpcode::Close,
                                       /* empty body */ std::span<const uint8_t>{});
```

To send a close with a status code + reason, build the body manually:

```cpp
std::vector<uint8_t> body;
body.push_back(uint8_t(1011 >> 8));   // status code, big-endian
body.push_back(uint8_t(1011 & 0xFF));
auto reason = std::string_view("server shutting down");
body.insert(body.end(), reason.begin(), reason.end());
co_await socket.send_websocket_message(WebSocketOpcode::Close, body);
```

The server echoes any incoming Close frame back to the peer with the
peer's code + reason intact, then closes the TCP connection. Your
`on_close` runs after that round-trip.

Reserved close codes (RFC 6455 §7.4):

| Code | Meaning |
|---|---|
| 1000 | Normal closure |
| 1001 | Endpoint going away |
| 1002 | Protocol error |
| 1003 | Cannot accept data type |
| 1007 | Invalid UTF-8 in Text frame |
| 1008 | Policy violation |
| 1009 | Message too big |
| 1011 | Server-side error |

## Limits

| Limit | Value | Notes |
|---|---:|---|
| Maximum frame payload | 16 MiB (`WebSocketFrame::kMaxPayloadBytes`) | A peer that declares more gets a `1009 message too big` close. |
| Handshake size | 8 KiB | Upgrade request headers must fit. |

Reserved bits (RSV1/RSV2/RSV3) in incoming frames trigger a `1002`
close. There's no per-message-deflate (RFC 7692) support — peers that
advertise it will see their offer ignored and use uncompressed frames.

## Threading

The handler is a single shared instance across all connections. The
same connection's coroutines all run on the same IOContext thread
(coroutines can migrate between threads on `co_await`, but for any
given suspension point the resume lands on whatever thread the IO
event came in on).

If you store per-connection state, key it by `socket.fd()` and protect
the lookup with a `std::mutex` — the default IOContext doesn't pin
connections to a thread.

## Errors

Throwing from any of the three callbacks closes the connection (the
exception is swallowed in `Server::handle_connection`'s catch-all). The
peer sees the TCP connection drop without a Close frame.

To gracefully close, send a Close frame yourself and return from the
handler; the server will tear down the connection after.

## Related

- [api-status.md](api-status.md) — overall feature matrix
- [http.md](http.md) — HTTP/1.1 handler (the upgrade handshake goes
  through the same `Server`)
- [threading.md](threading.md) — `Server(N)` thread count tuning
