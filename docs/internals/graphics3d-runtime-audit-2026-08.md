---
status: completed
audience: contributors
last-verified: 2026-08-01
---

# Graphics3D Runtime Correctness Audit (2026-08)

## 1. Scope and method

This is the closeout record for a source-level audit of the C runtime below
`src/runtime/graphics/3d`. It follows the broader July Graphics3D hardening
program and the backend, light-baker, and gameplay audits already recorded in
this directory. Those records were reviewed first so this pass could concentrate
on comparatively under-covered stateful systems: Ragdoll3D, Vehicle3D,
ReflectionProbe3D, Sky3D, TimeOfDay3D, and TextureAtlas3D.

The review combined manual data-flow and ownership analysis, adversarial
white-box tests, a warnings-as-errors incremental build, and an exhaustive
`cppcheck` warning/performance/portability pass over all compiled Graphics3D C
translation units. The matrix below contains 99 newly corrected, independently
observable failure modes. Similar-looking rows are kept separate when the bad
state occurred at a different lifecycle boundary or produced a different
runtime result.

No public function signature, runtime C ABI layout, IL contract, opcode,
grammar, verifier rule, workflow, or cross-layer dependency changed. The new
capacity fields are private implementation state. Consequently this batch does
not require an ADR under the repository policy.

## 2. Evidence key

- **RAG**: `test_rt_game3d_ragdoll_time`, especially the builder topology,
  rotated-pose, controller identity, world-space root-follow, finalizer, and
  exact blend-out cases.
- **VEH**: `test_rt_physics3d`, especially
  `test_vehicle_validates_membership_and_repairs_private_counts` and
  `test_vehicle_uses_relative_contact_motion_and_reaction_forces`.
- **ROB**: `test_rt_graphics3d_robustness`, especially the atlas,
  sky/time-of-day, and reflection-probe corruption tests.
- **BUILD**: the supported macOS build script with `ZANNA_SKIP_CLEAN=1`, using
  the project's warnings-as-errors configuration.
- **STATIC**: exhaustive `cppcheck` warning, performance, and portability
  analysis plus `git diff --check` and the platform-policy linter.
- **G3D**: the complete CTest `graphics3d` label.

## 3. Corrected issue ledger

### Ragdoll3D

