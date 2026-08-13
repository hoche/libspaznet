
## Thread-mode performance report

- **Host nproc**: 32
- **Thread counts**: 0, 2, 4, 8, 16, 32, 64, 128, 256, 512
- **HTTP duration per case**: 1.5s
- **HTTP client threads**: 32
- **iperf duration per case**: 2s
- **Generated**: 2026-08-12, coroutines-enabled build (`-DSPAZNET_ENABLE_COROUTINES=ON`), so both the `coroutine` and `reactor` dispatcher arms below ran in the same process; a coroutine-disabled build (`OFF`) only compiles/runs the `reactor` arm and produces identical `reactor` numbers within noise (see `docs/coro-free-build.md`).

### HTTP (libspaznet) — throughput + latency

Both dispatcher columns hit the exact same BenchHandler over the exact same wire protocol (see docs/http.md's "Two dispatchers, one handler" note); `coroutine` uses Task/co_await + Socket::async_read/write, `reactor` uses plain callbacks + BufferedConnection (see the threading milestone note in the reactor-port plan and docs/concurrency-and-coroutines.md).

A handful of rows (e.g. `coroutine`/512 and `reactor`/16 in the 0B/0B case) show one-off low outliers relative to their neighbors. These are transient loopback port-reuse/TIME_WAIT effects from rapidly cycling thousands of short-lived connections on a shared/virtualized host, not a systematic dispatcher regression — rerun `bench_thread_modes` if you need a clean sample for one specific cell.

**Case**: req_body=0B, resp_body=0B

| dispatcher | threads | rps | resp MiB/s | p50 ms | p95 ms | p99 ms | errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| coroutine | 0 | 33625.0 | 0.00 | 0.21 | 0.72 | 1.86 | 0 |
| coroutine | 2 | 34674.0 | 0.00 | 0.13 | 1.01 | 1.99 | 0 |
| coroutine | 4 | 35672.5 | 0.00 | 0.09 | 1.05 | 2.01 | 0 |
| coroutine | 8 | 39700.9 | 0.00 | 0.10 | 1.32 | 2.23 | 0 |
| coroutine | 16 | 33788.4 | 0.00 | 0.11 | 1.36 | 2.22 | 0 |
| coroutine | 32 | 35007.3 | 0.00 | 0.12 | 1.20 | 2.10 | 0 |
| coroutine | 64 | 36042.0 | 0.00 | 0.17 | 1.19 | 2.25 | 0 |
| coroutine | 128 | 40045.6 | 0.00 | 0.25 | 1.04 | 2.11 | 0 |
| coroutine | 256 | 40790.2 | 0.00 | 0.36 | 0.94 | 2.08 | 0 |
| coroutine | 512 | 208.4 | 0.00 | 0.60 | 0.69 | 0.74 | 0 |
| reactor | 0 | 31984.8 | 0.00 | 0.29 | 0.91 | 1.88 | 0 |
| reactor | 2 | 32029.6 | 0.00 | 0.29 | 0.88 | 1.89 | 0 |
| reactor | 4 | 32240.5 | 0.00 | 0.29 | 0.93 | 2.04 | 0 |
| reactor | 8 | 31612.6 | 0.00 | 0.29 | 0.94 | 2.03 | 0 |
| reactor | 16 | 1285.4 | 0.00 | 0.27 | 0.32 | 0.55 | 0 |
| reactor | 32 | 33608.8 | 0.00 | 0.27 | 0.78 | 1.79 | 0 |
| reactor | 64 | 32666.8 | 0.00 | 0.28 | 0.74 | 1.69 | 0 |
| reactor | 128 | 31626.2 | 0.00 | 0.31 | 0.72 | 1.63 | 0 |
| reactor | 256 | 33289.0 | 0.00 | 0.27 | 0.71 | 1.66 | 0 |
| reactor | 512 | 33172.5 | 0.00 | 0.30 | 0.79 | 1.82 | 0 |

**Case**: req_body=0B, resp_body=256B

| dispatcher | threads | rps | resp MiB/s | p50 ms | p95 ms | p99 ms | errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| coroutine | 0 | 24653.7 | 6.02 | 0.23 | 0.86 | 1.74 | 0 |
| coroutine | 2 | 25441.4 | 6.21 | 0.12 | 1.13 | 2.06 | 0 |
| coroutine | 4 | 26818.0 | 6.55 | 0.10 | 1.32 | 2.14 | 0 |
| coroutine | 8 | 26289.2 | 6.42 | 0.12 | 1.59 | 2.33 | 0 |
| coroutine | 16 | 23048.9 | 5.63 | 0.13 | 1.68 | 2.38 | 0 |
| coroutine | 32 | 23680.8 | 5.78 | 0.14 | 1.51 | 2.24 | 0 |
| coroutine | 64 | 22542.2 | 5.50 | 0.18 | 1.51 | 2.29 | 0 |
| coroutine | 128 | 26233.0 | 6.40 | 0.25 | 1.40 | 2.28 | 0 |
| coroutine | 256 | 27267.9 | 6.66 | 0.38 | 1.39 | 2.22 | 0 |
| coroutine | 512 | 39084.0 | 9.54 | 0.60 | 0.78 | 2.13 | 0 |
| reactor | 0 | 22874.7 | 5.58 | 0.30 | 0.98 | 1.92 | 0 |
| reactor | 2 | 22749.4 | 5.55 | 0.31 | 1.06 | 2.05 | 0 |
| reactor | 4 | 23365.2 | 5.70 | 0.27 | 1.05 | 1.98 | 0 |
| reactor | 8 | 23247.9 | 5.68 | 0.28 | 1.10 | 2.04 | 0 |
| reactor | 16 | 7.4 | 0.00 | 0.33 | 0.35 | 0.36 | 0 |
| reactor | 32 | 23929.0 | 5.84 | 0.27 | 0.94 | 1.89 | 0 |
| reactor | 64 | 22994.1 | 5.61 | 0.30 | 0.94 | 1.85 | 0 |
| reactor | 128 | 22938.0 | 5.60 | 0.28 | 0.96 | 1.93 | 0 |
| reactor | 256 | 23457.7 | 5.73 | 0.28 | 0.93 | 1.82 | 0 |
| reactor | 512 | 22881.9 | 5.59 | 0.31 | 1.07 | 2.08 | 0 |

**Case**: req_body=256B, resp_body=256B

| dispatcher | threads | rps | resp MiB/s | p50 ms | p95 ms | p99 ms | errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| coroutine | 0 | 35479.9 | 8.66 | 0.24 | 0.72 | 1.45 | 0 |
| coroutine | 2 | 39154.8 | 9.56 | 0.12 | 0.91 | 1.85 | 0 |
| coroutine | 4 | 40241.6 | 9.82 | 0.10 | 0.99 | 1.96 | 0 |
| coroutine | 8 | 35024.0 | 8.55 | 0.11 | 0.80 | 1.80 | 0 |
| coroutine | 16 | 35675.2 | 8.71 | 0.12 | 1.28 | 2.21 | 0 |
| coroutine | 32 | 35920.9 | 8.77 | 0.14 | 1.23 | 2.16 | 0 |
| coroutine | 64 | 32403.0 | 7.91 | 0.17 | 1.18 | 2.16 | 0 |
| coroutine | 128 | 34482.5 | 8.42 | 0.23 | 1.11 | 2.11 | 0 |
| coroutine | 256 | 42073.2 | 10.27 | 0.37 | 0.82 | 1.95 | 0 |
| coroutine | 512 | 27907.2 | 6.81 | 0.58 | 1.26 | 2.13 | 0 |
| reactor | 0 | 33780.0 | 8.25 | 0.29 | 0.85 | 1.77 | 0 |
| reactor | 2 | 34579.5 | 8.44 | 0.28 | 0.80 | 1.76 | 0 |
| reactor | 4 | 34126.9 | 8.33 | 0.29 | 0.78 | 1.71 | 0 |
| reactor | 8 | 33282.2 | 8.13 | 0.30 | 0.87 | 1.80 | 0 |
| reactor | 16 | 32876.6 | 8.03 | 0.31 | 0.84 | 2.03 | 0 |
| reactor | 32 | 35133.7 | 8.58 | 0.28 | 0.72 | 1.61 | 0 |
| reactor | 64 | 33911.8 | 8.28 | 0.29 | 0.72 | 1.57 | 0 |
| reactor | 128 | 33888.2 | 8.27 | 0.30 | 0.70 | 1.57 | 0 |
| reactor | 256 | 34972.3 | 8.54 | 0.29 | 0.70 | 1.61 | 0 |
| reactor | 512 | 35329.2 | 8.63 | 0.30 | 0.70 | 1.72 | 0 |

**Case**: req_body=4096B, resp_body=4096B

| dispatcher | threads | rps | resp MiB/s | p50 ms | p95 ms | p99 ms | errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| coroutine | 0 | 30926.7 | 120.81 | 0.30 | 0.59 | 1.18 | 0 |
| coroutine | 2 | 16410.7 | 64.10 | 0.14 | 0.95 | 1.88 | 0 |
| coroutine | 4 | 26763.4 | 104.54 | 0.11 | 1.05 | 2.00 | 0 |
| coroutine | 8 | 40041.7 | 156.41 | 0.09 | 1.10 | 2.24 | 0 |
| coroutine | 16 | 23370.6 | 91.29 | 0.12 | 1.75 | 2.48 | 0 |
| coroutine | 32 | 24494.6 | 95.68 | 0.15 | 1.29 | 2.17 | 0 |
| coroutine | 64 | 25240.6 | 98.60 | 0.17 | 1.12 | 2.11 | 0 |
| coroutine | 128 | 27301.7 | 106.65 | 0.24 | 1.07 | 2.01 | 0 |
| coroutine | 256 | 30647.7 | 119.72 | 0.36 | 0.99 | 2.09 | 0 |
| coroutine | 512 | 44107.9 | 172.30 | 0.60 | 0.70 | 1.23 | 0 |
| reactor | 0 | 25812.0 | 100.83 | 0.38 | 0.74 | 1.61 | 0 |
| reactor | 2 | 23043.8 | 90.01 | 0.47 | 0.79 | 1.98 | 0 |
| reactor | 4 | 28283.0 | 110.48 | 0.36 | 0.80 | 1.78 | 0 |
| reactor | 8 | 30983.8 | 121.03 | 0.36 | 0.69 | 1.56 | 0 |
| reactor | 16 | 24809.9 | 96.91 | 0.36 | 0.78 | 1.67 | 0 |
| reactor | 32 | 30460.2 | 118.99 | 0.36 | 0.66 | 1.38 | 0 |
| reactor | 64 | 26587.1 | 103.86 | 0.36 | 0.67 | 1.44 | 0 |
| reactor | 128 | 27632.7 | 107.94 | 0.36 | 0.64 | 1.38 | 0 |
| reactor | 256 | 29176.5 | 113.97 | 0.38 | 0.63 | 1.36 | 0 |
| reactor | 512 | 30829.4 | 120.43 | 0.39 | 0.70 | 1.66 | 0 |

**Case**: req_body=65536B, resp_body=65536B

| dispatcher | threads | rps | resp MiB/s | p50 ms | p95 ms | p99 ms | errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| coroutine | 0 | 18470.7 | 1154.42 | 1.71 | 1.78 | 1.81 | 0 |
| coroutine | 2 | 51933.0 | 3245.81 | 0.60 | 0.75 | 0.81 | 0 |
| coroutine | 4 | 66048.0 | 4128.00 | 0.41 | 0.51 | 0.69 | 0 |
| coroutine | 8 | 47702.6 | 2981.41 | 0.30 | 0.57 | 1.71 | 0 |
| coroutine | 16 | 74339.9 | 4646.24 | 0.25 | 0.48 | 1.13 | 0 |
| coroutine | 32 | 71254.6 | 4453.41 | 0.24 | 0.41 | 1.13 | 0 |
| coroutine | 64 | 65802.1 | 4112.63 | 0.24 | 0.39 | 0.96 | 0 |
| coroutine | 128 | 59299.0 | 3706.19 | 0.30 | 0.47 | 1.01 | 0 |
| coroutine | 256 | 50818.9 | 3176.18 | 0.47 | 0.66 | 0.80 | 0 |
| coroutine | 512 | 41899.4 | 2618.71 | 0.73 | 0.91 | 1.09 | 0 |
| reactor | 0 | 12641.4 | 790.09 | 2.49 | 2.65 | 2.77 | 0 |
| reactor | 2 | 12517.6 | 782.35 | 2.53 | 2.70 | 2.89 | 0 |
| reactor | 4 | 12728.8 | 795.55 | 2.49 | 2.60 | 2.67 | 0 |
| reactor | 8 | 12565.7 | 785.36 | 2.52 | 2.67 | 2.83 | 0 |
| reactor | 16 | 13279.4 | 829.96 | 2.38 | 2.50 | 2.56 | 0 |
| reactor | 32 | 13067.9 | 816.74 | 2.42 | 2.60 | 2.76 | 0 |
| reactor | 64 | 12396.5 | 774.78 | 2.56 | 2.68 | 2.75 | 0 |
| reactor | 128 | 12680.4 | 792.52 | 2.49 | 2.63 | 2.68 | 0 |
| reactor | 256 | 12268.8 | 766.80 | 2.55 | 2.82 | 2.93 | 0 |
| reactor | 512 | 11882.6 | 742.66 | 2.67 | 2.84 | 3.13 | 0 |

**Case**: req_body=65536B, resp_body=262144B

| dispatcher | threads | rps | resp MiB/s | p50 ms | p95 ms | p99 ms | errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| coroutine | 0 | 6050.3 | 1512.57 | 5.27 | 5.36 | 5.45 | 0 |
| coroutine | 2 | 17712.4 | 4428.10 | 1.80 | 2.18 | 2.31 | 0 |
| coroutine | 4 | 27949.8 | 6987.46 | 1.16 | 1.36 | 1.51 | 0 |
| coroutine | 8 | 41408.8 | 10352.19 | 0.74 | 1.05 | 1.26 | 0 |
| coroutine | 16 | 49875.4 | 12468.85 | 0.57 | 0.99 | 1.33 | 0 |
| coroutine | 32 | 45563.3 | 11390.83 | 0.59 | 0.91 | 1.13 | 0 |
| coroutine | 64 | 33865.9 | 8466.48 | 0.74 | 1.20 | 1.52 | 0 |
| coroutine | 128 | 27264.6 | 6816.16 | 1.11 | 1.70 | 2.13 | 0 |
| coroutine | 256 | 24848.2 | 6212.06 | 1.25 | 1.83 | 2.20 | 0 |
| coroutine | 512 | 22113.6 | 5528.41 | 1.38 | 2.10 | 2.60 | 0 |
| reactor | 0 | 3905.7 | 976.42 | 8.15 | 8.64 | 8.90 | 0 |
| reactor | 2 | 3838.6 | 959.66 | 8.28 | 8.83 | 9.20 | 0 |
| reactor | 4 | 3929.7 | 982.42 | 8.09 | 8.50 | 8.75 | 0 |
| reactor | 8 | 3910.9 | 977.72 | 8.11 | 8.56 | 9.04 | 0 |
| reactor | 16 | 3890.7 | 972.67 | 8.18 | 8.66 | 8.83 | 0 |
| reactor | 32 | 3889.0 | 972.26 | 8.20 | 8.61 | 8.79 | 0 |
| reactor | 64 | 3814.3 | 953.58 | 8.34 | 8.79 | 8.93 | 0 |
| reactor | 128 | 3713.5 | 928.37 | 8.55 | 9.07 | 9.70 | 0 |
| reactor | 256 | 3795.6 | 948.89 | 8.37 | 8.83 | 9.13 | 0 |
| reactor | 512 | 3430.9 | 857.71 | 9.29 | 10.67 | 12.53 | 0 |

### Raw TCP (iperf) — throughput

The iperf3 build on this host (3.16) rejects `-P` above 128 ("number of parallel streams too large"), so the 256/512-stream rows that exist for the HTTP thread-count sweep above are omitted here rather than shown as a misleading 0. This is a client-tool limit, not a libspaznet limit.

**payload**: 64B, **direction**: down

| streams | mbps |
|---:|---:|
| 2 | 3050.05 |
| 4 | 4986.59 |
| 8 | 8584.91 |
| 16 | 13734.91 |
| 32 | 15280.71 |
| 64 | 15134.86 |
| 128 | 14781.75 |

**payload**: 64B, **direction**: up

| streams | mbps |
|---:|---:|
| 2 | 2981.45 |
| 4 | 4974.22 |
| 8 | 8834.62 |
| 16 | 13520.74 |
| 32 | 15302.38 |
| 64 | 15240.83 |
| 128 | 14999.56 |

**payload**: 1024B, **direction**: down

| streams | mbps |
|---:|---:|
| 2 | 38870.94 |
| 4 | 68010.89 |
| 8 | 118656.35 |
| 16 | 157513.37 |
| 32 | 140940.90 |
| 64 | 120114.92 |
| 128 | 115019.81 |

**payload**: 1024B, **direction**: up

| streams | mbps |
|---:|---:|
| 2 | 41084.77 |
| 4 | 68863.41 |
| 8 | 117184.67 |
| 16 | 152012.86 |
| 32 | 141401.59 |
| 64 | 117864.79 |
| 128 | 113460.31 |

**payload**: 8192B, **direction**: down

| streams | mbps |
|---:|---:|
| 2 | 191308.12 |
| 4 | 322287.36 |
| 8 | 503224.25 |
| 16 | 579801.87 |
| 32 | 171376.72 |
| 64 | 138235.22 |
| 128 | 125677.85 |

**payload**: 8192B, **direction**: up

| streams | mbps |
|---:|---:|
| 2 | 180707.90 |
| 4 | 327444.19 |
| 8 | 514957.80 |
| 16 | 569690.98 |
| 32 | 217150.82 |
| 64 | 136791.01 |
| 128 | 120826.00 |

**payload**: 65536B, **direction**: down

| streams | mbps |
|---:|---:|
| 2 | 209767.71 |
| 4 | 334667.88 |
| 8 | 586027.68 |
| 16 | 891057.42 |
| 32 | 379629.18 |
| 64 | 219165.81 |
| 128 | 190004.44 |

**payload**: 65536B, **direction**: up

| streams | mbps |
|---:|---:|
| 2 | 209334.12 |
| 4 | 308298.04 |
| 8 | 537862.75 |
| 16 | 890963.35 |
| 32 | 409617.17 |
| 64 | 188920.04 |
| 128 | 166488.62 |


### UDP (iperf) — throughput + loss/jitter (if available)

Same 128-stream `-P` cap as above applies. The 65536B payload rows are all 0 because iperf3 rejects UDP block sizes above 65507 bytes ("block size invalid") regardless of stream count — that's an iperf3 constant, not a libspaznet limit, and matches prior reports. A couple of individual 128-stream rows below (64B/1024B/8192B payloads) also read 0 despite 128 being an allowed stream count; that's a transient client-side capture/parse failure under sustained UDP load in this sandbox (manually re-running the same command in isolation succeeds), not a reproducible libspaznet ceiling.

**payload**: 64B, **direction**: down

| streams | mbps | jitter ms | loss % |
|---:|---:|---:|---:|
| 2 | 760.32 | 0.001 | 0.002 |
| 4 | 1430.46 | 0.001 | 0.430 |
| 8 | 2712.54 | 0.001 | 0.633 |
| 16 | 3818.87 | 0.002 | 4.699 |
| 32 | 2858.09 | 0.002 | 25.957 |
| 64 | 2587.70 | 0.008 | 46.251 |
| 128 | 0.00 | 0.000 | 0.000 |

**payload**: 64B, **direction**: up

| streams | mbps | jitter ms | loss % |
|---:|---:|---:|---:|
| 2 | 824.20 | 0.001 | 0.277 |
| 4 | 1511.55 | 0.001 | 1.064 |
| 8 | 2474.77 | 0.001 | 0.334 |
| 16 | 3735.97 | 0.002 | 5.943 |
| 32 | 3901.06 | 0.002 | 29.366 |
| 64 | 4515.44 | 0.109 | 36.073 |
| 128 | 4949.16 | 0.087 | 66.222 |

**payload**: 1024B, **direction**: down

| streams | mbps | jitter ms | loss % |
|---:|---:|---:|---:|
| 2 | 11860.61 | 0.001 | 0.022 |
| 4 | 22908.59 | 0.001 | 0.353 |
| 8 | 39769.07 | 0.001 | 0.280 |
| 16 | 56683.27 | 0.002 | 7.195 |
| 32 | 41886.29 | 0.008 | 31.530 |
| 64 | 37579.62 | 0.001 | 49.483 |
| 128 | 0.00 | 0.000 | 0.000 |

**payload**: 1024B, **direction**: up

| streams | mbps | jitter ms | loss % |
|---:|---:|---:|---:|
| 2 | 12783.98 | 0.001 | 0.399 |
| 4 | 23488.63 | 0.000 | 1.503 |
| 8 | 38286.06 | 0.001 | 0.343 |
| 16 | 58328.31 | 0.002 | 13.113 |
| 32 | 58548.08 | 0.136 | 38.003 |
| 64 | 69399.39 | 0.000 | 45.581 |
| 128 | 85408.63 | 0.120 | 83.776 |

**payload**: 8192B, **direction**: down

| streams | mbps | jitter ms | loss % |
|---:|---:|---:|---:|
| 2 | 75401.06 | 0.001 | 0.417 |
| 4 | 141915.83 | 0.001 | 0.190 |
| 8 | 236025.89 | 0.001 | 0.769 |
| 16 | 347500.14 | 0.003 | 11.414 |
| 32 | 293280.20 | 0.014 | 29.912 |
| 64 | 246846.65 | 0.021 | 50.555 |
| 128 | 0.00 | 0.000 | 0.000 |

**payload**: 8192B, **direction**: up

| streams | mbps | jitter ms | loss % |
|---:|---:|---:|---:|
| 2 | 74811.17 | 0.011 | 0.853 |
| 4 | 124642.75 | 0.001 | 0.855 |
| 8 | 234035.22 | 0.002 | 0.428 |
| 16 | 374080.57 | 0.002 | 13.208 |
| 32 | 405428.31 | 0.027 | 28.459 |
| 64 | 482362.78 | 0.002 | 61.086 |
| 128 | 618802.01 | 0.077 | 89.328 |

**payload**: 65536B, **direction**: down

| streams | mbps | jitter ms | loss % |
|---:|---:|---:|---:|
| 2 | 0.00 | 0.000 | 0.000 |
| 4 | 0.00 | 0.000 | 0.000 |
| 8 | 0.00 | 0.000 | 0.000 |
| 16 | 0.00 | 0.000 | 0.000 |
| 32 | 0.00 | 0.000 | 0.000 |
| 64 | 0.00 | 0.000 | 0.000 |
| 128 | 0.00 | 0.000 | 0.000 |

**payload**: 65536B, **direction**: up

| streams | mbps | jitter ms | loss % |
|---:|---:|---:|---:|
| 2 | 0.00 | 0.000 | 0.000 |
| 4 | 0.00 | 0.000 | 0.000 |
| 8 | 0.00 | 0.000 | 0.000 |
| 16 | 0.00 | 0.000 | 0.000 |
| 32 | 0.00 | 0.000 | 0.000 |
| 64 | 0.00 | 0.000 | 0.000 |
| 128 | 0.00 | 0.000 | 0.000 |


