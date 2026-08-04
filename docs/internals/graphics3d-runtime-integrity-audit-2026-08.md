---
status: completed
audience: contributors
last-verified: 2026-08-03
---

# Graphics3D Runtime Integrity Audit (2026-08-03)

## 1. Scope and method

This audit reviewed all 279 files under `src/runtime/graphics/3d` (277 C,
include-fragment, and header files; 289,710 lines at review time). The compile
database contained 107 Graphics3D compilation entries covering 104 unique
production C translation units. Earlier Graphics3D ledgers were read first;
their closed findings are baseline and are not recounted here.

The review combined line-by-line ownership and arithmetic analysis, searches
for allocation publication and parallel-array growth, temporal-hash invariant
tracing, weak-reference lifetime analysis, hot-loop allocation review,
adversarial private-state tests, Clang static analysis, exhaustive `cppcheck`,
warnings-as-errors compilation, and focused plus label-wide runtime tests. The
ledger records **185 newly corrected bugs, performance problems, and correctness
hazards**. Entries stay separate where they affect a distinct field, allocation,
state transition, API result, ownership event, or hot-path cost.

No runtime registry row, public function signature, IL contract, opcode,
grammar, verifier rule, workflow, or production platform dependency changed.
The Canvas3D hash population counter and Behavior3D navigation cache are private
implementation state. The Path3D raw evaluator was already an internal API.
Consequently, repository policy does not require an ADR for this patch.

## 2. Evidence key

- **BHV**: `test_rt_game3d`, including physical path speed and extreme finite
  behavior state.
- **CANVAS**: `test_rt_canvas3d` and `test_rt_canvas3d_gpu_paths`, including
  corrupt transient collections, duplicate ownership, motion aliasing, hash
  growth/corruption, finite matrices, and draw rollback.
- **PHYS**: `test_rt_physics3d`, including candidate-list repair, actual zeroing
  weak trigger occupants, destroyed bodies, and corrupt trigger bounds.
- **ASSET**: `test_rt_gltf`, `test_rt_gltf_draco_internal`, and the Graphics3D
  texture/FBX/model tests.
- **PART**: `test_rt_particles3d_contract`.
- **BUILD**: supported incremental macOS build with warnings as errors and
  `ZANNA_SKIP_CLEAN=1`.
- **STATIC**: Clang analyzer, exhaustive Graphics3D `cppcheck`, platform-policy
  lint, and `git diff --check`.
- **G3D**: complete CTest `graphics3d` label.
- **SWPERF**: Ridgebound's 960x540 cinematic software-render probe, including
  its image-quality metrics and 5,000 ms render budget.

## 3. Corrected issue ledger

