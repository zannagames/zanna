---
status: completed
audience: contributors
last-verified: 2026-08-01
---

# Graphics3D Runtime Correctness Audit Follow-up (2026-08)

## 1. Scope and method

This follow-up is a source-level audit of the C runtime below
`src/runtime/graphics/3d`. The earlier August closeout was reviewed first, so
this pass concentrated on comparatively under-covered foundational systems:
Transform3D, Path3D, BlendTree3D, Material3D, Sprite3D, Decal3D, LensFlare3D,
Light3D, and InstanceBatch3D.

The review combined line-by-line ownership and data-flow analysis, cache and
derived-state analysis, complexity analysis, adversarial white-box regression
tests, an incremental warnings-as-errors build, and exhaustive `cppcheck`
warning/performance/portability analysis of every compiled Graphics3D C
translation unit. The ledger below records **114 newly corrected, independently
observable failure modes**. Similar entries remain separate only when they occur
at a different lifecycle boundary, affect a different public operation, or have
a distinct runtime consequence.

No public function signature, runtime C ABI surface, IL contract, opcode,
grammar, verifier rule, workflow, or cross-layer dependency changed. Appended
allocation identities and motion-history buffers are private object state.
Consequently this follow-up does not require an ADR under the repository policy.

## 2. Evidence key

- **ROB**: `test_rt_graphics3d_robustness`, including new exact-layout,
  corruption-repair, cache-coherence, ownership, renderer-revision, and
  camera-relative motion-history cases.
- **ANIM**: `test_rt_animcontroller3d`, including corrupt BlendTree3D metadata,
  triangle-index, triangulation, 1D-mode, and normalized barycentric cases.
- **BUILD**: the supported macOS build script with `ZANNA_SKIP_CLEAN=1` and the
  project's warnings-as-errors configuration.
- **STATIC**: exhaustive `cppcheck` warning, performance, and portability
  analysis of the compiled Graphics3D C subtree.
- **G3D**: the complete CTest `graphics3d` label.

## 3. Corrected issue ledger

### Transform3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-001 | Performance | A new identity transform started dirty, forcing a redundant first matrix rebuild. | Publish the already-built identity cache as clean. | ROB |
| G3F-002 | Correctness | Any nonzero `components_clean` value, including corrupt noncanonical values, bypassed component repair. | Accept exactly `1` as clean. | ROB |
| G3F-003 | Correctness | Repairing directly changed components did not dirty the cached matrix, so getters could return stale transforms. | Treat a cleared/noncanonical component-clean marker as evidence requiring a matrix rebuild. | ROB |
| G3F-004 | Performance | Setting the same effective position invalidated the matrix cache. | Repair, sanitize, compare, and mutate only on change. | ROB |
| G3F-005 | Performance | Setting the same normalized quaternion invalidated the matrix cache. | Compare the normalized candidate before assignment. | ROB |
| G3F-006 | Performance | Setting Euler angles that reproduce the stored quaternion rebuilt the matrix. | Build and normalize a local quaternion, then compare before committing. | ROB |
| G3F-007 | Performance | Setting the same effective scale invalidated the matrix cache. | Compare sanitized scale lanes before assignment. | ROB |
| G3F-008 | Correctness | Translation added an unbounded finite delta before clamping, so the intermediate sum could overflow to infinity and fall back to the wrong position. | Clamp displacement lanes before addition and clamp the result. | ROB, STATIC |
| G3F-009 | Performance | A zero or clamp-neutral translation dirtied an otherwise reusable matrix. | Return when the effective position is unchanged. | ROB |
| G3F-010 | Performance | A full-turn incremental rotation performed trigonometry and dirtied the cache. | Reduce modulo `2π` and return for an effective zero angle. | ROB |
| G3F-011 | Performance | `LookAt` dirtied the cache even when it reproduced the current orientation. | Compare the normalized result against the prior quaternion. | ROB |
| G3F-012 | Ownership | `GetEuler` leaked its temporary Quat object on both success and downstream-allocation failure paths. | Release the locally owned Quat on every path. | ROB |
| G3F-013 | Ownership | `GetEuler` leaked the temporary Vec3 returned by quaternion-to-Euler conversion. | Copy the three lanes, release the temporary Vec3, then construct the result. | ROB |

