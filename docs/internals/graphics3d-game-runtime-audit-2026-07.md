---
status: completed
audience: contributors
last-verified: 2026-07-31
---

# Graphics3D Game Runtime Audit (2026-07)

## Scope and result

This is a focused follow-up audit of the gameplay-facing C runtime in
`src/runtime/graphics/3d`, especially perception and behavior trees,
interaction, footsteps, dialogue/facial animation, target locking, minimaps,
and stock effects. It does not recount findings from the earlier
[Graphics3D runtime hardening](graphics3d-runtime-hardening-2026-07.md),
[backend](graphics3d-backend-audit-2026-07.md), or
[light-baker](graphics3d-lightbaker-audit-2026-07.md) audits.

The review combined line-by-line ownership and arithmetic analysis, fixed-array
and graph-invariant review, hot-loop allocation analysis, targeted cppcheck,
warn-as-error incremental builds, existing Game3D suites, and new white-box
regressions. All 63 distinct findings below are fixed. The product remains
dependency-free, and no opcode, grammar, verifier rule, registered runtime
signature, C ABI declaration, or platform adapter changed; no ADR was required.

Evidence tags used below:

| Tag | Coverage |
|-----|----------|
| `AUD` | `test_rt_game3d_runtime_audit` fixed-count, stale-handle, graph, arithmetic, LoS, minimap, and ownership regressions |
| `DLG` | Extended `test_rt_game3d_dialogue_facial` dialogue ownership/state and facial envelope/blink regressions |
| `TP` | Extended `test_rt_game3d_thirdperson` target-lock death, transition, and numeric regressions |
| `GAME` | Existing `test_rt_game3d` production Game3D and effects coverage |
| `ROB` | Existing `test_rt_graphics3d_robustness` returning-trap and malformed-state coverage |
| `SA` | Targeted cppcheck, warn-as-error compilation, `git diff --check`, and manual ownership review |

## Findings and resolutions

