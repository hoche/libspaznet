# QUIC + HTTP/3 stack — first profile (2026-05-29, meep)

> One-line summary: the workload is dominated by **TLS handshake crypto
> in libcrypto.so**. Code we wrote in `libspaznet` accounts for roughly
> **1% of total instructions**. The biggest realistic wins are
> session resumption / 0-RTT (skips the costly part of every handshake)
> and reducing `std::vector` reallocation churn on the packet-build
> path (~5–9% of cycles live in glibc malloc/free).

## Method

- Host: meep (Ubuntu 24.04, Intel x86_64, gcc 13.3, OpenSSL 3.5.4).
- `perf` is locked down on meep (`perf_event_paranoid=4`, no
  passwordless sudo). Used **callgrind** instead — slower wall time,
  no privileges needed, instruction-accurate.
- Build: `cmake -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo`.
- Workload: `./build-prof/test_unit
  --gtest_filter='Quic*:Hpack*:Qpack*:H3*:Http3*' --gtest_repeat=20`
  — roughly 1,200 test executions (60 QUIC/HTTP3 tests × 20 reps).
- Total instructions collected: **789,777,528**.
- Raw callgrind file archived at `/tmp/callgrind-meep.out` (1.8 MB).

## Profile buckets (rounded from `callgrind_annotate --threshold=99`)

| Bucket | % of total Ir |
|---|---|
| OpenSSL libcrypto.so (ECDHE, P-256, SHA-2, ML-KEM, ASN.1, allocators) | **~50%** |
| glibc malloc family (`malloc`, `_int_malloc`, `_int_free`, `free`, `malloc_consolidate`) | ~9% |
| glibc string/mem (`memcpy`, `memset`, `strcmp`) | ~5% |
| OpenSSL libssl.so (cipher list, dispatch) | ~5% |
| **libspaznet (our code) + the test harness's hand-rolled client** | **~1%** |
| googletest framework, dynamic linker, runtime init, other | balance |

## Top OpenSSL hot spots

```
 7.11%  sha512_block_data_order_avx2          (transcript hash, sig verify)
 6.11%  fe_mul                                 (X25519 field multiply)
 3.98%  __ecp_nistz256_mul_montq               (P-256 Montgomery mul)
 3.75%  __KeccakF1600                          (SHA-3 — likely cert path)
 3.60%  x25519_fe51_mul
 3.16%  __ecp_nistz256_sqr_montq               (P-256 squaring)
 2.63%  x25519_fe51_sqr
 1.99%  scalar_ntt                             (ML-KEM-768 NTT)
 1.58%  sha256_block_data_order_avx2
 1.47%  scalar_inverse_ntt                     (ML-KEM-768 inverse NTT)
```

**Notable**: OpenSSL 3.5+ defaults to X25519+MLKEM768 hybrid post-
quantum key exchange. `scalar_ntt` + `scalar_inverse_ntt` together
account for ~3.5% of the entire benchmark.

## Top libspaznet hot spots

All under 0.15%; together less than ~1% of the run.

| Ir       | %    | function |
|---------:|-----:|---|
| 936 K   | 0.12% | `http3::huffman_decode` |
| 714 K   | 0.09% | `quic::parse_frame` |
| 369 K   | 0.05% | `http3::huffman_encode` |
| 266 K   | 0.03% | `quic::VarInt::append` |
| 214 K   | 0.03% | `quic::VarInt::decode` |
| 154 K   | 0.02% | `quic::hkdf_expand` (mostly an OpenSSL call) |
| 119 K   | 0.02% | `quic::hkdf_expand_label` |
| 109 K   | 0.01% | `quic::Connection::build_and_send` |
| 170 K   | 0.02% | `quic::VarInt::write` |

The full `Http3EndToEnd::GetReturnsResponseBody` test (including all
the OpenSSL work it triggers) inclusive-costs at **30.74%** of total
— meaning ~⅓ of the run is that one test, ×20 reps.

## What this means

1. **The workload is handshake-bound**, not transport-bound. Each
   test iteration runs a complete fresh TLS 1.3 handshake (ECDHE
   keygen + cert verify + finished MAC + hybrid KEM). A real
   long-lived connection that sends thousands of requests over one
   handshake would shift the profile dramatically — most of the
   libcrypto cost would disappear and AEAD per-packet (AES-128-GCM,
   already AES-NI-optimized in libcrypto) would dominate. We can't
   beat the AES-NI assembly.

