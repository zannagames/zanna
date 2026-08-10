---
status: complete
audience: contributors
last-verified: 2026-08-09
---

# Graphics3D Runtime Hardening Program, Round Two (2026-08-09)

## 1. Summary

This is the traceability record for a second, non-duplicative review of the C
runtime under `src/runtime/graphics/3d`. It follows the earlier July/August
2026 audit ledgers and the first 2026-08-09 hardening tranche. The 105 entries
below cover newly reviewed World3D entity/animation storage, entity-graph
transactions, Timeline3D retained tracks, Canvas3D GPU readback staging, and
BC6H decoder boundaries.

The changes preserve public signatures and successful-call behavior. They add
no dependency, platform branch, IL surface, runtime C ABI entry point, or
workflow change, so no ADR is required for this tranche.

## 2. Hardening contract

- Private pointer/count/capacity mirrors never prove ownership. Address-bound
  allocation metadata authorizes `realloc` and `free`; damaged tuples are
  detached without touching untrusted pointers.
- Entity-tree spawn validates the complete graph and its future stable IDs
  before mutating a world. Operational failures roll back before trapping.
- Timeline tracks are bounded, fully initialized, kind-checked, and validated
  before playback or retained-object lifetime operations.
- Screenshot readback is limited to supported render-target dimensions and a
  bounded canvas-owned staging allocation.
- Compressed texture decode fails closed on invalid pointers, layout bounds,
  size arithmetic, or mip counts.

## 3. Corrected issue ledger

Evidence keys:

- `WORLD`: `test_rt_game3d` entity-tree, stable-ID, storage-repair, and animation-scratch cases.
- `TL`: `test_rt_game3d_cinematics` timeline corruption and retained-union cases.
- `CANVAS`: `test_rt_canvas3d_gpu_paths` readback ownership and dimension cases.
- `BC6H`: `test_rt_model3d` BC6H mode and invalid-pointer cases.
- `STATIC`: direct invariant inspection and warning-enabled static analysis.
- `BUILD`: repository build with warnings treated as errors.
- `G3D`: complete `graphics3d` CTest label.
- `PLATFORM`: repository platform-policy lint.

