---
status: complete
audience: contributors
last-verified: 2026-08-10
---

# Graphics3D Runtime Hardening Program, Round Three (2026-08-10)

## 1. Summary

This ledger records a third non-duplicative review of the C runtime under
`src/runtime/graphics/3d`. The 100 findings concentrate on the previously
under-reviewed PostFX3D runtime and its backend chain boundary: retained
storage ownership, corrupt-state repair, numeric normalization, packed CPU
framebuffer semantics, temporal stability, and bounded backend export.

The public backend-chain struct gains appended ownership metadata. That C ABI
decision is recorded in [ADR 0245](../adr/0245-postfx-chain-address-bound-ownership.md).
No external dependency, IL opcode, language grammar, runtime registry method,
serialized format, platform branch, or workflow changes.

## 2. Hardening contract

- Mutable pointer/count/capacity mirrors do not prove ownership. Only an
  address-bound authoritative tuple permits resize, clear, or free.
- Authored and backend PostFX chains contain at most 4,096 entries; every
  retained descriptor is normalized again before traversal or export.
- CPU effects share one packed `RGBRGB...` float representation and preserve
  the render target's alpha bytes.
- Reusable frame, effect, and history buffers are bounded by the supported
  render-target layout and grow transactionally.
- Topology changes reset temporal state. TAA reads an immutable current-frame
  snapshot so results do not depend on pixel traversal order.
- Backend-chain by-value copies are borrowed views and cannot release the
  stable owner's allocation.

## 3. Corrected issue ledger

Evidence keys:

- `SNAP`: `test_rt_postfx3d_snapshot` retained-state, cap, export, and ownership cases.
- `CPU`: `test_rt_postfx3d_cpu` packed LUT, exposure, TAA, and alpha cases.
- `BACKEND`: shared Metal, D3D11, OpenGL, and backend-utils tests.
- `PARITY`: `g3d_test_canvas3d_postfx_parity` end-to-end Canvas3D behavior.
- `STATIC`: direct invariant inspection, warning-as-error compilation, and static analysis.
- `G3D`: complete `graphics3d` CTest label.
- `PLATFORM`: repository platform-policy lint.

