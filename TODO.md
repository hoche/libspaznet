# TODO

## From the 2026-05-27 audit

- [ ] Replace QUIC + HPACK with a real library
  - Current QUIC is non-interoperable plaintext (no Initial keys, no header
    protection, no AEAD, inverted long-header bit, non-RFC varint).
  - Current HPACK silently corrupts headers (no RFC 7541 varint, no Huffman,
    no dynamic table).
  - Candidates: ngtcp2 / quiche / msquic for QUIC; nghttp2 for HPACK.

- [ ] Make the WebSocket layer RFC 6455 compliant
  - Enforce client→server masking server-side; fail with 1002 if unmasked.
  - Validate control frames: payload ≤125, fin must be true, no fragmented
    Close/Ping/Pong.
  - Reject reserved opcodes and RSV1/2/3 bits unless an extension is negotiated.
  - Reject non-minimal length encoding; reject the 64-bit length with the
    high bit set.
  - Add a configurable max_payload (default ~16 MiB); remove the dead
    `payload_length > size_t::max()` guard.
  - Fix the OOM in src/server_impl.cpp:985 — peer-supplied payload_len is
    cast to size_t and used to size a vector with no bound.
  - Confirm/implement Sec-WebSocket-Accept (SHA1+base64 of key + GUID).

- [ ] Re-architect Socket::async_read / async_write
  - Stop calling synchronous recv/send inside await_ready.
  - Stop using std::this_thread::sleep_for from inside coroutines
    (read_exact spins, blocks the whole worker on the default single-thread
    config).
  - Return ssize_t from async_read (currently declared Task, result is
    silently discarded at server_impl.cpp:335).
  - Add per-connection tracking and drain pending coroutines in Server::stop()
    before the IOContext goes away.

- [ ] Fix platform I/O hazards
  - TaskQueue Michael-Scott queue has a UAF between dequeue and a racing
    enqueue (io_context.hpp:474-499).
  - Round-trip user_data through epoll's data.ptr (currently dropped at
    platform_io_epoll.cpp:36,58,99) with a generation counter so fd-reuse
    can't resume the wrong coroutine.
  - Retry epoll_wait / kevent on EINTR instead of breaking out of run().
  - Map EPOLLHUP / EV_EOF to EVENT_READ as well as EVENT_ERROR so half-close
    unblocks readers.
  - IOCP backend is broken as-shipped (no WSAStartup, zero-byte WSABUF, no
    CancelIoEx on remove_fd, leaks OverlappedContext). Gate behind #if 0 or
    rewrite.

- [ ] Sanitize CR/LF in outgoing header values
  - HTTPResponse::serialize() and HTTP2 response builders write header
    values verbatim — response splitting / header injection.

- [ ] Build hygiene
  - Add -Wall -Wextra -Wpedantic -Wconversion -Wshadow to release/debug.
  - Wire up sanitizer build configs (ASan, UBSan, TSan).
  - find_package(Threads REQUIRED) and link Threads::Threads (Linux).
  - Add WSAStartup/WSACleanup on Windows.
  - Add install(EXPORT) so downstream find_package(spaznet) works.

## Other

- [ ] Add support for TLS
  - Currently, TLS is not supported.
  - Probably use OpenSSL as an optional config.
  - Need to be able to get cert information from the connection for auth





