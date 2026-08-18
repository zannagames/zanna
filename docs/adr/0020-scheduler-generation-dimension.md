---
status: active
audience: contributors
last-verified: 2026-07-01
---

# ADR 0020: Revision-Aware Scheduling (Zanna.Threads.Scheduler generations)

## Status

Accepted (runtime implemented; Zanna Studio's editor scheduler is the intended first
consumer). Driven by the GUI runtime-additions review, recommendation **R3**
(review document retired 2026-07-15; the recommendation ids are the surviving record).

## Context

Responsive editors do debounced background work: after the user stops typing for
N ms, run diagnostics / completion / hover for the *current* document revision.
The hard part is **supersession** — work queued for `(document, revisionN)` must
be discarded the instant `revisionN+1` is queued, or a stale result paints over
fresh input.

The runtime already ships the timing primitives this needs under `Zanna.Threads.*`:
`Scheduler` (`Schedule(key, delayMs)` / `Cancel` / `IsDue` / `Poll` / `Pending` /
`Clear`), `Debouncer`, and `Throttler`. The `Scheduler` is poll-based, uses the
monotonic clock, and **already debounces** — re-scheduling the same key replaces
its due time. Zanna Studio *uses* these for simple cases (`semantic_tokens.zia`,
`inlay_hints.zia`, autosave, `workspace_watcher.zia`).

But its core `editor/scheduler.zia` (435 LOC) can't be built on `Scheduler`,
because the one thing it needs the API can't express: a key's due entry carries no
**identity over time**. `IsDue("diag:foo")` tells you the timer elapsed, not
*which* revision's work elapsed — so it can't tell a current result from a
superseded one. Zanna Studio therefore hand-rolls timing on raw `Zanna.Time.Clock.Ticks()`,
a pattern that recurs in `editor/hover.zia`, `editor/diagnostics.zia`,
`editor/completion.zia` (15+ sites).

This is a missing dimension on an existing primitive, not a missing primitive.
Adding runtime methods is a runtime C-ABI surface change, which requires an ADR.

## Decision

Add a per-entry **generation** (an `int64_t` the caller supplies — Zanna Studio passes
the document revision) to `Zanna.Threads.Scheduler`, with three new methods:

- `ScheduleGen(key: str, delayMs: i64, generation: i64)` — schedule/reschedule
  `key` to fire after `delayMs`, tagged with `generation`. Re-scheduling a key
  replaces both its due time and its generation (the existing debounce, now
  carrying identity).
- `IsDueGen(key: str, generation: i64) -> i1` — `1` iff `key` is due **and** its
  stored generation equals `generation`. A newer `ScheduleGen` replaces the entry,
  so a query for the old generation returns `0` (superseded).
- `GenerationOf(key: str) -> i64` — the generation currently scheduled for `key`,
  or `-1` if `key` isn't scheduled. Lets a caller ask "is my revision still the
  one queued?" (the `IsQueued` predicate Zanna Studio needs).

The existing `Schedule(key, delayMs)` is unchanged and tags entries with
generation `0`; `IsDue`, `Poll`, `Cancel`, `Pending`, and `Clear` are unchanged
and ignore the generation. The supersession identity:

```text
IsDueGen(key, g) == (IsDue(key) && GenerationOf(key) == g)
```

### Implementation

A single `int64_t generation` field is added to the internal `sched_entry`; the
existing `rt_scheduler_schedule` body is refactored into a shared
`scheduler_schedule_impl(..., generation)` that both `Schedule` (generation `0`)
and `ScheduleGen` call, so the retain/trap-recovery/locking logic is written once.
`IsDueGen` mirrors `IsDue` with an added generation compare; `GenerationOf` mirrors
the lookup and returns the field. No new class, no new class id, no new file (so no
`source_health` runtime-contract surface change). The scheduler's existing mutex
makes the new methods thread-safe like the rest.

## Consequences

- **Adoption:** Zanna Studio's revision-keyed job logic (`Queue` / `IsQueued` / `IsDue`
  with a `generation` token) maps directly onto `ScheduleGen` / `GenerationOf` /
  `IsDueGen`, letting `editor/scheduler.zia` and the 15+ raw-`GetTickCount` sites
  collapse onto the runtime primitive. Any responsive app (search-as-you-type,
  live preview, incremental indexing) gets edit-superseding scheduling for free.
- **Determinism / cross-platform:** pure additive bookkeeping over the existing
  monotonic-clock scheduler; no new OS surface, no platform `#ifdef`.
- **No behavior risk:** existing methods are untouched; `Schedule` simply records
  generation `0`.

## Alternatives Considered

- **A new `Zanna.GUI.IdleScheduler` class.** Rejected — the report's central point:
  the `Scheduler`'s timing, locking, monotonic clock, and debounce are already
  correct; only identity was missing. A new class would duplicate all of that.
- **Encode the revision into the key (`"diag:foo@5"`).** Rejected: each revision
  becomes a *distinct* key, so the old entry isn't replaced (it lingers until it
  fires or is cancelled), `Pending` grows unbounded under typing, and there's no
  cheap "is the latest revision due?" query. Replacement-by-key + a generation
  field is what makes supersession O(1) and self-cleaning.
- **A generation-aware bulk drain `PollGen() -> Seq(Map)`.** Deferred: the driving
  consumer (the editor) checks per key with `IsDueGen`/`GenerationOf`, not a bulk
  drain, and the existing `Poll()` still drains due keys by name. `PollGen` can be
  added when a consumer needs generation-tagged bulk draining.
