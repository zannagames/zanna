# Graphics3D C Runtime Hardening Review

Date: 2026-08-08  
Scope: the C runtime beneath `src/runtime/graphics/3d`, its 2D window-adoption
seam, public C declarations, focused runtime tests, and the existing adoption
ADR. This review is additive to the 32-item review dated 2026-07-13; none of
those earlier resolutions are counted below.

## Outcome

This pass implements 50 additional correctness, ownership, overflow,
concurrency, failure-atomicity, and bounded-work improvements. The largest
changes replace partial fixed-stack Model3D walks with a checked preflight
collector, make Canvas3D window adoption an exclusive retained loan, harden
animation mirror-name resolution, and place explicit bounds around overlay
text and geometry operations.

Priority meanings: P0 is a correctness, lifetime, concurrency, undefined
behavior, or failure-atomicity defect; P1 is significant robustness or
performance work; P2 is API/documentation or lower-frequency hardening.

## Implemented findings

| # | Priority | Finding | Implemented resolution |
|---:|:---:|---|---|
| 1 | P0 | Model operations ignored scene roots after the old 64-entry root stack filled. | A dynamic collector now includes the template root and every validated scene root. |
| 2 | P0 | A traversal allocation failure could leave StripMeshes or SimplifyMeshes partially applied. | Every hierarchy is fully preflighted before mutation begins. |
| 3 | P0 | Cyclic child graphs could repeatedly revisit nodes until the silent walk ceiling. | An open-addressed seen set makes cycles terminate deterministically. |
| 4 | P1 | Shared roots/subtrees were processed repeatedly. | Pointer-identity deduplication visits each live node exactly once. |
| 5 | P0 | Root/child payloads could be followed without consistent class validation. | Every collected root and edge is checked as a live SceneNode3D handle. |
| 6 | P0 | The old `>=` ceiling silently skipped the boundary node and its descendants. | The collector accepts the documented limit and traps before applying an oversized walk. |
| 7 | P0 | Seen-table growth lacked a reusable checked allocation path. | Power-of-two growth now checks capacity and byte multiplication before allocation. |
| 8 | P0 | Node-stack/vector growth could overflow or degrade into a partial walk. | A size-checked vector helper either appends completely or fails the preflight. |
| 9 | P1 | SimplifyMeshes walked the whole hierarchy once per inventory mesh. | It collects once and reuses the node vector for every replacement. |
| 10 | P0 | GenerateLODs had a second, inconsistent hierarchy walker with root truncation and partial growth failures. | It now shares the comprehensive collector. |
| 11 | P0 | LOD-chain capacity doubling evaluated signed multiplication before proving it safe. | The `INT32_MAX / 2` guard now precedes multiplication. |
| 12 | P0 | Pending-node capacity and allocation sizes could overflow during LOD preparation. | Both arrays use pre-multiply capacity and `SIZE_MAX` checks. |
| 13 | P0 | Failure to allocate a mesh-chain entry silently skipped that node and returned partial success. | Phase-one allocation failure now aborts before worker mutation and emits a diagnostic. |
| 14 | P0 | Machine core counts were narrowed to `int32_t` before all bounds were applied. | Worker count remains `int64_t` until capped to the chain count and eight workers. |
| 15 | P0 | A non-finite mesh bounding radius could create non-finite LOD thresholds. | Radius validation uses a finite positive fallback before distance generation. |
| 16 | P1 | Ratios close to one could request the same triangle count for successive LODs. | Every target is forced below its current source count. |
| 17 | P0 | The LOD builder trusted simplifier output without validating its handle or actual triangle count. | Each result is validated and accepted only when it is nonempty and strictly smaller. |
| 18 | P0 | Rejected/no-progress LOD objects could leak their construction reference. | Invalid results are released immediately. |
| 19 | P0 | A node could retain a partially attached LOD chain after an add failure. | Attachment count is verified and incomplete chains are cleared. |
| 20 | P0 | GenerateLODs counted nodes even if their full chain was not attached. | Success accounting and auto-LOD activation occur only after complete attachment. |
| 21 | P2 | `rt_model3d_strip_meshes` was implemented/exported but absent from its C header. | The declaration and ownership/return contract are now documented. |
| 22 | P2 | `rt_model3d_simplify_meshes` was implemented/exported but absent from its C header. | The declaration and mutation/return contract are now documented. |
| 23 | P0 | Canvas3D stored a borrowed lender pointer that could outlive the caller's Canvas reference. | A successful window loan retains the 2D Canvas for the complete adoption lifetime. |
| 24 | P0 | Returning an adopted window did not balance lender ownership. | The new return helper releases the loan retain after state invalidation. |
| 25 | P0 | Two Canvas3D objects could adopt one platform window simultaneously. | Window adoption is now an exclusive state transition. |
| 26 | P0 | Concurrent adopters had a check-then-act race. | Atomic compare-and-exchange elects exactly one borrower. |
| 27 | P0 | `Canvas.Close` could race adoption and destroy a window after it was borrowed. | Borrow and close claim the same atomic state before touching the window. |
| 28 | P0 | Explicit close during a live adoption could invalidate Canvas3D's window. | Close traps and preserves the window while the loan is active. |
| 29 | P0 | A failed Canvas3D constructor stranded the exclusive lender loan. | Constructor failure immediately returns the window and releases the retain. |
| 30 | P1 | Loan return could release the last Canvas reference before its presentation cache was invalidated. | Cache invalidation is completed before the balanced release. |
| 31 | P0 | Mirror-name matching treated substrings such as `Bright` and `Leftover` as side tokens. | Side words must begin/end at delimiters or camel-case boundaries. |
| 32 | P1 | All-uppercase rig tokens (`LEFT`/`RIGHT`) were not mirrored. | Exact uppercase variants are supported alongside title/lowercase forms. |
| 33 | P0 | The fixed mirror-name buffer could return a truncated name that accidentally matched another bone. | Name swapping is all-or-nothing and clears undersized outputs. |
| 34 | P1 | Exact mirror partners with names longer than 95 bytes were unreachable. | Long names receive a checked dynamically sized swap buffer. |
| 35 | P0 | Dynamic mirror-name capacity math needed an overflow proof. | Required capacity is computed only after a `SIZE_MAX` subtraction guard. |
| 36 | P0 | Mirror-name allocation failure could not safely continue. | It falls back to the bounded stack buffer and then role matching without using partial data. |
| 37 | P0 | Two inferred humanoid roles targeting one partner silently dropped a valid channel. | A colliding channel first falls back to its still-unused source bone. |
| 38 | P0 | Irreconcilable mirror-channel collisions returned a silently incomplete animation. | The mirror operation now fails closed and releases the partial clip. |
| 39 | P0 | Retarget conformance initialization relied indirectly on an `ok` flag to prove two allocations. | Initialization is structurally dominated by explicit success of both allocations. |
| 40 | P0 | TtfFont cache identities had a data race and signed-increment overflow. | A relaxed atomic unsigned counter provides defined wrap and skips reserved zero. |
| 41 | P1 | TrueType draw and measurement could consume different prefixes of long strings. | Both paths use one shared 512-byte prefix helper and the same clamped face metrics. |
| 42 | P0 | Byte truncation could split UTF-8 and runtime strings with embedded NULs were not handled from stored length. | Prefix copying respects runtime byte length, stops at NUL, and ends before an incomplete codepoint. |
| 43 | P0 | TrueType raster dimensions were narrowed before finite/range checks, and height lacked the render-target maximum. | Width and height remain `double` through finite, positive, and maximum validation. |
| 44 | P0 | Image-region validation evaluated caller-controlled signed endpoint sums and accepted out-of-bounds rectangles. | Subtraction-based bounds checks precede frame creation and UV calculation. |
| 45 | P0 | Nine-slice source, destination, and coordinate sums could overflow signed `int64_t`. | Insets use subtraction checks and final coordinates are composed in floating point. |
| 46 | P0 | World-to-screen accepted null outputs, invalid dimensions, and non-finite input/matrix results. | It now validates all pointers, dimensions, clip coordinates, and projected outputs. |
| 47 | P0 | Frame/rounded-frame endpoint addition and clip float-to-`int64_t` getters could invoke signed overflow or out-of-range conversion. | Overlay endpoints use float components and getters use a saturating conversion helper. |
| 48 | P0 | Crosshair size narrowed to `int32_t` and endpoint addition could overflow. | Size is clamped to the output span and endpoints are formed with bounded float arithmetic. |
| 49 | P1 | Bitmap text rendering performed two unbounded passes while measurement reported the full string. | Draw and measure now share a documented 512-byte ceiling. |
| 50 | P0 | Debug AABB, sphere, ray, and axis helpers allocated/queued geometry after non-finite or overflowing endpoint math. | Each helper validates finite inputs and derived endpoints before allocation or frame work. |

