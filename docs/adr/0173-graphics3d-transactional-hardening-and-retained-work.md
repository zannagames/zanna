---
status: active
audience: contributors
last-verified: 2026-07-21
---

# ADR 0173: Make Graphics3D State Transactional and Retain Reusable Work

## Status

Accepted (2026-07-19); additive registry rows will be recorded here before
their implementation batches land.

## Context

The July 2026 Graphics3D audit found 48 correctness and performance defects
across asset import, scene ownership, rendering, physics, navigation, terrain,
and particles. Several defects came from one runtime module reading another
module's private object layout. Others came from mutating live state before all
required allocations had succeeded, or from rebuilding immutable geometry and
derived data every frame.

The Graphics3D registry is the supported public boundary under ADR 0102. Its C
entry points are an internal embedding ABI, while runtime object layouts are
private. Fixing these defects requires new internal module contracts and will
later require additive registry diagnostics/status methods. Repository policy
therefore requires an ADR before either dependency or ABI surface changes.

## Decision

### Compatibility and publication

- Existing registry names, method signatures, asset formats, rendering
  backends, and software fallbacks remain available. Hardening does not remove
  a feature or reinterpret a successful existing call.
- Parser output, scene ownership, nav adjacency/grids, physics steps, and draw
  queue commands use stage-then-publish transactions. All fallible validation,
  checked sizing, allocation, and reference acquisition occurs before the live
  object is changed. A failure releases staged ownership and preserves the
  prior live state.
- Additive status APIs are used where an existing object-returning or `void`
  API cannot distinguish complete, degraded, partial, or failed work. The
  precise registry rows and their C signatures are amendments to this Decision
  and must be present before their corresponding implementation.

### Internal object boundaries

- Runtime modules do not mirror or cast another module's private object
  payload. Cross-module reads use a narrow internal header whose functions
  validate the opaque handle and copy plain data into caller-owned output.
- Terrain3D exposes the following implementation-only contract in
  `rt_terrain3d_internal.h`:

  ```c
  typedef struct rt_terrain3d_grid_info {
      int32_t width;
      int32_t depth;
      double spacing_x;
      double height_scale;
      double spacing_z;
      double extent_x;
      double extent_z;
  } rt_terrain3d_grid_info;

  int8_t rt_terrain3d_get_grid_info_internal(
      void *terrain, rt_terrain3d_grid_info *out_info);
  ```

  The function returns zero and zeroes `out_info` for a null, wrong-class, or
  corrupt terrain. A successful result has `width, depth >= 2`, finite positive
  X/Z spacing, finite height scale, and finite extents computed as
  `(dimension - 1) * spacing`. The header exposes no Terrain3D payload fields.
- Similar new internal boundaries use the suffix `_internal` and live beside
  the owning module. Every new header carries the complete project source
  header and Doxygen ownership, lifetime, argument, return, and invariant
  documentation.

### Backend render-target lifetime amendment

The backend audit adds an implementation-only lifetime notification to
`vgfx3d_rendertarget_t`. This closes the ownership gap created when a native
backend cache retains textures but only borrows the GC-managed target shell.
It does not add a registry row or supported embedding API.

`rt_canvas3d_internal.h` defines:

```c
typedef void (*vgfx3d_rendertarget_release_fn)(
    void *userdata, vgfx3d_rendertarget_t *target);

struct vgfx3d_rendertarget {
    /* existing fields */
    vgfx3d_rendertarget_release_fn release_backend;
    void *release_backend_userdata;
};

static inline void vgfx3d_rendertarget_clear_backend_release(
    vgfx3d_rendertarget_t *target);
static inline void vgfx3d_rendertarget_release_backend(
    vgfx3d_rendertarget_t *target);
```

The runtime finalizer invokes `vgfx3d_rendertarget_release_backend` while the
shell, cache identity, dimensions, and CPU buffers are still valid. The helper
clears both release fields before calling the backend, making repeated or
re-entrant cleanup idempotent. A backend that installs the hook owns the one
native cache association for that shell; transferring the target to another
native owner first invokes the prior hook.

Backend context teardown clears its hooks from every live target before ARC or
COM/GL state disappears. Cache eviction must synchronize a dirty color mirror
before discarding its only native copy. If synchronization fails, the target
remains dirty with either its retry-capable native entry or no callback, so a
later read fails explicitly instead of returning stale bytes as current.

