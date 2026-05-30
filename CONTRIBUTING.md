# Contributing

Thanks for looking. A few things to know before sending a change.

## Quickstart

```bash
# Build (out-of-tree, Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Or just:
make

# Run tests
make test                    # all suites
make test-unit               # fast: unit only
make test-integration        # slower: real sockets
make test-performance        # benchmarks

# Or individual binaries
./build/test_unit
./build/test_integration --gtest_filter='WebSocket*'
./build/test_performance --gtest_filter='ConcurrentPerformanceTest.*'
```

## Where things live

```
include/libspaznet/                public headers
  server.hpp                       Server + Socket
  io_context.hpp                   Task, IOContext, coroutine plumbing
  handlers/                        HTTP, HTTP/2, WebSocket, UDP, HTTP/3 interfaces
  quic/                            QUIC transport (Connection, Listener, TLS, ...)
  http3/                           HTTP/3 + QPACK on top of quic::

src/                               implementations
  server_impl.cpp                  big file — accept, dispatch, WS frame loop, h1 handling
  platform/                        epoll / kqueue / poll / IOCP backends
  handlers/                        h1, h2, WS, UDP
  quic/                            QUIC internals
  http3/                           HTTP/3 internals

tests/
  unit/                            fast, in-process, no real sockets
  integration/                     real loopback sockets, real handlers
  performance/                     benchmarks (some run iperf3)

docs/                              user-facing documentation
```

## Branch + commit conventions

- Branch names: `fix/<short-thing>`, `quic-<feature>`, `docs/<thing>`.
  No strict rules — descriptive prefix is enough.
- Each commit should compile and pass tests on its own. Prefer
  small, reviewable commits over one mega-commit.
- Commit message: first line is a concise imperative summary
  (≤72 chars). Blank line. Body explains the **why** if non-obvious.
  Reference RFC sections when fixing RFC-conformance bugs.
- No `Co-Authored-By: Claude` trailers on commits — this is a
  hard rule. Other co-author trailers are fine.

Look at recent commits for the house style:

```bash
git log --oneline -20
```

## Code style

```bash
make format        # clang-format every C/C++ source
make check-format  # CI-equivalent: fail if anything needs reformatting
```

The repo uses `.clang-format` checked in. Don't reformat unrelated
files in a feature PR — limit `make format` runs to files you've
already touched, or rebase the formatting into a separate commit.

Naming:

- Types: `CamelCase`.
- Functions, methods, variables: `snake_case`.
- Member variables: trailing underscore (`buffer_`, `pending_io_`).
- Constants: `kCamelCase` for inline constants, `CamelCase` for
  CMake/compile-time constants.
- File names: `snake_case.cpp` / `.hpp`.

Comments only where the *why* is non-obvious. Lean on
self-explanatory names; don't narrate what the code does.

## Static analysis

```bash
make check-tidy        # clang-tidy
make check-cppcheck    # cppcheck
make lint              # everything
```

Both tools have project configs (`.clang-tidy`, plus per-file
`NOLINTBEGIN`/`NOLINTEND` for files where rules don't make sense).
Don't sprinkle new `NOLINT` blocks without a comment justifying
each one.

## Sanitizer builds

We run ASan, UBSan, and TSan in CI. Local sanitizer builds:

```bash
# ASan + UBSan
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j
./build-asan/test_unit
./build-asan/test_integration

# TSan
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
cmake --build build-tsan -j
./build-tsan/test_integration
```

On meep, TSan needs `setarch -R ./test_integration` to dodge the
kernel-ASLR-vs-tsan-shadow conflict.

If you change anything in `src/quic/`, `src/platform/`, or
`server_impl.cpp`, run at least ASan locally before sending a PR.

## Adding tests

| Kind | Lives in | Run on every PR? | Typical size |
|---|---|---|---|
| Unit | `tests/unit/test_<thing>.cpp` | yes | <1 ms per case |
| Integration | `tests/integration/test_<thing>.cpp` | yes | 10–1000 ms |
| Performance | `tests/performance/test_<thing>.cpp` | yes (sanity-only) | seconds |

A unit test that takes >50 ms is doing real I/O — promote it to
integration.

Add the file to `CMakeLists.txt` under the relevant
`SPAZNET_*_TEST_SOURCES` list. Tests under `if(SPAZNET_BUILD_QUIC)`
only compile when OpenSSL 3.5+ is available — put QUIC/HTTP/3
tests there.

## Adding a new protocol handler

There are five steps. Look at how WebSocket is wired for a worked
example; it's the cleanest precedent.

1. **Public interface** in `include/libspaznet/handlers/<protocol>_handler.hpp`.
   Pure-virtual base class with the callback set you want users to
   override. Return `Task` from anything that may need to `co_await`.
2. **Implementation file** in `src/handlers/<protocol>_handler.cpp`,
   if there's parser / serializer code worth isolating from the
   server dispatch. Many protocols don't need this — the WS parser
   lives directly in `server_impl.cpp` for hot-path locality.
3. **Setter on `Server`** in `include/libspaznet/server.hpp`:
   `void set_<protocol>_handler(std::unique_ptr<...>)`.
4. **Dispatch wiring** in `Server::handle_connection` (TCP) or
   `Server::receive_udp` (UDP) in `src/server_impl.cpp`. This is
   where new protocols actually start running. **A handler without
   dispatch is a stub** — see `HTTP2Handler` in
   [`api-status.md`](docs/api-status.md) for a cautionary tale.
5. **Tests**: at minimum a unit test for the parser/serializer and
   an integration test that talks to a real socket. Document the
   protocol in `docs/<protocol>.md`.

## Pull requests

- Rebase, don't merge. `git rebase origin/main` before pushing.
- One coherent feature or fix per PR. If a refactor and a fix are
  intertwined, send the refactor first.
- Reference any TODO.md entry you're closing in the PR description.
- Update [`CHANGELOG.md`](CHANGELOG.md) and
  [`docs/api-status.md`](docs/api-status.md) when the change is
  user-visible (new public type, new behavior, removed feature,
  status promotion).
- Document anything user-facing under `docs/`. SVG diagrams go in
  `docs/svgs/`.

## What gets reviewed

The author runs `make lint`, `make test`, and at least one sanitizer
build before sending the PR. CI runs the same on Linux (x86_64 +
arm64), macOS (Apple Silicon), FreeBSD (x86_64 + arm64), and
Windows. Reviewers focus on:

- Correctness, especially RFC conformance for protocol code.
- Concurrency safety (most regressions show up under TSan).
- Whether the change introduces new locks or new allocations on
  the hot path — both warrant explicit justification.
- API surface stability ([`docs/api-status.md`](docs/api-status.md)).

## Reporting issues

Open a GitHub issue with:

- Platform (OS, CPU arch, compiler + version, OpenSSL version).
- A minimal reproducer if it's a bug.
- The output of `git log -1` from your tree.

Security issues — anything that lets an attacker DoS the server,
crash it, exfiltrate memory, or speak as another peer — get reported
privately, not in a GitHub issue. Email is the only channel right
now (and the author is the only triage point); a future SECURITY.md
will formalize this.

## Things that aren't accepted

- New dependencies. The library is built on OpenSSL (QUIC TLS only)
  + libc++/libstdc++ + the C++ standard library. Adding a fourth is
  a serious decision; bring it up in an issue first.
- Vendored copies of someone else's code.
- Build-system changes that aren't CMake.
- Anything that requires C++23+. The compile target is C++20.
