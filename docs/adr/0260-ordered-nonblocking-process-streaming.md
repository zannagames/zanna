---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0260: Preserve Process Stream Order and Keep Windows Input Nonblocking

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0066 — Process and PTY Read Result APIs
- `src/runtime/system/rt_process.c` — cross-platform process adapter
- `src/zannastudio/src/build/build_system.zia` — Studio build/run owner

## Context

`ProcessHandle.ReadStdoutResult()` and `ReadStderrResult()` are safe bounded
reads, but a caller that drains stdout and then stderr destroys the observable
capture order between the two pipes. Build output can consequently move a
diagnostic away from the tool text that explains it. The separate chunks also
prevent a streaming parser from retaining independent stdout/stderr line
carries without imposing its own arbitrary ordering.

On Windows, `ProcessHandle.WriteStdin()` called synchronous `WriteFile` on an
anonymous pipe. A child that stopped consuming stdin could fill the pipe and
block the Studio UI thread indefinitely. POSIX already uses a nonblocking
descriptor and returns a partial count when capacity is unavailable.

## Decision

Add
`Zanna.System.Process.ProcessHandle.ReadOutputResult() -> Zanna.Collections.Map`
with C symbol `rt_process_read_output_result`.

The result contains:

- `chunks`: an ordered `Seq` of maps containing monotonic `sequence`, `stream`
  (`stdout` or `stderr`), and `text` fields.
- `truncated`: true when the bounded combined queue discarded bytes.

The runtime alternates immediately available pipe reads and records each
observed chunk before exposing it. The method consumes the ordered queue and
the corresponding legacy per-stream buffers; callers choose either the unified
or separate view rather than receiving the same bytes twice. Existing separate
read APIs remain available.

On Windows, retain the `WriteStdin(str) -> i64` signature but define its return
as bytes accepted by the runtime. The call copies into a bounded 1 MiB queue and
signals a process-owned writer thread. The potentially blocking Win32
`WriteFile` happens only on that worker. Queue saturation returns a partial
count or `-1` without blocking. Destroy signals/cancels and boundedly joins the
writer before releasing its pipe and synchronization state. POSIX retains its
nonblocking descriptor implementation under the same accepted-byte contract.

## Consequences

- Studio can display build stdout/stderr in capture order and parse each stream
  incrementally without line splicing.
- Combined output remains bounded and makes loss explicit.
- Windows debug/build input cannot freeze the UI when a child stops reading.
- Process handles own one additional bounded queue; Windows handles also own a
  short-lived writer thread and synchronization objects.
- The public runtime surface gains one additive method and C ABI symbol;
  `WriteStdin` keeps its source and binary signature while tightening its
  nonblocking behavioral contract.

## Alternatives Considered

- **Prefix two independently drained strings in Studio.** Rejected because the
  original relative capture order has already been lost.
- **Merge stderr into stdout at process creation.** Rejected because stream
  identity is useful for diagnostics, protocols, and presentation.
- **Perform one small synchronous Win32 write.** Rejected because even a small
  write can block once the pipe is full.
- **Use an unbounded writer queue.** Rejected because a non-reading child could
  turn process input into unbounded IDE memory growth.