| ID | Defect and impact | Correction | Evidence |
|---|---|---|---|
| G3R-001 | Bind-pose segment lengths read translation from matrix slots 12-14, although runtime matrices store it in slots 3, 7, and 11; ordinary chains therefore collapsed to terminal defaults. | Read the canonical translation column at every length calculation. | RAG |
| G3R-002 | Capsule centers used the same wrong matrix row and were built around the origin instead of their bones. | Compute body origins and centers from slots 3, 7, and 11. | RAG |
| G3R-003 | Joint anchors used slots 12-14, disconnecting constraints from the generated bodies. | Derive anchors from the canonical translation column. | RAG |
| G3R-004 | Joint frame translations were passed to `Mat4` in the unused final-row positions. | Populate frame translation arguments 3, 7, and 11. | RAG |
| G3R-005 | Activation read animated bone positions from the wrong matrix row. | Extract activation positions from slots 3, 7, and 11. | RAG |
| G3R-006 | Previous-pose positions for activation velocity handoff used the wrong row. | Use canonical translations for both current and prior poses. | RAG |
| G3R-007 | Root-motion deltas used wrong-row body and animation positions. | Calculate the world delta from canonical translations. | RAG |
| G3R-008 | Active physics overrides wrote translations into slots 12-14, so consumers saw rotation-only poses. | Publish translations in slots 3, 7, and 11. | RAG |
| G3R-009 | Blend-out source poses read translations from slots 12-14. | Interpolate from the canonical translation column. | RAG |
| G3R-010 | Matrix-to-quaternion extraction used signs for the opposite matrix convention, inverting rotated bind poses. | Re-derived extraction for the runtime's column-vector transform convention. | RAG |
| G3R-011 | Quaternion-to-matrix composition emitted the transposed rotation convention. | Emit the matching runtime matrix convention and test a nontrivial rotated skeleton. | RAG |
| G3R-012 | Terminal capsules took a matrix row as their fallback bone direction. | Use and robustly normalize the transform's Y basis column. | RAG |
| G3R-013 | A parent selected the first inserted child, making capsule geometry dependent on skeleton ordering. | Select the farthest direct child deterministically. | RAG |
| G3R-014 | A real child shorter than the minimum threshold was treated like a terminal bone and assigned the terminal length. | Distinguish “has a child” from “selected into the ragdoll”; only true leaves use the terminal fallback. | RAG |
| G3R-015 | Direct Euclidean normalization overflowed for large finite bind transforms and admitted nonfinite matrices. | Validate matrices and use scaled `hypot`/normalization paths. | RAG, STATIC |
| G3R-016 | Body-volume and aggregate-volume arithmetic could overflow before mass distribution. | Reject nonfinite/nonpositive volumes before constructing bodies. | RAG, BUILD |
| G3R-017 | A per-body 0.1 mass floor silently made total ragdoll mass exceed the configured total. | Distribute the requested total proportionally without an additive floor. | RAG |
| G3R-018 | Total mass and minimum-bone-length accepted unbounded finite values that overflowed later calculations. | Bound both configuration values to the runtime numeric ceiling. | RAG, STATIC |
| G3R-019 | Activation accepted an animation controller for a different skeleton, allowing incompatible palette indexing. | Require exact controller/skeleton identity before activation. | RAG |
| G3R-020 | Reactivation inherited stale body linear and angular velocities. | Clear body motion state before computing and applying activation handoff velocities. | RAG |
| G3R-021 | Activation velocity used only bone-origin motion, omitting tangential velocity from a rotating center-of-mass offset. | Include the rotated body offset in current and previous COM positions. | RAG |
| G3R-022 | Root following applied a world-space physics delta through a local-position setter. | Read and set the node's world position explicitly. | RAG |
| G3R-023 | The override palette was rebased even if moving the scene root failed. | Rebase only after the world-position setter succeeds. | RAG |
| G3R-024 | Testing powered bone 63 shifted a signed integer into its sign bit, which is undefined behavior. | Perform mask shifts with `UINT64_C(1)`. | BUILD, STATIC |
| G3R-025 | A zero, negative, or nonfinite `Step` delta advanced state using a fabricated 1/60-second interval. | Make invalid/nonpositive deltas a documented no-op. | RAG |
| G3R-026 | Immediate deactivation could capture an override palette before any physics pose had been published. | Track `override_ready` and publish the current body pose first when necessary. | RAG |
| G3R-027 | Blend allocation failure could leave timers and retained bindings partially established. | Allocate the complete blend snapshot before committing blend state. | RAG |
| G3R-028 | Blend time was decremented after interpolation, so the last step could release the binding without ever publishing the exact animation pose. | Compute interpolation after decrement and publish `t == 1` before release. | RAG |
| G3R-029 | Activation could leave a prefix of bodies or joints registered when a later registration failed. | Track additions and roll the registered prefix back in reverse on recoverable failure. | RAG |
| G3R-030 | Corrupt `slot_count` values drove unchecked traversal beyond the allocation. | Store an immutable private slot capacity and clamp all lifecycle traversals. | RAG, STATIC |
| G3R-031 | Bone-indexed arrays had no allocation capacity with which to validate a corrupt skeleton count. | Store and verify a shared private bone capacity before indexing. | RAG, STATIC |
| G3R-032 | Blend arrays reused the mutable skeleton count as their only bound. | Track a separate blend snapshot capacity and validate it during every blend step. | RAG, STATIC |
| G3R-033 | Setting an already-equal inactive configuration value rebuilt every body and joint. | Compare repaired values and rebuild only when a value actually changes. | RAG |
| G3R-034 | Finalizing an active ragdoll released handles but left bodies and joints registered in `World3D`. | Unregister joints and bodies before releasing retained objects. | RAG |
| G3R-035 | Reactivating while blending retained new bindings without releasing the old retained controller and node. | Retain replacements first, then type-safely release and replace old bindings. | RAG |
| G3R-036 | Persistent reference slots released wrong-class corrupt pointers as though they were owned runtime objects. | Validate expected classes; clear unowned corrupt slots without releasing them. | RAG, STATIC |
| G3R-037 | Ancestor walks had no cycle/step bound if private skeleton topology became corrupt. | Bound ancestry traversal by the validated bone count. | RAG, STATIC |
| G3R-038 | Orientation/frame object allocation results were consumed without a local success check during build/update. | Check every staged runtime object before use and fail the operation coherently. | BUILD, STATIC |
| G3R-039 | Joint limit replacement could publish only part of a six-axis update when temporary allocation failed. | Stage all six values and commit the complete update together. | RAG, BUILD |
| G3R-040 | Per-frame active stepping allocated a boxed `Vec3` for every ragdoll body just to read its position. | Use the validated raw body-pose path for allocation-free reads. | RAG, STATIC |

