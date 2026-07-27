---
status: active
audience: public
last-verified: 2026-07-26
---

# Threads

> Threads, async composition, synchronization primitives, and concurrent containers.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Threads.Thread](#zannathreadsthread)
- [Zanna.Threads.Monitor](#zannathreadsmonitor)
- [Zanna.Threads.SafeI64](#zannathreadssafei64)
- [Zanna.Threads.Gate](#zannathreadsgate)
- [Zanna.Threads.Barrier](#zannathreadsbarrier)
- [Zanna.Threads.RwLock](#zannathreadsrwlock)
- [Zanna.Threads.Pool](#zannathreadspool)
- [Zanna.Threads.Promise](#zannathreadspromise)
- [Zanna.Threads.Future](#zannathreadsfuture)
- [Zanna.Threads.Parallel](#zannathreadsparallel)
- [Zanna.Threads.CancelToken](#zannathreadscanceltoken)
- [Zanna.Threads.Debouncer](#zannathreadsdebouncer)
- [Zanna.Threads.Throttler](#zannathreadsthrottler)
- [Zanna.Threads.Scheduler](#zannathreadsscheduler)
- [Zanna.Threads.Async](#zannathreadsasync)
- [Zanna.Threads.ConcurrentMap](#zannathreadsconcurrentmap)
- [Zanna.Threads.ConcurrentQueue](#zannathreadsconcurrentqueue)
- [Zanna.Threads.Channel](#zannathreadschannel)

---

## Zanna.Threads.Thread

OS threads for Zanna programs (VM and native backends).

**Type:** Instance class (created by `Start`)

### Methods

| Method                    | Signature                       | Description                                |
|--------------------------|----------------------------------|--------------------------------------------|
| `Join()`                 | `Void()`                         | Wait until the thread finishes             |
| `JoinFor(ms)`            | `Boolean(Integer)`               | Join with timeout in milliseconds          |
| `SafeGetId()`            | `Integer()`                      | Get the ID of a safe thread handle         |
| `SafeIsAlive()`          | `Boolean()`                      | Check if a safe thread is still running    |
| `SafeJoin()`             | `Void()`                         | Join a safe thread handle                  |
| `Sleep(ms)`              | `Void(Integer)`                  | Sleep the current thread (ms, clamped to `0..2147483647`) |
| `Start(entry, arg)`      | `Thread(Function, Object)`       | Start a new thread with a managed callback reference |
| `StartOwned(entry, arg)` | `Thread(Function, Object)`       | Start a new thread and retain a runtime object argument until the entry returns |
| `StartSafe(entry, arg)`  | `Thread(Function, Object)`       | Start a bridged Zia thread with error boundaries; traps are captured instead of crashing |
| `StartSafeOwned(entry, arg)` | `Thread(Function, Object)`    | Safe-thread variant of `StartOwned`        |
| `TryJoin()`              | `Boolean()`                      | Non-blocking join attempt                  |
| `Yield()`                | `Void()`                         | Yield the current thread's time slice      |

### Properties

| Property   | Type                  | Description                            |
|------------|-----------------------|----------------------------------------|
| `Id`       | `Integer` (read-only) | Monotonic thread id (1, 2, 3, …)       |
| `IsAlive`  | `Boolean` (read-only) | True while entry function is running   |
| `HasError` | `Boolean` (read-only) | True if a safe thread exited with a trap (only for `StartSafe` threads) |
| `Error`    | `String` (read-only)  | Error message if the safe thread trapped (empty otherwise) |

### Entry Function

`Start(entry, arg)` expects a frontend function reference. Internally the runtime accepts one of these low-level IL signatures:

- `void()` (no args), or
- `void(ptr)` (one `ptr` argument).

In IL, obtain an entry handle via `addr_of`:

```text
func @worker(ptr %arg) -> void { ... }

func @main() -> i64 {
entry:
  %fn = addr_of @worker
  %t  = call @Zanna.Threads.Thread.Start(%fn, const_null)
  call @Zanna.Threads.Thread.Join(%t)
  ret 0
}
```

**Backend notes:**

- **Native:** `entry` is a backend callback handle with C ABI `void (*)(void *)`.
- **VM / BytecodeVM:** `entry` is a managed function reference and is invoked by a per-thread VM runner.
- `Start` and `StartSafe` do not retain `arg`; use `StartOwned` / `StartSafeOwned` when `arg` is a runtime-managed object or string handle that should stay alive until the callback returns.

### Native Stack-Overflow Protection

Native runtime threads initialize stack-safety support before executing user
work. On POSIX systems, alternate signal stacks are thread-specific: each
thread without one receives distinct thread-local storage, while a stack
already installed by a sanitizer, embedding host, debugger, or crash reporter
is preserved. Process-wide fatal-signal handlers are installed transactionally
at most once and do not replace an existing non-default handler. This prevents
one thread from unregistering or unmapping another thread's signal stack during
teardown.

Windows uses one process-wide vectored exception handler and CRT-initialized
threads. Handler initialization is safe under concurrent first use on every
supported platform. Low-stack diagnostics use platform primitives that do not
allocate or depend on formatted I/O; stack overflow remains terminal rather
than attempting unsafe recovery.

### Runtime Context Inheritance

A native runtime thread inherits the caller's active VM context, or the shared legacy context when
the caller is unbound. The runtime reserves that context before the OS thread is created, so the
parent may unbind after `Start` returns without opening a gap in which context cleanup can race the
child trampoline. Native-create failure cancels the reservation; normal child exit releases it.

Inherited threads share the context's RNG, module variables, argument store, BASIC file channels,
and type registry. Those mutable state families are synchronized independently. Cleanup is rejected
while any parent, child, or pre-start reservation remains bound. A caller-owned `RtContext` that has
been cleaned cannot be rebound until it is initialized again.

In safe Zia, use a function reference and a typed callback parameter:

```zia
bind Zanna.Terminal;

func worker(arg: Any) {
    // ...
}

var t = Zanna.Threads.Thread.StartSafe(&worker, 0);
```

The Zia frontend lowers `&worker` to the backend-specific function handle and boxes ordinary arguments as needed. Source code should not declare `Ptr`; callback handles are managed by the runtime bridge.

### Safe Threads (Error Boundaries)

`StartSafe(entry, arg)` is like `Start` but wraps the thread entry in a trap recovery context
using `setjmp`/`longjmp`. If the entry function triggers a trap (null dereference, bounds check
failure, etc.), the trap is captured instead of crashing the process.

This applies uniformly to VM-backed and BytecodeVM-backed worker functions. A
trapped worker marks the safe thread handle as failed instead of silently
completing. VM and BytecodeVM workers route both runtime-call traps and
interpreter traps through the same safe-thread boundary.

After the thread finishes, check `HasError` and `Error` on the returned handle:

```zia
bind Zanna.Terminal;

func worker(arg: Any) {
    // Perform fallible work here.
}

var t = Zanna.Threads.Thread.StartSafe(&worker, 0);
t.Join();
if (t.HasError) {
    Say("Thread trapped: " + t.Error);
} else {
    Say("Thread completed normally");
}
```

`SafeJoin()`, `SafeGetId()`, and `SafeIsAlive()` remain available for explicit
safe-thread handling. Standard `Join()`, `TryJoin()`, `JoinFor()`, `Id`, and
`IsAlive` also accept safe-thread handles and delegate to the underlying worker.
The safe-specific methods also accept regular thread handles.

**Use cases:**
- Database servers: isolate per-connection errors so one bad query doesn't crash all sessions
- Plugin systems: run untrusted code without risking process stability
- Test runners: run tests in threads and capture failures

### Join Timeouts

`JoinFor(ms)` behaves as:

| `ms` value | Behavior                         |
|------------|----------------------------------|
| `< 0`      | Wait indefinitely (same as Join) |
| `= 0`      | Immediate check (same as TryJoin)|
| `> 0`      | Wait up to `ms` milliseconds     |

`Join()`, `TryJoin()`, and `JoinFor()` are repeatable after the thread has
finished; there is no one-time "joined" state. POSIX workers are detached when
created, so their native resources are reclaimed when they exit whether or not
`Join()` is called. On Windows, the native thread handle remains open until the
runtime Thread wrapper is finalized. Query-only properties such as `Id`,
`IsAlive`, `HasError`, and `Error` remain usable after a successful join.

At the native C ABI layer, a successful join publishes entry-callback and
inherited-context completion. A detached worker may still be dropping its one
internal lifecycle reference immediately after waking joiners. Releasing the
caller's Thread reference remains safe in that interval; reclamation completes
when the worker drops the transient reference.

On Windows only, a positive `JoinFor` timeout is currently capped to one Win32
wait interval (`4294967295` ms, about 49.7 days). A larger request can therefore
return false before the full requested duration; POSIX uses the full 64-bit
duration. This cross-platform discrepancy is tracked in the review findings.

### Errors (Traps)

- `Thread.Start: null entry`
- `Thread.Start: failed to create thread`
- `Thread.Join: null thread`
- `Thread.Join: cannot join self`

### Zia Example

```zia
module ThreadDemo;

bind Zanna.Threads;

func worker(arg: Any) {
}

func start() {
    var thread = Zanna.Threads.Thread.StartSafe(&worker, 0);
    thread.SafeJoin();
}
```

### BASIC Example

```basic
' Sleep the current thread briefly
Zanna.Threads.Thread.Sleep(10)
PRINT "Slept for 10ms"

' Yield the current thread's time slice
Zanna.Threads.Thread.Yield()
PRINT "Yielded time slice"
```

---

## Zanna.Threads.Monitor

FIFO-fair, re-entrant monitor for explicit object locking.

**Type:** Static utility class

### Methods

| Method                      | Signature                    | Description                                         |
|----------------------------|------------------------------|-----------------------------------------------------|
| `Enter(obj)`               | `Void(Object)`               | Acquire monitor (blocks, FIFO)                      |
| `Exit(obj)`                | `Void(Object)`               | Release monitor (balances `Enter`)                  |
| `Notify(obj)`              | `Void(Object)`               | Wake one waiter (FIFO)                              |
| `NotifyAll(obj)`           | `Void(Object)`               | Wake all waiters (FIFO order, then FIFO re-acquire) |
| `TryEnter(obj)`            | `Boolean(Object)`            | Try to acquire immediately                          |
| `TryEnterFor(obj, ms)`     | `Boolean(Object, Integer)`   | Try to acquire with timeout (ms)                    |
| `Wait(obj)`                | `Void(Object)`               | Release monitor and wait for `Notify`/`NotifyAll`   |
| `WaitFor(obj, ms)`         | `Boolean(Object, Integer)`   | Wait with timeout (returns false on timeout)        |

### Notes

- **Re-entrant:** The owning thread may `Enter` multiple times; recursion is tracked and must be matched by `Exit`.
- **FIFO:** Contended acquisition is FIFO; waiters awakened by `Notify`/`NotifyAll` are moved to acquire in FIFO order.
- **Ownership required:** `Exit`, `Wait`, `WaitFor`, `Notify`, and `NotifyAll` trap if the calling thread is not the owner.
- **Timeout units:** milliseconds. `TryEnterFor`/`WaitFor` treat negative values as `0` (immediate).
- **WaitFor always re-acquires:** even on timeout, `WaitFor` re-acquires the monitor before returning.
- **Independent lookup stripes:** lazily attached object monitors are indexed
  behind 64 table-lock stripes, so first use or finalization of unrelated
  objects no longer contends on one process-wide monitor-table mutex.
- **Native failures are explicit:** table, monitor, waiter, mutex, and condition
  initialization failures trap before partial state is published. Unexpected
  wait or wake failures also trap; they are not treated as a timeout or a
  successful notification.

### Errors (Traps)

- `Monitor.Enter: null object`
- `Monitor.Exit: null object`
- `Monitor.Exit: not owner`
- `Monitor.Wait: not owner`
- `Monitor.Notify: not owner`
- `Monitor.NotifyAll: not owner`

### Zia Example

> Monitor is a static utility class that operates on any object. Use `bind Zanna.Threads.Monitor as Monitor;` and call `Monitor.Enter(obj)` / `Monitor.Exit(obj)`. Typically used with threads for synchronization.

### BASIC Example

```basic
' Create a simple object to use as monitor target
DIM obj AS OBJECT = Zanna.Collections.Seq.New()

' Enter (acquire monitor)
Zanna.Threads.Monitor.Enter(obj)
PRINT "Monitor acquired"

' TryEnter (re-entrant, same thread owns it)
DIM ok AS INTEGER = Zanna.Threads.Monitor.TryEnter(obj)
PRINT "TryEnter: "; ok

' TryEnterFor (re-entrant with timeout)
DIM ok2 AS INTEGER = Zanna.Threads.Monitor.TryEnterFor(obj, 10)
PRINT "TryEnterFor: "; ok2

' Exit to balance all three enters
Zanna.Threads.Monitor.Exit(obj)
Zanna.Threads.Monitor.Exit(obj)
Zanna.Threads.Monitor.Exit(obj)
```

---

## Zanna.Threads.SafeI64

FIFO-serialized “safe variable” for shared counters and flags.

**Type:** Instance class (requires `New(initial)`)

### Constructor

| Method        | Signature          | Description                  |
|---------------|--------------------|------------------------------|
| `New(initial)`| `SafeI64(Integer)` | Create a safe integer cell   |

### Methods

| Method                               | Signature                             | Description                                    |
|--------------------------------------|----------------------------------------|------------------------------------------------|
| `Add(delta)`                         | `Integer(Integer)`                     | Add and return new value                        |
| `CompareExchange(expected, desired)` | `Integer(Integer, Integer)`            | CAS; returns the value read before any update   |
| `Get()`                              | `Integer()`                            | Read current value                              |
| `Set(value)`                         | `Void(Integer)`                        | Write value                                     |

### Notes

- Operations are implemented by acquiring a FIFO monitor on the `SafeI64` object itself.
- You can also lock a `SafeI64` explicitly via `Monitor.Enter(cell)` when you need to group multiple operations.
- `Add` uses two's-complement wraparound on overflow instead of trapping.

### Errors (Traps)

- `SafeI64.New: alloc failed`
- `SafeI64.Get: null object`
- `SafeI64.Set: null object`
- `SafeI64.Add: null object`
- `SafeI64.CompareExchange: null object`

### Zia Example

```zia
module SafeI64Demo;

bind Zanna.Terminal;
bind Zanna.Threads.SafeI64 as SafeI64;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var counter = SafeI64.New(0);
    Say("Initial: " + Fmt.Int(counter.Get()));

    counter.Set(42);
    Say("After Set: " + Fmt.Int(counter.Get()));

    var newVal = counter.Add(8);
    Say("After Add(8): " + Fmt.Int(newVal));    // 50

    var old = counter.CompareExchange(50, 100);
    Say("CAS old: " + Fmt.Int(old));            // 50
    Say("CAS result: " + Fmt.Int(counter.Get())); // 100
}
```

### BASIC Example

```basic
DIM counter AS OBJECT = Zanna.Threads.SafeI64.New(0)
PRINT "Initial: "; counter.Get()

counter.Set(42)
PRINT "After Set: "; counter.Get()

DIM newVal AS INTEGER = counter.Add(8)
PRINT "After Add(8): "; newVal

DIM old AS INTEGER = counter.CompareExchange(50, 100)
PRINT "CAS old: "; old
PRINT "CAS result: "; counter.Get()
```

---

## Zanna.Threads.Gate

FIFO-fair permit gate (semaphore concept).

**Type:** Instance class (requires `New(permits)`)

### Constructor

| Method         | Signature       | Description                            |
|---------------|------------------|----------------------------------------|
| `New(permits)` | `Gate(Integer)`  | Create a gate with `permits` available |

### Methods

| Method              | Signature            | Description                                          |
|--------------------|----------------------|------------------------------------------------------|
| `Enter()`          | `Void()`             | Acquire one permit (blocks, FIFO)                    |
| `Leave()`          | `Void()`             | Release one permit; wakes one waiter if present      |
| `Leave(count)`     | `Void(Integer)`      | Release `count` permits; wakes up to `count` waiters |
| `TryEnter()`       | `Boolean()`          | Try to acquire immediately                           |
| `TryEnterFor(ms)`  | `Boolean(Integer)`   | Try to acquire with timeout in milliseconds          |

### Properties

| Property    | Type                  | Description                    |
|-------------|-----------------------|--------------------------------|
| `Permits`   | `Integer` (read-only) | Current available permit count |

### Notes

- **FIFO:** Contended `Enter`/`TryEnterFor` acquisition is FIFO-fair.
- **Timeout units:** milliseconds. `TryEnterFor` treats negative values as `0` (immediate).
- `Leave` traps instead of wrapping if the permit count would overflow.

### Errors (Traps)

- `Gate.New: permits cannot be negative`
- `Gate.Enter: null object`
- `Gate.TryEnter: null object`
- `Gate.TryEnterFor: null object`
- `Gate.Leave: null object`
- `Gate.Leave: count cannot be negative`
- `Gate.Leave: permit count overflow`

### Zia Example

```zia
module GateDemo;

bind Zanna.Terminal;
bind Zanna.Threads.Gate as Gate;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var gate = Gate.New(3);
    Say("Permits: " + Fmt.Int(gate.get_Permits()));    // 3

    gate.Enter();
    Say("After Enter: " + Fmt.Int(gate.get_Permits())); // 2

    Say("TryEnter: " + Fmt.Bool(gate.TryEnter()));      // true
    Say("Permits: " + Fmt.Int(gate.get_Permits()));      // 1

    gate.Leave();
    gate.Leave();
}
```

> **Note:** Gate properties (`Permits`) use the get_/set_ pattern; access as `gate.get_Permits()` in Zia.

### BASIC Example

```basic
DIM gate AS OBJECT = Zanna.Threads.Gate.New(3)
PRINT "Permits: "; gate.Permits

gate.Enter()
PRINT "After Enter: "; gate.Permits

PRINT "TryEnter: "; gate.TryEnter()
PRINT "Permits: "; gate.Permits

gate.Leave()
gate.Leave()
```

---

## Zanna.Threads.Barrier

Reusable N-party barrier.

**Type:** Instance class (requires `New(parties)`)

### Constructor

| Method         | Signature          | Description                                 |
|---------------|--------------------|---------------------------------------------|
| `New(parties)` | `Barrier(Integer)` | Create a barrier for `parties` participants |

### Methods

| Method      | Signature     | Description                                                  |
|-------------|---------------|--------------------------------------------------------------|
| `Arrive()`  | `Integer()`   | Arrive and wait; returns arrival index `0..Parties-1`        |
| `Reset()`   | `Void()`      | Reset for reuse (traps if any threads are currently waiting) |

### Properties

| Property    | Type                  | Description              |
|-------------|-----------------------|--------------------------|
| `Parties`   | `Integer` (read-only) | Total parties required   |
| `Waiting`   | `Integer` (read-only) | Number currently waiting |

### Notes

- When all parties arrive, all are released simultaneously and the barrier resets.

### Errors (Traps)

- `Barrier.New: parties must be >= 1`
- `Barrier.Arrive: null object`
- `Barrier.Reset: null object`
- `Barrier.Reset: threads are waiting`

### Zia Example

```zia
module BarrierDemo;

bind Zanna.Terminal;
bind Zanna.Threads.Barrier as Barrier;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var b = Barrier.New(3);
    Say("Parties: " + Fmt.Int(b.get_Parties()));  // 3
    Say("Waiting: " + Fmt.Int(b.get_Waiting()));  // 0
    // In a real program, multiple threads would call b.Arrive()
}
```

### BASIC Example

```basic
DIM b AS OBJECT = Zanna.Threads.Barrier.New(3)
PRINT "Parties: "; b.Parties
PRINT "Waiting: "; b.Waiting

' With a single-party barrier we can demonstrate Arrive
DIM b1 AS OBJECT = Zanna.Threads.Barrier.New(1)
DIM idx AS INTEGER = b1.Arrive()
PRINT "Arrive index: "; idx
```

---

## Zanna.Threads.RwLock

Writer-preference reader-writer lock.

**Type:** Instance class (requires `New()`)

### Constructor

| Method  | Signature  | Description       |
|---------|------------|-------------------|
| `New()` | `RwLock()` | Create a new lock |

### Methods

| Method             | Signature     | Description                                             |
|-------------------|---------------|---------------------------------------------------------|
| `ReadEnter()`      | `Void()`      | Acquire shared read lock (blocks on writer)             |
| `ReadExit()`       | `Void()`      | Release read lock                                       |
| `TryReadEnter()`   | `Boolean()`   | Non-blocking read acquire                               |
| `TryWriteEnter()`  | `Boolean()`   | Non-blocking write acquire                              |
| `WriteEnter()`     | `Void()`      | Acquire exclusive write lock (blocks on readers/writer) |
| `WriteExit()`      | `Void()`      | Release write lock                                      |

### Properties

| Property         | Type                   | Description              |
|------------------|------------------------|--------------------------|
| `Readers`        | `Integer` (read-only)  | Count of active readers  |
| `IsWriteLocked`  | `Boolean` (read-only)  | True if write lock held  |

### Notes

- **Writer preference:** new readers block while any writer is waiting.
- `ReadExit` must be called by the same thread that acquired the read lock.
- Write acquisition is recursive for the owning thread. Every successful
  `WriteEnter` or `TryWriteEnter` requires a matching `WriteExit`.
- Read-to-write upgrades are not supported: `WriteEnter` traps while the current thread holds a read lock, and `TryWriteEnter` returns false.
- A thread that already owns the write lock may also enter the read side; this supports explicit write-to-read downgrade patterns when the matching `ReadExit` and `WriteExit` calls are balanced.

### Errors (Traps)

- `RwLock.ReadEnter: null object`
- `RwLock.ReadExit: null object`
- `RwLock.ReadExit: exit without matching enter`
- `RwLock.WriteEnter: null object`
- `RwLock.WriteEnter: cannot upgrade read lock`
- `RwLock.WriteExit: null object`
- `RwLock.WriteExit: exit without matching enter`
- `RwLock.WriteExit: not owner`

### Zia Example

```zia
module RwLockDemo;

bind Zanna.Terminal;
bind Zanna.Threads.RwLock as RwLock;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var lock = RwLock.New();

    // Read lock (shared)
    lock.ReadEnter();
    Say("Readers: " + Fmt.Int(lock.get_Readers()));  // 1
    lock.ReadExit();

    // Write lock (exclusive)
    lock.WriteEnter();
    Say("IsWriteLocked: " + Fmt.Bool(lock.get_IsWriteLocked()));  // true
    lock.WriteExit();

    // Non-blocking try variants
    Say("TryRead: " + Fmt.Bool(lock.TryReadEnter()));   // true
    lock.ReadExit();
    Say("TryWrite: " + Fmt.Bool(lock.TryWriteEnter())); // true
    lock.WriteExit();
}
```

### BASIC Example

```basic
DIM lock AS OBJECT = Zanna.Threads.RwLock.New()

' Read lock (shared)
lock.ReadEnter()
PRINT "Readers: "; lock.Readers
lock.ReadExit()

' Write lock (exclusive)
lock.WriteEnter()
PRINT "IsWriteLocked: "; lock.IsWriteLocked
lock.WriteExit()

' Non-blocking try variants
PRINT "TryRead: "; lock.TryReadEnter()
lock.ReadExit()
PRINT "TryWrite: "; lock.TryWriteEnter()
lock.WriteExit()
```

---

## Zanna.Threads.Pool

Thread pool for submitting tasks to a fixed set of worker threads.

**Type:** Instance class (requires `New(size)`)

### Constructor

| Method       | Signature       | Description                                  |
|-------------|------------------|----------------------------------------------|
| `New(size)` | `Pool(Integer)`  | Create a pool with `size` worker threads (clamped to 1..1024) |

### Methods

| Method           | Signature             | Description                                                     |
|------------------|-----------------------|-----------------------------------------------------------------|
| `Submit(cb, arg)`| `Boolean(Function, Object)` | Submit a callback task; false on invalid input, shutdown, queue backpressure, or allocation failure |
| `SubmitOwned(cb, arg)`| `Boolean(Function, Object)` | Like `Submit`, but the pool retains `arg` until the task runs or is discarded |
| `Wait()`         | `Void()`              | Block until all pending tasks complete                          |
| `WaitFor(ms)`    | `Boolean(Integer)`    | Wait with timeout; returns true if all tasks completed          |
| `Shutdown()`     | `Void()`              | Graceful shutdown: finish pending tasks, then stop workers      |
| `ShutdownNow()`  | `Void()`              | Discard queued tasks, then wait for already-running tasks and workers |

### Properties

| Property     | Type                   | Description                         |
|--------------|------------------------|-------------------------------------|
| `Size`       | `Integer` (read-only)  | Number of worker threads            |
| `Pending`    | `Integer` (read-only)  | Number of queued (not yet running) tasks |
| `Active`     | `Integer` (read-only)  | Number of tasks currently executing |
| `IsShutdown` | `Boolean` (read-only)  | True if the pool has been shut down |

### Notes

- Tasks are executed in FIFO order.
- A native pool accepts at most 65,536 queued (not yet running) tasks by default.
  A positive integer in `ZANNA_THREADPOOL_MAX_PENDING` replaces that limit for
  pools constructed afterward. `Submit` returns false at the limit as well as
  after `Shutdown` or `ShutdownNow`.
- `ShutdownNow` discards queued tasks but cannot interrupt callbacks that have
  already started; it joins the workers after those callbacks return. `Shutdown`
  drains both queued and active work.
- Pool construction is transactional: if a monitor or worker cannot be created,
  already-started workers are stopped and joined and the partial Pool does not
  escape. Concurrent shutdown callers elect one native join/cleanup owner;
  other callers wait for or observe the same completed shutdown instead of
  returning while workers are still live.
- Calling `Wait`, `WaitFor`, `Shutdown`, or `ShutdownNow` from a worker in the same pool traps to prevent self-deadlock.
- Traps raised by a task do not leave the pool stuck in an active state. Once the pool drains, the next `Wait()`, successful `WaitFor(ms)`, `Shutdown()`, or `ShutdownNow()` rethrows the last task trap and clears it; later calls report the current pool state normally unless another task traps.
- `WaitFor(ms <= 0)` is an immediate drain-state check.
- On the native backend, `Submit` borrows `arg`: keep runtime-managed
  arguments alive until their callback finishes. `SubmitOwned` retains the
  runtime-managed argument on acceptance and releases it after the callback
  runs — or when the task is discarded by `ShutdownNow`/finalization — so
  owned arguments cannot leak through discarded tasks.
- Pool handles own their worker thread handles and release them after joins. Releasing a pool from one of its own workers requests shutdown and defers reclamation rather than freeing state out from under the running worker.
- Accepted `SubmitOwned` arguments and queued task records are exposed to the
  managed cycle collector. A final reference released by one of the pool's own
  workers transfers cleanup responsibility to a surviving worker, including
  the allocation-failure fallback path, so self-join is avoided without leaking
  the pool or its queued owned arguments.
- `Pool.Submit(pool, callback, arg)` accepts a managed Zia function reference such as `&worker`.
  The runtime owns the native callback adaptation internally. The current BASIC frontend supports
  `ADDRESSOF` with direct callback parameters such as `Thread.Start` and `Parallel.For`, but does
  not yet lower the callback parameter of `Pool.Submit`; submit pool work from Zia or the IL/C ABI.
- Native runtime adapters should call the typed C entry points
  `rt_threadpool_submit_fn`/`rt_threadpool_submit_owned_fn`. They preserve the real
  `void (*)(void *)` callback type; the legacy `void *` callback entry points remain for generated
  code and compatibility.

**VM behavior:** the standard VM and BytecodeVM do not enqueue `Pool.Submit`
work. They invoke the callback synchronously on the submitting thread and report
success, without consulting pool shutdown state or the native pending-task
limit. A callback trap therefore propagates from `Submit` immediately rather
than being deferred to `Wait`. Use a native build when actual pool concurrency
or native submission/backpressure behavior is required.

### Zia Example

```zia
func work(arg: Any) {
    // ...
}

var pool = Zanna.Threads.Pool.New(4);
var ok = Zanna.Threads.Pool.Submit(pool, &work, 0);
```

### BASIC Example

```basic
' Create a pool with 4 worker threads
DIM pool AS Zanna.Threads.Pool = Zanna.Threads.Pool.New(4)
PRINT "Size: "; pool.Size         ' Output: 4
PRINT "IsShutdown: "; pool.IsShutdown  ' Output: 0

' An empty pool is already drained.
DIM done AS INTEGER = pool.WaitFor(100)
PRINT "Completed in time: "; done  ' Output: 1

' Graceful shutdown (drains queue before stopping)
pool.Shutdown()
PRINT "IsShutdown: "; pool.IsShutdown  ' Output: 1
```

---

## Zanna.Threads.Promise

Producer side of a Future/Promise pair for asynchronous result passing between threads.

**Type:** Instance class

A Promise creates a linked Future object. The Promise is used by the producer thread to set the result
value (or error), and the Future is used by the consumer thread to retrieve the result.

### Constructor

| Method  | Signature    | Description        |
|---------|--------------|-------------------|
| `New()` | `Promise()`  | Create a new Promise |

### Methods

| Method           | Signature           | Description                                      |
|------------------|---------------------|--------------------------------------------------|
| `GetFuture()`    | `Future()`          | Get the linked Future (reuses its live cached wrapper) |
| `Set(value)`     | `Void(Object)`      | Complete with a value and retain runtime-managed values (can only call once) |
| `SetOwned(value)`| `Void(Object)`      | Explicit ownership-retaining alias for `Set`     |
| `SetError(msg)`  | `Void(String)`      | Complete with an error (can only call once)      |

### Properties

| Property | Type                   | Description                            |
|----------|------------------------|----------------------------------------|
| `IsDone` | `Boolean` (read-only)  | True if Set or SetError was called     |

### BASIC Example

```basic
' Create a promise and get its future
DIM promise AS OBJECT = Zanna.Threads.Promise.New()
DIM future AS OBJECT = promise.GetFuture()

' Complete exactly once with a runtime object
DIM result AS OBJECT = Zanna.Core.Box.I64(42)
promise.Set(result)
PRINT "Done: "; future.IsDone
PRINT "Value: "; Zanna.Core.Box.ToI64(future.Get())
```

On a different fresh Promise, use `SetError("Operation failed")` to reject it,
or `SetOwned(someObject)` when the retaining contract should be explicit. Do
not call more than one completion method on the same Promise.

### Notes

- `Set(value)` retains runtime-managed object and string handles for the
  Promise/Future pair. Each successful Future value getter returns its own
  retained handle; getters do not consume or clear the stored result.
- `SetOwned(value)` is kept for call sites that want to document ownership explicitly; it has the same retain semantics as `Set`.
- A Promise can only be completed once (either `Set`, `SetOwned`, or `SetError`).
- A duplicate completion traps and returns without replacing the first result,
  publishing another state, or falling through into listener delivery.
- `SetError(null)` stores `"Unknown error"`; a non-null message is copied.
- `GetFuture()` returns the cached Future while that wrapper is alive. If it has
  been finalized, a later call creates a new wrapper over the same Promise state.
  The cache is weak and is promoted while completion state is synchronized, so
  it neither creates a permanent Promise/Future cycle nor returns a wrapper that
  is concurrently being finalized.
- Native worker adapters that must settle after catching a trap use the internal C-only
  `rt_promise_try_set_transferred` and `rt_promise_try_set_error_cstr` functions. They are not
  registered Promise methods: the caller must own a Promise reference, transferred values are
  consumed on success or duplicate/invalid completion, and diagnostic OOM still publishes a
  message-less error state.

### Errors (Traps)

- `Promise: null object`
- `Promise: already completed` (calling Set or SetError twice)

---

## Zanna.Threads.Future

Consumer side of a Future/Promise pair for receiving asynchronous results.

**Type:** Instance class (obtained via `Promise.GetFuture()`)

A Future represents a value that will be available at some point in the future. It is linked to a
Promise which is used to set the value.

### Methods

| Method          | Signature           | Description                                          |
|-----------------|---------------------|------------------------------------------------------|
| `Get()`         | `Object()`          | Block until resolved, return value (traps on error)  |
| `TryGet()`      | `Object()`          | Non-blocking value; NULL if pending, errored, or successfully resolved with NULL |
| `GetFor(ms)`    | `Object(Integer)`   | Timed value; NULL on timeout, error, or a successful NULL result |
| `Wait()`        | `Void()`            | Block until resolved (value or error)                |
| `WaitFor(ms)`   | `Boolean(Integer)`  | Wait with timeout, returns true if resolved          |

### Properties

| Property  | Type                   | Description                                  |
|-----------|------------------------|----------------------------------------------|
| `IsDone`  | `Boolean` (read-only)  | True if resolved (value or error)            |
| `IsError` | `Boolean` (read-only)  | True if resolved with error                  |
| `Error`   | `String` (read-only)   | Error message (empty if no error)            |

### BASIC Example

```basic
' Get a future from a promise
DIM promise AS OBJECT = Zanna.Threads.Promise.New()
DIM future AS OBJECT = promise.GetFuture()

' In producer thread: set the result
DIM result AS OBJECT = Zanna.Core.Box.I64(42)
promise.Set(result)

' In consumer thread: get the result
DIM value AS OBJECT = future.Get()
PRINT Zanna.Core.Box.ToI64(value)
```

### Async Task Example

```basic
SUB ComputeAsync(promise AS OBJECT)
    ' Do some long computation
    DIM result AS INTEGER = ExpensiveComputation()
    promise.Set(result)
END SUB

' Start async computation
DIM promise AS OBJECT = Zanna.Threads.Promise.New()
DIM future AS OBJECT = promise.GetFuture()

' Start worker thread
DIM t AS OBJECT = Zanna.Threads.Thread.Start(ADDRESSOF ComputeAsync, promise)

' Do other work while computation runs
DoOtherWork()

' Wait for result
DIM result AS OBJECT = future.Get()
t.Join()
```

### Timeout Example

```basic
DIM future AS OBJECT = GetFutureFromSomewhere()

' Wait up to 5 seconds for result
IF future.WaitFor(5000) THEN
    IF NOT future.IsError THEN
        DIM value AS OBJECT = future.Get()
        PRINT "Got result"
    ELSE
        PRINT "Error: "; future.Error
    END IF
ELSE
    PRINT "Timed out waiting for result"
END IF
```

### Polling Pattern

```basic
DIM future AS OBJECT = GetFutureFromSomewhere()

' Poll without blocking
DO WHILE NOT future.IsDone
    ' Do other work
    ProcessEvents()
    Zanna.Time.Clock.Sleep(10)  ' Small delay
LOOP

IF NOT future.IsError THEN
    DIM value AS OBJECT = future.Get()
END IF
```

### Error Handling

```basic
DIM promise AS OBJECT = Zanna.Threads.Promise.New()
DIM future AS OBJECT = promise.GetFuture()

' Producer might fail
IF errorOccurred THEN
    promise.SetError("Failed to compute result")
ELSE
    promise.Set(result)
END IF

' Consumer checks for error
IF future.IsError THEN
    PRINT "Error: "; future.Error
ELSE
    DIM value AS OBJECT = future.Get()  ' Safe - we know it's not an error
END IF
```

### Errors (Traps)

- `Future: null object` (`Get`; the convenience queries/getters use their
  false/empty/null result for a null handle instead)
- the message supplied to `Promise.SetError` (calling `Get()` on an
  error-resolved Future; `"Unknown error"` when no message was supplied)

### Notes

- A Promise can only be completed once (`Set`, `SetOwned`, or `SetError`).
- `GetFuture()` returns the cached Future and each call receives its own retained handle.
- Calling `Get()` on an error-resolved Future traps with the stored error text;
  `Wait()` and `WaitFor()` merely wait for settlement and do not rethrow it.
- `GetFor(ms <= 0)` and `WaitFor(ms <= 0)` perform immediate checks.
- `WaitFor()` returns true for either a value or an error and false on timeout.
- `TryGet()` returns null for a pending or errored Future and for a successful
  null result alike. Inspect `IsDone` and `IsError`/`Error` when those cases must
  be distinguished.
- Multiple threads can wait on the same Future. Every retrieval of an owned
  runtime value receives a fresh retain and must be balanced at the C ABI layer.
- Completion listeners run outside the Promise lock. Listener traps are isolated after cleanup so remaining listeners still run and promise completion is not converted into a listener trap.
- Cancelling a pending completion listener runs its cleanup hook after removing it. Cleanup-hook traps are isolated after the listener and retained Future reference are released.
- Listener registration maintains a tail pointer, so appending a continuation
  is constant-time even when a Promise already has many pending listeners.
- Promise state, cached Future wrappers, stored results, and continuation-owned
  managed references participate in GC traversal; unreachable continuation or
  Promise/Future cycles can be collected.

---

## Zanna.Threads.Parallel

High-level parallel execution utilities for distributing work across CPU cores.

**Type:** Static utility class

Provides common parallel patterns like ForEach, Map, and Invoke using a shared thread pool.

### Methods

| Method                            | Signature                                 | Description                                           |
|-----------------------------------|-------------------------------------------|-------------------------------------------------------|
| `ForEach(seq, func)`              | `Void(Seq, Function)`                     | Execute managed function for each item                 |
| `ForEachPool(seq, func, pool)`    | `Void(Seq, Function, Pool)`               | ForEach with custom thread pool                        |
| `Map(seq, func)`                  | `Seq(Seq, Function)`                      | Transform items in parallel                            |
| `MapPool(seq, func, pool)`        | `Seq(Seq, Function, Pool)`                | Map with custom thread pool                            |
| `Invoke(funcs)`                   | `Void(Seq)`                               | Execute multiple functions in parallel                |
| `InvokePool(funcs, pool)`         | `Void(Seq, Pool)`                         | Invoke with custom thread pool                        |
| `For(start, end, func)`           | `Void(Integer, Integer, Function)`        | Parallel callback loop                                 |
| `ForPool(start, end, func, pool)` | `Void(Integer, Integer, Function, Pool)`  | Parallel callback loop with custom pool                |
| `Reduce(seq, func, identity)`     | `Object(Seq, Function, Object)`           | Reduce using a managed combine function                |
| `ReducePool(seq, func, id, pool)` | `Object(Seq, Function, Object, Pool)`     | Reduce with custom thread pool                         |
| `DefaultWorkers()`                | `Integer()`                               | Get online logical processor count (fallback 4)       |
| `DefaultPool()`                   | `Pool()`                                  | Get or create the shared default thread pool          |

### Zia Example

`Parallel` callback operations accept managed function references. The native worker callback handle remains an implementation detail of the runtime.

### BASIC ForEach Example

```basic
SUB ProcessItem(item AS OBJECT)
    ' Process each item
    PRINT "Processing: "; item
END SUB

' Process all items in parallel
DIM items AS OBJECT = CreateItems()
Zanna.Threads.Parallel.ForEach(items, ADDRESSOF ProcessItem)
```

### Map Example

```basic
FUNCTION Transform(item AS PTR) AS PTR
    ' Transform the item
    RETURN ComputeResult(item)
END FUNCTION

DIM inputs AS OBJECT = GetInputs()
DIM outputs AS OBJECT = Zanna.Threads.Parallel.Map(inputs, ADDRESSOF Transform)

' outputs has same length and order as inputs
FOR i = 0 TO outputs.Count - 1
    PRINT "Result "; i; ": "; outputs.Get(i)
NEXT i
```

### Parallel Ownership Notes

- `Parallel.Map` retains runtime-managed mapper results before placing them in the returned sequence, so exact input elements and other shared/borrowed runtime objects remain valid after the original owner is released.
- `Parallel.Reduce` returns the accumulator exactly as the reducer produces it. The runtime does not retain or release intermediate accumulator objects; reducers that allocate replacement accumulators are responsible for their own intermediate ownership discipline.

Input sequences, callback handles, custom pools, and non-result callback data
are borrowed for the duration of the blocking call. Do not mutate or release an
input sequence concurrently with a native parallel operation.

### Parallel For Example

```basic
SUB ProcessIndex(i AS INTEGER)
    ' Process item at index i
    DoWork(i)
END SUB

' Process indices 0..99 in parallel
Zanna.Threads.Parallel.For(0, 100, ADDRESSOF ProcessIndex)
```

### Invoke Example (Zia)

```zia
func taskA() {}
func taskB() {}
func taskC() {}

var tasks = Zanna.Collections.Seq.New();
tasks.Push(&taskA);
tasks.Push(&taskB);
tasks.Push(&taskC);
Zanna.Threads.Parallel.Invoke(tasks);
```

`Invoke` returns after every task finishes. BASIC can pass `ADDRESSOF` directly to `For`,
`ForEach`, and `Map`, but the current frontend cannot place an `ADDRESSOF` value in a `Seq`;
therefore `Invoke` and `InvokePool` are not currently callable from BASIC source.

### Custom Thread Pool

```basic
' Create a pool with 4 workers
DIM pool AS OBJECT = Zanna.Threads.Pool.New(4)

' Use custom pool for parallel operations
Zanna.Threads.Parallel.ForEachPool(items, ADDRESSOF Process, pool)

' Shut down pool when done
pool.Shutdown()
```

### Notes

- **Default pool:** Operations without an explicit pool use a shared pool sized
  from the online logical processor count, with the Pool constructor's
  `1..1024` clamp. On a machine reporting more than 1024 processors,
  `DefaultWorkers()` can therefore exceed the returned Pool's `Size`.
- **Default pool handle:** `DefaultPool()` returns a retained handle; release it like other runtime objects when using the C ABI directly.
- **Default pool typing:** Zia currently cannot chain Pool members directly
  from the untyped `DefaultPool()` result. Use an explicit receiver such as
  `Zanna.Threads.Pool.get_Size(Zanna.Threads.Parallel.DefaultPool())`.
- **Order preservation:** `Map` guarantees output order matches input order
- **Map result ownership:** Runtime-managed objects returned by a `Map` callback are retained for the result Seq, which releases those retained references when the Seq is freed.
- **Blocking:** All parallel operations block until work is complete
- **Thread safety:** Functions passed to parallel operations must be thread-safe
- **Work distribution:** Tiny workloads use one task per element. Larger
  workloads target four tasks per worker and at least 16 elements per task.
- **Reduce grouping:** Empty input returns `identity`; inputs of up to four
  elements are folded serially. Larger native inputs are folded left-to-right
  within contiguous chunks, then the chunk results are combined in chunk order
  with `identity` applied exactly once on the calling thread. The reducer must
  be associative if this regrouping is expected to match a sequential fold.
- **For range bounds:** `For(start, end, func)` traps if the half-open range is larger than `Integer` can represent.
- **Task traps:** If a worker callback traps, the operation wakes its caller and rethrows the callback's trap message instead of hanging.
- **Nested pool calls:** A `Parallel.*Pool` call made from a worker already running in the target pool executes inline to avoid self-deadlock.
- **Callback bridge:** `Parallel` callback APIs take managed function references at the frontend boundary; native worker callbacks remain internal.
- **Null/empty inputs:** `ForEach` and `Invoke` are no-ops for null/empty
  sequences; `Map` returns an empty Seq; `Reduce` returns its identity. A null
  callback similarly produces the operation's empty/no-op result.

### VM and BytecodeVM Behavior

The callback bridge preserves results but not parallelism on the interpreted
backends:

- Every `Parallel` method — `For`, `Invoke`, `ForEach`, `Map`, `Reduce`, and
  their `*Pool` variants — executes callbacks sequentially on the calling VM
  thread; an explicit pool is only a native concurrency hint.
- Only native execution uses worker chunks and the shared/custom thread pool,
  so ordering, callback thread identity, and trap timing can still differ
  between native and VM runs.

These differences are backend execution strategies, not a promise that
callbacks are safe to run concurrently on every backend.

### Use Cases

- **Data processing:** Transform large datasets in parallel
- **Batch operations:** Process many files or network requests concurrently
- **Computation:** Parallelize CPU-intensive algorithms
- **Initialization:** Load multiple resources simultaneously

---

## Zanna.Threads.CancelToken

Cooperative cancellation token for signaling cancellation to long-running or asynchronous operations. Thread-safe via atomic operations.

**Type:** Instance class
**Constructor:** `Zanna.Threads.CancelToken.New()`

### Properties

| Property      | Type    | Description                                |
|---------------|---------|--------------------------------------------|
| `IsCancelled` | Boolean | True if this token or any linked parent has been cancelled |

### Methods

| Method                    | Signature              | Description                                              |
|---------------------------|------------------------|----------------------------------------------------------|
| `Cancel()`                | `Void()`               | Request cancellation on this token                        |
| `Reset()`                 | `Void()`               | Reset the token for reuse                                |
| `Linked(parent)`          | `CancelToken(CancelToken)` | Class-level factory for a child linked to `parent` |
| `IsCancelled`             | `Boolean`              | True when this or a parent token has been cancelled      |
| `ThrowIfCancelled()`      | `Void()`               | Trap if the token has been cancelled                     |

### Notes

- **Thread-safe:** Local cancellation state uses atomic memory operations; linked parent/child bookkeeping is synchronized internally.
- **Reusable:** `Reset()` clears this token's local cancelled bit so it can be reused.
- **Linked tokens:** Invoke `Zanna.Threads.CancelToken.Linked(parent)` to create
  a child. Parent cancellation is propagated into children and is sticky:
  resetting the parent later does not clear a directly linked child's local
  cancellation bit.
- **Child reset:** After a parent has been reset, a linked child may be reset independently to clear its propagated cancellation state. If the parent is still cancelled, the child continues to report cancelled.
- **Cooperative:** Cancellation is advisory. The operation must check the token and respond appropriately.
- **Trap kind:** `ThrowIfCancelled()` raises an interrupt-kind trap with
  `OperationCancelledException: cancellation was requested`; it is not an
  ordinary runtime-error trap.

### BASIC Example

```basic
' Create a cancellation token
DIM token AS OBJECT = Zanna.Threads.CancelToken.New()

' Pass to a long-running operation
SUB ProcessItems(items AS OBJECT, cancel AS OBJECT)
    FOR i = 0 TO items.Count - 1
        ' Check for cancellation periodically
        IF cancel.IsCancelled THEN
            PRINT "Operation cancelled at item "; i
            EXIT SUB
        END IF
        ProcessItem(items.Get(i))
    NEXT
END SUB

' Cancel from another thread or after a condition
token.Cancel()

' Linked tokens for hierarchical cancellation
DIM parentToken AS OBJECT = Zanna.Threads.CancelToken.New()
DIM childToken AS OBJECT = Zanna.Threads.CancelToken.Linked(parentToken)

parentToken.Cancel()
PRINT childToken.IsCancelled  ' Output: 1 (true - parent was cancelled)

' ThrowIfCancelled traps if cancelled
token.ThrowIfCancelled()  ' Traps: OperationCancelledException: cancellation was requested
```

### Use Cases

- **Async operations:** Cancel HTTP requests, database queries, or file operations
- **Thread management:** Signal worker threads to stop gracefully
- **Timeout patterns:** Cancel operations that exceed time limits
- **UI responsiveness:** Cancel background work when user navigates away

---

## Zanna.Threads.Debouncer

Time-based debouncer that delays execution until a quiet period has elapsed. Useful for coalescing rapid events (e.g., keyboard input, resize events) into a single action.

**Type:** Instance class
**Constructor:** `Zanna.Threads.Debouncer.New(delayMs)`

### Properties

| Property      | Type                   | Description                                      |
|---------------|------------------------|--------------------------------------------------|
| `Delay`       | `Integer` (read-only)  | Configured delay in milliseconds                 |
| `IsReady`     | `Boolean` (read-only)  | True if delay has elapsed since last signal      |
| `SignalCount` | `Integer` (read-only)  | Number of signals since construction or `Reset`  |

### Methods

| Method       | Signature       | Description                                              |
|--------------|-----------------|----------------------------------------------------------|
| `Signal()`   | `Void()`        | Signal the debouncer (resets the timer)                  |
| `Reset()`    | `Void()`        | Reset debouncer to initial state                         |

### How It Works

1. Call `Signal()` each time an event occurs
2. The timer resets on each signal
3. `IsReady` returns true only after the full delay has elapsed with no new signals
4. This ensures the action fires only after events stop arriving

`IsReady` is a level, not a one-shot event: once ready, it stays true until the
next `Signal()` or `Reset()`. The Debouncer stores no callback or event payload;
the polling caller decides what action to run. Non-positive constructor delays
are clamped to zero, and `SignalCount` saturates at the largest Integer.

### BASIC Example

```basic
' Create a debouncer with 300ms delay
DIM debounce AS OBJECT = Zanna.Threads.Debouncer.New(300)

' In an event loop (e.g., processing keystrokes)
SUB OnKeystroke(key AS STRING)
    debounce.Signal()
    ' Don't search yet - wait for user to stop typing
END SUB

' In the main loop
IF debounce.IsReady AND debounce.SignalCount > 0 THEN
    ' User stopped typing for 300ms - perform search
    PerformSearch()
    debounce.Reset()
END IF
```

### Use Cases

- **Search-as-you-type:** Wait for user to stop typing before querying
- **Window resize:** Recalculate layout after resizing stops
- **Auto-save:** Save document after editing pauses
- **Network requests:** Coalesce rapid updates into a single API call
- **Thread-safe:** Debouncer state is synchronized internally; multiple threads can call `Signal`, `Reset`, and properties safely.

---

## Zanna.Threads.Throttler

Time-based throttler that limits operations to at most once per interval. Unlike debouncing which waits for a quiet period, throttling ensures a minimum time between executions.

**Type:** Instance class
**Constructor:** `Zanna.Threads.Throttler.New(intervalMs)`

### Properties

| Property       | Type                   | Description                                                |
|----------------|------------------------|------------------------------------------------------------|
| `CanProceed`   | `Boolean` (read-only)  | True if an operation would be allowed (without marking)    |
| `Count`        | `Integer` (read-only)  | Number of operations allowed so far                        |
| `Interval`     | `Integer` (read-only)  | Configured interval in milliseconds                        |
| `RemainingMs`  | `Integer` (read-only)  | Milliseconds until next operation is allowed (0 if ready)  |

### Methods

| Method         | Signature    | Description                                              |
|----------------|--------------|----------------------------------------------------------|
| `TryAcquire()` | `Boolean()`  | Try to execute (returns true if allowed, marks as used)  |
| `Reset()`      | `Void()`     | Reset throttler to allow immediate operation             |

### How It Works

1. Call `TryAcquire()` before performing the rate-limited operation
2. Returns `true` if enough time has passed since the last allowed operation
3. Returns `false` if the interval hasn't elapsed yet
4. The first call always succeeds

### BASIC Example

```basic
' Create a throttler allowing one operation per second
DIM throttle AS OBJECT = Zanna.Threads.Throttler.New(1000)

' In an event loop
SUB OnMouseMove(x AS INTEGER, y AS INTEGER)
    ' Only update at most once per second
    IF throttle.TryAcquire() THEN
        UpdateDisplay(x, y)
    END IF
END SUB

' Check without consuming
IF throttle.CanProceed THEN
    PRINT "Ready for next operation"
END IF

PRINT "Operations performed: "; throttle.Count
PRINT "Time until next allowed: "; throttle.RemainingMs; "ms"
```

### Debouncer vs Throttler

| Feature          | Debouncer                    | Throttler                      |
|------------------|------------------------------|--------------------------------|
| Timing           | After quiet period           | At regular intervals           |
| First event      | Delayed                      | Immediate                      |
| Rapid events     | One readiness after quiet    | First attempt passes, rest skip |
| Use case         | Wait for user to stop        | Limit rate of execution        |

### Use Cases

- **API rate limiting:** Limit outbound API calls per second
- **UI updates:** Throttle expensive re-renders
- **Logging:** Limit log output rate
- **Polling:** Control polling frequency
- **Thread-safe:** Throttler state is synchronized internally; multiple threads can call `TryAcquire`, `Reset`, and properties safely.
- Non-positive constructor intervals are clamped to zero, so every `TryAcquire()`
  succeeds. `Count` counts successful attempts only and saturates at the largest
  Integer; `Reset()` clears both timing state and the count.

---

## Zanna.Threads.Scheduler

Named task scheduler for scheduling delayed operations. Tasks are identified by name and become due after a specified delay. Poll-based (not thread-based).

**Type:** Instance class
**Constructor:** `Zanna.Threads.Scheduler.New()`

### Properties

| Property  | Type    | Description                           |
|-----------|---------|---------------------------------------|
| `Pending` | Integer | Number of scheduled tasks (due + not-yet-due) |

### Methods

| Method                     | Signature                   | Description                                      |
|----------------------------|-----------------------------|--------------------------------------------------|
| `Schedule(name, delayMs)`  | `Void(String, Integer)`     | Schedule a named task with delay in milliseconds |
| `ScheduleGeneration(name, delayMs, generation)` | `Void(String, Integer, Integer)` | Schedule, tagging the entry with a caller-supplied generation (e.g. a document revision) |
| `Cancel(name)`             | `Boolean(String)`           | Cancel a scheduled task by name                  |
| `IsDue(name)`              | `Boolean(String)`           | Check if a named task is due                     |
| `IsDueGeneration(name, generation)` | `Boolean(String, Integer)` | Due **and** the entry still carries `generation` (`0`/false if superseded) |
| `GenerationOf(name)`       | `Integer(String)`           | Generation currently scheduled for `name`, or `-1` if not scheduled |
| `GenerationOfOption(name)` | `Option[Integer](String)`   | `Some(generation)` when `name` is scheduled (even generation `-1`), `None` otherwise |
| `Poll()`                   | `Seq()`                     | Get all due tasks (removes them from scheduler)  |
| `Clear()`                  | `Void()`                    | Remove all scheduled tasks                       |

### Notes

- **Poll-based:** Tasks don't execute automatically. Call `Poll()` or `IsDue()` to check for due tasks.
- **Named tasks:** Tasks are identified by name. Scheduling a task with the same name as an existing task replaces it.
- **Revision-aware scheduling:** `ScheduleGeneration(name, delayMs, generation)` stamps an entry with a caller-defined generation (e.g. a document revision). Because re-scheduling a name replaces its entry, `IsDueGeneration(name, g)` fires only for the *latest* generation — a newer `ScheduleGeneration` supersedes an older one, so stale work is discarded in a single call (`GenerationOf(name)` answers "is my revision still the one queued?"). This is the canonical primitive for debounced, edit-superseding background work — live diagnostics, search-as-you-type, incremental indexing — and should be preferred over hand-rolled `Zanna.Time.Clock.NowMs()` timers. Plain `Schedule` records generation `0`.
- **Full string keys:** Task names compare by byte length and contents, so strings with embedded NUL bytes remain distinct.
- **Poll ownership:** `Poll()` returns a Seq that owns the returned task-name strings.
- **Clock:** Uses a monotonic clock when the platform exposes one and falls back
  to the realtime clock if that query fails.
- **Delay bounds:** Negative delays are treated as zero. Addition overflow
  saturates the due time at the largest Integer, making the task effectively
  never due.
- **Poll order:** New names are inserted at the head of an internal list, so
  due names are returned newest-scheduled first, not sorted by deadline. Replacing
  an existing name does not move its list position.
- **Generation sentinel:** `GenerationOf` uses `-1` for "not scheduled," while
  `ScheduleGeneration` accepts any Integer. Use `GenerationOfOption` when `-1` is a
  legitimate generation value: it returns `Some(generation)` for any scheduled name and
  `None` only for absence.
- **Immediate tasks:** A delay of 0 schedules a task that is immediately due on the next `Poll()`.
- **Thread-safe:** Scheduler operations are internally synchronized; multiple threads may schedule, cancel, poll, and clear the same instance.

### BASIC Example

```basic
' Create a scheduler
DIM sched AS OBJECT = Zanna.Threads.Scheduler.New()

' Schedule tasks with different delays
sched.Schedule("save", 5000)      ' Save in 5 seconds
sched.Schedule("refresh", 1000)   ' Refresh in 1 second
sched.Schedule("cleanup", 30000)  ' Cleanup in 30 seconds

PRINT sched.Pending  ' Output: 3

' Check specific task
IF sched.IsDue("refresh") THEN
    DoRefresh()
END IF

' Cancel a task
sched.Cancel("cleanup")
PRINT sched.Pending  ' Output: 2

' Poll for all due tasks in main loop
DO
    DIM due AS OBJECT = sched.Poll()
    FOR i = 0 TO due.Count - 1
        DIM taskName AS STRING = due.Get(i)
        SELECT CASE taskName
            CASE "save": DoSave()
            CASE "refresh": DoRefresh()
        END SELECT
    NEXT

    Zanna.Time.Clock.Sleep(100)  ' Poll every 100ms
LOOP WHILE sched.Pending > 0
```

### Use Cases

- **Delayed operations:** Schedule saves, cleanups, or refreshes
- **Game timers:** Schedule game events (spawn, power-up expiry)
- **Retry scheduling:** Schedule retry attempts with delays
- **Batch processing:** Accumulate work and process after delay

---

## Zanna.Threads.Async

Async task combinators for composing asynchronous results. Built on Future/Promise.

**Type:** Static utility class

### Methods

| Method                            | Signature                          | Description                                         |
|-----------------------------------|------------------------------------|-----------------------------------------------------|
| `Delay(ms)`                       | `Future(Integer)`                  | Return a Future that resolves after `ms` milliseconds with NULL |
| `All(futures)`                    | `Future(Seq)`                      | Return a Future that resolves when all input futures resolve (with a Seq of results) |
| `Any(futures)`                    | `Future(Object)`                   | Return a Future that resolves with the value of whichever input future completes first |
| `Run(callback, arg)`              | `Future(Function, Object)`         | Run `(arg) -> Object` on a new background thread; native `arg` is borrowed |
| `RunOwned(callback, arg)`         | `Future(Function, Object)`         | Native retaining form of `Run` for a runtime-managed argument |
| `Map(future, mapper, arg)`        | `Future(Future, Function, Object)` | Apply `(value, arg) -> Object` when the source settles; native `arg` is borrowed |
| `MapOwned(future, mapper, arg)`   | `Future(Future, Function, Object)` | Retaining form of `Map` for the extra mapper argument |
| `RunCancellable(callback, arg, token)` | `Future(Function, Object, CancelToken)` | Cancellable callback variant |
| `RunCancellableOwned(callback, arg, token)` | `Future(Function, Object, CancelToken)` | Cancellable owned-arg variant |

### Notes

- All methods are thread-safe.
- `Delay` returns a Future that resolves with NULL after the specified delay;
  negative delays are treated as zero.
- `All` returns a Future resolving to a Seq of results. If any input future has an error, the combined Future resolves with that error without waiting for the remaining inputs.
- The Seq returned by `All` owns retained runtime-managed result values, so results remain valid after the source futures are released.
- `All(null)` and `All` of an empty Seq resolve successfully with an empty Seq.
  `Any(null)` and `Any` of an empty Seq resolve as an error with
  `Async.Any: empty futures`.
- `Any` forwards either the value or error from the first input that wins the
  completion race. If inputs are already settled while listeners are being
  registered, the earliest such input in Seq order wins.
- `Run` spawns a new native thread and resolves with the callback's return
  value. Zia may pass `&function`; BASIC may pass `ADDRESSOF Function`.
- `Map` propagates a source error without calling the mapper. On success, the
  mapper runs synchronously on the thread that delivers completion; if the
  source was already settled, that may be the thread calling `Map`.
- `RunCancellable` checks the token before and after the callback. Cancellation
  does not preempt the callback; the callback must inspect the token
  cooperatively. A cancelled task resolves as an error with `cancelled`.
- Traps raised inside `Run`, `RunCancellable`, or `Map` callbacks are converted into Future errors.
- On the native backend, the non-`Owned` `Run`, `RunCancellable`, and `Map`
  variants borrow their extra `arg`; callers must keep it alive until the
  callback finishes. The corresponding `Owned` variants retain a
  runtime-managed argument for that lifetime.
- Runtime-managed values returned from `Run`, `RunCancellable`, and `Map` are retained before being published through the returned Future, so borrowed argument/input values and shared runtime objects remain valid after the original owner is released.
- If a callback wants to document ownership explicitly in custom promise/future flows, use `Promise.SetOwned`; it has the same retaining behavior as `Promise.Set`.

### VM and BytecodeVM Behavior

`Async.Run` and `Async.RunOwned` have dedicated managed callback bridges on
both VMs, with the same ownership contract as native: `Run` borrows its `arg`
(keep it alive until the worker finishes) and `RunOwned` retains it for the
worker lifetime. `Delay`, `All`, and `Any` do not invoke user callbacks and
use the shared C runtime implementation.

`RunCancellable`, `RunCancellableOwned`, `Map`, and `MapOwned` have no managed
callback bridge yet; invoking them under either VM traps with an explicit
`callback execution is not supported on the interpreted backends` message
(they run normally on native).

---

## Zanna.Threads.ConcurrentMap

Thread-safe string-keyed hash map for concurrent access from multiple threads.

**Type:** Instance class
**Constructor:** `Zanna.Threads.ConcurrentMap.New()`

### Properties

| Property  | Type    | Description                            |
|-----------|---------|----------------------------------------|
| `Count`      | Integer | Number of key-value pairs in the map   |
| `IsEmpty` | Boolean | Returns true if the map has no entries |

### Methods

| Method                       | Signature                     | Description                                                |
|------------------------------|-------------------------------|------------------------------------------------------------|
| `Set(key, value)`            | `Void(String, Object)`        | Thread-safe insert or update                               |
| `Get(key)`                   | `Object(String)`              | Thread-safe lookup; returns NULL if not found               |
| `GetOr(key, default)`        | `Object(String, Object)`      | Thread-safe lookup; returns `default` if not found          |
| `Has(key)`                   | `Boolean(String)`             | Thread-safe existence check                                |
| `SetIfMissing(key, value)`   | `Boolean(String, Object)`     | Insert only if key absent; returns true if inserted         |
| `Remove(key)`                | `Boolean(String)`             | Thread-safe removal; returns true if found                  |
| `Clear()`                    | `Void()`                      | Thread-safe removal of all entries                          |
| `Keys()`                     | `Seq()`                       | Get snapshot of all keys (may not reflect concurrent writes)|
| `Values()`                   | `Seq()`                       | Get snapshot of all values (may not reflect concurrent writes)|

### Notes

- Uses mutex protection; safe for concurrent reads and writes from any thread.
- Keys are copied on insert (not retained by reference).
- Keys compare by byte length and contents, so strings with embedded NUL bytes are supported.
- A null key is normalized to the empty byte string and therefore aliases `""`.
- Values are retained while in the map.
- `Get`, found-value `GetOr`, and `Values` return stable retained references/snapshots that remain valid even if the map is concurrently updated after the call returns. At the C ABI layer, callers release those returned references.
- Null values are valid. Use `Has(key)` to distinguish a present null value from
  an absent key. `GetOr` returns a present null rather than its default, and an
  absent-key default is passed through without an extra retain.
- `Keys()` and `Values()` are owning snapshots in hash-bucket walk order, not
  insertion or sorted order. Separate snapshots can describe different map
  states when writes occur between the calls.
- Uses FNV-1a hash with separate chaining for collision resolution.
- For single-threaded use, prefer `Zanna.Collections.Map` which has no locking overhead.

### Zia Example

```zia
module ConcMapDemo;

bind Zanna.Terminal;
bind Zanna.Threads.ConcurrentMap as CMap;
bind Zanna.Core.Box as Box;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var m = CMap.New();
    Say("Empty: " + Fmt.Bool(m.get_IsEmpty()));

    // Set key-value pairs (values must be objects, use Box)
    m.Set("name", Box.Str("Alice"));
    m.Set("age", Box.I64(30));
    Say("Count: " + Fmt.Int(m.get_Count()));

    // Get values
    Say("name: " + Box.ToStr(m.Get("name")));
    Say("age: " + Fmt.Int(Box.ToI64(m.Get("age"))));

    // Has check
    Say("Has name: " + Fmt.Bool(m.Has("name")));

    // SetIfMissing (returns false if key exists)
    Say("SetIfMissing name: " + Fmt.Bool(m.SetIfMissing("name", Box.Str("Bob"))));

    // Remove
    Say("Remove age: " + Fmt.Bool(m.Remove("age")));

    // Clear
    m.Clear();
    Say("Count after Clear: " + Fmt.Int(m.get_Count()));
}
```

### BASIC Example

```basic
DIM m AS OBJECT = Zanna.Threads.ConcurrentMap.New()
PRINT "Empty: "; m.IsEmpty

' Set key-value pairs (values are objects, use Box)
m.Set("name", Zanna.Core.Box.Str("Alice"))
m.Set("age", Zanna.Core.Box.I64(30))
PRINT "Count: "; m.Count

' Get values - use static call form for obj-returning methods
DIM nameVal AS OBJECT = Zanna.Threads.ConcurrentMap.Get(m, "name")
PRINT "name: "; Zanna.Core.Box.ToStr(nameVal)

DIM ageVal AS OBJECT = Zanna.Threads.ConcurrentMap.Get(m, "age")
PRINT "age: "; Zanna.Core.Box.ToI64(ageVal)

' Has check
PRINT "Has name: "; m.Has("name")

' SetIfMissing (returns false if key exists)
PRINT "SetIfMissing name: "; m.SetIfMissing("name", Zanna.Core.Box.Str("Bob"))

' Remove and Clear
PRINT "Remove age: "; m.Remove("age")
m.Clear()
PRINT "Count after Clear: "; m.Count
```

---

## Zanna.Threads.ConcurrentQueue

Thread-safe FIFO queue for concurrent access from multiple threads.

**Type:** Instance class
**Constructor:** `Zanna.Threads.ConcurrentQueue.New()`

### Properties

| Property  | Type                   | Description                              |
|-----------|------------------------|------------------------------------------|
| `Count`      | `Integer` (read-only)  | Mutex-protected element-count snapshot   |
| `IsEmpty` | `Boolean` (read-only)  | Mutex-protected empty-state snapshot      |
| `IsClosed` | `Boolean` (read-only) | True after `Close()` has been called     |

### Methods

| Method               | Signature             | Description                                                      |
|----------------------|-----------------------|------------------------------------------------------------------|
| `Push(item)`         | `Void(Object)`        | Add item to back of queue (thread-safe)                          |
| `Pop()`              | `Object()`            | Remove item from front (blocks until available)                  |
| `TryPop()`           | `Object()`            | Remove item from front (non-blocking); returns NULL if empty     |
| `PopFor(ms)`         | `Object(Integer)`     | Remove item with timeout; returns NULL if timeout expires        |
| `Peek()`             | `Object()`            | Peek at front item without removing; returns NULL if empty       |
| `Clear()`            | `Void()`              | Remove all items (thread-safe)                                   |
| `Close()`            | `Void()`              | Close the queue, wake blocked dequeuers, and reject future enqueues |

### Notes

- FIFO order is guaranteed.
- `Pop` blocks until an item is available or the queue is closed and drained.
- `TryPop` returns an Option: `Some(item)` when an item was removed and `None` when
  the queue is empty, so a queued null is never confused with an empty queue.
- Null items are valid. `Pop`, `PopFor`, and `Peek` return null both for a null item
  and for their empty/closed/timeout cases; only `TryPop` distinguishes the two.
- `Peek` returns a stable retained reference to the current front item, or NULL if empty.
- `Pop`, `TryPop`, and `PopFor` transfer the queue's retained item reference to the caller. At the C ABI layer, callers release returned runtime-managed values.
- `Close()` wakes blocked `Pop`/`PopFor` calls; once the queue is empty they return NULL.
- `Close()` is idempotent. `Push()` after close traps with
  `ConcurrentQueue.Enqueue: queue is closed`.
- `PopFor(ms <= 0)` is a non-blocking pop.
- `Count` and `IsEmpty` are exact while their internal lock is held, but another
  thread may change the queue immediately after the property returns.
- Values are retained while in the queue.

### Zia Example

```zia
module ConcQueueDemo;

bind Zanna.Terminal;
bind Zanna.Threads.ConcurrentQueue as CQ;
bind Zanna.Core.Box as Box;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var q = CQ.New();
    Say("Empty: " + Fmt.Bool(q.get_IsEmpty()));

    // Push items
    q.Push(Box.I64(10));
    q.Push(Box.I64(20));
    q.Push(Box.I64(30));
    Say("Count: " + Fmt.Int(q.get_Count()));

    // Peek (non-destructive)
    Say("Peek: " + Fmt.Int(Box.ToI64(q.Peek())));

    // TryPop (non-blocking, Option-returning)
    var next = q.TryPop();
    if next.IsSome {
        Say("TryPop: " + Fmt.Int(Box.ToI64(next.Unwrap())));
    }

    // Pop (blocking, but items available)
    Say("Pop: " + Fmt.Int(Box.ToI64(q.Pop())));

    // PopFor (timed)
    Say("PopFor: " + Fmt.Int(Box.ToI64(q.PopFor(100))));
    Say("Empty after all: " + Fmt.Bool(q.get_IsEmpty()));

    // Clear
    q.Push(Box.I64(99));
    q.Clear();
    Say("Count after clear: " + Fmt.Int(q.get_Count()));
}
```

### BASIC Example

```basic
DIM q AS OBJECT = Zanna.Threads.ConcurrentQueue.New()
PRINT "Empty: "; q.IsEmpty

' Push items (values are objects, use Box)
q.Push(Zanna.Core.Box.I64(10))
q.Push(Zanna.Core.Box.I64(20))
q.Push(Zanna.Core.Box.I64(30))
PRINT "Count: "; q.Count

' Peek (non-destructive)
PRINT "Peek: "; Zanna.Core.Box.ToI64(q.Peek())

' TryPop (non-blocking, Option-returning)
DIM next AS OBJECT = q.TryPop()
IF next.IsSome THEN
    PRINT "TryPop: "; Zanna.Core.Box.ToI64(next.Unwrap())
END IF

' Pop (blocking, but items available)
PRINT "Pop: "; Zanna.Core.Box.ToI64(q.Pop())

' PopFor (timed)
PRINT "PopFor: "; Zanna.Core.Box.ToI64(q.PopFor(100))
PRINT "Empty after all: "; q.IsEmpty

' Clear
q.Push(Zanna.Core.Box.I64(99))
q.Clear()
PRINT "Count after clear: "; q.Count
```

---

## Zanna.Threads.Channel

Thread-safe bounded channel for inter-thread communication. Supports blocking, non-blocking, and timeout-based send/receive operations.

**Type:** Instance class (requires `New(capacity)`)

### Constructor

| Method           | Signature          | Description                                         |
|------------------|--------------------|-----------------------------------------------------|
| `New(capacity)`  | `Channel(Integer)` | Create a channel; capacity is clamped to `0..1000000` |

### Methods

| Method                  | Signature                    | Description                                                  |
|-------------------------|------------------------------|--------------------------------------------------------------|
| `Send(item)`            | `Void(Object)`               | Send item, blocking if full (traps if closed)                |
| `TrySend(item)`         | `Boolean(Object)`            | Try to send without blocking; returns false if full or closed|
| `SendFor(item, ms)`     | `Boolean(Object, Integer)`   | Send with timeout; returns false if timed out or closed      |
| `Recv()`                | `Object()`                   | Receive item, blocking if empty; returns NULL if closed and empty |
| `TryRecv()`             | `Option[Object]()`           | Try to receive without blocking; returns `None` if empty     |
| `RecvFor(ms)`           | `Object(Integer)`            | Receive with timeout; returns NULL if timed out or closed    |
| `Close()`               | `Void()`                     | Close the channel; wakes all blocked senders/receivers       |

### Properties

| Property   | Type                   | Description                           |
|------------|------------------------|---------------------------------------|
| `Count`       | `Integer` (read-only)  | Number of items currently in channel  |
| `Capacity` | `Integer` (read-only)  | Channel capacity (0 for synchronous)  |
| `IsClosed` | `Boolean` (read-only)  | True if the channel has been closed   |
| `IsEmpty`  | `Boolean` (read-only)  | True if the channel contains no items |
| `IsFull`   | `Boolean` (read-only)  | True if the channel is at capacity    |

### Notes

- Closing a channel prevents further sends but receivers can still drain remaining items.
- Negative capacities are treated as 0 (synchronous); capacities above
  1,000,000 are clamped to that maximum.
- A synchronous channel (capacity 0) blocks the sender until a receiver is ready.
- On a synchronous channel, `TrySend()` succeeds only when a receiver is already waiting, publishes one retained handoff value, and returns without waiting for the receiver to acknowledge consumption.
- On a synchronous channel, `TryRecv()` is strictly non-blocking. It only consumes an already-published handoff value; it does not wait to rendezvous with a merely waiting sender. Use `Recv()` or `RecvFor()` for rendezvous receives.
- Null items are valid. Raw-object `Recv` and `RecvFor` cannot distinguish a
  transmitted null from closed-and-drained or timeout. The language-level
  `TryRecv()` returns `Some(null)` for a
  transmitted null and `None` when no item is immediately available.
- At the C ABI layer, `rt_channel_try_recv(channel, NULL)` and
  `rt_channel_recv_for(channel, NULL, ms)` are non-consuming availability probes.
  They never release a queued reference, and the immediate probe does not
  advertise a merely waiting synchronous sender as available. Native callers
  that intentionally consume and release an item use the explicit
  `rt_channel_recv_for_discard` helper.
- `IsFull` means a send would block. For synchronous channels it is false when a receiver is already waiting and no handoff value is queued.
- `SendFor` and `RecvFor` use one monotonic deadline that starts before monitor
  acquisition. The budget therefore includes lock contention, condition waits,
  and (for a synchronous send) handoff acknowledgement; repeated wakeups do not
  restart the requested interval.
- `SendFor(item, ms <= 0)` is `TrySend(item)` and `RecvFor(ms <= 0)` is a
  non-blocking receive.
- `Send` traps if the channel is closed.
- `TrySend`/`SendFor` return false instead of trapping for a closed channel, and
  `Close()` is idempotent.
- Sends retain runtime-managed items; receives transfer that retained reference
  to the caller. Sending does not consume the caller's existing reference.
- On a synchronous channel, `Count` can transiently be 1 while a published
  handoff awaits consumption even though `Capacity` is 0.
- Channel, Promise/Future, ConcurrentMap, and ConcurrentQueue strong references
  participate in managed graph traversal, so cycles through their stored values
  can be reclaimed by the runtime cycle collector.

### Zia Example

```zia
module ChannelDemo;

bind Zanna.Terminal;
bind Zanna.Threads.Channel as Channel;
bind Zanna.Core.Box as Box;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var ch = Channel.New(8);
    Say("Capacity: " + Fmt.Int(ch.get_Capacity())); // 8
    Say("IsEmpty: " + Fmt.Bool(ch.get_IsEmpty()));  // true

    // Send items
    ch.Send(Box.I64(1));
    ch.Send(Box.I64(2));
    ch.Send(Box.I64(3));
    Say("Count: " + Fmt.Int(ch.get_Count()));     // 3

    // Non-blocking send (returns false if full or closed)
    Say("TrySend: " + Fmt.Bool(ch.TrySend(Box.I64(4))));  // true

    // Receive items
    Say("Recv: " + Fmt.Int(Box.ToI64(ch.Recv())));    // 1
    var maybe = ch.TryRecv();
    if maybe.IsSome {
        Say("TryRecvOption: " + Fmt.Int(Box.ToI64(maybe.Unwrap()))); // 2
    }

    // Close channel
    ch.Close();
    Say("IsClosed: " + Fmt.Bool(ch.get_IsClosed()));  // true
}
```

### BASIC Producer/Consumer Example

```basic
' Bounded channel with capacity 16
DIM ch AS Zanna.Threads.Channel = Zanna.Threads.Channel.New(16)

' Producer: send items
SUB Producer(channel AS Zanna.Threads.Channel)
    DIM i AS INTEGER
    FOR i = 1 TO 10
        channel.Send(Zanna.Core.Box.I64(i))
    NEXT i
    channel.Close()
END SUB

' Consumer: receive until closed and empty
SUB Consumer(channel AS Zanna.Threads.Channel)
    DO
        DIM item AS OBJECT = channel.Recv()
        IF Zanna.Core.Object.RefEquals(item, NOTHING) THEN EXIT DO   ' closed and drained
        PRINT "Received: "; Zanna.Core.Box.ToI64(item)
    LOOP
END SUB

' In a single-threaded context, demonstrate send/recv
DIM i AS INTEGER
FOR i = 1 TO 5
    ch.Send(Zanna.Core.Box.I64(i * 10))
NEXT i
PRINT "Count: "; ch.Count     ' Output: 5
PRINT "IsFull: "; ch.IsFull  ' Output: 0 (cap 16, only 5 items)

' Non-blocking receive
DIM item AS Zanna.Option = ch.TryRecv()
IF item.IsSome THEN
    PRINT "TryRecvOption: "; Zanna.Core.Box.ToI64(item.Unwrap())  ' Output: 10
END IF

' Timeout-based receive (100ms)
DIM timed AS OBJECT = ch.RecvFor(100)
IF NOT Zanna.Core.Object.RefEquals(timed, NOTHING) THEN
    PRINT "Got: "; Zanna.Core.Box.ToI64(timed)
ELSE
    PRINT "Timed out"
END IF

' Send with timeout
DIM sent AS INTEGER = ch.SendFor(Zanna.Core.Box.I64(99), 200)
PRINT "Sent: "; sent

ch.Close()
PRINT "IsClosed: "; ch.IsClosed  ' Output: 1
```

---

## See Also

- [Collections](collections/README.md) - Single-threaded and specialized collection types
- [Time & Timing](time.md) - `Clock.Sleep()` and timing utilities