### Path3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-014 | Ownership | The finalizer freed mutable legacy coordinate pointers, so pointer corruption could free foreign storage or leak the real arrays. | Keep private ownership identities and free only those allocations. | ROB |
| G3F-015 | Correctness | Operations followed mutable coordinate pointers rather than restoring the arrays actually owned by the path. | Restore all three public mirrors from private identities before use. | ROB |
| G3F-016 | Correctness | Corrupt count/capacity metadata could drive coordinate traversal beyond the allocation. | Keep a private allocation capacity and clamp counts to that real bound. | ROB, STATIC |
| G3F-017 | Correctness | Repairing NaN/Infinity/extreme coordinates left cached polyline and spline lengths marked valid. | Detect coordinate changes and invalidate both derived caches. | ROB |
| G3F-018 | Correctness | Canonicalizing a corrupt looping flag did not invalidate topology-dependent caches. | Normalize the flag and dirty both caches when it changes. | ROB |
| G3F-019 | Correctness | A huge finite cached length above the runtime numeric ceiling was returned as valid. | Bound cached lengths by `PATH3D_LENGTH_MAX`. | ROB |
| G3F-020 | Correctness | Spline lookup count and pointer state had no independent allocation bound. | Track owned LUT identity/capacity and repair the public pointer/count pair. | ROB, STATIC |
| G3F-021 | Correctness | Nonfinite, decreasing, or oversized cumulative spline entries could reach binary search. | Validate the full active table for finite monotonic bounded values before reuse. | ROB |
| G3F-022 | Correctness | `segments * substeps + 1` could overflow and request an effectively unbounded LUT. | Compute in 64 bits and cap the table at 1,000,001 samples. | ROB, STATIC |
| G3F-023 | Performance | `AddPoint` rescanned every existing coordinate, making repeated append quadratic. | Split constant-time storage repair from content/cache repair. | ROB |
| G3F-024 | Performance | `GetPointCount` performed a complete coordinate scan. | Repair pointer/count metadata in O(1). | ROB |
| G3F-025 | Performance | Each polyline sample rescanned every control point, multiplying length computation by point count. | Repair once at the public boundary and use a storage-only raw evaluator internally. | ROB |
| G3F-026 | Performance | Each Catmull-Rom LUT sample repeated a complete repair scan. | Refresh once, then sample through the storage-only raw evaluator. | ROB |
| G3F-027 | Performance | Setting an unchanged loop value discarded both length caches. | Canonicalize and compare before invalidation. | ROB |
| G3F-028 | Correctness | `Clear` left the old cached length resident behind an empty path. | Reset cached length to zero immediately. | ROB |
| G3F-029 | Performance | `Clear` retained a potentially multi-megabyte derived LUT that no longer described any points. | Release the LUT while retaining reusable coordinate capacity. | ROB |
| G3F-030 | Ownership | LUT growth used `realloc` on the mutable public LUT pointer. | Grow from the private owned identity and publish only a successful replacement. | ROB |
| G3F-031 | Correctness | Direct `sqrt(dx*dx + dy*dy + dz*dz)` overflowed for large finite spline segments and silently dropped their length. | Use max-component scaled Euclidean length and saturating accumulation. | ROB, STATIC |
| G3F-032 | Correctness | Tangent normalization used the same overflowing squared-length expression and returned a zero tangent for valid large segments. | Use scaled normalization for spline tangents. | ROB, STATIC |

