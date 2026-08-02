---
status: completed
audience: contributors
last-verified: 2026-08-01
---

# Graphics3D Animation and Navigation Runtime Audit (2026-08)

## 1. Scope and method

This is a fresh source-level audit of three stateful C implementations below
`src/runtime/graphics/3d`: IKSolver3D, MorphTarget3D, and NavAgent3D. The July
hardening program and both earlier August closeouts were reviewed first; their
213 findings are baseline and are not recounted here.

The review combined line-by-line ownership and data-flow analysis, numeric and
transform analysis, cache/history analysis, hot-path complexity review,
adversarial white-box regression tests, an incremental warnings-as-errors
build, full Graphics3D CTest coverage, platform-policy lint, and exhaustive
`cppcheck` warning/performance/portability analysis of the compiled Graphics3D
C subtree. The ledger records **104 newly corrected, independently observable
failure modes**. Similar entries remain separate only when they affect a
different allocation, lifecycle boundary, public operation, or runtime result.

No public function signature, runtime registry row, runtime C ABI surface, IL
contract, opcode, grammar, verifier rule, workflow, or production cross-layer
dependency changed. New allocation identities, capacities, suffix caches, and
history flags are private object state. Existing public Seq and Box validation
APIs are used at the IK boundary. Consequently this audit does not require an
ADR under repository policy.

## 2. Evidence key

- **IK**: `test_rt_animcontroller3d`, including rotated-parent look-at,
  malformed FABRIK input, exact chain cardinality, pose-mirror repair, retained
  state repair, and ownership tests.
- **MORPH**: `test_rt_morphtarget3d`, including exact private layouts,
  pointer/count/vertex repair, sparse/no-op edits, cache invalidation, clone,
  packed storage, and finalizer tests.
- **NAV**: `test_rt_navagent3d`, including state repair, path/suffix ownership,
  neighbor scratch ownership, singular-parent world writes, grid parity, and
  crowd performance cases.
- **ROB**: `test_rt_graphics3d_robustness` integration coverage.
- **DRAW**: `test_rt_canvas3d_gpu_paths`, including failed CPU/GPU resource
  setup, retain rollback, and accepted-frame morph-history coverage.
- **BUILD**: supported macOS build script with `ZANNA_SKIP_CLEAN=1` and
  warnings-as-errors enabled.
- **STATIC**: exhaustive Graphics3D `cppcheck`, platform-policy lint, and
  `git diff --check`.
- **G3D**: the complete CTest `graphics3d` label.

## 3. Corrected issue ledger

