---
status: completed
audience: contributors
last-verified: 2026-08-02
---

# Graphics3D Game-Core Correctness Audit (2026-08)

## 1. Scope and method

This audit is a line-by-line source review of the C runtime under
`src/runtime/graphics/3d`, concentrating on the previously under-covered
Game3D core, controller, entity-hierarchy, preset, diagnostics, and composed
subsystem boundaries. The July backend audit, August animation/navigation
audit, and August foundational-runtime follow-up were reviewed first; their
findings are not counted again here.

The review combined opaque-handle and returning-trap analysis, retained-reference
and raw-allocation ownership tracing, corrupt-private-state injection, numeric
range/overflow analysis, allocation-failure transaction analysis, controller
pause semantics, hot-path allocation review, concurrent diagnostics stress,
and focused plus label-wide runtime tests. The ledger records **186 newly
corrected, independently observable bugs, correctness hazards, and performance
problems**. Entries remain separate where a different public type, lifecycle
boundary, state transition, or external side effect made the failure
independently observable.

No public function signature, runtime C ABI symbol, IL contract, opcode,
grammar, verifier rule, workflow, or cross-layer dependency changed. New child
and fade allocation identities are appended private payload state. The atomic
helpers are internal platform primitives, and the C++ guard only gives the
existing C symbol its correct language linkage. Therefore no ADR is required
under the repository policy.

## 2. Evidence key

- **CORE**: `test_rt_game3d`, including exact-layout spoofing, returning traps,
  input repair, hierarchy ownership, component quarantine, controllers, and
  transactional preset cases.
- **TP**: `test_rt_game3d_thirdperson`, including third-person/target-lock
  private-state repair, ownership, pause, and cross-world cases.
- **CINE**: `test_rt_game3d_cinematics`, including rail state/key repair and
  zero-time behavior.
- **DIAG**: `test_rt_game3d_diagnostics`, including eight-thread exact-count and
  saturation stress.
- **PATH**: `test_rt_transform_path`, including same-class undersized Path3D
  rejection.
- **SHARED**: `test_stl_load`, `test_rt_canvas3d`, `test_rt_material3d`,
  `test_rt_scene3d`, `test_rt_physics3d`, and
  `test_rt_graphics3d_robustness`.
- **BUILD**: supported macOS build script with `ZANNA_SKIP_CLEAN=1` and the
  warnings-as-errors configuration.
- **PLATFORM**: platform-policy lint and cross-platform smoke script.
- **G3D**: complete CTest `graphics3d` label.

## 3. Corrected issue ledger

