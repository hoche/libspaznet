# WebSocket demos

Two small servers built on `spaznet::websocket::make_dispatcher`, both
serving plain HTTP/1.1 and WebSocket on the same port (8080).

## `echo.cpp` — minimal echo server

```bash
./ws_echo
wscat -c ws://localhost:8080/
> hi
< hi
```

Every message a client sends is written straight back to that same
client. `handle_message` reads `Connection& conn` and `on_open`/`on_close`
do nothing. This is the "hello world" of the `Handler` interface: it
shows the frame parsing, masking/unmasking, fragmentation handling, and
handshake logic in `src/dispatcher.cpp` without any handler-side state.

## `chat.cpp` — broadcast chat room

```bash
./ws_chat
# open http://localhost:8080/ in two or more browser tabs
```

Every client that connects joins a shared room; a text message from one
client is fanned out to every *other* connected client, with join/leave
notices in between. The HTTP fallback on the same port serves a small
self-contained HTML+JS chat page, so the whole thing can be driven from a
browser with no extra tooling.

### Why this is a better WebSocket demonstration

An echo server only exercises a connection talking to itself — it would
look almost identical built on plain TCP with a request/response loop.
Chat is a better fit for WebSocket specifically because it needs the two
things HTTP request/response doesn't give you:

- **The server pushes data the client didn't ask for.** A message from
  client A has to arrive on client B's socket with no corresponding
  request from B. That's the entire reason WebSocket exists instead of
  polling.
- **State and events live for the life of the connection, not one
  request.** `on_open`/`on_close` become meaningful (join/leave a shared
  room) rather than no-ops, and the connection itself is the addressable
  "who do I send this to" identity.

That combination also surfaces the concurrency problem a real WebSocket
server has to solve and an echo server never encounters: one connection's
`handle_message` needs to deliver data to a *different* connection's
socket, which it does not own and cannot safely write to directly. The
demo's `Session`/`ChatRoom`/`writer_loop` machinery (see the comment block
at the top of `chat.cpp`) exists to solve exactly that — each connection
queues outbound data for its peers and only ever writes its own socket,
serialized against the dispatcher's own control-frame writes (Pong,
Close) via the `Connection` write gate in `src/dispatcher.cpp`. Echo never
needs any of this because it only ever writes back to the connection that
is already asking it to.