### IKSolver3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3AN-001 | Correctness | Cross-product lanes were clamped independently, changing the direction of valid large vectors. | Compute in double and apply one common scale that preserves component ratios. | IK, STATIC |
| G3AN-002 | Correctness | Vector length narrowed a squared dot product to float before `sqrt`, severely understating large finite lengths. | Use max-component scaling and double `hypot`, narrowing only the final bounded result. | IK, STATIC |
| G3AN-003 | Correctness | Normalization divided by a saturated float length, so large vectors could remain non-unit. | Normalize from scaled double components independently of the public length helper. | IK, STATIC |
| G3AN-004 | Correctness | Point subtraction occurred in float and could overflow before distance evaluation. | Subtract in double and use scaled Euclidean distance. | IK, STATIC |
| G3AN-005 | Correctness | Quaternion normalization overflowed its float sum of squares and replaced valid rotations with identity. | Normalize through a scaled double norm. | IK, STATIC |
| G3AN-006 | Correctness | The FABRIK factory called trapping Seq operations on wrong-class handles. | Validate the managed Seq class before any collection accessor. | IK |
| G3AN-007 | Correctness | A Seq whose logical length exceeded its capacity could reach indexed reads. | Compare public length and capacity before reading the bounded prefix. | IK |
| G3AN-008 | Correctness | FABRIK used trapping integer unboxing for non-integer elements. | Use the non-trapping boxed-integer probe and reject mismatches. | IK |
| G3AN-009 | Correctness | An oversized corrupt chain count was silently clamped to 32 and could become a different valid chain. | Reject counts outside the fixed inline bound exactly. | IK |
| G3AN-010 | Correctness | Shortening a FABRIK chain in private state silently changed the solved constraint. | Preserve construction-time chain cardinality independently and require an exact match. | IK |
| G3AN-011 | Correctness | Unknown solver kinds fell through to the chain solver. | Dispatch only the three exact supported kinds and reject every other value. | IK |
| G3AN-012 | Correctness | A two-bone solver did not independently enforce its three-index cardinality after construction. | Require exactly three indexes during retained-state repair. | IK |
| G3AN-013 | Correctness | A look-at solver did not independently enforce its one-index cardinality after construction. | Require exactly one index during retained-state repair. | IK |
| G3AN-014 | Correctness | Mutated chain indexes/topology were trusted after construction. | Revalidate every index and direct parent edge at each solve/apply boundary. | IK |
| G3AN-015 | Correctness | Standalone solve wrote through a mutable local-pose pointer that could address foreign memory. | Restore the pointer from a private allocation identity before use. | IK |
| G3AN-016 | Correctness | Standalone solve likewise wrote through a mutable global-pose pointer. | Keep and restore an independent global allocation identity. | IK |
| G3AN-017 | Ownership | Finalization freed the mutable local-pose mirror, risking a foreign free and leaking the real allocation. | Free only the private local allocation identity. | IK |
| G3AN-018 | Ownership | Finalization freed the mutable global-pose mirror with the same failure mode. | Free only the private global allocation identity. | IK |
| G3AN-019 | Correctness | Growth or corruption of the retained skeleton bone count could make solve exceed its pose allocations. | Store the allocated bone capacity and cap every solve to it. | IK, STATIC |
| G3AN-020 | Correctness | A wrong-class retained skeleton could reach solve/apply as a Skeleton3D. | Validate the retained class and clear wrong-class corruption as unowned. | IK |
| G3AN-021 | Correctness | The skeleton getter hid a wrong-class value in its return result but left the corrupt slot resident. | Clear the slot when validation fails. | IK |
| G3AN-022 | Correctness | Nonfinite/extreme retained target lanes bypassed setter-only sanitization. | Persist finite coordinate-bounded target lanes during repair. | IK |
| G3AN-023 | Correctness | Retained pole lanes had the same setter-only validation gap. | Persist finite coordinate-bounded pole lanes during repair. | IK |
| G3AN-024 | Correctness | A corrupt ground normal could remain nonfinite or degenerate and poison foot orientation. | Sanitize, robustly normalize, and fall back to model-space up. | IK |
| G3AN-025 | Correctness | Noncanonical pole and ground flags persisted in solver state. | Canonicalize both flags to zero or one. | IK |
| G3AN-026 | Correctness | Nonfinite or out-of-range retained weight state bypassed setter clamping. | Persist a finite weight in `[0,1]`. | IK |
| G3AN-027 | Correctness | Zero weight returned success before validating storage, skeleton, kind, chain, or topology. | Repair and validate first; only a valid zero-weight solver is a successful no-op. | IK |
| G3AN-028 | Correctness | Look-at treated a desired model-space rotation as local, producing the wrong aim below rotated parents. | Convert the desired global quaternion through inverse parent rotation before blending. | IK |
| G3AN-029 | Correctness | Foot orientation accepted a parent only when its index was numerically lower than the child, breaking valid non-topological skeletons. | Accept any distinct in-range parent. | IK |
| G3AN-030 | Ownership | Construction retained/froze the skeleton before fallible pose allocation, so allocation failure could leave a needless structural side effect. | Allocate and seed both buffers before retaining and freezing the skeleton. | BUILD |