| ID | Area | Class | Finding and implemented resolution | Evidence |
|----|------|-------|------------------------------------|----------|
| GGR-001 | Perception | Lifetime | `Perception3D.New` accepted a destroyed owner. Construction now uses the shared live-entity validator. | `AUD` |
| GGR-002 | Perception | Memory safety | Corrupt negative or oversized track counts reached fixed 16-slot arrays. Every query, scan, compaction, creation, and tick now clamps or repairs the count. | `AUD`, `ROB` |
| GGR-003 | Perception | Memory safety | Heard-event getters and insertion trusted a corrupt count against an eight-slot buffer. Reads and writes now use one bounded count helper. | `AUD`, `ROB` |
| GGR-004 | Perception | Correctness | Negative world entity counts were converted to capacity-sized scans, exposing nonlogical slots. AI world scans now fail closed and only clamp positive oversize counts. | `AUD` |
| GGR-005 | Perception | Correctness | An unbounded eye offset could overflow the sight origin. The setting and combined origin now use the shared coordinate bound. | `AUD`, `SA` |
| GGR-006 | Perception | Bug | The owner's collider could be the first sight-ray hit and hide every target. Sight rays now ignore the owner body and compare against a validated target body. | `AUD` |
| GGR-007 | Perception | Performance | Each in-cone target allocated two Vec3 objects and a hit object every simulation step. Sight uses the allocation-free raw closest-body query. | `AUD`, `SA` |
| GGR-008 | Perception | Lifetime | Tracks and public queries held or matched raw pointers, allowing a freed/reused entity address to alias an old target. Tracks now use zeroing weak references, promote before access, and require the stable entity id. | `AUD` |
| GGR-009 | Perception | Correctness | A finite loudness whose range product overflowed was rejected as inaudible. Overflowed positive reach is now treated as effectively unbounded while invalid distances still fail closed. | `AUD` |
| GGR-010 | Behavior tree | Memory safety | A corrupt negative node count could index before tree storage during append. Node creation now rejects every count outside the fixed budget. | `AUD`, `ROB` |
| GGR-011 | Behavior tree | Correctness | Move speed and arrival distance accepted finite overflow-prone magnitudes. Both are now positive and coordinate-bounded. | `AUD`, `SA` |
| GGR-012 | Behavior tree | Correctness | Custom id zero collided with the public “no pending custom leaf” sentinel. Zero is now rejected before node creation. | `AUD` |
| GGR-013 | Behavior tree | Bug | `AddChild` prevented only direct self-links, so indirect cycles could recurse forever. A bounded iterative reachability check rejects all cycle-closing edges. | `AUD` |
| GGR-014 | Behavior tree | Correctness | Leaf parents, duplicate edges, and multi-child inverters could form invalid executable topology. The builder now enforces node kind, uniqueness, and exactly-one decorator arity. | `AUD` |
| GGR-015 | Behavior tree | Memory safety | Instance construction trusted corrupt node/root metadata. It now requires a live owner and a nonempty bounded tree with an in-range root. | `AUD`, `ROB` |
| GGR-016 | Behavior tree | Correctness | Passing a wrong-class or destroyed target silently cleared the previous target. Only explicit null clears; invalid replacements trap or fail closed without mutation. | `AUD` |
| GGR-017 | Behavior tree | Memory safety | `Resolve` and `PendingCustom` trusted a private pending index and could index outside result arrays or expose contradictory state. They now validate id, node, tree, type, and flag together and clear orphan flags on repair. | `AUD`, `ROB` |
| GGR-018 | Behavior tree | Correctness | A non-finite movement distance was treated as arrival success. It now returns leaf failure. | `AUD`, `SA` |
| GGR-019 | Behavior tree | Memory safety | Composite child counts, root bounds, and recursive node indices were not guarded during execution. Evaluation now fails closed at every fixed-array boundary. | `AUD`, `ROB` |
| GGR-020 | Behavior tree | Lifetime | CanSee and movement leaves could follow a freed or reused raw target pointer. Instances now store a zeroing weak reference, promote it for each evaluation, and still require matching stable perception ids. | `AUD` |
| GGR-021 | Interaction | Lifetime | Interactable and Interactor constructors accepted destroyed owners. Both now use the shared live-entity validator. | `AUD` |
| GGR-022 | Interaction | Correctness | A negative world entity count expanded to a capacity scan. Interaction scans now fail closed for missing or nonpositive logical storage. | `AUD` |
| GGR-023 | Interaction | Performance | Every LoS candidate allocated Vec3 and hit objects in the per-step focus loop. The scanner now uses the raw closest-body query with no managed allocation. | `AUD`, `SA` |
| GGR-024 | Interaction | Bug | The scanner owner's collider could block its own focus ray. Raw LoS queries now ignore the owner body. | `AUD` |
| GGR-025 | Interaction | Lifetime | `GetFocused` and `Interact` trusted retained focus and a raw entity back-reference between ticks, including after disable, destruction, replacement, or entity deallocation. Owner slots are now zeroing weak references, and both APIs revalidate and clear stale focus before acting. | `AUD` |
| GGR-026 | Interaction | Integer safety | Lifetime interaction telemetry could overflow signed `int64_t`. It now saturates at `INT64_MAX`. | `AUD` |
| GGR-027 | Interaction | Correctness | Multiplying a negative current-focus score by 1.10 made it worse, reversing hysteresis. Negative scores now use 0.90 so the current target receives a sign-correct boost. | `AUD` |
| GGR-028 | Interaction | Correctness | The `-1e30` best-score sentinel excluded valid lower priorities. Priorities are bounded and selection starts at `-DBL_MAX`. | `AUD` |
| GGR-029 | Interaction | Correctness | A corrupt/non-finite owner rotation left a non-finite forward vector. Normalization now restores canonical negative Z on failure. | `AUD`, `SA` |
| GGR-030 | Footsteps | Type safety | Surface rows retained arbitrary runtime objects and later sent them to audio playback. `AddClip` now accepts only Sound handles. | `AUD` |
| GGR-031 | Footsteps | Memory/resource safety | Row lookup, modulo selection, getters, append, and finalization trusted the private eight-slot clip count. Operational reads clamp it, append releases a pre-existing underreported slot, and finalization visits every physical slot. | `AUD`, `ROB` |
| GGR-032 | Footsteps | Lifetime | `Footsteps3D.New` accepted a destroyed owner and retained an unsafe raw owner back-reference. It now rejects stale entities and stores the owner in a zeroing weak slot. | `AUD` |
| GGR-033 | Footsteps | Bug/performance | The closest downward hit could be the entity's own collider, producing its surface instead of the ground, while the corrective all-hit path allocated several objects per step. A raw closest-body ray now ignores the owner and reads the ground collider directly without managed allocations. | `AUD`, `SA` |
| GGR-034 | Footsteps | Integer safety | Step telemetry wrapped after `INT64_MAX`. It now saturates. | `AUD`, `SA` |
| GGR-035 | Footsteps | Bug | `VolumeScale` was stored but never applied. Positional voices now receive the scaled master volume, capped to the SoundSource3D range. | `GAME`, `SA` |
| GGR-036 | Dialogue | Resource | A successful localized lookup returned an owned string that was copied and leaked. Resolution now releases the temporary after copying. | `DLG` |
| GGR-037 | Dialogue | Type safety | `SayVoiced` retained arbitrary objects as clips and mutated the queue before failure. It now validates a Sound handle before appending. | `DLG` |
| GGR-038 | Dialogue | Memory/resource safety | Line/choice getters, queueing, ticking, drawing, and finalization trusted corrupt fixed-array counts. Accesses clamp counts, queue overwrite releases an underreported slot, and finalization releases all physical voice slots. | `DLG`, `ROB` |
| GGR-039 | Dialogue | Bug | Showing an empty dialogue wedged the world's active slot, while a choice-only dialogue never activated its choice. Empty conversations remain inactive and choice-only conversations arm immediately. | `DLG` |
| GGR-040 | Dialogue | Correctness | Invalid line indices could leave an active conversation permanently inert. Tick/advance now recover into a valid pending choice or hide the dialogue. | `DLG`, `ROB` |
| GGR-041 | Dialogue | Integer safety | Adding an arbitrary signed choice delta could overflow, and confirmation trusted corrupt selection/count state. Movement now compares before addition; confirmation clamps repaired state. | `DLG` |
| GGR-042 | Dialogue | Undefined behavior | NaN, infinity, negative, or huge reveal counts were cast directly to `size_t` and could wedge ticking. Tick repairs reveal/hold state and speed, while text and overlay paths validate and clamp in floating point before conversion. | `DLG`, `ROB` |
| GGR-043 | Dialogue | Lifetime | A destroyed speaker could remain retained and be read during anchored rendering. Assignment requires a live entity; tick/draw release an anchor that later becomes stale. | `DLG`, `SA` |
| GGR-044 | Facial | Memory/resource safety | Corrupt shape counts reached four-slot arrays in binding, ticking, lookup, and finalization. Reads clamp the count, rebinding releases an underreported slot, and finalization visits every physical interned-name slot. | `DLG`, `ROB` |
| GGR-045 | Facial | Resource | Replacing a metered voice, switching to DriveLevel, or finalizing LipSync left metering enabled on the old voice. Every transition now disables the prior meter exactly once. | `DLG`, `SA` |
| GGR-046 | Facial | Bug | DriveLevel left `Driving` true indefinitely even though ticks targeted zero, so the public state contradicted the releasing envelope. Injected levels are now one-sample impulses and clear on the next tick. | `DLG` |
| GGR-047 | Facial | Correctness | Gaze copied NaN/infinite Vec3 lanes and could overflow an entity eye-height offset into IK state. It now uses the shared bounded Vec3 reader and bounded coordinate addition. | `DLG`, `ROB` |
| GGR-048 | Facial | Correctness | A simulation step spanning the blink midpoint could skip the blink entirely. Crossing steps now sample the triangular envelope's closed-eye peak. | `DLG` |
| GGR-049 | Target lock | Lifetime | Entities latched dead by Health3D remained eligible and locked until entity destruction. Candidate collection, target queries, movement bias, cycling, and maintenance now treat health-dead entities as untargetable. | `TP` |
| GGR-050 | Target lock | Correctness/performance | LoS allocation/raycast failure and non-finite distance could fail open as visible, while every candidate boxed vectors and a hit list. A bounded allocation-free raw non-trigger query now preserves its negative invalid/failure result so target locking fails closed; only finite coincident points are trivially visible. | `AUD`, `TP`, `ROB` |
| GGR-051 | Target lock | Resource | Overlap collection invoked the physics query even when center Vec3 allocation failed. It now returns before the call and preserves cleanup symmetry. | `TP`, `SA` |
| GGR-052 | Target lock | State | Explicit `Clear` left stale acquired/lost one-shot flags observable. It now resets both transition flags and the LoS timer. | `TP` |
| GGR-053 | Target lock | Numeric safety | Squared planar lengths overflowed and unbounded movement lanes could return infinite bias vectors. Inputs are coordinate-bounded and lengths use `hypot`. | `TP` |
| GGR-054 | Minimap | Type safety | Map images accepted arbitrary runtime handles that the canvas later treated as Pixels. SetMapImage now validates Pixels or null before retaining. | `AUD` |
| GGR-055 | Minimap | Numeric safety | Finite but enormous/inverted map bounds could overflow spans and affine math. Bounds are coordinate-limited and require a usable positive span. | `AUD` |
| GGR-056 | Minimap | Lifetime | Destroyed entities could be assigned as tracked/marker entities, and an entity destroyed later remained retained as the compass/objective origin. Assignment requires liveness; draw releases stale tracked and marker refs in place. | `AUD` |
| GGR-057 | Minimap | Undefined behavior | Viewport, world canvas extents, marker scales, and compass values could reach `int64_t` conversions without representable bounds. Stored and derived screen-space values are now repaired, positive where required, and coordinate-limited before conversion. | `AUD`, `ROB` |
| GGR-058 | Minimap | Type safety | Marker icons accepted arbitrary objects before Canvas image drawing. Both marker forms validate optional Pixels handles before mutation. | `AUD` |
| GGR-059 | Minimap | Integer safety | Monotonic marker ids overflowed signed `int64_t` and could collide after wrap. Allocation wraps explicitly, scans active ids, and issues only positive unique values. | `AUD` |
| GGR-060 | Minimap | Correctness | NaN/infinite world or camera projection state propagated into canvas coordinates. Projection sanitizes inputs, heading, and screen results and bounds finite affine output without clamping it to the viewport. | `AUD` |
| GGR-061 | Minimap | State/resource | Removed or stale-dropped markers left parallel ids behind, and a corrupt unused flag could make reuse overwrite retained refs. Removal clears ids, and reuse transactionally clears any residual slot ownership. | `AUD` |
| GGR-062 | Effects | Resource | Explosion, sparks, dust, and smoke returned an unregistered creation reference when registry growth failed. Each preset now releases partial particles and returns null unless registry ownership succeeds. | `GAME`, `SA` |
| GGR-063 | Effects | Resource | Impact decals continued after texture failure and returned orphan decals after registry failure. Texture creation is checked and every partial texture/decal reference is released transactionally. | `GAME`, `SA` |

## Regression and validation contract

The permanent focused target is `test_rt_game3d_runtime_audit`, registered with
the `graphics3d`, `runtime`, and `unit` labels. It deliberately mirrors only
private prefixes needed to inject impossible public states; compile-time layout
use and warn-as-error compilation make drift visible. Existing dialogue/facial
and third-person targets carry the behavioral regressions closest to those
subsystems.

Closeout uses incremental builds only:

```sh
ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 \
  ZANNA_SKIP_LINT=1 ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 \
  ZANNA_SKIP_INSTALL=1 ./scripts/build_zanna_mac.sh

ctest --test-dir build -R \
  'test_rt_game3d_(runtime_audit|dialogue_facial|thirdperson)$' \
  --output-on-failure -j 3

ctest --test-dir build -L graphics3d --output-on-failure -j 10
./scripts/lint_platform_policy.sh
```

Incremental warn-as-error builds passed on macOS arm64. The focused three-test
command passed 3/3 tests, and the final Graphics3D label passed 156/156 tests.
The platform-policy lint, source-header audit, and final diff checks also
completed successfully.
