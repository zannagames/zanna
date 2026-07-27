---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Graphics3D Runtime Hardening Program (2026-07)

## 1. Summary and objective

This document is the implementation specification and traceability record for
the 48-item audit of `src/runtime/graphics/3d`. The objective is to preserve the
existing Graphics3D feature set while making malformed assets fail
deterministically, making state changes transactional, removing silent render
loss, and replacing known per-frame or serialized hot paths with retained or
parallel-safe representations.

The work is complete only when every numbered requirement below has a focused
regression, the supported macOS build and complete CTest inventory pass, and
the cross-platform policy and smoke gates pass. Changes are delivered in
coherent batches of fewer than 50 files so an individual failure remains
reviewable.

## 2. Scope

In scope:

- glTF/GLB, KTX2, and FBX parsing, validation, decoding, and animation import;
- Scene3D ownership, parenting, ray queries, snapshots, materials, mesh
  simplification, occlusion, instancing, and render scaling;
- Physics3D stepping, inertia transforms, damping, CCD, and failure atomicity;
- NavMesh3D build/query concurrency and NavAgent3D avoidance updates;
- Terrain3D/Vegetation3D integration and Particles3D simulation/rendering;
- additive runtime diagnostics/status APIs needed to expose degraded or partial
  outcomes without changing existing successful call sites;
- CTests, fuzz seeds/harness assertions, ADRs, and runtime documentation.

Out of scope:

- IL grammar, opcode, or verifier changes;
- third-party parsers, codecs, allocators, or rendering libraries;
- removal of an existing asset format, rendering backend, software fallback,
  or compatibility entry point;
- CI workflow changes.

## 3. Feature toggles and configuration

The correctness guarantees are always enabled. They are not feature-toggled
because accepting malformed input or publishing partial state cannot be a safe
compatibility mode.

Existing permissive texture loading remains available and marks a checkerboard
fallback as degraded. Additive strict texture-loading entry points reject that
same decode failure. Rendering keeps software fallbacks when a native backend
capability is absent. Hard resource ceilings are compile-time runtime constants
with checked arithmetic. The existing FBX file-size setting remains bounded by
its hard ceiling, and `ZANNA_FBX_MAX_LOAD_BYTES` may only lower the aggregate
per-load budget; neither setting can disable validation or make results depend
on allocator/platform integer width.

Simulation work is bounded per public update call. Physics and particles carry
unprocessed fixed-step time forward where possible. If the documented maximum
catch-up budget is exceeded, Particles3D records the explicitly dropped amount
instead of silently clamping it.

## 4. Technical requirements and regression matrix

### Asset parsing and import