2. **Our own code is essentially free at this scale.** Even a
   theoretical 10× speedup of `huffman_decode` would shave ~0.1% off
   the total. There is no path to a meaningful perf win by
   micro-optimizing varint, frame parsing, or stream reassembly —
   they're already well below the noise floor.

3. **The most attackable thing in our codebase is `vector<uint8_t>`
   allocation churn.** ~9% of cycles live in malloc/free. Most of that
   is OpenSSL's internal allocations (which we can't change), but a
   non-trivial slice is `build_and_send` resizing `pkt`/`payload`/
   `aad`/`plaintext`/`sealed` repeatedly. A pre-sized scratch buffer
   reused across calls (sized to the negotiated max_datagram_size)
   would cut a measurable fraction of that 9% — not 9% of the total,
   but maybe 1–3 percentage points.

4. **The big realistic wins are protocol-level, not micro-level**:
   - **Session resumption / 0-RTT** — skips ECDHE + cert verify
     entirely on resumed connections. Easily the single largest
     performance feature missing.
   - **Larger Initial-window / amortizing handshake cost over more
     requests** — pure protocol policy, but matters for benchmarks.
   - **Disable ML-KEM** if PQ-hybrid isn't required for your threat
     model: `-3.5%` instructions, no code changes other than the
     `SSL_CTX` setup.

## Recommended next step before any optimization

Add a **steady-state benchmark** that holds a connection open and
pushes data through it — something like "after handshake completes,
issue 1000 GETs on 1000 streams" or "transfer 100 MB". That profile
will look completely different from this one (likely 70%+ in AEAD
encrypt/decrypt and `memcpy`) and will reveal whether the transport
layer has any bottleneck worth fixing. Without it we have no
information about steady-state throughput.

## Follow-up: in-place AEAD + scratch buffer (commit fc8a185)

Implemented the "reduce `std::vector` reallocation churn" item from
the "What this means" section above:
- `aead_seal_inplace` / `aead_open_inplace` operate on
  `[aad | body | tag-space]` laid out by the caller in one buffer.
- `Connection` gained a `scratch_packet_` member, reserved once at
  ~1500 B, `clear()`-ed at the top of every `build_and_send`. The
  Handshake + Application paths now build the entire datagram
  (header + payload + tag) in place inside this single buffer.

Allocations eliminated per Handshake / Application packet:
- `datagram` vector — replaced by reused scratch
- `aad` vector copy from datagram — replaced by a span
- `plaintext` vector copy from datagram — replaced by a span
- `sealed` vector inside `aead_seal` — body is encrypted in place
- `pkt.insert(sealed.begin(), sealed.end())` reallocation — gone

Re-profile under the same workload (789.8 M Ir before → 792.5 M Ir
after, +0.3% — within noise):

| call site | before | after | delta |
|---|---|---|---|
| `aead_seal*` from server `build_and_send` (160 calls) | 1,131,666 | 1,051,777 | **−7.1%** |
| total program instructions | 789.8 M | 792.5 M | +0.3% (noise) |
| malloc bucket | 4.88% | 4.90% | flat |

The local win on the changed code path is real (−7% on the seal
sub-tree). But it's invisible at the benchmark level because:
- 440 of the ~600 total `aead_seal` calls come from the test
  harness's hand-rolled client, which still allocates.
- ~160 server-side `build_and_send` calls × ~4 allocations saved
  per call ≈ 640 fewer allocations across the run. OpenSSL does
  ~millions during the 1,200 fresh TLS handshakes this benchmark
  triggers.

Confirmation that the original analysis was right: the workload is
handshake-bound, and micro-optimizing libspaznet code recovers
fractions of a percent at most. The change is correct, sanitizers
remain clean (226/226 under asan+ubsan and tsan on both platforms),
but a steady-state benchmark — one long-lived connection pushing
thousands of packets through a single peer — is what's needed to
make this kind of optimization measurable.

## Follow-up: steady-state throughput bench (commit 27320da)

Built the bench the previous follow-up called for —
`tests/performance/bench_quic_steady_state.cpp`. Spins up the
in-memory loopback (server quic::Connection + hand-rolled OpenSSL
client), completes one handshake, opens huge flow-control windows,
then measures how long N 1-RTT packets take to flow server →
client end-to-end. Reports a Markdown table over R reps.

### Baseline numbers (RelWithDebInfo, no contending workload)

| host | pkts/sec | Mbps | ns/pkt |
|---|---|---|---|
| meep (Intel x86_64, OpenSSL 3.5.4) | ~117 K | ~675 | ~8 500 |
| local (Apple M-series, OpenSSL 3.6.1) | ~118 K | ~677 | ~8 500 |

