---
status: active
audience: contributors
last-verified: 2026-07-02
---

# ADR 0040: Input Key Namespace

## Status

Accepted

## Context

Keyboard key-code constants were split across state/query classes and game
mirrors. `Zanna.Input.Keyboard` exposed `Key*` properties, while
`Zanna.Game3D.Keys` duplicated many of the same values for 3D examples. That
made it harder to answer the simple public API question: "where do I get a key
code?"

Removing the existing constants would break examples and user code, so the
runtime needs a canonical namespace while preserving compatibility.

## Decision

Add `Zanna.Input.Key` as the canonical static constants class for keyboard key
codes:

- letters: `A` through `Z`;
- top-row digits: `Digit0` through `Digit9`;
- function keys: `F1` through `F12`;
- navigation/editing keys such as `Up`, `PageDown`, `Enter`, and `Escape`;
- modifiers with side-specific names such as `LeftShift` and `RightControl`;
- punctuation with readable names such as `LeftBracket`;
- numpad keys with explicit `Numpad*` names.

`Zanna.Input.Keyboard` remains the owner of keyboard state/query behavior, and
its existing `Key*` constants remain compatibility aliases. `Zanna.Game3D.Keys`
also remains available as a compatibility mirror, but new docs and examples
should import `Zanna.Input.Key`.

## Consequences

- New code has one obvious key-code namespace shared by 2D, GUI, Game3D, and
  action-mapping APIs.
- Existing code using `Keyboard.Key*` or `Game3D.Keys` keeps working.
- Public API audits can allow compatibility mirrors while flagging new
  duplicate key constant namespaces.
- Docs and examples should bind `Key` for constants and `Keyboard` only for
  input state queries.