### Exact opaque-handle boundaries

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3C-001 | Correctness | LayerMask accepted a matching class ID with an undersized payload and then read past it. | Require the complete `rt_game3d_layermask` payload. | CORE |
| G3C-002 | Correctness | Input3D class-ID spoofs reached snapshot arrays and scalar fields. | Validate the full Input3D layout at every public entry. | CORE |
| G3C-003 | Correctness | Entity3D validation checked only the class tag before lifetime and component reads. | Require the complete Entity3D payload. | CORE |
| G3C-004 | Correctness | Sound3D validation could dereference an undersized same-class object. | Add exact Sound3D payload validation. | CORE |
| G3C-005 | Correctness | EffectRegistry3D accepted an incomplete payload before registry traversal. | Add exact effects-registry validation. | CORE |
| G3C-006 | Correctness | Environment handles trusted class identity without enough storage for their world/entity refs. | Require the full environment-handle layout. | CORE |
| G3C-007 | Correctness | BodyDef access and `AttachBody` could read recipe fields from an undersized spoof. | Require the complete BodyDef payload before dispatch. | CORE |
| G3C-008 | Correctness | Collision3DEvent getters could read beyond a class-only object. | Validate the full event wrapper. | CORE |
| G3C-009 | Correctness | Animator3D wrappers trusted the class tag before controller/state access. | Require the complete animator payload. | CORE |
| G3C-010 | Correctness | ModelTemplate operations accepted an incomplete cache/template payload. | Add exact template validation. | CORE |
| G3C-011 | Correctness | AssetHandle3D operations could read async state from an undersized object. | Add exact asset-handle validation. | CORE |
| G3C-012 | Correctness | WorldStream3D methods trusted a same-class allocation too small for stream metadata. | Require the complete stream payload. | CORE |
| G3C-013 | Correctness | World3D validation could read `destroyed` and subsystem slots past an undersized object. | Require the complete world payload first. | CORE |
| G3C-014 | Correctness | CharacterController3D accepted an incomplete controller layout. | Add exact controller validation. | CORE |
| G3C-015 | Correctness | FirstPersonController getters/setters accepted an undersized class spoof. | Add exact first-person validation. | CORE |
| G3C-016 | Correctness | FreeFlyController entry points accepted an undersized class spoof. | Add exact free-fly validation. | CORE |
| G3C-017 | Correctness | OrbitController accessed target/range fields after class-only validation. | Require the complete orbit payload. | CORE |
| G3C-018 | Correctness | FollowController accessed target/offset fields after class-only validation. | Require the complete follow payload. | CORE |
| G3C-019 | Correctness | ThirdPersonController accessed boom/fade state from undersized same-class storage. | Require the complete third-person payload. | CORE, TP |
| G3C-020 | Correctness | LipSync3D entry points accepted an undersized same-class allocation. | Add exact LipSync3D validation. | CORE |
| G3C-021 | Correctness | Dialogue3D entry points accepted an incomplete private layout. | Add exact dialogue validation. | CORE |
| G3C-022 | Correctness | Timeline3D entry points accepted an incomplete track/state layout. | Add exact timeline validation. | CORE |
| G3C-023 | Correctness | RailCamera3D methods accessed keys and refs after class-only validation. | Require the complete rail payload. | CORE, CINE |
| G3C-024 | Correctness | TargetLock3D methods accepted an undersized lock object. | Require the complete target-lock payload. | CORE, TP |
| G3C-025 | Correctness | Canvas3D public methods accepted an undersized class spoof. | Make the shared canvas validator size-aware while preserving approved stack fixtures. | CORE, SHARED |
| G3C-026 | Correctness | Camera3D methods accepted an incomplete object through the shared validator. | Make camera validation exact while preserving stack fixtures. | CORE, SHARED |
| G3C-027 | Correctness | Scene3D methods trusted only the class ID. | Require the full scene payload outside approved internal struct paths. | CORE, SHARED |
| G3C-028 | Correctness | Mutable and const SceneNode3D paths could traverse undersized storage. | Make both validators exact. | CORE, SHARED |
| G3C-029 | Correctness | Physics World3D entry points accepted an incomplete private world. | Require `sizeof(rt_world3d)` before use. | CORE, SHARED |
| G3C-030 | Correctness | Body3D entry points accepted an incomplete body allocation. | Require `sizeof(rt_body3d)` before field access. | CORE, SHARED |
| G3C-031 | Correctness | Character3D entry points accepted an incomplete character allocation. | Add exact character validation. | CORE, SHARED |
| G3C-032 | Correctness | Trigger3D entry points accepted an incomplete trigger allocation. | Add exact trigger validation. | CORE, SHARED |
| G3C-033 | Correctness | Mesh3D methods accepted an undersized same-class handle. | Make the mesh validator payload-size aware. | CORE, SHARED |
| G3C-034 | Correctness | Material3D methods accepted an undersized same-class handle. | Make the material validator payload-size aware. | CORE, SHARED |
| G3C-035 | Correctness | Light3D methods accepted an undersized same-class handle. | Make the light validator payload-size aware. | CORE, SHARED |
| G3C-036 | Correctness | PostFX3D methods accepted an undersized same-class handle. | Make the post-FX validator payload-size aware. | CORE, SHARED |
| G3C-037 | Correctness | Path3D public evaluators accepted a class-ID spoof too small for its arrays/caches. | Add one exact Path3D validator and use it at every public boundary. | PATH |
| G3C-038 | Correctness | A returning trap handler let live-world getters continue into destroyed world state. | Return `NULL` immediately after the lifetime trap. | CORE |
| G3C-039 | Portability | `rt_obj_class_id` lacked C++ language-linkage guards, forcing ad-hoc declarations and risking mangled references. | Wrap the declaration in `extern "C"` for C++ consumers. | BUILD, SHARED |

