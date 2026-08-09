# Graphics3D C Runtime Hardening Program

Date: 2026-08-09
Status: complete — 100 of 100 findings implemented and validated
Scope: existing C implementations under `src/runtime/graphics/3d` and the
3D-facing audio implementation under `src/runtime/audio/rt_sound3d*.c`, plus
focused tests and contract documentation. Findings closed by earlier audit
ledgers are baseline and are not counted again.

## Specification

### Summary and objective

Find and correct 100 new correctness, memory-safety, numeric, ownership,
concurrency, bounded-work, and performance defects in the 3D runtime without
weakening cross-platform behavior or VM/native determinism.

### Scope

In scope: implementation hardening of existing APIs, retained-state repair,
failure atomicity, allocation arithmetic, bounded traversal, deterministic
fallbacks, hot-path allocation/work reduction, tests, and behavior
documentation.

Out of scope: new IL operations, grammar or verifier changes, new dependencies,
CI workflow changes, and unrecorded runtime C ABI additions or removals.

### Feature toggle

Not required. These are corrections to unconditional existing contracts;
shipping a second unsafe behavior behind a toggle would make runtime results
configuration-dependent.

### Configuration

None. Existing resource caps and runtime configuration remain authoritative.

### Error handling

No new user-visible error strings are introduced in the first batch. Invalid
numeric state is repaired to the documented bounded fallback. Invalid handles
retain the existing null/status/sentinel behavior. Operations that already
fail silently at the C boundary continue to do so deterministically.

### Tests

- Given extreme finite listener basis vectors, when a listener snapshot is
  built, then direction is preserved and every basis lane is finite.
- Given corrupt active-listener state, when it is published or consumed, then
  position, orientation, velocity, and validity are canonical.
- Given missing calculation input, when valid output pointers are supplied,
  then volume and pan are initialized to zero.
- Given sonic/supersonic approach or sonic listener recession, when Doppler is
  calculated, then the result saturates to the correct `[0.5, 2]` endpoint.
- Given corrupt SoundListener3D/SoundSource3D private state, when any accessor
  validates the object, then repaired values persist in object storage.
- Given extreme/non-finite floating-origin deltas, when a source is rebased,
  then current and synchronization positions remain finite and bounded.
- Given a rotated, nonuniformly scaled listener node at the supported world
  coordinate limit, when bindings synchronize, then forward/up use only the
  linear transform and remain finite, normalized, and translation-independent.

## Completed finding ledger

### Batch 1 — spatial-audio numeric and retained-state integrity