| # | Required implementation contract | Required regression |
|---:|---|---|
| 1 | A preload GLB whose JSON chunk cannot be extracted returns no bundle, releases every staged allocation, and records an invalid-data asset error. | Truncated/missing-JSON preload GLBs fail and publish no partial bundle. |
| 2 | Synchronous and preload GLB loading share one bounded chunk iterator that validates every chunk, permits only the defined JSON/BIN layout, and rejects malformed trailing bytes. | Equivalent malformed GLBs produce the same failure through both paths. |
| 3 | glTF integer and boolean-like fields consume one complete JSON token; fractional, exponent, sign-only, suffix, and overflow forms are rejected for integral fields. | Table-driven raw JSON token boundary cases. |
| 4 | Array iteration requires a value followed by exactly comma-or-close; a malformed separator or element invalidates the entire containing object instead of returning a prefix. | Missing comma, duplicate comma, trailing junk, and partial accessor arrays fail atomically. |
| 5 | Synchronous `.gltf` loading is length-aware and rejects an embedded NUL before JSON parsing. | Embedded-NUL JSON does not truncate into a valid prefix. |
| 6 | Imported sampler state preserves minification, magnification, and mip selection independently through material snapshot and backend command creation. | Every glTF sampler enum maps to the expected three-axis sampler state. |
| 7 | Texture-coordinate indices are validated against the vertex streams actually carried by a primitive. UV0 and UV1 remain supported; unsupported indices fail with a precise asset error instead of silently sampling UV0. | UV0/UV1 success and missing/UV2+ rejection cases. |
| 8 | KTX2 zlib supercompression uses the shared zero-dependency zlib decoder, validates CMF/FLG, rejects preset dictionaries, validates deflate bounds, and verifies Adler-32 before publishing output. | Header, dictionary, deflate, output-size, and checksum corruption cases. |
| 9 | One format-capability table drives CPU decode support, native backend support queries, block geometry, and fallback selection; partial ASTC/BC6H coverage is represented accurately. | Capability rows agree with successful decoders for every advertised format. |
| 10 | A permissive KTX2 load that substitutes a checkerboard records `degraded=true` plus a stable reason. Strict loads return no asset for the same input. | Permissive/strict decode-failure pair and status getters. |
| 11 | Texture mip residency owns per-level source/backing state. Evicted decoded mips release CPU memory and can be reconstructed from retained compressed/source bytes on re-entry. | Resident-byte accounting falls after eviction and pixels reproduce after reload. |
| 12 | KTX2 supercompressed levels decode into their final owned backing allocation; avoid a whole-level intermediate copy when conversion is not required. | Allocation instrumentation bounds peak live bytes for a large level. |
| 13 | ASCII FBX is parsed into the same typed node/property/connection graph used by binary FBX and therefore supports multiple models, geometries, materials, skeletons, and animations. | Multi-object ASCII fixture matches the equivalent binary graph/scene counts. |
| 14 | ASCII FBX scanning is tokenized and brace-scoped; identifiers in comments, strings, or sibling blocks cannot satisfy a lookup. | Decoy identifiers/comments do not affect imported objects. |
| 15 | One per-load FBX budget accounts for file bytes, expanded arrays, decoded property payloads, node/property metadata, and generated animation samples before allocation. | Many individually valid compressed arrays exceeding the aggregate budget fail cleanly. |
| 16 | Animated FBX transforms use the same pivot, pre/post rotation, rotation-order, geometric-transform, and inheritance composer as static scene construction. | Animated pivot/pre-rotation fixture matches evaluated bind and keyed poses. |
| 17 | FBX object IDs and connection endpoints use a load-local hash index and adjacency lists; animation resolution must not rescan all nodes or connections per curve. | Large synthetic graph has near-linear lookup/probe counts. |
| 18 | FBX constant interpolation remains step-exact; cubic curves are adaptively subdivided until the configured value/time error bound is satisfied; linear remains linear. | Step discontinuity and cubic extrema/tangent fixtures. |
| 19 | FBX object names are dynamically sized, length-bounded by the load budget, NUL-safe, and never used as identity in place of numeric object IDs. | Long colliding-prefix names remain distinct in scene and skeleton output. |

### Scene, mesh, and rendering

| # | Required implementation contract | Required regression |
|---:|---|---|
| 20 | A Scene3D root cannot acquire a parent or move to another scene. Rejection leaves both scenes and all node links unchanged. Game3D wrapping transfers an implicit root only by preflighting a replacement root and atomically leaving the source scene valid and empty. | Cross-scene and same-scene root reparent attempts are no-ops with stable counts; `Entity3D.FromNode` transfers and spawns an imported scene hierarchy without reparenting an implicit root. |
| 21 | Owner propagation first validates the full subtree and reserves all traversal/storage capacity, then publishes every owner change as one commit. | Injected allocation failure leaves every prior owner unchanged. |
| 22 | Mesh ray queries use a retained triangle BVH keyed by mesh geometry revision; rebuild occurs once after mutation and traversal returns the same closest hit as brute force. | `test_ray_mesh_retained_bvh_matches_linear_reference` compares 128 deterministic randomized rays against an independent exhaustive sweep, bounds triangle probes, verifies one unchanged build, and verifies one rebuild after mutation. |
| 23 | Queued heap meshes retain immutable ref-counted geometry revisions; draw snapshots reference a revision instead of copying all vertex/index bytes each frame. Mutation uses copy-on-write. | `test_static_mesh_geometry_identity_forwarded` and `test_canvas_draw_retains_heap_mesh_geometry` prove queued bytes survive later mutation, same-frame revision forks coexist, and unchanged frames allocate zero geometry snapshot bytes. |
| 24 | Missing tangents are generated into a persistent cache keyed by geometry revision and UV/normal inputs, then reused across frames/backends. | `test_canvas_draw_persistently_caches_missing_normal_map_tangents` verifies raw/tangent build and hit counters, zero frame-copy bytes, no false global mutation signal, and exactly one rebuild after geometry/UV-bearing mutation. |
| 25 | Canvas submission reports capacity/allocation failure through additive status/diagnostic APIs; existing void draw calls remain compatible and never silently claim success. | Fault-injected queue/snapshot exhaustion increments failure status and preserves prior commands. |
| 26 | Pixels tracks alpha classification by content generation; deferred routing consumes the cached opaque/binary/fractional classification instead of scanning all texels per draw. | Classification invalidates on writes and is reused across repeated draws. |
| 27 | Non-native instancing splits any instance count into backend-safe chunks and submits every instance in stable order. | More than 65,536 instances produce multiple commands with no omissions. |
| 28 | Metal, D3D11, and OpenGL implement scaled scene targets plus explicit upscale/composite; software behavior remains supported and output dimensions stay logical-canvas sized. | Shared backend contract tests plus platform adapter smoke assertions. |
| 29 | High-poly occluders exceeding the precise CPU Hi-Z triangle budget contribute a conservative coarse proxy instead of bypassing occlusion entirely. | Large occluder hides only fully covered bounds and never hides edge-visible probes. |
| 30 | Mesh simplification remaps every side stream: normals/tangents/UV/color, 64-bit positions, skin joints/weights, morph deltas, submesh/material ranges, and retained references. | Attribute-rich mesh retains aligned stream counts and valid indices. |
| 31 | Simplification exposes requested and achieved triangle counts plus a complete/partial status while the existing `Simplify` object result remains available. | Unreachable target reports partial with exact achieved count. |
| 32 | Edge collapse enforces manifold link conditions, preserves classified boundaries, and rejects collapses producing duplicate/degenerate/inverted faces. | Non-manifold, boundary, bow-tie, and duplicate-face fixtures. |

