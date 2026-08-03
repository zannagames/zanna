---
status: completed
audience: contributors
last-verified: 2026-08-02
---

# Graphics3D Core Runtime Deep Audit (2026-08)

## 1. Scope and method

This audit is a source-level review of the C runtime below
`src/runtime/graphics/3d`, concentrating on the previously unaudited interaction
surfaces among Skeleton3D/Animation3D playback, skinning, Cloth3D, NavMesh3D,
and VSCN rig persistence. Earlier Graphics3D audit ledgers were reviewed first;
their findings are baseline and are not recounted here.

The review combined ownership/lifetime tracing, adversarial private-state
analysis, numeric-range and transform analysis, allocation-failure ordering,
binary persistence review, hot-path complexity review, regression tests, an
incremental warnings-as-errors build, the complete Graphics3D CTest label,
platform-policy lint, and exhaustive `cppcheck` warning/performance/portability
analysis. The ledger records **163 newly corrected, independently observable
failure modes**. Entries remain separate when they cross a different allocation,
API boundary, persisted field, temporal history, or rendering result.

No public function signature, runtime registry row, runtime C ABI surface, IL
contract, opcode, grammar, verifier rule, workflow, or production cross-layer
dependency changed. The added allocation identities and repair metadata are
private implementation state, so no ADR is required by repository policy.

## 2. Evidence key

- **SKEL**: `test_rt_skeleton3d`, including sparse TRS, retargeting, extreme
  clocks, corrupted mirrors, allocation authority, and temporal history.
- **CTRL**: `test_rt_animcontroller3d`, including repaired skeleton/clip views.
- **DRAW**: `test_rt_canvas3d` and `test_rt_canvas3d_gpu_paths`, including
  extreme weights, instanced palettes, telemetry, fallback, and history keys.
- **CLOTH**: `test_rt_cloth3d`, including extreme inputs, degenerate colliders,
  binding, fixed-step catch-up, and finite-state repair.
- **NAV**: `test_rt_navmesh_blend`, including degenerate geometry, exact strings,
  transactional metadata, import/export, scene baking, and indexed queries.
- **SCENE**: `test_rt_scene3d`, including deterministic skeletal VSCN output,
  strict JSON numeric types, exact names, bind-pose reconstruction, and malformed
  key rejection.
- **BUILD**: supported macOS build script with `ZANNA_SKIP_CLEAN=1` and
  warnings-as-errors enabled.
- **STATIC**: exhaustive Graphics3D `cppcheck`, platform-policy lint, and
  `git diff --check`.
- **G3D**: the complete CTest `graphics3d` label.

## 3. Corrected issue ledger

