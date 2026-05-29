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
