---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0249: Replayable RNG Stream State and Cross-Run Stable Integer Mixing

## Status

Accepted (2026-08-16)

## Consulted

- `src/runtime/core/rt_random.c` — LCG stream and instance payload
- `src/runtime/core/rt_math.c` — integer mixing
- `src/il/runtime/defs/api/math_text.def` — registry surface
- `src/il/runtime/defs/classes/io_text.def` — `Zanna.Math.Random` class block
- `docs/languages/arithmetic-semantics.md` — checked-arithmetic policy

## Context

Two gaps came out of a full audit of the Legacy Baseball tree (52,641 lines of
Zia, the largest real consumer of the runtime). Both forced that project to
hand-write primitives the runtime should own, and both will hit every future
Zanna game the same way.

### A. A `Random` stream's cursor was write-only

`Zanna.Math.Random` instances already carry their own `uint64_t` state
(`rt_random.c`, `rt_random_impl`), and `rt_random_new` seeds it with no warm-up
step, so a stream is fully described by one integer. But nothing exposed it:
`inst_Seed` is registered `RT_INTERNAL_FUNC`, and there was no getter at all.

That makes three ordinary things impossible:

- **Save games.** Persisting "where the injury stream had got to" requires
  reading the cursor.
- **Replay and checkpointing.** Resuming mid-sequence requires writing it.
- **Per-subsystem stream layout.** Deriving decorrelated child streams from one
  run seed requires a defined derivation, not an ad-hoc prime multiply.

Baseball's response was to abandon the runtime generator entirely and hand-roll
**two** xorshift32 implementations with rejection sampling — one for the season
layer (`season_rng.zia`, whose `state` field is round-tripped through the save
file) and one for the single-game engine (`sim/rng.zia`, with explicit
`snapshot()` / `restore()` / `drawCount()`). Both re-derive bounded sampling
that `rt_random_bounded_u64_from_state` already does correctly.

### B. There is no replay-stable integer hash

`Zanna.Crypto.Hash.NonCryptoFastInt` looks like the primitive for this, but its
own contract says otherwise (`rt_hash.c`): *"Values are stable only within one
process run."* It is keyed SipHash. Using it for anything replayable is a bug.

There is also no wrapping arithmetic: `docs/languages/arithmetic-semantics.md`
states there is *"no public wrapping arithmetic path at the IL level for signed
integers,"* and Zia's `Integer` arithmetic traps on overflow. So a Zia program
cannot even write a conventional 64-bit mixer.

Baseball's `watch3d/synth/seq_hash.zia` documents the workaround verbatim:
*"Zia Integer overflow TRAPS, so the mixer works in 32-bit space with ≤16-bit
multiplier constants (max intermediate < 2^48)."* It hand-builds a Wang-style
32-bit avalanche out of `Zanna.Math.Bits` calls. Two more mixers exist elsewhere
in the same tree. All of this exists because the presentation layer is
*forbidden* from binding an RNG (a static bind-graph guard enforces it) yet
still needs deterministic per-event variety.

## Decision

### `Zanna.Math.Random` — stream state

| Member | Signature | Behaviour |
|---|---|---|
| `State` | property `i64` | Reads/writes the instance cursor. Every 64-bit pattern is a valid state (the LCG has full period), so no normalization is applied. |
| `Clone()` | `obj<Random>()` | Forks an independent stream at the parent's current position. Neither stream perturbs the other. |
| `Derive(ns)` | `obj<Random>(i64)` | Child seed is `mix64(parent_state ^ mix64(ns))`. **Does not advance the parent**, so derivation is reproducible regardless of call order — the property a per-subsystem stream layout depends on. |
| `GlobalState` | static property `i64` | Checkpoints the effective context's shared cursor, the one the static draw functions use. Acquisition follows the same RNG-lock discipline as those draws. |
| `HashRange(seed, seq, salt, lo, hi)` | `i64(i64,i64,i64,i64,i64)` | **Stateless.** Consumes no stream and mutates nothing; a pure function of its arguments. Rejection-sampled so the range is uniform. This is the shape a presentation or replay layer needs when it must be reproducible under arbitrary seeking. |
| `ChancePercent(percent)` | `i1(i64)` | Integer-percent probability. `<= 0` and `>= 100` short-circuit **without advancing**, matching the guard every caller writes by hand. Avoids the float rounding that makes `Chance(f64)` awkward for replayable logic. |

### `Zanna.Math` — stable mixing

| Member | Signature |
|---|---|
| `Mix(x)` | `i64(i64)` |
| `Mix2(a, b)` | `i64(i64,i64)` |
| `Mix3(a, b, c)` | `i64(i64,i64,i64)` |

splitmix64 finalizers. `Mix3` is the `(seed, sequence, salt)` shape: it lets
independent consumers of the same event decorrelate without any of them owning
RNG state.

**Stability is the contract.** The mapping is fixed: the same input yields the
same output in every process, on every backend, in every future runtime
version. That is exactly what separates these from
`Zanna.Crypto.Hash.NonCryptoFastInt`, and it is why they live under
`Zanna.Math` rather than `Zanna.Crypto` — they are deliberately not
cryptographic, and filing them under `Crypto` would invite misuse.

## Consequences

- Save-games, replays, and deterministic harnesses can use the runtime
  generator instead of hand-rolling one. A project can now keep its persisted
  stream cursor and its generator in the same place.
- `HashRange` and `Mix*` give a stateless deterministic-variety primitive to
  layers that are architecturally forbidden from holding RNG state.
- Adopting `Random` in place of an existing hand-rolled generator **changes the
  number stream** and therefore any golden output derived from it. That is a
  deliberate, separately-scheduled migration for each consumer, not a drop-in.
- The wrapping-arithmetic gap is *worked around*, not closed: Zia still cannot
  express a 64-bit mixer natively. `Mix*` moves the one common need into the
  runtime. A general `Zanna.Math.Bits` wrapping-multiply is left for a later
  ADR if more cases appear.
- No IL opcode, grammar, or verifier changes. Registry surface only: 7 new
  functions on `Zanna.Math.Random`, 3 on `Zanna.Math`.

## Links

- `baseball/plans/58-runtime-adoption.md` — the audit that surfaced both gaps
- ADR 0250 — deterministic playback primitives (companion determinism work)