### Physics and navigation

| # | Required implementation contract | Required regression |
|---:|---|---|
| 33 | World-space torque and angular impulse multiply by the rotation-transformed inverse-inertia tensor through one shared helper. | Rotated anisotropic body response matches analytic world-space result. |
| 34 | CCD sweep volume conservatively encloses each collider: exact sphere/capsule dimensions and transformed box/convex/mesh bounds, never the smallest half-extent. | Fast box corner collision is detected where the prior sphere missed. |
| 35 | CCD time-of-impact produces a normal contact constraint carrying both bodies' material properties and is solved by the ordinary restitution/friction path. | Target restitution/friction affect post-TOI velocity. |
| 36 | Linear and angular damping use exponential decay (`exp(-rate * dt)`) so an equal elapsed time is invariant to substep partitioning. | One step and many substeps match within floating tolerance. |
| 37 | Fast-body CCD computes local per-body TOI segments and does not globally subdivide unrelated bodies/world constraints. | One fast body leaves slow-body integration/solver counts unchanged. |
| 38 | Physics scratch growth and pair/contact capacity are prepared before mutation. Internal stepping returns status; fixed-step accumulator time is consumed only after a successful committed step. | Allocation failure preserves bodies, contacts, events, and accumulator for retry. |
| 39 | Nav build-grid dimensions and products use checked `size_t`/`uint64_t` arithmetic and fall back to bounded non-grid processing when replication would exceed budget. | Huge extents/tiny cell size neither wrap nor allocate an undersized grid. |
| 40 | Nav adjacency and spatial-grid rebuilds construct complete temporary state and swap only on success. | Injected allocation failure keeps previous queries usable and bit-identical. |
| 41 | Off-mesh point sampling uses the nav spatial index with expanding cells and a bounded fallback rather than scanning every polygon. | `test_navmesh_sample_position_uses_sublinear_local_probe` verifies an exact boundary result, no fallback, and fewer than one eighth of a dense mesh's triangle probes. |
| 42 | Avoidance supports snapshot/solve/apply batches: all agents read the same start-of-tick positions/velocities and publish results in deterministic handle order. Individual update remains compatible. | `test_navagent_batch_update_is_order_independent` resets one head-on pair and proves reversed input produces identical desired velocities, actual velocities, and positions. |
| 43 | A* mutable arrays live in caller/query workspaces or a bounded synchronized pool; independent queries can run concurrently without one navmesh-wide query lock. | `test_navmesh_path_workspace_reuse_is_concurrent_safe` stresses eight workers and requires the atomic overlap watermark to reach at least two. |

### Terrain, vegetation, and particles

| # | Required implementation contract | Required regression |
|---:|---|---|
| 44 | Vegetation obtains dimensions and scale through an opaque validated Terrain3D internal descriptor; it never casts a Terrain3D handle to a mirrored private layout. | Isolated mock and real Terrain3D integration both exercise non-unit scale. |
| 45 | Scatter X/Z extents are `(sample_count - 1) * spacing`, with margins clamped for terrains smaller than four world units. | Every generated coordinate remains inside the actual last terrain vertex. |
| 46 | Particles3D advances in bounded substeps, carries normal residual time, and records any catch-up time explicitly dropped by the safety budget. | Large update matches repeated substeps up to budget and reports exact dropped time. |
| 47 | A particle integrates for `min(dt, remaining_lifetime)`, reaches its exact endpoint, and expires in that update. An explicit final-frame option may render the endpoint once. | Expiration position and optional terminal-frame draw have no stale extra frame. |
| 48 | Hardware backends draw a retained unit quad with compact per-particle instance data; software retains the CPU-expanded path. Repeated hardware draws do not rebuild four vertices and six indices per particle. | `RTParticles3DContractTests` reconstructs hardware corners and colors against the software mesh and covers mixed CPU trails; `test_rt_canvas3d_gpu_paths` verifies snapshots and retained geometry identity; `test_vgfx3d_particle_backend_contract` verifies all three hardware adapters and the software opt-out. |