### Vehicle3D

| ID | Defect and impact | Correction | Evidence |
|---|---|---|---|
| G3R-041 | A negative private wheel count allowed `AddWheel` to index before the array. | Repair counts before insertion and reject invalid capacity state. | VEH |
| G3R-042 | An oversized private wheel count made `Step` traverse beyond wheel storage. | Centralize a capacity-bounded safe wheel count. | VEH |
| G3R-043 | Wheel getters trusted the corrupt raw count even where stepping had been guarded. | Use the same safe count for all getters and index checks. | VEH |
| G3R-044 | Construction accepted a dynamic chassis not registered in the supplied world. | Require exact world membership at construction. | VEH |
| G3R-045 | Construction accepted static chassis bodies that cannot respond to vehicle forces. | Require a dynamic chassis and document the precondition. | VEH |
| G3R-046 | A vehicle continued applying forces after its chassis was removed from or moved to another world. | Revalidate body/world ownership on every step. | VEH |
| G3R-047 | Invalid or detached world/chassis endpoints left stale contact flags, points, normals, and suspension state visible to callers. | Clear all wheel observations on endpoint validation and membership failures. | VEH |
| G3R-048 | Corrupt wrong-class world/chassis slots could be released as owned objects. | Type-check retained slots and clear unowned corruption safely. | VEH, STATIC |
| G3R-049 | Nonfinite suspension, grip, damping, radius, rest-length, and control fields propagated NaNs into the solver. | Repair all mutable tuning and control state to bounded canonical values. | VEH |
| G3R-050 | Driven-wheel count was rescanned inside the wheel loop, making stepping quadratic in wheel count. | Count usable and driven wheels once per step. | VEH, STATIC |
| G3R-051 | Suspension damping compared chassis velocity with zero instead of the contacted body's point velocity. | Compute relative contact-point velocity against moving ground. | VEH |
| G3R-052 | Longitudinal and lateral tire slip likewise ignored moving platforms. | Build tire slip from relative chassis/ground point velocity. | VEH |
| G3R-053 | Suspension force was applied only to the chassis, creating net momentum on dynamic ground. | Apply an equal and opposite force to the contacted dynamic body. | VEH |
| G3R-054 | Tire impulses had no reaction on a dynamic contacted body. | Apply equal and opposite tire force at the ground contact. | VEH |
| G3R-055 | The friction ellipse normalized both axes with the same grip budget, misclamping asymmetric longitudinal/lateral grip. | Normalize each axis by its own force limit. | VEH |
| G3R-056 | Speed reporting returned NaN when chassis velocity was corrupt. | Validate the sampled velocity and return a finite canonical result. | VEH |
| G3R-057 | Degenerate/nonfinite contact normals and tire basis vectors could reach force calculations. | Robustly normalize the complete tire frame or skip the unusable wheel. | VEH |
| G3R-058 | Corrupt chassis forward/up axes were normalized independently but not made orthogonal, coupling steering and suspension. | Gram-Schmidt the forward vector against up and rebuild a normalized right axis. | VEH |
| G3R-059 | Invalid wheel slots remained in the brake/mass denominator, diluting force on valid wheels. | Count usable wheel records once and split loads only among them. | VEH |
| G3R-060 | A ray hit could reference a body owned by a different world, yet receive reaction forces. | Verify contacted-body ownership before using its motion or applying reactions. | VEH |

### ReflectionProbe3D