### Skeleton3D, Animation3D, and math

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3CORE-001 | Ownership | Skeleton operations trusted the mutable bone-table mirror, permitting reads or writes through foreign memory after private corruption. | Added a stable owned bone identity and restore the mirror at checked boundaries. | SKEL |
| G3CORE-002 | Correctness | Mutable bone count/capacity values could expose uninitialized records or overrun the allocation. | Derive live bone state from initialized count and owned capacity, bounded to 256. | SKEL, STATIC |
| G3CORE-003 | Ownership | Alias lookup/growth trusted mutable alias pointer/count/capacity fields. | Added independent alias ownership, capacity, and initialized-count authority. | SKEL |
| G3CORE-004 | Correctness | Corrupt fixed-size alias strings could be unterminated before comparison. | Repair both alias name tails to NUL before use. | SKEL |
| G3CORE-005 | Ownership | Skeleton finalization freed mutable mirrors and walked a corrupt logical prefix. | Release initialized names and free only owned bone/alias allocations. | SKEL |
| G3CORE-006 | Correctness | Bone-name equality, C-string matching, and getters could dereference wrong-class runtime handles. | Validate every runtime string and clear invalid stored name slots. | SKEL, CTRL |
| G3CORE-007 | Ownership | Bone growth reallocated a mutable mirror and did not maintain independent initialized authority. | Grow the owned table and publish capacity/count metadata only after success. | SKEL |
| G3CORE-008 | Ownership | Animation operations trusted the mutable channel-table mirror and its public count/capacity. | Added stable channel ownership and initialized/capacity repair. | SKEL |
| G3CORE-009 | Ownership | Each channel independently trusted mutable keyframe pointer/count/capacity fields. | Added per-channel owned key storage and initialized/capacity authority. | SKEL |
| G3CORE-010 | Ownership | Animation finalization could foreign-free mirrors, leak the actual allocations, or miss hidden initialized channels. | Walk initialized owned channels and free only owned key/channel storage. | SKEL |
| G3CORE-011 | Correctness | `Animation3D.New` treated a wrong-class name as a runtime string. | Copy a name only after runtime-string validation; otherwise use the empty name. | SKEL |
| G3CORE-012 | Ownership | A new channel was counted before its first key allocation, leaving a published empty/zombie record on failure. | Allocate the initial key table before publishing the channel count. | SKEL, BUILD |
| G3CORE-013 | Correctness | Grown channel suffix records were uninitialized and later repair/finalization could interpret garbage ownership. | Zero every newly allocated channel-capacity suffix. | SKEL, STATIC |
| G3CORE-014 | Ownership | Key growth reallocated the mutable key mirror and could lose the real allocation after corruption. | Reallocate the owned identity, zero its suffix, then republish mirrors. | SKEL |
| G3CORE-015 | Numeric | Keyframe quaternion normalization squared large floats directly, overflowing valid rotations to identity. | Normalize with max-component scaling and double `hypot`. | SKEL, SCENE |
| G3CORE-016 | Numeric | Matrix multiplication accumulated in float, producing infinity/NaN from representable inputs. | Accumulate in double and publish finite saturated float lanes. | SKEL |
| G3CORE-017 | Numeric | `dt * speed` overflow reset looping clocks or produced the wrong endpoint for one-shot clips. | Reduce looping elapsed time before multiplication and compare one-shot endpoint time before multiplying. | SKEL |
| G3CORE-018 | Correctness | Matrix inversion rejected valid invertible transforms solely because `abs(det) < 1e-12`. | Reject only an exact zero or non-finite determinant. | SKEL |
| G3CORE-019 | Numeric | Float cofactor/determinant arithmetic overflowed or lost precision for extreme matrices. | Compute inversion in double and require every result to fit float. | SKEL |
| G3CORE-020 | Correctness | A failed in-place inversion could partially overwrite an aliased input/output matrix. | Stage all sixteen float results and copy only after complete validation. | SKEL |
| G3CORE-021 | Correctness | SLERP assumed normalized finite endpoints, so malformed clips yielded invalid arcs. | Normalize both inputs robustly before dot/angle interpolation and normalize the result. | SKEL |
| G3CORE-022 | Numeric | Matrix-to-quaternion extraction could take a negative square root or divide by a zero/NaN branch denominator. | Clamp radicands and fall back to identity for every degenerate denominator. | SKEL |
| G3CORE-023 | Numeric | TRS decomposition computed basis lengths with overflow-prone float sums. | Use double `hypot` norms and bounded narrowing. | SKEL |
| G3CORE-024 | Correctness | TRS decomposition discarded reflection handedness and returned only positive scales. | Detect basis determinant sign and carry reflection on one scale axis. | SKEL |
| G3CORE-025 | Correctness | Compression narrowed negative, infinite, or NaN tolerances directly to float. | Map invalid/negative tolerances to zero and saturate oversized finite tolerances. | SKEL |
| G3CORE-026 | Correctness | Compression could remove keys around non-finite or unordered key times. | Refuse redundancy when any participating time/span is invalid. | SKEL |
| G3CORE-027 | Correctness | Compression compared absent sparse TRS lanes, allowing uninitialized fallback bytes to drive key removal. | Compare only lanes selected by the matching presence masks. | SKEL |
| G3CORE-028 | Correctness | Non-finite position, scale, rotation, or interpolated lanes could be considered within tolerance. | Require finite sampled/source lanes and a finite quaternion dot before removal. | SKEL |
| G3CORE-029 | Correctness | Tangent assignment did not validate the bone index before scanning channels. | Enforce the complete supported bone-index range first. | SKEL |
| G3CORE-030 | Correctness | NaN/infinite Hermite tangent lanes were copied into playback. | Sanitize every position, rotation, and scale tangent lane to finite zero. | SKEL, SCENE |
| G3CORE-031 | Correctness | A tangent pair marked a component cubic even when that key lacked part of the component. | Enable cubic interpolation only for complete corresponding presence masks. | SKEL |
| G3CORE-032 | Correctness | Retarget role/name inference dereferenced corrupt bone-name handles. | Validate source/destination runtime strings and use deterministic empty fallbacks. | SKEL |
| G3CORE-033 | Numeric | Retarget proportional length used overflow-prone float squared sums. | Measure bind translation with double `hypot` and bounded narrowing. | SKEL |
| G3CORE-034 | Ownership | Retargeting published destination channels before allocation and copied invalid times/masks/TRS/tangents verbatim. | Stage key ownership first, canonicalize time/masks/cubic state, sanitize lanes, then publish. | SKEL |
| G3CORE-035 | Correctness | Proportional retargeting scaled translation values but not translation Hermite tangents, changing the curve shape. | Scale incoming and outgoing translation tangents by the same bone-length ratio. | SKEL |

