---
status: complete
audience: contributors
last-verified: 2026-08-09
---

# Graphics3D Runtime Hardening Program (2026-08-09)

## 1. Summary and objective

This is the traceability record for a 102-finding review of the C runtime
under `src/runtime/graphics/3d`. It starts after the July/August 2026 audit
ledgers, so an item is counted only when it describes a distinct defect or
measurable avoidable cost not already closed by those ledgers.

The program is complete when at least 100 findings have focused regression or
static evidence, every implemented batch remains below 50 changed files, the
Graphics3D CTests and prescribed platform gates pass, and public behavior
changes are reflected in the runtime guides. This tranche concentrates on
previously thinly tested traversal-query, node-metadata, transient-frame
ownership, character/platform, trigger, animation, and spatial-index seams.

## 2. Scope and configuration

In scope:

- memory safety, malformed/corrupt-state containment, finite numeric policy,
  ownership transactions, deterministic failure outputs, and bounded work;
- hot-path allocation removal and retained-memory limits where behavior is
  unchanged;
- focused unit/CTest coverage and public documentation clarifications.

Out of scope:

- IL grammar, opcodes, verifier rules, runtime C ABI additions/removals, and CI
  workflow changes;
- external dependencies or platform-specific implementations outside the
  existing adapter boundaries.

No feature toggle or runtime configuration is added. These are unconditional
correctness and resource-safety guarantees. Existing public signatures and
successful-call behavior remain compatible, so this tranche does not require
an ADR.

## 3. Error and validation contract

- Wrong public object kinds retain their existing trap/null conventions.
- Non-finite spatial inputs fail closed rather than being translated to the
  origin; finite coordinates and distances use the shared Physics3D query
  ceilings.
- Optional raw-overlap outputs are initialized deterministically before any
  early return.
- Metadata mutations reject malformed UTF-8 and structurally invalid retained
  tables without publishing a partial replacement.
- Rollback refunds only allocations whose ownership was actually removed.

## 4. Corrected issue ledger

Evidence keys used below:

- `PROBE`: focused traversal/raw-overlap unit coverage.
- `META`: focused SceneNode metadata unit coverage and persistence checks.
- `NODEANIM`: focused NodeAnimation3D/NodeAnimator3D unit coverage.
- `CHAR`: focused Character3D controller unit and third-person coverage.
- `TRIGGER`: focused Trigger3D occupancy/edge unit coverage.
- `TEMP`: focused Canvas3D transient-manager unit coverage.
- `SPATIAL`: focused SceneGraph BVH, query, refit, and deformation-bound coverage.
- `STATIC`: direct source or documentation inspection for a deterministic local invariant.
- `BUILD`: warning-as-error incremental runtime build.
- `G3D`: complete `graphics3d` CTest label.
- `PLATFORM`: platform-policy lint and cross-platform smoke.

