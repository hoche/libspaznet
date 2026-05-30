# Coroutine pitfalls

A short list of gotchas that have actually bitten this codebase, plus
the patterns we use to avoid them. Read [`concurrency-and-coroutines.md`](concurrency-and-coroutines.md)
first for the model.

## 1. Coroutine-lambda lifetime

A C++ coroutine preserves its **parameters** and **local variables**
across suspension points — *not* the object that the function is a
member of. For lambdas, the captures are member variables of the
lambda object, so:

```cpp
spaznet::Task bad_handler() {
    auto buf = std::vector<uint8_t>(4096);

    co_return co_await [&buf]() -> spaznet::Task {  // ← bug
        co_await read_into(buf);
        co_return;
    }();  // lambda destroyed at end of full-expression
          // but the coroutine has captured `buf` BY REFERENCE,
          // and the lambda object is gone before resume
}
```

The lambda is a temporary that dies at the end of the
full-expression — but the coroutine frame stores a pointer to its
`operator()`'s implicit `this`, which is now dangling. The next
resume reads through that pointer.

**Rule we follow**: don't make handlers or other long-lived coroutines
be lambda invocations. Make them named member functions on a class
whose lifetime you control, or free functions that take parameters
by value.

There's a deeper write-up of the language semantics at
<https://stackoverflow.com/questions/60592174/lambda-lifetime-explanation-for-c20-coroutines>
— that's the post our older `lambda_cautions.txt` was a copy of.

## 2. Capturing `this` in a coroutine

Closely related: a coroutine that captures `this` is fine *only* if
the object outlives every suspension point. The `WebSocketHandler`
implementation in `server_impl.cpp` works because the `Server` owns
the handler in a `unique_ptr` and never destroys it until after
`Server::stop` has drained every in-flight coroutine.

If your handler stores per-connection state in a member variable and
multiple connections call into it concurrently, that's racy — see
[`websocket.md`](websocket.md) for the recommended pattern (locals
inside the coroutine, or a keyed `std::unordered_map` with a mutex).

## 3. References to caller-stack data after `co_await`

```cpp
spaznet::Task echo(spaznet::Socket& socket) {
    std::string greeting = "hi\n";
    co_await socket.async_write({greeting.begin(), greeting.end()});
    // OK so far: greeting is a coroutine local; the frame holds it.
}
```

vs

```cpp
spaznet::Task echo(const std::string& greeting, spaznet::Socket& socket) {
    co_await socket.async_write({greeting.begin(), greeting.end()});
    // DANGER: `greeting` is a const& parameter.  The coroutine frame
    // holds the *reference*, not the bytes.  If the caller passed a
    // temporary, the temporary may be destroyed before this resumes.
}
```

The fix is to take such parameters **by value** so the coroutine
frame owns a copy:

```cpp
spaznet::Task echo(std::string greeting, spaznet::Socket& socket) { ... }
```

`Socket&` is the deliberate exception: the dispatcher guarantees the
`Socket` outlives the coroutine, so the reference is safe.

## 4. `std::move` from a parameter that another `co_await` will read

```cpp
spaznet::Task handle_message(spaznet::WebSocketMessage&& m,
                             spaznet::Socket& s) override {
    co_await s.send_websocket_message(m.opcode, m.data);
    log_size(m.data.size());   // ← UB if send_websocket_message had moved m.data
}
```

`send_websocket_message` takes `std::span<const uint8_t>`, so it doesn't
move — but if you forwarded the parameter into a function that *could*
move it, the second use is reading from a moved-from vector. Don't
move from a parameter you'll read again after a `co_await`.

## 5. Throwing from a coroutine

Exceptions thrown inside a coroutine propagate through the `Task`'s
final-suspend handler. The dispatcher (`Server::handle_connection`)
catches them with `catch (...)` and closes the connection. The peer
sees the TCP socket drop — no graceful close frame, no HTTP error
response.

If you want a graceful close:

```cpp
try {
    co_await do_thing();
} catch (const my_app_error& e) {
    response.status_code = 500;
    response.body = bytes_of(e.what());
    co_return;
}
```

Catch where you can produce a response. Don't rely on the dispatcher's
catch-all.

## 6. Re-entering the same coroutine from itself

Don't `co_await` a function that, transitively, awaits the same
in-progress operation:

```cpp
spaznet::Task fan_out(spaznet::Socket& s) {
    co_await s.async_write(...);    // suspends; resume on IO ready
    co_await s.async_write(...);    // fine: sequential
    co_await another_coro(s);       // fine if another_coro doesn't
                                    // try to take a lock that the
                                    // caller already holds
}
```

The library doesn't currently hold locks across `co_await`, so
re-entry within a single connection's coroutine chain is safe. But
if your handler takes an application-level mutex and `co_await`s
under it, beware: the resume may land on another thread that's
waiting for the same mutex. Hold mutexes for synchronous work only,
release before `co_await`.

## 7. `Task` is move-only

`Task` is a coroutine handle wrapper with refcount semantics — it's
move-only by design. Passing one through `std::function<Task()>` or
storing it in a `std::vector<Task>` works fine; copying does not.

## 8. `co_await` an awaitable, not a `Task`-returning function

```cpp
co_await some_func();      // OK: some_func() returns a Task,
                           // Task is itself awaitable
auto t = some_func();
co_await t;                // OK
co_await some_func;        // ERROR: some_func is a function, not awaitable
```

Compiler errors here are confusing (the diagnostic mentions
`std::coroutine_traits` and `promise_type`). If you see those,
check that you're awaiting the *result* of the call.

## Related

- [`concurrency-and-coroutines.md`](concurrency-and-coroutines.md) —
  what the runtime actually does
- [`threading.md`](threading.md) — when coroutines migrate between
  threads
- [`websocket.md`](websocket.md), [`http.md`](http.md) — handler
  authoring patterns