### Behavior3D movement, numerics, and hot loops

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3I-001 | Numeric | Spin-axis normalization squared extreme finite lanes and overflowed. | Normalize with nested `hypot`. | BHV, STATIC |
| G3I-002 | Correctness | Overflowed spin normalization could publish a zero axis as valid. | Require a finite nonzero robust norm before storing the axis. | BHV |
| G3I-003 | Numeric | Finite but extreme spin rates were retained without a stability bound. | Clamp rates to the shared Game3D angular limit. | BHV |
| G3I-004 | Correctness | A corrupted nonfinite spin axis propagated into quaternion construction. | Re-normalize retained state and restore the +Y fallback. | BHV |
| G3I-005 | Numeric | A corrupted nonfinite spin phase persisted forever. | Reset invalid phases before advancing. | BHV |
| G3I-006 | Numeric | `phase + rate * dt` could overflow before `fmod`. | Bound rate/delta and advance through one finite phase helper. | BHV |
| G3I-007 | Performance | Spin allocated a Vec3 box on every simulation tick. | Build the raw quaternion directly. | BHV |
| G3I-008 | Performance | Spin allocated a Quat box on every simulation tick. | Write raw quaternion components with the node transform API. | BHV |
| G3I-009 | Performance | Face-target allocated a constant +Y Vec3 every tick. | Construct the yaw quaternion without an axis box. | BHV |
| G3I-010 | Performance | Sine motion allocated a local-position Vec3 every tick. | Read local components into stack storage. | BHV |
| G3I-011 | Performance | Path following allocated and released a position Vec3 every tick. | Use the existing raw spline evaluator. | BHV |
| G3I-012 | Correctness | FollowPath passed world distance to an API expecting normalized `t`, reaching the end almost immediately on paths longer than one unit. | Convert physical distance to normalized travel. | BHV |
| G3I-013 | Correctness | Normalized curve parameter is not normalized arc length, so the documented constant speed still varied along splines. | Evaluate through Path3D's arc-length lookup. | BHV |
| G3I-014 | Numeric | Corrupt negative/nonfinite retained path distance reached wrapping and evaluation. | Repair it before integration. | BHV |
| G3I-015 | Numeric | Path-distance integration could overflow and poison later frames. | Bound speed/delta and revalidate the sum. | BHV |
| G3I-016 | Correctness | Corrupt retained path speed bypassed constructor validation. | Reapply the shared speed bound on every update. | BHV |
| G3I-017 | Numeric | Direct Behavior3D updates accepted arbitrarily large finite `dt`. | Use the shared controller-delta clamp. | BHV |
| G3I-018 | Correctness | Unknown private flag bits survived and could affect later extensions nondeterministically. | Mask retained flags to the defined preset set. | BHV |
| G3I-019 | Correctness | Nonfinite lifetime state never reached a deterministic expiry. | Canonicalize retained lifetime before decrement. | BHV |
| G3I-020 | Numeric | Extreme initial lifetimes exceeded the supported runtime state range. | Clamp at configuration time. | BHV |
| G3I-021 | Correctness | Target world coordinates were trusted after component lookup. | Clamp all three lanes before chase/face/nav use. | BHV |
| G3I-022 | Correctness | Direct-chase current coordinates could be nonfinite. | Repair current world position before subtraction. | BHV |
| G3I-023 | Numeric | Direct chase used `sqrt(dx*dx + dz*dz)`, overflowing for large finite deltas. | Use `hypot`. | BHV, STATIC |
| G3I-024 | Correctness | Corrupt retained chase speed bypassed setter checks. | Reapply the speed bound per tick. | BHV |
| G3I-025 | Correctness | Corrupt retained stopping range bypassed setter checks. | Reapply the coordinate/range bound per tick. | BHV |
| G3I-026 | Numeric | Chase output addition could exceed the supported coordinate range. | Clamp each moved world lane. | BHV |
| G3I-027 | Correctness | Orbit-center corruption reached trigonometric placement. | Repair all retained center lanes. | BHV |
| G3I-028 | Correctness | Orbit radius and rate corruption bypassed configuration validation. | Bound both retained values on update. | BHV |
| G3I-029 | Numeric | Orbit phase/output could overflow or become nonfinite. | Use bounded phase advancement and coordinate-safe sums. | BHV |
| G3I-030 | Correctness | Sine base, amplitude, speed, and phase corruption poisoned local transforms. | Repair each retained scalar before use. | BHV |
| G3I-031 | Numeric | Sine base-plus-offset could overflow. | Clamp the resulting local height. | BHV |
| G3I-032 | Numeric | Face-target's squared-distance test overflowed for large finite deltas. | Use `hypot` for the degeneracy test. | BHV |
| G3I-033 | Performance | Nav-assisted chase rebuilt the same path every simulation tick. | Cache the last submitted target and repath only after movement/arrival. | BHV |
| G3I-034 | Performance | The redundant nav repath also allocated a target Vec3 every tick. | Allocate only when a target submission is required. | BHV |
| G3I-035 | Correctness | Changing the bound NavAgent could reuse the previous agent's target cache. | Invalidate cached nav state on rebinding. | BHV |