| ID | Class | Finding | Resolution | Evidence |
|---|---|---|---|---|
| G3H-001 | Numeric | Raw squared-length normalization overflowed for extreme finite listener forward vectors and replaced a valid direction with the fallback. | Normalize in max-scaled space without reconstructing the unrepresentable original magnitude. | `test_rt_sound3d_contract` |
| G3H-002 | Numeric | The same overflow discarded extreme finite listener up vectors and caller-authored roll. | Apply scaled normalization uniformly to forward and up. | `test_rt_sound3d_contract` |
| G3H-003 | Correctness | Listener-state construction stored velocity at the much larger coordinate bound instead of the Doppler velocity bound. | Copy velocity through a dedicated `1e6`-bounded helper. | `test_rt_sound3d_contract` |
| G3H-004 | Correctness | Active-listener publication copied non-finite/unbounded position state verbatim. | Rebuild a sanitized snapshot before publication. | `test_rt_sound3d_contract` |
| G3H-005 | Correctness | Active-listener publication trusted a caller-supplied right vector that could be non-finite, non-unit, or inconsistent with forward/up. | Reconstruct the full orthonormal basis from sanitized forward/up. | `test_rt_sound3d_contract` |
| G3H-006 | Correctness | Active-listener publication retained unbounded velocity. | Clamp all published velocity lanes to the Doppler domain. | `test_rt_sound3d_contract` |
| G3H-007 | Correctness | Noncanonical nonzero listener validity flags survived publication. | Canonicalize accepted snapshots to validity `1`. | `test_rt_sound3d_contract` |
| G3H-008 | Correctness | Voice calculation consumed an explicitly invalid listener snapshot rather than selecting the fallback listener. | Treat `valid == 0` like a null listener. | `test_rt_sound3d_contract` |
| G3H-009 | Correctness | A null source position left a valid volume output untouched, exposing caller-stale state. | Initialize volume to zero before input validation. | `test_rt_sound3d_contract` |
| G3H-010 | Correctness | A null source position likewise left pan untouched. | Initialize pan to zero before input validation. | `test_rt_sound3d_contract` |
| G3H-011 | Correctness | Direct voice calculations trusted corrupt basis fields even when the listener claimed validity. | Sanitize the effective listener locally before spatial math. | `test_rt_sound3d_contract` |
| G3H-012 | Numeric | An exactly sonic approaching source made the Doppler denominator zero and incorrectly returned neutral pitch. | Saturate a non-positive/near-zero denominator to maximum upward shift. | `test_rt_sound3d_contract` |
| G3H-013 | Numeric | A supersonic approaching source made the denominator negative and incorrectly clamped to the minimum pitch. | Preserve the physical approach direction and saturate to `2`. | `test_rt_sound3d_contract` |
| G3H-014 | Numeric | Sonic-or-faster listener recession produced a non-positive numerator through generic division/clamping. | Handle the numerator boundary explicitly and saturate to `0.5`. | `test_rt_sound3d_contract` |
| G3H-015 | Correctness | SoundListener3D getters returned repaired position copies while leaving corrupt stored position for later mixer publication. | Persist the sanitized pose during checked access. | `test_rt_sound3d_objects` |
| G3H-016 | Correctness | SoundListener3D getters repaired orientation only in a temporary snapshot. | Persist the rebuilt orthonormal basis. | `test_rt_sound3d_objects` |
| G3H-017 | Correctness | SoundListener3D getters bounded only returned velocity values. | Persist Doppler-bounded velocity. | `test_rt_sound3d_objects` |
| G3H-018 | Correctness | Corrupt listener synchronization coordinates could poison a later derived velocity. | Repair all three retained last-position lanes. | `test_rt_sound3d_objects` |
| G3H-019 | Correctness | A noncanonical listener last-position flag survived and made private state ambiguous. | Canonicalize it to zero/one. | `test_rt_sound3d_objects` |
| G3H-020 | Correctness | A corrupted per-object active flag could claim a second active listener. | Derive the flag from the unique active-listener identity. | `test_rt_sound3d_objects` |
| G3H-021 | Correctness | SoundSource3D getters returned repaired position copies but retained poisonous storage. | Persist coordinate-bounded source position. | `test_rt_sound3d_objects` |
| G3H-022 | Correctness | SoundSource3D velocity repair affected only returned copies. | Persist bounded velocity before any accessor/mixer path. | `test_rt_sound3d_objects` |
| G3H-023 | Correctness | A corrupt cached Doppler factor could survive until composition. | Canonicalize it to `[0.5, 2]` with neutral fallback. | `test_rt_sound3d_objects` |
| G3H-024 | Correctness | Corrupt source synchronization coordinates could poison the next auto-velocity update. | Persist finite bounded last-position lanes. | `test_rt_sound3d_objects` |
| G3H-025 | Correctness | A noncanonical source last-position flag survived. | Canonicalize it to zero/one. | `test_rt_sound3d_objects` |
| G3H-026 | Correctness | Non-finite/negative retained reference distance bypassed setter validation. | Restore the documented positive default and upper bound. | `test_rt_sound3d_objects` |
| G3H-027 | Correctness | Non-finite/negative retained maximum distance bypassed setter validation. | Repair it with the same zero/infinite-range fallback as the setter. | `test_rt_sound3d_objects` |
| G3H-028 | Correctness | Corrupt retained falloff radii could leave maximum distance below reference distance. | Re-establish ordering whenever both radii are positive. | `test_rt_sound3d_objects` |
| G3H-029 | Correctness | Retained source volume could remain outside `[0,100]` after a getter returned a clamp. | Persist the clamped logical volume. | `test_rt_sound3d_objects` |
| G3H-030 | Correctness | Negative corrupt voice IDs remained in object storage until selected liveness calls. | Clear them during checked access. | `test_rt_sound3d_objects` |
| G3H-031 | Correctness | Noncanonical looping flags survived even though getters returned boolean values. | Persist a zero/one flag. | `test_rt_sound3d_objects` |
| G3H-032 | Numeric | Authored/retained pitch accepted extreme finite values outside the mixer's supported range. | Repair and store pitch in `[0.25,4]`; invalid input becomes `1`. | `test_rt_sound3d_objects` |
| G3H-033 | Correctness | The occlusion getter exposed NaN/corrupt retained fractions. | Centralize and persist `[0,1]` repair for getters and setters. | `test_rt_sound3d_objects` |
| G3H-034 | Correctness | The mix-group getter exposed invalid retained identifiers. | Repair invalid IDs to SFX during checked access. | `test_rt_sound3d_objects` |
| G3H-035 | Numeric | Floating-origin rebase accepted non-finite/extreme deltas directly in subtraction. | Sanitize each delta before arithmetic. | `test_rt_sound3d_objects` |
| G3H-036 | Numeric | Rebase subtraction could leave current source position non-finite or beyond the supported world bound. | Clamp every result lane after subtraction. | `test_rt_sound3d_objects` |
| G3H-037 | Numeric | The source synchronization baseline used the same unchecked rebase arithmetic. | Rebase and clamp the retained baseline in lockstep. | `test_rt_sound3d_objects` |
| G3H-038 | Numeric | Authored pitch multiplied by Doppler could overflow or exceed the documented mixer input range. | Preflight the multiplication and clamp the composed rate to `[0.25,4]`. | warnings-as-errors build, focused tests |

