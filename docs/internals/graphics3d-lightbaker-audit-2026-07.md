---
status: completed
audience: contributors
last-verified: 2026-08-17
---

# Graphics3D Light Baker Correctness Audit (2026-07)

## Summary and objective

This document records the focused follow-up audit of the C implementation in
`src/runtime/graphics/3d/render/rt_lightbaker3d.c`. The broader Graphics3D
hardening and backend audits remain the baseline; the findings here cover the
baked-lightmap and irradiance-probe paths that were not closed by those audits.

The objective is to make bake setup failure-atomic, keep all geometry and
radiance math finite, avoid silently publishing partial lightmaps, make probe
files portable and exact, and remove avoidable superlinear or per-pixel runtime
overhead. The public runtime C ABI is unchanged.

All 42 findings below were resolved in the runtime, covered by focused
regressions where observable, and validated against the complete Graphics3D
test label. No opcode, grammar, verifier rule, runtime signature, or external
dependency changed.

## Scope and configuration

In scope are `LightBaker3D`, `LightProbeGrid3D`, their internal BVH and chart
packer, `.vlpg` persistence, focused unit tests, and user-facing baked-GI
documentation. No feature toggle or runtime configuration is required: the
changes enforce existing correctness and portability contracts. IL grammar,
opcodes, verifier rules, renderer backend APIs, and workflow files are out of
scope.

## Findings and required resolutions

| ID | Class | Finding and required resolution |
|---:|---|---|
| LB-001 | Correctness | The fixed 512-entry scene stack silently omitted later children. Use a dynamically grown, hard-bounded traversal and fail the gather instead of publishing a partial scene. |
| LB-002 | Resource | A failed or retried gather could overwrite partial triangle state. Stage triangles/nodes locally and either commit the complete gather or release every staged reference/allocation. |
| LB-003 | Correctness | One unreadable triangle stopped import of every later triangle in the mesh. Skip only the malformed triangle. |
| LB-004 | Correctness | One unreadable vertex likewise stopped every later triangle. Skip only the malformed triangle. |
| LB-005 | Correctness | Non-finite transformed positions could poison BVH bounds, ray tests, chart sizing, and pixel conversion. Reject those triangles before publication. |
| LB-006 | Correctness | Degenerate triangles received an arbitrary up normal and consumed atlas space. Exclude zero-area/non-finite triangles. |
| LB-007 | Correctness | Authored normals were compared in object space against world-space geometry. Transform them by the inverse-transpose normal matrix before orienting a face. |
| LB-008 | Lifetime | Gathered node, mesh, and material pointers were borrowed across incremental bake calls. Retain each committed entry and release it in the baker finalizer. |
| LB-009 | Correctness | Mesh mutation during an incremental bake mixed old triangle snapshots with new UV targets. Capture geometry revisions and fail before further UV publication when a source changes. |
| LB-010 | Correctness | Empty scenes behaved differently on the first and second probe bake attempts. Treat an empty acceleration structure as a valid sky-only probe input. |
| LB-011 | Resource | Partial BVH allocation could leak, poison retries, or report a usable tree with a missing order array. Allocate/build transactionally and validate complete cached state. |
| LB-012 | Memory safety | Recursive BVH child failure left negative child indices in an internal node. Propagate failure and discard the whole candidate tree. |
| LB-013 | Performance | Insertion-sorting every BVH subtree was quadratic on ordered/high-poly scenes. Use deterministic introselect median partitioning with bounded fallback sorting. |
| LB-014 | Correctness | Replacing a parallel ray reciprocal with `1e12` produced scale-dependent slab results. Test parallel slabs explicitly. |
| LB-015 | Correctness | Baked local lights ignored authored range. Reject contributions beyond a positive finite range. |
| LB-016 | Correctness | Baked local lights ignored linear/cubic/no-decay modes. Apply the same finite decay exponent contract as realtime lighting. |
| LB-017 | Correctness | Spot lights baked as omnidirectional point lights. Apply the retained inner/outer cone and smooth falloff. |
| LB-018 | Correctness | Lights with `CastsShadows=false` still traced bake shadow rays. Gate occlusion on the copied flag. |
| LB-019 | Determinism | Changing density, samples, bounces, sky, or lights after gathering began produced one atlas with mixed settings. Freeze bake inputs at the first gather. |
| LB-020 | Undefined behavior | Huge but finite world edges were multiplied and narrowed to `int32_t` before clamping. Bound chart dimensions before conversion and use overflow-resistant length math. |
| LB-021 | Correctness | Atlas exhaustion advanced directly to completion and published a partial bake. Preflight every chart and leave `Atlas` null if all charts cannot fit. |
| LB-022 | Performance | Publishing a 1024-square atlas called the validated Pixels setter once per texel and advanced its generation up to 1,048,576 times. Fill validated backing storage directly and publish one mutation. |
| LB-023 | Resource | HDR accumulation and coverage buffers remained resident after the 8-bit atlas was published or setup failed. Release both as soon as they are no longer needed. |
| LB-024 | Performance | UV writes invalidated retained mesh geometry once per triangle. Defer publication and touch each distinct changed mesh once. |
| LB-025 | Correctness | `Apply` included gathered nodes without a baked chart and could mutate a prefix before a later material allocation failed. Stage all eligible material instances, then commit them together. |
| LB-026 | Lifetime | Repeated `Apply` could revisit a borrowed original material already released by the first replacement. Retained gather inputs plus idempotent apply eliminate the stale access. |
| LB-027 | Correctness | Probe bounds accepted non-finite or inverted coordinates and could narrow a huge ratio to `int32_t`. Validate ordering/finiteness and clamp before integer conversion. |
| LB-028 | Correctness | An extent exactly divisible by spacing allocated one extra probe plane. Use `ceil(extent / spacing) + 1`, preserving the documented two-to-sixty-four axis bound. |
| LB-029 | Resource | Probe coefficient/validity allocation failure returned a live object with null backing arrays. Constructor allocation is now all-or-nothing. |
| LB-030 | Correctness | Probe bake only ensured the BVH while gathering, so a gathered baker with a failed/missing BVH silently became sky-only. Ensure the acceleration structure independently. |
| LB-031 | Correctness | Invalid-probe infill copied only one hop, did not mark copies valid, and sampling ignored validity. Propagate valid coefficients and renormalize trilinear weights over valid corners. |
| LB-032 | Undefined behavior | Non-finite sample positions and zero/non-finite normals reached `floor`/integer conversion or a degenerate SH basis. Return black for invalid positions and use positive Y for invalid normals. |
| LB-033 | Portability | `.vlpg` persisted native integer/float representations. Encode all multibyte fields as fixed-width IEEE little-endian values. |
| LB-034 | Correctness | `.vlpg` load accepted non-finite bounds/coefficients, noncanonical validity bytes, and trailing payload. Validate every field and require exact EOF. |
| LB-035 | Failure atomicity | `.vlpg` load committed replacement arrays before the input stream was successfully closed. Close first and publish only the complete staged payload. |
| LB-036 | Correctness | Save trusted private grid counts/backing and invalid string handles. Validate the complete grid and path before staging output. |
| LB-037 | Performance | Material color gathering allocated and released a temporary Vec3 per node despite already having a validated material payload. Copy sanitized material fields directly. |
| LB-038 | Performance | BVH bounds used fixed `+/-1e30` sentinels, making valid finite geometry outside that scale produce unnecessarily loose nodes. Initialize with `+/-DBL_MAX`. |
| LB-039 | Correctness | Indirect rays stopped at an arbitrary `1e12` distance even though gathered geometry accepts the full finite-double domain. Use `DBL_MAX`; explicit local-light range still bounds shadow rays. |
| LB-040 | Undefined behavior | Newly staged triangles copied indeterminate chart-placement fields before preflight initialized them. Zero-initialize each complete candidate record at construction. |
| LB-041 | Correctness | Directional lights were rejected when their unused position payload was non-finite. Validate position only for local lights, while continuing to require a finite unit direction for sun/moon inputs. |
| LB-042 | Correctness | An incremental bake validated only mesh geometry revisions, so a node could change mesh/material bindings, material radiance fields, or world transform and later receive an atlas baked from stale source state. Capture and revalidate every bake-relevant source field during slicing, before UV publication, and again before delayed `Apply`. |