### LensFlare3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-033 | Correctness | A negative private element count let insertion index before the inline array. | Clamp count to `[0,16]` before insertion. | ROB |
| G3F-034 | Correctness | An oversized element count made drawing traverse beyond the inline array. | Use the repaired bounded count for traversal. | ROB, STATIC |
| G3F-035 | Ownership | Finalization trusted the mutable count and leaked retained ghosts in sparse or truncated state. | Release all sixteen slots independently of count. | ROB |
| G3F-036 | Correctness | Drawing dereferenced a wrong-class retained light as `rt_light3d`. | Validate the light slot before use and clear corruption. | ROB |
| G3F-037 | Ownership | Finalization released wrong-class light corruption as if it were an owned Light3D. | Use class-specific retained-slot cleanup. | ROB |
| G3F-038 | Ownership | Wrong-class ghost corruption could be released as owned Pixels. | Clear wrong-class slots as unowned. | ROB |
| G3F-039 | Ownership | A correct-class but malformed Pixels ghost was cleared without releasing its retained reference. | Separate ownership-class validation from drawable-layout validation. | ROB |
| G3F-040 | Ownership | Failed procedural Pixels construction could return or leak a partial object. | Validate exact backing storage and release partial construction on failure. | ROB |
| G3F-041 | Correctness | Ghosts with unexpected dimensions reached a draw call hard-coded for 32x32 source pixels. | Require exact 32x32 Pixels storage before drawing. | ROB |
| G3F-042 | Portability | Signed right shifts decoded negative packed RGB values implementation-dependently. | Decode through `uint64_t`. | ROB, STATIC |
| G3F-043 | Correctness | Nonfinite or unbounded axis offsets produced invalid overlay positions. | Repair offsets into the supported `[-1,2]` range. | ROB |
| G3F-044 | Correctness | Nonfinite, nonpositive, or oversized retained element sizes reached integer draw dimensions. | Repair each size into `[1,1024]`. | ROB |
| G3F-045 | Correctness | Projected float-to-integer conversion could be undefined outside the integer range. | Validate dimensions and projected coordinates before conversion. | ROB, STATIC |
| G3F-046 | Correctness | Nonfinite clip coordinates other than `w` were not rejected. | Validate all four clip components and all derived screen components. | ROB |
| G3F-047 | Correctness | Large-world light projection narrowed world coordinates before the view-projection multiply. | Keep world and projection arithmetic in double precision. | ROB |
| G3F-048 | Correctness | Positional lights outside the near/far clip interval still emitted flares. | Reject positional lights with NDC depth outside `[-1,1]`. | ROB |
| G3F-049 | Correctness | Directional-vector length computation overflowed for large finite direction lanes. | Normalize with max-component scaling and `hypot`. | ROB, STATIC |
| G3F-050 | Correctness | A depth target with inconsistent dimensions/storage could be sampled out of bounds. | Require a fully validated render-target pixel layout before CPU probing. | ROB |
| G3F-051 | Correctness | Nonfinite CPU/GPU depth samples counted as occluded, causing transient disappearance from incomplete readback. | Treat missing/nonfinite samples conservatively as visible and clamp the comparison reference. | ROB |
| G3F-052 | Correctness | Signed frame-serial ordering made visibility smoothing fragile at serial rollover. | Store serials unsigned and use wrap-safe unsigned distance. | ROB |
| G3F-053 | Correctness | NaN or out-of-range smoothed visibility persisted and poisoned all later draws. | Reset nonfinite state and clamp every smoothed result to `[0,1]`. | ROB |

### Material3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-054 | Correctness | The persisted-texture getter returned unsupported corrupt pointers to serializers. | Validate and clear the selected persisted slot before returning it. | ROB |
| G3F-055 | Ownership | Lightmap was omitted from texture repair/clone handling, leaving its retained slot less protected than the other seven textures. | Include lightmap in the common repair path and test clone/finalizer coverage. | ROB |
| G3F-056 | Correctness | An invalid metallic-roughness map promoted a material to PBR before the setter trapped. | Promote only after texture assignment succeeds. | ROB |
| G3F-057 | Correctness | An invalid AO map likewise changed workflow despite rejecting the input. | Make AO assignment and workflow promotion transactional. | ROB |

