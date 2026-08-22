---
status: active
audience: contributors
last-verified: 2026-08-21
---

# ADR 0291: Make the BASIC Compiler Authoritative for Signature Help

## Status

Accepted

## Context

Studio used the BASIC compiler for diagnostics, completion, hover, and document
symbols, but independently re-parsed declarations for signature help in Zia.
That scanner could drift from the actual grammar, type suffixes, class methods,
parameter modifiers, object types, and parser recovery behavior.

Adding a language-service method changes the runtime C ABI and registry surface.

## Decision

Add `Zanna.Basic.LanguageService.SignatureInfoForFile`, backed by
`rt_basic_completion_signature_info_for_file`. The bridge locates the active
call structurally, then derives overload names, parameters, modifiers, array
shape, and return types from the parsed and semantically analyzed BASIC AST.
Studio's asynchronous signature controller calls this compiler service.

The bounded Studio scanner remains an indexing mechanism for multi-file
definition, reference, rename, and call-hierarchy queries; it is no longer the
authority for an editor semantic result already available from the compiler.

## Consequences

- Signature help follows the same declarations the compiler accepts.
- Class methods and BASIC parameter details share parser-owned AST metadata.
- The weak runtime stub preserves frontend-free runtime builds by returning an
  unavailable protocol-shaped map.

## Alternatives Considered

Expanding the Zia scanner to mirror every BASIC declaration grammar would
create a second parser and keep the drift risk. Running a command-line language
server for every keystroke would add process and serialization overhead to an
already available in-process frontend.
