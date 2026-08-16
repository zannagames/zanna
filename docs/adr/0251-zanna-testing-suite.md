---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0251: `Zanna.Testing` — Non-Fatal Assertions and Golden Digests

## Status

Accepted (2026-08-16)

## Consulted

- `src/runtime/core/rt_trap.c` — the trapping `Diagnostics.Assert*` family
- `src/runtime/core/rt_testing.c` — implementation
- `src/runtime/text/rt_hash.c` — SHA-256 for golden digests
- `src/runtime/text/rt_diff.c` — unified diff, already present
- `src/il/runtime/defs/api/core_crypto.def`, `.../classes/io_text.def` — registry

## Context

Zanna has assertions. It does not have a test harness, and the difference
matters.

`Zanna.Core.Diagnostics.Assert`, `AssertEq`, `AssertEqStr`, `AssertEqNum`,
`AssertGt`, and the rest all funnel into `rt_trap()` (`rt_trap.c`). They abort
the program on the **first** failure. That is the right behaviour for an
invariant check inside library code. It is the wrong behaviour for a test
program, which needs to run every check, report every failure, and finish with a
verdict a script can branch on.

`Zanna.GUI.TestHarness` exists but solves a different problem: it drives a
`Zanna.GUI.App` widget tree (`BindApp`, `SendKey`, `FindById`, `RenderFrame`,
`CaptureHash`). A headless logic test has no widget tree.

There is also no `zanna test` command.

The result is that every Zanna project that wants tests writes the same harness.
The Legacy Baseball audit measured one instance precisely: **44 probe programs,
9,660 lines**, containing 12 private assert helpers, 127 inline
`Say("FAIL …"); ok = false;` sites, 96 verdict-line emissions in 4 different
formats, a shell runner that greps those lines, and a
digest-versus-`EXPECTED_DIGEST` protocol with an `"UNSET"` sentinel — all of it
re-derived per file.

## Decision

Add `Zanna.Testing.Suite` and `Zanna.Testing.Golden`.

### `Zanna.Testing.Suite`

| Member | Signature |
|---|---|
| `New(name)` | `obj(str)` |
| `Check(cond, msg)` | `i1(i1,str)` |
| `EqInt(actual, expected, msg)` | `i1(i64,i64,str)` |
| `EqStr(actual, expected, msg)` | `i1(str,str,str)` |
| `EqNum(actual, expected, epsilon, msg)` | `i1(f64,f64,f64,str)` |
| `InBand(value, lo, hi, msg)` | `i1(f64,f64,f64,str)` |
| `Fail(msg)` / `Note(msg)` | `void(str)` |
| `Failures` / `Checks` / `Name` / `Passed` | properties |
| `Report()` | `i1()` |
| `ExitCode()` | `i64()` |

Design points that are not arbitrary:

- **Nothing traps.** A failed check increments a counter, prints
  `FAIL: <msg> (expected X, got Y)`, and returns `false`. The return value lets
  a caller early-out (`if !t.EqInt(...) { return; }`) without making that the
  default.
- **`EqNum` takes the tolerance as an argument.** `Diagnostics.AssertEqNum`
  hard-codes a 1e-9 relative epsilon. A simulation tolerance and a rendering
  tolerance are not the same number, and picking one silently hides real drift.
  A NaN on either side always fails rather than comparing equal-ish.
- **`InBand` exists** because statistical calibration gates assert that a
  measured rate lands inside a band, not on an exact value. Writing that as two
  `Check`s loses the actual value from the failure message.
- **`Report()` prints exactly one `RESULT: ` line.** That is the whole contract
  a shell runner needs, and `ExitCode()` gives the same verdict as a status so a
  runner can drop the grep entirely.

### `Zanna.Testing.Golden`

| Member | Signature |
|---|---|
| `Digest(text)` | `str(str)` |
| `Check(suite, label, actual, expectedDigest)` | `i1(obj<Suite>,str,str,str)` |

- **The computed digest is always printed**, pass or fail. Re-pinning a
  deliberately-moved golden should be a copy-paste, not a re-derivation.
- **An empty or `"UNSET"` expected digest puts the suite into baseline mode.**
  `Report()` then prints `RESULT: baseline (…)` and `ExitCode()` returns `2`.
  A first run must not be able to read as a pass — that is exactly the failure
  mode a hand-rolled sentinel gets wrong.

## Consequences

- A Zanna project gets a test harness without writing one. The immediate
  measured saving in the audited project is ~400 lines of boilerplate and the
  collapse of 4 verdict formats into 1.
- `Diagnostics.Assert*` keeps its meaning: hard invariant, abort on violation.
  The two families are complementary and the docs should say so.
- **Not addressed here:** there is still no `zanna test` command — discovery,
  parallel execution, and result aggregation remain the project's own shell
  script. `Golden.Diff` was left out because `Zanna.Text.Diff.Unified` already
  covers it and a wrapper would only hide it.
- The suite name is copied into a fixed 96-byte inline buffer, so the object
  owns no heap references and needs no finalizer.
- Registry surface: 2 new classes, 16 new functions. No IL opcode, grammar, or
  verifier changes.

## Links

- ADR 0249, ADR 0250 — the determinism work these tests are written to guard
- `baseball/plans/58-runtime-adoption.md` — the audit that measured the gap
