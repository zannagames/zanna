---
status: completed
audience: contributors
last-verified: 2026-08-04
---

# Graphics3D World Runtime Integrity Audit (2026-08-04)

## Scope and method

This follow-up reviewed the complete `src/runtime/graphics/3d` inventory and
then performed a line-by-line ownership, allocation-publication, retained-state,
and hot-loop review of the three mutable world effects where the new defect
pattern concentrated:

- `world/rt_water3d.c`;
- `world/rt_vegetation3d.c`;
- `world/rt_particles3d.c`.

Earlier Graphics3D ledgers were read first. Their closed findings are baseline
and are not counted again here. In particular, this ledger does not recount the
previous particle sort-pair or overflow-table growth transactions. It records
**160 newly corrected bugs, correctness hazards, ownership failures, and
performance problems**. Entries remain separate when a different retained
field, owner identity, allocation, finalizer action, state transition, or hot
loop was independently unsafe.

The implementation preserves the historical private struct prefixes used by
isolated tests and internal stack views. New owner identities are appended; no
runtime registry row, public function signature, C ABI surface, IL contract,
grammar, verifier rule, workflow, or dependency changed. No ADR is required.

## Evidence key

- **WATER**: `test_rt_water3d_contract`, including resource-mirror corruption,
  persistent scalar repair, pause/distance-gate behavior, and material-only
  updates.
- **VEG**: `test_rt_vegetation3d_contract`, including population, visible-set,
  render-resource, density-map, and CSR-grid corruption plus no-op rebuilds.
- **PART**: `test_rt_particles3d_contract`, including emitter-state repair,
  pool/trail/resource/draw-table ownership, retained scratch reuse, and
  overflow slots.
- **BUILD**: supported warnings-as-errors incremental macOS build with
  `ZANNA_SKIP_CLEAN=1`.
- **STATIC**: exhaustive Graphics3D `cppcheck`, platform-policy lint, and
  `git diff --check`.
- **G3D**: complete CTest `graphics3d` label.

## Corrected issue ledger

