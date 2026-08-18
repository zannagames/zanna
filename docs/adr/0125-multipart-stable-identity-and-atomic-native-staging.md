---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0125: Give Multipart Stable Identity and Atomic Native Staging

## Status

Accepted

## Context

`Zanna.Network.Multipart` objects were allocated with class identifier zero and
public methods cast any non-null managed pointer directly to their private
payload. Passing a Seq, Bytes, freed address, or another zero-tagged runtime
object could therefore read arbitrary counts and native pointers. The parser
also allocated its Multipart before most structural checks and raised traps
without releasing the partial object. Malformed input and allocation failures
could leak all parts parsed before the failure.

`Build` calculated its size up front but allocated escaped copies of every name
and filename during the write pass. Most failures reported a trap and then
allocated an empty Bytes fallback. A returning embedder trap hook therefore
continued failed control flow; a secondary OOM could obscure the original
error. If final managed Bytes allocation trapped, the fully populated native
staging buffer leaked.

`ParseResult` attempted to allocate its diagnostic String while the same parser
recovery frame was active. Diagnostic OOM jumped back into that handler and
could repeat indefinitely. Its success Result retained the parsed Multipart but
did not release the parser's initial reference, and its error Result similarly
leaked the initial diagnostic String reference.

Adding a stable native class identifier to the public Multipart header changes
the runtime C boundary contract and therefore requires this ADR. Registered
language method names and signatures remain unchanged.

## Decision

- Reserve `RT_MULTIPART_CLASS_ID` as `-0x720209`. Constructors and successful
  parses store it in the heap header. Receiver methods validate class identity
  and the full private payload size before field access.
- Preserve all existing builder features: fluent mutation, random boundaries,
  embedded NUL bytes in field values, optional NULL file data as an empty file,
  quote/backslash escaping, and control-byte sanitization. Required field names
  must be non-empty and header parameters still reject embedded NUL bytes.
- Validate file payloads as Bytes before reading their storage. Validate all
  String handles before borrowing C-string views. Check part-count growth before
  signed addition and keep geometric native part storage.
- Serialize in two passes. The first computes the exact size with checked
  arithmetic; the second writes boundaries, headers, escaped parameters, and
  bodies directly into one native buffer. Do not allocate per-part escaped
  copies.
- Return NULL after a `Build` trap instead of allocating fallback Bytes. Cover
  final managed Bytes construction with a recovery frame that releases native
  staging and any partial result before re-raising.
- Validate Content-Type and Bytes input before parser object allocation. Accept
  only complete delimiter tokens at body start or a CRLF line boundary, reject
  boundary-prefix false matches, require CRLF framing, reject unterminated
  quoted parameters, and cap each part-header block at 64 KiB in addition to the
  existing 64 MiB body cap.
- Run parser object allocation and the complete part loop under one recovery
  frame. Track the current native header copy separately. On any trap, release
  it and finalize/free the partial Multipart before propagating the saved
  diagnostic.
- Build ParseResult diagnostics only after clearing parser recovery, under a
  fresh allocation-recovery frame. On success, let Result retain the Multipart
  and then consume its initial parse reference. Apply the same balanced retain
  rule to error Strings.

## Consequences

- Wrong-class managed receivers cannot corrupt memory through Multipart APIs.
- Malformed, truncated, oversized, or allocation-failing parses remain strict
  and atomic without retaining partial native part graphs.
- Serialization uses one native allocation regardless of part count, reducing
  allocation traffic and eliminating per-part OOM branches.
- A returning trap hook observes one Build failure and NULL, not a newly
  allocated empty Bytes value. Missing-value getters continue returning their
  historical empty String/Bytes values during ordinary non-error lookup.
- Parser boundary handling is intentionally stricter. Inputs with LF-only
  delimiters, unterminated quoted parameters, oversized header sections, or a
  boundary token followed by extra token bytes are rejected rather than
  ambiguously parsed.
- The class-ID macro expands the native header surface but no language-visible
  method, feature, or registry signature is removed or renamed.

## Verification

- `test_rt_multipart` verifies wrong-class rejection, native Build staging
  cleanup on injected Bytes OOM, partial parser cleanup, successful and error
  ParseResult ownership, existing escaping, quoted/bare parameters, empty parts,
  final delimiters, and embedded-NUL field round trips.
- `test_rt_trap_return_network` verifies invalid Multipart Build raises exactly
  one trap and returns NULL when the embedder hook resumes.
- Runtime network documentation records identity, limits, strict delimiter
  rules, ownership, and the allocation-light serializer.

## Alternatives Considered

- Keep class identifier zero and validate only payload size: rejected because
  unrelated zero-tagged objects can have equal or larger payloads.
- Return an empty Bytes value after Build failure: rejected because it makes a
  trap look like successful empty serialization and allocates after failure.
- Escape each parameter into a temporary allocation: rejected because exact
  sizing already permits direct output and the temporary work grows with part
  count.
- Let ParseResult borrow the parsed object or diagnostic String: rejected
  because Result finalizers own retained payloads and every constructor must
  balance the producer's initial reference explicitly.
- Make parsing lenient and return partial parts: rejected because callers cannot
  distinguish a complete upload from a truncated one; the existing strict
  feature is preserved.