### Sprite3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-058 | Correctness | Construction accepted positive-dimension Pixels without backing data. | Require a nonempty exact Pixels implementation with data storage. | ROB |
| G3F-059 | Ownership | Correct-class malformed texture slots were cleared as unowned and leaked the retained Pixels. | Release by ownership class, independently of drawable validity. | ROB |
| G3F-060 | Correctness | Cached texture dimensions survived direct texture-layout repair or mutation. | Rederive width and height from live Pixels before rendering. | ROB |
| G3F-061 | Correctness | Cached frame coordinates could remain outside repaired/current texture dimensions. | Re-run frame clamping after texture-dimension repair. | ROB |
| G3F-062 | Correctness | Nonfinite/extreme position, scale, anchor, tint, and noncanonical additive state reached mesh/material generation. | Centralize bounded retained-state repair before draw/rebase. | ROB |
| G3F-063 | Portability | Packed tint extraction shifted a signed integer. | Decode the color through an unsigned representation. | ROB, STATIC |
| G3F-064 | Correctness | Origin rebasing subtracted from unrepaired position state and could preserve NaN. | Repair numeric state before applying the delta. | ROB |
| G3F-065 | Correctness | Non-additive textured sprites used opaque alpha mode, discarding ordinary texture transparency. | Always use alpha blending; retain additive as the blend-equation choice. | ROB |
| G3F-066 | Correctness | A partial quad build after allocation failure could be submitted as valid geometry. | Require exactly four validated vertices and six indices before queueing. | ROB |
| G3F-067 | Correctness | Constructor dimension checks and draw-time slot checks disagreed about what constituted drawable Pixels. | Route both through the same exact texture predicate. | ROB |

### Decal3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-068 | Correctness | Construction accepted Pixels with missing data or invalid dimensions. | Require positive bounded dimensions and backing storage. | ROB |
| G3F-069 | Ownership | Correct-class malformed texture corruption was cleared without releasing the retained Pixels. | Separate ownership cleanup from drawable-layout validation. | ROB |
| G3F-070 | Correctness | Changing/repairing the retained texture left a cached material bound to the old texture. | Track the unowned material source identity and invalidate on mismatch. | ROB |
| G3F-071 | Correctness | Repairing position, normal, or size left a cached quad built from stale geometry. | Detect repair changes and release the derived mesh. | ROB |
| G3F-072 | Correctness | Nonfinite/inverted lifetime and maximum-lifetime state produced inconsistent fade/expiry behavior. | Normalize corrupt lifetime state to the documented permanent state and bound finite timers. | ROB |
| G3F-073 | Correctness | Nonfinite/out-of-range alpha could reach the cached material. | Repair alpha and force expired decals to zero alpha. | ROB |
| G3F-074 | Correctness | Repaired depth bias was not reapplied to an already cached material. | Clamp bias and refresh the material bias on every draw. | ROB |
| G3F-075 | Correctness | `Draw`, `Update`, and `IsExpired` used different ad-hoc lifetime repair rules. | Route all three through one canonical retained-state repair. | ROB |
| G3F-076 | Correctness | Rebase and position getters exposed or propagated corrupt placement state. | Repair placement before both operations. | ROB |
| G3F-077 | Correctness | A partially generated decal quad could be cached and submitted. | Validate the exact four-vertex/six-index result and discard malformed builds. | ROB |

### Light3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-078 | Performance | Every setter advanced the process-wide light revision even when the sanitized value was unchanged, invalidating flattened-light caches globally. | Compare effective state in intensity, attenuation, color, flags, position, direction, area, radius, decay, range, and spot setters. | ROB |
| G3F-079 | Correctness | Repairing an invalid stored type did not advance the revision, so renderer snapshots could keep the old interpretation. | Advance the revision when `GetType` repairs storage. | ROB |
| G3F-080 | Correctness | Color and intensity getters sanitized only their return values, leaving poisonous stored values and stale renderer snapshots. | Persist repaired values and advance the revision once. | ROB |
| G3F-081 | Correctness | Enabled/shadow getters normalized their return values but not stored noncanonical flags; volume shadow state could remain illegally enabled. | Canonicalize storage with type-aware shadow repair and revision tracking. | ROB |
| G3F-082 | Correctness | Position getter returned a sanitized copy without repairing the position consumed by render flattening. | Persist the bounded position and advance revision. | ROB |
| G3F-083 | Correctness | Direction getter normalized only a temporary vector, leaving invalid direction storage. | Normalize the retained direction and advance revision. | ROB |
| G3F-084 | Correctness | A rectangle light with a valid direction but corrupt basis vectors kept the corrupt emitter basis. | Rebuild and compare both basis vectors whenever rectangle direction is read/set. | ROB |
| G3F-085 | Correctness | Width, height, radius, and range getters returned repaired values without repairing renderer-visible storage. | Persist positive bounded dimensions/range with revision tracking. | ROB |
| G3F-086 | Correctness | Invalid decay type was repaired without invalidating cached light packets. | Advance revision on repair and avoid revision churn for unchanged setters. | ROB |
| G3F-087 | Correctness | Attenuation getter exposed inconsistent type handling and left corrupt stored attenuation untouched. | Apply type-aware local/volume repair, persist it, and advance revision. | ROB |
| G3F-088 | Correctness | Spot getters decoded malformed/inverted cone cosines but left the invalid pair in renderer state. | Reconstruct a valid cosine pair and advance revision before returning degrees. | ROB |
| G3F-089 | Documentation | Public type and attenuation comments omitted rectangle, sphere, and volume-era behavior. | Document all registered light IDs and supported falloff emitters. | BUILD |