### Canvas transient ownership and arena state

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3I-036 | Correctness | A missing temp-buffer list with stale count/capacity could be traversed. | Normalize the pointer/count/capacity tuple before use. | CANVAS |
| G3I-037 | Correctness | Negative temp-buffer counts could index before the list. | Repair counts to zero. | CANVAS |
| G3I-038 | Correctness | Temp-buffer counts above capacity could read/free beyond the allocation. | Clamp to recorded capacity. | CANVAS |
| G3I-039 | Correctness | A negative temp-buffer capacity was repaired open, defeating resource-transaction failure. | Preserve the sentinel as fail-closed for tracking. | CANVAS |
| G3I-040 | Numeric | Temp-buffer append could overflow `count + 1`. | Reject the signed-count limit before growth. | CANVAS, STATIC |
| G3I-041 | Numeric | Temp-buffer geometric growth could overflow signed capacity. | Bound doubling and allocation size. | CANVAS, STATIC |
| G3I-042 | Ownership | The same malloc buffer could be tracked twice and freed twice. | Deduplicate by hash with a linear authoritative fallback. | CANVAS |
| G3I-043 | Correctness | A missing buffer-set allocation could retain stale nonzero capacity. | Reset capacity whenever the pointer is absent. | CANVAS |
| G3I-044 | Correctness | A non-power-of-two buffer-set capacity broke mask-based probing. | Validate supported powers of two and discard malformed tables. | CANVAS |
| G3I-045 | Numeric | Buffer-set load-factor multiplication could overflow. | Enforce the `2^30` table bound before doubling. | CANVAS, STATIC |
| G3I-046 | Correctness | Hash allocation failure could leave a stale set that falsely reported absence. | Drop the set and keep the linear list authoritative. | CANVAS |
| G3I-047 | Correctness | Swap-untracking left a stale tail pointer. | Null the vacated slot and rebuild the filter. | CANVAS |
| G3I-048 | Correctness | Temp-buffer clear trusted malformed list metadata. | Repair before iteration and clear each released slot. | CANVAS |
| G3I-049 | Correctness | A missing temp-object list with stale metadata could be traversed. | Normalize its tuple before use. | CANVAS |
| G3I-050 | Correctness | Negative temp-object count could underflow append/removal logic. | Repair it to zero. | CANVAS |
| G3I-051 | Correctness | Temp-object count above capacity could overread and release foreign memory. | Clamp it to capacity. | CANVAS |
| G3I-052 | Ownership | Repairing negative object capacity open could lose prior retains and admit a failed draw. | Fail closed without mutating existing ownership. | CANVAS |
| G3I-053 | Numeric | Temp-object append could overflow the signed count. | Reject the limit before addition. | CANVAS, STATIC |
| G3I-054 | Numeric | Temp-object growth could overflow capacity or bytes. | Check doubling and `SIZE_MAX`. | CANVAS, STATIC |
| G3I-055 | Ownership | Duplicate transient objects acquired duplicate retains/releases. | Deduplicate before retaining. | CANVAS |
| G3I-056 | Correctness | A missing object-set allocation could retain stale capacity. | Reset absent-set metadata. | CANVAS |
| G3I-057 | Correctness | Non-power-of-two object-set storage made probing unsafe. | Validate and discard malformed tables. | CANVAS |
| G3I-058 | Numeric | Object-set sizing could overflow on `count * 2`. | Apply the supported table bound first. | CANVAS, STATIC |
| G3I-059 | Correctness | Object-set growth failure could publish an unindexed retained object behind a stale table. | Drop the table and fall back to the list. | CANVAS |
| G3I-060 | Correctness | Object swap-removal retained a stale tail reference. | Clear the tail before rebuilding. | CANVAS |
| G3I-061 | Ownership | Object cleanup trusted malformed tuple metadata. | Repair, release only the bounded prefix, and clear slots. | CANVAS |
| G3I-062 | Correctness | Final-overlay temp-buffer pointer/count/capacity divergence could overrun cleanup. | Repair the tuple at track/untrack/clear boundaries. | CANVAS |
| G3I-063 | Ownership | Duplicate final-overlay buffers could be freed twice. | Reject duplicate ownership. | CANVAS |
| G3I-064 | Numeric | Final-overlay buffer growth could overflow count/capacity/bytes. | Check every bound before allocation. | CANVAS, STATIC |
| G3I-065 | Correctness | Final-overlay object tuple corruption could overrun retained slots. | Repair metadata before all operations. | CANVAS |
| G3I-066 | Ownership | Duplicate final-overlay objects could be released twice. | Deduplicate retained object slots. | CANVAS |
| G3I-067 | Ownership | Deferred overlay clearing trusted three independently corrupt collections. | Repair and null all released command/buffer/object slots. | CANVAS |
| G3I-068 | Numeric | Frame-arena alignment round-up could wrap. | Reject a zero wrapped size before allocation. | CANVAS, STATIC |
| G3I-069 | Correctness | Corrupt chunk `used > capacity` underflowed remaining-space subtraction. | Clamp every bump offset to capacity. | CANVAS |
| G3I-070 | Correctness | A foreign `frame_arena_current` pointer could be dereferenced. | Require current to belong to the retained chain. | CANVAS |
| G3I-071 | Correctness | A cycle in the arena chain caused infinite reset/free traversal. | Detect and sever cycles before traversal. | CANVAS |
| G3I-072 | Numeric | Frame-arena byte telemetry wrapped on long/extreme frames. | Saturate at `SIZE_MAX`. | CANVAS |
| G3I-073 | Correctness | A null final-overlay arena could retain stale capacity/used state and permit null pointer arithmetic. | Reset metadata whenever storage is absent. | CANVAS |
| G3I-074 | Correctness | `used > capacity` in the overlay arena underflowed availability. | Reject the allocation and preserve a bounded high-water signal. | CANVAS |
| G3I-075 | Ownership | Reset could retain a nonnull overlay allocation with zero capacity. | Release the unusable allocation and clear state. | CANVAS |
| G3I-076 | Correctness | The shared next-power helper returned a non-power value above `2^30`, invalidating every mask-based caller. | Return failure for unrepresentable signed powers and bound callers. | CANVAS, STATIC |

