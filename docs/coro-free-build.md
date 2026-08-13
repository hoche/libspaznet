# Building without coroutines

`libspaznet` has two runtimes for every protocol it speaks: a
C++20-coroutine one and a callback/state-machine ("reactor") one. They
share everything except the per-connection dispatch loop — the same
event loop, the same platform I/O backends, the same protocol codecs
(HTTP parser, HPACK, WebSocket framing, the entire QUIC/HTTP3 stack),
and the same handler interfaces a user writes against. `-DSPAZNET_ENABLE_COROUTINES=OFF`
builds only the reactor half: the core library, all five protocol
libraries, every demo, and the full test suite, with zero
`<coroutine>`/`Task`/`co_await` anywhere in the compiled output.

This document covers three things: how the two runtimes are layered,
how to write a new reactor state machine if you're extending one of
the protocol dispatchers (or adding a new protocol), and the
lifetime/threading rules a reactor `IoHandler` must follow. For the
coroutine runtime specifically, see `docs/concurrency-and-coroutines.md`;
for the threading primitive both runtimes' completion paths rely on,
see that document's "Reactor Threading Model" section.

## Why this exists

Coroutine frames are self-contained: a suspended `Task` carries its own
local state and can be resumed on any thread that drains the right
queue, which is what lets `IOContext` round-robin coroutine work across
worker threads for free. That's a genuine strength, and it's also a
hard dependency on a C++20 language feature that not every toolchain
or target wants — bare-metal/embedded cross-compilers, older compiler
versions pinned for other reasons, or codebases with a policy against
compiler-synthesized coroutine state machines in the debug build.

Rather than fork the codebase to serve that audience, every dispatcher
that suspends is mirrored by a second one that doesn't: an explicit
state machine with the same phases the coroutine version suspends
between, driven by ordinary function calls from `BufferedConnection`'s
`on_data()`/`on_closed()` callbacks instead of `co_await`. The pair is
differentially tested — the same integration test suite runs against
both dispatchers via `DispatcherKind::{Coroutine, Reactor}` parameterization
(see `dispatcher_test_support.hpp` in each `example/*/tests/integration/`)
— so behavioral divergence between them is a caught bug, not a documentation
gap.

## The build matrix

| `SPAZNET_ENABLE_COROUTINES` | What you get |
|---|---|
| `ON` (default) | Everything: both dispatchers per protocol, `Server::set_connection_handler`/`set_datagram_handler` (coroutine) *and* `set_connection_factory`/`set_sync_datagram_handler` (reactor), `make_dispatcher(...)` *and* `make_reactor_dispatcher(...)` in every protocol header. |
| `OFF` | Reactor only. `make_dispatcher(...)`, `ConnectionHandler`, `DatagramHandler`, and `Server::set_connection_handler`/`set_datagram_handler` are `#ifdef SPAZNET_HAS_COROUTINES`'d out of the public headers — not just unused, genuinely absent, so a downstream build can't accidentally reintroduce the dependency by calling them. `make_reactor_dispatcher(...)` and `set_connection_factory`/`set_sync_datagram_handler` are unaffected. |

