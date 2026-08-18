---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0120: Keep HTTP Router Publication Stable and Matching Trap-Safe

## Status

Accepted

## Context

`HttpRouter` permits route registration at the same time as matching. Its first
synchronization pass protected one contiguous route array with a reader-writer
lock, but `Match` also allocated a parameter Map, capture strings, and the
`RouteMatch` object before releasing the read lock. A recoverable runtime trap
uses `longjmp`, so an allocation failure could bypass the unlock and leave the
router permanently deadlocked. Allocating an empty Map before testing every
same-method candidate also made a miss proportional in both route checks and
managed allocation churn.

Moving result construction outside the lock requires the selected route's
address to remain valid while another thread appends a route. A contiguous
array cannot provide that property because capacity growth relocates every
element. The router and match objects also used managed class id zero and their
public accessors cast opaque payloads without checking allocation size. A
wrong-class or undersized managed object could therefore be interpreted as
private router state.

These changes define managed class tags and opaque-handle validation at the
runtime C ABI boundary, so an ADR is required.

## Decision

- Assign stable class tags `RT_HTTP_ROUTER_CLASS_ID` and
  `RT_ROUTE_MATCH_CLASS_ID`. Every public entry point validates both class and
  minimum private payload size before casting a non-null opaque handle.
- Store each immutable route in a separately allocated node. The router keeps
  a growable pointer table in registration order. Appending can relocate the
  table, but never a published node selected by a concurrent reader.
- Split matching into two phases. Under the shared lock, compare cached method
  and segment lengths and select the first matching route without allocating or
  trapping. Release the lock before allocating capture strings, the parameter
  Map, the copied pattern, or the `RouteMatch` object.
- Protect result construction with one local trap-recovery boundary. Until
  construction succeeds, every partially owned Map, string, and match object
  remains in an explicit cleanup slot. Cleanup restores the original trap
  diagnostic after releasing those references.
- Do not allocate an empty parameter Map for a literal-only route. A missing
  Map has the same public `Param` behavior as an empty Map.
- Treat reader-writer lock initialization as mandatory. Constructor failure
  releases the fresh managed router and traps; it never publishes an
  unsynchronized fallback object.
- Validate registration methods as non-empty, case-sensitive HTTP tokens.
  Preserve extension methods that use the token punctuation alphabet. Reject
  lone `:` and `*` capture markers, require a non-wildcard capture to consume a
  non-empty request segment, retain pattern-side repeated-slash normalization,
  and retain terminal wildcard and duplicate-name validation.
- Cache exact method, pattern, and segment byte lengths. Matching and result
  construction use bounded byte spans rather than repeated `strlen` scans.

## Consequences

- A managed allocation trap cannot strand the router's shared lock. Failed
  match construction leaves no tracked parameter Map or partial result.
- Route misses perform no managed allocation. Literal matches allocate only
  their result object; capture matches allocate only the selected route's
  result state.
- Concurrent Add, Match, and Count preserve first-registration-wins behavior,
  including across pointer-table reallocations.
- Each route uses one additional small native allocation. This stable-node cost
  is accepted in exchange for short lock hold times and trap-safe construction.
- Wrong-class and undersized handles trap at the API boundary instead of
  reading unrelated payload memory.
- Invalid HTTP methods and nameless capture markers now fail during
  registration, leaving the router unchanged.

## Alternatives Considered

- Keep the contiguous array and copy a selected route while locked: rejected
  because copying every string and segment can itself allocate and trap under
  the lock, while a shallow copy does not own stable pointed-to storage.
- Retain the read lock through construction and manually check every
  allocation: rejected because nested Map/string helpers may trap non-locally.
- Freeze a router after registration: rejected because the existing public
  contract intentionally supports concurrent registration and matching.
- Build parameter Maps speculatively for every candidate: rejected because
  route compatibility can be decided without allocation.