### MorphTarget3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3AN-031 | Correctness | Operations trusted the mutable shape-table pointer. | Restore it from a private allocation identity at API boundaries. | MORPH |
| G3AN-032 | Correctness | Current-weight traversal trusted a mutable pointer independently of the shape table. | Keep and restore an aligned private weight identity. | MORPH |
| G3AN-033 | Correctness | Previous-weight history trusted a mutable pointer. | Keep and restore its private identity. | MORPH |
| G3AN-034 | Correctness | Motion snapshots trusted a mutable pointer and could copy through foreign memory. | Keep and restore a fourth aligned private identity. | MORPH |
| G3AN-035 | Correctness | The packed position getter could expose a corrupt pointer while its real allocation remained owned elsewhere. | Restore the packed-position mirror from its private identity. | MORPH |
| G3AN-036 | Correctness | Packed normal storage had the same mirror gap. | Restore it from an independent private identity. | MORPH |
| G3AN-037 | Correctness | An oversized shape count could traverse past all four aligned allocations. | Derive the live count from initialized records and allocation capacity. | MORPH, STATIC |
| G3AN-038 | Correctness | A downward-corrupt shape count permanently hid valid shapes. | Restore it from the independently maintained initialized count. | MORPH |
| G3AN-039 | Correctness | Mutable shape capacity was accepted as the allocation bound. | Restore it from private allocation capacity. | MORPH |
| G3AN-040 | Correctness | Mutable vertex count sized channel reads, clone copies, bindings, and packed buffers. | Restore it from the per-channel allocation vertex count. | MORPH, STATIC |
| G3AN-041 | Ownership | A malformed required shape record could shrink the prefix and then be overwritten by a later append, leaking subsequent owned channels. | Preserve initialized-record authority and refuse growth from an incomplete prefix. | MORPH |
| G3AN-042 | Ownership | Finalization walked only the mutable logical shape prefix, leaking initialized or reserved records hidden by corruption. | Walk every record in the actual allocated shape table. | MORPH |
| G3AN-043 | Ownership | Finalization freed mutable top-level and packed mirrors, risking foreign frees and leaks. | Free only the six private top-level allocation identities. | MORPH |
| G3AN-044 | Ownership | Shape-table growth copied/freed mutable public mirrors. | Stage replacements from owned identities and publish all four arrays transactionally. | MORPH |
| G3AN-045 | Ownership | Packed-payload rebuild freed mutable packed mirrors. | Replace and free only the owned packed identities. | MORPH |
| G3AN-046 | Correctness | A shape's required position channel was read through a mutable pointer. | Restore it from a per-shape owned identity. | MORPH |
| G3AN-047 | Correctness | Optional normal channels were read through mutable pointers. | Restore them from per-shape owned identities. | MORPH |
| G3AN-048 | Correctness | Optional tangent channels were read through mutable pointers. | Restore them from per-shape owned identities. | MORPH |
| G3AN-049 | Ownership | Per-shape finalization freed the three mutable channel mirrors. | Free only the three per-shape allocation identities. | MORPH |
| G3AN-050 | Correctness | Corrupt fixed-size shape names could be unterminated before `strcmp` or persistence. | Force byte 63 to NUL during shape repair. | MORPH |
| G3AN-051 | Correctness | Persistence append called unbounded `strlen` on an external fixed-size name. | Use a bounded 64-byte terminator search and reject unterminated names. | MORPH, STATIC |
| G3AN-052 | Correctness | Persistence append sized allocations from corrupt destination vertex metadata. | Repair destination storage before validating and allocating the staged copy. | MORPH |
| G3AN-053 | Correctness | Clone sized channel copies from corrupt source vertex metadata. | Repair source storage before creating the destination or computing byte counts. | MORPH |
| G3AN-054 | Correctness | Clone copied nonfinite/extreme channel lanes verbatim. | Sanitize every copied position, normal, and tangent lane. | MORPH |
| G3AN-055 | Correctness | Remapped clone likewise copied corrupt selected lanes. | Sanitize every mapped channel triple. | MORPH |
| G3AN-056 | Correctness | Repairing a zero payload generation could leave a packed payload falsely clean at generation one. | Mark packed storage dirty whenever zero is repaired. | MORPH |
| G3AN-057 | Correctness | The same zero-generation repair could alias a stale maximum-delta cache. | Clear its represented generation and cached value. | MORPH |
| G3AN-058 | Correctness | Generation wrap to one could alias a maximum cache previously computed at generation one. | Invalidate maximum-cache generation on every payload mutation. | MORPH |
| G3AN-059 | Correctness | Frame serial zero doubled as “history uninitialized,” skipping legitimate frame-zero state. | Track motion-history initialization with an explicit flag. | MORPH |
| G3AN-060 | Correctness | A draw with no active position deltas returned before advancing weight history. | Advance history for every otherwise valid draw. | MORPH, ROB |
| G3AN-061 | Correctness | CPU fallback also skipped history, so switching back to GPU resurrected an old snapshot. | Advance the same temporal state before backend-path selection. | MORPH, ROB |
| G3AN-062 | Correctness | Clone copied noncanonical/inconsistent temporal flags. | Canonicalize initialization and previous-history state in clones and remaps. | MORPH |
| G3AN-063 | Performance | Repeating an identical position delta invalidated packed buffers and downstream uploads. | Compare sanitized lanes before writing or bumping generation. | MORPH |
| G3AN-064 | Performance | A zero normal edit allocated a full vertex channel, and repeated values still invalidated caches. | Preserve the implicit sparse zero and make identical writes no-ops. | MORPH |
| G3AN-065 | Performance | A zero tangent edit allocated storage, forced CPU morphing, and repeated values invalidated caches. | Preserve sparse zero tangent state and skip identical writes. | MORPH |
| G3AN-066 | Correctness | CPU accumulation could overflow float lanes and later replace the entire position with the base vertex. | Accumulate each lane in double and saturate to finite float range. | MORPH, STATIC |
| G3AN-067 | Correctness | The overflow fallback copied nonfinite base position lanes back into output. | Repair each base-position lane independently to a finite value. | MORPH |
| G3AN-068 | Correctness | Degenerate morphed normals/tangents copied corrupt or non-unit base directions. | Sanitize and normalize the fallback direction, or publish zero. | MORPH |
| G3AN-069 | Performance | Detaching from an already-unbound mesh still advanced the geometry revision. | Return before mutation when the retained slot is already null. | MORPH |
| G3AN-070 | Correctness | Mesh binding compared against corrupt morph vertex metadata. | Repair the morph container before enforcing exact vertex-count compatibility. | MORPH |