### Canvas motion history

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3I-077 | Correctness | If current and output matrices aliased, zeroing output erased current before it was stored. | Snapshot current before initializing outputs. | CANVAS |
| G3I-078 | Correctness | Nonfinite current matrices poisoned motion vectors and persistent history. | Reject them before lookup/update. | CANVAS |
| G3I-079 | Correctness | Nonfinite retained current matrices survived pruning. | Discard malformed entries. | CANVAS |
| G3I-080 | Correctness | Nonfinite retained previous matrices could be returned. | Clear `has_prev` unless the matrix is finite. | CANVAS |
| G3I-081 | Correctness | Missing entry storage with stale count/capacity could be indexed. | Repair the entry tuple centrally. | CANVAS |
| G3I-082 | Correctness | Negative entry count could underflow indexing. | Repair it to zero. | CANVAS |
| G3I-083 | Correctness | Entry count beyond capacity could overrun prune/lookup. | Clamp it to capacity. | CANVAS |
| G3I-084 | Correctness | Missing hash storage could retain stale capacity/population. | Reset both when the pointer is absent. | CANVAS |
| G3I-085 | Correctness | A non-power-of-two hash capacity made probing unsafe. | Validate supported capacities before masking. | CANVAS |
| G3I-086 | Numeric | Oversized hash requests could overflow signed sizing. | Enforce the `2^30` representation/load bound. | CANVAS, STATIC |
| G3I-087 | Correctness | The table had no way to detect an incomplete index. | Track the exact indexed population. | CANVAS |
| G3I-088 | Correctness | Growing the hash zeroed all old slots and inserted only the newest entry. | Rebuild every existing entry after growth. | CANVAS |
| G3I-089 | Correctness | A corrupt encoded slot could index outside history. | Validate and rebuild before access. | CANVAS |
| G3I-090 | Ownership | A new history entry was published before hash insertion succeeded. | Stage and roll back the append transaction. | CANVAS |
| G3I-091 | Correctness | Failed insertion could leave a permanently unfindable duplicate-prone entry. | Restore count and rebuild the prior index. | CANVAS |
| G3I-092 | Numeric | Count/load arithmetic could overflow while sizing the hash. | Reject unsupported counts before multiplication/addition. | CANVAS, STATIC |
| G3I-093 | Numeric | Signed frame subtraction could overflow when pruning. | Compute age with unsigned distance. | CANVAS |
| G3I-094 | Correctness | A future/corrupt frame stamp could make an entry immortal. | Rebase it to the current frame and reset previous history. | CANVAS |
| G3I-095 | Correctness | Zero-key entries could occupy history indefinitely. | Drop them during repair/prune. | CANVAS |
| G3I-096 | Correctness | Corrupt temporal-presence bytes escaped as noncanonical booleans. | Canonicalize current/previous flags. | CANVAS |
| G3I-097 | Correctness | Invalid instance counts still produced shared nonzero motion keys. | Return zero for invalid ranges. | CANVAS |
| G3I-098 | Correctness | Out-of-range instance indices aliased valid history identities. | Reject negative and `index >= count`. | CANVAS |

