# Integrating libspaznet into your project

Two supported paths: install once and use `find_package`, or pull
the source tree in via `add_subdirectory`. Pick one based on whether
you want to track upstream or pin to a snapshot.

## Requirements (for any consumer)

- **C++20 compiler**. Specifically: GCC 13.1+, Clang 17+, or
  AppleClang from Xcode 15+. The library uses `<format>`,
  `<coroutine>`, and `<span>`.
- **CMake 3.20+**.
- **TCP TLS (HTTPS / WSS / h2)** — optional. Default: **OpenSSL 1.1.1+**
  via `SPAZNET_ENABLE_TLS` (auto-disables with a warning if missing).
  Enables `Server::listen_tls` for HTTP/1.1, HTTP/2 (`h2`), and
  WebSocket upgrade over TLS. See *TCP TLS* under *TLS backend
  location* below.
- **QUIC TLS backend** — *only* required if you want QUIC + HTTP/3.
  Default: **OpenSSL 3.5+**. Alternate: **wolfSSL** with QUIC
  (`-DSPAZNET_USE_WOLFSSL=ON`). Without a usable backend the rest of
  the library builds fine; `SPAZNET_BUILD_QUIC` auto-disables with a
  warning. Independent of `SPAZNET_ENABLE_TLS`.

## Option 1: install + `find_package`

The library exports a CMake config package so downstream projects
can use the standard `find_package` flow.

```bash
git clone <repo> libspaznet && cd libspaznet
cmake -B build -DCMAKE_BUILD_TYPE=Release \
               -DCMAKE_INSTALL_PREFIX=/opt/spaznet
cmake --build build -j
cmake --install build
```

Then in your project's `CMakeLists.txt`:

```cmake
find_package(spaznet REQUIRED)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE spaznet::spaznet)
```

And configure your project with the install prefix on the search
path:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/opt/spaznet
```

The core `spaznet::spaznet` package brings `Threads::Threads`
unconditionally, and `find_dependency(OpenSSL)` when the install was
built with `SPAZNET_ENABLE_TLS`. Link `spaznet::quic_http3` for
QUIC + HTTP/3; that target carries the selected QUIC TLS backend
(OpenSSL 3.5 or wolfSSL) separately from core TCP TLS.

### `find_package` from `vcpkg` / `Conan` / system package manager

Not supported today. There's no `vcpkg.json` checked in and no Conan
recipe. If you want to package libspaznet for a registry, the
`spaznetConfig.cmake` produced by the install is already
relocatable — you can wrap it.

## Option 2: `add_subdirectory`

Drop the libspaznet tree alongside your code and let CMake build it
as part of your project. This is what `netbench/` does.

```cmake
add_subdirectory(third_party/libspaznet)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE spaznet)
```

Notes:

- Use `EXCLUDE_FROM_ALL` on the subdirectory if you don't want
  libspaznet's tests + benchmarks to land in `cmake --build`'s default
  target set. The library target itself (`spaznet`) is always built
  when you link to it.
- libspaznet pulls in GoogleTest via `FetchContent` at configure
  time (for its own tests). The download happens on first configure
  even with `EXCLUDE_FROM_ALL`, but the test binaries don't compile.

## The `SPAZNET_BUILD_QUIC` / `SPAZNET_USE_WOLFSSL` knobs

`SPAZNET_BUILD_QUIC` defaults `ON`. When `OFF` (or auto-disabled
because no usable TLS backend was found), the build skips:

- The `spaznet::quic_http3` library (`example/quic-http3/`).
- All QUIC + HTTP/3 tests + benchmarks.

`SPAZNET_USE_WOLFSSL` defaults `OFF` (OpenSSL 3.5). Set `ON` to
build QUIC against a QUIC-enabled wolfSSL instead — OpenSSL is then
not required.

Cleartext HTTP/1.1, HTTP/2 (h2c), WebSocket, and UDP build with
`-DSPAZNET_ENABLE_TLS=OFF` and no OpenSSL on core. HTTPS / WSS / h2
over TLS need TLS on.

To force-disable QUIC:

```cmake
set(SPAZNET_BUILD_QUIC OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libspaznet)
```

Or on the command line:

```bash
cmake -B build -DSPAZNET_BUILD_QUIC=OFF
```

To detect from your own code whether you got the QUIC API:

```cpp
#ifdef SPAZNET_HAS_QUIC
    server.set_coroutine_datagram_handler(
        spaznet::http3::make_coroutine_dispatcher(std::move(svc)));
#endif
```

`SPAZNET_HAS_QUIC` is defined by the `spaznet::quic_http3` target's
public interface, so any TU that links it gets the macro.

## TLS backend location

Two independent stacks: **TCP TLS** on core (`listen_tls`) and **QUIC
TLS** on `spaznet::quic_http3`. You can enable either, both, or neither.

### TCP TLS (OpenSSL 1.1.1+)

`SPAZNET_ENABLE_TLS` defaults `ON` when `find_package(OpenSSL 1.1.1)`
succeeds. Distro OpenSSL 3.x packages satisfy this; no separate 3.5
install is required for HTTPS/WSS.

Internals that matter for integrators:

- Ciphertext rides **memory BIOs** + explicit `recv`/`send` (OpenSSL
  never owns the socket). Safe with IOCP's zero-byte readiness probes.
- Handshake runs on the accept path; the live `TlsStream` hands off to
  the connection factory via `thread_local` (same thread), then lives
  under `Socket` / `BufferedConnection`. Dispatchers see plaintext.
- **Reactor** TLS takes no per-connection mutex (IO-thread affinity).
  **Coroutine** `Socket::attach_tls` calls `enable_serialized_io()` so
  HTTP/2∥WS reader/writer tasks sharing one `SSL*` serialize.
- Per-listener ALPN only (`http/1.1` or `h2`); no multi-protocol ALPN
  mux on one port. WSS uses `alpn={"http/1.1"}`.

```bash
# Typical: system OpenSSL is enough for TCP TLS
cmake -B build

