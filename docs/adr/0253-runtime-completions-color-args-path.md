---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0253: Runtime Completions — Additive Colour, CLI Arguments, Data-Root Search

## Status

Accepted (2026-08-16)

## Consulted

- `src/runtime/graphics/2d/rt_color.c` — the existing percentage-based ops
- `src/runtime/core/rt_args.c` — the raw argument store
- `src/runtime/io/rt_path.c` — path composition
- `src/il/runtime/defs/api/graphics2d.def`, `.../core_crypto.def`,
  `.../audio_io.def` — registry

## Context

Three unrelated holes, grouped because each is small, each was found the same
way (an audit of the largest real Zanna application), and each is the kind of
thing that gets rewritten slightly differently in every project.

### A. `Color` has no additive channel operation

`Zanna.Graphics.Color` has 30 members and a conspicuous gap. `Brighten(c, n)`
and `Darken(c, n)` move each channel by a **percentage** of its remaining
distance to 255 or 0 (`rt_color.c`):

```c
r = r + (255 - r) * amount / 100;
```

That is right for a UI tint. It is wrong for indexed pixel art, where a
palette's tones are defined by a *constant* luminance step — a percentage move
changes the spacing between them non-linearly, so a three-tone sprite recipe
comes out wrong. Both operations are needed; neither substitutes for the other,
and only one existed.

The audited project's `theme.lighten`/`darken` are additive for exactly this
reason, and its procedural sprite bank depends on the constant step.

### B. The packed↔float colour boundary was one-sided

`Color.Rgb(i64,i64,i64)` and `GetRed(i64)` handle packed integers.
`Material3D.PBR` takes `f64` channels in 0..1. Every 3D program therefore
hand-writes `(GetRed(c) + 0.0) / 255.0` three times per material. There is also
no luminance accessor, so tone-mapping and contrast checks re-derive the Rec.709
weights inline.

### C. Two universal one-liners that are not one-liners

- **CLI parsing.** `Zanna.System.Environment` exposes `GetArgumentCount` and
  `GetArgument`. There is nothing above that, so every CLI-facing program writes
  the same linear scan for "was `--flag` passed?" and "what followed `--opt`?".
- **Data-root discovery.** "Find my `media/` from wherever I was launched" has
  no runtime support, and getting it wrong **fails silently**: the program runs,
  finds nothing, and quietly renders placeholder content. The audited project's
  asset-root module documents that exact incident in its header, and a second
  copy of the same walk exists elsewhere in the same tree because the two
  modules may not bind each other.

## Decision

### `Zanna.Graphics.Color`

| Member | Signature | Notes |
|---|---|---|
| `AddChannels(color, amount)` | `i64(i64,i64)` | Signed constant offset, clamped 0..255. The complement of `Brighten`/`Darken`, not a replacement. |
| `ScaleRgb(color, factor)` | `i64(i64,f64)` | Multiplicative, clamped. |
| `RgbF(r, g, b)` | `i64(f64,f64,f64)` | Pack 0..1 floats. |
| `GetRedF` / `GetGreenF` / `GetBlueF` | `f64(i64)` | Unpack to 0..1. |
| `Luma(color)` | `f64(i64)` | **Rec.709** relative luminance, 0..1. Distinct from `Grayscale`, which uses Rec.601 weights and returns a *colour*; this returns the scalar. |

Alpha and explicit-alpha intent are preserved by every new operation, matching
the existing family.

### `Zanna.System.Args`

| Member | Signature |
|---|---|
| `HasFlag(name)` | `i1(str)` |
| `GetOption(name, fallback)` | `str(str,str)` |
| `GetOptionInt(name, fallback)` | `i64(str,i64)` |
| `Positionals()` | `seq<str>()` |

- Accepts both `--opt value` and `--opt=value`.
- A trailing `--opt` with nothing after it returns the fallback rather than an
  empty string, so "absent" and "explicitly empty" stay distinguishable.
- A bare `--` ends option processing — the convention every shell user expects.
- **Deliberately minimal**: no spec object, no usage generation, no subcommands.
  Those are opinionated, and a program needing them will want its own. The part
  that is genuinely shared is the scan.

### `Zanna.IO.Path`

| Member | Signature |
|---|---|
| `FindUpward(startDir, relativeMarker, maxLevels)` | `str(str,str,i64)` |
| `ResolveDataRoot(envVar, relativeMarker)` | `str(str,str)` |

- Returns the directory **containing** the marker, because that is what callers
  then join against.
- The marker is probed as both a file and a directory — a project root is as
  often identified by `assets/` as by `project.json`.
- The level bound is mandatory and clamped to 64, so a missing marker cannot
  become an unbounded climb to `/`.
- `ResolveDataRoot` checks an environment override first (packaged builds, test
  harnesses) and falls back to the walk. It returns **empty** rather than a
  guess when unresolved: a caller that silently accepts a wrong root is exactly
  the failure this is meant to prevent.

## Consequences

- The additive/percentage distinction is now expressible, which unblocks
  retiring hand-rolled `lighten`/`darken` pairs. Note this is *not* a
  behaviour-neutral swap for anything already using percentage ops.
- `GetRedF`/`RgbF` remove the divides from every PBR call site.
- `Positionals()` returns an **owning** Seq; the retained references from
  `rt_args_get` transfer in without a second retain.
- **Not addressed here:** histogram/statistics (`Mean`, `Median`, `Percentile`),
  pixel statistics for render-regression gates, instance-batch tagging, a
  carry-corrected integer delta clock, LUT texel addressing, a retained event
  log, and `List↔Seq` bridging. All were identified by the same audit and remain
  open.
- Registry surface: 1 new class (`Zanna.System.Args`), 13 new functions. No IL
  opcode, grammar, or verifier changes.

## Links

- ADR 0249–0252 — the other completions from the same audit
- `baseball/plans/58-runtime-adoption.md` — the audit
