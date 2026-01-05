# Why Mutexes Instead of Atomics

This document explains why `libspaznet` uses mutexes in specific places instead of atomic operations, despite the library's emphasis on lock-free design.

## Overview

While `libspaznet` uses lock-free atomic operations extensively (for enqueue operations, I/O handle registration, statistics tracking), mutexes are used in three critical areas:

1. **TaskQueue dequeue operations** (`dequeue_mutex_`)
2. **Timer management** (`timer_mutex_`)
3. **Server listen file descriptor tracking** (`listen_fds_mutex_`)

## 1. TaskQueue Dequeue Mutex

### The Problem: Use-After-Free

```cpp
// Multi-producer enqueue (lock-free)
void enqueue(Task task) {
    Node* node = new Node(std::move(task));
    Node* prev_tail = tail_.exchange(node, std::memory_order_acq_rel);
    prev_tail->next.store(node, std::memory_order_release);
}

// Multi-consumer dequeue (protected by mutex)
auto dequeue(Task& task) -> bool {
    std::lock_guard<std::mutex> lock(dequeue_mutex_);
    
    Node* head = head_.load(std::memory_order_acquire);
    Node* next = head->next.load(std::memory_order_acquire);
    
    head_.store(next, std::memory_order_release);
    task = std::move(next->task);  // Read from node
    delete head;                    // Delete node
    return true;
}
```

### Why Atomics Aren't Sufficient

The dequeue operation requires **three sequential steps** that must be atomic as a group:

1. **Read** the node's task data
2. **Move** the task out of the node
3. **Delete** the node

**Race condition without mutex:**
```
Thread A: Reads head → Reads next → Reads task data
Thread B: Reads head → Reads next → Reads task data
Thread A: Updates head → Deletes old head node
Thread B: Tries to read from deleted node → USE-AFTER-FREE
```

Even if we use atomics for each individual step, we cannot atomically ensure that:
- No other thread is reading from a node while we delete it
- The node isn't deleted between our read and our use of its data

### Why Not Lock-Free?

A fully lock-free dequeue would require:
- **Hazard pointers**: Track which nodes are being accessed by each thread
- **Epoch-based reclamation**: Delay deletion until all threads have moved past the node
- **Reference counting**: More complex memory management

These techniques add significant complexity and overhead. The mutex approach is:
- **Simpler**: Easy to understand and verify correctness
- **Sufficient**: Dequeue is already single-consumer per queue (each worker thread has its own queue)
- **Low contention**: In practice, only one thread dequeues from each queue

### Performance Impact

The mutex is only held during the brief dequeue operation (a few memory operations). Since each worker thread has its own queue, there's minimal contention. The lock-free enqueue (which happens from multiple threads) remains fast.

## 2. Timer Management Mutex

### The Problem: Complex Data Structure Operations

```cpp
std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerCompare> timers_;
std::unordered_map<uint64_t, bool> cancelled_timers_;
std::unordered_map<void*, std::shared_ptr<Task>> suspended_tasks_;
std::mutex timer_mutex_;
```

### Why Atomics Aren't Sufficient

Timer operations require **multiple related data structures** to be updated atomically:

**Adding a timer:**
```cpp
{
    std::lock_guard<std::mutex> lock(timer_mutex_);
    // 1. Check if timers_ is empty or if this is the earliest timer
    if (timers_.empty() || first_fire < timers_.top().next_fire) {
        should_wake = true;
    }
    // 2. Add to suspended_tasks_ map
    suspended_tasks_[task_ptr->handle.address()] = task_ptr;
    // 3. Push to priority queue
    timers_.push(TimerEntry{...});
}
```

**Processing timers:**
```cpp
{
    std::lock_guard<std::mutex> lock(timer_mutex_);
    // 1. Check if timer is cancelled
    auto cancelled = cancelled_timers_.find(entry.id);
    // 2. Pop from priority queue
    timers_.pop();
    // 3. If repeating, push back with new time
    if (entry.repeat) {
        timers_.push(entry);
    }
    // 4. Remove from suspended_tasks_
    suspended_tasks_.erase(handle.address());
}
```