### Concurrent diagnostics and atomic portability

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3C-040 | Portability | GCC/Clang builds had no common 64-bit atomic-store helper corresponding to the MSVC implementation. | Add `rt_atomic_store_i64` using the compiler intrinsic. | BUILD, PLATFORM |
| G3C-041 | Portability | GCC/Clang builds had no common 64-bit compare/exchange helper for portable saturating counters. | Add `rt_atomic_compare_exchange_i64`. | BUILD, PLATFORM |
| G3C-042 | Correctness | Concurrent broadphase-fallback producers raced and lost increments. | Use a saturating atomic CAS loop. | DIAG |
| G3C-043 | Correctness | Concurrent CCD clamped-frame producers raced. | Make the frame counter atomic and saturating. | DIAG |
| G3C-044 | Correctness | Concurrent CCD affected-body additions raced and could undercount by large amounts. | Make multi-count additions atomic and saturating. | DIAG |
| G3C-045 | Correctness | Animation workers raced while recording dropped events. | Make the event-drop counter atomic and saturating. | DIAG |
| G3C-046 | Correctness | Navigation workers raced while recording grid fallbacks. | Make the navigation counter atomic. | DIAG |
| G3C-047 | Correctness | Stale-entity diagnostics could race across simulation/API callers. | Make the stale-call counter atomic. | DIAG |
| G3C-048 | Correctness | Async asset completion threads raced while dropping stale loads. | Make the stale-load counter atomic. | DIAG |
| G3C-049 | Correctness | Stream staging-error producers raced. | Make the staging-error counter atomic. | DIAG |
| G3C-050 | Correctness | Stream workers raced while dropping obsolete prepared stages. | Make the stale-stage counter atomic. | DIAG |
| G3C-051 | Correctness | EPA fallback telemetry raced across physics activity. | Make the EPA counter atomic. | DIAG |
| G3C-052 | Correctness | Renderer shadow-slot reuse telemetry raced with reads/reset. | Make the shadow-reuse counter atomic. | DIAG |
| G3C-053 | Correctness | Auto-instancing telemetry raced with reads/reset. | Make the folded-draw counter atomic. | DIAG |
| G3C-054 | Correctness | Spatial-audio eviction telemetry used a non-atomic process global. | Use an atomic saturating counter in the audio bridge. | DIAG |
| G3C-055 | Correctness | `Reset` used `memset` against counters concurrently accessed atomically or by workers. | Reset every field through an atomic store and reset audio atomically. | DIAG |
| G3C-056 | Correctness | `Summary` read diagnostic fields directly, reintroducing data races despite atomic getters. | Snapshot only through public atomic getters. | DIAG |
| G3C-057 | Correctness | A corrupted negative counter remained negative under later increments instead of recovering to a valid count. | Normalize the CAS base to zero before addition. | DIAG |
| G3C-058 | Correctness | The 512-byte summary could not hold all degradation names at `INT64_MAX`. | Increase the bounded buffer to cover the worst-case digest. | DIAG |
| G3C-059 | Correctness | Summary truncation could publish a partial `name=value` line. | Append a line only when the complete line and terminator fit. | DIAG |