### Retained derived work

- Mesh geometry publication creates an immutable reference-counted revision.
  Snapshot commands retain revisions, mesh mutation uses copy-on-write, and
  derived triangle BVHs/tangent streams are keyed by revision.
- Pixels alpha classification is cached by its content generation. Texture
  mips keep reconstructable per-level backing state so residency can actually
  release decoded memory.
- Navigation queries own or lease mutable A* workspaces. A NavMesh3D does not
  serialize unrelated read-only queries on one mutable scratch block.
- Hardware particle rendering retains unit geometry and sends compact instance
  data. The CPU-expanded billboard path remains the software fallback.

### Determinism and bounded work

- Checked `size_t`/`uint64_t` arithmetic precedes allocation and replication.
  If a spatial acceleration structure would exceed its budget, the operation
  either uses a bounded correctness-preserving fallback or fails without
  replacing the prior structure.
- Batched navigation avoidance snapshots all inputs, solves from that immutable
  snapshot, and applies output in stable runtime-handle order.
- Fixed-step and catch-up code records unprocessed or explicitly dropped time;
  it does not silently consume time after a failed step or clamp a large update
  without telemetry.

The complete behavioral requirements and test matrix are normative for this
program in
`docs/internals/graphics3d-runtime-hardening-2026-07.md`.

### Retained mesh revision and ray-query boundary amendment

Requirements 22–24 add an internal native ownership boundary; they do not add
or remove a scripting registry row. A heap `Mesh3D` lazily publishes one
`rt_mesh3d_geometry_revision` for its current geometry revision. The object
contains exact-size immutable vertex/index copies, an atomic native reference
count, and an optional separately allocated tangent-bearing vertex variant.
The mesh owns one reference. Each Canvas3D frame-snapshot entry that queues the
revision acquires one additional reference and releases it before the frame
snapshot table resets.

The following implementation-only C functions live in
`rt_canvas3d_internal.h`:

```c
rt_mesh3d_geometry_revision *rt_mesh3d_get_retained_geometry(
    rt_mesh3d *mesh);
void rt_mesh3d_geometry_revision_retain(
    rt_mesh3d_geometry_revision *revision);
void rt_mesh3d_geometry_revision_release(
    rt_mesh3d_geometry_revision *revision);
void rt_mesh3d_invalidate_retained_geometry(rt_mesh3d *mesh);
int rt_mesh3d_geometry_revision_ensure_tangents(
    rt_mesh3d *mesh, rt_mesh3d_geometry_revision *revision);
```

All geometry mutation paths detach the mesh-owned revision before incrementing
`geometry_revision`. A queued canvas reference therefore keeps the previous
bytes alive while a later draw lazily forks a new revision. Unchanged later
frames reuse the mesh-owned revision and do not allocate frame vertex/index
copies. Camera-relative rebase geometry, caller-owned transient facades,
skinning, and morphing keep their existing explicit dynamic snapshot paths.

The raw backend geometry identity remains the heap mesh handle. A generated
tangent variant uses a distinct stable key within that mesh and the same source
revision, allowing raw and generated-tangent uploads to coexist in Metal,
Direct3D 11, and OpenGL caches. Tangent generation reads only immutable
position, normal, UV, and index bytes; any edit to those inputs increments the
source revision and invalidates both variants. Stack facades used for tangent
generation suppress global Mesh3D mutation notifications because they do not
change authored scene geometry.

Scene `Ray3D.IntersectMesh` owns a separate local-space triangle BVH in the
private mesh payload, keyed by `geometry_revision`. Identity and invertible
transform queries traverse that retained tree near-first. A singular transform
or allocation failure preserves the prior exact world-space linear fallback.
Equal-distance triangles resolve to the lower source triangle index, so BVH
traversal is result-equivalent to the former ascending brute-force scan. The
private lifetime counters for BVH rebuild/probes and retained geometry/tangent
builds/hits exist solely for deterministic CTest assertions and are not runtime
ABI or registry surface.

### Navigation query and batch ABI amendment

Requirement 42 adds one embedding-level C API without registering a new
scripting method:

```c
int64_t rt_navagent3d_update_batch(
    void *const *agents, int64_t agent_count, double dt);
```