| ID | Defect and impact | Correction | Evidence |
|---|---|---|---|
| G3R-061 | Constructor bounds accepted NaN/Infinity, permanently poisoning containment and capture state. | Reject nonfinite bounds and clamp extreme finite coordinates. | ROB |
| G3R-062 | `Contains` could return true for NaN points because all ordered comparisons were false. | Reject nonfinite inputs before containment math. | ROB |
| G3R-063 | Computing `(min + max) / 2` overflowed for valid extreme bounds. | Use overflow-safe scaled midpoint arithmetic. | ROB |
| G3R-064 | Direct span and normalized-distance arithmetic overflowed for extreme but finite boxes. | Scale the point/bounds before normalized containment tests. | ROB |
| G3R-065 | Changing capture resolution did not mark the probe dirty. | Repair, compare, and mark dirty only on an actual resolution change. | ROB |
| G3R-066 | Capture accepted arbitrary objects in place of Canvas3D and Scene3D. | Validate both runtime classes and reject capture during an active frame. | ROB |
| G3R-067 | Capture unconditionally reset the canvas target, destroying a caller's prior render target. | Snapshot and restore the exact prior target. | ROB |
| G3R-068 | Restoring with the window-only reset path trapped for an offscreen canvas. | Restore offscreen targets directly through validated canvas target state. | ROB |
| G3R-069 | Every capture allocated 18 temporary `Vec3` objects for camera look-at setup. | Use the component-level camera helper for all six faces. | ROB, STATIC |
| G3R-070 | `GetCubemap` returned a wrong-class or incomplete cached object. | Validate class and six-face completeness, clearing malformed unowned cache slots. | ROB |
| G3R-071 | Corrupt resolution and position fields could drive oversized allocation or invalid camera state. | Repair resolution and position before capture. | ROB |
| G3R-072 | A malformed prior render-target owner, size, or private target could be restored through unchecked state. | Validate target identity, dimensions, and exact pixel storage before capture. | ROB |
| G3R-073 | A newly captured cubemap was published before target restoration completed. | Stage the map, restore first, then atomically replace the public cache. | ROB |
| G3R-074 | Some setup/restoration failures left the probe falsely clean. | Keep or restore `CaptureDirty` on every failed capture path. | ROB |

### Sky3D and TimeOfDay3D

| ID | Defect and impact | Correction | Evidence |
|---|---|---|---|
| G3R-075 | Direct sun-direction length calculation overflowed for huge finite vectors and collapsed them incorrectly. | Use max-component scaled normalization. | ROB |
| G3R-076 | Setting an unchanged normalized sun direction rebuilt the cubemap. | Compare repaired normalized components before marking dirty. | ROB |
| G3R-077 | Setting an unchanged ground albedo also rebuilt all six faces. | Mark dirty only after an actual clamped-color change. | ROB |
| G3R-078 | Procedural sky generation called the public pixel setter for every texel, repeatedly validating and bumping generations. | Write validated raw storage and call `pixels_touch` once per face. | ROB, STATIC |
| G3R-079 | Sky update accepted a wrong-class optional canvas handle. | Validate Canvas3D when supplied. | ROB |
| G3R-080 | Sky getters/update trusted malformed cached cubemap and corrupt numeric private fields. | Type-check/release the cache and repair resolution, turbidity, albedo, and direction. | ROB |
| G3R-081 | Repair-normalizing a corrupt private sun direction did not dirty the dependent cubemap. | Compare pre/post repair state and invalidate generated content. | ROB |
| G3R-082 | `TimeOfDay.Advance` computed `dt / dayLength * 24`, overflowing for `DBL_MAX` or tiny valid day lengths. | Reduce with `fmod` before scaling to hours. | ROB |
| G3R-083 | Nonfinite refresh direction/timers could permanently suppress environment refreshes. | Repair all time, latitude, refresh, and last-direction state before use. | ROB |
| G3R-084 | Wrong-class sky/light/environment slots were released as owned objects. | Use class-specific retained-slot cleanup. | ROB, STATIC |
| G3R-085 | Binding an object to the same slot performed unnecessary retain/release churn. | Recognize self-binding after state repair. | ROB, STATIC |
| G3R-086 | A failed sky update committed the refresh marker, delaying the next retry. | Commit `last_refresh_dir` only after successful allocation/update. | ROB |
| G3R-087 | Raw sun-direction output was left uninitialized when validation trapped. | Initialize the output to the canonical direction before validating. | ROB |

### TextureAtlas3D

