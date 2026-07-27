---
status: active
audience: contributors
last-verified: 2026-06-27
---

# ADR 0002: Shared-Memory Threads with FIFO Monitors and Safe Variables

Date: 2025-12-18

Status: Implemented; verified against source and runtime API on 2026-06-27.

## Context

Before this ADR, Zanna supported host/embedder-level concurrency by running multiple VM instances in parallel, but it did
not expose language-level shared-memory threading to Zanna programs. The runtime also needed thread-safe ownership and
runtime-context behavior before shared heap objects could be used safely across worker threads.

Zanna now provides a `Zanna.Threads` runtime namespace for shared-memory concurrency. It is implemented in
`src/runtime/threads/`, exposed through the runtime API registry, and bridged in both the tree-walking VM and BytecodeVM
for VM function pointers.

## Decision

Provide `Zanna.Threads` with:

1. Shared-memory OS threads in the VM, BytecodeVM, and native backends.
2. A re-entrant, FIFO-fair monitor primitive (`Zanna.Threads.Monitor`) for explicit object locking.
3. A monitor-backed safe integer type (`Zanna.Threads.SafeI64`) for common atomic counter/state patterns.
4. A separate runtime component library (`zanna_rt_threads`) so native codegen can link thread support when referenced
   by the program.

Definitions

- Defined threaded program: A program is defined if all shared mutable state is accessed under `Monitor` or via
  thread-safe runtime types such as `SafeI64`. Programs with data races are undefined; VM/native equivalence is not
  required for undefined programs.
- FIFO fairness: For contended monitor operations, queue order determines service order for lock acquisition and for
  waiters awakened via `Pause`/`PauseAll`.
- Re-entrant monitor: The thread currently owning the monitor may `Enter` again without blocking; the monitor tracks a
  recursion count and requires the matching number of `Exit` calls.

Runtime Surface

The live runtime API exposes these ADR-0002 core classes:

Monitor (static)

- `Zanna.Threads.Monitor.Enter(obj) -> void`
- `Zanna.Threads.Monitor.TryEnter(obj) -> i1`
- `Zanna.Threads.Monitor.TryEnterFor(obj, i64 ms) -> i1`
- `Zanna.Threads.Monitor.Exit(obj) -> void`
- `Zanna.Threads.Monitor.Wait(obj) -> void`
- `Zanna.Threads.Monitor.WaitFor(obj, i64 ms) -> i1`
- `Zanna.Threads.Monitor.Pause(obj) -> void`
- `Zanna.Threads.Monitor.PauseAll(obj) -> void`

Thread

- `Zanna.Threads.Thread.Start(obj entry, obj arg) -> obj`
- `Zanna.Threads.Thread.StartOwned(obj entry, obj arg) -> obj`
- `Zanna.Threads.Thread.StartSafe(obj entry, obj arg) -> obj`
- `Zanna.Threads.Thread.StartSafeOwned(obj entry, obj arg) -> obj`
- `Zanna.Threads.Thread.Join(obj) -> void`
- `Zanna.Threads.Thread.TryJoin(obj) -> i1`
- `Zanna.Threads.Thread.JoinFor(obj, i64 ms) -> i1`
- `Zanna.Threads.Thread.Sleep(i64 ms) -> void`
- `Zanna.Threads.Thread.Yield() -> void`
- `Zanna.Threads.Thread.SafeJoin(obj) -> void`
- `Zanna.Threads.Thread.SafeGetId(obj) -> i64`
- `Zanna.Threads.Thread.SafeIsAlive(obj) -> i1`
- readonly properties: `Id: i64`, `IsAlive: i1`, `HasError: i1`, `Error: str`

Safe variables

- `Zanna.Threads.SafeI64.New(i64 initial) -> obj`
- `Zanna.Threads.SafeI64.Get(obj) -> i64`
- `Zanna.Threads.SafeI64.Set(obj, i64) -> void`
- `Zanna.Threads.SafeI64.Add(obj, i64 delta) -> i64`
- `Zanna.Threads.SafeI64.CompareExchange(obj, i64 expected, i64 desired) -> i64`

The live registry also includes additional `Zanna.Threads` classes beyond this ADR's original scope: `Pool`, `Channel`,
`ConcurrentQueue`, `ConcurrentMap`, `Gate`, `Barrier`, `RwLock`, `Promise`, `Future`, `Async`, `Parallel`,
`CancelToken`, `Debouncer`, `Throttler`, and `Scheduler`. `SafeF64`, `SafeStr`, and `SafeObj` are not present in the
runtime API as of the verification date.

Semantics

Monitor.Enter/Exit

- `Enter(obj)` traps on null `obj`.
- If the calling thread already owns the monitor, `Enter` increments recursion and returns immediately.
- Otherwise `Enter` enqueues the calling thread in the monitor's FIFO acquisition queue and blocks until it becomes the
  owner.