### NavAgent3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3AN-071 | Correctness | Path following read and wrote through a mutable corner pointer. | Keep an owned corner identity and restore the legacy mirror before public operations. | NAV |
| G3AN-072 | Ownership | Path clear/finalization freed the mutable corner mirror. | Free only the private corner identity. | NAV |
| G3AN-073 | Correctness | Corrupt path count could overrun or hide the current corner allocation. | Restore the exact published point count from allocation authority. | NAV, STATIC |
| G3AN-074 | Correctness | Corrupt path index could address before or after the corner array. | Clamp it to the current allocation during repair. | NAV |
| G3AN-075 | Correctness | `has_path` could remain true without any usable owned path storage. | Clear path state and remaining distance when either owned path allocation is absent. | NAV |
| G3AN-076 | Correctness | Corners and derived lengths were not one transaction, permitting mismatched derived state. | Stage a matching suffix-length allocation and publish both only after complete success. | NAV |
| G3AN-077 | Performance | Remaining distance resummed every later corner on every update/getter, making long-path following O(corners) per tick. | Cache suffix polyline lengths and answer with one live hop plus one indexed suffix. | NAV |
| G3AN-078 | Correctness | Automatic repath cleared a still-valid path before a transient navmesh query failure. | Preserve the old path until a replacement query succeeds. | NAV |
| G3AN-079 | Correctness | Suffix-cache allocation failure after a successful query also destroyed the old path. | Treat query points and suffix storage as one staged replacement. | NAV |
| G3AN-080 | Correctness | Target distance was computed before repath changed synchronized position/target state. | Recompute it after every due repath. | NAV |
| G3AN-081 | Correctness | Avoidance collection wrote through a mutable neighbor-scratch pointer. | Restore and grow from a private allocation identity. | NAV |
| G3AN-082 | Correctness | Mutable neighbor capacity was accepted as an allocation bound. | Restore it from private allocation capacity. | NAV, STATIC |
| G3AN-083 | Ownership | Finalization freed the mutable neighbor pointer. | Free only the private scratch identity. | NAV |
| G3AN-084 | Correctness | Missing neighbor ownership with a positive private capacity could report success and append through null. | Reset allocation capacity whenever its owner identity is absent. | NAV |
| G3AN-085 | Ownership | Wrong-class retained navmesh corruption could be dereferenced or released as owned. | Clear it as an unowned mismatched slot. | NAV |
| G3AN-086 | Ownership | Wrong-class Character3D binding corruption had the same lifecycle gap. | Validate and clear the retained character slot centrally. | NAV |
| G3AN-087 | Ownership | Wrong-class SceneNode3D binding corruption had the same lifecycle gap. | Validate and clear the retained node slot centrally. | NAV |
| G3AN-088 | Correctness | Radius, height, avoidance radius, stopping distance, and desired speed could retain nonfinite, negative, or unbounded values. | Persist bounded canonical configuration at every handle boundary. | NAV |
| G3AN-089 | Correctness | Remaining distance, repath interval, and repath accumulator could retain invalid values. | Repair them to bounded, internally consistent values. | NAV |
| G3AN-090 | Correctness | Position, velocity, desired velocity, and target vectors could retain nonfinite/extreme lanes. | Persist coordinate- or speed-bounded lanes centrally. | NAV |
| G3AN-091 | Correctness | Target/path/repath/avoidance flags retained noncanonical byte values, and path state could survive without a target. | Canonicalize all four flags and reconcile path presence with target/storage state. | NAV |
| G3AN-092 | Correctness | Repair could increase effective avoidance reach without expanding the global grid-query bound, allowing peer misses. | Grow the bound immediately from repaired radius and speed. | NAV |
| G3AN-093 | Performance | A failed raw node-position read allocated a temporary Vec3 only to reinterpret local space as world space. | Use the allocation-free component helper and fail to a finite origin. | NAV, STATIC |
| G3AN-094 | Performance | Every parented node write allocated parent/world/inverse/local runtime wrappers. | Use the allocation-free transactional world-position setter. | NAV, STATIC |
| G3AN-095 | Correctness | If the parent transform was singular, the old fallback wrote a world position directly as local and moved the child incorrectly. | Leave local state unchanged when exact world-to-local conversion fails. | NAV |
| G3AN-096 | Performance | Unchanged desired-speed assignments rescanned the full process-wide registry. | Compare repaired state and return in O(1). | NAV |
| G3AN-097 | Performance | Desired-speed increases also rescanned all agents even though the maximum reach can only grow. | Update the global maximum directly; rescan only when a former maximum shrinks. | NAV |
| G3AN-098 | Performance | Avoidance-radius assignments had the same unconditional global rescan. | Make unchanged/increasing assignments O(1), retaining a shrink-only rescan. | NAV |
| G3AN-099 | Correctness | Character or node rebinding synchronized a new start position but kept a path built from the old binding. | Immediately rebuild an active target path after either binding changes. | NAV |
| G3AN-100 | Correctness | Neighbor-count increment and large scratch-capacity doubling could overflow signed `int32_t` before their progress checks. | Reject an exhausted count, guard at `INT32_MAX / 2`, and switch directly to the validated required capacity. | STATIC, BUILD |
| G3AN-101 | Ownership | A failed path provider result could pair a nonpositive count with an allocated corner buffer, which the failure branch leaked. | Release any staged corner allocation before applying preserve-or-clear failure policy. | BUILD, STATIC |