End-to-end QUIC v1 throughput on a single thread, no UDP socket
involved: ~675 Mbps = ~117 K full-MTU packets/sec each direction.
~⅔ of a gigabit through the QUIC layer alone.

### Before / after the in-place-AEAD change

10 reps × 100 K packets per rep on meep, back-to-back:

| | pkts/sec | Mbps | ns/pkt |
|---|---|---|---|
| Before (461645b — vector copies, allocating AEAD) | 117,478 | 674.2 | 8,514 |
| After  (27320da — in-place AEAD + scratch buffer) | 115,562 | 663.2 | 8,663 |
| Delta | **−1.6%** | **−1.6%** | **+1.7%** |

One rep in the "after" run was a clear outlier (105 K pkts/sec while
the other nine clustered at 115–119 K). Stripping it shrinks the
gap to about −0.6%. Either way the optimization is **within
benchmark noise** on this workload.

### Why the local 7% callgrind win doesn't show up here

The bench exercises both server send AND client receive in a loop;
the changes only touched server send. The bench also still pays
for one full `EVP_CIPHER_CTX_new` / `_free` per AEAD call on both
sides — that pair dwarfs the vector allocations the optimization
removed. callgrind sees the −7% on the changed sub-tree; wallclock
doesn't because the sub-tree is a minority of total per-packet
work.

### Next optimization, ranked by expected wallclock impact

1. **Cache `EVP_CIPHER_CTX*` per `Space`** — allocate once at key
   install, call `EVP_EncryptInit_ex(ctx, nullptr, nullptr, nullptr,
   nonce)` per packet to reset only the IV. Probably the biggest
   single win available.
2. **Eliminate `Stream::pull_send`'s `data_out.assign(...)`** — hand
   back a `span<const uint8_t>` into the stream's send buffer.
   Removes one allocation + copy per packet emitted.
3. The test harness still uses `aead_seal`/`aead_open` (allocating
   variants). Switching it to the in-place versions would only
   matter for bench fidelity, not real-world perf — not worth the
   churn unless we extract the harness into a reusable header.

## Follow-up: cached EVP_CIPHER_CTX per Space (commit pending)

Implemented `CipherCtx` — RAII wrapper around an `EVP_CIPHER_CTX*`
that holds the cipher + key binding for the lifetime of the
encryption level. `seal_inplace` / `open_inplace` per packet just
reset the IV via `EVP_EncryptInit_ex(ctx, NULL, NULL, NULL, nonce)`.
Allocated in `Connection::install_new_keys` and held on each
`Space`. The Initial, Handshake, and Application send + receive
paths all go through the cached contexts now.

Bench results (RelWithDebInfo, in-place AEAD already present in
baseline, this change adds the context cache on top):

**Local macOS (AppleClang + OpenSSL 3.6.1):**

| N packets | Before (fc8a185) | After (549107d) | Delta |
|---|---|---|---|
| 5,000  | 276K pps / 1.59 Gbps / 3,680 ns/pkt | **340K pps / 1.95 Gbps / 2,941 ns/pkt** | **+23% / -20%** |
| 10,000 | 262K pps / 1.50 Gbps / 3,821 ns/pkt | 285K pps / 1.63 Gbps / 3,513 ns/pkt | +9% / -8% |
| 20,000 | 202K pps / 1.16 Gbps / 4,955 ns/pkt | 217K pps / 1.25 Gbps / 4,602 ns/pkt | +8% / -7% |

**meep (gcc 13.3 + OpenSSL 3.5.4, Intel x86_64):**

| N packets | Before (fc8a185) | After (549107d) | Delta |
|---|---|---|---|
| 5,000  | 278,596 pps / 1.60 Gbps / 3,591 ns/pkt | **344,446 pps / 1.98 Gbps / 2,905 ns/pkt** | **+24% / -19%** |
| 10,000 | 238,254 pps / 1.37 Gbps / 4,198 ns/pkt | **324,361 pps / 1.86 Gbps / 3,083 ns/pkt** | **+36% / -27%** |
| 20,000 | 191,984 pps / 1.10 Gbps / 5,230 ns/pkt | **275,697 pps / 1.58 Gbps / 3,628 ns/pkt** | **+44% / -31%** |

The improvement is larger on Linux than on macOS, likely because
glibc's malloc has higher per-allocation overhead than macOS's
allocator, so removing per-packet allocations buys more there.