- `Exit(obj)` traps on null `obj` and traps if the calling thread is not the current owner.
- `Exit` decrements recursion; when recursion reaches zero, ownership transfers to the next FIFO waiter if any.

Monitor.TryEnter/TryEnterFor

- `TryEnter(obj)` returns 1 if acquired or re-entered, returns 0 if contended, and traps on null `obj`.
- `TryEnterFor(obj, ms)` waits up to `ms` milliseconds using the same acquisition queue and returns 1 if acquired or 0
  on timeout.
- Negative `ms` values are treated as zero by the runtime.

Monitor.Wait/WaitFor and Pause/PauseAll

These operations provide condition-variable-like behavior associated with the monitor object.

- Precondition: the calling thread must own the monitor; otherwise the runtime traps.
- `Wait(obj)` fully releases the monitor, enqueues the thread on the monitor's FIFO wait queue, blocks until awakened by
  `Pause`/`PauseAll`, and then re-acquires the monitor with the prior recursion depth before returning.
- `WaitFor(obj, ms)` follows the same release/re-acquire behavior and returns 0 if the timeout elapses before a wake-up;
  negative `ms` values are treated as zero.
- `Pause(obj)` wakes the oldest waiter if any.
- `PauseAll(obj)` wakes all waiters in FIFO order.

Thread.Start and function pointers

`Thread.Start(entry, arg)` starts a new OS thread that invokes `entry(arg)` and then terminates. The `entry` pointer is
backend-defined:

- Native runtime: `entry` is a raw callback pointer.
- Tree-walking VM: `src/vm/ThreadsRuntime.cpp` installs VM-aware handlers for `Thread.Start`, `StartOwned`,
  `StartSafe`, and `StartSafeOwned`.
- BytecodeVM: `src/bytecode/BytecodeVM.cpp` installs equivalent handlers and validates bytecode entry signatures.

`StartOwned` and `StartSafeOwned` retain a runtime-managed argument until the worker completes. The `StartSafe` variants
wrap the worker in a trap boundary and expose `HasError`, `Error`, and safe join/query helpers.

Joining is repeatable after a thread has finished. `Join`, `TryJoin`, and `JoinFor` reject self-join attempts. For
`JoinFor`, `ms < 0` waits indefinitely, `ms == 0` behaves like `TryJoin`, and positive values wait up to that number of
milliseconds.

Determinism and Repeatability

- FIFO fairness is guaranteed for monitor queues, providing repeatability for synchronized sections.
- Zanna does not attempt to control OS scheduling. A program that is data-race-free but still depends on timing, such as
  a busy loop without synchronization, is undefined for VM/native equivalence.

Implementation Status

Verified on 2026-06-27:

- `src/runtime/CMakeLists.txt` builds `zanna_rt_threads` from `src/runtime/threads/*` and selects POSIX or Win32 monitor
  and thread implementations by platform.
- `src/codegen/common/RuntimeComponents.hpp` maps `rt_monitor_*`, `rt_thread_*`, `rt_safe_*`, channels, futures,
  parallel, and related thread symbols to the Threads component for selective native linking.
- `src/runtime/threads/rt_threads.h` declares the public Monitor, Thread, SafeThread, and SafeI64 C ABI surface.
- `src/runtime/threads/rt_monitor_posix.c` and `src/runtime/threads/rt_monitor_win.c` implement re-entrant FIFO monitor
  behavior.
- `src/runtime/threads/rt_safe_i64.c` implements SafeI64 using monitor-protected operations on POSIX and Interlocked
  operations inside monitor sections on Windows.
- `src/vm/ThreadsRuntime.cpp` and `src/bytecode/BytecodeVM.cpp` provide VM-aware handlers for thread starts and safe
  starts.
- Focused tests passed: `test_rt_threads_monitor`, `test_rt_threads_thread`, and `test_rt_threads_primitives`.

## Consequences

Pros:

- Adds language-level concurrency primitives with explicit, predictable fairness semantics.
- Enables VM/native parity for synchronized, defined threaded programs.
- Keeps thread runtime code in a separate component for selective native linking.

Cons:

- Atomic/thread-safe ownership and FIFO fairness add overhead compared to single-threaded execution.
- Deadlocks remain possible in user programs that misuse locks.
- VM and BytecodeVM bridges must keep runtime function-pointer handling aligned with native semantics.

## Alternatives

- Cooperative green threads in the VM: rejected because they would not match native OS-thread behavior.
- Native-only threads with VM trapping: rejected because VM execution must support the runtime surface.
- Non-FIFO locks relying on OS scheduling: rejected because FIFO fairness is required for repeatable synchronized
  behavior.
- Per-object embedded mutex/condition fields: rejected because they would bloat every heap object and constrain object
  layout.

Spec Impact

- Adds the `Zanna.Threads` runtime namespace and runtime symbols.
- Does not add IL opcodes.
- Defines data-race-free synchronization as the boundary for VM/native equivalence of threaded programs.