### Batch 2 — bound-node direction hot path

| ID | Class | Finding | Resolution | Evidence |
|---|---|---|---|---|
| G3H-039 | Correctness | World direction was derived by subtracting two translated points, so floating-point cancellation at large world positions erased small rotated directions. | Multiply the local direction by only the world matrix's linear 3x3 portion. | `test_rt_sound3d_objects` |
| G3H-040 | Correctness | Each transformed point was independently clamped to the coordinate bound before subtraction, making directions collapse exactly at that bound. | Remove position decoding/clamping from direction calculation. | `test_rt_sound3d_objects` |
| G3H-041 | Performance | Every bound-node forward/up query allocated a Mat4 wrapper and four Vec3 wrappers. | Read the existing raw world-matrix component API into stack storage. | warnings-as-errors build, focused tests |
| G3H-042 | Performance | Direction calculation performed two general 4x4 point transforms plus subtraction when translation was intentionally discarded. | Use one direct 3x3 matrix-vector multiply. | warnings-as-errors build, focused tests |
| G3H-043 | Numeric | A finite extreme linear transform could overflow the matrix-vector multiplication and spuriously select the fallback. | Scale matrix and local vector lanes independently before multiplication. | focused tests, source inspection |
| G3H-044 | Numeric | Normalization reconstructed the original vector magnitude (`max * scaled_length`), which could overflow even when direction was representable. | Normalize entirely in max-scaled space. | focused tests, source inspection |
| G3H-045 | Correctness | Non-finite linear matrix lanes were not rejected until after allocations and transformed-object decoding. | Validate every consumed matrix lane before multiplication. | source inspection, warnings-as-errors build |
| G3H-046 | Safety | A null local-direction pointer was dereferenced for an otherwise valid node. | Treat a missing local direction as unavailable transform input and copy the fallback. | source inspection, warnings-as-errors build |
| G3H-047 | Performance | Five temporary object releases and associated reference-count branches ran for every successful direction query. | Stack-only component math removes all temporary ownership traffic. | source inspection, warnings-as-errors build |

### Batch 3 — binding synchronization and camera pose fidelity