The borrowed handle array may contain invalid or duplicate entries. The
implementation filters those entries, sorts the remaining agents by monotonic
creation order, prepares every path-following velocity, snapshots the complete
live agent registry, solves every selected agent from that immutable snapshot,
then publishes velocities and positions in the same stable order. The return
value is the number of unique valid handles processed. Individual
`rt_navagent3d_update` and the registered `NavAgent3D.Update` method remain
available and retain their single-agent behavior. The batch API is main-thread
only because agent bindings and the live spatial hash are main-thread-owned.

Requirements 41 and 43 retain a fixed pool of four independently mutable A*
workspaces per navmesh. Searches claim one slot and wait only when all four are
occupied. Off-mesh sampling starts at the query's nearest grid cell, expands
rectangular cell rings until a conservative unvisited-distance bound proves the
current closest point final, and falls back to a whole-mesh pass only when the
grid is unavailable, corrupt, or exceeds the fixed 4,096-cell/131,072-reference
query budget. The fallback remains exact and is bounded by the validated live
triangle count.

### Internal verification symbols

The navigation transaction/query batches export the following C test hooks.
They are never registered as runtime methods and are not supported embedding
APIs:

- `rt_navmesh3d_test_set_query_grid_alloc_failure`;
- `rt_navmesh3d_test_set_adjacency_alloc_failure`;
- `rt_navmesh3d_test_voxel_grid_dimensions`;
- `rt_navmesh3d_test_get_last_sample_probe_count`;
- `rt_navmesh3d_test_get_last_sample_used_fallback`;
- `rt_navmesh3d_test_reset_path_peak_concurrency`;
- `rt_navmesh3d_test_get_path_peak_concurrency`;
- `rt_navmesh3d_test_get_ready_path_scratch_count`;
- `rt_navmesh3d_test_get_path_workspace_permits`;
- `rt_navagent3d_test_set_batch_bucket_alloc_failure`;
- `rt_scene3d_test_set_precise_hit_growth_failure`.

The first two inject failure before staged state can be published. The third
executes production grid-dimension arithmetic without allocating the resulting
grid. The next four report the latest sample's conservative probe/fallback
metrics and the resettable peak count of overlapping A* workspace owners. Those
hooks exist solely to make requirements 39–43 deterministic under CTest.
The added scratch/permit readers verify that bounded path queries block without
spinning and retain their corridor/funnel work arrays. The two failure injectors
prove that NavAgent3D batch preflight and precise SceneGraph collect-all queries
remain transactional when native result storage cannot grow.

### Particles3D timing and terminal-frame ABI amendment

Requirements 46 and 47 add the following public registry rows without changing
or removing an existing row:

| Runtime name | C symbol | Signature |
|---|---|---|
| `Zanna.Graphics3D.Particles3D.set_RenderFinalFrame` | `rt_particles3d_set_render_final_frame` | `void(obj,i1)` |
| `Zanna.Graphics3D.Particles3D.get_RenderFinalFrame` | `rt_particles3d_get_render_final_frame` | `i1(obj)` |
| `Zanna.Graphics3D.Particles3D.get_DroppedTime` | `rt_particles3d_get_dropped_time` | `f64(obj)` |
| `Zanna.Graphics3D.Particles3D.get_LastDroppedTime` | `rt_particles3d_get_last_dropped_time` | `f64(obj)` |
| `Zanna.Graphics3D.Particles3D.get_ResidualTime` | `rt_particles3d_get_residual_time` | `f64(obj)` |
| `Zanna.Graphics3D.Particles3D.ResetDroppedTime` | `rt_particles3d_reset_dropped_time` | `void(obj)` |

`RenderFinalFrame` defaults to true for visual compatibility, but terminal
particles are no longer counted as live. They are copied into a terminal
snapshot, rendered for the update interval in which they expired, and discarded
by the next valid update. `DroppedTime` is cumulative since construction or
`ResetDroppedTime`; `LastDroppedTime` describes the latest update call; and
`ResidualTime` is the sub-step remainder carried into the next call.

### Compact particle renderer boundary amendment

Requirement 48 adds an internal renderer command contract, not a public
runtime registry row. `vgfx3d_particle_instance_t` is exactly 64 bytes and is
laid out as four contiguous `float[4]` lanes in this order:

```c
typedef struct {
    float center[4];
    float right[4];
    float up[4];
    float color[4];
} vgfx3d_particle_instance_t;
```

