# Performance characteristics

What to expect from libspaznet on a typical loopback workload. Real
network performance is bounded by the network, not the library —
these numbers are the "how much CPU does the library itself eat?"
ceiling.

For tuning advice see [`threading.md`](threading.md). For raw
comparison-vs-libzenomt numbers, see the netbench repo.

## Headline numbers — meep (Linux x86_64, 32 cores, 2026-05-30)

### iperf3 ceiling (kernel TCP loopback, no library)

| streams | sent MiB/s |
|---:|---:|
| 1 | 15,612 |
| 4 | 51,261 |
| 16 | 137,856 |

These are the upper bounds. Nothing built on TCP can be faster.

### HTTP/1.1 keep-alive (bench_http)

| body size | best N | rps | resp MiB/s | CPU µs/req |
|---:|---:|---:|---:|---:|
| 0 B | 4 | 719K | 0 | 10.4 |
| 256 B | 4 | 573K | 140 | 13.9 |
| 4 KiB | 16 | 401K | 1,566 | 51.8 |
| 64 KiB | 16 | 108K | 6,727 | 181 |

At 64 KiB the loopback ceiling starts to matter: 6.7 GiB/s is ~5%
of the iperf3 16-stream number, with the rest of the path being HTTP
framing + handler dispatch + scheduler.

### WebSocket echo (bench_websocket)

| payload | best N | rps | MiB/s | CPU µs/msg |
|---:|---:|---:|---:|---:|
| 64 B | 4 | 552K | 33.7 | 13.3 |
| 1 KiB | 4 | 627K | 613 | 12.0 |
| 8 KiB | 4–8 | 341K | 2,667 | 22.1 |
| 64 KiB | 8 | 101K | 6,336 | 131 |

The 64 B case is a microbenchmark of per-frame overhead. After the
2026-05-30 stash-buffered recv (`eb3ea04`), libspaznet runs 6.7 µs
of CPU per message at `N=0` — within ~20% of libzenomt's tighter
single-threaded number on the same workload.

### Connect storm (bench_connect_storm)

| N | cps | CPU µs/conn |
|---:|---:|---:|
| 0 | 22.9K | 614 |
| 1 | 22.6K | 712 |
| 4 | 22.8K | 847 |
| 8 | 28.8K | 814 |

Pure accept/teardown rate. Throughput is nearly flat across thread
counts — accept-path contention, not framework headroom. For this
workload, **`Server(0)` is the right choice**.

## Headline numbers — Mac M1 (Apple Silicon, 14 cores, 2026-05-30)

macOS loopback is much harsher on multi-stream than Linux (the kernel
serializes single-receiver loopback), so the per-core numbers are
roughly comparable but the multi-stream ceiling is dramatically lower.

### iperf3 ceiling

| streams | sent MiB/s |
|---:|---:|
| 1 | 12,771 |
| 4 | 5,086 |
| 16 | 5,055 |

(Yes — multi-stream loopback is **slower** on macOS than single-stream
because of kernel-side serialization. Use single-stream as your
ceiling reference on Mac.)

### HTTP/1.1 keep-alive — Mac

| body size | best N | rps | resp MiB/s | CPU µs/req |
|---:|---:|---:|---:|---:|
| 0 B | 0 | 141K | 0 | 14.6 |
| 256 B | 0 | 128K | 31 | 17.7 |
| 4 KiB | 4 | 112K | 438 | 65.1 |
| 64 KiB | 4 | 52K | 3,250 | 128 |

Per-core Mac numbers are close to per-core meep numbers; the
absolute throughput cap is much lower because there are fewer cores
and the kernel doesn't parallelize loopback well.

### WebSocket echo — Mac

| payload | best N | rps | MiB/s | CPU µs/msg |
|---:|---:|---:|---:|---:|
| 64 B | 0 | 105K | 6.4 | 16.0 |
| 4 KiB | 4 | 85K | 333 | 80.8 |
| 64 KiB | 4 | ~25K | ~1,500 | ~600 |

## How to read the CPU column

`CPU µs/req` (or `µs/msg`) is total process CPU time consumed per
completed operation. The harness runs both the server and the client
in one process, so the figure is the *combined* cost. Comparing two
backends apples-to-apples requires holding the client side constant
(which netbench does — same client implementation drives both
libspaznet and libzenomt).

