# UDP demos

Three small servers built on `spaznet::udp::make_dispatcher`, each on
its own port so they can run side by side.

## `echo.cpp` — minimal echo server

```bash
./udp_echo
echo -n hi | nc -u -w1 127.0.0.1 8080
```

Every datagram is `sendto()`'d straight back to the address it came
from. This is the "hello world" of the `Handler` interface — it shows
the wiring (`handle_packet` -> `sendto` using `Packet::listen_fd` /
`peer` / `peer_len`) but nothing UDP-specific: a request/reply loop
like this would look almost the same over TCP.

## `relay.cpp` — broadcast relay ("chat" over UDP)

```bash
./udp_relay
nc -u 127.0.0.1 8080     # terminal A
nc -u 127.0.0.1 8080     # terminal B
# type a line in A, it shows up in B, and vice versa
```

Demonstrates the defining consequence of UDP being connectionless: over
TCP/WebSocket, "who is connected" and "how do I reply to them" come for
free from the accepted connection's `Socket`/`Connection` object. UDP
has no connection object at all — the server has to *reconstruct*
membership itself from the datagrams it happens to receive, remembering
each peer's raw `sockaddr` in a table so it can `sendto()` them later.
Each incoming line is fanned out to every other known peer, with a join
notice the first time a new address is seen.

## `statsd.cpp` — fire-and-forget metrics aggregator

```bash
./udp_statsd
printf 'requests:1|c\ntemp:21.5|g' | nc -u -w0 127.0.0.1 8081
echo -n 'requests:1|c' | nc -u -w0 127.0.0.1 8081
# every ~2s:
#   [statsd] counters: requests=2
#            gauges:   temp=21.5
```

Demonstrates the other classic UDP pattern: one-way, fire-and-forget
telemetry with *no reply at all*. A minimal statsd-style line protocol
(`name:value|type`, `c` for counter or `g` for gauge; several
newline-separated metrics may share one datagram) is folded into
shared counters/gauges and periodically reported — callers never wait
on an ack, so a slow or unreachable collector can never make a sender
block.

### Why these show off UDP

Each demo leans on a property TCP/WebSocket don't have:

- **`relay`** — connectionless peer tracking. There's no connection
  object to hand out identity or membership; the server rebuilds "who's
  here" from raw sockaddrs, and one UDP socket `sendto()`s many peers.
- **`statsd`** — one-way, fire-and-forget delivery. The handler never
  calls `sendto()` at all; the whole value of UDP for telemetry is that
  senders don't wait for (and this server doesn't provide) an
  acknowledgment.
- Both rely on UDP preserving message boundaries: a `Packet` is exactly
  one datagram as the sender wrote it, so there's no framing/reassembly
  step the way a TCP-based protocol needs (see `example/http`'s chunked
  body parser, by contrast).