| ID | Class | Finding | Resolution | Evidence |
|---|---|---|---|---|
| G3H-048 | Correctness | A listener getter's `dt=0` binding refresh advanced the velocity baseline, erasing camera motion before the next real frame sample. | Preserve an initialized baseline when elapsed time cannot produce a derivative. | `test_rt_sound3d_objects` |
| G3H-049 | Correctness | Source getters likewise consumed a bound node's position delta without calculating velocity. | Apply the same zero-delta baseline rule to source synchronization. | shared helper, focused tests |
| G3H-050 | Correctness | Camera-bound listeners copied only position/forward, so camera roll never reached the listener up/right basis. | Synchronize the camera's normalized up vector as part of its complete view pose. | `test_rt_sound3d_objects` |
| G3H-051 | Performance | Camera binding allocated a Vec3 wrapper every synchronization just to read three position scalars. | Use the existing allocation-free camera position component API. | warnings-as-errors build, focused tests |
| G3H-052 | Reliability | Failure to allocate the camera position wrapper silently teleported the listener to the origin. | The component query removes that allocation failure; a failed query preserves the prior position. | source inspection, focused tests |
| G3H-053 | Reliability | Failure to allocate/decode the camera forward wrapper installed a degenerate direction rather than preserving a valid orientation. | Explicitly retain the prior sanitized forward on query failure. | source inspection, focused tests |
| G3H-054 | Reliability | The newly complete camera pose needed deterministic behavior if its up-vector wrapper could not be produced. | Preserve the prior sanitized up direction on query failure. | source inspection, focused tests |
| G3H-055 | Correctness | `SoundSource3D.SetVelocity` promised to skip one auto-derived frame but left the baseline armed, so the very next sync overwrote the authored value. | Disarm the baseline; the next bound sync preserves authored velocity and establishes a new sample. | `test_rt_sound3d_objects` |
| G3H-056 | Performance | Scalar position convenience setters allocated and released temporary Vec3 objects for both listener and source. | Clamp/store the three scalar lanes directly on the validated objects. | warnings-as-errors build, focused tests |

### Batch 4 — Game3D audio immersion retained state