### Water3D retained state, ownership, and rebuild cost

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3W-001 | Ownership | A corrupted public-prefix mesh mirror replaced the retained mesh identity. | Append an authoritative mesh owner and republish the mirror. | WATER |
| G3W-002 | Ownership | A corrupted material mirror lost the retained material identity. | Retain and validate a private material owner. | WATER |
| G3W-003 | Ownership | A foreign texture mirror could replace the retained Pixels reference. | Assign and read through a private texture owner. | WATER |
| G3W-004 | Ownership | A foreign normal-map mirror could replace the retained Pixels reference. | Add a distinct normal-map owner and restore the mirror. | WATER |
| G3W-005 | Ownership | A foreign environment-map mirror could replace the retained CubeMap3D reference. | Add a complete-cubemap owner and restore the mirror. | WATER |
| G3W-006 | Ownership | Finalization freed/released the mutable mesh mirror. | Finalize only the mesh owner. | WATER |
| G3W-007 | Ownership | Finalization released the mutable material mirror. | Finalize only the material owner. | WATER |
| G3W-008 | Ownership | Finalization could release a foreign texture mirror. | Release only the texture owner. | WATER |
| G3W-009 | Ownership | Finalization could release a foreign normal-map mirror. | Release only the normal-map owner. | WATER |
| G3W-010 | Ownership | Finalization could release a foreign cubemap mirror. | Release only the environment-map owner. | WATER |
| G3W-011 | Correctness | `GetTexture` could return a corrupt, unretained mirror. | Repair first and return the validated owner. | WATER |
| G3W-012 | Correctness | `GetNormalMap` could return a corrupt, unretained mirror. | Repair first and return the validated owner. | WATER |
| G3W-013 | Correctness | `GetEnvMap` could return a corrupt or incomplete cubemap mirror. | Repair first and return the complete retained owner. | WATER |
| G3W-014 | Correctness | Mesh-mirror divergence did not force geometry recovery. | Mark geometry dirty whenever the owner must be republished. | WATER |
| G3W-015 | Correctness | Material or binding divergence did not force material recovery. | Mark material state dirty independently of geometry. | WATER |
| G3W-016 | Correctness | Nonfinite/nonpositive retained width survived until selected update paths. | Persist a finite positive bounded width at every boundary. | WATER |
| G3W-017 | Correctness | Nonfinite/nonpositive retained depth survived likewise. | Persist a finite positive bounded depth. | WATER |
| G3W-018 | Correctness | Corrupt retained height escaped through readback. | Persist the bounded finite height before reads and use. | WATER |
| G3W-019 | Correctness | Corrupt center X escaped readback and distance calculations. | Persist a bounded finite center X. | WATER |
| G3W-020 | Correctness | Corrupt center Z escaped readback and distance calculations. | Persist a bounded finite center Z. | WATER |
| G3W-021 | Correctness | Retained wave speed could bypass setter validation. | Reapply the signed parameter bound centrally. | WATER |
| G3W-022 | Correctness | Retained wave amplitude could become negative/nonfinite. | Persist the supported nonnegative range. | WATER |
| G3W-023 | Correctness | Retained wave frequency could become negative/nonfinite. | Persist the supported nonnegative range. | WATER |
| G3W-024 | Correctness | Corrupt RGB lanes were sanitized only transiently. | Clamp and store all three tint lanes. | WATER |
| G3W-025 | Correctness | Corrupt alpha could escape readback or material upload. | Clamp and persist alpha. | WATER |
| G3W-026 | Correctness | Corrupt reflectivity could escape readback or shader state. | Clamp and persist reflectivity. | WATER |
| G3W-027 | Correctness | Negative/nonfinite retained phase time poisoned later updates. | Reset invalid time to zero. | WATER |
| G3W-028 | Numeric | Unbounded retained phase time lost trigonometric precision. | Range-reduce the persisted clock. | WATER |
| G3W-029 | Correctness | Simulation distance could remain negative/nonfinite. | Persist a finite nonnegative gate distance. | WATER |
| G3W-030 | Correctness | Corrupt resolution reached grid arithmetic or inconsistent readback. | Restore the supported default before use. | WATER |
| G3W-031 | Correctness | Negative retained wave count could index before the wave array. | Repair the lower bound. | WATER |
| G3W-032 | Correctness | Wave count beyond the fixed array could overread waves. | Repair the upper bound. | WATER |
| G3W-033 | Correctness | Camera-presence state was not canonical Boolean data. | Canonicalize it at every boundary. | WATER |
| G3W-034 | Correctness | Nonfinite/out-of-range cached camera coordinates poisoned distance gating. | Validate all lanes and discard the camera sample atomically. | WATER |
| G3W-035 | Correctness | Corrupt dirty bytes could retain noncanonical state. | Canonicalize geometry and material dirty flags. | WATER |
| G3W-036 | Performance | Material color and bindings were resent on every wave frame. | Refresh material only when missing or dirty. | WATER |
| G3W-037 | Performance | Color, alpha, texture, normal, env, or reflectivity edits forced an expensive grid rewrite. | Split material invalidation from geometry invalidation. | WATER |
| G3W-038 | Correctness | The paused-update fast path skipped pending material changes. | Apply material invalidation before the pause fast path. | WATER |
| G3W-039 | Correctness | The distance gate skipped pending material/resource recovery. | Repair and refresh material before distance culling. | WATER |
| G3W-040 | Ownership | Vertex growth could publish before the optional f64 sidecar succeeded. | Stage both vertex representations and commit together. | WATER, STATIC |
| G3W-041 | Ownership | Index growth could fail after vertex storage had already changed. | Stage indices in the same reserve transaction. | WATER, STATIC |
| G3W-042 | Correctness | A positive capacity with a missing vertex/index pointer passed reserve. | Treat missing storage as a growth requirement. | WATER |
| G3W-043 | Performance | Static grid indices were regenerated on every animation tick. | Rewrite indices only when topology changes. | WATER |
| G3W-044 | Performance | Each vertex repeated wave validation and time-range reduction. | Sanitize waves and cache reduced phases once per rebuild. | WATER |
| G3W-045 | Performance | Reapplying unchanged geometry settings dirtied the full mesh. | Make height, placement, wave, resolution, and clear operations change-sensitive. | WATER |
| G3W-046 | Performance | Reapplying unchanged material settings caused redundant uploads. | Make color, bindings, and reflectivity change-sensitive. | WATER |
| G3W-047 | Correctness | A failed mesh rebuild could leave stale geometry eligible for draw. | Keep geometry dirty and recheck after attempted recovery. | WATER |
| G3W-048 | Correctness | Invalid camera output could remain as the last valid distance-gate sample. | Clear the complete cached sample on any invalid lane. | WATER |
| G3W-049 | Correctness | Draw silently failed to recover a missing mesh or material. | Invoke a zero-delta lazy recovery before submission. | WATER |
| G3W-050 | Ownership | Lazily created mesh/material handles were published only into mutable mirrors. | Publish new resources into owner identities first. | WATER |