| ID | Area | Class | Finding and resolution | Evidence |
|---|---|---|---|---|
| G3H3-001 | Effect storage | Ownership | The mutable effect pointer authorized resize and free. Add an authoritative owned pointer and address-bound cookie. | SNAP |
| G3H3-002 | Effect storage | Bounds | Mutable capacity could suppress growth or authorize an oversized clear. Restore it only from validated owner capacity. | SNAP |
| G3H3-003 | Effect storage | Bounds | Mutable count also represented initialized storage. Track the initialized authoritative prefix separately. | SNAP |
| G3H3-004 | Effect repair | Memory safety | Pointer/count/capacity disagreement could traverse borrowed memory. Repair all traversal mirrors from the owner tuple. | SNAP |
| G3H3-005 | Effect storage | Resource | Authored chains grew to allocator limits. Enforce the 4,096-entry logical ceiling. | SNAP |
| G3H3-006 | Effect storage | Overflow | Doubling capacity could overflow near the ceiling. Use checked, saturating growth. | STATIC |
| G3H3-007 | Effect storage | Determinism | Grown tail entries were not an authoritative initialized region. Zero the complete new tail before publication. | STATIC, BUILD |
| G3H3-008 | Effect append | Correctness | Adding an effect preserved stale TAA/exposure history from a different topology. Reset temporal state on append. | SNAP |
| G3H3-009 | Effect repair | Type safety | Unknown retained discriminators reached union dispatch. Remove them with stable compaction. | SNAP |
| G3H3-010 | Effect repair | Correctness | Retained per-entry enable bytes accepted noncanonical values. Normalize each to zero or one. | SNAP |
| G3H3-011 | Chain clear | Correctness | Clear reset only the mutable count. Reset the authoritative initialized prefix too. | SNAP |
| G3H3-012 | Chain clear | Determinism | Cleared descriptors remained in warm storage. Zero the retained allocation before reuse. | SNAP |
| G3H3-013 | Finalizer | Ownership | Finalization freed the mutable effect mirror. Free only a cookie-valid owner allocation. | SNAP |
| G3H3-014 | Chain queries | Bounds | Count/kind/export paths trusted retained mirrors independently. Route traversal through common repair. | SNAP |
| G3H3-015 | Effect removal | Correctness | Removal needed stable authored ordering. Shift the validated suffix without reordering it. | SNAP |
| G3H3-016 | Effect removal | Determinism | The vacated tail retained stale union bytes. Zero it after shifting. | SNAP |
| G3H3-017 | Effect removal | Correctness | Removing an entry left history derived from the previous topology. Reset temporal state. | SNAP |
| G3H3-018 | Effect removal | Lifetime | Removing the last LUT entry retained its Pixels object indefinitely. Release it when no LUT entry remains. | SNAP |
| G3H3-019 | Bloom | Numeric | Retained threshold could be negative or non-finite. Restore a bounded nonnegative value. | SNAP |
| G3H3-020 | Bloom | Numeric | Retained intensity could be negative or non-finite. Restore a bounded nonnegative value. | SNAP |
| G3H3-021 | Bloom | Bounds | Retained pass count could exceed CPU/backend loop policy. Clamp it to 0–32. | SNAP |
| G3H3-022 | Tonemap | Validation | Retained mode could select an unknown mapping branch. Clamp it to the closed 0–2 range. | SNAP |
| G3H3-023 | Tonemap | Numeric | Retained exposure could be negative or non-finite. Restore a bounded nonnegative multiplier. | SNAP |
| G3H3-024 | FXAA | Numeric | Retained edge threshold could be non-finite or outside normalized space. Clamp it to `[0,1]`. | SNAP |
| G3H3-025 | FXAA | Numeric | Retained minimum threshold had the same exposure. Normalize it independently. | SNAP |
| G3H3-026 | Color grade | Numeric | Retained brightness could be non-finite or extreme. Clamp it to the authored signed interval. | SNAP |
| G3H3-027 | Color grade | Numeric | Retained contrast could be negative or non-finite. Clamp it to the supported multiplier range. | SNAP |
| G3H3-028 | Color grade | Numeric | Retained saturation could be negative or non-finite. Clamp it independently. | SNAP |
| G3H3-029 | Vignette | Numeric | Retained radius could escape normalized coordinates. Clamp it to `[0,1]`. | SNAP |
| G3H3-030 | Vignette | Numeric | Zero, negative, or non-finite softness destabilized its division. Restore a positive bounded value. | SNAP |
| G3H3-031 | SSAO | Numeric | Retained radius could be negative, non-finite, or extreme. Apply the scene-radius ceiling. | SNAP |
| G3H3-032 | SSAO | Numeric | Retained intensity could be negative or non-finite. Restore a bounded nonnegative value. | SNAP |
| G3H3-033 | SSAO | Bounds | Retained sample count could drive an excessive loop. Clamp it to 1–128. | SNAP |
| G3H3-034 | Depth of field | Numeric | Retained focus distance could be negative, non-finite, or extreme. Normalize it to scene limits. | SNAP |
| G3H3-035 | Depth of field | Numeric | Retained aperture could be negative or non-finite. Restore a bounded nonnegative value. | SNAP |
| G3H3-036 | Depth of field | Bounds | Retained maximum blur could drive unbounded gathers. Clamp it to 0–128 pixels. | SNAP |
| G3H3-037 | Motion blur | Numeric | Retained intensity could be non-finite or outside a blend weight. Clamp it to `[0,1]`. | SNAP |
| G3H3-038 | Motion blur | Bounds | Retained sample count could drive an excessive gather. Clamp it to 1–64. | SNAP |
| G3H3-039 | TAA | Numeric | Retained blend could be non-finite or remove current-frame influence. Clamp it to 0.5–0.98. | SNAP |
| G3H3-040 | SSR | Numeric | Retained intensity could be non-finite or outside a blend weight. Clamp it to `[0,1]`. | SNAP |
| G3H3-041 | SSR | Numeric | Retained roughness cutoff could escape normalized material space. Clamp it to `[0,1]`. | SNAP |
| G3H3-042 | SSR | Bounds | Retained step count could drive an excessive march. Clamp it to 1–128. | SNAP |
| G3H3-043 | Auto exposure | Numeric | Retained minimum EV could be non-finite or extreme. Clamp it to the supported EV range. | SNAP |
| G3H3-044 | Auto exposure | Numeric | Retained maximum EV needed independent normalization. Clamp it to the same range. | SNAP |
| G3H3-045 | Auto exposure | Correctness | A repaired maximum not greater than minimum made adaptation nonsensical. Restore the ordered default interval. | SNAP |
| G3H3-046 | Auto exposure | Numeric | Retained adaptation speed could be zero, negative, or non-finite. Restore a positive bounded rate. | SNAP |
| G3H3-047 | Color LUT | Numeric | Retained LUT blend could be non-finite or outside interpolation space. Clamp it to `[0,1]`. | SNAP |
| G3H3-048 | Sun shafts | Numeric | Retained intensity could be negative or non-finite. Restore a bounded nonnegative value. | SNAP |
| G3H3-049 | Sun shafts | Numeric | Retained decay could be non-finite or unstable at interval endpoints. Clamp it inside `(0,1)`. | SNAP |
| G3H3-050 | Sun shafts | Bounds | Retained radial sample count could exceed the implementation budget. Clamp it to 8–48. | SNAP |
| G3H3-051 | Chain state | Correctness | The master enabled byte accepted noncanonical retained values. Normalize it during repair. | SNAP |
| G3H3-052 | Diagnostics | Memory safety | A corrupted error buffer could reach `strlen` without a terminator. Force termination before readback. | SNAP |
| G3H3-053 | Temporal matrix | Numeric | Cached previous view-projection lanes could contain NaN or infinity. Invalidate and clear the matrix. | SNAP |
| G3H3-054 | Auto exposure | Numeric | Cached EV state could be non-finite or outside policy. Reset both value and validity. | SNAP |
| G3H3-055 | Temporal flags | Correctness | TAA, previous-matrix, and exposure validity bytes accepted arbitrary values. Canonicalize or invalidate them. | SNAP |
| G3H3-056 | LUT lifetime | Ownership | A mutable LUT pointer alone represented a retained reference. Add an address-bound owner record. | SNAP |
| G3H3-057 | LUT repair | Type safety | A corrupt LUT mirror could be sampled or released as Pixels. Restore only a class-valid authoritative owner. | SNAP |
| G3H3-058 | Chain clear | Lifetime | Clear removed the LUT entry but kept its retained Pixels source. Release the owner explicitly. | SNAP |
| G3H3-059 | Worker pool | Ownership | A mutable pool pointer alone authorized shutdown and release. Add owner, worker count, and cookie metadata. | SNAP |
| G3H3-060 | Worker pool | Performance | Lazy creation queried the default worker count twice. Compute and retain it once. | STATIC |
| G3H3-061 | Worker pool | Type safety | A corrupt pool mirror could reach submit/shutdown. Restore only a class-valid owner with a bounded count. | SNAP |
| G3H3-062 | TAA history | Ownership | Mutable history pointer/dimensions authorized allocation operations. Add a cookie-valid owner and byte capacity. | SNAP |
| G3H3-063 | TAA history | Bounds | Corrupt dimensions could claim more initialized history than allocated. Validate layout bytes against owner capacity. | SNAP |
| G3H3-064 | Frame scratch | Ownership | The retained CPU framebuffer mirror could be resized or freed when borrowed. Give it independent owner metadata. | SNAP |
| G3H3-065 | Primary scratch | Ownership | Primary effect scratch had the same mirror-only ownership problem. Give it an independent cookie domain. | SNAP |
| G3H3-066 | Secondary scratch | Ownership | Secondary effect scratch also needed independent ownership proof. Add its own owner tuple. | SNAP |
| G3H3-067 | Float storage | Resource | Frame/history requests were bounded only by `size_t`. Enforce the supported render-target dimension/byte ceiling. | SNAP, STATIC |
| G3H3-068 | Float layout | Overflow | Width×height×channels×bytes could overflow in scattered callers. Centralize checked layout arithmetic. | STATIC, BUILD |
| G3H3-069 | Float storage | Performance | Exact-size growth caused repeated reallocations as targets grew. Use checked 1.5× geometric growth. | STATIC |
| G3H3-070 | Float storage | Memory safety | Growth could publish capacity before `realloc` succeeded. Commit pointer, capacity, and cookie transactionally. | STATIC, BUILD |
| G3H3-071 | CPU apply | Performance | Per-frame and effect scratch allocations churned despite stable dimensions. Retain and reuse all three allocations. | CPU, PARITY |
| G3H3-072 | Gamma conversion | Performance | Every tonemap invocation rebuilt the 1,025-entry gamma table. Initialize one process-lifetime table atomically. | STATIC, BACKEND |
| G3H3-073 | SSAO CPU | Correctness | SSAO treated packed RGB as planar channels, modulating unrelated pixels. Index each pixel at `i*3`. | CPU, PARITY |
| G3H3-074 | DOF CPU | Correctness | DOF copied/sampled packed storage with planar channel offsets. Use packed triplets throughout. | CPU, PARITY |
| G3H3-075 | Motion blur CPU | Correctness | Motion blur gathered planar offsets from a packed buffer. Use packed source/destination triplets. | CPU, PARITY |
| G3H3-076 | SSR CPU | Correctness | SSR ray hits blended planar channel regions. Blend the hit pixel's packed triplet. | CPU, PARITY |
| G3H3-077 | Auto exposure CPU | Correctness | Luminance sampled three different pixels as RGB. Measure each packed pixel triplet. | CPU |
| G3H3-078 | Color LUT CPU | Correctness | LUT input/output used planar indexing, moving channels between pixels. Transform packed triplets. | CPU |
| G3H3-079 | Sun shafts CPU | Correctness | Shaft compositing addressed planar channel blocks. Composite each packed triplet. | CPU, PARITY |
| G3H3-080 | TAA CPU | Correctness | History and current-frame accesses disagreed with the packed representation. Store and sample packed triplets. | CPU |
| G3H3-081 | TAA CPU | Determinism | In-place resolve made later neighborhood clamps observe already-blended pixels. Snapshot the current frame before traversal. | CPU |
| G3H3-082 | CPU output | Correctness | Scene-aware rewrites risked copying RGB work over alpha. Keep alpha outside float work storage and preserve target bytes. | CPU |
| G3H3-083 | TAA history | Performance | A dimension change freed and freshly allocated history. Grow and reuse the bounded owner allocation. | CPU, STATIC |
| G3H3-084 | Matrix inverse | Memory safety | The helper accepted null matrix pointers. Reject them before lane access. | STATIC, PARITY |
| G3H3-085 | Matrix inverse | Numeric | A failed/non-finite inverse could partially publish output. Compute locally, validate every lane, then commit. | STATIC, PARITY |
| G3H3-086 | Depth input | Numeric | NaN, infinity, or out-of-NDC depth reached reconstruction. Reject it at the common accessor. | PARITY, STATIC |
| G3H3-087 | Linear depth | Numeric | Invalid near/far metadata or non-finite division results reached effect kernels. Validate metadata and output. | PARITY, STATIC |
| G3H3-088 | Reprojection | Numeric | World/project helpers accepted non-finite matrices, vectors, or homogeneous divisors. Fail closed. | PARITY, STATIC |
| G3H3-089 | Scene apply | Bounds | Canvas target dimensions reached float allocation before common policy validation. Reject unsupported layouts first. | PARITY, STATIC |
| G3H3-090 | Scene apply | Numeric | Non-finite or inverted camera planes contaminated reconstruction. Restore safe near/far metadata. | PARITY, STATIC |
| G3H3-091 | Sun projection | Correctness | Sun coordinates were published even when matrix inversion/project failed. Mark them valid only after complete projection. | PARITY |
| G3H3-092 | Temporal cache | Correctness | An invalid current view-projection could become next frame's history transform. Cache only a finite validated matrix. | PARITY |
| G3H3-093 | Backend chain | Ownership | Public pointer/capacity mirrors authorized resize and free. Append owner pointer, capacity, and address-bound cookie. | SNAP, ADR 0245 |
| G3H3-094 | Backend chain | Ownership | By-value chain copies appeared to own the same allocation. Bind ownership to the stable containing address so copies are borrowed. | SNAP, BACKEND |
| G3H3-095 | Backend reset | Memory safety | Reset could clear borrowed stack storage. Detach an invalid owner tuple without touching its pointer. | SNAP |
| G3H3-096 | Backend free | Memory safety | Free could release borrowed or corrupted storage. Release only a cookie-valid owner tuple. | SNAP, BACKEND |
| G3H3-097 | Backend copy | Resource | Source count could request an unbounded destination allocation. Reject counts above 4,096. | SNAP |
| G3H3-098 | Backend copy | Type safety | Unknown source effect kinds reached backend dispatch. Validate the full source before mutation. | SNAP |
| G3H3-099 | Backend copy | Numeric | Copy propagated malformed snapshot fields. Sanitize every descriptor into the destination. | SNAP, BACKEND |
| G3H3-100 | Backend export | Correctness | Auto exposure, color LUT, and sun shafts silently disappeared from ordered exports. Preserve all three discriminators and enforce the chain cap in backend usability checks. | SNAP, BACKEND |

