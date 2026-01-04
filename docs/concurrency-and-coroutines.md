# Concurrency, Coroutines, and Event Loop

This document explains how `libspaznet` schedules work, how coroutines move between threads, and why coroutine-native handlers cannot be mixed with ad-hoc lambdas.

## Architecture Overview

```mermaid
graph TB
    subgraph Application[Application Layer]
        HANDLER[HTTP/WebSocket/QUIC Handlers]
        HANDLER -->|returns Task| SCHEDULE[IOContext::schedule]
    end
    
    subgraph IOContext[IOContext - Event Loop Coordinator]
        MAIN[Main Loop Thread]
        SCHEDULE -->|round-robin| QUEUES[TaskQueues 0..N-1]
        MAIN -->|process_io_events| PENDING[(pending_io_ map)]
        MAIN -->|process_timers| TIMERS[(Timer Heap)]
    end
    
    subgraph Workers[Worker Thread Pool]
        W0[Worker 0]
        W1[Worker 1]
        WN[Worker N-1]
    end
    
    subgraph Platform[Platform I/O Abstraction]
        PIO[PlatformIO epoll/kqueue/poll/IOCP]
        PIO -->|wait events| KERNEL[OS Kernel]
        KERNEL -->|I/O ready| PIO
    end
    
    subgraph Coroutines[Coroutine Execution]
        CORO1[Coroutine A]
        CORO2[Coroutine B]
        CORO3[Coroutine C]
    end
    
    QUEUES -->|dequeue| Workers
    Workers -->|resume| Coroutines
    Coroutines -->|co_await I/O| PENDING
    Coroutines -->|co_await timer| TIMERS
    Coroutines -->|yield| QUEUES
    
    PIO -->|events| MAIN
    MAIN -->|schedule ready| QUEUES
    
    style IOContext fill:#e1f5ff
    style Workers fill:#fff4e1
    style Platform fill:#e1ffe1
    style Coroutines fill:#f0e1ff
    style Application fill:#ffe1f5
```

## Core Execution Model

- **IOContext** owns the event loop, worker threads, timer wheel, and platform-specific I/O demultiplexer.
- **Task** is a coroutine return type; its promise (`TaskPromise`) stores a continuation handle so `co_await` chains resume correctly.
- **TaskQueue** is a multi-producer/single-consumer queue per worker thread; enqueue is atomic, dequeue is mutex-protected to simplify correctness.
- **PlatformIO** (epoll/kqueue/poll/IOCP) translates OS events into coroutine resumes; pending I/O registrations store raw coroutine handles for atomic swaps.

```mermaid
flowchart TB
    subgraph MainLoop[Main Event Loop Thread]
        RUN[IOContext::run]
        TIMERS[process_timers]
        WAIT[platform_io_->wait]
        PROC[process_io_events]
        STEAL[Steal from queues]
    end
    
    subgraph Workers[Worker Threads 0..N-1]
        W0[Worker 0]
        W1[Worker 1]
        WN[Worker N-1]
    end
    
    subgraph Queues[TaskQueues]
        Q0[TaskQueue 0]
        Q1[TaskQueue 1]
        QN[TaskQueue N-1]
    end
    
    subgraph Platform[Platform I/O]
        PIO[epoll/kqueue/poll/IOCP]
        PENDING[(pending_io_ map)]
    end
    
    subgraph TimerHeap[Timer Management]
        HEAP[(Priority Queue)]
        CANCEL[(Cancelled Set)]
    end
    
    subgraph Kernel[OS Kernel]
        EVENTS[I/O Events]
        CLOCK[System Clock]
    end
    
    RUN --> TIMERS
    TIMERS --> WAIT
    WAIT -->|timeout| PROC
    WAIT -->|events| PROC
    PROC --> STEAL
    STEAL --> RUN
    
    Kernel -->|fd ready| EVENTS
    EVENTS --> PIO
    PIO --> PROC
    PROC -->|extract handles| PENDING
    PROC -->|schedule| Queues
    
    CLOCK --> TIMERS
    TIMERS -->|check due| HEAP
    TIMERS -->|schedule| Queues
    HEAP --> CANCEL
    
    Queues -->|dequeue| Workers
    Workers -->|resume| CORO[Coroutines]
    CORO -->|yield/suspend| Queues
    Workers -->|reschedule if not done| Queues
    
    style MainLoop fill:#e1f5ff
    style Workers fill:#fff4e1
    style Queues fill:#f0e1ff
    style Platform fill:#e1ffe1
    style TimerHeap fill:#ffe1f5
```