| ID | Area | Class | Finding and required resolution | Evidence |
|---|---|---|---|---|
| G3H-001 | Traversal probes | Correctness | Non-finite `Vec3` lanes were replaced with zero, silently moving invalid probes to another location. Reject non-finite vectors. | PROBE |
| G3H-002 | Traversal probes | Numeric | Finite probe coordinates bypassed the shared Physics3D coordinate ceiling. Reuse the query sanitizer so derived arithmetic remains bounded. | PROBE |
| G3H-003 | Traversal probes | Numeric | Positive radius/height/depth values had no upper bound. Apply the shared query-distance ceiling before any products or sums. | PROBE |
| G3H-004 | Traversal probes | Numeric | Horizontal direction length used overflow-prone `sqrt(x*x + z*z)`. Normalize with robust finite math. | PROBE |
| G3H-005 | Traversal probes | Numeric | Vault reach arithmetic (`thickness + 2*radius`) could overflow or exceed the supported sweep range. Compute it with a saturating bounded sum. | PROBE |
| G3H-006 | Traversal probes | Correctness | The wall-sweep capsule's upper endpoint used `maxHeight / 2`, so its actual height was shorter than the standing capsule promised by the API. Build the complete foot-level capsule axis. | PROBE |
| G3H-007 | Traversal probes | Bounds | Clearance scanned `world->body_count` without clamping it to the allocated body table. Bound iteration by pointer/count/capacity agreement. | PROBE |
| G3H-008 | Traversal probes | Type safety | Clearance trusted every retained body slot as a `Body3D`. Skip invalid slots before dereferencing geometry/filter fields. | PROBE |
| G3H-009 | Raw overlap | Type safety | Invalid collider handles were passed to a setter whose failure was ignored, leaving fallback spheres eligible to report false hits. Validate both colliders before constructing the query poses. | PROBE |
| G3H-010 | Raw overlap | Correctness | Non-finite positions were component-wise replaced with zero, allowing malformed combat volumes to hit at the origin. Reject non-finite positions and clamp valid coordinates. | PROBE |
| G3H-011 | Raw overlap | Correctness | Raw pose quaternions bypassed the normalization guaranteed by `Body3D.SetOrientation`. Normalize finite inputs and use identity for a degenerate quaternion. | PROBE |
| G3H-012 | Raw overlap | Determinism | Miss/invalid-input paths left optional normal, depth, and point outputs unchanged. Initialize all supplied outputs before validation. | PROBE |
| G3H-013 | Raw overlap | Numeric | A finite depth alone was enough to publish unchecked normal/point lanes. Normalize the normal and bound witness coordinates/depth before publication. | PROBE |
| G3H-014 | Raw overlap | Performance | Every combat narrow-phase pair allocated two GC `Body3D` shells. Use stack-local, borrowed-collider query poses. | PROBE |
| G3H-015 | Clearance | Performance | Every clearance query allocated a GC `Body3D` shell in addition to its temporary collider. Use a stack-local query body and retain only the required collider allocation. | PROBE |
| G3H-016 | Ledge results | Numeric | Result getters trusted retained vector/scalar/Boolean lanes. Return bounded finite vectors/heights and canonical flags. | PROBE |
| G3H-017 | Ledge results | Diagnostics | Allocation failure from `ProbeVault` reported `ProbeLedge` in the trap text. Pass the owning API name to the result allocator. | STATIC, BUILD |
| G3H-018 | Node metadata | Validation | The metadata string validator claimed UTF-8 but accepted arbitrary non-NUL bytes. Enforce the runtime's strict UTF-8 predicate for keys and string values. | META |
| G3H-019 | Node metadata | Bounds | Table validation checked only aggregate count/capacity fields, so invalid key lengths, kinds, or unsorted entries could reach binary-search dereferences. Validate every retained entry and strict ordering. | META |
| G3H-020 | Node metadata | Numeric | Retained float/string/Boolean payload corruption could escape through typed getters. Validate payload invariants and return documented defaults on failure. | META |
| G3H-021 | Node metadata | Determinism | `MetadataKeys` could enumerate a structurally valid outer table whose individual entries were malformed. Reject the complete malformed table instead of publishing a prefix. | META |
| G3H-022 | Canvas transient manager | Accounting | Mesh-snapshot rollback refunded caller-supplied bytes even when the corresponding buffer was no longer tracked, allowing duplicate rollback to undercount live memory. Refund only successfully untracked allocations. | TEMP |
| G3H-023 | Canvas frame arena | Resource | Reset retained the first eight chunks regardless of byte size, so one exceptional allocation could pin an arbitrarily large block for the canvas lifetime. Enforce a total retained-byte ceiling as well as the chunk-count ceiling. | TEMP |
| G3H-024 | Canvas transient manager | Documentation | The temp-object tracker documentation said hash-set allocation failure failed the operation, while the implementation intentionally falls back to the authoritative list. Document the actual ownership guarantee. | STATIC |
| G3H-025 | Node animation | Validation | Clip names accepted malformed UTF-8 and could later cross serialization and lookup seams. Normalize malformed imported names to the documented empty name. | NODEANIM |
| G3H-026 | Node animation | Correctness | Embedded NUL bytes let distinct clip names alias through C-string lookup. Reject the ambiguous identifier and compare exact stored byte spans. | NODEANIM |
| G3H-027 | Node animation | Validation | Channel target names accepted malformed UTF-8 and embedded NUL bytes. Reject invalid target identifiers before retaining channel state. | NODEANIM |
| G3H-028 | Node animator | Correctness | `Play` used prefix-truncated C-string equality rather than exact runtime-string contents. Match validated byte length and contents. | NODEANIM |
| G3H-029 | Node animator | Memory safety | A positive target-cache capacity paired with a null pointer reached cache reads and writes. Repair pointer/capacity disagreement before use. | NODEANIM |
| G3H-030 | Node animator | Bounds | Target-cache capacity and allocation arithmetic were unbounded under corrupt clip metadata. Enforce the clip-channel ceiling and checked allocation size. | NODEANIM |
| G3H-031 | Node animator | Memory safety | Sample-scratch pointer/capacity disagreement could return null as writable storage or resize from corrupt metadata. Repair and bound the tuple before sampling. | NODEANIM |
| G3H-032 | Node animator | Memory safety | Traversal-stack pointer/capacity disagreement could write through a null or stale pointer. Repair the tuple before every traversal. | NODEANIM |
| G3H-033 | Node animator | Bounds | Target resolution had no independent traversal budget, so corrupt/shared scene topology could consume unbounded work and storage. Cap visited nodes and stack growth. | NODEANIM |
| G3H-034 | Node animator | Type safety | Target lookup trusted child slots as `SceneNode` objects. Skip wrong-class children before reading names or descendants. | NODEANIM |
| G3H-035 | Node animator | Type safety | A wrong-class cached target was dereferenced as a scene node. Validate the cache hit before use and resolve it again when invalid. | NODEANIM |
| G3H-036 | Node animator | Ownership | Cache replacement/finalization invoked object lifetime operations on unchecked retained pointers. Release only validated cached scene nodes during corruption recovery. | NODEANIM |
| G3H-037 | Node animator | Bounds | Descendant checks followed parent pointers without cycle detection or a visit limit. Detect parent cycles and fail closed. | NODEANIM |
| G3H-038 | Node animator | Correctness | Repairing a compacted clip table left target-cache keys and retained targets associated with the old indices. Invalidate the cache whenever compaction or current-index repair changes clip identity. | NODEANIM |
| G3H-039 | Node animation | Numeric | Binary key search trusted interior key times after validating only aggregate channel shape. Validate the selected key and both neighbors before interpolation. | NODEANIM |
| G3H-040 | Node animation | Numeric | Non-finite selected sample values propagated into node transforms and morph weights. Reject the sample before publication. | NODEANIM |
| G3H-041 | Node animation | Numeric | Cubic interpolation trusted non-finite selected tangent spans. Validate both tangents before Hermite evaluation. | NODEANIM |
| G3H-042 | Node animation | Numeric | Finite inputs could still overflow interpolation into non-finite output. Validate the completed sample and skip the channel on overflow. | NODEANIM |
| G3H-043 | Node animator | Numeric | The `Speed` getter exposed corrupt non-finite or unbounded private state. Return a finite value clamped to the supported playback range. | NODEANIM |
| G3H-044 | Node animator | Numeric | The `Time` getter exposed negative, non-finite, or extreme private state. Return a finite value in the supported timeline range. | NODEANIM |
| G3H-045 | Node animator | Correctness | Forward one-shot playback remained marked playing when a frame landed exactly on the clip duration. Stop on the exact endpoint. | NODEANIM |
| G3H-046 | Node animator | Correctness | Reverse one-shot playback remained marked playing when a frame landed exactly on time zero. Stop on the exact endpoint. | NODEANIM |
| G3H-047 | Character controller | Type safety | Collision candidate filtering dereferenced unchecked world slots as `Body3D`. Validate the class before reading trigger, motion, or filter fields. | CHAR |
| G3H-048 | Character controller | Type safety | A malformed world body table could be passed into the shared query broadphase, whose internal cache assumes typed entries. Keep malformed tables on the class-checking direct fallback. | CHAR |
| G3H-049 | Character controller | Correctness | A velocity with one non-finite lane silently applied its finite sibling lanes. Reject the complete vector so malformed input cannot cause partial movement. | CHAR |
| G3H-050 | Character controller | Type safety | Public position/height operations and finalization trusted the retained private body kind. Validate it before dispatch, dereference, or release. | CHAR |
| G3H-051 | Character controller | Type safety | The retained world getter, replacement, movement path, and finalizer trusted private world kind metadata. Validate the world at every ownership/use boundary. | CHAR |
| G3H-052 | Character controller | Ownership | Ground-body replacement, lookup, and finalization performed lifetime operations on unchecked private pointers. Retain/release/expose only validated bodies. | CHAR |
| G3H-053 | Character controller | Correctness | Switching or detaching worlds preserved grounded/support state from the old world. Clear grounding, landing, sliding, and the shortlist when world identity changes. | CHAR |
| G3H-054 | Character controller | Correctness | Teleporting preserved the old grounded platform, so the next move could inherit motion from a remote support. Clear ground state on `SetPosition`. | CHAR |
| G3H-055 | Character controller | Correctness | A retained ground body removed from the world could continue driving platform motion. Require bounded current-world membership and clear stale public support state. | CHAR |
| G3H-056 | Moving platforms | Correctness | Platform displacement was written directly to the character pose, allowing fast platforms to teleport a rider through walls. Route the displacement through the bounded sweep solver. | CHAR |
| G3H-057 | Moving platforms | Correctness | The movement shortlist started after direct platform displacement, so it could omit obstacles along the ride path. Build the shortlist from the pre-ride position and total travel. | CHAR |
| G3H-058 | Moving platforms | Correctness | Achieved body velocity used a start position captured after the ride and therefore omitted platform motion. Measure from the pre-ride position. | CHAR |
| G3H-059 | Moving platforms | Numeric | Extreme finite platform yaw was passed directly to trigonometric functions, losing useful argument precision. Reduce the angle modulo one turn first. | CHAR |
| G3H-060 | Moving platforms | Numeric | Platform linear/rotational displacement bypassed the controller movement ceiling. Sanitize and direction-preservingly cap the combined ride delta. | CHAR |
| G3H-061 | Character controller | Resource | A corrupt enormous shortlist capacity could be trusted as writable storage and suppress reallocation. Discard capacity beyond the bounded world-body ceiling. | CHAR |
| G3H-062 | Character controller | Bounds | Direct world fallback scans accepted pointer/count/capacity disagreement and unbounded counts. Use one bounded table validator at every scan. | CHAR |
| G3H-063 | Trigger occupancy | Type safety | `Trigger3D.Update` called `body_aabb` on unchecked world slots. Skip wrong-class entries before any body access. | TRIGGER |
| G3H-064 | Trigger occupancy | Correctness | Duplicate/corrupt world slots could count the same body as entering more than once in one update. Use the existing epoch stamp to process each body once. | TRIGGER |
| G3H-065 | Trigger documentation | Documentation | The rendering reference described `Contains` as accepting a body, but the runtime performs a point-in-AABB query on `Vec3`. Correct the signature and description. | STATIC |
| G3H-066 | Trigger documentation | Documentation | The guide described `Update` as accepting one body, but the runtime recomputes occupancy against a world. Correct the signature and frame contract. | STATIC |
| G3H-067 | Character documentation | Documentation | The guide named the public grounded property `Grounded`; the runtime surface is `IsGrounded`. Correct the reference name. | STATIC |
| G3H-068 | Spatial storage | Memory safety | A null entry pointer with a positive cached capacity was treated as writable storage. Discard pointer/capacity disagreement before rebuild. | SPATIAL |
| G3H-069 | Spatial storage | Memory safety | A null leaf-order pointer with a positive capacity suppressed allocation and reached index writes. Normalize the tuple before rebuild. | SPATIAL |
| G3H-070 | Spatial storage | Memory safety | A null BVH-node pointer with a positive capacity suppressed allocation and reached node writes. Normalize the tuple before rebuild. | SPATIAL |
| G3H-071 | Spatial storage | Bounds | Corrupt capacities above the runtime's bounded scene policy could be trusted as real allocations. Discard policy-exceeding entry, ordering, and node storage. | SPATIAL |
| G3H-072 | Spatial entries | Bounds | Entry growth and append lacked an explicit logical ceiling independent of signed allocation arithmetic. Enforce the one-million-entry limit before increment. | STATIC, BUILD |
| G3H-073 | Spatial ordering | Bounds | Leaf-order allocation growth accepted unsupported sizes and relied on later allocator failure. Bound requested capacity and multiplication. | STATIC, BUILD |
| G3H-074 | Spatial BVH | Bounds | BVH node allocation could overflow signed growth near corrupt capacities. Use the node ceiling plus checked growth/multiplication. | STATIC, BUILD |
| G3H-075 | Query candidates | Memory safety | Pooled candidate pointer/count/capacity disagreement could write through null or beyond the logical list. Repair the tuple and bound growth before append. | SPATIAL |
| G3H-076 | BVH traversal | Memory safety | Query-stack metadata was trusted across growth and could suppress allocation or overflow its count. Normalize the tuple and enforce the node ceiling. | SPATIAL |
| G3H-077 | Spatial cache | Correctness | The clean-index fast path returned without validating aggregate live ranges and backing pointers. Validate constant-time storage invariants before accepting the cache. | SPATIAL |
| G3H-078 | Spatial cache | Correctness | A cached BVH root whose parent was not the root sentinel could enter malformed topology. Require `parent == -1` and rebuild on mismatch. | SPATIAL |
| G3H-079 | AABB queries | Numeric | Internal BVH collection accepted non-finite or inverted query bounds. Reject them before traversal. | SPATIAL |
| G3H-080 | BVH nodes | Numeric | Cached node bounds were consumed without checking finite ordered lanes. Invalidate the cache before intersection arithmetic. | SPATIAL |
| G3H-081 | BVH leaves | Memory safety | Leaf `start + count` validation could overflow or escape the ordering allocation. Validate with subtraction against both live count and capacity. | SPATIAL |
| G3H-082 | BVH leaves | Memory safety | Cached leaf entry indexes were trusted before dereferencing the entry array. Range-check every index. | SPATIAL |
| G3H-083 | Spatial entries | Type safety | Cached entries could retain a wrong-class node and later dereference it during query/refit. Validate each node kind before use. | SPATIAL |
| G3H-084 | Spatial entries | Correctness | Corrupt noncanonical visibility/cullability bytes could change filtering and accounting semantics. Accept only zero or one. | SPATIAL |
| G3H-085 | BVH topology | Correctness | Internal children could be self-referential, duplicated, out of range, or disagree with their parent link. Validate the complete local edge before pushing either child. | SPATIAL |
| G3H-086 | BVH traversal | Resource | Cyclic/shared cached topology had no visit budget and could loop or grow scratch indefinitely. Bound work to the live node count. | SPATIAL |
| G3H-087 | Spatial diagnostics | Numeric | Rejected-candidate accumulation could overflow signed `int32_t`. Use saturating non-negative addition. | STATIC, BUILD |
| G3H-088 | Spatial rebuild | Correctness | A failed rebuild could leave the previous cache marked valid while partially overwriting its arrays. Invalidate and reset topology before mutation. | SPATIAL |
| G3H-089 | Spatial rebuild | Resource | Hierarchy traversal and traversal-order increment were not independently capped. Enforce bounded work and order before any append. | SPATIAL |
| G3H-090 | Spatial rebuild | Type safety | Wrong-class child slots and cyclic parent chains could be traversed as scene nodes. Validate classes and reject cyclic chains with bounded work. | SPATIAL |
| G3H-091 | Spatial visibility | Resource | Effective-visibility lookup followed unchecked ancestor chains and could loop on a parent cycle. Validate and bound the walk. | SPATIAL |
| G3H-092 | Spatial animation | Resource | Effective-animator lookup followed unchecked ancestors and could loop or dereference a wrong-class parent. Validate and bound the walk. | SPATIAL |
| G3H-093 | Spatial refit | Resource | Ancestor-dirty lookup had the same unbounded/cyclic parent-chain exposure. Validate and bound it independently. | SPATIAL |
| G3H-094 | BVH refit | Memory safety | Full refit trusted node kinds, leaf ranges, entry indexes, children, and bounds. Validate each node/edge and fail to a rebuild when any invariant is broken. | SPATIAL |
| G3H-095 | BVH path refit | Resource | Parent-path refit could loop on a cached parent cycle and reported no failure to its caller. Add a node-count budget and propagate failure. | SPATIAL |
| G3H-096 | Spatial refit | Correctness | A dirty node whose cached revisions happened to match did not refit, leaving poisoned entry/BVH bounds intact. Track refresh attempts separately and force a validating refit. | SPATIAL |
| G3H-097 | Spatial refit | Numeric | The rebuild-ratio test multiplied a corrupt/large refresh count by eight in signed arithmetic. Compare against a rounded division threshold instead. | STATIC, BUILD |
| G3H-098 | Flat collection | Type safety | Full-entry collection trusted every cached entry because it did not traverse leaves. Apply the same entry validation as indexed collection. | SPATIAL |
| G3H-099 | BVH construction | Memory safety | Recursive range construction trusted ordering indexes and entry bounds after sorting. Validate every range/index/bound before deriving aggregate nodes. | STATIC, BUILD |
| G3H-100 | Morph culling | Correctness | A supported raw morph workload above the bounded scan budget returned zero padding and could under-cull deformed geometry. Use the conservative scene-distance ceiling without scanning. | SPATIAL |
| G3H-101 | Skeletal culling | Numeric | A non-finite live palette translation was ignored as zero reach, producing an undersized skinned bound. Contain it with the conservative scene-distance ceiling and a bone-count guard. | SPATIAL |
| G3H-102 | Flat collection | Performance | Full spatial collection sorted an already traversal-ordered entry stream on every draw/query. Skip the `O(n log n)` sort for the normal empty destination while preserving sorting for append callers. | STATIC, SPATIAL |