## 4. Validation record

macOS arm64, warning-as-error configuration:

```text
ctest --test-dir build \
  -R '^(test_vgfx3d_backend_utils|test_vgfx3d_backend_d3d11_shared|test_vgfx3d_backend_metal_shared|test_vgfx3d_backend_opengl_shared|test_rt_postfx3d_snapshot|test_rt_postfx3d_cpu|g3d_test_canvas3d_postfx_parity)$' \
  --output-on-failure
7/7 passed

ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-g3h-sanitize \
  -R '^(test_vgfx3d_backend_utils|test_vgfx3d_backend_d3d11_shared|test_vgfx3d_backend_metal_shared|test_vgfx3d_backend_opengl_shared|test_rt_postfx3d_snapshot|test_rt_postfx3d_cpu)$' \
  --output-on-failure -j6
6/6 passed under AddressSanitizer and UndefinedBehaviorSanitizer

ctest --test-dir build -L graphics3d --output-on-failure -j8
151/151 passed; soak passed in 120.56 seconds

cppcheck --project=build/compile_commands.json \
  --file-filter='*/src/runtime/graphics/3d/*' \
  --enable=warning,performance,portability --check-level=exhaustive \
  --error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem
106/106 translation units checked; no findings

./scripts/lint_platform_policy.sh
Platform policy lint: clean

ZANNA_SKIP_CLEAN=1 ./scripts/build_zanna_mac.sh
warnings-as-errors build and Zanna Studio native build passed
1967/1967 non-slow tests passed (one expected platform skip)
runtime surface audit passed (7,868 functions, 532 classes, 9,224 declarations)
cross-platform smoke and native smoke lanes passed
non-interactive install completed

git diff --check
passed
```

Leak detection was disabled because the Apple AddressSanitizer runtime does not
provide LeakSanitizer; address and undefined-behavior checks remained fail-fast.