### Input snapshots and external side effects

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3C-060 | Correctness | Look-sensitivity getters sanitized only the return value, leaving corrupt state for controller consumers. | Persist the repaired bounded sensitivity. | CORE |
| G3C-061 | Correctness | A noncanonical `has_snapshot` flag selected unpredictable live/snapshot behavior. | Canonicalize it to zero or one. | CORE |
| G3C-062 | Correctness | Corrupt or backend-extreme integer mouse X deltas could overflow later controller conversion. | Clamp snapshot and live X deltas before use. | CORE |
| G3C-063 | Correctness | Corrupt or backend-extreme integer mouse Y deltas had the same overflow path. | Clamp snapshot and live Y deltas. | CORE |
| G3C-064 | Correctness | NaN, infinity, or extreme fractional mouse X poisoned look integration. | Repair and bound fractional X deltas. | CORE |
| G3C-065 | Correctness | NaN, infinity, or extreme fractional mouse Y poisoned look integration. | Repair and bound fractional Y deltas. | CORE |
| G3C-066 | Correctness | Nonfinite or extreme wheel state could poison orbit distance. | Repair and bound wheel snapshots and live reads. | CORE |
| G3C-067 | Correctness | A corrupt bound-pad index could reach backend polling and retain stale axes. | Repair unsupported bindings to `-1` and clear pad state. | CORE |
| G3C-068 | Correctness | Noncanonical pad-connected flags were treated as arbitrary truthy state. | Canonicalize the flag. | CORE |
| G3C-069 | Correctness | Disconnected or unbound pads retained prior-frame stick values. | Centralize complete pad-snapshot clearing. | CORE |
| G3C-070 | Correctness | Corrupt left-stick axes outside `[-1,1]` distorted movement and dead-zone math. | Repair both left axes to finite backend bounds. | CORE |
| G3C-071 | Correctness | Corrupt right-stick axes outside `[-1,1]` distorted look response. | Repair both right axes to finite backend bounds. | CORE |
| G3C-072 | Correctness | Fresh backend pad snapshots accepted out-of-range finite axes without normalization. | Sanitize every axis while capturing the frame. | CORE |
| G3C-073 | Correctness | `sqrt(lx*lx + ly*ly)` could overflow in movement dead-zone calculation. | Use `hypot`. | CORE |
| G3C-074 | Correctness | Right-stick look magnitude used the same overflow-prone expression. | Use `hypot`. | CORE |
| G3C-075 | Correctness | After an invalid Input3D trap returned, keyboard queries fell through to global live state. | Return neutral key state for an invalid receiver. | CORE |
| G3C-076 | Correctness | Invalid Input3D mouse-button queries fell through to global live state. | Return neutral button state. | CORE |
| G3C-077 | Correctness | Invalid Input3D delta/wheel queries exposed unrelated process-wide input. | Return zero vectors/scalars. | CORE |
| G3C-078 | Correctness | `captureMouse` still changed OS cursor state after an invalid-receiver trap returned. | Stop before the global side effect. | CORE |
| G3C-079 | Correctness | `releaseMouse` still changed OS cursor state after an invalid-receiver trap returned. | Stop before the global side effect. | CORE |
| G3C-080 | Correctness | `setRelativeLook` still changed global mouse mode after an invalid trap and forwarded noncanonical flags. | Validate first and pass a canonical flag. | CORE |
| G3C-081 | Correctness | `bindPad` treated every negative integer as an unbind request, hiding bad indices. | Accept exactly `-1` for unbind. | CORE |
| G3C-082 | Correctness | `bindPad` accepted indices beyond the backend controller-slot count. | Trap on unsupported non-negative indices. | CORE |
| G3C-083 | Correctness | A failed pad bind could replace the previous valid selection. | Validate transactionally before mutation. | CORE |
| G3C-084 | Correctness | Rebinding did not immediately clear the old controller's captured axes. | Clear pad snapshot state on every bind/unbind. | CORE |
| G3C-085 | Correctness | CharacterController3D used a separate keyboard-only planar helper, so a bound pad moved cameras but not characters. | Route character drive through the merged Input3D planar axis. | CORE, TP |