### AnimPlayer3D and AnimBlend3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3CORE-036 | Ownership | Player skeleton/current/outgoing references were mutable mirrors used for release and evaluation. | Added stable retained identities and republish repaired mirrors. | SKEL, DRAW |
| G3CORE-037 | Ownership | Player finalization could release wrong pointers and free foreign pose mirrors. | Release/free only the stable reference and allocation identities. | SKEL |
| G3CORE-038 | Ownership | Five independently mutable player pose pointers could diverge from their allocations. | Added owned current/previous/snapshot/local/global identities. | SKEL |
| G3CORE-039 | Correctness | Mutable pose capacity could exceed one or more player buffers. | Track one authoritative common owned capacity. | SKEL, STATIC |
| G3CORE-040 | Ownership | Sequential `realloc` growth could leave a partially enlarged player after a later allocation failure. | Allocate all five replacements, copy, and publish atomically. | SKEL |
| G3CORE-041 | Correctness | A missing player buffer was ignored whenever numeric capacity was already large enough. | Require every authoritative buffer before the capacity fast path and rebuild missing storage. | SKEL, DRAW |
| G3CORE-042 | Correctness | Reconstructing missing previous/snapshot buffers retained stale temporal-history flags. | Reset motion history whenever either temporal buffer is reconstructed. | DRAW |
| G3CORE-043 | Correctness | Crossfade channel lookup used first-wins while ordinary evaluation used last-wins for duplicate corrupt channels. | Make both sides of crossfade mapping deterministic last-wins. | SKEL |
| G3CORE-044 | Correctness | A one-shot target reaching its endpoint stopped all updates before an active fade completed. | Continue advancing/evaluating the outgoing fade after target playback stops. | SKEL |
| G3CORE-045 | Correctness | Losing the active clip could leave an orphan outgoing reference and stale pose. | Clear the outgoing owner, stop playback, and restore bind pose. | SKEL |
| G3CORE-046 | Numeric | Current, outgoing, and fade clocks used overflow-prone additions/products. | Route clip clocks through bounded advancement and complete fades by remaining-time comparison. | SKEL |
| G3CORE-047 | Correctness | Player speed/time getters hid corrupt state without repairing it for later calls. | Persist canonical speed, duration, and bounded one-shot time. | SKEL |
| G3CORE-048 | Correctness | Frame serial zero doubled as “motion history uninitialized,” losing legitimate frame-zero history. | Track initialization with an explicit flag. | DRAW |
| G3CORE-049 | Correctness | Bone-matrix retrieval indexed globals without first repairing/growing pose capacity. | Ensure authoritative capacity and reject unavailable indexes before access. | SKEL |
| G3CORE-050 | Ownership | Blend skeleton and state clips were released/evaluated through mutable mirrors. | Added stable skeleton and per-state clip identities. | SKEL |
| G3CORE-051 | Correctness | Blend state count, names, and looping flags could remain corrupt after construction. | Restore initialized count, force name termination, and canonicalize flags. | SKEL |
| G3CORE-052 | Ownership | Seven blend pose/accumulator pointers could independently target foreign memory. | Added owned identities for palettes, snapshot, locals, scratch, accumulators, and globals. | SKEL |
| G3CORE-053 | Ownership | Sequential blend-buffer growth could publish a partially resized seven-buffer set. | Stage all seven allocations and replace them as one transaction. | SKEL |
| G3CORE-054 | Correctness | Missing blend buffers were skipped at sufficient numeric capacity, including temporal storage. | Require all buffers, preserve available data, and reset history when temporal buffers are rebuilt. | SKEL, DRAW |
| G3CORE-055 | Correctness | Non-finite/out-of-range retained blend weights were only sanitized in return values. | Persist weights in `[0,1]` before evaluation and getters. | SKEL |
| G3CORE-056 | Numeric | Float total-weight accumulation lost precision or overflowed with corrupt state. | Accumulate in double and compute a bounded incremental blend factor. | SKEL |
| G3CORE-057 | Numeric | Per-state blend clocks overflowed under extreme elapsed time/speed. | Use the shared bounded clock advancement helper. | SKEL |
| G3CORE-058 | Correctness | Blend motion history also treated frame zero as uninitialized. | Added an explicit blend-history initialization flag. | DRAW |
| G3CORE-059 | Performance | Repeating an identical weight or speed assignment performed needless state writes. | Compare canonical values and leave identical state untouched. | SKEL |