# Explicit off
cmake -B build -DSPAZNET_ENABLE_TLS=OFF
```

Detect in application code:

```cpp
#ifdef SPAZNET_HAS_TLS
    server.listen_tls(8443, std::move(cfg));
#endif
```

### QUIC: OpenSSL 3.5+ (default)

On most Linux distros OpenSSL 3.5+ is too new for stock packages —
you'll need a custom install. On macOS Homebrew ships
`openssl@3.5`. The build looks at `OPENSSL_ROOT_DIR`:

```bash
# macOS Homebrew
cmake -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3.5)

# Custom install
cmake -B build -DOPENSSL_ROOT_DIR=/opt/openssl-3.5
```

The CMake check is `find_package(OpenSSL 3.5 QUIET)` — `QUIET` so a
missing OpenSSL doesn't error; it warns and disables QUIC. A system
OpenSSL 1.1.1 / 3.0 install can still satisfy **TCP TLS** even when
QUIC is disabled.

### QUIC: wolfSSL (alternate)

Stock distro wolfSSL packages usually lack `--enable-quic`. Build
from source, then point CMake at the prefix:

```bash
./configure --prefix=$HOME/wolfssl \
  --enable-quic --enable-opensslextra --enable-opensslall \
  --enable-session-ticket --enable-alpn --enable-tls13 \
  --enable-aesgcm --enable-chacha --enable-poly1305 \
  --enable-hkdf --enable-sha384 --enable-ecc \
  --enable-supportedcurves --enable-sni \
  --enable-certgen --enable-keygen --enable-aesctr \
  --disable-shared --enable-static \
  CPPFLAGS="-DHAVE_AES_ECB"
make -j && make install

cmake -B build \
  -DSPAZNET_USE_WOLFSSL=ON \
  -DWOLFSSL_ROOT=$HOME/wolfssl
```

CMake probes for `wolfSSL_set_quic_method` and refuses the wolfSSL
path (with a warning + QUIC disabled) if QUIC support is missing.

## Compiler selection

If your distro's default `g++` is older than 13.1, install `g++-13`
and point CMake at it:

```bash
sudo apt install g++-13
cmake -B build -DCMAKE_CXX_COMPILER=g++-13
```

The build has a configure-time `<format>` probe (`check_cxx_source_compiles`)
that fails fast with an actionable message rather than blowing up
deep in compilation.

## Minimal `main.cpp` for a consumer

```cpp
#include <libspaznet/server.hpp>
#include <libspaznet/http/dispatcher.hpp>
#include <libspaznet/http/handler.hpp>

class Hello : public spaznet::http::HTTPHandler {
public:
    void handle_request(
        const spaznet::http::HTTPRequest&,
        spaznet::http::ResponseWriter writer
    ) override {
        spaznet::http::HTTPResponse r;
        r.body = {'O','K'};
        writer.complete(std::move(r));
    }
};

int main() {
    spaznet::Server server(4);
    server.set_coroutine_connection_handler(
        spaznet::http::make_coroutine_dispatcher(std::make_unique<Hello>()));
    server.listen_tcp(8080);
    server.run();
}
```

Build with:

```bash
g++-13 -std=c++20 main.cpp -lspaznet_http -lspaznet -lpthread -o myapp
```

Or in CMake:

```cmake
cmake_minimum_required(VERSION 3.20)
project(myapp CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(spaznet REQUIRED)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE spaznet::spaznet spaznet::http)
```

The core `spaznet::spaznet` target has only the low-level
`Server` / `Socket` / `IOContext` / `Task` / `PlatformIO`.  Protocol
support comes from the example libraries:

| Library | Headers | Use for |
|---|---|---|
| `spaznet::http` | `<libspaznet/http/{handler,dispatcher}.hpp>` | HTTP/1.1 |
| `spaznet::http_websocket` | `<libspaznet/websocket/{handler,dispatcher,send}.hpp>` + the http ones | HTTP/1.1 + WebSocket on the same port |
| `spaznet::http2` | `<libspaznet/http2/{handler,dispatcher}.hpp>` | HTTP/2 (h2c) |
| `spaznet::udp` | `<libspaznet/udp/{handler,dispatcher}.hpp>` | UDP handler-interface idiom |
| `spaznet::quic_http3` | `<libspaznet/{quic,http3}/...>` | QUIC v1 + HTTP/3 |

Link only the libraries you need; the core target is dependency-free
(no OpenSSL even if `SPAZNET_BUILD_QUIC=ON`).

## Versioning + upgrades

libspaznet does not ship versioned releases yet. Pin a commit SHA
in your project, read [`CHANGELOG.md`](../CHANGELOG.md) before
bumping, and re-run your test suite. The only deliberate hard break
so far is the 2026-05-29 removal of the toy QUIC/HTTP/3 types —
see [`migration.md`](migration.md).

## Related

- [`CHANGELOG.md`](../CHANGELOG.md) — what changed when
- [`migration.md`](migration.md) — breaking changes between snapshots
- [`api-status.md`](api-status.md) — what's safe to depend on