`center` is already in the frame's camera-relative render space. `right` and
`up` are half-axis vectors, so retained unit corner `(x, y)` expands to
`center + x * right + y * up`. `center.w` is 1, the right/up `w` lanes are zero
and reserved, and `color.w` carries alpha; none changes the 64-byte stride.

The backend vtable advertises support with the internal
`particle_instancing` bit. A supporting backend accepts a null matrix payload
for an instanced command only when `vgfx3d_draw_cmd_t.particle_instances` is
non-null. Canvas3D validates every record, copies it into frame-owned storage,
retains material dependencies transactionally, computes conservative aggregate
bounds, and queues it against one process-lifetime four-vertex/six-index unit
quad with a stable geometry-cache identity. Metal, D3D11, and OpenGL opt in and
bind the same four lanes; software deliberately does not opt in and continues
to receive CPU-expanded billboards. Particle trail ribbons also remain
CPU-expanded because their endpoints carry independent width and alpha.

The implementation-only symbols
`rt_particles3d_test_instance_scratch_capacity` and
`rt_particles3d_test_instance_scratch_grow_count` expose retained scratch
telemetry solely to CTest. They are not registered runtime methods or supported
embedding APIs.

### Canvas3D submission diagnostics ABI amendment

Requirement 25 adds the following public registry rows without changing the
return type or trapping behavior of any existing draw method:

| Runtime name | C symbol | Signature |
|---|---|---|
| `Zanna.Graphics3D.Canvas3D.get_LastSubmissionStatus` | `rt_canvas3d_get_last_submission_status` | `i64(obj)` |
| `Zanna.Graphics3D.Canvas3D.get_SubmissionFailureCount` | `rt_canvas3d_get_submission_failure_count` | `i64(obj)` |
| `Zanna.Graphics3D.Canvas3D.ResetSubmissionDiagnostics` | `rt_canvas3d_reset_submission_diagnostics` | `void(obj)` |

`LastSubmissionStatus` is a sticky diagnostic for the most recently observed
submission failure: zero means no failure has been recorded since construction
or reset, 1 means deferred-command queue capacity/allocation failure, 2 means a
geometry snapshot was denied by its byte budget or allocation, 3 means
temporary resource-ownership bookkeeping could not grow, and 4 means an
instance-payload allocation failed. A successful later draw does not clear the
status. `SubmissionFailureCount` is a saturating count of recorded failures
since construction or `ResetSubmissionDiagnostics`; reset clears both values.
This makes failures observable even when a legacy `void` draw traps or takes a
documented degraded path, while preserving its established compatibility
behavior.

The implementation-only functions
`rt_canvas3d_test_fail_next_queue_reserve` and
`rt_canvas3d_test_fail_next_mesh_snapshot` arm one-shot failures before the
corresponding transaction publishes any new command or snapshot. They exist
solely for deterministic CTest coverage and are not registered runtime methods
or supported embedding APIs.

### Cross-backend render-scale contract amendment

Requirement 28 implements the existing `set_render_scale` backend hook on
Metal, Direct3D 11, and OpenGL; it adds no runtime registry row and does not
change the hook's C signature. A reduced scale affects only a window-backed 3D
scene pass. Explicit render targets retain their authored dimensions, while
HUD/final-overlay passes and the presented/read-back image retain the logical
output dimensions.

All four renderers use the implementation-only helper
`vgfx3d_compute_scaled_scene_extent`. It accepts a finite scale in the closed
range `[0.25, 1]`, positive logical output dimensions, and two non-null output
pointers. Each scaled dimension is `floor(output_dimension * scale)`, clamped
to at least one pixel; scale 1 returns the exact logical dimensions. Invalid
input returns zero after clearing both outputs. This is the normative sizing
rule for software and hardware adapters.

For a reduced scale, Metal, Direct3D 11, and OpenGL render scene color, motion,
and depth into targets at that computed extent. Presentation uses an explicit
full-screen sampled composite whose destination viewport is the logical output
extent; post-processing may be folded into that composite. Overlay targets are
logical-output-sized and are composited after scene upscaling. Screenshot
readback observes the same logical-output-sized composite rather than a clipped
scaled scene. Changing scale invalidates size-dependent scene, post-processing,
and temporal-history resources before the next scene begins. If a backend
cannot construct the required route, its hook reports failure and preserves or
restores the prior scale. Canvas3D publishes its `RenderScale` property only
after that hook succeeds, including transitions back to scale 1, so a busy or
allocation-failing adapter cannot leave the public property out of sync with
its live targets. A fixed-scale backend without the optional hook still accepts
scale 1 as a compatibility no-op.

