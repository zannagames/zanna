---
status: active
audience: contributors
last-verified: 2026-08-10
---

# Graphics 2D Runtime Hardening Audit (August 2026)

This second follow-up reviews the C runtime under `src/runtime/graphics/2d/`
after findings `G2D-001` through `G2D-319`. It concentrates on complete
private-state validation for Camera, Scene, SpriteSheet, TextureAtlas, and the
Action registry, including malformed same-class runtime objects and corrupt
private linked-list state.

The 100 findings below are fixed. No public runtime function, registered class,
IL rule, opcode, or C ABI entry point changed. All cookies and state checks are
private implementation details, so this hardening pass does not require an ADR.

## Regression suites

| Tag | Focused binaries |
|-----|------------------|
| `CAM` | `test_rt_graphics2d` |
| `SCN` | `test_rt_scene` |
| `ATL` | `test_rt_sprite_consolidated` |
| `ACT` | `test_rt_action_mapping`, `test_rt_action_persist` |

## Camera state and parallax ownership

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-320 | Bug | A zeroed same-class Camera payload passed the old size-and-class check. Constructed cameras now carry a private state cookie. | `CAM` |
| G2D-321 | Correctness | Camera dimensions and zoom were trusted after construction. Shared validation now requires positive dimensions and the supported 10–1000 percent zoom range. | `CAM` |
| G2D-322 | Correctness | Bounds/dirty flags and dead-zone sizes could become noncanonical or negative. The Camera checker now enforces Boolean flags and nonnegative zones. | `CAM` |
| G2D-323 | Bug | A corrupt parallax count could drive fixed-array traversal outside its eight slots. Every Camera API now proves the count range first. | `CAM` |
| G2D-324 | Correctness | Parallax active markers admitted arbitrary byte values. Each slot now has a canonical Boolean state. | `CAM` |
| G2D-325 | Bug | Active parallax slots trusted a class-correct Pixels header without proving its complete allocation. Retained sources now pass full Pixels validation. | `CAM` |
| G2D-326 | Bug | Prepared parallax cache handles received only shallow trust. Cached Pixels now pass the same complete validator before use or release. | `CAM` |
| G2D-327 | Correctness | Corrupt parallax scroll factors could escape the documented 0–100 percent range. Slot validation now enforces both factors. | `CAM` |
| G2D-328 | Bug | The stored parallax count could disagree with the number of active slots, making admission and lookup inconsistent. Validation now recomputes the active total. | `CAM` |
| G2D-329 | Resource | Inactive slots could retain stale source or cache pointers that finalization might mishandle. Inactive state now requires both references to be null. | `CAM` |
| G2D-330 | Bug | `AddParallax` accepted malformed same-class Pixels objects and retained them. Admission now uses the complete soft Pixels cast. | `CAM` |
| G2D-331 | Resource | Camera finalization trusted uninitialized payload state and unbounded slot metadata. It now requires the cookie, clears it, and releases only the fixed slot range. | `CAM` |
| G2D-332 | Correctness | Camera construction relied on allocator-zeroed private bytes. It now explicitly initializes the complete payload before installing defaults and the cookie. | `CAM` |