### Mesh and Canvas skinning

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3CORE-060 | Correctness | Mesh skeleton binding used corrupt mesh geometry state and raw skeleton bone counts. | Repair mesh counts and derive the palette bound from safe skeleton authority. | DRAW |
| G3CORE-061 | Performance | Rebinding the same skeleton/count advanced geometry revision and invalidated caches. | Touch geometry only when the retained binding or derived count changes. | DRAW |
| G3CORE-062 | Correctness | Corrupt partitioned-mesh bone counts survived skeleton binding. | Clamp partition-local palette counts to `[0,256]`. | DRAW |
| G3CORE-063 | Numeric | Summing four `DBL_MAX` weights overflowed and erased otherwise valid influences. | Normalize by the maximum weight and sum scaled ratios. | DRAW |
| G3CORE-064 | Numeric | Weights narrowed to float before normalization, turning large finite doubles into infinity. | Keep normalization in double and narrow only finite normalized results. | DRAW |
| G3CORE-065 | Correctness | Removing a former maximum influence could leave a stale derived palette count. | Recompute the mesh-wide maximum when the old high-water influence disappears. | DRAW |
| G3CORE-066 | Performance | Repeating identical normalized weights always dirtied geometry and GPU caches. | Compare final indices/weights/count and skip the revision when unchanged. | DRAW |
| G3CORE-067 | Correctness | An invalid partition bone-map entry silently sampled skeleton bone zero. | Gather an identity transform for invalid current and previous mappings. | DRAW |
| G3CORE-068 | Correctness | Negative/corrupt GPU skinning counters wrapped or continued from invalid state. | Repair to zero and update draw/upload telemetry with saturation. | DRAW |
| G3CORE-069 | Numeric | Palette upload-byte accounting could overflow signed `int64_t`. | Use checked `size_t` arithmetic and saturate the public counter. | DRAW, STATIC |
| G3CORE-070 | Correctness | Controller drawing ignored the controller-reported palette length and used the skeleton count, risking palette overread. | Bound the submission to both reported palette and safe skeleton counts. | DRAW |
| G3CORE-071 | Correctness | Per-instance palette lane/byte products could wrap before frame allocation. | Check every multiplication before forming snapshot sizes. | DRAW, STATIC |
| G3CORE-072 | Correctness | A failed current-palette snapshot aborted instead of taking the supported CPU path; a failed previous snapshot was still charged as uploaded. | Fall back safely and count previous bytes only when the snapshot exists. | DRAW |
| G3CORE-073 | Correctness | Instanced telemetry counted accepted calls even when the queue legitimately dropped a draw. | Compare deferred draw count around each chunk and record only actual queued chunks/bytes. | DRAW |
| G3CORE-074 | Correctness | CPU per-instance fallback keyed motion history by palette address, causing collisions and allocator-dependent continuity. | Use a stable mesh/material/batch/index motion key. | DRAW |
| G3CORE-075 | Performance | Crowd palette packing allocated and freed two large heap blocks every draw. | Allocate both blocks from the frame arena. | DRAW |
| G3CORE-076 | Correctness | Crowd palettes were validated in a first pass but copied later without checking that the live count still matched. | Revalidate each current palette during packing and reject shape changes. | DRAW |
| G3CORE-077 | Correctness | Previous controller palettes were copied without validating their reported bone count. | Copy previous data only when its exact count matches the batch. | DRAW |