### Character3D and Trigger3D

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3I-099 | Correctness | Missing character shortlist storage could retain active stale metadata. | Repair pointer/count/capacity/active together. | PHYS |
| G3I-100 | Correctness | Negative shortlist capacity/count could underflow traversal. | Normalize both before use. | PHYS |
| G3I-101 | Correctness | Candidate count above capacity could overrun the allocation. | Clamp it to capacity. | PHYS |
| G3I-102 | Correctness | Noncanonical active state could select an unusable shortlist. | Canonicalize and disable it when storage is absent. | PHYS |
| G3I-103 | Numeric | Candidate growth omitted the pointer-array byte overflow check. | Validate against `SIZE_MAX`. | PHYS, STATIC |
| G3I-104 | Numeric | Candidate append could overflow `count + 1`. | Reject `INT32_MAX` before addition. | PHYS, STATIC |
| G3I-105 | Correctness | Shortlist construction dereferenced a null start point. | Validate the input first. | PHYS |
| G3I-106 | Numeric | Nonfinite/negative move length produced invalid query bounds. | Reject malformed travel. | PHYS |
| G3I-107 | Numeric | Query-min subtraction could overflow supported coordinates. | Saturate each lane. | PHYS |
| G3I-108 | Numeric | Query-max addition could overflow supported coordinates. | Saturate each lane. | PHYS |
| G3I-109 | Correctness | Fallback character collision scans trusted world body count beyond capacity. | Bound count to available storage. | PHYS |
| G3I-110 | Numeric | Character height accepted arbitrarily large finite capsules. | Cap height to the supported physics range. | PHYS |
| G3I-111 | Correctness | Nonfinite/oversized radius made height constraints invalid. | Validate radius before resizing. | PHYS |
| G3I-112 | Correctness | Height getter could return NaN/infinity or corrupt extremes. | Repair from body fallback and clamp. | PHYS |
| G3I-113 | Correctness | Push-strength getter exposed nonfinite/out-of-range private state. | Return the canonical supported range. | PHYS |
| G3I-114 | Correctness | Grounded getter exposed noncanonical `int8_t` state. | Return strict zero/one. | PHYS |
| G3I-115 | Correctness | Just-landed combined noncanonical private booleans. | Return strict zero/one. | PHYS |
| G3I-116 | Correctness | Collide-dynamic getter exposed noncanonical state. | Return strict zero/one. | PHYS |
| G3I-117 | Correctness | Ride-platforms getter exposed noncanonical state. | Return strict zero/one. | PHYS |
| G3I-118 | Correctness | Sliding getter exposed noncanonical state. | Return strict zero/one. | PHYS |
| G3I-119 | Ownership | Trigger3D called raw occupant pointers “weak,” leaving dangling pointers after body destruction. | Store actual zeroing weak handles. | PHYS |
| G3I-120 | Correctness | Allocator address reuse could make a new body inherit a dead body's occupancy. | Resolve occupants through zeroing handles. | PHYS |
| G3I-121 | Correctness | A missing member of the four-array trigger tuple left the others traversable. | Discard the tuple atomically. | PHYS |
| G3I-122 | Correctness | Negative trigger capacity/count could underflow update/finalization. | Repair both before iteration. | PHYS |
| G3I-123 | Correctness | Trigger count beyond capacity could overrun all four arrays. | Clamp it to the common capacity. | PHYS |
| G3I-124 | Ownership | Body-array realloc could succeed before a later parallel realloc failed. | Allocate all four replacements before publication. | PHYS |
| G3I-125 | Ownership | `was_inside` growth could partially publish a new common capacity. | Include it in the all-or-nothing transaction. | PHYS |
| G3I-126 | Ownership | `is_inside` growth had the same partial-publication hazard. | Include it in the transaction. | PHYS |
| G3I-127 | Ownership | Seen-stamp growth had the same partial-publication hazard. | Include it in the transaction. | PHYS |
| G3I-128 | Numeric | Parallel trigger allocation sizes were not all overflow-checked. | Validate every element product. | PHYS, STATIC |
| G3I-129 | Ownership | A failed weak-handle allocation could still publish a tracked slot. | Load-validate the weak slot before incrementing count. | PHYS |
| G3I-130 | Correctness | Trigger removal accepted unchecked slot indexes. | Repair metadata and range-check removal. | PHYS |
| G3I-131 | Ownership | Finalization freed weak slots without destroying their handles. | Clear every live weak handle first. | PHYS |
| G3I-132 | Correctness | `Contains` trusted corrupt/inverted/nonfinite retained bounds. | Re-run the canonical bounds setter before testing. | PHYS |
| G3I-133 | Correctness | Trigger update trusted the same corrupt bounds. | Repair bounds at the update boundary. | PHYS |
| G3I-134 | Correctness | Trigger world scans trusted body count beyond allocation capacity. | Use a fail-closed bounded body count. | PHYS |
| G3I-135 | Correctness | Corrupt previous-inside bytes propagated into edge detection. | Canonicalize while rolling the frame. | PHYS |
| G3I-136 | Numeric | Trigger enter counter could overflow signed storage. | Increment saturating. | PHYS |
| G3I-137 | Numeric | Trigger exit counter could overflow signed storage. | Increment saturating. | PHYS |
| G3I-138 | Correctness | Negative corrupt edge counts escaped through getters. | Clamp public results to nonnegative values. | PHYS |