| ID | Area | Class | Finding and resolution | Evidence |
|---|---|---|---|---|
| G3H2-001 | World registry | Ownership | The entity pointer and mutable capacity mirror alone authorized resizing. Bind the allocation to an authoritative capacity and address-derived cookie. | WORLD |
| G3H2-002 | World registry | Memory safety | Corrupt entity pointer/capacity mirrors could be accepted as an allocation. Restore mirrors only from the authoritative tuple. | WORLD |
| G3H2-003 | World registry | Correctness | A damaged count mirror discarded an otherwise valid registry or risked an overrun. Recover only the validated dense `Entity3D` prefix. | WORLD |
| G3H2-004 | World registry | Determinism | Grown entity-array tail slots were uninitialized, preventing safe prefix recovery. Zero every newly allocated slot before publication. | WORLD |
| G3H2-005 | World registry | Resource | Entity storage and graph operations had no common logical ceiling. Enforce one million entity nodes independently of allocator limits. | WORLD |
| G3H2-006 | World sweep | Bounds | Tick callbacks can mutate the registry while it is swept. Recompute the authoritative safe count on each iteration. | WORLD |
| G3H2-007 | World animation | Type safety | Animation collection trusted registry slots as `Entity3D`. Validate every slot before reading its animator. | WORLD |
| G3H2-008 | World finalizer | Ownership | Finalization freed the entity mirror even when corruption replaced it with borrowed memory. Free only a cookie-valid owned allocation. | WORLD |
| G3H2-009 | Animator scratch | Ownership | Reusable animator-list storage lacked independent ownership metadata. Add authoritative capacity and an address-bound cookie. | WORLD |
| G3H2-010 | Animator scratch | Memory safety | Pointer/capacity disagreement could suppress allocation or pass a borrowed pointer to `realloc`. Detach invalid tuples and repair valid mirrors. | WORLD |
| G3H2-011 | Animator scratch | Resource | Animator scratch growth accepted unsupported counts. Apply the entity-node ceiling and checked geometric growth. | WORLD |
| G3H2-012 | Animator scratch | Determinism | Grown animator scratch exposed uninitialized tail slots. Zero the new tail. | WORLD |
| G3H2-013 | Animator dedup | Ownership | The open-addressed seen table lacked authoritative ownership metadata. Bind pointer and capacity with a dedicated cookie. | WORLD |
| G3H2-014 | Animator dedup | Memory safety | Corrupt seen-table mirrors could authorize `realloc` of unowned memory. Repair or detach before use. | WORLD |
| G3H2-015 | Animator dedup | Correctness | Open addressing silently assumed a power-of-two capacity. Reject non-power-of-two requests. | WORLD |
| G3H2-016 | Animator dedup | Resource | Dedup capacity could grow without a runtime policy bound. Cap it at the smallest power-of-two ceiling covering twice the entity limit, with checked byte arithmetic. | WORLD |
| G3H2-017 | Animation jobs | Ownership | Reusable job descriptors lacked authoritative allocation identity. Add capacity and a dedicated address-bound cookie. | WORLD |
| G3H2-018 | Animation jobs | Memory safety | Corrupt job pointer/capacity mirrors could suppress allocation or resize borrowed storage. Repair or detach before growth. | WORLD |
| G3H2-019 | Animation jobs | Resource | Job-array growth trusted arbitrary requested counts. Bound it by the entity ceiling and checked capacity math. | WORLD |
| G3H2-020 | World finalizer | Ownership | Animator, seen-set, and job mirrors were unconditionally freed. Release each scratch allocation only when its independent ownership tuple is valid. | WORLD |
| G3H2-021 | Animation worker | Bounds | A worker descriptor carried only start/end indexes and could walk beyond its borrowed animator array. Carry and validate the authoritative element count. | WORLD |
| G3H2-022 | Animation worker | Numeric | A non-finite worker delta reached every animator update. Reject it at the job boundary. | WORLD |
| G3H2-023 | Animation worker | Type safety | Worker slots were dispatched without checking `Animator3D` class identity. Validate before update. | WORLD |
| G3H2-024 | Serial animation | Bounds | The serial fallback trusted corrupt count and delta values. Enforce the node ceiling and finite delta. | WORLD |
| G3H2-025 | Serial animation | Type safety | The serial fallback also dispatched wrong-class slots. Apply the same animator validation as worker jobs. | WORLD |
| G3H2-026 | Job partitioning | Correctness | An empty animator list incorrectly produced one job. Return zero while preserving overflow-safe worker scaling. | WORLD |
| G3H2-027 | Entity traversal | Resource | Tree-stack growth had no independent node ceiling. Refuse pushes beyond the common graph bound. | WORLD |
| G3H2-028 | Spawn rollback | Resource | The rollback journal could grow beyond the supported graph. Bound journal growth by the node ceiling. | WORLD |
| G3H2-029 | Entity traversal | Resource | Pointer visitation storage could grow indefinitely on malformed graphs. Bound its open-addressed table and insert count. | WORLD |
| G3H2-030 | Spawn traversal | Correctness | Cyclic or shared child graphs could spawn a node repeatedly. Deduplicate the complete traversal by entity address. | WORLD |
| G3H2-031 | Despawn traversal | Correctness | Despawn had the same cycle/shared-child exposure. Deduplicate before scheduling descendants. | WORLD |
| G3H2-032 | Entity traversal | Type safety | Child slots were dereferenced as `Entity3D` without full-graph validation. Reject wrong-class children. | WORLD |
| G3H2-033 | Stable IDs | Validation | Negative explicit entity IDs reached registration. Require zero for unassigned or a positive stable ID. | WORLD |
| G3H2-034 | Stable IDs | Overflow | Invalid, nonpositive, or exhausted next-ID counters could wrap assignment. Reject the complete spawn before mutation. | WORLD |
| G3H2-035 | Stable IDs | Correctness | A corrupt existing registry could contain duplicate or nonpositive IDs. Validate it before accepting new entities. | WORLD |
| G3H2-036 | Stable IDs | Correctness | Two explicit IDs inside one planned graph could collide after earlier nodes were already registered. Detect planned duplicates during preflight. | WORLD |
| G3H2-037 | Stable IDs | Correctness | Automatically assigned IDs could collide with existing or planned explicit IDs. Reserve the complete future range in the same set. | WORLD |
| G3H2-038 | Spawn transaction | Correctness | Validation and registry-capacity failures could occur after partial publication. Preflight the complete graph, ownership state, IDs, and final count first. | WORLD |
| G3H2-039 | Spawn rollback | Correctness | A failed transaction left IDs assigned during the attempt and could trap before rollback. Journal assignments, reset them, and defer the operational trap until cleanup completes. | WORLD |
| G3H2-040 | Stable IDs | Performance | Duplicate detection scanned the world once per spawned entity, making large graph spawn quadratic. Build linear open-addressed ID sets in two passes. | WORLD, STATIC |
| G3H2-041 | Timeline storage | Ownership | The mutable track pointer alone represented ownership. Add an authoritative owned pointer. | TL |
| G3H2-042 | Timeline storage | Ownership | Track count was both public state and initialized-slot authority. Track initialized storage count separately. | TL |
| G3H2-043 | Timeline storage | Ownership | Track capacity lacked allocation identity. Add authoritative capacity and an address-derived cookie. | TL |
| G3H2-044 | Timeline storage | Resource | Timeline growth was bounded only by signed allocation math. Enforce 65,536 tracks. | TL |
| G3H2-045 | Timeline storage | Determinism | Grown track storage left its tail uninitialized. Zero new slots before any append. | TL |
| G3H2-046 | Timeline repair | Memory safety | A corrupted track pointer mirror could be dereferenced. Restore it from the valid owned tuple. | TL |
| G3H2-047 | Timeline repair | Bounds | A corrupted track-count mirror could overrun initialized storage. Restore it from the authoritative initialized count. | TL |
| G3H2-048 | Timeline repair | Bounds | A corrupted capacity mirror could suppress growth. Restore it from authoritative capacity. | TL |
| G3H2-049 | Timeline finalizer | Ownership | Finalization unconditionally freed the track mirror. Free only the cookie-valid owned allocation. | TL |
| G3H2-050 | Timeline finalizer | Ownership | Corrupt live count could omit retained references in initialized tail slots. Release every initialized authoritative slot. | TL |
| G3H2-051 | Timeline finalizer | Type safety | A corrupt kind could reinterpret a borrowed union pointer and release it as another payload type. Release retained union members only for a matching validated kind. | TL |
| G3H2-052 | Timeline finalizer | Type safety | The retained world pointer was released without class validation. Release only a valid `GameWorld3D`. | TL |
| G3H2-053 | Timeline tracks | Validation | Arbitrary track-kind values reached union dispatch. Reject kinds outside the closed enum. | TL |
| G3H2-054 | Timeline tracks | Numeric | NaN or infinity track times defeated comparison and scheduling logic. Require finite time. | TL |
| G3H2-055 | Timeline tracks | Correctness | Retained track times could be out of order. Validate monotonic ordering before playback. | TL |
| G3H2-056 | Timeline tracks | Resource | Extremely large finite times could create unbounded timeline duration. Apply the 24-hour runtime ceiling. | TL |
| G3H2-057 | Timeline tracks | Correctness | Noncanonical fired flags changed one-shot event behavior. Accept only zero or one. | TL |
| G3H2-058 | Camera tracks | Validation | Corrupt easing selectors reached interpolation dispatch. Validate the supported easing range. | TL |
| G3H2-059 | Audio tracks | Validation | Noncanonical positional flags changed retained union interpretation. Accept only zero or one. | TL |
| G3H2-060 | Camera tracks | Numeric | Non-finite or extreme camera position/look vectors reached interpolation and camera publication. Require bounded finite lanes. | TL |
| G3H2-061 | Camera tracks | Numeric | Invalid FOV values reached camera updates. Enforce the supported finite FOV interval. | TL |
| G3H2-062 | Camera tracks | Numeric | Invalid crossfade durations reached blend division. Require a finite value within the timeline range. | TL |
| G3H2-063 | Audio tracks | Type safety | `AddAudio` retained arbitrary object handles as `Sound`. Validate the class before mutating the timeline. | TL |
| G3H2-064 | Text tracks | Memory safety | Retained fixed text could lack a terminator. Require one within the track buffer. | TL |
| G3H2-065 | Letterbox tracks | Numeric | Letterbox amounts outside `[0,1]` reached overlay state. Validate the finite normalized range. | TL |
| G3H2-066 | Fade tracks | Numeric | Fade alpha outside `[0,1]` reached overlay state. Validate the finite normalized range. | TL |
| G3H2-067 | Retained tracks | Type safety | Sound/entity union members were trusted based only on kind. Validate each retained payload class before use or release. | TL |
| G3H2-068 | Look-at tracks | Type safety | Subclasses or unrelated handles could be treated as an entity payload. Require the exact `Entity3D` class. | TL |
| G3H2-069 | Look-at tracks | Lifetime | A destroyed entity remained eligible as a camera target. Reject destroyed targets before playback. | TL |
| G3H2-070 | Timeline text | Validation | Names/subtitles accepted malformed UTF-8. Enforce strict runtime UTF-8 validation. | TL |
| G3H2-071 | Timeline text | Correctness | Embedded NUL bytes made distinct runtime strings alias through fixed C text. Reject them. | TL |
| G3H2-072 | Timeline text | Validation | Byte truncation could split a multibyte code point. Snapshot only at a valid UTF-8 boundary. | TL |
| G3H2-073 | Timeline append | Transaction | Text and retained payload validation occurred after mutation in some append paths. Complete validation before growing or publishing a track. | TL |
| G3H2-074 | Timeline repair | Correctness | Corrupt cached duration could end or extend playback incorrectly. Derive it from validated tracks. | TL |
| G3H2-075 | Timeline repair | Numeric | Corrupt playback time could remain non-finite or outside duration. Repair it to the validated interval. | TL |
| G3H2-076 | Timeline repair | Correctness | Playing/skippable mirrors accepted noncanonical Boolean bytes. Canonicalize them during repair. | TL |
| G3H2-077 | Timeline repair | Bounds | Corrupt marker count could expose stale marker slots. Clamp it to the fixed per-step buffer. | TL |
| G3H2-078 | Timeline repair | Memory safety | Corrupt active subtitle could lack a terminator before overlay `strlen`. Force termination during repair. | TL |
| G3H2-079 | Timeline repair | Numeric | Cached letterbox/fade overlays could be non-finite or out of range. Reset them to safe normalized values. | TL |
| G3H2-080 | Timeline repair | Ownership | Cached camera ownership flags could disagree with actual camera tracks. Derive ownership from validated storage. | TL |
| G3H2-081 | Timeline repair | Correctness | The sorted flag could disagree with track order. Recompute it instead of trusting the mirror. | TL |
| G3H2-082 | Timeline playback | Memory safety | Playback/reset/skip/camera paths consumed tracks without a common structural check. Repair the container and reject a malformed track before dispatch. | TL |
| G3H2-083 | Readback scratch | Ownership | The screenshot staging mirror pointer alone represented ownership. Add an authoritative owned pointer. | CANVAS |
| G3H2-084 | Readback scratch | Ownership | Capacity lacked allocation identity. Bind authoritative capacity to pointer with an address-derived cookie. | CANVAS |
| G3H2-085 | Readback scratch | Memory safety | A corrupt staging pointer mirror could be passed to the backend. Restore it from authoritative storage. | CANVAS |
| G3H2-086 | Readback scratch | Bounds | A corrupt capacity mirror could suppress allocation. Restore it from authoritative capacity. | CANVAS |
| G3H2-087 | Readback scratch | Ownership | An invalid ownership tuple could pass borrowed memory to `realloc`. Detach it without freeing or resizing the untrusted pointer. | CANVAS |
| G3H2-088 | Readback scratch | Resource | Staging storage could grow beyond any supported render target. Enforce the maximum RGBA byte count. | CANVAS |
| G3H2-089 | Readback scratch | Overflow | Geometric growth could overflow or overshoot the policy ceiling. Fall back to the checked exact request. | CANVAS |
| G3H2-090 | Canvas finalizer | Ownership | Finalization freed the mutable staging mirror. Free only the cookie-valid owned allocation. | CANVAS |
| G3H2-091 | Screenshot | Bounds | Logical screenshot/render-target dimensions were not checked against the runtime maximum in the copy path. Reject them before allocation or sync. | CANVAS |
| G3H2-092 | Screenshot | Bounds | Physical HiDPI framebuffer dimensions could exceed the runtime maximum. Reject them before backend readback and stride arithmetic. | CANVAS |
| G3H2-093 | BC6H block | Memory safety | A null output pointer was immediately cleared. Return without dereferencing it. | BC6H |
| G3H2-094 | BC6H block | Memory safety | A null input block was read for mode bits. Zero a valid output and fail closed. | BC6H |
| G3H2-095 | BC6H arithmetic | Portability | Floor division negated a negative `int64_t` directly, which is undefined for `INT64_MIN`. Form magnitude without signed overflow. | STATIC, BUILD |
| G3H2-096 | BC6H layout | Memory safety | Field descriptors indexed the endpoint variable array without a destination bound. Reject an invalid descriptor. | STATIC, BUILD |
| G3H2-097 | BC6H layout | Undefined behavior | Forward field shifts could exceed a 32-bit destination; one dead unsigned-negative check obscured the real guard. Validate offset and width before publication. | STATIC, BUILD |
| G3H2-098 | BC6H layout | Undefined behavior | Reversed field shifts lacked an equivalent offset/width guard. Reject malformed layout metadata before shifting. | STATIC, BUILD |
| G3H2-099 | BC6H reader | Performance | A failed bounded index read still interpolated the remainder of the block before clearing it. Fail closed immediately. | BC6H |
| G3H2-100 | BC6H fallback | Memory safety | A null source buffer reached pointer arithmetic in the mip fallback. Reject it before size calculations. | STATIC, BUILD |
| G3H2-101 | BC6H fallback | Overflow | `(dimension + 3) / 4` wrapped for extreme unsigned dimensions. Use subtraction-based ceil division after dimension validation. | STATIC, BUILD |
| G3H2-102 | BC6H fallback | Resource | The mip decoder accepted arbitrary positive counts despite the KTX2 policy ceiling. Enforce the 32-level maximum locally. | STATIC, BUILD |
| G3H2-103 | BC6H fallback | Overflow | The successful-mip counter used `int` while the API count is `int64_t`. Match the counter width so accumulation cannot overflow first. | STATIC, BUILD |
| G3H2-104 | Entity visit set | Memory safety | A null slot pointer paired with nonzero capacity could bypass allocation and reach hashing writes. Validate pointer/count/capacity agreement before reserve or insert. | STATIC, BUILD |
| G3H2-105 | Stable-ID set | Memory safety | ID-set rehash could iterate a null old pointer when corrupt metadata reported nonzero capacity. Validate the complete power-of-two layout before growth. | STATIC, BUILD |