### Vegetation3D storage authority, grid safety, and hot paths

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3W-051 | Ownership | A foreign density-map mirror replaced the retained Pixels identity. | Add a validated density-map owner. | VEG |
| G3W-052 | Ownership | A foreign blade-mesh mirror replaced the retained mesh identity. | Add a private mesh owner and republish it. | VEG |
| G3W-053 | Ownership | A foreign blade-material mirror replaced the retained material identity. | Add a private material owner and republish it. | VEG |
| G3W-054 | Ownership | Finalization could release a foreign density mirror. | Release only the density owner. | VEG |
| G3W-055 | Ownership | Finalization could release a foreign mesh mirror. | Release only the mesh owner. | VEG |
| G3W-056 | Ownership | Finalization could release a foreign material mirror. | Release only the material owner. | VEG |
| G3W-057 | Ownership | A foreign base-transform mirror could replace population storage. | Keep an authoritative base-transform allocation. | VEG |
| G3W-058 | Ownership | A foreign position mirror could replace the parallel position allocation. | Keep a distinct authoritative position allocation. | VEG |
| G3W-059 | Correctness | Corrupt total count could overrun the population owners. | Restore and bound it from private authority. | VEG |
| G3W-060 | Correctness | Corrupt population capacity could authorize foreign/out-of-bounds storage. | Restore the exact owner capacity. | VEG |
| G3W-061 | Ownership | A foreign visible-transform mirror could replace render scratch. | Keep an authoritative visible allocation. | VEG |
| G3W-062 | Correctness | Corrupt visible count could submit stale/out-of-bounds instances. | Restore the authoritative bounded count. | VEG |
| G3W-063 | Correctness | Corrupt visible capacity could defeat growth and write checks. | Restore the owner capacity. | VEG |
| G3W-064 | Ownership | A foreign CSR offset mirror could replace the retained grid. | Keep an authoritative offset allocation. | VEG |
| G3W-065 | Ownership | A foreign CSR index mirror could replace the retained grid indices. | Keep an authoritative index allocation. | VEG |
| G3W-066 | Correctness | Corrupt grid dimensions changed offset indexing. | Restore both dimensions from the grid transaction. | VEG |
| G3W-067 | Correctness | Corrupt grid origins changed cell selection. | Restore both finite owner origins. | VEG |
| G3W-068 | Correctness | Corrupt cell extents caused divide-by-zero/nonfinite traversal. | Restore both positive finite owner extents. | VEG |
| G3W-069 | Correctness | Grid-ready state could be noncanonical or detached from storage. | Publish readiness only with the complete owner tuple. | VEG |
| G3W-070 | Ownership | Finalization freed mutable population mirrors. | Free only base/position owners. | VEG |
| G3W-071 | Ownership | Finalization freed mutable visible scratch. | Free only the visible owner. | VEG |
| G3W-072 | Ownership | Finalization freed mutable grid mirrors. | Free only the two grid owners. | VEG |
| G3W-073 | Correctness | Corrupt blade width escaped into rebuilds. | Persist the finite positive supported value. | VEG |
| G3W-074 | Correctness | Corrupt blade height escaped into rebuilds. | Persist the finite positive supported value. | VEG |
| G3W-075 | Correctness | Size variation could remain nonfinite or outside `[0,1]`. | Clamp and persist it. | VEG |
| G3W-076 | Correctness | Retained wind speed bypassed setter validation. | Persist the signed supported range. | VEG |
| G3W-077 | Correctness | Retained wind strength bypassed setter validation. | Persist the nonnegative supported range. | VEG |
| G3W-078 | Correctness | Retained turbulence bypassed setter validation. | Persist the nonnegative supported range. | VEG |
| G3W-079 | Numeric | Corrupt/unbounded wind time poisoned phase precision. | Reset or range-reduce the retained clock. | VEG |
| G3W-080 | Correctness | Corrupt near/far LOD values could invert or collapse the band. | Persist a finite ordered pair. | VEG |
| G3W-081 | Correctness | A zero retained scatter seed degenerated the LCG stream. | Restore a deterministic nonzero seed. | VEG |
| G3W-082 | Ownership | Population transform allocation could succeed while position allocation failed. | Stage both allocations before publication. | VEG, STATIC |
| G3W-083 | Correctness | Population pointers, capacity, and accepted count were published at different stages. | Commit the complete tuple after scattering finishes. | VEG |
| G3W-084 | Ownership | Visible growth used `realloc` on a corrupt public mirror. | Reallocate only the private visible owner. | VEG |
| G3W-085 | Correctness | Collection could write beyond visible scratch after tuple divergence. | Require owner capacity before every append. | VEG |
| G3W-086 | Performance | CSR construction allocated a third full cursor array. | Reuse an appended scratch suffix in the offsets allocation. | VEG |
| G3W-087 | Correctness | CSR terminal offset was trusted before iteration. | Verify zero origin and exact terminal count. | VEG |
| G3W-088 | Correctness | Per-cell offset ranges and blade indices were trusted. | Validate begin, end, and every referenced blade. | VEG |
| G3W-089 | Correctness | Mid-grid corruption left a partial visible set. | Discard it, free the grid, and restart with the linear fallback. | VEG |
| G3W-090 | Performance | No-op or variation-only blade edits rebuilt shared geometry. | Rebuild only for dimension changes or actual mesh damage. | VEG |