### Why Not Lock-Free?

These operations involve:
- **Priority queue operations**: `top()`, `pop()`, `push()` - not atomic operations
- **Hash map operations**: `find()`, `insert()`, `erase()` - complex internal state
- **Multi-step transactions**: Need to check multiple data structures together

Making these lock-free would require:
- **Lock-free priority queue**: Extremely complex, typically uses skip lists or other complex structures
- **Lock-free hash maps**: Possible but complex, requires careful memory reclamation
- **Transaction-like semantics**: Ensuring all related updates happen atomically

The mutex ensures:
- **Atomicity**: All related operations happen together
- **Consistency**: Data structures remain in valid states
- **Simplicity**: Standard library containers work as-is

### Performance Impact

Timer operations are relatively infrequent compared to I/O operations:
- Timers are added/removed on coroutine suspension/resumption
- Timer processing happens in the main event loop, not in hot paths
- The mutex is held for short durations (just the data structure updates)

## 3. Server Listen FDs Mutex

### The Problem: Vector Operations During Shutdown

```cpp
std::vector<int> listen_fds_;
std::mutex listen_fds_mutex_;
```

### Why Atomics Aren't Sufficient

The `listen_fds_` vector is accessed in multiple scenarios:

**Adding a listen socket:**
```cpp
{
    std::lock_guard<std::mutex> lock(listen_fds_mutex_);
    listen_fds_.push_back(listen_fd);
}
```

**Stopping the server (from destructor or stop()):**
```cpp
{
    std::lock_guard<std::mutex> lock(listen_fds_mutex_);
    for (int fd : listen_fds_) {
        close_socket(fd);
    }
    listen_fds_.clear();
}
```

### Why Not Lock-Free?

`std::vector` operations are not thread-safe:
- **`push_back()`**: May reallocate, invalidating iterators
- **Iteration**: Reading while another thread modifies is undefined behavior
- **`clear()`**: Must be synchronized with concurrent access

A lock-free approach would require:
- **Lock-free vector**: Complex, typically uses segmented arrays or other structures
- **Atomic reference counting**: For safe iteration during modification
- **RCU (Read-Copy-Update)**: For safe concurrent reads and writes

### Performance Impact

This mutex is only used for:
- **Server setup**: When adding listen sockets (rare, happens at startup)
- **Server shutdown**: When closing all sockets (rare, happens at shutdown)

These are not hot paths, so the mutex overhead is negligible.

## General Principles

### When to Use Atomics

Use atomics for:
- **Single-word operations**: Reading/writing a single value
- **Simple compare-and-swap**: Updating a pointer or counter
- **Hot paths**: Operations that happen frequently (I/O registration, statistics)

### When to Use Mutexes

Use mutexes for:
- **Multi-step operations**: Operations that must be atomic as a group
- **Complex data structures**: Standard library containers that aren't thread-safe
- **Memory safety**: Preventing use-after-free or data races on non-atomic types
- **Infrequent operations**: Where lock-free complexity isn't justified

### Design Philosophy

`libspaznet` uses a **hybrid approach**:
- **Lock-free where it matters**: Enqueue operations, I/O handle registration (hot paths)
- **Mutexes where necessary**: Complex operations, data structure management (cold paths)

This balances:
- **Performance**: Lock-free operations in hot paths
- **Correctness**: Mutexes ensure safety for complex operations
- **Simplicity**: Avoiding overly complex lock-free algorithms where not needed

## Summary

| Location | Why Mutex? | Why Not Atomic? |
|----------|-----------|-----------------|
| `TaskQueue::dequeue` | Prevents use-after-free when deleting nodes | Can't atomically ensure no readers during deletion |
| `IOContext::timer_mutex_` | Protects priority queue + hash maps | Complex multi-step operations on non-atomic structures |
| `Server::listen_fds_mutex_` | Protects vector during iteration/modification | `std::vector` operations aren't thread-safe |

The key insight: **atomics work for simple operations, but mutexes are needed when you must ensure multiple related operations happen atomically together, or when working with complex data structures that aren't designed for lock-free access.**