## Scene graph integrity and transform work

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-333 | Bug | A zeroed same-class SceneNode payload was accepted as a live node. Constructed nodes now carry a private state cookie. | `SCN` |
| G2D-334 | Correctness | Local and cached world scale could become nonpositive and poison descendant transforms. Node validation now requires every scale to remain positive. | `SCN` |
| G2D-335 | Correctness | Visibility and transform-dirty fields could hold non-Boolean values. The node checker now admits only canonical flags. | `SCN` |
| G2D-336 | Bug | A class-correct child Seq could expose inconsistent length, capacity, or item storage. Scene validation now proves the complete shallow Seq layout. | `SCN` |
| G2D-337 | Resource | A node’s child Seq could stop owning its retained elements, leaking or invalidating ownership. The established owning-sequence contract is now enforced. | `SCN` |
| G2D-338 | Bug | A non-null parent pointer was trusted by class alone. Parent links now require a complete initialized SceneNode. | `SCN` |
| G2D-339 | Bug | Optional node names could contain forged runtime handles. Non-null names now pass runtime String-handle validation. | `SCN` |
| G2D-340 | Bug | A zeroed same-class Scene payload passed its old cast. Constructed scenes now carry a private state cookie. | `SCN` |
| G2D-341 | Bug | Scene roots were not revalidated as complete, unparented nodes. The Scene checker now proves both properties at every boundary. | `SCN` |
| G2D-342 | Resource | Draw scratch capacity could disagree with its pointer or overflow its entry-byte calculation. Scene validation now enforces the pointer/capacity/size relationship. | `SCN` |
| G2D-343 | Resource | Scene and SceneNode finalizers could act on zeroed same-class payloads. Both now require and clear their private cookie. | `SCN` |
| G2D-344 | Resource | Defensive cleanup could release arbitrary forged child/name/root pointers. Owned references are now released only when they identify live heap payloads. | `SCN` |
| G2D-345 | Bug | Child traversal trusted Seq elements and did not prove reciprocal parent links. Every visited child is now a complete node whose parent is the traversed owner. | `SCN` |
| G2D-346 | Bug | Parent-chain walks could follow malformed nodes indefinitely or dereference invalid state. Every link is now validated within the existing depth bound. | `SCN` |
| G2D-347 | Bug | Transform-chain recomputation trusted all ancestors after validating only the starting node. It now validates every node before reading cached transform state. | `SCN` |
| G2D-348 | Performance | Assigning the current X coordinate dirtied the complete descendant subtree. `SetX` now returns immediately when the value is unchanged. | `SCN` |
| G2D-349 | Performance | Assigning the current Y coordinate likewise caused needless subtree work. `SetY` is now idempotent. | `SCN` |
| G2D-350 | Performance | `SetPosition` dirtied descendants when both coordinates already matched. It now tests the complete effective position first. | `SCN` |
| G2D-351 | Performance | Zero movement, including saturated movement that cannot change either coordinate, still dirtied descendants. `Move` now skips no-op results. | `SCN` |
| G2D-352 | Performance | Reassigning the current horizontal scale invalidated cached transforms. `SetScaleX` now skips identical values. | `SCN` |
| G2D-353 | Performance | Reassigning the current vertical scale invalidated cached transforms. `SetScaleY` now skips identical values. | `SCN` |
| G2D-354 | Performance | Reassigning both current scale components through the uniform setter invalidated the subtree. `SetScale` now recognizes the no-op. | `SCN` |
| G2D-355 | Performance | Reassigning the current rotation dirtied every descendant. `SetRotation` now leaves clean transform caches intact. | `SCN` |

## SpriteSheet and TextureAtlas metadata

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-356 | Bug | A zeroed same-class SpriteSheet payload passed the old checked cast. Constructed sheets now carry a private state cookie. | `ATL` |
| G2D-357 | Bug | SpriteSheet validation did not reject an unexpected private vtable field. The reserved pointer must remain null. | `ATL` |
| G2D-358 | Bug | A sheet’s backing Pixels was validated only by class and header size. It now passes complete Pixels allocation validation. | `ATL` |
| G2D-359 | Correctness | Cached sheet dimensions could diverge from the retained atlas. Both dimensions must exactly match the validated Pixels object. | `ATL` |
| G2D-360 | Bug | Region count and capacity relationships were trusted outside growth. Shared validation now enforces their nonnegative ordered bounds. | `ATL` |
| G2D-361 | Bug | Nonempty sheets could expose missing parallel name or region arrays. Both arrays are now mandatory when capacity is nonzero. | `ATL` |
| G2D-362 | Resource | Region-array capacity could overflow byte-size calculations. The checker now proves both parallel allocation sizes are representable. | `ATL` |
| G2D-363 | Resource | SpriteSheet finalization trusted corrupt state while releasing names and atlas ownership. It now requires the cookie and clamps cleanup to proven bounds. | `ATL` |
| G2D-364 | Correctness | Sheet construction depended on zero-filled private fields. The whole payload is now explicitly initialized and receives its cookie only after allocation succeeds. | `ATL` |
| G2D-365 | Bug | `GetRegion` returned corrupt retained geometry without rechecking it. Selected regions now remain within positive atlas bounds. | `ATL` |
| G2D-366 | Bug | Region lookup and `RegionNames` trusted private name pointers and lengths. Discovery now requires a pointer and a positive representable byte length. | `ATL` |
| G2D-367 | Bug | Region names could lack the trailing NUL promised by private storage. Enumeration now verifies the exact terminator. | `ATL` |
| G2D-368 | Correctness | Embedded NUL bytes could make stored region identity differ between length-aware and C-string consumers. Enumerated names must be NUL-free internally. | `ATL` |
| G2D-369 | Correctness | A region name’s cached hash could disagree with its bytes and make lookup inconsistent. Enumeration now verifies the hash. | `ATL` |
| G2D-370 | Bug | Name enumeration could publish entries whose rectangles were outside the backing image. Every enumerated record now passes geometry validation. | `ATL` |
| G2D-371 | Bug | A zeroed same-class TextureAtlas payload passed the old checked cast. Constructed atlases now carry a private state cookie. | `ATL` |
| G2D-372 | Bug | TextureAtlas retained Pixels received only shallow class validation. Atlas state now requires a complete Pixels object. | `ATL` |
| G2D-373 | Correctness | Cached TextureAtlas dimensions could disagree with the backing image. Validation now requires exact equality. | `ATL` |
| G2D-374 | Bug | A corrupt atlas region count could overrun the fixed region and hash-slot arrays. The count is now limited to their compiled capacity. | `ATL` |
| G2D-375 | Bug | Hash slots could contain negative or out-of-range one-based indices. Lookup and insertion now validate a slot before indexing a region. | `ATL` |
| G2D-376 | Bug | TextureAtlas lookup trusted private region name lengths. Each live name must have a positive representable length. | `ATL` |
| G2D-377 | Bug | TextureAtlas names could lack their exact trailing terminator. Region validation now checks it before comparison. | `ATL` |
| G2D-378 | Correctness | Embedded NUL bytes could produce inconsistent region identities. Live atlas names must be internally NUL-free. | `ATL` |
| G2D-379 | Correctness | A stale cached region hash could make an entry unreachable or alias another probe chain. Validation recomputes and compares it. | `ATL` |
| G2D-380 | Correctness | Negative region origins were admitted into private atlas state. Every live rectangle now starts at nonnegative coordinates. | `ATL` |
| G2D-381 | Bug | Positive region extents could still extend beyond the retained image. Checked subtraction now proves the complete rectangle is in bounds. | `ATL` |
| G2D-382 | Resource | TextureAtlas finalization could release forged backing ownership from uninitialized state. It now requires and clears the private cookie. | `ATL` |
| G2D-383 | Correctness | TextureAtlas construction relied on allocator-zeroed tables and metadata. The complete payload is now explicitly initialized before the cookie is installed. | `ATL` |
| G2D-384 | Bug | Grid construction accepted a malformed same-class Pixels object. It now uses the complete Pixels validator before retaining or deriving regions. | `ATL` |

