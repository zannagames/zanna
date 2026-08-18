---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0028: Terminal Option and Result Input APIs

## Status

Superseded in part — the four APIs added here (`TryReadLine`, `TryAsk`,
`ReadLineResult`, `AskResult`) remain the registered public terminal-input
surface, but the public-surface standardization retired `Zanna.Terminal.Ask`
and `Zanna.Terminal.InputLine`. Only `Zanna.Terminal.ReadLine` survives from
the nullable trio named in the Decision below (VDOC-287).

## Context

`Zanna.Terminal.ReadLine` and `Zanna.Terminal.Ask` historically returned
nullable strings. That shape preserves compatibility with earlier Zia, BASIC,
and IL examples, but it is not the clearest public API for robust applications:
EOF is an expected outcome, while allocation failures and overlong input are
runtime failures. New user-facing APIs need to communicate that distinction
without deleting the existing nullable functions.

The runtime already exposes `Zanna.Option` and `Zanna.Result`, so terminal input
can use the same explicit success/failure vocabulary as parsing and other
modern runtime surfaces.

## Decision

Add four terminal input functions while preserving all existing functions:

- `Zanna.Terminal.TryReadLine() -> Option<String>`
- `Zanna.Terminal.TryAsk(prompt: String) -> Option<String>`
- `Zanna.Terminal.ReadLineResult() -> Result<String, String>`
- `Zanna.Terminal.AskResult(prompt: String) -> Result<String, String>`

EOF before any bytes are read is represented as `None` for the `Try*` APIs and
as `Err(String)` for the `*Result` APIs. Input allocation failures, overlong
lines, and other fatal runtime errors continue to trap. `ReadLine`, `Ask`, and
`InputLine` remain registered for source and IL compatibility.

## Consequences

New examples and documentation should prefer `TryReadLine` or
`ReadLineResult` depending on whether the caller needs a simple optional value
or a diagnostic string. Existing nullable calls continue to work, but they are
documented as compatibility APIs rather than the recommended public shape.

The runtime API dump reports these APIs as object-returning calls with
`option`/`result` fallibility metadata, which gives tools a structured way to
prefer the modern APIs during migrations.