### Cloth3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3CORE-078 | Ownership | Position, previous-position, pin, pin-position, and constraint mirrors were trusted as allocation identities. | Added stable owned identities and restore all hot-path mirrors centrally. | CLOTH |
| G3CORE-079 | Correctness | Mutable point/constraint counts and patch dimensions could overrun arrays or reinterpret topology. | Preserve allocated counts and immutable topology dimensions as repair authority. | CLOTH, STATIC |
| G3CORE-080 | Correctness | Collider count and type flags could be corrupt, exposing uninitialized fixed slots. | Track initialized collider count, bound it to 16, and canonicalize every type. | CLOTH |
| G3CORE-081 | Ownership | Mesh and animator bindings were retained/released through mutable mirrors. | Added stable typed owner slots and republish validated mirrors. | CLOTH |
| G3CORE-082 | Ownership | Override masks/globals and their skeleton count could diverge independently. | Added owned override identities and one allocated skeleton-bone authority. | CLOTH |
| G3CORE-083 | Ownership | Cloth finalization could free foreign mirrors or release wrong-class bindings. | Free/release only owned arrays and typed retained identities. | CLOTH |
| G3CORE-084 | Ownership | Construction published the runtime object before five fallible allocations, complicating rollback and leaking partial state. | Allocate all arrays first, then construct/publish the object. | CLOTH, BUILD |
| G3CORE-085 | Correctness | Allocation byte products and impossible point/constraint counts were not checked together. | Validate limits and `SIZE_MAX` products before allocation. | CLOTH, STATIC |
| G3CORE-086 | Numeric | Chain length and patch extents accepted finite values beyond the common coordinate range. | Reject dimensions outside the bounded Game3D coordinate contract. | CLOTH |
| G3CORE-087 | Numeric | Patch coordinates accumulated `step * index` rounding and diagonal rest length used overflow-prone squaring. | Generate endpoint-exact ratios and use `hypot` for the diagonal. | CLOTH |
| G3CORE-088 | Numeric | Unbounded retained or public gravity scale could overflow acceleration. | Repair and clamp gravity scale to `[-1000000,1000000]`. | CLOTH |
| G3CORE-089 | Numeric | Unbounded wind response destabilized the fixed-step integrator. | Repair and clamp the non-negative coefficient to 120. | CLOTH |
| G3CORE-090 | Numeric | Wind direction-times-strength could overflow despite individually finite inputs. | Clamp strength, multiply in double, and coordinate-clamp every resulting lane. | CLOTH |
| G3CORE-091 | Correctness | `GetPoint` returned non-finite corrupt simulation state to managed callers. | Repair current and previous lanes using pin/origin fallbacks before returning. | CLOTH |
| G3CORE-092 | Correctness | Retained collider endpoints/radii could bypass public setter validation. | Repair endpoint lanes, radius range, and degenerate capsule state at checked boundaries. | CLOTH |
| G3CORE-093 | Correctness | Mesh binding cleared/appended before proving point values fit vertex floats or enough capacity existed. | Validate all points and reserve complete vertex/index capacity first. | CLOTH |
| G3CORE-094 | Ownership | Mesh binding retained the new mesh before construction finished, replacing a valid old binding on failure. | Publish the retained mesh only after exact topology construction succeeds. | CLOTH |
| G3CORE-095 | Performance | Building a patch mesh advanced geometry revision once per vertex/triangle. | Enclose construction in one geometry batch. | CLOTH |
| G3CORE-096 | Correctness | Bone-chain binding accepted wrong-class animator/name handles before runtime access. | Validate AnimController3D and runtime-string classes explicitly. | CLOTH |
| G3CORE-097 | Correctness | Chains beyond 32 bones were silently truncated into a different binding. | Detect and reject the oversized chain exactly. | CLOTH |
| G3CORE-098 | Correctness | Branch detection mutated the live chain prefix before failure, destroying a prior valid binding. | Discover bones/rest lengths in staged local arrays. | CLOTH |
| G3CORE-099 | Ownership | Override allocation freed the old buffers before both replacements succeeded. | Allocate both replacements first and swap only after complete success. | CLOTH |
| G3CORE-100 | Numeric | Bone-chain rest lengths used overflow-prone squared translation and trusted invalid prior rests. | Use robust length and finite positive fallback rest values. | CLOTH |
| G3CORE-101 | Correctness | A corrupt Verlet point could enter integration with NaN/infinite state or a noncanonical pin flag. | Repair every point before arithmetic. | CLOTH |
| G3CORE-102 | Numeric | Wind integration reconstructed velocity with division by `dt`, multiplying it back immediately and increasing overflow/rounding risk. | Use the algebraically equivalent displacement form with precomputed coefficients. | CLOTH |
| G3CORE-103 | Correctness | Constraint endpoints/rest lengths were trusted and correction used overflow-prone vector math. | Validate endpoints/rest and apply robust normalized correction with bounded writes. | CLOTH |
| G3CORE-104 | Numeric | Capsule closest-point projection squared a long axis and could overflow to a wrong parameter. | Normalize the axis with `hypot` and project in distance units. | CLOTH |
| G3CORE-105 | Numeric | Collision distance/push-out squared large deltas and multiplied by `radius / distance`. | Use robust lengths and normalized-direction radius push-out. | CLOTH |
| G3CORE-106 | Correctness | Re-pinning copied corrupt pin coordinates directly into both Verlet states. | Canonicalize pin coordinates before synchronizing current/previous state. | CLOTH |
| G3CORE-107 | Numeric | Bone override quaternion creation assumed unit directions and multiplication could drift or overflow. | Normalize directions/quaternions robustly, handle antiparallel degeneracy, and normalize products. | CLOTH |
| G3CORE-108 | Correctness | Initial animator anchoring translated the cloth only when the first offset exceeded a teleport threshold. | Treat the first successful anchor sync as a required rigid translation. | CLOTH |
| G3CORE-109 | Correctness | Override writing trusted chain bone indexes and narrowed non-finite matrices to float. | Bound indexes and sanitize/saturate every output matrix lane. | CLOTH |
| G3CORE-110 | Correctness | Mesh output trusted topology/counts and formed normals from unbounded tangents. | Revalidate topology/counts, normalize tangent directions first, and publish finite normals. | CLOTH |
| G3CORE-111 | Performance | An unchanged cloth mesh advanced geometry revision every stepped frame. | Compare generated position/normal/positions64 lanes and touch only on change. | CLOTH |
| G3CORE-112 | Numeric | Huge `dt` overflowed the accumulator and catch-up discarded all fractional time. | Cap work at eight steps, reduce huge time with `fmod`, and retain only the fractional remainder. | CLOTH |
| G3CORE-113 | Correctness | Corrupt zero, negative, non-finite, or implausibly large substep/accumulator state could stall or spin simulation. | Restore the 1/120 default and a finite non-negative accumulator. | CLOTH |
| G3CORE-114 | Correctness | World cloth removal left a stale pointer in the now-unused tail slot. | Clear the vacated tail after compaction. | CLOTH |