### Loader, particle, spatial, and analyzer closeout

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3I-139 | Correctness | glTF output growth dereferenced a null context/asset. | Validate both before access. | ASSET, STATIC |
| G3I-140 | Correctness | Negative `extra` mesh growth was treated as success. | Accept only zero as the no-op case. | ASSET |
| G3I-141 | Numeric | glTF mesh-count addition could overflow. | Check before forming the needed capacity. | ASSET, STATIC |
| G3I-142 | Correctness | glTF's common capacity could disagree with mesh/material/variant storage. | Validate the entire tuple before copying. | ASSET |
| G3I-143 | Numeric | glTF table byte products lacked complete overflow checks. | Validate every target element size. | ASSET, STATIC |
| G3I-144 | Ownership | Mesh-table realloc could publish before material growth failed. | Allocate replacement tables transactionally. | ASSET |
| G3I-145 | Ownership | Material-table realloc could publish before variant growth failed. | Publish only after all three allocations succeed. | ASSET |
| G3I-146 | Ownership | Variant-table failure left earlier pointers/capacities partially changed. | Roll back by retaining all original arrays. | ASSET |
| G3I-147 | Correctness | Asset mesh capacity was advanced before side tables were usable. | Commit capacity last with all pointers. | ASSET |
| G3I-148 | Correctness | Missing old tables with positive capacity could leave uninitialized copied prefixes. | Fail closed and zero all grown suffixes with `calloc`. | ASSET |
| G3I-149 | Ownership | Particle sort-key growth could succeed while scratch growth failed. | Allocate both buffers before replacing either. | PART |
| G3I-150 | Ownership | Particle sort scratch could grow alone behind the shared capacity. | Commit both pointers and capacity together. | PART |
| G3I-151 | Performance | Sort-buffer realloc copied stale keys that are regenerated before every draw. | Use fresh buffers and avoid pointless copies. | PART |
| G3I-152 | Ownership | Overflow particle vertex-slot growth could partially publish. | Stage all five parallel arrays. | PART |
| G3I-153 | Ownership | Overflow index-slot growth could partially publish. | Include it in the transaction. | PART |
| G3I-154 | Ownership | Overflow vertex-capacity growth could partially publish. | Include it in the transaction. | PART |
| G3I-155 | Ownership | Overflow index-capacity growth could partially publish. | Include it in the transaction. | PART |
| G3I-156 | Ownership | Overflow material-slot growth could partially publish retained state. | Publish all five only after success. | PART |
| G3I-157 | Correctness | Positive overflow capacity with a missing parallel array could be used. | Validate the full tuple before copying. | PART |
| G3I-158 | Correctness | Newly grown overflow slot suffixes could contain garbage pointers/capacities. | Allocate zeroed replacements. | PART |
| G3I-159 | Numeric | Physics broadphase's first centroid used overflow-prone `(min + max) / 2`. | Compute half-min plus half-max. | PHYS, STATIC |
| G3I-160 | Numeric | Every later broadphase centroid repeated that overflow risk. | Use the safe midpoint in the scan. | PHYS, STATIC |
| G3I-161 | Numeric | Scene BVH centroid calculation could overflow and destabilize ordering. | Use an overflow-resistant midpoint. | CANVAS, STATIC |
| G3I-162 | Performance | BC6H decode initialized endpoint width before immediately overwriting it. | Scope the value to the branch that uses it. | STATIC, BUILD |
| G3I-163 | Performance | KTX2 parsing stored a fallback detail that was always overwritten. | Initialize the pointer without the dead value. | STATIC, BUILD |
| G3I-164 | Performance | Socket synchronization assigned `pull_from_body` immediately before leaving that decision chain. | Remove the dead store. | STATIC, BUILD |
| G3I-165 | Performance | FBX model loading zeroed a preload size after its last read. | Remove the dead store. | STATIC, BUILD |
| G3I-166 | Performance | FBX ASCII exponent parsing assigned the digit start twice. | Keep only the effective assignment. | STATIC, BUILD |
| G3I-167 | Performance | FBX threshold bisection assigned `lo` on an exact-hit exit where only `hi` is emitted. | Remove the dead assignment. | STATIC, BUILD |
| G3I-168 | Performance | CCD proxy failure zeroed remaining time immediately before `break`. | Remove the dead write. | STATIC, BUILD |
| G3I-169 | Performance | CCD no-hit handling repeated the same dead remaining-time write. | Remove it. | STATIC, BUILD |
| G3I-170 | Performance | Capsule collision, sweep-axis selection, and Transform3D rotation retained three additional overwritten locals. | Remove the dead initializers/terminal assignment while preserving results. | STATIC, BUILD |
| G3I-171 | Correctness | Particle overflow-slot capacity could report success before checking that all five parallel arrays existed. | Validate the full tuple before the capacity fast path. | PART, STATIC |
| G3I-172 | Correctness | IK cross-product rejection returned without initializing its output, allowing a caller to consume indeterminate quaternion lanes. | Zero the output before validating borrowed operands. | G3D, STATIC |
| G3I-173 | Correctness | Character delta sanitization did not initialize its output for a rejected source and read/write aliasing was implicit. | Produce a deterministic zero rejection and snapshot lanes before in-place writes. | PHYS, STATIC |
| G3I-174 | Correctness | VSCN f32 reconstruction relied only on the decoder's aggregate-length postcondition before four indexed byte reads. | Revalidate the remaining decoded span at every element boundary. | G3D, STATIC |
| G3I-175 | Correctness | VSCN f64 reconstruction likewise lacked a local eight-byte span guard. | Check each element span before little-endian assembly. | G3D, STATIC |
| G3I-176 | Correctness | The strict Base64 decoder allocated uninitialized output and did not assert that every advertised byte was emitted. | Zero-initialize the decode buffer and reject any short internal emission. | G3D, STATIC |
| G3I-177 | Ownership | An authoritative temp-buffer list could diverge from an otherwise valid-looking duplicate filter, falsely accepting an unowned allocation. | Track indexed population and rebuild whenever it differs from the list. | CANVAS, STATIC |
| G3I-178 | Ownership | The temp-object filter had the same stale-positive hazard, which could skip a required retain. | Track its population and reconstruct it from the authoritative retained-object list. | CANVAS, STATIC |
| G3I-179 | Numeric | Signed Canvas3D frame-serial increment invoked undefined behavior after `INT64_MAX`. | Rebase to a positive epoch before increment can overflow. | CANVAS, STATIC |
| G3I-180 | Correctness | Rebasing a frame serial without invalidating serial-keyed state could alias ancient motion, occlusion, streaming, or text-cache stamps. | Clear every canvas-owned temporal cache at the epoch boundary. | CANVAS |
| G3I-181 | Numeric | Texture-stream pruning added its 600-frame retention window to a signed near-limit timestamp. | Compare ordered stamps with unsigned distance. | CANVAS, STATIC |
| G3I-182 | Numeric | Texture draw-gap detection likewise evaluated `previous_touch + 2` in signed arithmetic. | Reuse the overflow-safe serial-gap predicate. | CANVAS, STATIC |
| G3I-183 | Performance | Roughness filtering recursively entered the fully defensive public cubemap sampler for every one of its five or nine blur taps. | Resolve one borrowed six-face view and reuse it across the entire kernel. | SWPERF, G3D |
| G3I-184 | Performance | Every bilinear cubemap lookup routed its four topology-wrapped texel taps through a helper that revalidated the complete managed cubemap. | Sample the four texels from the already-validated face view. | SWPERF, G3D |
| G3I-185 | Performance | Both the bilinear sampler and its nearest helper re-resolved the selected Pixels face after whole-map validation, multiplying heap-registry lock traffic per fragment. | Cache all six checked implementations for the synchronous sample lifetime. | SWPERF, G3D, STATIC |

