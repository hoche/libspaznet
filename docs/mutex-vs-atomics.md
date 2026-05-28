# Why Mutexes Instead of Atomics

This document explains why `libspaznet` uses mutexes in specific places
instead of atomic operations. The library's design leans heavily on
`std::atomic<…>` for cross-thread state, but five locking primitives
remain. This is the rationale for each.

## Inventory

| Location | Primitive | Reason it can't be atomics |
|---|---|---|
| `TaskQueue::mutex_` | `std::mutex` | Linked-list node lifetime: a lock-free queue would require hazard pointers or epoch-based reclamation to free dequeued nodes safely. |
| `IOContext::timer_mutex_` | `std::mutex` | A min-heap plus two associative containers that must be mutated as a single transaction. |
| `IOContext::map_lock_` | `std::atomic_flag` spinlock | Brief structural guard for the `pending_io_` map; held across an `add_fd` / `modify_fd` call so a rehash can't invalidate the in-use entry. |
| `Server::listen_fds_mutex_` | `std::mutex` | `std::vector` is not thread-safe; iterate-and-close on `stop()` must serialize with `listen_tcp()` appends. |
| `Server::client_fds_mutex_` | `std::mutex` | `std::unordered_set` is not thread-safe; concurrent insert/erase by `handle_connection` coroutines must serialize with `Server::stop()`'s walk-and-shutdown sweep. |

Plus one `std::once_flag` in the Windows-only WSAStartup helper, which
fires at most once per process.

## 1. TaskQueue — both ends, single mutex

```cpp
class TaskQueue {
    Node* head_;
    Node* tail_;
    mutable std::mutex mutex_;

    void enqueue(Task task) {
        Node* node = new Node(std::move(task));
        std::lock_guard<std::mutex> lock(mutex_);
        tail_->next = node;
        tail_ = node;
    }

    bool dequeue(Task& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        Node* head = head_;
        Node* next = head->next;
        if (next == nullptr) return false;
        head_ = next;
        task = std::move(next->task);
        delete head;
        return true;
    }
};
```

### Why a single mutex on both ends

An earlier design used a lock-free Michael-Scott enqueue alongside a
mutex-protected dequeue:

```cpp
// Old enqueue — looked lock-free, wasn't actually safe.
Node* prev_tail = tail_.exchange(node, acq_rel);
prev_tail->next.store(node, release);
```

Between the `exchange` and the next `store`, a concurrent dequeue (under
the dequeue mutex) could observe `prev_tail` as the head, advance past
it, and free it. The subsequent `prev_tail->next.store()` was then a
use-after-free on a node the allocator had often already recycled.
Multi-consumer workloads turned this into freelist corruption that
crashed minutes later in unrelated allocations.

The Michael-Scott invariants cannot be restored without **hazard
pointers** or **epoch-based reclamation** — both substantial machinery
to maintain correctly inside a coroutine scheduler. The pragmatic
alternative is a single `std::mutex` covering both ends. The queue is
no longer lock-free in the API sense, but on the contended paths the
original `atomic_exchange` traffic already serialized through cache
coherence, so the measured throughput delta on the audit's bench was
in the noise. If the lock-free property ever becomes a measured
bottleneck, the right move is a battle-tested external queue
(e.g. moodycamel::ConcurrentQueue), not reimplementing
MS+hazard-pointers in-tree.

## 2. Timer management mutex

```cpp
std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCompare> timers_;
std::unordered_set<uint64_t> cancelled_timers_;
std::unordered_map<void*, std::shared_ptr<Task>> suspended_tasks_;
std::mutex timer_mutex_;
```

Every timer operation touches at least two of these containers as a
single transaction:

* **Add a timer:** check whether the new entry is now the earliest
  (so we can wake the event loop), insert into the suspended-tasks
  map keyed by handle, push onto the heap.
* **Fire a timer:** pop the heap, check the cancelled set, re-push
  if repeating, erase from the suspended-tasks map.

`std::priority_queue::push`/`pop` and `std::unordered_map::insert`/
`erase` aren't atomic, and the relationship between the heap and the
side tables only makes sense if all three are observed at the same
instant. A lock-free design would need an MPMC priority queue plus
two lock-free hash maps with consistent visibility — far more
complexity than the timer path's traffic justifies. The mutex is held
for the bounded duration of those container ops, on a path that fires
once per timer expiry rather than per I/O event.

## 3. `pending_io_` spinlock

```cpp
std::unordered_map<int, PendingIO> pending_io_;
mutable std::atomic_flag map_lock_ = ATOMIC_FLAG_INIT;
```

The hottest of the five locks. Held for:

* `register_io`: bump the entry's generation counter, copy the
  caller's coroutine handle into `PendingIO::read` / `write`, and
  call into the platform layer (`add_fd` / `modify_fd`) with the
  packed `(generation, fd)` token. Held across the platform call so
  a rehash cannot invalidate the entry while the kernel is reading
  its `user_data` pointer.
* `remove_io`: drop the entry.
* `process_io_events`: per-event, decode the token's generation,
  look up the entry, check the generation, transfer the suspended
  handle into the local schedule list, and (if interest changed)
  re-issue a `modify_fd` with the same token.

A spinlock rather than a `std::mutex` because every critical section
is bounded by a handful of map operations and (sometimes) one syscall;
sleeping for that would be more expensive than spinning, and the
contention domain is "one event-loop thread plus the schedulers that
just woke up on workers", not "every coroutine in the process".

## 4. & 5. Server's two fd containers

```cpp
std::mutex listen_fds_mutex_;
std::vector<int> listen_fds_;
std::mutex client_fds_mutex_;
std::unordered_set<int> active_client_fds_;
```

Both containers are standard-library types that aren't thread-safe.
The locks are taken on:

* `listen_tcp()` (rare; once per port at startup) and `stop()`'s
  swap-and-close sweep (once at shutdown).
* `handle_connection` entry/exit through the `ConnectionGuard` RAII
  helper (per accepted client), and `Server::stop()`'s walk over
  every active client to `shutdown(SHUT_RDWR)` it.

The connection-side traffic on `client_fds_mutex_` is the only one
that scales with request rate. It's held briefly (a single
`insert` or `erase`) and never across blocking I/O, so even under
load it doesn't appear in profiles.

## General principles

### When atomics are enough

* A single read or write of a single word.
* A simple compare-and-swap on a pointer or counter.
* Per-element flags where adjacent elements don't need a consistent
  view (statistics counters, the `running_` flag, the
  `active_connections_` count).

### When you need a lock

* You're mutating a non-atomic container (`std::vector`,
  `std::unordered_map`, `std::priority_queue`).
* You're freeing memory another thread might still be reading.
* You're updating two or more pieces of state that must be observed
  together.
* You're holding state across a syscall that needs to see a
  consistent snapshot.

`libspaznet`'s five locks each fall into one of those categories. The
remaining cross-thread state — coroutine ref-counts, the per-fd
generation counter, the statistics block, ids, flags — is all
`std::atomic<…>` and never touches a mutex.