## Coroutine Lifecycle and I/O Suspension

```mermaid
sequenceDiagram
    autonumber
    participant Handler as Handler Code
    participant Coro as Coroutine Frame
    participant Promise as TaskPromise
    participant Socket as Socket::async_read
    participant IO as IOContext
    participant PIO as PlatformIO
    participant Kernel as OS Kernel
    participant Queue as TaskQueue
    participant Worker as Worker Thread
    
    Handler->>Coro: Task handle_request(...)
    Coro->>Promise: initial_suspend() → suspend_always
    Note over Coro: Coroutine suspended at start
    
    Handler->>IO: schedule(Task)
    IO->>Queue: enqueue(Task) [round-robin]
    Queue->>Worker: dequeue(Task)
    Worker->>Coro: handle.resume()
    
    Note over Coro: Coroutine executing
    Coro->>Socket: co_await async_read(buffer)
    Socket->>Socket: await_ready() → false
    Socket->>IO: register_io(fd, EVENT_READ, handle)
    IO->>PIO: add_fd(fd, events)
    IO->>PIO: pending_io_[fd].read_handle = handle
    Socket->>Coro: await_suspend() → suspend
    Note over Coro: Coroutine suspended,<br/>handle stored atomically
    
    Coro-->>Worker: (suspended, returns to worker)
    Worker->>Queue: (no more work, yield)
    
    Kernel->>PIO: fd becomes readable
    PIO->>IO: wait() returns events
    IO->>IO: process_io_events()
    IO->>IO: pending_io_[fd].read_handle.exchange(nullptr)
    IO->>IO: extract coroutine handle
    IO->>Queue: schedule(Task{handle})
    Queue->>Worker: dequeue(Task)
    Worker->>Coro: handle.resume()
    
    Note over Coro: Coroutine resumed
    Coro->>Socket: await_resume()
    Socket->>Socket: recv() reads data
    Socket-->>Coro: return data
    Coro->>Coro: continue execution
    Coro->>Promise: co_return
    Promise->>Promise: final_suspend()
    Promise->>Promise: resume continuation if any
    Note over Coro: Coroutine completes
```

## Task Continuation Chaining

```mermaid
flowchart TB
    subgraph Task1[Task A]
        T1Coro[Coroutine Frame A]
        T1Promise[TaskPromise A]
        T1Cont[continuation = noop]
    end
    
    subgraph Task2[Task B]
        T2Coro[Coroutine Frame B]
        T2Promise[TaskPromise B]
        T2Cont[continuation = Task A handle]
    end
    
    subgraph Await[co_await Task B]
        Awaiter[Awaiter]
    end
    
    Task1 -->|co_await Task B| Await
    Await -->|await_suspend| Task2
    Task2 -->|sets| T2Cont
    T2Cont -.->|points to| T1Coro
    
    T2Coro -->|executes| T2Coro
    T2Coro -->|completes| T2Promise
    T2Promise -->|final_suspend| T2Cont
    T2Cont -->|resume| T1Coro
    
    style Task1 fill:#e1f5ff
    style Task2 fill:#fff4e1
    style Await fill:#f0e1ff
```

## Complete I/O Operation Flow