## 4. Observable contract clarifications

- `Behavior3D.AddFollowPath(path, speed, loop)` now fulfills its existing
  constant-speed contract: speed is world units per simulation second measured
  along Path3D arc length.
- Trigger3D occupancy is genuinely non-owning and zeroing; body destruction
  cannot leave a dangling occupant or an address-reuse alias.
- Canvas3D transient lists remain authoritative if an optional duplicate hash
  cannot be allocated. Negative capacity sentinels still fail draw resource
  transactions closed.
- Motion-history updates are atomic: an entry is either fully indexed or not
  published, and current/output matrix aliasing is supported.
- CPU cubemap sampling validates managed face ownership once per filtered
  lookup; topology-aware bilinear and roughness taps reuse that borrowed view.

## 5. Validation

All builds for this audit are incremental because other work was active in the
checkout. The supported build script is always invoked with
`ZANNA_SKIP_CLEAN=1`; no clean build or raw full CMake build is used.

```sh
ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_TESTS=1 ZANNA_SKIP_LINT=1 \
  ZANNA_SKIP_AUDIT=1 ZANNA_SKIP_SMOKE=1 ZANNA_SKIP_INSTALL=1 \
  ./scripts/build_zanna_mac.sh

ctest --test-dir build \
  -R '^(test_rt_canvas3d|test_rt_canvas3d_gpu_paths|test_rt_game3d|test_rt_physics3d|test_rt_particles3d_contract|test_rt_gltf|test_rt_gltf_draco_internal|test_rt_transform_path|test_rt_scene3d|g3d_vscn_golden)$' \
  --output-on-failure --parallel 6

ctest --test-dir build -L graphics3d --output-on-failure --parallel 8

(cd examples/games/ridgebound && \
  ZANNA_3D_BACKEND=software ../../../build/src/tools/zia/zia smoke_probe.zia)

cppcheck --project=build/compile_commands.json \
  --file-filter='*/src/runtime/graphics/3d/*' \
  --enable=warning,performance,portability --check-level=exhaustive \
  --error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem

./scripts/lint_platform_policy.sh
git diff --check

ZANNA_SKIP_CLEAN=1 ZANNA_SKIP_INSTALL=1 ./scripts/build_zanna_mac.sh
```