## 5. Implemented regression traceability

The following CTest targets and test functions are the permanent closeout
evidence for each numbered requirement. A row can name more than one function
when the contract crosses a runtime boundary or has both success and
fault-injection behavior.

| # | CTest target and focused regression function(s) |
|---:|---|
| 1 | `test_rt_gltf` — `test_gltf_shared_glb_chunk_validation_is_atomic` |
| 2 | `test_rt_gltf` — `test_gltf_shared_glb_chunk_validation_is_atomic` |
| 3 | `test_rt_gltf` — `test_gltf_json_integral_tokens_are_exact` |
| 4 | `test_rt_gltf` — `test_gltf_json_array_iteration_rejects_malformed_suffixes` |
| 5 | `test_rt_gltf` — `test_gltf_embedded_nul_is_not_prefix_truncated` |
| 6 | `test_rt_gltf` — `test_gltf_maps_every_sampler_filter_enum_to_independent_axes` |
| 7 | `test_rt_gltf` — `test_gltf_validates_authored_texture_uv_streams` |
| 8 | `test_rt_canvas3d` — `test_textureasset3d_zlib_integrity_validation`; `test_rt_compress` — `test_zlib_exact_destination_validation`; companion zstd coverage: `test_rt_zstd` — `test_exact_destination_rejects_size_mismatch_and_trailing_input` |
| 9 | `test_rt_canvas3d` — `test_textureasset3d_format_capability_table` |
| 10 | `test_rt_canvas3d` — `test_textureasset3d_decode_failure_checker_fallback` |
| 11 | `test_rt_canvas3d` — `test_textureasset3d_mip_residency` |
| 12 | `test_rt_canvas3d` — `test_textureasset3d_supercompression_direct_backing_allocation` |
| 13 | `test_rt_model3d` — `test_fbx_ascii_typed_graph_matches_binary_multi_object_graph` |
| 14 | `test_rt_model3d` — `test_fbx_ascii_parser_ignores_decoy_identifiers_and_scopes` |
| 15 | `test_rt_model3d` — `test_fbx_aggregate_budget_rejects_many_valid_compressed_arrays` |
| 16 | `test_rt_model3d` — `test_fbx_animation_uses_static_transform_stack_composer` |
| 17 | `test_rt_model3d` — `test_fbx_large_animation_graph_lookup_probes_are_near_linear` |
| 18 | `test_rt_model3d` — `test_fbx_constant_animation_curve_preserves_step`, `test_fbx_cubic_animation_adaptive_sampling_preserves_shape` |
| 19 | `test_rt_model3d` — `test_fbx_long_colliding_names_remain_distinct_and_id_bound` |
| 20 | `test_rt_scene3d` — `test_scene_rejects_reparenting_implicit_root`; `test_rt_game3d` — `test_entity_from_node_wraps_imported_subtree` (implicit-root transfer/spawn) |
| 21 | `test_rt_scene3d` — `test_owner_propagation_allocation_failure_is_atomic`; `test_rt_game3d` — `test_entity_from_node_wraps_imported_subtree` (transfer preflight fault injection) |
| 22 | `test_rt_raycast3d` — `test_ray_mesh_retained_bvh_matches_linear_reference` |
| 23 | `test_rt_canvas3d_gpu_paths` — `test_static_mesh_geometry_identity_forwarded`; `test_rt_canvas3d` — `test_canvas_draw_retains_heap_mesh_geometry` |
| 24 | `test_rt_canvas3d` — `test_canvas_draw_persistently_caches_missing_normal_map_tangents` |
| 25 | `test_rt_canvas3d_gpu_paths` — `test_mesh_draw_traps_when_deferred_queue_cannot_grow`, `test_mesh_snapshot_failure_reports_status_and_preserves_prior_commands` |
| 26 | `test_rt_canvas3d_gpu_paths` — `test_pixels_alpha_classification_is_cached_by_content_generation` |
| 27 | `test_rt_canvas3d` — `test_canvas_instanced_fallback_chunks_without_omission` |
| 28 | `test_vgfx3d_backend_utils` — `test_scaled_scene_extent_contract`; `test_rt_canvas3d` — `test_canvas_render_scale_failure_preserves_state`; runtime fixtures `g3d_test_canvas3d_render_scale` and `g3d_test_canvas3d_render_scale_metal` |
| 29 | `test_rt_canvas3d` — `test_canvas_hiz_high_poly_proxy_is_conservative` |
| 30 | `test_rt_mesh_simplify` — `test_attribute_animation_and_range_remap` |
| 31 | `test_rt_mesh_simplify` — `test_partial_status_duplicate_guard_and_range_coalescing` |
| 32 | `test_rt_mesh_simplify` — `test_nonmanifold_and_bowtie_guards` |
| 33 | `test_rt_physics3d` — `test_rotated_anisotropic_body_uses_world_inverse_inertia` |
| 34 | `test_rt_physics3d` — `test_ccd_proxy_encloses_rotated_box_corners` |
| 35 | `test_rt_physics3d` — `test_ccd_toi_uses_target_collider_material` |
| 36 | `test_rt_physics3d` — `test_exponential_damping_is_substep_partition_invariant` |
| 37 | `test_rt_physics3d` — `test_ccd_segmentation_is_body_local` |
| 38 | `test_rt_physics3d` — `test_world_step_failure_is_atomic_and_retryable` |
| 39 | `test_rt_navmesh_blend` — `test_navmesh_voxel_grid_extreme_arithmetic_is_bounded` |
| 40 | `test_rt_navmesh_blend` — `test_navmesh_rebuild_allocation_failure_is_atomic` |
| 41 | `test_rt_navmesh_blend` — `test_navmesh_sample_position_uses_sublinear_local_probe` |
| 42 | `test_rt_navagent3d` — `test_navagent_batch_update_is_order_independent` |
| 43 | `test_rt_navmesh_blend` — `test_navmesh_path_workspace_reuse_is_concurrent_safe` |
| 44 | `test_rt_instterrain` — `test_terrain_grid_info_uses_sample_intervals`; `test_rt_vegetation3d_contract` — `test_populate_uses_opaque_scaled_terrain_extents` |
| 45 | `test_rt_vegetation3d_contract` — `test_populate_uses_opaque_scaled_terrain_extents` |
| 46 | `test_rt_particles3d_contract` — `test_update_is_bounded_and_reports_exact_dropped_time` |
| 47 | `test_rt_particles3d_contract` — `test_particle_final_frame_renders_end_state`, `test_particle_final_frame_can_be_disabled` |
| 48 | `test_rt_particles3d_contract` — `test_hardware_instances_match_software_billboards_and_reuse_scratch`, `test_hardware_particle_path_preserves_cpu_trail_ribbons`; `test_rt_canvas3d_gpu_paths` — `test_compact_particle_batches_snapshot_instances_and_retain_unit_geometry`; `test_vgfx3d_particle_backend_contract` — compact-record plus Metal/D3D11/OpenGL/software adapter contract tests |

