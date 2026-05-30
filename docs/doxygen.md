# Doxygen API reference

This file describes how to generate Doxygen API docs for libspaznet and
how to annotate coroutine-heavy code so the output stays accurate.

## Status

Doxygen is **not wired into the CMake build** and the HTML output is
**not hosted**. To get an API reference you run `doxygen` yourself
locally, against the in-tree `docs/Doxyfile`. The narrative docs under
`docs/*.md` (everything else in this directory) are the recommended
entry point; Doxygen exists for when you need a comprehensive symbol
listing.

If you want the API reference always available, the standard recipes
are:

- Generate to `build/docs/html/` and serve it from a static-site host
  (GitHub Pages, S3, …) in CI.
- Or add a `cmake --build . --target docs` custom target that runs
  Doxygen. Neither is in tree today.

## Quick Start

1. Install Doxygen and Graphviz (for call graphs):
   ```bash
   # macOS
   brew install doxygen graphviz

   # Debian / Ubuntu
   sudo apt-get install doxygen graphviz
   ```
2. From the repo root, generate docs into `build/docs/html`:
   ```bash
   doxygen docs/Doxyfile
   ```
3. Open `build/docs/html/index.html` in your browser.

## What `docs/Doxyfile` does

- Scans `include/` and `src/`.
- Uses [`docs/concurrency-and-coroutines.md`](concurrency-and-coroutines.md)
  as the main page (`USE_MDFILE_AS_MAINPAGE`).
- Generates call graphs for coroutine entry points.
- Shows source with line numbers for navigation.

If you're modifying it, run `doxygen -u docs/Doxyfile` after a
Doxygen version bump to keep the config in sync with current
defaults.

## How to document coroutines

- **Return types:** Use `Task` in signatures and describe awaited
  operations in `\brief` / `\details`.
- **Thread hops:** Mention when a coroutine may resume on a different
  worker. Example: "Resumes on any worker thread after I/O
  readiness." See [`threading.md`](threading.md) for the model.
- **Awaitables:** Document awaiter structs (e.g.
  `IOContext::TimerAwaiter`) with `\struct` and describe their
  `await_suspend` behavior.
- **No lambdas for handlers:** Virtual overrides must return `Task`;
  plain lambdas don't participate in continuation chaining. See
  [`coroutine-pitfalls.md`](coroutine-pitfalls.md) for why.

Example for a coroutine handler:

```cpp
/// \brief Handles an HTTP request asynchronously using libspaznet
///        coroutines.
/// \details Starts on the dispatching thread, may resume on any
///          worker after I/O readiness.  Avoid mixing with ad-hoc
///          lambdas; keep the coroutine in Task.
/// \param request   Parsed HTTP request.
/// \param response  Mutable response to fill.
/// \param socket    Connected client socket; non-owning.
/// \return Task that completes when the response is ready to send.
Task handle_request(const HTTPRequest& request,
                    HTTPResponse& response,
                    Socket& socket) override;
```

## Groups

Group related APIs with `\defgroup` to improve sidebar navigation:

- `\defgroup core Core` — `IOContext`, `PlatformIO`, `Task`, `TaskQueue`
- `\defgroup handlers Handlers` — HTTP/1.1, HTTP/2, WebSocket, UDP
- `\defgroup quic QUIC` — `Connection`, `Listener`, `TlsContext`
- `\defgroup http3 HTTP/3` — `Http3Server`, `QuicHttp3Service`
- `\defgroup utils Utilities` — `binary_utils`, `header_utils`,
  `number_utils`, `string_utils`

Attach symbols with `\ingroup <name>` in their comments.

## Diagrams

Diagrams under `docs/svgs/` are standalone, hand-authored SVG files
(no Mermaid or other runtime dependency). Markdown references them
with plain image syntax — e.g.
`![Threading layout](svgs/threading-multi.svg)` — which renders
identically on GitHub and in Doxygen-generated HTML.

To revise a diagram, edit the `.svg` directly; there is no source
format to keep in sync. SVG XML is small and verbose but readable in
a text editor, and tools like Inkscape, Boxy SVG, or a browser's
devtools work well for visual tweaks.

When adding a new diagram, add a one-line `<title>` element at the
top so screen readers and the diagram-vs-text-prose-as-fallback
case work. Doxygen pulls `<title>` for alt text automatically when
the SVG is referenced from a `\image` directive.

## Related

- [`api-status.md`](api-status.md) — narrative status matrix; you
  almost always want this before Doxygen.
- [`concurrency-and-coroutines.md`](concurrency-and-coroutines.md) —
  the Doxygen main page.