Closeout results:

- incremental warnings-as-errors macOS builds: passed;
- focused edited-target tests: 10/10 passed;
- complete Graphics3D label: 168/168 passed, including slow and soak tests;
- Ridgebound's isolated 960x540 software cinematic rendered in 408.624 ms
  against its 5,000 ms budget, with all day/night/compact image metrics passing;
- exhaustive `cppcheck`: 106/106 compiled 3D translation-unit entries
  completed with no warning, performance, or portability findings;
- Clang path-sensitive static analyzer: 106/106 entries completed with analyzer
  warnings promoted to errors and no findings;
- supported incremental pipeline: 1,999/1,999 executed tests passed with one
  expected platform skip, followed by the 7,852-function/531-class runtime
  surface audit, its 8/8 focused tests, and all host smoke slices passing;
- platform-policy lint and `git diff --check`: clean.

## 6. Related records

- [Graphics3D Core Runtime Deep Audit (2026-08)](graphics3d-core-runtime-deep-audit-2026-08.md)
- [Graphics3D Game-Core Correctness Audit (2026-08)](graphics3d-game-core-audit-2026-08.md)
- [Graphics3D Animation and Navigation Audit (2026-08)](graphics3d-animation-navigation-audit-2026-08.md)
- [Graphics3D Runtime Correctness Audit Follow-up (2026-08)](graphics3d-runtime-audit-followup-2026-08.md)
- [Graphics3D architecture](graphics3d-architecture.md)
- [Runtime testing policy](testing.md)