### Particles3D allocation authority and persistent emitter repair

| ID | Class | Defect and impact | Correction | Evidence |
|---|---|---|---|---|
| G3W-091 | Ownership | A foreign particle-pool mirror replaced the retained pool. | Add an authoritative pool owner. | PART |
| G3W-092 | Correctness | Corrupt max-particle mirror changed every pool bound. | Restore the construction-time owner capacity. | PART |
| G3W-093 | Correctness | Corrupt live count escaped into spawn/update/draw. | Restore a bounded authoritative live count. | PART |
| G3W-094 | Correctness | Corrupt terminal count could overlap the live prefix. | Restore and validate an authoritative terminal count. | PART |
| G3W-095 | Ownership | Finalization freed the mutable particle-pool mirror. | Free only the pool owner. | PART |
| G3W-096 | Ownership | A foreign trail-position mirror replaced retained ring storage. | Add an authoritative position-ring owner. | PART |
| G3W-097 | Ownership | A foreign trail-age mirror replaced its parallel allocation. | Add an authoritative age owner. | PART |
| G3W-098 | Ownership | A foreign trail-length mirror replaced its parallel allocation. | Add an authoritative length owner. | PART |
| G3W-099 | Ownership | A foreign trail-head mirror replaced its parallel allocation. | Add an authoritative head owner. | PART |
| G3W-100 | Correctness | Corrupt trail-segment metadata changed all ring strides. | Restore the owner segment count with the full tuple. | PART |
| G3W-101 | Ownership | Finalization freed four mutable trail mirrors. | Free only the four trail owners. | PART |
| G3W-102 | Ownership | Disabling trails freed potentially foreign mirrors. | Clear the owner tuple through one helper. | PART |
| G3W-103 | Performance | Reapplying the same trail segment count replaced all four rings. | Reuse the owner tuple and update only lifetime. | PART |
| G3W-104 | Ownership | Trail allocation could partially publish one of four arrays. | Stage all four arrays and commit atomically. | PART, STATIC |
| G3W-105 | Ownership | A foreign texture mirror replaced the retained Pixels identity. | Add a validated texture owner. | PART |
| G3W-106 | Correctness | Texture get/set compared or returned the mutable mirror. | Compare, assign, and return the owner. | PART |
| G3W-107 | Ownership | Finalization could release a foreign texture mirror. | Release only the texture owner. | PART |
| G3W-108 | Ownership | A corrupt cached-material mirror could be released as retained state. | Add and validate a private cached-material owner. | PART |
| G3W-109 | Ownership | Foreign fixed-slot vertex mirrors replaced reusable draw buffers. | Add owner pointers for every fixed vertex slot. | PART |
| G3W-110 | Ownership | Foreign fixed-slot index mirrors replaced reusable index buffers. | Add owner pointers for every fixed index slot. | PART |
| G3W-111 | Correctness | Corrupt fixed vertex capacities defeated growth checks. | Publish capacities from owner metadata. | PART |
| G3W-112 | Correctness | Corrupt fixed index capacities defeated growth checks. | Publish capacities from owner metadata. | PART |
| G3W-113 | Ownership | Foreign fixed-slot material mirrors replaced retained materials. | Add owner slots for every material. | PART |
| G3W-114 | Ownership | Finalization traversed fixed draw mirrors. | Free/release only fixed-slot owners. | PART |
| G3W-115 | Ownership | A foreign overflow vertex-table mirror replaced the retained table. | Add a top-level table owner. | PART |
| G3W-116 | Ownership | A foreign overflow index-table mirror replaced the retained table. | Add a top-level table owner. | PART |
| G3W-117 | Ownership | A foreign overflow vertex-capacity table changed bounds. | Add an authoritative capacity-table owner. | PART |
| G3W-118 | Ownership | A foreign overflow index-capacity table changed bounds. | Add an authoritative capacity-table owner. | PART |
| G3W-119 | Ownership | A foreign overflow material table replaced retained slots. | Add an authoritative material-table owner. | PART |
| G3W-120 | Correctness | Corrupt overflow slot capacity authorized unrelated tables. | Restore the exact owner capacity. | PART |
| G3W-121 | Ownership | Finalization used mutable overflow tables and capacity. | Traverse and free only the complete owner tuple. | PART |
| G3W-122 | Ownership | A foreign sort-key mirror replaced persistent sort storage. | Add an authoritative key owner. | PART |
| G3W-123 | Ownership | A foreign sort-scratch mirror replaced its paired storage. | Add an authoritative scratch owner. | PART |
| G3W-124 | Correctness | Corrupt sort capacity/growth telemetry escaped test and draw paths. | Restore both from owner metadata. | PART |
| G3W-125 | Ownership | Finalization freed mutable sort mirrors. | Free only the paired sort owners. | PART |
| G3W-126 | Ownership | A foreign compact-instance mirror replaced retained scratch. | Add an authoritative instance owner. | PART |
| G3W-127 | Correctness | Corrupt instance capacity/growth telemetry defeated reuse checks. | Restore both from owner metadata. | PART |
| G3W-128 | Ownership | Finalization freed mutable instance scratch. | Free only the instance owner. | PART |
| G3W-129 | Ownership | Per-slot vertex growth could publish before index growth failed. | Stage both buffers and commit together. | PART, STATIC |
| G3W-130 | Performance | A failed draw-slot preparation permanently consumed a same-frame slot. | Increment slot use only after successful preparation. | PART |
| G3W-131 | Correctness | Positive per-slot capacity with a missing pointer passed preparation. | Treat missing nonempty buffers as growth. | PART |
| G3W-132 | Correctness | Corrupt emitter position was only sanitized at selected uses. | Persist all three bounded finite lanes. | PART |
| G3W-133 | Correctness | Zero/nonfinite retained direction produced unstable cone frames. | Robustly normalize or restore +Y. | PART |
| G3W-134 | Correctness | Corrupt spread escaped readback and trigonometry. | Persist `[0, pi]`. | PART |
| G3W-135 | Correctness | Corrupt speed bounds could remain negative, nonfinite, or inverted. | Persist a bounded ordered pair. | PART |
| G3W-136 | Correctness | Corrupt lifetime bounds could remain invalid or inverted. | Persist a positive bounded ordered pair. | PART |
| G3W-137 | Correctness | Corrupt start/end sizes escaped readback and render state. | Persist both nonnegative bounded values. | PART |
| G3W-138 | Correctness | Corrupt gravity lanes poisoned integration. | Persist all three finite bounded lanes. | PART |
| G3W-139 | Correctness | Corrupt start-color lanes escaped packed readback. | Clamp and store all three lanes before packing. | PART |
| G3W-140 | Correctness | Corrupt end-color lanes escaped packed readback. | Clamp and store all three lanes before packing. | PART |
| G3W-141 | Correctness | Corrupt start/end alpha escaped readback and interpolation. | Persist both lanes in `[0,1]`. | PART |
| G3W-142 | Correctness | Corrupt emission rate survived until selected update paths. | Persist the supported nonnegative range. | PART |
| G3W-143 | Correctness | Nonfinite/negative/oversized spawn accumulator destabilized emission. | Repair and bound it to pool capacity. | PART |
| G3W-144 | Correctness | Emitting state could remain a noncanonical byte. | Persist strict zero/one. | PART |
| G3W-145 | Correctness | Additive-blend state could remain a noncanonical byte. | Persist strict zero/one. | PART |
| G3W-146 | Correctness | Corrupt velocity stretch bypassed setter limits. | Persist `[0,8]`. | PART |
| G3W-147 | Correctness | Corrupt softness bypassed setter limits. | Persist the supported nonnegative range. | PART |
| G3W-148 | Correctness | Unknown emitter-shape values reached spawn dispatch. | Restore the point-emitter fallback. | PART |
| G3W-149 | Correctness | Negative/nonfinite emitter extents reached spawn offsets. | Persist absolute bounded extents. | PART |
| G3W-150 | Correctness | A zero retained xorshift seed locked the PRNG. | Restore the nonzero fallback. | PART |
| G3W-151 | Correctness | Invalid trail lifetime left a nominally enabled tuple. | Disable the complete owner tuple or persist a valid lifetime. | PART |
| G3W-152 | Correctness | Nonfinite/negative per-particle trail age poisoned sampling cadence. | Repair every live age. | PART |
| G3W-153 | Correctness | Trail length outside the ring capacity caused overreads. | Reset invalid live lengths. | PART |
| G3W-154 | Correctness | Trail head outside the ring capacity caused out-of-bounds writes. | Reset invalid live heads. | PART |
| G3W-155 | Correctness | Invalid fixed-step residual survived telemetry and integration. | Persist the valid substep remainder range. | PART |
| G3W-156 | Correctness | Invalid cumulative dropped-time telemetry escaped readback. | Persist finite nonnegative telemetry. | PART |
| G3W-157 | Correctness | Invalid last-update dropped time escaped readback. | Persist finite nonnegative telemetry. | PART |
| G3W-158 | Correctness | Final-frame state was noncanonical and disabled state retained snapshots. | Canonicalize it and clear terminal authority when disabled. | PART |
| G3W-159 | Correctness | Spawn, swap-kill, clear, and terminal transitions changed mirrors without owner counts. | Synchronize authority at every count transition. | PART |
| G3W-160 | Correctness | Invalid-delta/empty-burst boundaries returned before repairing retained state. | Repair first, then apply the operation-specific no-op. | PART, VEG |