### BlendTree3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-090 | Correctness | Corrupt dimensions could route one-dimensional storage through two-dimensional logic. | Repair dimensions to the supported `{1,2}` set before weighting. | ANIM |
| G3F-091 | Correctness | Sample count could disagree with backend blend capacity and expose uninitialized samples. | Clamp to both inline and backend bounds before every read. | ANIM |
| G3F-092 | Correctness | Corrupt triangle count could traverse beyond the fixed triangulation array. | Validate count and discard/rebuild malformed triangulation. | ANIM, STATIC |
| G3F-093 | Correctness | In-range triangle count could still contain negative, repeated, or out-of-range sample indices. | Validate every cached triangle before interpolation. | ANIM |
| G3F-094 | Correctness | Nonfinite parameters/sample coordinates and noncanonical dirty/mode flags reached triangulation and weighting. | Canonicalize all metadata; invalidate triangulation only when topology coordinates change. | ANIM |
| G3F-095 | Correctness | Tolerance-clamped negative barycentric weights no longer summed to one. | Clamp, verify, and renormalize the accepted barycentric triple. | ANIM |
| G3F-096 | Performance | Setting identical parameters recomputed all weights. | Make an unchanged repaired parameter assignment O(1). | ANIM |
| G3F-097 | Correctness | One-dimensional trees retained/exposed meaningless 2D mode changes and recomputed weights. | Ignore 2D mode requests for 1D trees and report mode zero. | ANIM |

### InstanceBatch3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-098 | Ownership | The finalizer freed mutable legacy matrix pointers, risking foreign frees and leaks of the actual allocations. | Keep six stable private ownership identities and free only those. | ROB |
| G3F-099 | Correctness | Mutable pointers/count/capacity were trusted before traversal. | Restore public mirrors and clamp all counts to a private allocation bound. | ROB, STATIC |
| G3F-100 | Correctness | State repair could recreate only the live double mirror, leaving motion history represented solely by floats. | Maintain double live, current-snapshot, and previous-snapshot arrays plus float mirrors. | ROB |
| G3F-101 | Correctness | Frame serial zero was indistinguishable from “snapshot already captured,” skipping first-frame initialization. | Track initialization with an explicit flag. | ROB |
| G3F-102 | Correctness | Camera-relative drawing returned before the common motion-history capture path. | Capture current/previous history before choosing submission mode. | ROB |
| G3F-103 | Correctness | Camera-relative previous transforms were absent, so motion vectors could not describe large-world movement. | Convert both double histories relative to the same camera origin. | ROB |
| G3F-104 | Correctness | Narrowing history to absolute floats before origin subtraction erased sub-unit motion at large coordinates. | Preserve snapshots in double precision until camera-relative conversion. | ROB |
| G3F-105 | Correctness | Growing the batch did not allocate/copy double current and previous history. | Grow all six primary buffers transactionally. | ROB |
| G3F-106 | Correctness | Swap-removal moved float history but not corresponding double history. | Move both precision representations under the same count rules. | ROB |
| G3F-107 | Correctness | `Clear` left the frame-initialization state set, allowing the next lifetime of the batch to inherit stale sequencing. | Reset the explicit motion-initialized flag with all history counts. | ROB |
| G3F-108 | Correctness | Camera-relative batches skipped per-instance frustum culling entirely. | Cull and compact converted current/previous matrices together. | ROB |
| G3F-109 | Performance | Normal culling evaluated the same transformed AABB once to count and again to compact. | Cache one visibility byte per instance and reuse it during compaction. | ROB |
| G3F-110 | Correctness | Partial-cull scratch failure could desynchronize current and previous compacted arrays. | Require matching scratch capacity before compaction and otherwise submit the coherent full batch. | ROB |