The existing software path remains supported and uses the same dimension
helper before its established nearest-neighbor output expansion. Platform-free
CTest covers the exact sizing/validation contract; the Metal adapter is built
and exercised on macOS, while the Direct3D 11 and OpenGL adapters retain their
platform-specific build/smoke coverage on Windows and Linux respectively.

### glTF root, sampler-axis, and UV-stream boundary amendment

Requirements 1–7 add no public registry rows. Synchronous and worker-preload
loading share `gltf_parse_root_document_view`, a bounded borrowed-span parser
for plain JSON and GLB 2.0. A GLB contains one non-empty JSON chunk first and
at most one BIN chunk second. Unknown/duplicate chunks, literal JSON NUL bytes,
misalignment, bounds overflow, truncated chunk headers, declared-length drift,
and trailing bytes fail before a preload bundle or runtime asset is published.
The allocation-free JSON scanner validates complete recursive container
grammar and exact integral tokens before the runtime JSON DOM is constructed.

Imported material sampler state retains the three independent glTF axes:
minification, magnification, and mip selection. The private `rt_material3d`
payload, immutable draw snapshot, backend command, deferred sort key, and each
hardware sampler cache carry all three axes. The existing legacy `filter`
field/API remains available and maps one value to both texel axes while
selecting the non-mipmapped compatibility path. glTF import uses this
implementation-only extension instead:

```c
void rt_material3d_set_import_texture_slot_sampler_axes(
    void *material,
    int64_t slot,
    int64_t uv_set,
    double offset_u,
    double offset_v,
    double scale_u,
    double scale_v,
    double rotation,
    int64_t wrap_s,
    int64_t wrap_t,
    int64_t min_filter,
    int64_t mag_filter,
    int64_t mip_filter);
```

The axis constants remain the existing nearest/linear values; mip additionally
uses `RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE` when glTF `minFilter` selects no
mipmap sampling. Metal maps the axes directly, Direct3D 11 selects the exact
MIN/MAG/MIP filter tuple, and OpenGL configures independent MIN/MAG filters
with the appropriate mipmapped MIN enum only when mip selection is enabled.
Anisotropy may replace the authored min/mag tuple only for the existing
explicit anisotropy opt-in.

Each parsed glTF texture-info slot also retains whether it was authored and its
unclamped UV-set index. Before any primitive is committed, the loader checks
the base and variant materials against the primitive's successfully decoded
`TEXCOORD_0`/`TEXCOORD_1` streams. UV set 2 or greater is unsupported; a
referenced but missing UV0/UV1 stream is corrupt input. Both failures record a
named asset diagnostic and roll back the whole asset rather than copying UV0
into UV1 or silently changing a material to UV0.

### KTX2 capability, degradation, and residency ABI amendment

Requirements 8–12 preserve the two existing permissive KTX2 loaders and add
the following registry rows:

| Runtime name | C symbol | Signature |
|---|---|---|
| `Zanna.Graphics3D.TextureAsset3D.LoadKtx2Strict` | `rt_textureasset3d_load_ktx2_strict` | `obj(str)` |
| `Zanna.Graphics3D.TextureAsset3D.LoadKtx2AssetStrict` | `rt_textureasset3d_load_ktx2_asset_strict` | `obj(str)` |
| `Zanna.Graphics3D.TextureAsset3D.get_Degraded` | `rt_textureasset3d_get_degraded` | `i1(obj)` |
| `Zanna.Graphics3D.TextureAsset3D.get_DegradedReason` | `rt_textureasset3d_get_degraded_reason` | `str(obj)` |
| `Zanna.Graphics3D.TextureAsset3D.get_RetainedBytes` | `rt_textureasset3d_get_retained_bytes` | `i64(obj)` |