| ID | Defect and impact | Correction | Evidence |
|---|---|---|---|
| G3R-088 | Atlas insertion trusted a null/corrupt backing buffer and mutable dimensions. | Validate immutable allocation metadata and repair public storage fields before use. | ROB |
| G3R-089 | Corrupt atlas width/height could produce out-of-bounds row copies. | Preserve allocation width/height and derive all copy bounds from validated values. | ROB |
| G3R-090 | Source Pixels width, height, and data layout were not validated as one exact object, permitting out-of-bounds reads. | Use the checked Pixels implementation and exact storage-size validation. | ROB |
| G3R-091 | Region repair compacted later records over malformed entries, renumbering previously returned atlas IDs. | Truncate at the first malformed region and preserve the stable prefix. | ROB |
| G3R-092 | A corrupt shelf cursor could overlap new textures with existing packed regions. | Reconstruct shelf coordinates from the validated stable prefix. | ROB |
| G3R-093 | Cached atlas Pixels could have the wrong class, dimensions, data pointer, or storage extent. | Validate the complete embedded layout and rebuild invalid caches. | ROB |
| G3R-094 | Rebuilding cached Pixels copied bytes without updating its content generation. | Touch the Pixels object once after the bulk copy. | ROB |
| G3R-095 | Atlas insertion invoked a per-pixel API in the inner copy loop. | Replace it with checked row-wise `memcpy`. | ROB, STATIC |
| G3R-096 | Corruption of the public backing pointer could redirect writes and the finalizer's free. | Keep a private allocation identity, restore the public pointer, and free only the owned allocation. | ROB |
| G3R-097 | `Add` trapped on a wrong-class source even though its documented failure contract is `-1`. | Use the nontrapping checked Pixels path and return `-1`. | ROB |
| G3R-098 | Atlas repair subtracted from a corrupt `INT32_MIN` region coordinate before validating it, invoking signed-overflow undefined behavior. | Validate region coordinates before computing the padding-row offset. | ROB, STATIC |

### Final line-by-line closeout finding

| ID | Defect and impact | Correction | Evidence |
|---|---|---|---|
| G3R-099 | Ragdoll joint-frame construction dereferenced a null boxed body-position result when temporary object allocation failed. | Check each position object before reading it and roll the staged frame build back on failure. | BUILD, STATIC |

The audit found and corrected **99** distinct runtime issues. In addition, the
existing chain-skeleton test fixture was corrected to use the runtime matrix
translation convention; its wrong-row translations had masked several of the
production defects above, so it is not counted as a separate runtime finding.

## 4. Public behavior documentation

The affected headers now document the corrected observable contracts:

- Vehicle3D construction requires a dynamic chassis already registered in the
  supplied world.
- Ragdoll3D activation requires controller/skeleton identity; configuration and
  blend durations are bounded; invalid/nonpositive step deltas are no-ops.
- Reflection capture preserves the prior render target and publishes only a
  complete capture.
- Sky3D setters invalidate generated content only on an actual effective change.
- TimeOfDay3D advances huge finite deltas with bounded arithmetic and retries a
  failed environment refresh.

These are validation/lifecycle clarifications of existing calls, not new ABI
surface.

## 5. Validation

All builds for this audit are incremental because other work was active in the
same checkout. The build invocation used the supported script and explicitly
set `ZANNA_SKIP_CLEAN=1`; no raw CMake configuration or clean build was run.

```sh
ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 ZANNA_SKIP_LINT=1 \
  ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 ZANNA_SKIP_INSTALL=1 \
  ./scripts/build_zanna_mac.sh

ctest --test-dir build \
  -R '^(test_rt_graphics3d_robustness|test_rt_physics3d|test_rt_game3d_ragdoll_time)$' \
  --output-on-failure -j3

ctest --test-dir build -L graphics3d --output-on-failure

cppcheck --project=build/compile_commands.json \
  --file-filter='*/src/runtime/graphics/3d/*' \
  --enable=warning,performance,portability --check-level=exhaustive \
  --error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem

./scripts/lint_platform_policy.sh
git diff --check
```

Closeout on 2026-08-01 produced the following results:

- incremental warnings-as-errors macOS build: passed;
- focused edited-target run: 3/3 passed;
- complete `graphics3d` label: 156/156 passed, including the short soak,
  backend fixtures, runtime manifest, and ABI-surface checks;
- exhaustive static analysis: 106/106 compiled Graphics3D C translation units
  checked with no warning/performance/portability diagnostics;
- platform-policy lint and `git diff --check`: clean.

## 6. Related records

- [Graphics3D Runtime Hardening Program (2026-07)](graphics3d-runtime-hardening-2026-07.md)
- [Graphics3D architecture](graphics3d-architecture.md)
- [Runtime testing policy](testing.md)
- [ADR 0102: Graphics3D runtime boundary](../adr/0102-graphics3d-runtime-boundary-and-contract-manifest.md)