### Final line-by-line closeout finding

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3F-111 | Correctness | If spline-LUT growth failed, constant-speed evaluation consumed the retained but stale prior table while reporting the cache dirty. | Preserve the allocation for retry but bypass it until a successful refresh clears the dirty flag. | BUILD, STATIC |
| G3F-112 | Performance | Re-normalizing an already-unit decal normal could toggle one lane by one ULP and invalidate the cached quad on the next repair. | Preserve near-identical normalized lanes so cache invalidation reflects a material geometry change. | BUILD |
| G3F-113 | Correctness | Getter/mode entry points could repair BlendTree3D parameters or sample metadata without refreshing the corresponding backend weights. | Reapply weights whenever those entry points make an observable repair, including ignored 1D mode requests. | ANIM |
| G3F-114 | Correctness | Positional lens flares multiplied absolute world coordinates by a camera-relative view-projection matrix, moving large-world lights off screen. | Subtract the validated frame origin in double precision before projection. | ROB |

The follow-up found and corrected **114** distinct runtime issues: 13 in
Transform3D, 20 in Path3D, 22 in LensFlare3D, 4 in Material3D, 10 in Sprite3D,
11 in Decal3D, 12 in Light3D, 9 in BlendTree3D, and 13 in InstanceBatch3D.

## 4. Public behavior documentation

The affected headers now document the corrected observable contracts:

- unchanged Transform3D mutations and Path3D loop assignments preserve derived
  caches;
- clearing a path retains coordinate capacity but releases its derived spline
  lookup table;
- BlendTree3D 2D mode is inapplicable to one-dimensional trees;
- LensFlare3D repairs retained elements and smoothing state before drawing;
- Light3D documentation covers all seven registered emitter types and current
  local-falloff behavior;
- InstanceBatch3D camera-relative submission preserves and culls current and
  previous matrices in a common frame.

These are correctness and lifecycle clarifications of existing calls, not new
ABI surface.

## 5. Validation

All builds for this audit are incremental because other work was active in the
same checkout. The build invocation uses the supported script and explicitly
sets `ZANNA_SKIP_CLEAN=1`; no raw CMake configuration or clean build is used.

```sh
ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 ZANNA_SKIP_LINT=1 \
  ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 ZANNA_SKIP_INSTALL=1 \
  ./scripts/build_zanna_mac.sh

ctest --test-dir build \
  -R 'test_rt_(graphics3d_robustness|animcontroller3d|transform_path|instterrain)' \
  --output-on-failure

ctest --test-dir build -L graphics3d --output-on-failure -j4

cppcheck --project=build/compile_commands.json \
  --file-filter='*/src/runtime/graphics/3d/*' \
  --enable=warning,performance,portability --check-level=exhaustive \
  --error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem

./scripts/lint_platform_policy.sh
git diff --check
```

Closeout results:

- incremental warnings-as-errors macOS build: passed;
- focused edited-target tests: 4/4 passed;
- complete `graphics3d` label: 156/156 passed, including the 120-second
  short soak, backend contracts, runtime manifest, and ABI-surface checks;
- adversarial `test_rt_graphics3d_robustness` repeat: 20/20 passed;
- exhaustive static analysis: 106/106 compiled Graphics3D C translation units
  checked with no warning/performance/portability diagnostics;
- platform-policy lint and `git diff --check`: clean.

## 6. Related records

- [Graphics3D Runtime Correctness Audit (2026-08)](graphics3d-runtime-audit-2026-08.md)
- [Graphics3D Runtime Hardening Program (2026-07)](graphics3d-runtime-hardening-2026-07.md)
- [Graphics3D architecture](graphics3d-architecture.md)
- [Runtime testing policy](testing.md)
- [ADR 0102: Graphics3D runtime boundary](../adr/0102-graphics3d-runtime-boundary-and-contract-manifest.md)