### NavMesh3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3CORE-115 | Correctness | Query-grid fallback telemetry could wrap negative and continue overflowing. | Repair negative state and saturate at `INT64_MAX`. | NAV |
| G3CORE-116 | Ownership | Finalization unreferenced corrupt area/link-kind slots as if they were runtime strings. | Validate string handles before release. | NAV |
| G3CORE-117 | Numeric | Portal-width tests used overflow-prone squared planar deltas. | Measure width with double `hypot`. | NAV |
| G3CORE-118 | Serialization | Navmesh string export used `strlen`, truncating runtime strings at embedded NUL. | Serialize the exact runtime-string byte length. | NAV |
| G3CORE-119 | Correctness | Area interning used `strcmp`, conflating distinct exact strings sharing a prefix before NUL. | Compare validated length plus `memcmp`. | NAV |
| G3CORE-120 | Ownership | Invalid existing area slots or a failed retain could be dereferenced/published. | Validate every table handle and retain before incrementing the published count. | NAV |
| G3CORE-121 | Correctness | Triangle traversal-cost lookup trusted raw mutable triangle count. | Use the bounded safe triangle-count reader. | NAV |
| G3CORE-122 | Correctness | Tile size and `floor` results could be non-finite/out of integer range before cast. | Enforce tile bounds and a finite explicit tile-index range before conversion. | NAV |
| G3CORE-123 | Correctness | Navmesh builds ignored authoritative Mesh3D `positions64` and silently sanitized non-finite positions. | Read the authoritative source and reject any non-finite vertex. | NAV |
| G3CORE-124 | Correctness | Scene-bake flattening trusted raw mesh vertex/index counts and unchecked aggregate additions. | Repair both meshes and check all count additions. | NAV |
| G3CORE-125 | Correctness | Non-finite node world-matrix lanes were replaced with zero, inventing geometry. | Reject the node append before mutation. | NAV |
| G3CORE-126 | Correctness | Scene-bake normals used the direct linear basis, wrong under non-uniform scale and reflection. | Apply the cofactor inverse-transpose and preserve authored orientation under reflection. | NAV |
| G3CORE-127 | Performance | Scene flattening repeatedly grew and dirtied the destination for every primitive. | Reserve the complete append and wrap it in one geometry batch. | NAV |
| G3CORE-128 | Correctness | Direct navmesh build trusted mutable Mesh3D counts and indexes. | Repair/bound counts and validate indexes against the safe vertex prefix. | NAV |
| G3CORE-129 | Numeric | Face cross products and centroid sums occurred in float, overflowing large valid geometry. | Compute geometry in double and narrow normalized results. | NAV |
| G3CORE-130 | Correctness | An all-degenerate input returned a non-null empty navmesh. | Reject builds with no valid source triangles. | NAV |
| G3CORE-131 | Correctness | Triangle recomputation converted a degenerate edit into an invented up-facing normal. | Reject non-finite or zero-area recomputation. | NAV |
| G3CORE-132 | Serialization | Export trusted corrupt vertex, area-name, triangle-index, area-id, and cost fields. | Validate every referenced table/record before writing. | NAV |
| G3CORE-133 | Serialization | Export emitted invalid configuration, link, and obstacle scalars/order. | Sanitize bounded configuration and reject malformed links/obstacles. | NAV |
| G3CORE-134 | Correctness | V1 import omitted the preserved source-triangle table required by later slope/tile edits. | Clone imported triangles into an editable source baseline. | NAV |
| G3CORE-135 | Serialization | V2 import accepted empty or duplicate exact area names, making IDs ambiguous. | Reject empty and length-exact duplicate entries. | NAV |
| G3CORE-136 | Serialization | V2 import treated embedded-NUL link kinds as empty and preserved inverted obstacle bounds. | Use exact length for kinds and canonicalize each obstacle min/max lane. | NAV |
| G3CORE-137 | Ownership | Replacing off-mesh metadata released the old kind before retaining the new one, causing same-handle use-after-free. | Retain the replacement first, then release the prior slot. | NAV |
| G3CORE-138 | Performance | `SetArea` interned a new name even when the bounds touched no source triangle. | Probe overlap first and return without table mutation. | NAV |
| G3CORE-139 | Numeric | Off-mesh endpoint matching squared deltas without finite/component gates. | Apply finite component gates and double `hypot` distance. | NAV |
| G3CORE-140 | Performance | Setting an identical maximum slope rebuilt adjacency/index state needlessly. | Return before rebuild when the canonical value is unchanged. | NAV |
| G3CORE-141 | Correctness | Debug drawing trusted raw triangle counts and vertex indexes. | Use the safe count and skip malformed records. | NAV |

