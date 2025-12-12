# Doxygen Guide

This file describes how to generate Doxygen API docs for `libspaznet` and how to annotate coroutine-heavy code so the output stays accurate.

## Quick Start

1. Install Doxygen and Graphviz (for call graphs):
   ```bash
   sudo apt-get install doxygen graphviz
   ```
2. From the repo root, generate docs into `build/docs/html`:
   ```bash
   doxygen docs/Doxyfile
   ```
3. Open `build/docs/html/index.html` in your browser.

## Included Doxyfile

`docs/Doxyfile` is configured to:
- Scan `include/` and `src/`
- Enable Markdown (`USE_MDFILE_AS_MAINPAGE = docs/concurrency-and-coroutines.md`)
- Generate call graphs for coroutine entry points
- Show source with line numbers for navigation

Feel free to adjust output paths or enabled diagrams to match your build environment.

## How to Document Coroutines and Threads

- **Return types:** Use `Task` in signatures and describe the awaited operations in `\brief`/`\details`.
- **Thread hops:** Mention when a coroutine may resume on a different worker. Example: “Resumes on any worker thread after I/O readiness.”
- **Awaitables:** Document awaiter structs (e.g., `IOContext::TimerAwaiter`) with `\struct` and describe their `await_suspend` behavior.
- **No lambdas for handlers:** When documenting handlers, note that virtual overrides must return `Task`; plain lambdas do not participate in continuation chaining.

Example comment for a coroutine handler:
```cpp
/// \brief Handles an HTTP request asynchronously using libspaznet coroutines.
/// \details Starts on the dispatching thread, may resume on any worker after
/// I/O readiness. Avoid mixing with ad-hoc lambdas; keep the coroutine in Task.
/// \param request Parsed HTTP request.
/// \param response Mutable response to fill.
/// \param socket Connected client socket; non-owning.
/// \return Task that completes when the response is ready to send.
Task handle_request(const HTTPRequest& request,
                    HTTPResponse& response,
                    Socket& socket) override;
```

## Common Groups

Consider grouping related APIs to improve navigation:

- `\defgroup core Core` — `IOContext`, `PlatformIO`, `Task`, `TaskQueue`
- `\defgroup handlers Handlers` — HTTP/1.1, HTTP/2, WebSocket, UDP
- `\defgroup utils Utilities` — `binary_utils`, `header_utils`, `number_utils`, `string_utils`

Attach symbols with `\ingroup core` (etc.) in their comments.

## Mermaid in Doxygen

Doxygen preserves fenced code blocks; GitHub-compatible Mermaid diagrams in `docs/*.md` (e.g., `concurrency-and-coroutines.md`) render on GitHub and remain readable in generated HTML as code blocks. Keep them fenced with ```mermaid for consistency.