| ID | Class | Finding | Resolution | Evidence |
|---|---|---|---|---|
| G3H-057 | Correctness | `SourceCount` claimed to report active sources but counted stopped/finished retained entries. | Prune finished sources before returning the count. | `test_rt_game3d` |
| G3H-058 | Correctness | Changing the Game3D master volume affected only future positional sources. | Push the clamped volume to every tracked source immediately. | `test_rt_game3d` |
| G3H-059 | Correctness | Disabling occlusion left previously blocked sources muffled indefinitely. | Clear occlusion on all tracked sources when the feature is disabled. | `test_rt_game3d` |
| G3H-060 | Correctness | The Game3D reference-distance getter sanitized only its return value. | Persist the positive bounded default during checked access. | `test_rt_game3d` |
| G3H-061 | Correctness | Retained maximum distance remained invalid or below the reference distance after observation. | Persist a bounded, ordered maximum distance. | `test_rt_game3d` |
| G3H-062 | Correctness | The master-volume getter returned a clamp while retaining an out-of-range value. | Persist volume in `[0,100]`. | `test_rt_game3d` |
| G3H-063 | Correctness | Noncanonical camera-follow flags survived checked access. | Canonicalize the retained flag to zero/one. | `test_rt_game3d` |
| G3H-064 | Correctness | Noncanonical reverb-routing flags survived and made routing branches state-dependent on arbitrary bytes. | Canonicalize reverb routing. | `test_rt_game3d` |
| G3H-065 | Correctness | Noncanonical occlusion-enabled flags survived. | Canonicalize the retained occlusion flag. | `test_rt_game3d` |
| G3H-066 | Numeric | Non-finite/negative reverb blend duration could poison the easing step. | Restore the 0.5-second default. | `test_rt_game3d` |
| G3H-067 | Numeric | Retained reverb room size could be non-finite/outside its mixer domain. | Persist a finite `[0,1]` value. | `test_rt_game3d` |
| G3H-068 | Numeric | Retained reverb damping had the same unchecked path. | Persist a finite `[0,1]` value. | `test_rt_game3d` |
| G3H-069 | Numeric | The reverb-wet getter exposed NaN and left it in state. | Persist a finite `[0,1]` wet mix. | `test_rt_game3d` |
| G3H-070 | Numeric | Retained occlusion amount could be non-finite/outside `[0,1]`. | Repair it at every checked audio access. | `test_rt_game3d` |
| G3H-071 | Bounded work | Corrupt occlusion budgets could disable probes or request billions of loop iterations. | Restore the default for nonpositive values and cap at 256. | `test_rt_game3d` |
| G3H-072 | Correctness | A negative retained round-robin cursor survived until selected ticks. | Canonicalize it to zero. | `test_rt_game3d` |
| G3H-073 | Correctness | Corrupt mixer group/effect IDs below the `-1` unset sentinel survived. | Normalize all three lazy IDs to the unset sentinel. | source inspection, warnings-as-errors build |
| G3H-074 | Numeric | Reverb-zone AABB lanes could retain NaN/Inf or unsupported world magnitudes. | Clamp all bounds to finite world coordinates. | `test_rt_game3d` |
| G3H-075 | Correctness | Corrupt reverb-zone bounds could invert min/max and break containment. | Re-sort every axis during checked access. | `test_rt_game3d` |
| G3H-076 | Numeric | Retained zone room/damping/wet parameters bypassed fluent-setter clamps. | Persist all three in `[0,1]`. | `test_rt_game3d` |
| G3H-077 | Bounded work | Corrupt AmbientBed3D zone counts could remain negative or exceed the fixed 16-slot array. | Persist the count in `[0,16]`. | `test_rt_game3d` |
| G3H-078 | Numeric | Ambient-zone AABBs could retain non-finite, unsupported, or inverted bounds. | Clamp and order all live zone axes. | `test_rt_game3d` |
| G3H-079 | Correctness | Ambient zone/default/fade volumes could remain outside the mixer range. | Persist every volume in `[0,100]`. | `test_rt_game3d` |
| G3H-080 | Numeric | Non-finite/negative ambient crossfade duration escaped its getter and tick. | Restore the two-second default. | `test_rt_game3d` |
| G3H-081 | Correctness | A corrupt active-zone index could address outside the live zone table. | Reset out-of-range indices to the uninitialized sentinel. | `test_rt_game3d` |
| G3H-082 | Correctness | Negative retained current/previous ambient voice IDs reached mixer controls. | Clear them during state repair. | `test_rt_game3d` |
| G3H-083 | Numeric | NaN/out-of-range fade progress reached trigonometry and integer conversion. | Persist fade progress in `[0,1]`. | `test_rt_game3d` |
| G3H-084 | Numeric | Negative/non-finite immersion `dt` could reverse or poison reverb/crossfade easing. | Treat it as a zero-time update. | `test_rt_game3d` |
| G3H-085 | Bounded work | A huge immersion `dt` could snap state across an unbounded stall. | Cap it at the Game3D frame-delta maximum. | source inspection, warnings-as-errors build |
| G3H-086 | Numeric | Floating-origin rebase could push reverb and ambient zone bounds beyond the supported coordinate domain. | Sanitize the delta and clamp every shifted bound. | `test_rt_game3d` |

### Batch 5 — voice lifecycle and API failure semantics