`SPAZNET_HAS_COROUTINES` is the corresponding preprocessor define,
set `PUBLIC` on the `spaznet` CMake target when the option is `ON` so
every downstream consumer (including your own code, if you link
`spaznet::spaznet`) sees the same definition the library itself was
built with — check it, don't check `SPAZNET_ENABLE_COROUTINES`
(that's a CMake cache variable, not visible to C++).

```bash
cmake -B build-reactor-only -DSPAZNET_ENABLE_COROUTINES=OFF
cmake --build build-reactor-only
ctest --test-dir build-reactor-only --output-on-failure
```

This is exercised on every push/PR: the `Linux x64` CI workflow
(`.github/workflows/linux-x64.yml`) runs the full build-and-test cycle
twice, once per value of `SPAZNET_ENABLE_COROUTINES`.

Per-protocol source files that are entirely coroutine (a whole
`dispatcher.cpp`, never a `dispatcher_reactor.cpp`) are excluded from
the CMake source list rather than `#ifdef`'d internally — see e.g.
`example/http2/CMakeLists.txt`'s `SPAZNET_HTTP2_SOURCES` — so the
`OFF` build never even attempts to compile a translation unit that
has no non-coroutine content at all.

## Picking a runtime

Both are first-class; neither is a fallback for the other. Some
guidance for a new project:

- **Coroutine runtime** — if your toolchain has C++20 coroutines and
  you're comfortable with them, this is the more ergonomic one to
  *read*: `serve_keep_alive`, `Http2Connection`'s coroutine sibling,
  etc. read top-to-bottom like the wire protocol they implement, with
  suspension points where the protocol actually waits for I/O.
  Multi-threaded scaling is "free" — `IOContext` round-robins
  suspended frames across worker threads with no extra code.
- **Reactor runtime** — if you can't or don't want a coroutine
  dependency, or if you're debugging a hung connection and want a
  `bt` that shows real call frames instead of a suspended coroutine
  handle. Every dispatcher's state machine has a small, explicit
  phase enum and member variables you can inspect directly in a
  debugger or a core dump. Multi-threaded scaling needs (and gets, as
  of the `threading` milestone) explicit affinity — see "Threading
  and re-entrancy" below — rather than coming for free.
- **Both, side by side** — every demo (`http_hello`, `ws_echo`,
  `http2_showcase`, `udp_echo`, ...) accepts a `--reactor` flag and
  runs the same handler object under whichever dispatcher you pick.
  There's no requirement to choose one for an entire process; a
  `Server` just dispatches whatever `ConnectionFactory`/`ConnectionHandler`
  you installed.

## Authoring a reactor state machine

If you're porting a new protocol, or extending an existing reactor
dispatcher, the shape is the same across all five in this tree
(`Http1Connection`, `Http2Connection`, `WsConnection`, UDP's plain
callback, and QUIC/HTTP3's `dispatch_one`):

1. **Pick your phases.** A connection-oriented protocol gets a small
   enum tracking where it is in its own byte stream — HTTP/1.1's is
   `{ReadingRequest, Dispatching}`, WebSocket's is `{Sniffing,
   ReadingHeader, ReadingPayload}`, HTTP/2's is `{Preface, FrameHeader,
   FramePayload}`. There's deliberately no separate "Writing" or
   "Closing" phase in any of them: writing is just a
   `BufferedConnection::write()` call (fire-and-forget; the
   `OutputBuffer` serializes it), and closing is a terminal action
   your code takes, not a state you wait in.
2. **Own a `std::vector<std::uint8_t> buffer_`** (or per-field
   scratch buffers, for HTTP/2's frame header/payload) and reparse or
   incrementally consume it from `on_data()`. Whether to reparse the
   whole buffered prefix each time (cheap for HTTP/1.1's small
   request lines) or consume incrementally and remember an offset
   (necessary for WebSocket's up-to-16MiB payloads) is a
   per-protocol call — see `dispatcher_reactor.cpp` in `example/http`
   vs. `example/http-websocket` for both styles side by side.
3. **Reuse the protocol codec unchanged.** `HTTPParser`, `codec.cpp`'s
   `Frame`/`Settings`/`HPACK`/`Parser`, WebSocket's `Frame::serialize`/
   `parse`, the entire `quic::`/`http3::` transport — none of it
   knows or cares which dispatcher is calling it. If you find yourself
   wanting to change codec behavior only for the reactor side, stop:
   that's very likely to introduce exactly the kind of drift the
   differential tests exist to catch.
4. **Answer through the same handler API both runtimes share.**
   `spaznet::ResponseWriter<Response>` (`include/libspaznet/reactor/response_writer.hpp`,
   itself coroutine-free) is the completion token for HTTP/1.1 and
   HTTP/2's `handle_request(const Request&, ResponseWriter)` — a
   handler that answers inline or defers to a background thread looks
   identical to both dispatchers. WebSocket's handler shape doesn't
   map onto `ResponseWriter` (sending is itself the point, not a single
   terminal answer), so it gets a parallel, purely-synchronous
   `websocket::reactor::Handler`/`Connection` instead of reusing the
   coroutine `Handler` — see `reactor_handler.hpp`. UDP's
   `handle_packet` was already synchronous everywhere in this tree, so
   its "reactor adapter" is a direct call with no wrapping at all.
5. **Register with `Server` via a `ConnectionFactory`** (`std::function<std::shared_ptr<IoHandler>(int fd, IOContext&, std::function<void()> on_closed)>`,
   set with `Server::set_connection_factory()`) for anything
   connection-oriented, or a `SyncDatagramHandler` (`std::function<void(Datagram)>`,
   `Server::set_sync_datagram_handler()`) for datagram protocols. The
   factory builds whatever `IoHandler` drives the connection — almost
   always a `BufferedConnection` plus your state machine holding a
   `weak_ptr` back to it (see "Ownership" below) — and must arrange for
   the passed-in `on_closed` to fire exactly once.

## Connection lifetime and re-entrancy

These are the rules every reactor dispatcher in this tree follows, and
the ones a new one needs to follow too. Getting them wrong reproduces
real bugs this codebase has already hit and fixed (see `CHANGELOG.md`'s
`http2-reactor` entry for the double-free this exact set of rules was
written to prevent).

- **No reference cycle between the buffer and the state machine.**
  `Server`'s `ConnectionFactory` returns a `shared_ptr<IoHandler>` —
  conventionally a `BufferedConnection` — that both `Server` and
  `IOContext`'s fd table hold onto. Your protocol state machine
  (`Http1Connection`, etc.) is a *separate* object that holds only a
  `weak_ptr` back to that `BufferedConnection`; the strong ownership
  direction runs the other way, through `BufferedConnection::on_data`/
  `on_closed`, which capture a `shared_ptr` to your state machine. That
  asymmetry is what keeps the pair from leaking each other forever.
- **A deferred completion can outlive the connection.** A handler that
  defers (spawns a background thread, waits on another service, etc.)
  holds its own independent `shared_from_this()` copy of your state
  machine, separate from the one `BufferedConnection`'s callbacks hold.
  If the peer disconnects first, `BufferedConnection` is destroyed,
  your state machine's `weak_ptr` to it goes stale, and the eventual
  completion callback's `weak_ptr::lock()` returns `nullptr` — a
  deliberate, safe no-op instead of touching freed memory. Never store
  a raw `BufferedConnection*` or a strong `shared_ptr<BufferedConnection>`
  in anything that might run after the connection could plausibly be
  gone.
- **Bound recursion on synchronous completions.** If your handler API
  allows a handler to complete *before* the call that invoked it
  returns (the common case for both `ResponseWriter` and WebSocket's
  reactor `Connection::send`), the completion callback needs a flag
  (`dispatch_call_active_`, in this tree's dispatchers) distinguishing
  "still inside the original call, just flip state and let the caller's
  own loop continue" from "arriving asynchronously, need to kick off
  processing myself." Without it, a client pipelining many
  synchronously-answered requests on one connection grows the call
  stack by one frame per request instead of running in a loop.
- **A self-destroying callback needs to survive its own destruction.**
  A state machine can decide to tear itself down (protocol error,
  clean close) from inside a call *it is currently running on its own
  stack* — `on_readable()` calling something that ultimately decides to
  destroy the very object `on_readable()` is a method of. Hold a
  `shared_ptr` across the whole dispatch, and if you need to actually
  free something before the call returns, use `IOContext::defer_destruction(shared_ptr<void>)`
  (the reap list, drained at the end of the current loop iteration)
  rather than letting a `shared_ptr` go out of scope mid-callback.

## Threading and re-entrancy across connections

A reactor state machine's member variables are only safe to touch from
one thread — unlike a coroutine frame, which is self-contained and
genuinely thread-portable. In this codebase specifically, **exactly one
thread per `IOContext` ever calls into `on_readable()`/`on_writable()`**:
the thread that called `IOContext::run()`. Worker threads never touch
`PlatformIO` directly. That means a connection's own I/O callbacks are
single-threaded by construction with no lock needed — the risk is
entirely code that reaches into connection state from *outside* that
call chain:

- A `ResponseWriter`/`Connection::send()` completion arriving from a
  background thread the handler spawned.
- One connection's handler reaching into a *different* connection's
  state (a WebSocket chat broadcast, an HTTP/2 stream's response
  racing the same connection's frame-reading loop).

`IOContext::post_to_io_thread(std::function<void()>)` is the primitive
that makes both safe: it runs `fn` inline if the caller is already on
the IO thread (the common case — zero overhead), or queues it onto a
queue drained *only* by `run()`'s own loop otherwise. Every
`ResponseWriter` completion in `example/http` and `example/http2`, and
every `websocket::reactor::Connection::send()` call, routes through it.
A new reactor dispatcher's completion path should too — see
`docs/concurrency-and-coroutines.md`'s "Reactor Threading Model" for
the full rationale, including why this replaced a per-connection
`recursive_mutex` in HTTP/2 rather than living alongside one. That
model is one loop per `IOContext`; `Server(N)`'s worker threads do not
scale reactor I/O. Use `Server(ServerConfig{.loops = N})` for N
independent loops with accept-and-shard — see
[`reactor-threading.md`](reactor-threading.md).

## Verifying a change under both configurations

Any change that touches a shared component (the event loop, a codec,
`BufferedConnection`, a handler interface) should be verified under
both values of `SPAZNET_ENABLE_COROUTINES` before landing — CI does
this on every push, but locally:

```bash
cmake -B build     -DSPAZNET_ENABLE_COROUTINES=ON  && cmake --build build     && ctest --test-dir build     --output-on-failure
cmake -B build-off -DSPAZNET_ENABLE_COROUTINES=OFF && cmake --build build-off && ctest --test-dir build-off --output-on-failure
```

A change that touches only one dispatcher (e.g. a coroutine-only bug
fix in `serve_keep_alive`) only needs the matching configuration, but
if the fix reveals a matching bug in the reactor sibling — as happened
during the `http2-reactor` milestone — fix both and re-run the
differential test suite for that protocol.