### VSCN skeletal persistence

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3CORE-142 | Serialization | Required skeleton matrix arrays used a permissive numeric reader that could coerce booleans/default malformed values. | Require exact finite integer/float JSON elements with no bool coercion. | SCENE |
| G3CORE-143 | Correctness | Loader-created bones bypassed `Skeleton3D.AddBone`, leaving cached bind translation/rotation/scale uninitialized. | Construct through the runtime insertion path, then restore serialized parent/inverse data. | SCENE, SKEL |
| G3CORE-144 | Ownership | Direct skeleton allocation in the loader omitted the new ownership/initialized metadata. | Let standard insertion establish all allocation authority. | SCENE |
| G3CORE-145 | Serialization | Loader bone names were converted through C strings and truncated at embedded NUL. | Preserve parsed runtime-string handles and exact byte lengths. | SCENE |
| G3CORE-146 | Correctness | Parent indexes and bind/inverse lanes could be out of range or unrepresentable in runtime float storage. | Validate parent domain, finiteness, and float range before publication. | SCENE |
| G3CORE-147 | Serialization | Skeletal animation duration accepted missing, boolean, non-positive, non-finite, or oversized values through defaults/coercion. | Require an exact positive finite duration fitting float. | SCENE |
| G3CORE-148 | Correctness | Loader accepted zero-key channels that playback/persistence could not represent consistently. | Require a positive bounded key count. | SCENE |
| G3CORE-149 | Correctness | Multiple loaded channels could target the same bone with ambiguous winner semantics. | Reject duplicate bone channels. | SCENE |
| G3CORE-150 | Serialization | Base64 decode length used C-string `strlen`, truncating parsed strings containing embedded NUL. | Decode using the parser-reported exact byte length. | SCENE |
| G3CORE-151 | Correctness | Decoded key times could be NaN, negative, out of float range, duplicate, or decreasing, breaking binary search/interpolation. | Require finite non-negative strictly increasing bounded times. | SCENE |
| G3CORE-152 | Correctness | Unknown/partial presence-mask bits entered sparse TRS evaluation. | Enforce defined position/scale bits and either absent or complete rotation. | SCENE |
| G3CORE-153 | Correctness | Cubic flags were accepted for incomplete TRS components. | Require each cubic bit's complete corresponding presence mask. | SCENE |
| G3CORE-154 | Correctness | Non-finite decoded position, scale, or rotation lanes reached playback. | Validate every stored TRS lane before ownership publication. | SCENE |
| G3CORE-155 | Correctness | Non-finite decoded Hermite tangents reached cubic evaluation. | Validate all position/rotation/scale tangent lanes. | SCENE |
| G3CORE-156 | Numeric | Decoded rotations were not robustly normalized and large finite quaternions could overflow. | Reject degenerate rotations and normalize with max scaling plus `hypot`. | SCENE |
| G3CORE-157 | Ownership | Loader-created animation channel/key arrays omitted owned pointers, capacities, and initialized counts. | Publish complete ownership metadata for every decoded allocation. | SCENE, SKEL |
| G3CORE-158 | Serialization | Metadata output re-derived stored string size with `strlen` instead of honoring its validated native length. | Escape the exact recorded bytes. | SCENE |
| G3CORE-159 | Serialization | Skeleton saving emitted invalid name handles, parents, or non-finite bind/inverse matrices. | Validate the complete skeleton record before writing. | SCENE |
| G3CORE-160 | Serialization | Skeleton saving truncated valid runtime names at embedded NUL. | Escape the exact runtime-string length. | SCENE |
| G3CORE-161 | Serialization | Animation saving emitted invalid duration/name/channel index or duplicate bone channels. | Validate clip header and channel uniqueness/range before encoding. | SCENE |
| G3CORE-162 | Serialization | Saver copied raw keyframe arrays without mirroring loader invariants, allowing invalid times, masks, lanes, tangents, and rotations. | Validate every member against the same strict wire contract. | SCENE |
| G3CORE-163 | Correctness | Raw struct encoding included uninitialized padding, leaking bytes and making identical saves nondeterministic. | Copy members into zero-initialized wire structs before Base64 encoding. | SCENE |