A permissive load may substitute the established magenta/black checker only
when a recognized compressed format cannot be decoded on the CPU. Such an
asset reports `Degraded = true` and the stable, machine-comparable reason
`cpu_decode_failed`; its existing warning retains the human-readable format
and decoder detail. The equivalent strict loader records an unsupported-data
asset error and returns no object. All other successful assets report false
and an empty reason. The implementation-only memory loader gains the parallel
`rt_textureasset3d_load_ktx2_memory_strict` entry point for importer and CTest
parity; it is not a registry row.

One immutable format-capability table is authoritative for Vulkan format
matching, normalized name, block geometry, decoder kind, CPU support quality,
native format id, and backend capability bit. CPU support quality is `NONE`,
`PARTIAL`, or `FULL`: ASTC is partial because the zero-dependency fallback
covers LDR void-extent blocks, ETC2 is partial while T/H/planar color modes are
native-only, and BC6H is partial because its complete block decoder is exposed
as a clamped RGBA8 fallback rather than an HDR-preserving surface. The legacy
boolean support query treats both partial and full support as available. Test
enumeration helpers expose read-only copies of the table; backend upload
selection consumes the same row-derived capability bit rather than maintaining
another format switch.

The shared compression layer adds exact-destination, implementation-only
helpers for zlib-wrapped DEFLATE and Zstandard frames. The zlib helper validates
CM, CINFO, FCHECK, and FDICT; consumes exactly the bytes preceding the four-byte
trailer; requires the exact caller-provided output length; and verifies the
big-endian Adler-32 before success. KTX2 allocates one final canonical backing
span for all levels and decodes each supercompressed level directly into its
assigned slice. The parser therefore never allocates a second whole-level
output solely to copy it into retained storage.

Each TextureAsset3D owns that immutable canonical backing plus per-level
fallback state. Moving the resident window first reconstructs every entering
decoded mip from the backing, then releases decoded Pixels outside the new
window. Native payload views borrow immutable slices from the same backing and
remain valid for deferred upload snapshots. `ResidentBytes` measures the
active upload/fallback window; `RetainedBytes` measures the canonical backing
plus decoded Pixels still alive, and therefore falls when decoded mips are
evicted while remaining nonzero enough to reconstruct them. BasisLZ assets
capture their decoded RGBA8 levels into the same canonical representation so
they obey the same residency contract.

Implementation-only CTest hooks report the final canonical supercompression
allocation and its peak live destination bytes. They carry no allocator
replacement semantics and are not registered runtime methods; their sole
purpose is proving that a large level does not coexist with a redundant
whole-level intermediate.

### FBX typed-text, budget, indexing, transform, and curve amendment

Requirements 13–19 add no public registry rows. Binary and ASCII FBX inputs
produce the same owned `fbx_node_t`/`fbx_prop_t` document graph before geometry,
material, scene, skeleton, skin, morph, or animation extraction begins. The
ASCII reader is a length-aware lexer/parser: comments, quoted strings, numbers,
punctuation, and braces are distinct tokens; every lookup is performed on a
parsed node's direct children. Array declarations are count-checked and folded
into the same typed array properties emitted by the binary reader. Text found
inside comments, strings, or a closed sibling scope can therefore never satisfy
an object or property lookup.

Every load owns one `fbx_load_context_t`. Its checked byte budget is charged
before retaining file bytes, node/property metadata, decoded string/raw
properties, expanded numeric arrays, object/connection indexes, and generated
animation sample storage. The default budget is 1 GiB and may only be lowered
by `ZANNA_FBX_MAX_LOAD_BYTES`; the existing file-size limit remains independent.
Any addition or multiplication that would overflow, or any charge exceeding the
budget, records a recoverable too-large asset error and publishes no asset.

The parsed `Objects` node owns a load-local open-addressed numeric-id index.
The connection table owns independent open-addressed endpoint indexes whose
entries head stable parent and child adjacency lists. Duplicate object IDs are
invalid. Import passes resolve identity only through numeric IDs; display names
are never a fallback identity key. Object and node names are length-carrying,
dynamically allocated strings charged to the load budget. Display-name decoding
stops at the first embedded NUL/type suffix, strips the last namespace prefix,
and creates an exact-length runtime string, so long names with equal prefixes
remain distinct.

