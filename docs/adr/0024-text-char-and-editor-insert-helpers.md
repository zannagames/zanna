---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0024: Text/Editing Helpers (Zanna.Text.Char + CodeEditor.InsertAndPlaceCursor)

## Status

Accepted (runtime implemented; Zanna Studio is the intended first consumer). Driven by
the GUI runtime-additions review, recommendation **R7**
(review document retired 2026-07-15; the recommendation ids are the surviving record).

## Context

Two small text-editing primitives are missing, so any code/text editor re-derives
them:

1. **Identifier character classification.** Deciding whether a character can start
   or continue an identifier is needed for completion triggers, word selection, and
   ctrl-arrow navigation. Zanna Studio hand-classifies it inline —
   `app/dispatch_helpers.zia:64-80` (`HasIdentifierInput`) loops a string checking
   `ch >= "a" and ch <= "z"`, the digit range, and `"_"`.
2. **Place the caret inside an insertion.** After inserting a multi-line snippet, the
   caret should land at a specific offset *within* the inserted text, not at its end.
   `editor/completion.zia:724-744` (`PlaceCursorAtInsertedOffset`) walks the inserted
   text by hand, counting newlines, to compute the line/column.

Both are missing standard helpers, not application logic. Adding runtime
functions/methods is a runtime C-ABI surface change, requiring an ADR.

## Decision

### `Zanna.Text.Char` — ASCII identifier classification

A static class with three predicates, each taking a string and classifying its
**first character** (so it drops directly into char-by-char string iteration; empty
strings and non-ASCII leading bytes return false):

- `IsIdentifierStart(ch: str) -> i1` — ASCII letter or `_`.
- `IsIdentifierPart(ch: str) -> i1` — ASCII letter, digit, or `_`.
- `IsAlnum(ch: str) -> i1` — ASCII letter or digit.

Classification uses explicit ASCII ranges (not `ctype.h`), so it is locale- and
platform-independent. Lives in `src/runtime/core/rt_string_advanced.c` (core, always
available — not graphics-gated).

### `Zanna.GUI.CodeEditor.InsertAndPlaceCursor(text: str, caretOffset: i64)`

Insert `text` at the primary cursor, then place the caret `caretOffset` characters
into the inserted text (counting newlines). It captures the pre-insert position,
inserts, advances by the offset, and sets the cursor — composing the editor's
existing `InsertAtCursor` / cursor get/set. The offset→position math
(`rt_codeeditor_advance_position`) is a UTF-8-aware `static inline` (a continuation
byte shares its leading byte's column), shared with the method and unit-tested on its
own; the full method is added to the existing `CodeEditor` class (no new class).

## Consequences

- **Adoption:** `HasIdentifierInput`'s inline character ranges become
  `Char.IsIdentifierPart(ch)`, and `PlaceCursorAtInsertedOffset` becomes a single
  `editor.InsertAndPlaceCursor(text, offset)`. Both generalize to any text-editing
  UI (word selection, completion triggers, snippet insertion).
- **Determinism / cross-platform:** the Char predicates are pure ASCII arithmetic;
  the offset math is pure; both are platform-independent. The editor method composes
  existing self-guarding editor calls.
- **No behavior risk:** purely additive; `Zanna.Text.Char` is new, and the editor
  gains one method.

## Alternatives Considered

- **Take an `i64` codepoint instead of a string.** Rejected as the primary form: the
  consumer iterates a string character by character (single-char substrings), so a
  string argument drops in without a separate codepoint conversion. (Identifier rules
  are ASCII, so the first byte is the codepoint for the cases that matter, and a
  multibyte leading byte correctly classifies as a non-identifier character.)
- **Unicode identifier rules (XID_Start/XID_Continue).** Rejected for now: Zanna Studio
  and the Zanna/Zia/BASIC languages use ASCII identifiers; full Unicode tables are a
  large addition with no current consumer. ASCII matches the hand-rolled code being
  replaced.
- **Expose only the offset math (no `InsertAndPlaceCursor`).** Rejected: the value is
  doing the insert + caret placement atomically against the editor's real cursor;
  leaving the caller to insert and then call a math helper keeps the two-step idiom.