## 5. Regression scenarios

- Given non-finite or extreme traversal inputs, when clearance/ledge/vault is
  queried, then the call fails or uses the documented shared ceiling without
  producing non-finite result state.
- Given invalid colliders, non-unit quaternions, and pre-filled raw outputs,
  when raw overlap misses or rejects input, then no fallback sphere hit occurs
  and outputs contain deterministic defaults.
- Given invalid UTF-8 or corrupted retained metadata entries, when metadata is
  read or mutated, then no entry is exposed and the table remains unchanged.
- Given a duplicate snapshot rollback, when byte accounting is inspected, then
  only the first ownership transfer changes the live-byte count.
- Given an arena allocation above the retention ceiling, when the frame resets,
  then that exceptional chunk is released while ordinary chunks remain reusable.
- Given malformed identifiers, corrupt cache metadata, wrong-class scene slots,
  parent cycles, or non-finite key data, when a node animator updates, then it
  rejects or repairs the state without unsafe traversal or transform pollution.
- Given non-looping forward or reverse playback that lands exactly on an
  endpoint, when the animator updates, then playback stops on that frame.
- Given a malformed velocity, wrong-class world slot, removed platform,
  teleport, or world replacement, when a character moves or exposes its state,
  then it fails closed and cannot inherit stale support motion.