### Entity hierarchy, ownership, and transactions

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3C-086 | Ownership | Entity finalization freed the mutable `children` pointer, so corruption could free foreign memory. | Bind owned storage to a private pointer/capacity cookie and free only a match. | CORE |
| G3C-087 | Ownership | An underreported child count made finalization leak retained children. | Traverse the trusted allocation capacity and release owned parent-linked entries. | CORE |
| G3C-088 | Correctness | An oversized child count could drive finalization/traversal past allocation bounds. | Bound traversal by trusted storage capacity. | CORE |
| G3C-089 | Ownership | Child growth could call `realloc` on a corrupt borrowed pointer. | Quarantine untrusted storage without freeing/reallocating it. | CORE |
| G3C-090 | Correctness | Corrupt public child capacity was treated as an allocation bound. | Restore public mirrors from trusted allocation metadata. | CORE |
| G3C-091 | Correctness | An underreported public count hid a still-dense child prefix from spawn and later insertion. | Recover the dense prefix from trusted slots. | CORE |
| G3C-092 | Correctness | Newly grown child slots were not explicitly cleared, undermining dense-prefix recovery. | Zero every new slot after successful growth. | CORE |
| G3C-093 | Correctness | A malformed same-class child slot could be dereferenced during count recovery. | Require a complete Entity3D payload for each recovered slot. | CORE |
| G3C-094 | Correctness | An invalid `parent` pointer was followed during detach/ancestry checks. | Validate the complete parent and clear bad links fail-closed. | CORE |
| G3C-095 | Correctness | A parent cycle could evade the old depth-limited ancestry check and permit another cycle. | Add tortoise/hare cycle detection and reject conservatively. | CORE |
| G3C-096 | Correctness | An implausibly deep parent chain was reported as safe once the fixed depth budget expired. | Treat budget exhaustion as corrupt/cyclic. | CORE |
| G3C-097 | Correctness | Recursive subtree setters could loop forever on cyclic scene graphs. | Preflight through a duplicate-detecting pointer set. | CORE |
| G3C-098 | Performance | Shared/DAG scene nodes could be visited and assigned repeatedly. | Collect every node once with an open-addressed seen set. | CORE |
| G3C-099 | Correctness | Recursive setters accepted malformed child handles returned by a corrupt graph. | Fail the preflight on any incomplete SceneNode3D. | CORE |
| G3C-100 | Correctness | Mesh-recursive stack allocation failure left an arbitrary prefix mutated. | Complete all allocation/traversal preflight before applying changes. | CORE |
| G3C-101 | Correctness | Material-recursive allocation failure likewise left a partial subtree update. | Use the same transactional collection/apply split. | CORE |
| G3C-102 | Correctness | The entity's retained mesh slot could be changed even when recursive propagation failed. | Commit the retained slot only after successful traversal/apply. | CORE |
| G3C-103 | Correctness | The retained material slot could diverge from the subtree after failed propagation. | Commit it only after successful apply. | CORE |
| G3C-104 | Correctness | Scene-node parenting failure occurred after the child was detached from its old Entity3D parent, orphaning it. | Preflight/grow and establish scene parenting before committing entity ownership. | CORE |
| G3C-105 | Correctness | A failed spawned-tree handoff did not restore the child's prior Entity3D parent/index. | Save and transactionally restore the prior relationship. | CORE |
| G3C-106 | Correctness | The same failed handoff did not restore the previous SceneNode3D parent. | Restore the prior scene parent during rollback. | CORE |
| G3C-107 | Ownership | Reparent rollback could leak or double-release the retain transferred between parent arrays. | Explicitly transfer the retain back or release it exactly once. | CORE |
| G3C-108 | Correctness | A spawned destination with a corrupt world pointer could be dereferenced by `AddChild`. | Validate spawned parent ownership before mutation. | CORE |
| G3C-109 | Correctness | A spawned child with a corrupt world pointer could reach cross-world comparison/mutation. | Validate the child's world before reparenting. | CORE |
| G3C-110 | Correctness | `AttachBody` cast a spawned entity's world without exact validation. | Reject the operation and release any newly built body transactionally. | CORE |
| G3C-111 | Correctness | `Entity3D.Of` accepted an undersized Mesh3D and returned a partial entity after downstream traps. | Validate the complete mesh before entity allocation. | CORE |
| G3C-112 | Correctness | `Entity3D.Of` accepted an undersized Material3D with the same partial-construction behavior. | Validate the complete material before allocation. | CORE |
| G3C-113 | Ownership | Failure to allocate the default entity name returned a partially initialized object. | Tear down the entity and trap on name allocation failure. | CORE |
| G3C-114 | Ownership | Finalization released corrupt name/persistence slots as runtime strings. | Release only valid runtime-string handles and clear corruption unowned. | CORE |
| G3C-115 | Correctness | Name getters returned a temporary empty string but left the corrupt retained slot and name index state behind. | Persist a retained empty name and invalidate the world name index. | CORE |
| G3C-116 | Correctness | `SetName` accepted a non-string handle and could corrupt ownership. | Require a valid runtime string before retaining. | CORE |
| G3C-117 | Correctness | Name-index invalidation cast a spawned entity's world without exact validation. | Invalidate only through a complete World3D. | CORE |
| G3C-118 | Correctness | Layer getters repaired only their return value, leaving the entity and attached body on different filters. | Persist the default layer and update the body filter. | CORE |
| G3C-119 | Correctness | Noncanonical `spawned` flags leaked through public predicates and branching. | Canonicalize stored spawned state. | CORE |
| G3C-120 | Correctness | Noncanonical `destroyed` flags leaked through public predicates and branching. | Canonicalize stored destroyed state. | CORE |
| G3C-121 | Correctness | Euler rotation forwarded a null quaternion after allocation failure. | Mutate the node only when quaternion construction succeeds. | CORE |
| G3C-122 | Correctness | Entity finalization cast an undersized LipSync3D slot before clearing its back-reference. | Require the complete lip-sync payload. | CORE |
| G3C-123 | Correctness | World simulation sweeps dereferenced undersized entity slots in the registry. | Exact-check every registry entry before stamping/ticking. | CORE |
| G3C-124 | Correctness | Interpolation capture/apply/restore dereferenced malformed entity/node slots. | Exact-check entities and SceneNode3D payloads at each phase. | CORE |
| G3C-125 | Correctness | Rebase, ragdoll, debug, and streamed-terrain loops trusted class-only world-owned entries. | Exact-check private Game3D payloads before direct access. | CORE |