### MorphTarget3D draw-integration closeout

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3AN-102 | Ownership | CPU morph fallback retained a mesh before scratch-buffer tracking, but a tracking failure returned without undoing that new retain. | Detect whether the mesh was already tracked and roll back only the retain introduced by the rejected draw. | DRAW, STATIC |
| G3AN-103 | Correctness | GPU setup failure unconditionally untracked the morph target, so a target already retained for an earlier deferred draw lost that draw's lifetime protection. | Record preexisting tracking and release only a morph retain acquired by the failing attempt. | DRAW |
| G3AN-104 | Correctness | Morph weight history advanced before fallible CPU/GPU resource setup, allowing a rejected draw to replace the last accepted frame's temporal state. | Commit history only after the selected path has secured its fallible local resources. | DRAW |

The audit corrected **104** distinct issues: 30 in IKSolver3D, 43 in
MorphTarget3D, and 31 in NavAgent3D. Of these, 10 are explicit performance
findings; the remainder close correctness, ownership, numeric, cache, and
lifecycle failures that could also have secondary performance costs.

## 4. Public behavior documentation

The affected headers now document the corrected observable contracts:

- FABRIK rejects wrong collection kinds, inconsistent bounds, and non-integer
  elements without invoking a trapping indexed accessor; solver validation
  occurs even at zero weight, and look-at orientation is parent-relative.