The private `vgfx3d_bone_t` payload likewise replaces its fixed 64-byte name
array with one retained `rt_string`. `Skeleton3D.AddBone` retains the complete
runtime string, `GetBoneName` returns a retained reference, exact lookup and
retarget matching compare the full byte length, and the skeleton finalizer
releases every published name. Scene VSCN load/save converts through that same
owned field, so persistence no longer reintroduces the former prefix collision.
This changes no registry signature or public C structure: `vgfx3d_bone_t`
remains private to the animation runtime.

Static scene construction, skeleton bind extraction, and animation sampling all
use `fbx_transform_components_t` plus the same matrix composer. At each animation
sample, authored Lcl T/R/S curves replace only those lanes in the model's full
base component record; rotation order, active state, pre/post rotation, offsets,
pivots, geometric transform, and inheritance mode remain intact before matrix
decomposition and optional axis conversion.

Constant curve segments add the immediately preceding representable FBX tick,
making the step exact at stored tick resolution. Linear segments add no derived
samples. Cubic Hermite segments are recursively subdivided until both a 1 ms
maximum time span and a mixed absolute/relative value error of `1e-4 + 1e-4 *
max(|value|)` are satisfied, subject to the existing global key/sample ceiling
and load budget; inability to satisfy either bound rejects the animation load
rather than silently publishing a coarse curve.

Private animation key times are stored as `double`, preserving distinct FBX
ticks after conversion to seconds. The public compatibility entry point
`rt_animation3d_add_keyframe` remains `void`; it delegates to the internal
status-returning `rt_animation3d_try_add_keyframe`. Transactional importers call
the latter so allocation or channel/key capacity failure aborts and rolls back
the staged clip instead of publishing a truncated animation. This internal
dependency is declared in `rt_skeleton3d_internal.h` and adds no registry row.

The following implementation-only CTest hooks are not registry methods or a
supported embedding ABI:

- `rt_fbx_test_set_load_budget_bytes` lowers the budget of the next load on the
  calling thread (zero restores the default);
- `rt_fbx_test_get_last_budget_used_bytes` reports the final charged amount;
- `rt_fbx_test_get_last_lookup_probe_count` reports object-hash and endpoint-
  adjacency hash probes from the most recent load.

These hooks make aggregate-budget rejection and near-linear graph resolution
deterministic without replacing the runtime allocator.

### Game3D scene-root transfer amendment

Requirement 20's prohibition on reparenting a `Scene3D` implicit root remains
absolute. Game3D import and world streaming therefore do not weaken the scene
graph invariant. When `rt_game3d_entity_from_node` receives an implicit root,
it allocates a replacement empty `SceneNode3D` and preflights a complete
`scene_owner_transaction` over the old hierarchy before publishing any change.
The commit installs the replacement as the source scene's owned root, transfers
the source scene's existing retained reference for the old root directly into
the new `Entity3D`, and clears matching owner links across the transferred
hierarchy without allocation. The source scene remains a valid one-node scene;
preflight failure leaves its root, counts, parent links, and every owner field
unchanged.

This is an internal Game3D-to-Scene3D dependency and adds no registry or public
C ABI row. Ordinary detached nodes and non-root scene nodes retain the existing
`FromNode` behavior. World streaming continues to retain its loader scene for
metadata and teardown compatibility, but resident-byte accounting, HLOD proxy
baking, and impostor traversal use the detached hierarchy owned by the spawned
entity. Deterministic CTest coverage exercises successful transfer/spawn and an
injected owner-transaction growth failure.

The typed FBX importer also treats a declared `Geometry` of type `Mesh` that
cannot produce one valid triangle as corrupt input. Shape/non-mesh geometry is
still handled by its dedicated passes. This clarifies error classification only:
it adds no runtime surface and prevents malformed supported FBX content from
being published as a successful empty asset.

### Mesh simplification metadata and topology ABI amendment

Requirements 30–32 keep `Mesh3D.Simplify(target)` as the object-returning
compatibility API and add three read-only properties to the returned mesh:

| Runtime name | C symbol | Signature |
|---|---|---|
| `Zanna.Graphics3D.Mesh3D.get_SimplifyRequestedTriangles` | `rt_mesh3d_get_simplify_requested_triangles` | `i64(obj)` |
| `Zanna.Graphics3D.Mesh3D.get_SimplifyAchievedTriangles` | `rt_mesh3d_get_simplify_achieved_triangles` | `i64(obj)` |
| `Zanna.Graphics3D.Mesh3D.get_SimplifyStatus` | `rt_mesh3d_get_simplify_status` | `i64(obj)` |