### Controllers, target lock, and fade ownership

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3C-126 | Ownership | Controller finalizers released private Game3D refs by class alone, so undersized corruption could be released as owned. | Add complete-payload repair/release/assignment helpers. | CORE, TP |
| G3C-127 | Correctness | Character controller speed, jump, gravity, and eye-height corruption reached integration. | Persist finite bounded defaults before every operation. | CORE |
| G3C-128 | Correctness | Corrupt capsule radius/heights, crouch/platform flags, and vertical velocity produced invalid movement state. | Repair the full character-controller invariant set. | CORE, TP |
| G3C-129 | Correctness | First-person speed/look/capture state could remain nonfinite or noncanonical. | Centralize first-person state repair. | CORE |
| G3C-130 | Correctness | Free-fly speed/look/capture state could poison camera integration. | Centralize free-fly state repair. | CORE |
| G3C-131 | Correctness | Orbit distance ranges, yaw, pitch, damping, and capture flags could become inconsistent. | Repair, order, clamp, and persist orbit state. | CORE |
| G3C-132 | Correctness | Follow damping and offset slots could remain corrupt. | Repair the scalar and typed Vec3 slot before use. | CORE |
| G3C-133 | Correctness | Third-person boom/aim/FOV/range/flag state could remain NaN, inverted, or noncanonical. | Repair all retained numeric and flag invariants at validation. | TP |
| G3C-134 | Correctness | Target-lock distances, cone, stickiness, grace timers, and transition flags could remain corrupt. | Repair all target-lock invariants at validation. | TP |
| G3C-135 | Correctness | A zero/nonfinite CharacterController3D delta became a default positive frame step. | Preserve it as a paused no-op. | CORE |
| G3C-136 | Correctness | First-person zero/nonfinite updates still rotated or moved the camera. | Return before input and movement. | CORE |
| G3C-137 | Correctness | Free-fly zero/nonfinite updates still integrated camera motion. | Return before capture/input/camera integration. | CORE |
| G3C-138 | Correctness | Orbit zero/nonfinite updates still consumed mouse/wheel state. | Treat them as paused. | CORE |
| G3C-139 | Correctness | Follow zero/nonfinite late updates still advanced exponential damping. | Preserve camera state on pause. | CORE |
| G3C-140 | Correctness | Third-person zero/nonfinite updates advanced lock, aim, boom, and fade state. | Return before all controller state transitions. | TP |
| G3C-141 | Correctness | TargetLock3D zero/nonfinite updates cleared one-shot transition flags and advanced LOS time. | Preserve transition/timer state on pause. | TP |
| G3C-142 | Correctness | Corrupt controller world slots could identify and detach an unrelated world during rebinding. | Exact-repair the world slot before detach/bind. | CORE |
| G3C-143 | Correctness | First-person character assignment accepted an undersized class spoof and replaced a valid binding. | Validate the complete nested controller transactionally. | CORE |
| G3C-144 | Correctness | Third-person character assignment had the same incomplete and nontransactional path. | Exact-check and world-check before assignment. | TP |
| G3C-145 | Ownership | Dead orbit targets remained retained indefinitely. | Release a valid dead entity target when resolving it. | CORE |
| G3C-146 | Ownership | Dead follow targets likewise remained retained. | Release the stale retained target and return neutral. | CORE |
| G3C-147 | Performance | Setting a child node's world position allocated world/inverse matrices and temporary vectors per sync. | Invert the parent's affine 3x3+translation directly in scalar storage. | CORE |
| G3C-148 | Performance | World-rotation sync allocated parent/world/local quaternion objects. | Read normalized components and compose the local quaternion allocation-free. | CORE |
| G3C-149 | Performance | Body synchronization allocated world-position and world-scale vectors every update. | Use component getters and allocate only the orientation object required by the body API. | CORE |
| G3C-150 | Correctness | Nonfinite or singular parent transforms reached inverse conversion and could publish invalid local positions. | Validate every matrix lane and determinant before mutation. | CORE |
| G3C-151 | Correctness | Quaternion length used overflow-prone sum-of-squares normalization. | Use nested `hypot` and reject degenerate results. | CORE |
| G3C-152 | Correctness | Third-person controllers accepted a TargetLock3D owned by another world. | Compare complete retained worlds and reject transactionally. | TP |
| G3C-153 | Ownership | Third-person fade reset/finalization freed the mutable fade pointer even when it was foreign. | Bind fade allocations to a private pointer/capacity cookie and free only owned storage. | TP |
| G3C-154 | Correctness | Corrupt fade count/capacity metadata could drive entry traversal out of bounds. | Restore or quarantine metadata against trusted storage capacity. | TP |
| G3C-155 | Ownership | Fade entries dereferenced/released undersized node and material objects. | Exact-check node/original/clone slots and clear corruption unowned. | TP |
| G3C-156 | Correctness | Target-lock distance used overflowing squared-distance arithmetic. | Use nested `hypot`. | TP |

