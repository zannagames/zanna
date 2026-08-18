---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0126: Make HTTP Client Identity and Ownership Transactional

## Status

Accepted

## Context

The native HTTP client allocated public `HttpReq` and `HttpRes` objects and its
private keep-alive pool with class identifier zero. Public entry points then
cast any non-null managed pointer directly to the corresponding payload. A Seq,
Bytes, stale address, or another zero-tagged object could therefore be read as
native request pointers, response lengths, or a mutex-bearing pool.

Request body and header setters destroyed the old value before every new
allocation had succeeded. A rejected Bytes handle or allocation trap could
silently clear a reusable request. Header parsing and response publication mixed
native and managed ownership across nested trap frames; failures while copying
headers, following redirects, decoding gzip, or wrapping a response could leak
partial Maps, Strings, bodies, connections, and cloned requests. `SendResult`
also attempted to allocate its diagnostic Result from the recovery frame that
caught the original request trap, making diagnostic OOM recursive and leaving
producer references unbalanced.

One-shot helpers duplicated ownership logic for each method and returned newly
allocated empty fallback objects after some failures when an embedder's trap
hook resumed. The process-wide default keep-alive pool cached an initialization
failure forever. `Http.Download` wrote directly through a predictable temp-name
and backup sequence, did not synchronize completed content before publication,
and could expose replacement races or lose existing permissions.

Adding stable native class identifiers changes the runtime C boundary contract.
The connection-pool initialization protocol and portable atomic size adapter
also establish cross-platform runtime contracts, so this decision is recorded
explicitly. Language-visible class names, methods, signatures, and features are
preserved.

## Decision

- Reserve `RT_HTTP_REQ_CLASS_ID` as `-0x72020A`,
  `RT_HTTP_RES_CLASS_ID` as `-0x72020B`, and
  `RT_HTTP_CONN_POOL_CLASS_ID` as `-0x72020C`. Validate both class identity and
  full payload size before every non-null receiver is accessed. Preserve the
  historical neutral result for NULL response accessors, but trap on other
  invalid handles.
- Install request, response, and pool finalizers before fallible subordinate
  construction. A partially initialized finalizer must be safe, and native
  mutex initialization must complete before the pool is published.
- Make request updates transactional. Validate managed String/Bytes/pool
  identity first, allocate a complete replacement body or header node, and only
  then exchange it with the prior value. `SetHeader` still replaces every
  case-insensitive match; `AddHeader` still preserves repeated fields.
- Use one ownership-aware implementation for the static string and Bytes HTTP
  methods. Copy caller-owned String bodies, borrow validated Bytes only for the
  synchronous transaction, and keep all temporary request state in heap storage
  that remains defined across `longjmp`. A failed operation returns NULL after
  trapping; it does not manufacture an empty success value.
- Construct `Result.ErrStr` and `Result.Ok(HttpRes)` under fresh recovery frames
  after clearing the request frame. Balance the initial String or response
  reference after Result retains it, and clean every partial value if Result
  construction itself traps.
- Treat native response reading as one transaction. Recovery handlers close or
  return the active connection and release partial status text, header lines,
  Maps, boxed values, bodies, gzip staging, redirect Locations, and cloned
  requests before re-raising the saved categorized diagnostic.
- Convert HTTP/2 headers and response transforms under the same ownership rules
  as HTTP/1.1. Response headers remain a managed Map with lowercase keys and
  joined duplicate values. Public `Headers`, `Header`, and `Body` accessors
  return independent snapshots and release partial snapshots on allocation
  failure.
- Initialize the default HTTP keep-alive pool with a retryable atomic state
  machine. Exactly one thread constructs it, waiters yield while construction is
  active, and a trap returns the state to uninitialized so a later request can
  retry. Retained pool setters validate again after acquiring a live reference.
- Keep `Http.Download` as a non-trapping Boolean API. Validate String identity
  and embedded NUL bytes before transport or filesystem access. Stream into an
  exclusively created random sibling file, flush and synchronize it, preserve
  ordinary permissions from an existing destination, and atomically publish it
  with POSIX `rename` or Windows `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`.
  Remove staged content on every pre-publication failure. Do not copy set-user,
  set-group, or sticky bits.
- Route the download-name counter through the portable
  `rt_atomic_fetch_add_size` adapter. GCC/Clang and MSVC implementations must
  provide the same acquire/release order parameter contract without raw
  platform atomics in HTTP code.

## Consequences

- Forged or wrong-class managed handles cannot reach request pointers, response
  buffers, or pool mutexes.
- Rejected and allocation-failing mutations leave reusable requests unchanged.
- Every parser, redirect, decompression, publication, and Result-construction
  failure has a single owner-driven cleanup path. Returning trap hooks observe
  one failure and no fabricated fallback object.
- The default pool can recover from transient startup OOM and remains safe under
  concurrent first use. Existing keep-alive behavior and all request features
  remain available.
- Successful downloads are published only after data synchronization. Existing
  ordinary permissions survive replacement, while failed downloads leave no
  staged file and do not replace the destination.
- Download replacement remains atomic for regular files on the same filesystem.
  Directory durability after a host crash is not promised because the parent
  directory itself is not synchronized.
- Public registry signatures and language APIs do not change. The three class
  identifiers and the portable size-atomic adapter extend the native runtime
  contract.

## Verification

- `test_rt_network` verifies stable request, response, and HTTP-pool identities;
  transactional body/pool replacement; wrong-class receiver rejection; exact
  header snapshot cleanup; complete one-shot and `SendResult` allocation sweeps;
  redirects; gzip transformation; streaming; destination permission
  preservation; embedded-NUL rejection; and staged-file removal after an
  injected response allocation trap.
- `test_rt_trap_return_network` verifies one-shot validation raises exactly one
  trap and returns NULL when an embedder resumes, while `Http.Download` rejects
  forged handles without trapping.
- The runtime network documentation records snapshot ownership, stable receiver
  identity, Result behavior, retryable default pooling, and transactional
  download publication.

## Alternatives Considered

- Keep class identifier zero and validate only payload size: rejected because
  unrelated zero-tagged objects can satisfy the same size check.
- Clear request state before allocating replacements: rejected because a
  recoverable validation or allocation failure must not mutate a reusable
  request.
- Duplicate one-shot implementations per HTTP verb: rejected because their
  ownership rules had already drifted and every new method multiplied cleanup
  paths.
- Build error Results in the request recovery handler: rejected because
  allocation failure can jump back into the same handler and retry indefinitely.
- Permanently cache default-pool construction failure: rejected because
  transient OOM should not disable keep-alive for the remainder of the process.
- Publish downloads through a predictable backup rename: rejected because the
  extra pathname creates collision and replacement races without improving the
  atomic replacement guarantee.
- Remove or narrow existing HTTP methods: rejected because the hardening work
  must preserve the complete runtime feature surface.