`SimplifyStatus` is 0 (`NOT_RUN`) for an ordinary mesh, 1 (`COMPLETE`) when
the achieved triangle count is no greater than the sanitized requested count,
and 2 (`PARTIAL`) when preserving topology or classified boundaries makes the
requested count unreachable. A partial result remains a valid new Mesh3D and
is returned normally; allocation or invalid-input failure still returns null.
The requested value is clamped to at least one, and the achieved value is the
exact triangle count in the returned index buffer. A later geometry mutation
clears all three diagnostics back to the not-run state so they cannot become
stale.

The private Mesh3D payload gains an owned array of
`rt_mesh3d_submesh_range` records:

```c
typedef struct rt_mesh3d_submesh_range {
    uint32_t first_index;
    uint32_t index_count;
    int32_t material_slot;
} rt_mesh3d_submesh_range;
```

Ranges are ascending, non-overlapping, triangle-aligned spans of the mesh's
index buffer. Simplification keeps surviving faces in source order, removes
empty spans, adjusts every range to the compacted index buffer, and coalesces
adjacent spans carrying the same material slot. The metadata is private and
adds no direct scripting API in this amendment.

Subset placement retains the surviving endpoint's complete interleaved vertex
record (normal, tangent, both UV sets, color, and primary joint/weight lanes).
The corresponding authoritative double position and influences 5–8 record are
copied through the same new-to-source map. `bone_map`, `compact_streams`, and
the owned Skeleton3D reference are retained. An attached MorphTarget3D is
deep-cloned through the implementation-only function
`rt_morphtarget3d_clone_remapped(source, new_to_old, new_vertex_count)`, which
remaps position, normal, and tangent deltas plus weight history. Its result has
exactly the simplified vertex count. The simplifier never publishes an output
whose required side stream or owned reference could not be staged.

Every candidate collapse must satisfy the triangular-manifold link condition.
The candidate edge has one or two incident faces; both endpoint fans must each
be connected manifolds; and their one-ring intersection must be exactly the
opposite vertices of the incident faces. Open edges, attribute seams, and
material discontinuities are classified boundaries. A collapse may proceed
along a classified boundary only when both endpoints are boundary vertices;
an unclassified interior edge may proceed only when neither endpoint lies on
a boundary. Before publication the simulated local faces must remain
non-degenerate, preserve orientation, and remain unique as unordered vertex
triples. This rejects non-manifold edges, bow-tie fans, boundary pinches,
duplicate faces, and inverted faces while allowing the ordinary manifold QEM
path to return a deterministic partial result when no legal edge remains.

## Consequences

- Private object layouts can evolve without silently corrupting sibling
  modules and isolated test doubles.
- OOM and malformed-input behavior becomes retryable and deterministic instead
  of leaving half-built live objects.
- Static scenes retain more small metadata objects (revisions, cache keys, and
  optional acceleration structures) but avoid repeated whole-mesh copies and
  scans in hot frames.
- Registry additions require synchronized updates to runtime definitions,
  graphics-disabled stubs, ABI/manifest contract tests, and generated runtime
  documentation.
- Coherent retained-geometry, FBX-connection, NavMesh query-grid, and regression
  sections are split into dedicated include/source units so no new mega-file or
  manual-allocation-hotspot debt is accepted. The source-health baseline rises
  only for two intentional inventory changes: runtime API contract files
  `857 -> 859` (the Terrain3D internal header and split TextureAsset3D stub
  translation unit) and graphics-disabled stub functions `1143 -> 1159` (the
  complete additive ABI). Stub classification improves from 303 to 293
  unclassified entries.
- Some implementation batches are larger because all three GPU backends and
  the software fallback must preserve one command contract.

## Alternatives Considered

- Expose complete Terrain3D, Mesh3D, or NavMesh3D payloads in shared headers:
  rejected because it would turn private layout into a de facto ABI and repeat
  the defect that motivated the change.
- Keep existing APIs silent and only log failures: rejected because callers
  cannot test, recover, or report degraded rendering reliably.
- Reparse/rebuild data on every query or frame to avoid retained state:
  rejected because the audited hot paths are proportional to total asset or
  mesh size even when nothing changed.
- Introduce third-party import, compression, or geometry libraries: rejected
  by the zero-dependency product policy.
