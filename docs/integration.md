# Integrating libspaznet into your project

Two supported paths: install once and use `find_package`, or pull
the source tree in via `add_subdirectory`. Pick one based on whether
you want to track upstream or pin to a snapshot.

## Requirements (for any consumer)

- **C++20 compiler**. Specifically: GCC 13.1+, Clang 17+, or
  AppleClang from Xcode 15+. The library uses `<format>`,
  `<coroutine>`, and `<span>`.
- **CMake 3.20+**.
- **OpenSSL 3.5+** — *only* required if you want QUIC + HTTP/3.
  Without it the rest of the library (UDP, HTTP/1.1, HTTP/2 parser,
  WebSocket) builds fine; `SPAZNET_BUILD_QUIC` will auto-disable
  with a warning.

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

The package brings `OpenSSL::SSL` + `OpenSSL::Crypto` in transitively
when the install was built with QUIC, and `Threads::Threads`
unconditionally.

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

## The `SPAZNET_BUILD_QUIC` knob

Default `ON`. When `OFF` (or auto-disabled because OpenSSL 3.5+
wasn't found), the build skips:

- `src/quic/*`, `src/http3/*`
- `include/libspaznet/quic/*`, `include/libspaznet/http3/*`
- The `spaznet::quic_http3` library (`example/quic-http3/`).
- All QUIC + HTTP/3 tests + benchmarks.

The HTTP/1.1, HTTP/2 parser, WebSocket, and UDP paths build with no
OpenSSL dependency.

To force-disable:

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
    server.set_datagram_handler(
        spaznet::http3::make_dispatcher(std::move(svc)));
#endif
```

`SPAZNET_HAS_QUIC` is defined by the `spaznet::quic_http3` target's
public interface, so any TU that links it gets the macro.

## OpenSSL location

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
missing OpenSSL doesn't error; it warns and disables QUIC.

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
    spaznet::Task handle_request(
        const spaznet::http::HTTPRequest&,
        spaznet::http::HTTPResponse& r,
        spaznet::Socket&
    ) override {
        r.body = {'O','K'};
        co_return;
    }
};

int main() {
    spaznet::Server server(4);
    server.set_connection_handler(
        spaznet::http::make_dispatcher(std::make_unique<Hello>()));
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