## 6. Error handling

- Parser validation failures record the existing recoverable asset-error code
  appropriate to invalid data, unsupported capability, resource limit, or
  integrity failure, and return no newly published object.
- Existing APIs that historically trap on invalid caller arguments keep their
  trap strings. Additive `Try*`/strict APIs return false or null and leave a
  queryable status; they do not partially mutate live objects.
- Allocation failure during a transaction leaves the old state fully usable.
  Where the existing allocator contract traps, staged allocations are owned by
  a trap cleanup boundary before the potentially trapping call.
- Render submission failure leaves commands already queued in the frame valid,
  increments a diagnostic counter, and records a stable reason. It does not
  overwrite an earlier failure until that status is consumed/reset.
- Arithmetic overflow is treated as a resource-limit/invalid-data failure
  before allocation or pointer arithmetic.

## 7. Validation commands

Each batch runs its directly affected test binaries through CTest. The closeout
gates are:

```sh
./scripts/lint_platform_policy.sh
./scripts/run_cross_platform_smoke.sh
./scripts/build_zanna_mac.sh
```

The final report records exact CTest counts, skips, and elapsed time, and maps
all 48 requirements to the committed regression names.

## 8. Architecture references

- [Graphics3D architecture](graphics3d-architecture.md)
- [Runtime testing policy](testing.md)
- [ADR 0102: Graphics3D runtime boundary](../adr/0102-graphics3d-runtime-boundary-and-contract-manifest.md)
- ADR 0173 (this program's runtime C ABI and internal boundary decisions; added
  with the first ABI-affecting batch)