## Action registry, chords, and persistence

| ID | Class | Finding and implemented resolution | Evidence |
|----|-------|------------------------------------|----------|
| G2D-385 | Bug | Action APIs treated any non-null pointer as a runtime String. Lookup, definition, removal, loading, and preset selection now reject forged handles. | `ACT` |
| G2D-386 | Resource | Action lookup followed a corrupt or cyclic global list without a traversal bound. Both C-string and runtime-string lookup stop at the registry limit. | `ACT` |
| G2D-387 | Bug | Zeroed or stale malloc storage could be interpreted as an Action node. Every constructed Action now carries a private cookie. | `ACT` |
| G2D-388 | Bug | Zeroed or stale malloc storage could be interpreted as a Binding node. Every constructed Binding now carries a separate private cookie. | `ACT` |
| G2D-389 | Bug | Private Action names were trusted without proving pointer, length, terminator, or embedded-NUL invariants. One shallow validator now proves the complete stored-name contract. | `ACT` |
| G2D-390 | Correctness | Action kind and cached pressed/released/held state could hold noncanonical values. The shallow validator now requires Boolean state. | `ACT` |
| G2D-391 | Correctness | A corrupt cached axis value could be NaN or infinity and escape through queries or updates. Action state now requires a finite cached value. | `ACT` |
| G2D-392 | Bug | Binding count could be negative, excessive, or disagree with a null head pointer. Action validation now enforces the complete shallow relationship. | `ACT` |
| G2D-393 | Bug | Binding consumers trusted type payloads without proving initialized state. Shared validation now checks the cookie, finite value, action-kind compatibility, codes, pads, and chords. | `ACT` |
| G2D-394 | Resource | The per-frame update could loop forever on a cyclic Action registry. It is now bounded by `ACTION_MAX_ACTIONS`. | `ACT` |
| G2D-395 | Resource | The per-frame update followed Binding links beyond their declared count. It now visits at most the proven per-action count. | `ACT` |
| G2D-396 | Correctness | A malformed Binding could leave partially accumulated input state visible. Update now neutralizes that action and emits only the appropriate release edge. | `ACT` |
| G2D-397 | Bug | A cyclic or overlong Binding chain could still have a valid prefix equal to the declared count. Update now detects the residual link and fails closed. | `ACT` |
| G2D-398 | Resource | Action removal traversed the global list without a bound or node validation. It now validates each node and stops at the registry limit. | `ACT` |
| G2D-399 | Bug | Ordinary binding removal trusted every linked node and could loop on corrupt state. It now validates and bounds traversal by the declared count. | `ACT` |
| G2D-400 | Resource | Freed Action and Binding nodes retained valid-looking cookies. All owned cleanup/removal paths clear cookies before release, and corrupt chains are leaked safely instead of traversed unsafely. | `ACT` |
| G2D-401 | Bug | Chord bind/unbind accepted a forged object in place of `seq<int>`. Both entry points now require a real initialized Seq handle. | `ACT` |
| G2D-402 | Bug | A class-correct Seq could expose inconsistent layout or non-integer elements before chord indexing. Chord admission now proves its storage and nontrapping-unboxes every member as an integer. | `ACT` |
| G2D-403 | Correctness | Chord creation rejected duplicate keys but chord removal did not, making invalid descriptors unexpectedly matchable. Unbind now applies the same uniqueness rule. | `ACT` |
| G2D-404 | Bug | Chord removal traversed bindings without node validation or a count bound. It now validates every visited binding and stops at the declared count. | `ACT` |
| G2D-405 | Bug | `ChordCount` could hang or count a valid prefix of a malformed list. It now validates every node and rejects any residual chain. | `ACT` |
| G2D-406 | Resource | `Action.List` could loop on a cyclic registry or read malformed names. Enumeration is now bounded and validates every Action first. | `ACT` |
| G2D-407 | Bug | Binding-description formatting switched on a potentially stale Binding node. It now recognizes only initialized binding cookies. | `ACT` |
| G2D-408 | Bug | `BindingsStr` followed the private list without a count bound or payload validation. Formatting now validates each bounded binding and fails atomically. | `ACT` |
| G2D-409 | Resource | `KeyBoundTo` could loop over corrupt Action or Binding lists. Both traversals are now bounded and validate nodes before use. | `ACT` |
| G2D-410 | Resource | `MouseBoundTo` had the same unbounded private traversals. It now fails closed on malformed registry state. | `ACT` |
| G2D-411 | Resource | `PadButtonBoundTo` also trusted cyclic or malformed lists. It now applies registry and per-action bounds plus full validation. | `ACT` |
| G2D-412 | Bug | Action serialization trusted the global registry while emitting output. Save now bounds the registry and validates every Action before appending JSON. | `ACT` |
| G2D-413 | Bug | Serialization could emit a valid Binding prefix from a cyclic or overlong chain. It now requires exact termination at the declared count. | `ACT` |
| G2D-414 | Bug | `Action.Load` passed forged String pointers into the JSON parser. The input must now be a live runtime String handle. | `ACT` |
| G2D-415 | Resource | Load’s duplicate-name scan could loop on preexisting corrupt registry state. It is now bounded and validates every existing Action. | `ACT` |
| G2D-416 | Bug | Actions staged by JSON loading lacked an initialized-state discriminator. The private cookie is now installed immediately after allocation. | `ACT` |
| G2D-417 | Resource | Preset transaction cloning trusted an unbounded or malformed Action list. It now validates and bounds the complete source registry. | `ACT` |
| G2D-418 | Resource | A failed partial preset clone advertised the source’s full binding count and could evade safe cleanup. Clone ownership is now counted incrementally and must terminate exactly. | `ACT` |
| G2D-419 | Bug | Preset selection, definition, and unique-binding insertion trusted forged names or inconsistent lists. These paths now validate handles, bound scans, and reject count/list mismatches before mutation. | `ACT` |

## Validation

- A clean warnings-as-errors macOS build completed successfully through the
  prescribed `build_zanna_mac.sh` workflow, including the Zanna Studio native
  compile, smoke probes, and staged install.
- The full non-slow CTest selection passed all 1,967 tests, with one expected
  platform/capability skip and no failures.
- Twelve affected runtime suites passed in the normal build and in separate
  AddressSanitizer and UndefinedBehaviorSanitizer builds (12/12 in each
  configuration).
- The new tests cover zeroed same-class objects, malformed retained Pixels,
  invalid Scene graph handles, clean-cache idempotent transforms, forged action
  Strings/Seqs, noncanonical Action state, invalid Binding cookies, cyclic
  Binding lists, and duplicate chord unbind descriptors.
- Scoped C static analysis completed without warning, performance, or
  portability findings. Platform-policy lint, documentation checks, source
  header audit, formatting, and whitespace validation also passed.