## Validation

All build-script invocations for this audit were incremental because other work
was active in the checkout. No clean build was permitted or used.

- The supported macOS build script completed with warnings-as-errors enabled,
  `ZANNA_SKIP_CLEAN=1`, `ZANNA_ENABLE_FUZZ=OFF`, and the test, lint, audit,
  smoke, install, and Studio stages skipped. Tests and policy checks were run
  separately below.
- `ctest --test-dir build -L graphics3d -LE slow --output-on-failure
  --parallel 10` passed **143/143** tests. A separate run of the three slow
  Graphics3D tests passed **3/3**, including the 120-second short soak, for
  **146/146** label tests overall.
- The isolated Water3D, Vegetation3D, and Particles3D contracts passed **3/3**
  together after the final source rebuild.
- The generated runtime-surface audit passed, including `rtgen` validation and
  **8/8** focused link, catalog, BASIC, Zia, and surface-audit tests.
- Exhaustive `cppcheck` analysis of the compile database with warning,
  performance, and portability checks passed all **106/106** compiled
  Graphics3D C translation units without a diagnostic.
- `scripts/lint_platform_policy.sh` and `git diff --check` passed.
- `scripts/source_health_audit.sh` passes after the follow-up reconciliation.
  Six collection-private headers are explicitly classified and acknowledged as
  intentional runtime boundaries, sixteen promoted Graphics3D symbols have
  canonical frontend expectations, and centralized HTTP-client recovery-state
  destruction reduced that file from 73 raw-allocation matches to 65. Together
  with the Particles3D reduction from 80 to 66, this restores the manual
  allocation hotspot total to its baseline of 28.

## Related records

- [Graphics3D Runtime Integrity Audit (2026-08-03)](graphics3d-runtime-integrity-audit-2026-08.md)
- [Graphics3D Core Runtime Deep Audit (2026-08)](graphics3d-core-runtime-deep-audit-2026-08.md)
- [Graphics3D Runtime Correctness Audit Follow-up (2026-08)](graphics3d-runtime-audit-followup-2026-08.md)
- [Graphics3D architecture](graphics3d-architecture.md)
- [Runtime testing policy](testing.md)