A first-order interpretation:

- **Lower µs/op is better** independent of how many cores you have.
  Doubling N halves wall-clock time only if the per-op CPU stays the
  same. If µs/op goes up with N, you're paying for scheduling
  overhead instead of buying parallelism.
- **µs/op is roughly invariant across body sizes for small payloads.**
  Around 4 KiB and up, memcpy dominates and µs/op grows linearly with
  size.

## Where the time goes (snapshot 2026-05-29, meep, callgrind)

At full-MTU QUIC datagrams with the post-`bcac7b4` fixes, on the
QUIC steady-state bench:

| Subsystem | Inclusive % |
|---|---:|
| AEAD (EVP_CIPHER_CTX path) | ~30% |
| Header protection (AES-ECB / ChaCha20 mask) | ~10% |
| Frame parse / build | ~12% |
| ACK processing | ~8% |
| `Stream::pull_send` (vector copy) | ~6% |
| `IOContext` scheduling | ~5% |
| Coroutine frame allocate / free | ~4% |
| Remainder | ~25% |

Full breakdown: `docs/quic-profile-2026-05-29.md`.

For the HTTP/1.1 + WebSocket paths, profiling is dominated by:

| Subsystem | Inclusive % |
|---|---:|
| Per-frame syscalls (recv/send) | ~25% on small payloads, ~10% on 64 KiB |
| Coroutine resume + `IOContext` | ~15% |
| HTTP/1.1 parse / WebSocket unmask | ~10% |
| `std::unordered_map` header storage | ~8% |
| Header serialization | ~5% |
| Remainder (memcpy, allocator, kernel transitions) | ~37% |

## Reproducing these numbers

```bash
# libspaznet's own bench (QUIC steady-state)
cd build && ./bench_quic_steady_state

# Comparative benches (libspaznet vs libzenomt vs iperf3)
cd ../netbench
cmake -B build -DCMAKE_BUILD_TYPE=Release \
               -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3.5)   # macOS
cmake --build build -j
./build/bench_iperf3   --seconds 5 --streams 1,4,16        > iperf3.md
./build/bench_http     --seconds 3                          > http.md
./build/bench_websocket --seconds 3                         > ws.md
./build/bench_connect_storm --seconds 3                     > storm.md

# libspaznet's HTTP-only sweep across thread counts
cd ../libspaznet/build && ./bench_thread_modes > thread_modes.md
```

## What affects the numbers

In order of how much they move the dial:

1. **Compiler + optimization level.** GCC 13.3 vs Clang 17 differ
   by ~5%; `-O2` vs `-O3` can be another 5–10% in either direction
   depending on which loops the optimizer chooses to unroll. CI
   builds at `-O3`.
2. **Allocator.** glibc malloc is ~5–10% slower than jemalloc /
   tcmalloc on the HTTP keep-alive workload. We don't link a custom
   allocator; downstream consumers who do see the gain.
3. **kernel version.** Linux 5.x → 6.x improved loopback TCP
   throughput materially. The 2026-05-30 meep numbers are on 6.17.
4. **Sanitizer builds.** ASan / UBSan / TSan all roughly halve
   throughput. Don't benchmark sanitizer builds.
5. **Thread count.** Sweep across `--spaznet-threads` in the
   benches; the "best N" column above is workload-specific.

## What doesn't affect the numbers (much)

- **Whether you build with `SPAZNET_BUILD_QUIC=ON` vs `OFF`.** The
  HTTP/WebSocket paths are unchanged; QUIC code links in but doesn't
  run.
- **clang-tidy / cppcheck warnings.** These don't generate code.

## Related

- [`threading.md`](threading.md) — picking `N` for your workload
- [`mutex-vs-atomics.md`](mutex-vs-atomics.md) — why the hot path is
  almost lock-free
- [`quic-profile-2026-05-29.md`](quic-profile-2026-05-29.md) —
  callgrind output snapshot, post-rewrite
- [`concurrency-and-coroutines.md`](concurrency-and-coroutines.md) —
  what `co_await` actually compiles to
