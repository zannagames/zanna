---
status: active
audience: contributors
last-verified: 2026-07-02
---

# ADR 0043: Random Chance Boolean API

## Status

Accepted

## Context

`Zanna.Math.Random.Chance(probability)` reads like a predicate and is commonly
used in examples as a probability check. Its public signature returned `i64`,
with `1` for success and `0` for failure. That shape encouraged integer
truthiness in user code and made the API less clear than the surrounding
boolean probe methods.

Existing programs may still need the numeric 0/1 value for formatting, storage,
or compatibility with older examples.

## Decision

Make `Zanna.Math.Random.Chance(probability)` return `Boolean` (`i1`). Add
`Zanna.Math.Random.ChanceInt(probability)` for the previous integer 0/1 result.

Both functions share the same probability clamping and deterministic RNG state:
probability `<= 0.0` fails, probability `>= 1.0` succeeds, and values between
those bounds sample the active deterministic generator.

## Consequences

- New code can write `if Random.Chance(0.25)` without comparing against `1`.
- Existing numeric consumers can use `ChanceInt` without losing the feature.
- Runtime API dumps classify `ChanceInt` as a compatibility row with a migration
  target back to `Chance`.
- Docs and examples should teach `Chance` as a boolean predicate and reserve
  `ChanceInt` for compatibility notes or API audits.