### Rail cameras, presets, and world composition

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3C-157 | Correctness | Rail progress, smoothing, speed, damping, and key-ease state could remain nonfinite/noncanonical. | Repair and persist every scalar before access. | CINE |
| G3C-158 | Correctness | Negative or oversized rail key counts caused missing keys or fixed-array overreads. | Clamp counts to `0..16` before traversal. | CINE |
| G3C-159 | Correctness | Nonfinite/out-of-range rail key times and values poisoned interpolation/camera projection. | Repair times, FOV, and roll to documented bounds. | CINE |
| G3C-160 | Correctness | Unsorted corrupt rail keys produced wrong segments during evaluation. | Stable-sort repaired keys by time. | CINE |
| G3C-161 | Correctness | Duplicate key times wasted the 16-key budget and made interpolation ambiguous. | Coalesce corruption and make new duplicate inserts replace the last value. | CINE |
| G3C-162 | Correctness | Unused rail key slots retained corrupt data that could reappear after count corruption. | Clear the unused suffix after repair. | CINE |
| G3C-163 | Correctness | Rail key evaluation trusted null outputs, excessive counts, noncanonical easing, and unbounded `t`. | Defensively validate and normalize evaluator inputs. | CINE |
| G3C-164 | Correctness | Rail progress addition could overflow before its final clamp. | Sanitize the computed sum and clamp it transactionally. | CINE |
| G3C-165 | Ownership | Corrupt rail look-mode refs could leave multiple retained modes active or release wrong-kind slots. | Repair typed refs and enforce entity > point > path exclusivity. | CINE |
| G3C-166 | Correctness | Rail entity look targets could belong to another world. | Validate controller-world ownership before assignment. | CINE |
| G3C-167 | Correctness | Rail view-vector normalization overflowed for large finite coordinates. | Use scaled Euclidean normalization. | CINE |
| G3C-168 | Correctness | Rail late update used class-only camera/path/entity handles. | Use exact camera/entity boundaries and the exact public Path3D evaluator. | CINE, PATH |
| G3C-169 | Correctness | Lighting/quality helpers cast a world canvas after only a non-null test. | Route all helpers through the exact canvas validator. | CORE |
| G3C-170 | Correctness | Studio lighting cleared the working rig before all replacement allocations succeeded. | Construct key/fill directions and lights first, then commit. | CORE |
| G3C-171 | Correctness | Outdoor lighting cleared the rig before sun construction succeeded. | Construct and validate the sun first. | CORE |
| G3C-172 | Correctness | Night lighting published a partial moon/lamp rig on allocation failure. | Allocate the full pair before replacing state. | CORE |
| G3C-173 | Correctness | Interior lighting likewise published a partial point-light pair. | Make the preset transactional. | CORE |
| G3C-174 | Correctness | Outdoor direction normalization used overflow-prone squared length. | Use nested `hypot`. | CORE |
| G3C-175 | Correctness | `Materials.FromAlbedoMap` accepted malformed same-class Pixels storage. | Require a complete usable Pixels implementation first. | CORE |
| G3C-176 | Correctness | `PostFX.None` cleared the current chain when replacement allocation failed. | Return without mutation on OOM. | CORE |
| G3C-177 | Correctness | Quality probing could pass a null capability string after allocation failure. | Check capability-string construction before backend calls. | CORE |
| G3C-178 | Correctness | Low segment requests could select an oversized fallback, bypassing the stated 256 cap. | Clamp both request and fallback to `8..256`. | CORE |
| G3C-179 | Performance | Prefabs allocated meshes before rejecting an invalid material. | Exact-validate the optional material before mesh construction. | CORE |
| G3C-180 | Ownership | Prefab wrapping did not fail/clean up transactionally for malformed generated meshes or default-material OOM. | Validate generated objects and release the owned mesh on every failure. | CORE |
| G3C-181 | Correctness | World component getters returned undersized same-class canvas/camera/scene/input/audio/effects handles. | Validate each complete component before exposing it. | CORE |
| G3C-182 | Ownership | World teardown released corrupt subsystem slots as generically owned objects. | Use class-specific retained-slot cleanup for stream/effects/audio/input/physics/scene/camera/canvas. | CORE |
| G3C-183 | Correctness | Stream getter/telemetry accepted undersized stream storage and could read its counters. | Quarantine/replace incomplete streams and exact-check telemetry. | CORE |
| G3C-184 | Correctness | Rebase, interpolation, debug, and hitch paths cast private stream/effects/audio entries class-only. | Exact-check every directly accessed private payload. | CORE |
| G3C-185 | Correctness | Canvas light assignment and counting accepted an undersized Light3D. | Require the full light payload before retain/read. | CORE, SHARED |
| G3C-186 | Correctness | Post-FX and quality assignment cast an incomplete effects registry and could publish invalid chains. | Exact-check effects/canvas and use typed transactional post-FX assignment. | CORE |

## 4. Public behavior and verification

The public Game3D guide now documents exact opaque-handle failure behavior,
atomic/saturating diagnostics, complete summary lines, strict gamepad binding,
snapshot repair, bound-pad character motion, controller pause semantics,
transactional/cross-world controller setters, third-person fade ownership, rail
key limits and replacement, segment bounds, and transactional preset setup.

All builds for this audit were incremental (`ZANNA_SKIP_CLEAN=1`) because other
work was active in the same repository. Verification used the supported build
script, focused regression binaries, the complete `graphics3d` label,
platform-policy checks, cross-platform smoke coverage, and static analysis of
the compiled Graphics3D C translation units. No dependency was downloaded or
introduced.
