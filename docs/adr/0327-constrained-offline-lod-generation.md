---
status: active
audience: contributors
last-verified: 2026-09-05
---

# ADR 0327: Constrained Offline LOD Generation

## Status

Accepted and implemented. The isolated macOS canonical build passes all
2,023 configured non-slow tests, runtime audit and follow-up smoke checks;
the native Studio build and installation were skipped. Real Windows/Linux
validation and final stadium LOD certification remain outstanding.

## Context

Offline `asset bake --lods` calls unconstrained mesh simplification, even
though `--simplify-meshes` already supports seam locking and a quadric-cost
ceiling. Thin stadium wall faces disappear at reduced levels while their
bounds remain valid. Runtime workarounds retain complete wall meshes but
cannot supply a useful, consistently constrained asset pipeline.

## Decision

- Add the tool-facing C ABI
  `int64_t rt_model3d_generate_lods_ex(void *model, int64_t levels,
  double ratio, int64_t flags, double max_error_frac)`.
- Existing `rt_model3d_generate_lods` delegates with zero constraints and
  preserves existing chained-decimation behavior. No frontend registry,
  language syntax, opcode, serialization format, or product dependency changes.
- Flags use `RT_MESH3D_SIMPLIFY_FLAG_LOCK_BOUNDARIES`; other bits are ignored
  as in `rt_mesh3d_simplify_ex`. Positive finite `max_error_frac` enables the
  existing cost ceiling; other values disable it. Existing level/ratio
  sanitation, source deduplication, worker dispatch, ownership and skip-existing-
  chain semantics remain unchanged. Graphics-disabled stubs return zero.
- Constrained levels simplify the original source, not the preceding reduced
  mesh. This keeps the error reference and seam authority fixed across levels.
  Targets still decrease by the requested ratio. A valid result that cannot
  reduce the preceding level ends the chain; no duplicate full-resolution
  meshes are attached merely to satisfy a requested level count.
- Add opt-in CLI `--lod-lock-seams` and `--lod-max-error F`; both require
  `--lods N` with `N > 0`. `F` must be a complete finite numeric token in
  `(0,1)`. Exact usage errors are
  `zanna asset bake: --lod-lock-seams/--lod-max-error require --lods > 0`
  and `zanna asset bake: --lod-max-error expects a bounding-diameter fraction in (0,1)`.
  Existing `--simplify-*` options retain their separate scope and defaults.

The ceiling is a quadric-cost limit, **not a Hausdorff-distance or screen-pixel
guarantee**. Authored silhouette/contact tests and visual review remain required.
No claim is made that every mesh can reach its requested triangle ratio.

## Verification

- A triangulated open grid reduces with seam locking while retaining every
  original boundary vertex; existing sources and instances remain valid.
- A tight cost ceiling can leave a mesh without a reduced level; callers keep
  the base mesh, not an empty renderable. Invalid handles return zero.
- Legacy zero-constraint generation retains its existing behavior and tests.
- CLI rejects missing LODs, incomplete/non-finite/out-of-range error fractions;
  constrained generation bakes and reloads successfully through the JSON path.
- Cooked stadium wall/contact and material tests must pass before runtime
  preservation workarounds are removed. Measure achieved reductions separately.
- Run canonical engine build/tests and platform-policy checks; real Windows
  and Linux certification remains necessary before cross-platform completion.