```mermaid
stateDiagram-v2
    [*] --> Created: schedule(Task)
    Created --> Scheduled: enqueue to TaskQueue
    Scheduled --> Running: worker dequeue & resume
    Running --> Suspended: co_await async_read()
    Suspended --> Registered: register_io(fd, handle)
    Registered --> Waiting: OS waiting for I/O
    Waiting --> Ready: fd becomes readable
    Ready --> Extracted: process_io_events()
    Extracted --> Rescheduled: schedule(Task)
    Rescheduled --> Running: worker resume
    Running --> Completed: co_return
    Completed --> [*]
    
    note right of Suspended
        Coroutine handle stored
        atomically in pending_io_
    end note
    
    note right of Ready
        Main loop extracts handle
        and schedules for resume
    end note
```

## Threading and Coroutine Handoffs

- `IOContext::run` spins the main loop and also starts N worker threads; all share the same `IOContext`.
- Coroutines may start on the main loop, be resumed on a worker, and bounce between workers as they `co_await` I/O or timers. Handles are portable because the continuation is stored in the promise, not the stack.
- `register_io` places coroutine handles into `pending_io_`; when the OS reports readiness, the handles are atomically extracted and rescheduled.
- Timers are placed in a min-heap and, when due, their coroutine handles are also rescheduled.
- Workers and the main loop both drain queues: every resume that yields again must be resubmitted via `schedule`.

## TaskQueue Internal Structure

```mermaid
flowchart LR
    subgraph Queue[TaskQueue - Lock-Free Enqueue]
        HEAD[head_ atomic Node*]
        TAIL[tail_ atomic Node*]
        DUMMY[Dummy Node]
        N1[Node 1: Task A]
        N2[Node 2: Task B]
        N3[Node 3: Task C]
    end
    
    DUMMY -->|next| N1
    N1 -->|next| N2
    N2 -->|next| N3
    N3 -->|next| NULL
    
    HEAD -.->|points to| DUMMY
    TAIL -.->|points to| N3
    
    ENQ[enqueue Task] -->|new Node| TAIL
    DEQ[dequeue Task] -->|mutex lock| HEAD
    DEQ -->|move Task| OUT[Return Task]
    
    style DUMMY fill:#ffe1e1
    style HEAD fill:#e1ffe1
    style TAIL fill:#e1ffe1
    style ENQ fill:#fff4e1
    style DEQ fill:#f0e1ff
```

## Pending I/O Map Structure

```mermaid
flowchart TB
    subgraph PendingMap[pending_io_ unordered_map]
        FD1[fd: 5]
        FD2[fd: 7]
        FD3[fd: 12]
    end
    
    subgraph PendingIO1[PendingIO for fd: 5]
        R1[read_handle atomic void*]
        W1[write_handle atomic void*]
    end
    
    subgraph PendingIO2[PendingIO for fd: 7]
        R2[read_handle atomic void*]
        W2[write_handle atomic void*]
    end
    
    subgraph PendingIO3[PendingIO for fd: 12]
        R3[read_handle atomic void*]
        W3[write_handle atomic void*]
    end
    
    FD1 --> PendingIO1
    FD2 --> PendingIO2
    FD3 --> PendingIO3
    
    R1 -.->|coroutine handle| CORO1[Coroutine A]
    W1 -.->|coroutine handle| CORO2[Coroutine B]
    R2 -.->|coroutine handle| CORO3[Coroutine C]
    
    LOCK[map_lock_ spinlock] -->|protects| PendingMap
    
    style PendingMap fill:#e1f5ff
    style LOCK fill:#ffe1e1
    style R1 fill:#fff4e1
    style W1 fill:#fff4e1
```

## Timer Management Flow

```mermaid
sequenceDiagram
    participant Coro as Coroutine
    participant Awaiter as TimerAwaiter
    participant IO as IOContext
    participant Heap as Timer Priority Queue
    participant MainLoop as Main Loop
    participant Queue as TaskQueue
    
    Coro->>Awaiter: co_await sleep_for(100ms)
    Awaiter->>Awaiter: await_ready() → false
    Awaiter->>IO: add_timer(now + 100ms, 100ms, false, handle)
    IO->>Heap: push(TimerEntry)
    Note over Heap: Min-heap by next_fire time
    
    MainLoop->>IO: process_timers()
    IO->>Heap: top() - check earliest timer
    alt Timer not due
        IO->>MainLoop: compute_wait_timeout_ms()
        MainLoop->>MainLoop: wait with timeout
    else Timer due
        IO->>Heap: pop()
        IO->>IO: check cancelled_timers_
        IO->>Queue: schedule(Task{handle})
        Queue->>Coro: resume coroutine
    end
    
    Note over Heap: Repeating timers:<br/>next_fire += interval
```