## Regression coverage

- `test_rt_model3d`: more than 64 independent scene roots, a duplicate root,
  and near-one LOD ratios with strict triangle-count progress.
- `test_rt_canvas3d`: retained/exclusive window loans, 16 simultaneous
  borrowers, adopted-window close rejection, overflow-safe image/nine-slice
  input, saturating clip readback, invalid projection rejection, bounded bitmap
  text, long TrueType text, and a UTF-8 sequence crossing byte 512.
- `test_rt_canvas_state_contract`: isolated Canvas linkage includes the new
  ownership seam.
- `test_rt_skeleton3d`: substring boundaries, uppercase tokens, long exact
  names, and mirror-role collisions that preserve channels.

The Canvas adoption and animation mirroring contracts are updated in ADRs 0242
and 0243. ADR 0244 records the internal C ABI classification for the offline
Model3D bake helpers.

## Validation

- Warning-as-error incremental macOS build: passed.
- Focused CTests (`test_rt_canvas3d`, `test_rt_model3d`,
  `test_rt_skeleton3d`, `test_rt_canvas_state_contract`): passed.
- `scripts/lint_platform_policy.sh`: passed.
- Focused C11 cppcheck warning/performance/portability pass: passed.
- Full `graphics3d` CTest label: 150/150 passed, including the 120-second soak.
- Complete build-script CTest run: 1,964/1,965 passed initially. The sole
  failure was the strict surface audit identifying the two newly declared
  offline model helpers as unclassified; ADR 0244 and
  `RuntimeSurfacePolicy.inc` now classify them, and both surface-audit tests
  pass on rerun.
- Post-test build-script stages: platform policy lint, runtime surface audit
  (8/8 focused tests), cross-platform smoke tests, and local install passed.
- Final combined regression/surface rerun: 6/6 passed.