## 4. Focused validation record

macOS arm64, warning-as-error configuration:

```text
ZANNA_BUILD_DIR=build-g3h ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 \
  ZANNA_SKIP_LINT=1 ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 \
  ZANNA_SKIP_INSTALL=1 ZANNA_SKIP_STUDIO=1 ./scripts/build_zanna_mac.sh
passed; Warn-as-error: ON

ctest --test-dir build-g3h \
  -R '^(test_rt_model3d|test_rt_textureasset3d|test_rt_canvas3d_gpu_paths|test_rt_canvas3d|test_rt_game3d|test_rt_game3d_cinematics)$' \
  --output-on-failure -j6
6/6 passed

ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-g3h-sanitize \
  -R '^(test_rt_model3d|test_rt_textureasset3d|test_rt_canvas3d_gpu_paths|test_rt_canvas3d|test_rt_game3d|test_rt_game3d_cinematics)$' \
  --output-on-failure -j6
6/6 passed under AddressSanitizer and UndefinedBehaviorSanitizer

ctest --test-dir build-g3h -L graphics3d --output-on-failure -j8
150/150 passed; soak passed in 122.60 seconds

./scripts/lint_platform_policy.sh
Platform policy lint: clean
```

Warning, performance, and portability checks with `cppcheck` also completed
without findings for every changed C translation unit. Leak detection was
disabled in the sanitizer run because the Apple AddressSanitizer runtime does
not support LeakSanitizer; address and undefined-behavior checks remained
enabled and fail-fast.