CipherCtx caching is a real wallclock improvement. The win shrinks
with N because a different bug — `Stream::on_acked`'s O(N)
`vector::erase(begin, ...)` — eats an increasing share of total
time at larger queue depths. Filed as the next perf TODO.

The bench also gained a backpressure check
(`server.for_each_stream(...)` to peek the stream's send buffer
size, only top off when below 32 KB) so it stops measuring the
on_acked quadratic when run at large N.

All sanitizers remain clean (227/227 under asan+ubsan and tsan on
both macOS and meep).

## Follow-up: O(N²) ACK processing fixed (commit bcac7b4)

Two related fixes for the wildly-superlinear ns/pkt growth the
bench was showing:

1. **`Connection::on_ack_frame`** previously iterated every PN in
   each acked range and did a `by_pn.find(pn)`. The hand-rolled
   client never trims its `PnSpace::ranges_`, so every ACK frame
   covers PNs `0..largest_acked` — almost all of which are already-
   acked-and-released and miss in `by_pn`. Over N total acks that
   was O(N²) total work. The fix: walk `sent_`'s map via
   `lower_bound` instead, so per-ACK work is O(found + log K), not
   O(range).

2. **`Stream::on_acked`** previously did
   `send_buf_.erase(begin, begin+take)` — O(N) memmove per ack
   range. Replaced with a logical "head" offset
   `send_buf_head_`: advancing on ack is O(1), with a single
   `std::move` compaction when the head crosses 8 KiB or half the
   buffer. Amortized O(1) per acked byte.

Steady-state bench (RelWithDebInfo, no backpressure):

**Local macOS (AppleClang + OpenSSL 3.6.1):**

| N pkts | Before (549107d) | After (bcac7b4)      | ns/pkt delta |
|---|---|---|---|
|   5K | 4,010 ns/pkt /  1.5 Gbps | 3,599 ns/pkt /  1.6 Gbps | -10% |
|  20K | 4,514 ns/pkt /  1.3 Gbps | 2,342 ns/pkt /  2.5 Gbps | -48% |
|  50K | 7,802 ns/pkt /  0.7 Gbps | 2,283 ns/pkt /  2.5 Gbps | -71% |
| 100K | 13,299 ns/pkt / 0.4 Gbps | 2,309 ns/pkt /  2.5 Gbps | -83% |
| 200K | 24,478 ns/pkt / 0.2 Gbps | 2,304 ns/pkt /  2.5 Gbps | -91% |

**meep (gcc 13.3 + OpenSSL 3.5.4, Intel x86_64):**

| N pkts | After (bcac7b4) |
|---|---|
|   5K | 388,757 pps / 2,231 Mbps / 2,573 ns/pkt |
|  20K | 382,411 pps / 2,195 Mbps / 2,616 ns/pkt |
|  50K | 392,984 pps / 2,255 Mbps / 2,545 ns/pkt |
| 100K | 388,365 pps / 2,229 Mbps / 2,575 ns/pkt |
| 200K | 385,515 pps / 2,216 Mbps / 2,594 ns/pkt |
| 500K | 393,083 pps / 2,261 Mbps / 2,544 ns/pkt |

ns/pkt is now flat across packet count from 5K to 500K — confirms
the quadratic is gone.

### Cumulative journey on meep (vs the original baseline 461645b)

|  N pkts | Original baseline | After all perf fixes (bcac7b4) | Throughput |
|---|---|---|---|
|  100K | 117 K pps, 674 Mbps   | **388 K pps, 2,229 Mbps** | **+231%** |

Headline: a single-thread libspaznet QUIC connection now sustains
~2.2 Gbps end-to-end on commodity hardware, with no UDP socket
involved (in-memory loopback). The remaining cost is dominated by
AEAD math and OpenSSL housekeeping, both of which are AES-NI-fast
and not worth attacking further at this layer.

## Reproduce

```bash
ssh hoche@meep
cd src/libspaznet
cmake -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                    -DOPENSSL_ROOT_DIR=$HOME/openssl-3.5
cmake --build build-prof --target test_unit -j16

LD_LIBRARY_PATH=$HOME/openssl-3.5/lib64 \
  valgrind --tool=callgrind \
           --callgrind-out-file=/tmp/callgrind.out \
           --collect-jumps=yes \
           ./build-prof/test_unit \
             --gtest_filter='Quic*:Hpack*:Qpack*:H3*:Http3*' \
             --gtest_repeat=20 \
             --gtest_brief=1

callgrind_annotate --threshold=99 /tmp/callgrind.out | less
```