- No-op morph edits preserve caches, implicit zero normal/tangent channels stay
  sparse, clone state is canonicalized, null detach is idempotent, and every
  accepted draw advances temporal weight history consistently. Rejected draw
  setup leaves prior history and preexisting lifetime protection intact.
- NavAgent3D remaining-distance queries use a suffix cache, automatic repaths
  retain a valid path across transient failure, binding changes replan from the
  synchronized position, and singular parent transforms reject inexact world
  writes.

These are compatibility-preserving clarifications of existing calls, not new
ABI surface.

## 5. Validation

Every build for this audit is incremental because concurrent work was active in
the checkout. The supported script was always invoked with
`ZANNA_SKIP_CLEAN=1`; no clean or raw full CMake build was used.

```sh
ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 ZANNA_SKIP_LINT=1 \
  ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 ZANNA_SKIP_INSTALL=1 \
  ./scripts/build_zanna_mac.sh

ctest --test-dir build \
  -R '^(test_rt_animcontroller3d|test_rt_morphtarget3d|test_rt_navagent3d|test_rt_graphics3d_robustness|test_rt_canvas3d_gpu_paths)$' \
  --output-on-failure -j4

ctest --test-dir build -L graphics3d --output-on-failure -j4

cppcheck --project=build/compile_commands.json \
  --file-filter='*/src/runtime/graphics/3d/*' \
  --enable=warning,performance,portability --check-level=exhaustive \
  --error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem

./scripts/lint_platform_policy.sh
git diff --check
```

Closeout results:

- incremental macOS warnings-as-errors build: passed;
- focused IK/Morph/Nav/draw/robustness tests: passed;
- complete Graphics3D label: passed;
- exhaustive Graphics3D static analysis: passed with no findings;
- platform-policy lint and whitespace validation: passed.

## 6. Related records

- [Graphics3D Runtime Correctness Audit Follow-up (2026-08)](graphics3d-runtime-audit-followup-2026-08.md)
- [Graphics3D Runtime Correctness Audit (2026-08)](graphics3d-runtime-audit-2026-08.md)
- [Graphics3D Runtime Hardening Program (2026-07)](graphics3d-runtime-hardening-2026-07.md)
