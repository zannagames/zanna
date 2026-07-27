---
status: active
audience: contributors
last-verified: 2026-07-02
---

# ADR 0039: Option Failure Aliases

## Status

Accepted

## Context

The runtime overhaul identified several public value-producing `Try*` APIs that
used nullable objects, empty strings, or numeric sentinels to represent absence:

- collection helpers such as `Queue.TryPop`, `Stack.TryPop`,
  `Heap.TryPop`, `Heap.TryPeek`, and deque pop helpers;
- thread helpers such as `ConcurrentQueue.TryDequeue`, `Channel.TryRecv`, and
  `Future.TryGet`;
- `DateTime.TryParse`, where failure and a valid Unix epoch parse both returned
  `0`;
- localization helpers such as `Locale.TryParse` and
  `MessageBundle.TryGet`;
- `NumberFormat.TryParse*`, which already returned `Option` objects but were
  still declared with plain `obj` signatures.

Changing existing signatures would break current programs, but leaving the
runtime without explicit optional forms keeps the public API difficult to learn
and unsafe for generated code.

## Decision

Keep all existing sentinel and nullable APIs for compatibility, and add modern
Option-returning aliases for new code:

- `Queue.TryPopOption`
- `Stack.TryPopOption`
- `Heap.TryPopOption`
- `Heap.TryPeekOption`
- `Deque.TryPopFrontOption`
- `Deque.TryPopBackOption`
- `ConcurrentQueue.TryDequeueOption`
- `Channel.TryRecvOption`
- `Future.TryGetOption`
- `DateTime.TryParseOption`
- `Locale.TryParseOption`
- `MessageBundle.TryGetOption`

Add `MessageBundle.GetOr(key, default)` for the common fallback-value case.
Correct `NumberFormat.TryParseDecimal`, `TryParseInteger`, and
`TryParseCurrency` signatures to `obj<Zanna.Option>` because their
implementations already returned `Option`.

The Option aliases distinguish absence from a present null or empty value. For
example, `TryPopOption` returns `None` only when no collection element exists;
if a collection stores a null object, it returns `Some(NULL)`.

Also type Future-producing registry signatures as
`obj<Zanna.Threads.Future>` (`Promise.GetFuture` and `Async.*`) so code can
call `Future` methods such as `TryGetOption` without explicit annotations.

## Consequences

- Existing source code keeps compiling against the legacy nullable/sentinel
  helpers.
- New code and docs can teach one clear absence vocabulary: `Option`.
- Runtime ownership metadata must mark the new helpers as owned, allocating
  results so optimizer passes preserve object lifetimes.
- Future factory metadata must preserve the concrete `Future` class type; plain
  `obj` return signatures make chained Future calls harder for frontends to
  resolve.
- A later breaking cleanup can consider collapsing canonical `Try*` names to
  Option signatures after compatibility policy allows it.