The audit corrected **163** distinct issues: 35 in Skeleton3D/Animation3D/math,
24 in player/blend playback, 18 in skinning, 37 in Cloth3D, 27 in
NavMesh3D, and 22 in VSCN persistence. Nine findings are explicit hot-path
performance/cache issues; the remainder close correctness, ownership, numeric,
serialization, and lifecycle failures that also carried secondary performance
or security costs.

## 4. Public behavior documentation

The affected public headers now state the observable stability contracts:

- animation compression canonicalizes invalid tolerances; tangent input is
  finite-sanitized and only enables cubic interpolation for complete components;
- repeated skeleton binding and normalized weight assignments are idempotent,
  and extreme finite weights normalize without overflow;
- Cloth3D gravity, wind response, and wind strength have explicit stability
  bounds, while large elapsed time is capped to eight fixed substeps with a
  retained fractional remainder.

These are compatibility-preserving clarifications of existing calls. No new
runtime surface or persistence version was introduced.

## 5. Validation

Every build was incremental because concurrent work was active in the checkout.
The supported script was always invoked with `ZANNA_SKIP_CLEAN=1`; no clean full
build was used.

```sh
ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 ZANNA_SKIP_LINT=1 \
  ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 ZANNA_SKIP_INSTALL=1 \
  ./scripts/build_zanna_mac.sh

ctest --test-dir build \
  -R '^(test_rt_skeleton3d|test_rt_animcontroller3d|test_rt_canvas3d|test_rt_canvas3d_gpu_paths|test_rt_cloth3d|test_rt_navmesh_blend|test_rt_scene3d)$' \
  --output-on-failure -j4

ctest --test-dir build -L graphics3d --output-on-failure -j4

cppcheck --project=build/compile_commands.json \
  --file-filter='*/src/runtime/graphics/3d/*' \
  --enable=warning,performance,portability --check-level=exhaustive \
  --error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem

./scripts/lint_platform_policy.sh
./scripts/run_cross_platform_smoke.sh
git diff --check

ZANNA_SKIP_CLEAN=1 ./scripts/build_zanna_mac.sh
```

Closeout results:

- incremental macOS warnings-as-errors build: passed;
- focused animation/skinning/cloth/navigation/VSCN tests: passed;
- complete Graphics3D label: passed;
- exhaustive Graphics3D static analysis: passed with no findings;
- platform-policy lint, cross-platform smoke, and whitespace validation: passed;
- final supported incremental build/test pipeline: passed.

## 6. Related records

- [Graphics3D Animation and Navigation Runtime Audit (2026-08)](graphics3d-animation-navigation-audit-2026-08.md)
- [Graphics3D Runtime Correctness Audit Follow-up (2026-08)](graphics3d-runtime-audit-followup-2026-08.md)
- [Graphics3D Runtime Correctness Audit (2026-08)](graphics3d-runtime-audit-2026-08.md)
- [Graphics3D Runtime Hardening Program (2026-07)](graphics3d-runtime-hardening-2026-07.md)