## Why Lambdas and Coroutines Must Not Be Mixed

The scheduler expects **coroutine-aware callables** that return `Task` and yield via `co_await`. Mixing raw lambdas (e.g., `std::function<void()>`, thread-pool callbacks, or ad-hoc captures) with coroutines breaks this contract:

- **No continuation wiring:** Lambdas do not carry a `TaskPromise::continuation`, so resuming through a lambda loses the coroutine chain and can deadlock waits.
- **Wrong lifetime:** Lambdas capture by value and run immediately on the calling thread; they are not reentrant resumable frames. Passing a coroutine handle into a lambda that executes later risks dangling captures or double-destruction when the coroutine frame is already destroyed in `Task`'s destructor.
- **Thread confusion:** Lambdas invoked by external threads bypass `IOContext::schedule`, so they may resume a coroutine on a thread that is not draining the queues, violating the library's scheduling invariants and causing data races against `pending_io_` or timer structures.
- **Type mismatch:** Handlers in `libspaznet` are virtual methods returning `Task`. A lambda with `auto` return cannot satisfy the vtable contract and cannot be stored in the places that expect `Task`.

## What to Do Instead

- **Write coroutine functions that return `Task`.** Define named member functions or free functions with `co_await` and let the scheduler handle thread hops.

```cpp
Task MyHTTPHandler::handle_request(const HTTPRequest& req,
                                   HTTPResponse& res,
                                   Socket& sock) {
    auto read = co_await sock.read_some(buffer);
    co_await ctx.sleep_for(10ms);
    res.status_code = 200;
    res.body.assign(read.begin(), read.end());
    co_return;
}
```

- **Bridge callbacks by scheduling Tasks.** If an external API gives you a callback, convert it to a coroutine task and push it through `IOContext::schedule`.

```cpp
void on_external_ready(IOContext& ctx, ExternalEvent ev) {
    ctx.schedule([](IOContext& ctx, ExternalEvent ev) -> Task {
        // Safe: coroutine frame owns its continuation and lifetime
        co_await ctx.sleep_for(0ms); // yield into scheduler
        co_return;
    }(ctx, ev));
}
```

- **Use awaiters for simple values.** The helper `make_awaiter(value)` produces an already-ready awaitable without mixing in lambdas.
- **Keep coroutine ownership with Task.** Do not stash raw `std::coroutine_handle` inside arbitrary lambdas; always wrap them in `Task` so destruction and rescheduling remain centralized.

## Practical Guidance

- Prefer one `IOContext` per process; share it across handlers.
- Keep handler methods coroutine-based; avoid blocking or OS threads that bypass the scheduler.
- When you need concurrency, start additional coroutines (`IOContext::schedule(Task{...})`) instead of launching background threads with lambdas.
- If you must integrate with a callback-style library, immediately hop into the coroutine world via a small `Task` wrapper and let `IOContext` control resumption.

## Failure Modes When Mixing

- Coroutines resume on arbitrary threads, causing races against `pending_io_` spinlock or timer heap mutex.
- Continuations are dropped; `co_await` never resumes, appearing as a hang.
- A `Task` gets destroyed while a lambda still holds its raw handle, leading to `resume` on a destroyed frame (undefined behavior).
- Handler vtables are violated, so HTTP/WebSocket/TCP dispatch cannot call your handler at all.

Keep coroutine boundaries clean: once you start with `Task`, stay with coroutines and let the `IOContext` scheduler be the only component that moves work across threads.