## Error handling

- Invalid or wrong-class handles keep the existing operation-specific traps.
- Non-finite or inverted probe bounds trap with
  `LightProbeGrid3D.New: bounds must be finite and ordered` and return null if a
  recovery handler resumes execution.
- Probe backing allocation failure traps with
  `LightProbeGrid3D.New: coefficient allocation failed` and returns null.
- Gather, BVH, chart-capacity, atlas-allocation, or source-revision failure
  completes the compatibility `BakeStep` call with no atlas; callers determine
  success through `Atlas != null`, as before.
- Malformed, noncanonical, non-finite, truncated, overlong, or close-failed
  `.vlpg` input returns false and preserves the previous grid bit-for-bit.

## Regression requirements

- Given more than 512 static child nodes, when the bake completes and applies,
  then a node beyond the old stack ceiling receives a lightmap.
- Given a malformed early triangle followed by a valid one, when gathering,
  then the valid triangle still produces an atlas.
- Given more than one incremental slice, when configuration is changed after
  the first slice, then getters retain the frozen values and one mesh revision
  is published for all UV writes.
- Given a node transform, material binding, or in-place material-radiance change
  after gathering, when the next slice or delayed `Apply` runs, then no stale
  atlas is published/applied.
- Given an empty scene and nonblack sky, when a probe grid bakes once, then it
  samples finite nonblack sky irradiance.
- Given exact-multiple extents, inverted/non-finite bounds, an unbaked grid,
  non-finite positions, and zero normals, then counts and samples follow the
  finite contracts above without undefined conversion.
- Given valid, noncanonical-validity, non-finite-coefficient, trailing, and
  truncated `.vlpg` files, then only the exact canonical file commits; rejected
  files preserve the previous sample.
- Given a published atlas, its Pixels generation advances once rather than once
  per texel.

## Validation

Use only incremental builds while concurrent work is present:

```sh
ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 ZANNA_SKIP_LINT=1 \
ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 ZANNA_SKIP_INSTALL=1 \
./scripts/build_zanna_mac.sh
ctest --test-dir build -R '^test_rt_lightbaker3d$' --output-on-failure
ctest --test-dir build -L graphics3d --output-on-failure
./scripts/lint_platform_policy.sh
./scripts/run_cross_platform_smoke.sh
```

Validation completed on macOS arm64 on 2026-07-31:

- Incremental `build_zanna_mac.sh` build: passed with warnings treated as
  errors; no clean build was run.
- Focused `test_rt_lightbaker3d`: 10/10 scenarios passed.
- Complete `graphics3d` CTest label: 155/155 tests passed, including software,
  Metal, soak, smoke, ABI-surface, and runtime-manifest coverage.
- Focused cppcheck warning/performance/portability analysis: no findings.
- Platform-policy lint, cross-platform host smoke slice, documentation checks,
  formatting, and whitespace validation: passed.