- Given a fast platform carrying a character toward a wall, when the platform
  displacement is applied, then the ride is swept, blocked, and included in
  the achieved velocity.
- Given duplicate or wrong-class world slots, when a trigger updates, then each
  valid body contributes at most one edge and invalid entries are skipped.
- Given malformed BVH storage, bounds, indexes, links, or parent cycles, when a
  query/refit runs, then the cache rebuilds or uses the exact flat fallback with
  bounded work and no out-of-range access.
- Given a supported raw morph stream above the culling scan budget or a
  non-finite skeletal palette translation, when dynamic bounds are computed,
  then the finite conservative scene ceiling is used rather than an undersized
  bound.

## 6. Validation record

Pre-change baseline on macOS arm64:

```text
ctest --test-dir build -L graphics3d --output-on-failure -j8
150/150 passed; short soak and cross-backend conformance included.
```

Final tranche results on macOS arm64:

```text
ZANNA_BUILD_DIR=build-g3h ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 \
  ZANNA_SKIP_LINT=1 ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 \
  ZANNA_SKIP_INSTALL=1 ZANNA_SKIP_STUDIO=1 ./scripts/build_zanna_mac.sh
warning-as-error build passed

ctest --test-dir build-g3h \
  -R '^(test_rt_scene3d|test_rt_physics3d|test_rt_game3d_thirdperson|test_rt_canvas3d)$' \
  --output-on-failure -j4
4/4 passed

ctest --test-dir build-g3h -L graphics3d --output-on-failure -j8
150/150 passed in 122.57 seconds; short soak, cross-backend conformance,
GPU smoke, and open-world probes included

./scripts/lint_platform_policy.sh
clean

./scripts/run_cross_platform_smoke.sh
passed on macOS arm64
```