| ID | Class | Finding | Resolution | Evidence |
|---|---|---|---|---|
| G3H-087 | Correctness | Low-level spatial updates wrote volume/pan/pitch to finished voice identifiers. | Check liveness and return before mixer writes. | `test_rt_sound3d_contract` |
| G3H-088 | Correctness | A positive reference-distance update override was discarded after one call despite zero meaning “reuse.” | Persist the override in the tracked voice slot. | `test_rt_sound3d_contract` |
| G3H-089 | Correctness | Maximum-distance overrides were likewise one-shot. | Persist the normalized maximum distance. | `test_rt_sound3d_contract` |
| G3H-090 | Correctness | Persisted overrides could recreate a maximum-before-reference interval. | Sanitize and order both radii before storing. | `test_rt_sound3d_contract` |
| G3H-091 | Performance | Persisting an override could require another linear metadata lookup. | Return the matching slot from the existing lookup and update it directly. | source inspection, focused tests |
| G3H-092 | Correctness | A null SoundSource3D position handle silently teleported the source to the origin. | Ignore null/invalid object setters. | `test_rt_sound3d_objects` |
| G3H-093 | Correctness | A null SoundSource3D velocity handle silently erased authored velocity. | Preserve the prior velocity. | `test_rt_sound3d_objects` |
| G3H-094 | Correctness | A null SoundListener3D position handle silently moved the listener to the origin. | Preserve the prior position. | `test_rt_sound3d_objects` |
| G3H-095 | Correctness | A null listener forward handle reset orientation to fallback `-Z`. | Preserve the prior forward direction. | `test_rt_sound3d_objects` |
| G3H-096 | Correctness | A null listener up handle reset authored roll. | Preserve the prior up direction. | `test_rt_sound3d_objects` |
| G3H-097 | Correctness | A null listener velocity handle erased Doppler input. | Preserve the prior velocity. | `test_rt_sound3d_objects` |
| G3H-098 | Correctness | Pitch authored before `Play` was not applied to the newly created voice. | Push composed authored/Doppler pitch immediately after successful playback. | `test_rt_sound3d_objects` |
| G3H-099 | Correctness | Occlusion authored before `Play` was not applied until a later mutation/sync. | Push retained occlusion immediately after successful playback. | source inspection, focused playback test |
| G3H-100 | Reliability | Ambient beds retained finished current/previous voice IDs and could not recover from an initial group/loop-start failure. | Reap both IDs each tick and retry lazy group/loop creation for the selected clip. | `test_rt_game3d` |

## Validation log

- Pre-change `graphics3d` label baseline: 150/150 passed, including the
  120-second soak.
- Pre-change exhaustive Graphics3D cppcheck: 106/106 translation units, no
  warning/performance/portability diagnostics.
- Tests-first red state: both `test_rt_sound3d_contract` and
  `test_rt_sound3d_objects` failed on the new adversarial assertions.
- Incremental warnings-as-errors macOS build after implementation: passed.
- Focused spatial-audio tests after implementation: 3/3 passed.
- Batch-2 tests-first red state: the large-translation node-forward assertions
  failed with fallback `-Z` instead of rotated `+X`.
- Batch-2 focused object test after the allocation-free transform: passed.
- Batch-3 tests-first red state: four assertions exposed consumed motion
  history, ignored camera roll, and prematurely overwritten source velocity.
- Batch-3 focused object test after implementation: passed (79/79 assertions).
- Batch-4 tests-first red state: active-source count, zone repair, and retained
  audio scalar assertions failed; the focused Game3D test passed after repair.
- Batch-5 tests-first red state: all three focused targets exposed lifecycle,
  null-setter, persistence, or new-voice parameter failures.
- Batch-5 focused targets after implementation: 3/3 passed.
- Final clean macOS warnings-as-errors build: passed, including the Zanna
  Studio native build.
- Full prescribed non-slow CTest suite: 1966/1966 passed; the unavailable-audio
  probe was skipped as designed on the audio-enabled build.
- Runtime-surface audit: passed (7868 functions, 532 classes, and 9222 header
  declarations), with all 8 focused surface tests passing.
- Platform-policy lint, host smoke slice, native AArch64 smoke probes, and
  non-interactive install: passed.
- Full post-change `graphics3d` label: 150/150 passed, including the
  120.53-second soak.
- Direct cppcheck warning/performance/portability analysis of every changed C
  translation unit: clean. The convenience `cppcheck-runtime` CMake target is
  not runnable because the repository references a missing
  `cppcheck-runtime.supp`; this does not affect the equivalent direct analysis.
- Final formatting, `git diff --check`, and worktree scope checks: clean.
