---
status: active
audience: contributors
last-verified: 2026-07-29
---

# Graphics3D Backend Correctness Audit (July 2026)

This audit covers the C/Objective-C runtime under `src/runtime/graphics/3d/`,
with focused review of the OpenGL 3.3, Direct3D 11, Metal, and software
implementations. It supplements the broader [Graphics3D runtime hardening
program](graphics3d-runtime-hardening-2026-07.md).

The review combined line-by-line ownership and arithmetic analysis, backend
parity comparisons, shader review, whole-tree cppcheck analysis, targeted unit
tests, source-contract tests for platform-excluded translation units, and
headless production renders. The 350 findings below are fixed; none changes a
registered scripting API.

The current source-wide extension begins at G3D-249. G3D-249 through G3D-348
track the requested 100 distinct failure modes found and fixed in this pass;
the subsequent entries record additional analyzer and final-review findings.
Repeated unsafe sites that share one root cause are consolidated within an entry.

## Regression suites

The evidence column uses these abbreviations:

| Tag | Coverage |
|-----|----------|
| `U` | `test_vgfx3d_backend_utils` shared validation, arithmetic, conversion, and layout cases |
| `GL` | `test_vgfx3d_backend_opengl_shared`, including concatenated OpenGL source contracts |
| `D3` | `test_vgfx3d_backend_d3d11_shared`, including concatenated D3D11 source contracts |
| `MTL` | `test_vgfx3d_backend_metal_shared`, including concatenated Metal source contracts |
| `PROD` | `test_rt_canvas3d_production` deterministic software rendering and lifetime checks |
| `GLTF` | `test_rt_gltf` import validation plus whole-Graphics3D cppcheck analysis |
| `DRACO` | `test_rt_gltf_draco_internal` checked-arithmetic and malformed-stream regressions, plus `test_rt_gltf_draco_probe` |
| `FBX` | `g3d_test_fbx_ascii` plus whole-Graphics3D cppcheck analysis |
| `GAME` | Focused Game3D combat, persistence, audio, timeline, third-person, and cloth unit suites |
| `NAV` | `test_rt_navmesh_blend` navmesh metadata, serialization, and defensive-count coverage |
| `TERRAIN` | `test_rt_terrain3d_upgrades` hole rasterization, extreme-coordinate, and growth coverage |
| `RIDGE` | `zia_smoke_ridgebound` Metal release-scene luminance, coverage, and frame-budget gate |
| `ASH` | `zia_visual_ashfall_metal` authored multi-light scene and HDR post-FX visual gate |

## Findings and resolutions

| ID | Area | Class | Finding and implemented resolution | Evidence |
|----|------|-------|------------------------------------|----------|
| G3D-001 | Shared | Correctness | A zero Pixels cache key could alias the sentinel for “no generation.” Generations now normalize zero to one. | `U` |
| G3D-002 | Shared | Correctness | Native texture mip snapshots could publish partially initialized outputs after validation failed. Output metadata is now cleared first and published transactionally. | `U` |
| G3D-003 | Shared | Correctness | Cubemap generation zero had the same sentinel collision. Cubemap generations now normalize zero to one. | `U` |
| G3D-004 | Shared | Performance | Row flipping allocated and freed a heap buffer for each copy. It now swaps fixed-size stack chunks with no heap traffic. | `U` |
| G3D-005 | Shared | Correctness | Positive infinity converted to black in HDR-to-UNORM conversion. It now saturates to 255 while NaN and negative values remain safely bounded. | `U` |
| G3D-006 | Shared | Bug | Pixel-copy layout math mixed signed byte counts and element counts, permitting overflow or a short row. It now uses checked `size_t` byte products. | `U` |
| G3D-007 | Shared | Portability | RGBA16F-to-RGBA8 readback dereferenced potentially unaligned `uint16_t` pixels. Each pixel is now loaded with `memcpy`. | `U` |
| G3D-008 | Shared | Portability | RGBA16F-to-RGBA32F had the same unaligned access. It now uses alignment-safe local half words. | `U` |
| G3D-009 | Shared | Correctness | Matrix inversion could leave a partial result or publish non-finite values. It now stages into a temporary and leaves the destination unchanged on failure. | `U` |
| G3D-010 | Shared | Correctness | Finite but enormous matrices overflowed downstream shader arithmetic. Matrix copies now enforce a component bound and use a deterministic fallback. | `U`, `GL`, `D3`, `MTL` |
| G3D-011 | Shared | Correctness | All-zero shadow matrices passed finite-only checks. Shadow transforms now require a bounded, non-zero usable matrix. | `U`, `GL`, `D3`, `MTL` |
| G3D-012 | Shared | Correctness | Invalid workflow, alpha, shading, shadow, wrap, filter, mip, and UV-set enum values reached backend switches and shaders. Shared sanitizers now canonicalize every discriminator. | `U` |
| G3D-013 | Shared | Correctness | NaN, infinity, invalid flags, and out-of-range material scalars could enter native draw snapshots. A complete draw-command sanitizer now guards every backend entry. | `U`, `GL`, `D3`, `MTL`, `PROD` |
| G3D-014 | Shared | Correctness | Invalid camera basis vectors, clipping planes, and matrices could poison an entire frame. Camera snapshots now receive bounded finite fallbacks. | `U` |
| G3D-015 | Shared | Correctness | Invalid light vectors, colors, attenuation, cone values, and emitter dimensions reached lighting math. Light arrays and ambient RGB are now sanitized before upload. | `U` |
| G3D-016 | Shared | Bug | Light shadow spans could reference incomplete or nonexistent native slots. Spans are now clipped to the completed shadow range. | `U`, `GL`, `D3`, `MTL` |
| G3D-017 | Shared | Bug | Clustered-light tables trusted revision, count, offset monotonicity, and indices. A shared validator now rejects every malformed table before upload. | `U`, `GL`, `D3`, `MTL` |
| G3D-018 | Shared | Bug | Post-FX chains trusted count, pointer, discriminator, and effect payload structure. Chains are now structurally validated before any backend traversal. | `U`, `GL`, `D3`, `MTL` |
| G3D-019 | Shared | Correctness | Post-FX snapshots could contain non-finite or unbounded effect parameters. A shared snapshot sanitizer now supplies per-field limits and defaults. | `U`, `GL`, `D3`, `MTL` |
| G3D-020 | Shared | Correctness | GPU reversed-depth probes could publish NaN, infinity, or values outside `[0,1]`. Publication now uses one finite clamping helper. | `U`, `GL`, `D3`, `MTL` |
| G3D-021 | Shared | Bug | Native compressed uploads accepted noncanonical BC/ETC/ASTC block descriptions. Format/block geometry is now validated against the exact format contract. | `U` |
| G3D-022 | Shared | Bug | Compressed mip chains did not prove exact halving or total payload coverage. Every level and byte span is now checked before native API calls. | `U`, `GL`, `D3`, `MTL` |
| G3D-023 | Shared | Bug | Prefiltered cubemap tails accepted impossible base levels or excessive mip counts. One exact IBL-layout validator is shared by all GPU backends. | `U`, `GL`, `D3`, `MTL` |
| G3D-024 | Shared | Performance | Whole-resource overlays could start after exhausting the per-frame upload budget. A shared whole-upload admission check prevents partial resource publication and starvation. | `U`, `GL`, `D3`, `MTL` |
| G3D-025 | OpenGL | Bug | The clip-control convention was process-global mutable state, so two contexts could compile incompatible shaders. It is now stored and compiled per context. | `GL` |
| G3D-026 | OpenGL | Bug | Shader source assembly trusted null chunks, negative counts, and overflowing lengths. The compiler helper now validates and bounds the complete source. | `GL` |
| G3D-027 | OpenGL | Bug | Shader/program diagnostic buffers could be read without NUL initialization after a driver returned no log. Buffers now start zeroed. | `GL` |
| G3D-028 | OpenGL | Correctness | Tangents were transformed by the normal matrix, breaking handedness under non-uniform scale. The shader now uses the model linear transform. | `GL` |
| G3D-029 | OpenGL | Correctness | Bone, object, camera, shadow, inverse, and history matrices reached uniforms without uniform finite/bounded policy. All matrix upload paths now use shared guarded copies. | `GL` |
| G3D-030 | OpenGL | Bug | Perspective/cube shadow projection mishandled `w`, invalid projection kinds, and unusable matrices. Projection is now type-aware and failure clears its output. | `GL` |
| G3D-031 | OpenGL | Correctness | Depth probe NDC coordinates were not finite-clamped, and readback always sampled the scene FBO even during RTT/direct rendering. Requests and source selection now follow the active target. | `GL` |
| G3D-032 | OpenGL | Bug | Asynchronous PBO readback could map before the GPU finished or overwrite an in-flight request. Per-context fences, zero-time polling, timeout abandonment, and exact cleanup now serialize publication. | `GL` |
| G3D-033 | OpenGL | Bug | PBO allocation/read/map/unmap failures could leave prior probe results visible. Every failure now invalidates result state and destroys the pending sync object. | `GL` |
| G3D-034 | OpenGL | Correctness | Depth-disabled draws wrote motion vectors that temporal passes treated as authoritative. Such draws now use color-only output. | `GL` |
| G3D-035 | OpenGL | Correctness | Material UV selectors, sampler enums, clear colors, and shadow bias accepted invalid numeric values. Upload paths now apply shared enum/finite clamps. | `GL` |
| G3D-036 | OpenGL | Bug | Texture, cubemap, and morph cache growth could overflow capacity arithmetic or fail to make progress. Growth is now checked against `INT_MAX`, `SIZE_MAX`, and required capacity. | `GL` |
| G3D-037 | OpenGL | Performance | Three cache-prune paths allocated, sorted, and freed age arrays each frame. The context now owns one geometrically grown reusable scratch array. | `GL` |
| G3D-038 | OpenGL | Bug | A budget pause and a terminal texture upload failure shared one state, causing endless retries or permanent fallback after a pause. Uploads now have explicit pending/complete/failed states and memoized failed generations. | `GL` |
| G3D-039 | OpenGL | Bug | A failed shadow geometry/upload draw could still mark the slot complete. The pass records failure and only publishes a valid native depth target. | `GL` |
| G3D-040 | OpenGL | Correctness | Raw normalization, unbounded coat/sheen roughness, and duplicate unbounded height-fog exponent paths could produce NaN/overflow in GLSL. All affected shader paths now use bounded finite math. | `GL` |
| G3D-041 | OpenGL | Bug | IBL overlays modified GPU mip levels while later CPU decode could still fail, and ignored exact face sizes/byte counts. The complete chain is measured and decoded before any upload. | `GL`, `U` |
| G3D-042 | OpenGL | Correctness | IBL shading was enabled from runtime metadata before the matching native overlay became resident. It now requires the exact cubemap generation and applied IBL identity. | `GL` |
| G3D-043 | OpenGL | Resource | Normal context destruction omitted the unlit program, BRDF LUT, depth-probe PBO, and fence. Shared teardown now releases every fixed GL object on all exit paths. | `GL` |
| G3D-044 | OpenGL | Bug | RTT and CPU-present paths trusted target extents, color format, and signed stride multiplication. They now validate formats and checked byte layouts before binding/copying. | `GL` |
| G3D-045 | Metal | Correctness | Bone palettes were copied without per-matrix validation and correct MSL column-major conversion. Palette packing now sanitizes and transposes each matrix. | `MTL` |
| G3D-046 | Metal | Correctness | Current/previous instance, camera, object, inverse, and shadow matrices could carry invalid values into transient buffers. All are staged through bounded finite helpers. | `MTL` |
| G3D-047 | Metal | Correctness | Tangents used a normal-space transform and depth-disabled draws published motion. Tangents now use the model transform and motion attachments are disabled for non-authoritative draws. | `MTL` |
| G3D-048 | Metal | Bug | Nested/stale shadow passes, invalid geometry, and upload failures could publish a slot; bias was also unbounded. Explicit pass recovery/failure state and bounded bias now protect publication. | `MTL` |
| G3D-049 | Metal | Bug | All depth probes shared one mutable buffer across in-flight command buffers. Probe storage now lives in the protected transient ring and completion captures the exact buffer. | `MTL` |
| G3D-050 | Metal | Bug | Probe completion published after failed command buffers and could sample the wrong depth texture during RTT. Only `Completed` buffers publish, and source selection follows the active target. | `MTL` |
| G3D-051 | Metal | Bug | RTT creation reused the HDR color descriptor for the BGRA8 motion attachment and replaced textures piecemeal. Color/motion/depth allocation is now format-correct and transactional. | `MTL` |
| G3D-052 | Metal | Bug | Row and cubemap uploads trusted region bounds and signed pitches. Regions, unsigned byte pitches, and exact mip extents are now checked before `replaceRegion`. | `MTL` |
| G3D-053 | Metal | Bug | Readback accepted narrowing/stride overflow, left uncovered HDR pixels stale, and accepted a non-HDR texture for an HDR mirror. It now validates/zeroes the complete destination and requires the expected format. | `MTL` |
| G3D-054 | Metal | Correctness | HDR RTT pipeline selection followed requested state instead of the actual attachment format. It now selects from the bound texture's native pixel format. | `MTL` |
| G3D-055 | Metal | Performance | Terminal texture/cubemap upload failures were retried every frame, and failed sentinels were pruned immediately. Failed generations are memoized and age like other cache entries. | `MTL` |
| G3D-056 | Metal | Bug | Texture/cubemap replacement released the previous resource before the replacement was known good. New resources are staged locally and published only after complete allocation/validation. | `MTL` |
| G3D-057 | Metal | Bug | Native mip metadata and payload spans were not proven exact before upload. Metal now uses the shared compressed-chain validator. | `MTL`, `U` |
| G3D-058 | Metal | Bug | IBL upload accepted partial mip regions, changed GPU state before full decode, and ignored whole-upload budget admission. It now validates, decodes, budgets, and publishes the complete overlay transactionally. | `MTL`, `U` |
| G3D-059 | Metal | Correctness | IBL shading could start before the matching native overlay was applied. The material path now resolves the texture first and requires exact generation/identity residency. | `MTL` |
| G3D-060 | Metal | Use-after-free | The render-target cache retained a borrowed C shell after GC finalization. An internal release hook now removes and drains the ARC cache entry before the shell is freed. | `MTL`, `PROD` |
| G3D-061 | Metal | Correctness | Cache eviction/context teardown cleared `color_dirty`, making stale CPU pixels appear current. Dirty targets are now synchronized first or remain explicitly dirty on failure. | `MTL` |
| G3D-062 | Metal | Bug | Main target recreation replaced attachments one-by-one, leaving mixed dimensions after OOM. Required resources now stage locally and publish as one transaction. | `MTL` |
| G3D-063 | Metal | Bug | Readback treated only explicit `Error` as failure, allowing other non-completed states. Both pending-render and copy command buffers now require exact `Completed` status. | `MTL` |
| G3D-064 | D3D11 | Correctness | View-projection/history matrices accepted finite overflow-prone components and inverse failure left stale state. The D3D11 shared matrix policy now bounds and resets deterministically. | `D3` |
| G3D-065 | D3D11 | Correctness | Draw, light, ambient, shadow-span, and instanced snapshots lacked uniform sanitization at all entry points. They now use the same shared guards as the other backends. | `D3` |
| G3D-066 | D3D11 | Bug | Post-FX traversal trusted chain structure and effect payloads. Chain validation and per-pass sanitized snapshots now precede constant-buffer upload. | `D3` |
| G3D-067 | D3D11 | Bug | Failed shadow draws could still publish their slot. Explicit `shadow_pass_failed` state now gates completion. | `D3` |
| G3D-068 | D3D11 | Bug | HDR readback could narrow `width * 4` into an overflowing signed float stride. Width and mapped row pitch are validated before conversion. | `D3` |
| G3D-069 | D3D11 | Correctness | Coat/sheen roughness and height-fog optical depth were insufficiently bounded in HLSL. Shader inputs and exponent paths now stay in finite representable ranges. | `D3` |
| G3D-070 | Software | Correctness | NaN/Infinity escaped clamp and power helpers into color/depth math. Numeric primitives now return deterministic bounded values. | `PROD`, `U` |
| G3D-071 | Software | Correctness | Light attenuation used overflow-prone float intermediates and invalid decay data. It now uses sanitized inputs and bounded double intermediates. | `PROD` |
| G3D-072 | Software | Bug | One global shadow bias and unchecked shadow spans selected the wrong slot/range. Bias is per slot and resolver inputs are validated against completed maps. | `PROD` |
| G3D-073 | Software | Bug | The renderer could apply vertex lighting and per-pixel lighting to the same fragment. Lighting-mode detection now selects exactly one path. | `PROD` |
| G3D-074 | Software | Correctness | Tangents and several fragment attributes used affine or wrong-space interpolation. Tangents use the model transform and all fragment/shadow-alpha attributes use finite perspective-correct weights. | `PROD` |
| G3D-075 | Software | Bug | Texture wrap/index conversion, sRGB conversion, and UV normalization could overflow or consume non-finite values. Sampling now uses checked `int64_t` indices and finite fallbacks. | `PROD` |
| G3D-076 | Software | Bug | Framebuffer dimensions, strides, bounding-box casts, and wireframe deltas could overflow signed arithmetic. Surface validation and widened/clamped math now precede rasterization. | `PROD` |
| G3D-077 | Software | Bug | Partial worker submission silently skipped tiles. Unsubmitted work now runs synchronously before the barrier completes. | `PROD` |
| G3D-078 | Software | Bug | Tile counts/capacities and grid allocation products could overflow or narrow through signed integers. Counts now use checked `size_t` arithmetic and guarded `calloc`. | `PROD` |
| G3D-079 | Software | Bug | Render-target binding freed the previous target buffers before all replacements existed. Allocation is now staged and published transactionally. | `PROD` |
| G3D-080 | Software | Correctness | Texture views, camera/fog/IBL/shadow inputs, depth-probe endpoints, and instance-matrix spans were trusted at direct backend hooks. Each path now validates before indexing or math. | `PROD`, `U` |
| G3D-081 | Software | Performance | Debug counters ran and could overflow even when diagnostics were disabled. They now use `size_t`, correct formatting, and execute only when debug output is requested. | `PROD` |
| G3D-082 | FBX importer | Crash | Procedural geometry dereferenced `geometry` and `geometry_type` before checking either pointer. Argument validation now precedes type/child access. | `FBX` |
| G3D-083 | FBX importer | Crash | `fbx_find_child` unconditionally dereferenced a null parent. The shared query helper now safely rejects null parent/name inputs. | `FBX` |
| G3D-084 | Metal | Bug | Changing the window post-FX route while an RTT remained bound skipped window-target reconstruction, so the next window frame selected missing attachments. Route changes now rebuild the transactional window resource set regardless of the current RTT binding. | `MTL` |
| G3D-085 | Demo integration | Correctness/Performance | Ridgebound's secondary directional fill and moon inherited shadow casting, wasting shadow slots, while its low-energy night SH coefficients used a daylight-sized IBL multiplier and made deep night non-navigable. The non-key directionals now opt out of shadows, night IBL uses a documented phase-weighted scale, and the smoke probe reports and gates lit-scene coverage. | `RIDGE` |
| G3D-086 | Metal | Bug | MSL aligned a trailing `int3` in each light record to 16 bytes, producing a 176-byte shader array stride for the C uploader's 160-byte elements. Multi-light scenes consequently read shifted or out-of-bounds colors and generated non-finite lighting. The shader tail now uses three scalar integers, while C static assertions pin the sensitive offsets and complete element size. | `MTL`, `ASH` |
| G3D-087 | GPU shaders | Correctness | A non-finite material result could escape into an HDR attachment and poison every downstream full-screen pass. OpenGL and Metal now replace invalid fragment outputs with bounded finite HDR colors and valid alpha, matching the existing D3D11 containment policy. | `GL`, `MTL`, `ASH` |
| G3D-088 | OpenGL/Metal | Correctness | Bloom mip downsampling and additive upsampling allowed one invalid HDR texel to contaminate most of the frame. Both backends now sanitize and half-float-bound bloom values throughout the mip chain. | `GL`, `MTL`, `ASH` |
| G3D-089 | D3D11 | Resource | Readback staging resize released the cached texture before its replacement existed. The new staging texture is now created and validated before publication. | `D3` |
| G3D-090 | D3D11 | Resource | Presented-backbuffer snapshot resize had the same release-before-create window. Allocation failure now leaves the prior snapshot resource and dimensions intact. | `D3` |
| G3D-091 | D3D11 | Bug | Scene color, motion, and depth targets were published piecemeal, so a late view failure discarded the last complete scene set. All nine COM resources now stage and commit together. | `D3` |
| G3D-092 | D3D11 | Resource | Overlay resize destroyed the live texture/RTV/SRV before allocating its replacement. A complete local set now precedes unbind and publication. | `D3` |
| G3D-093 | D3D11 | Resource | The primary post-FX target released its prior complete set before replacement creation. It now follows the backend's stage-then-publish rule. | `D3` |
| G3D-094 | D3D11 | Resource | The secondary post-FX scratch target independently had the same failure mode. Its texture/RTV/SRV now stage before the previous set is retired. | `D3` |
| G3D-095 | D3D11 | Bug | Bloom resize exposed a partial mip chain and then destroyed both generations after a late allocation failure. Every mip resource and extent now stages in local arrays before one commit. | `D3` |
| G3D-096 | D3D11 | Bug | TAA history resize could lose the usable pair when allocation of the second history target failed. Both complete targets now stage before replacement. | `D3` |
| G3D-097 | D3D11 | Resource | SSR resize released the prior output target before allocation. Texture, RTV, and SRV now publish only as a complete replacement. | `D3` |
| G3D-098 | D3D11 | Resource | A changed RGBA texture generation evicted its known-good cache entry before replacement allocation. Texture/SRV allocation failure now preserves the resident generation. | `D3` |
| G3D-099 | D3D11 | Resource | Compressed native textures also evicted their previous generation before allocation. Both replacement COM resources must now exist before the cache entry changes. | `D3` |
| G3D-100 | D3D11 | Resource | Cubemap replacement had the same early-eviction window. Cube texture/SRV allocation is now staged before releasing the old entry. | `D3` |
| G3D-101 | D3D11 shaders | Bug | FXC's DXBC validator rejected the shared shadow/light pixel shader because early-return control flow left a temporary component apparently uninitialized on one path. Both helpers now initialize one result and return it after structured control flow; real D3D11 RTT and viewmodel probes confirm hardware-backend initialization. | `D3` |
| G3D-102 | D3D11 diagnostics | Diagnostics | Shader compilation diagnostics were truncated to the same short warning budget on failure, hiding the validator error that caused software fallback. Failed initialization now retains a bounded 7,936-byte diagnostic while successful warning output keeps the prior 768-byte cap. | `D3` |
| G3D-103 | Shared | Correctness | Camera snapshot sanitization accepted negative shadow slope bias even though the public setter and raster backends define a nonnegative `[0,16]` contract. Snapshots now clamp to that contract and use the runtime default of `1` for non-finite input. | `U` |
| G3D-104 | Shared | Correctness | Rectangle-light basis vectors were normalized independently, so parallel or merely nonorthogonal inputs produced a degenerate light plane. The secondary basis now uses checked Gram-Schmidt projection with a deterministic least-aligned-axis fallback. | `U` |
| G3D-105 | Software | Bug | A depth probe queued while an RTT was bound sampled the window z-buffer and dimensions. Software probes now select the active RTT depth storage and extent, matching the GPU backends' active-target behavior. | `PROD` |
| G3D-106 | Software | Performance | Every software depth-buffer dimension change reallocated exactly `width * height` floats, including alternating sizes that already fit prior storage. The z-buffer now retains a checked geometrically grown capacity. | `PROD` |
| G3D-107 | Software | Performance | Increasing vertex counts reallocated the transformed-vertex scratch array to every exact count. Scratch now grows by a checked 1.5x policy and reuses the excess capacity. | `PROD` |
| G3D-108 | Software | Performance | Render-scale color storage used exact-fit reallocations for each small resolution increase. It now shares the checked geometric-capacity policy. | `PROD` |
| G3D-109 | Software | Performance | The opaque-depth snapshot used by soft particles also grew one exact frame size at a time. Its retained capacity now grows geometrically. | `PROD` |
| G3D-110 | Software | Correctness | The software begin-frame path allowed a slope bias up to `1000`, diverging from the public and GPU-backend limit of `16`. It now applies the shared `[0,16]` bound. | `PROD`, `U` |
| G3D-111 | OpenGL | Correctness | Helper passes captured one generic framebuffer binding and restored it to both read and draw targets, collapsing intentionally split bindings. They now preserve `GL_READ_FRAMEBUFFER_BINDING` and `GL_DRAW_FRAMEBUFFER_BINDING` independently. | `GL` |
| G3D-112 | OpenGL | State | Target allocation changed the active texture unit without restoring it. The helper state snapshot now includes `GL_ACTIVE_TEXTURE`. | `GL` |
| G3D-113 | OpenGL | State | Target allocation overwrote the active unit's 2D texture binding. The binding is now captured and restored, with a deleted live name redirected to its committed replacement. | `GL` |
| G3D-114 | OpenGL | State | RTT allocation left its depth renderbuffer bound. `GL_RENDERBUFFER_BINDING` now participates in transactional state restoration. | `GL` |
| G3D-115 | OpenGL | State | Full-screen helper cleanup could return with texture unit 1–3 active, while ordinary material code assumes unit 0 at entry. Main draw-state restoration now explicitly activates unit 0. | `GL` |
| G3D-116 | OpenGL | Portability | Scene, post-effect, shadow, and RTT allocation paths trusted positive dimensions without checking the live `GL_MAX_TEXTURE_SIZE`. All target constructors now reject device-oversized extents before issuing GL calls. | `GL` |
| G3D-117 | OpenGL | Portability | The LDR scene attachment used unsized `GL_RGBA`, leaving storage selection to the driver. It now uses the deterministic `GL_RGBA8` format. | `GL` |
| G3D-118 | OpenGL | Resource | Scene resize destroyed the complete color/motion/depth set before any replacement attachment existed. The complete set now stages in detached names and publishes in one commit. | `GL` |
| G3D-119 | OpenGL | Resource | A failed RGBA16F scene attempt destroyed the prior live scene before the LDR retry, and a short-circuited completeness check could leave the HDR error queued to poison that retry. HDR and fallback candidates now leave the prior complete scene authoritative until one candidate validates, consuming each attempt's error state independently. | `GL` |
| G3D-120 | OpenGL | Bug | The in-scene post-FX fallback wrote intermediate color into the motion-vector attachment. Scene targets now include a dedicated third color attachment for fallback ping-pong. | `GL` |
| G3D-121 | OpenGL | Correctness | HDR scene color was quantized through the RGBA8 motion attachment during fallback post-processing. The dedicated fallback attachment always matches the actual scene precision. | `GL` |
| G3D-122 | OpenGL | Bug | Reusing the motion attachment as color destroyed motion vectors needed by later motion-blur entries. Motion data now remains immutable throughout an ordered chain. | `GL` |
| G3D-123 | OpenGL | Correctness | The restricted in-scene fallback treated TAA and SSR entries as ordinary generic passes and reported success without their temporal/reflection work. It now rejects those entries so the caller can take its defined passthrough recovery path. | `GL` |
| G3D-124 | OpenGL | Correctness | The in-scene fallback could mutate scene color through early passes before discovering a deterministic unsupported pass or missing bloom chain. It now preflights the entire chain and bloom allocation before its first scene write. | `GL` |
| G3D-125 | OpenGL | Resource | Post-FX readback resize deleted its usable framebuffer and texture before replacement validation. A generic stage-then-publish color-target builder now preserves the live pair on failure. | `GL` |
| G3D-126 | OpenGL | Resource | The secondary post-FX scratch target had the same destroy-before-create failure window. It now uses the transactional builder. | `GL` |
| G3D-127 | OpenGL | Resource | Logical presentation-target resize discarded the prior complete target before allocation. Candidate framebuffer completeness is now established before publication. | `GL` |
| G3D-128 | OpenGL | Resource | Bloom resize published and destroyed mips incrementally, so a late failure lost the previous chain. All mip textures, framebuffers, and extents now stage and commit as one set. | `GL` |
| G3D-129 | OpenGL | Resource | TAA history resize could delete the old pair before the second replacement history existed. Both histories now validate before either live history changes. | `GL` |
| G3D-130 | OpenGL | Resource | SSR resize deleted its live output target before allocating a replacement. It now follows the shared transactional color-target path. | `GL` |
| G3D-131 | OpenGL | Resource | Shadow-map resize released the completed slot before validating a new depth texture/FBO. The replacement now stages, preserving the prior slot on allocation failure. | `GL` |
| G3D-132 | OpenGL | Resource | RTT format/size changes destroyed the active framebuffer, color texture, and depth renderbuffer before replacement or required CPU synchronization succeeded. All three objects now stage before the old target is synchronized and retired. | `GL` |
| G3D-133 | OpenGL | Correctness | Bloom intermediates were always RGBA16F even after a scene fell back to LDR, needlessly depending on the unsupported format that caused fallback. Bloom now follows the actual scene precision. | `GL` |
| G3D-134 | OpenGL | Correctness | TAA histories were likewise hard-coded to RGBA16F after an LDR scene fallback. Their format now follows the active scene attachment. | `GL` |
| G3D-135 | OpenGL | Correctness | SSR always resolved into RGBA8, clipping valid HDR reflections and downstream color. Its target now matches the actual scene precision. | `GL` |
| G3D-136 | OpenGL | Correctness | Scratch-target precision was recomputed from policy/capabilities rather than the scene format that actually survived allocation. All post-FX intermediates now key their precision discriminator from `scene_hdr_active`. | `GL` |
| G3D-137 | OpenGL | Bug | Scene-target reuse checked only the FBO name and dimensions, so a missing color, motion, fallback, or depth name could be treated as complete. Reuse now requires every owned attachment. | `GL` |
| G3D-138 | OpenGL | Bug | Bloom-target reuse trusted count and dimensions even when a mip texture or FBO name was zero. Every resident mip pair is now checked before reuse. | `GL` |
| G3D-139 | OpenGL | Bug | TAA reuse checked only the first history FBO. It now requires both FBO and texture names before preserving history. | `GL` |
| G3D-140 | OpenGL | Bug | Single-color target reuse could accept an FBO whose texture name had been lost. The shared helper requires both live names and a matching precision/extent. | `GL` |
| G3D-141 | OpenGL | Correctness | Bloom was prefiltered once from original scene color even when an earlier effect had already transformed the ordered-chain source. Each bloom-enabled entry now consumes its current source texture. | `GL` |
| G3D-142 | OpenGL | Correctness | Multiple bloom entries all reused the first entry's threshold. Bloom is now regenerated per entry with that entry's sanitized threshold. | `GL` |
| G3D-143 | OpenGL | Bug | Bloom allocation/encoding failure silently disabled bloom and let the chain report success. The failure now propagates to the chain caller. | `GL` |
| G3D-144 | OpenGL | Bug | A failed TAA resolve silently copied the unprocessed source and advanced through the chain. Missing temporal output now fails the chain. | `GL` |
| G3D-145 | OpenGL | Bug | A failed SSR resolve likewise degraded to a generic copy while reporting success. Missing reflection output now fails the chain. | `GL` |
| G3D-146 | OpenGL | Performance | Every RTT synchronization allocated and freed a full-frame byte or float readback buffer. RTT, screenshot, overlay, and fallback presentation paths now share one context-owned scratch allocation. | `GL` |
| G3D-147 | OpenGL | Performance | The shared readback scratch still reallocated at every small high-water increase. It now grows geometrically with checked overflow fallback. | `GL` |
| G3D-148 | OpenGL | Performance | RTT readback issued redundant hard-coded pack-alignment resets on every success and failure path immediately before restoring the captured value. Cleanup now restores the exact snapshot once. | `GL` |
| G3D-149 | OpenGL | Correctness | HDR readback narrowed `width * 16` into a signed float-row stride without proving the multiplication representable. Width is now bounded before conversion. | `GL` |
| G3D-150 | OpenGL | Correctness | RTT readback did not independently prove the destination RGBA stride could hold a tight row. Checked destination and tight-byte products now reject short layouts. | `GL` |
| G3D-151 | OpenGL | Bug | RTT teardown and switching consulted only the context dirty bit even though the target shell has its own dirty latch. Either authoritative dirty flag now forces synchronization. | `GL` |
| G3D-152 | OpenGL | Bug | Rebinding the already-active RTT cleared both dirty latches, potentially discarding an outstanding lazy readback. Same-target rebinding now leaves content state unchanged. | `GL` |
| G3D-153 | OpenGL | Performance | Switching between RTT shells with identical extent/format destroyed and rebuilt identical GPU storage. After synchronizing the old shell, the backend now reuses the complete objects. | `GL` |
| G3D-154 | OpenGL | Correctness | Requested HDR RTTs silently allocated RGBA8 when float color targets were unavailable, then advertised HDR semantics. Unsupported HDR targets are now rejected. | `GL` |
| G3D-155 | OpenGL | Data loss | A failed old-target synchronization during an RTT switch or unbind could be followed by destructive teardown. The candidate is now discarded and the dirty old target, storage, and synchronization hook remain bound and recoverable. | `GL` |
| G3D-156 | OpenGL | Resource | Mesh-cache refresh deleted the resident VBO/IBO before compact packing, object creation, and both uploads succeeded. Candidate buffers now replace the cache entry only after complete upload. | `GL` |
| G3D-157 | OpenGL | Resource | Morph-cache refresh evicted the old entry before payload-size and texture-buffer-limit validation. Validation now precedes any resident mutation. | `GL` |
| G3D-158 | OpenGL | Resource | Failure to upload optional morph-normal deltas discarded a valid prior morph entry and the new position buffer. Both candidate channels now succeed before publication. | `GL` |
| G3D-159 | OpenGL | Performance | Morph entries could grow without a hard same-frame ceiling and were only reduced by a later age-prune pass. Misses now append only below a 64-entry cap and otherwise replace the LRU slot. | `GL` |
| G3D-160 | OpenGL | Bug | Morph-cache count was incremented before upload, leaving blank counted slots after allocation failure. Count now advances only when a complete candidate is published. | `GL` |
| G3D-161 | D3D11 | State | Binding an invalid-sized render target returned before setting a viewport, leaving the previous target's viewport active. The backend now explicitly binds zero viewports for an invalid extent. | `D3` |
| G3D-162 | D3D11 | Device loss | Generic post-FX `Draw` is a void command, so device removal during the draw was reported as success. The path now queries device health after the draw and fails the pass. | `D3` |
| G3D-163 | D3D11 | Device loss | Bloom downsample draws did not check device status. Each level now aborts to cleanup on device removal. | `D3` |
| G3D-164 | D3D11 | Device loss | Bloom upsample draws had the same unchecked void-command failure. Each level now validates device health. | `D3` |
| G3D-165 | D3D11 | Device loss | TAA advanced history parity/validity even if the resolve draw lost the device. Health is checked first; history advances only after success. | `D3` |
| G3D-166 | D3D11 | Device loss | SSR returned its output SRV without checking whether the resolve draw completed on a live device. It now returns failure on removal. | `D3` |
| G3D-167 | D3D11 | Device loss | Overlay composition reported success after an unchecked draw. The draw now participates in the same status propagation. | `D3` |
| G3D-168 | D3D11 | Device loss | Skybox submission never observed device removal because its draw is void. It now updates backend device-loss diagnostics immediately after submission. | `D3` |
| G3D-169 | D3D11 | Bug | A failed bloom encoder left a null bloom SRV but the chain continued as if bloom were disabled. The chain now fails explicitly. | `D3` |
| G3D-170 | D3D11 | Bug | A failed TAA encoder silently preserved the pre-TAA source and continued. Missing temporal output now propagates. | `D3` |
| G3D-171 | D3D11 | Bug | A failed SSR encoder silently preserved the pre-SSR source and continued. Missing reflection output now propagates. | `D3` |
| G3D-172 | Metal | Portability | General Metal color/depth texture helpers accepted any positive signed extent. They now enforce a documented 16,384-texel portability ceiling before `NSUInteger` conversion and allocation. | `MTL` |
| G3D-173 | Metal | Portability | Native compressed texture upload validated block layout but not the top mip against the texture-dimension ceiling. Oversized native snapshots are now rejected before descriptor creation. | `MTL` |
| G3D-174 | Metal | Portability | Cubemap upload likewise accepted an oversized face. Face extent now passes the same checked 2D limit. | `MTL` |
| G3D-175 | Metal | Correctness | Shadow-atlas dimensions multiplied caller sizes by four and two without validating the expanded allocation. Per-slot and expanded atlas extents are now checked before conversion. | `MTL` |
| G3D-176 | Metal | Bug | Mipmap generation returned void, so command-buffer or blit-encoder allocation failure was unobservable. It now returns success only when no work is needed or a complete blit is committed. | `MTL` |
| G3D-177 | Metal | Bug | RGBA texture generation was published before the mip-generation command was known to exist. Publication now follows successful mip commit. | `MTL` |
| G3D-178 | Metal | Bug | Cubemap generation had the same publish-before-mip ordering. It now publishes only after the mip command commits. | `MTL` |
| G3D-179 | Metal | Resource | A changed Pixels generation mutated the resident cache entry in place; a later row/mip failure discarded the known-good texture. Streaming now occurs in a candidate that retains and restores the complete fallback generation on failure. | `MTL` |
| G3D-180 | Metal | Resource | Native compressed replacement had the same in-place failure window. It now uses the retained fallback candidate protocol. | `MTL` |
| G3D-181 | Metal | Resource | Cubemap replacement could discard the resident cube after a later face or mip failure. Candidate uploads now restore the prior complete entry and memoize the failed generation. | `MTL` |
| G3D-182 | Metal | Performance | The texture cache had only age-based pruning, allowing an unbounded burst of unique textures or failed-generation sentinels in one frame. Every pending, successful, or failed insertion now enforces a 256-entry LRU ceiling while protecting the new key. | `MTL` |
| G3D-183 | Metal | Performance | The cubemap cache had the same unbounded same-frame growth, including terminal failure entries. Every cache publication now enforces a 64-entry ceiling. | `MTL` |
| G3D-184 | Metal | Performance | The morph cache was age-pruned but unbounded within a frame. New-key publication now evicts one LRU entry at a 64-entry ceiling. | `MTL` |
| G3D-185 | Metal | Resource | Morph refresh overwrote the resident entry's position buffer before the complete replacement existed. Position and optional normal buffers now stage in a fresh entry. | `MTL` |
| G3D-186 | Metal | Bug | Cached morph-normal allocation failure could publish position-only deformation for a mesh that requested normal deltas. Both requested channels are now mandatory. | `MTL` |
| G3D-187 | Metal | Bug | The transient, uncached morph path had the same position-only fallback. It now rejects the whole morph binding when a requested normal channel cannot allocate. | `MTL` |
| G3D-188 | Metal | Correctness | Instance scratch capacity narrowed from `NSUInteger` to `int32_t` before growth arithmetic. Oversized retained capacity is now rejected before the cast. | `MTL` |
| G3D-189 | Metal | Bug | Instance scratch published the new capacity even when `NSMutableData` allocation returned nil or short storage. A local replacement is now validated before capacity and pointer publication. | `MTL` |
| G3D-190 | Metal | Resource | A geometry-cache miss evicted the LRU entry before allocating either replacement buffer. Eviction now occurs only after a complete candidate exists. | `MTL` |
| G3D-191 | Metal | Performance | Refreshing an existing geometry key still ran the capacity eviction path and could discard an unrelated mesh. Same-key replacement no longer evicts another entry. | `MTL` |
| G3D-192 | Metal | Correctness | Bloom was always generated from original `offscreenColor`, ignoring effects earlier in the ordered chain. The encoder now takes the current source texture. | `MTL` |
| G3D-193 | Metal | Correctness | Multiple bloom entries reused the first enabled threshold and result. Each entry now encodes its current source with its own sanitized threshold. | `MTL` |
| G3D-194 | Metal | Bug | Failed TAA prerequisites silently passed the unprocessed source through a generic copy. Missing temporal output now fails post-FX encoding. | `MTL` |
| G3D-195 | Metal | Bug | Failed SSR prerequisites had the same silent passthrough. Missing reflection output now fails encoding. | `MTL` |
| G3D-196 | Metal | Correctness | Every post-FX source was labeled HDR even after the chain had entered BGRA8 ping-pong storage, causing later passes to repeat HDR transfer behavior. `sceneIsHdr` now derives from the actual source pixel format. | `MTL` |
| G3D-197 | Metal | Resource | Drawable capture overwrote the retained display texture before replacement allocation succeeded. The candidate now publishes only after allocation. | `MTL` |
| G3D-198 | Metal | Correctness | Drawable capture could enqueue a zero-width or zero-height blit after clamping mismatched extents. Empty copies are now rejected. | `MTL` |
| G3D-199 | Metal | Resource | Shadow-atlas resize assigned a nil allocation directly over the usable atlas. A local replacement now preserves the old texture on failure. | `MTL` |
| G3D-200 | Metal | Resource | Per-slot shadow texture resize had the same direct-overwrite failure. Slot replacements now stage locally. | `MTL` |
| G3D-201 | Metal | Bug | Replacing the shared shadow atlas invalidated its contents but left completion flags set for other atlas-backed light slots. Every atlas slot is now invalidated and the published count recomputed. | `MTL` |
| G3D-202 | Metal | Bug | The atlas “cleared this frame” serial advanced before a render encoder was created. It now advances only after the encoder exists, so a retry still clears uninitialized storage. | `MTL` |
| G3D-203 | Metal | Bug | Shadow begin continued after command-buffer acquisition returned nil and attempted to create an encoder from no buffer. It now aborts immediately. | `MTL` |
| G3D-204 | Metal | Portability | Main window-target reconstruction accepted oversized dimensions before allocating its multi-attachment set. It now enforces the shared Metal extent contract at entry. | `MTL` |
| G3D-205 | Metal | Bug | A tiny scene could produce zero bloom mips yet publish an otherwise “successful” post-FX target set. Bloom-enabled reconstruction now requires at least one mip. | `MTL` |
| G3D-206 | Metal | Bug | Failure to allocate either TAA history texture silently published a target set without temporal histories. Target reconstruction now remains transactional and fails. | `MTL` |
| G3D-207 | Metal | Resource | Context and mutable-cache allocation results were dereferenced without validation. Base context creation now checks the object and every required dictionary before returning. | `MTL` |
| G3D-208 | Metal | Resource | In-flight semaphore creation was unchecked, leaving frame pacing to message a nil dispatch object. Queue/default initialization now fails if the semaphore is unavailable. | `MTL` |
| G3D-209 | Metal | Resource | Native view layer, host layer, and `CAMetalLayer` creation were assumed to succeed. Layer attachment now validates each object and reports failure. | `MTL` |
| G3D-210 | Metal | Resource | Default white texture, sampler, cubemap, and BRDF LUT creation failures were silently published into a context that later required them. Default-resource construction now returns failure unless all exist. | `MTL` |
| G3D-211 | Metal | Resource | Required depth states, shared samplers, and shadow pipeline/sampler/depth state were built by void helpers and never validated. Each helper now returns a required-resource status that gates context creation. | `MTL` |
| G3D-212 | Metal | Resource | A context-initialization failure after layer attachment left an orphan `CAMetalLayer` in the host view. Failure cleanup now detaches and clears the layer before returning. | `MTL` |
| G3D-213 | glTF import | Robustness | Camera-node pointer-array sizing used correct manual division guards that the scoped analyzer could not prove and duplicated established overflow logic. It now uses the shared checked add/multiply helpers and allocates from the verified byte counts. | `GLTF` |
| G3D-214 | OpenGL | Portability | The shared readback allocator exposed byte-typed storage even when an HDR path consumed the naturally aligned allocation as floats, obscuring the allocation's generic effective type. It now returns raw `void *` storage and callers select the destination type. | `GL` |
| G3D-215 | D3D11 | Robustness | Cluster-table selection relied on the validator returning false for null before the conditional expression dereferenced the table. The call site now includes an explicit null guard and records usability once. | `D3` |
| G3D-216 | OpenGL | Resource | A changed cubemap generation reused and overwrote the resident GL texture before every replacement face and mip was complete. Replacement now uploads into a candidate while the previous complete cube remains bindable until publication. | `GL` |
| G3D-217 | OpenGL | Portability | Cubemap allocation trusted only the runtime face validator and could exceed the live context's `GL_MAX_TEXTURE_SIZE`. Upload now rejects faces above the queried implementation limit before creating storage. | `GL` |
| G3D-218 | D3D11 | Resource | A streamed RGBA replacement released the complete SRV before all rows and generated mips succeeded. The cache now owns separate candidate and fallback COM pairs and publishes transactionally. | `D3` |
| G3D-219 | D3D11 | Resource | Native compressed texture replacement had the same destructive continuation-failure window. Its prior mip range remains visible until the candidate completes. | `D3` |
| G3D-220 | D3D11 | Resource | Cubemap replacement likewise discarded the resident cube before all faces and mip generation completed. A failed candidate now leaves the previous complete cube authoritative. | `D3` |
| G3D-221 | D3D11 | Correctness | RGBA replacement allocation/start failure preserved the old COM objects internally but returned no SRV for the current draw, producing avoidable white/empty frames. Resolution now returns the still-complete resident SRV. | `D3` |
| G3D-222 | D3D11 | Correctness | Native texture replacement allocation/start failure also hid the still-resident prior mip range. The resolver now binds it and records the fallback use. | `D3` |
| G3D-223 | D3D11 | Correctness | Cubemap replacement allocation/start failure returned no cube despite retaining the prior one. The resolver now keeps that complete cube visible. | `D3` |
| G3D-224 | D3D11 | Performance | A continuously requested terminal-failure sentinel did not refresh its LRU age, so pruning eventually retried the same doomed upload. Failed 2D/native/cubemap generations now age on use while still exposing any complete fallback. | `D3` |
| G3D-225 | Metal | Performance | Metal's memoized failed texture and cubemap generations had the same stale-LRU behavior and could resume repeated allocation/decode work after pruning. Failure hits now refresh their cache age. | `MTL` |
| G3D-226 | Metal | Correctness | RGBA replacement retained the prior entry after failure but every pending/failure return still produced nil, hiding the known-good texture. A visibility selector now binds only the complete active or fallback texture. | `MTL` |
| G3D-227 | Metal | Correctness | Native compressed replacement retained but hid its prior complete mip range. Pending, immediate-failure, continuation-failure, and memoized-failure paths now return that range. | `MTL` |
| G3D-228 | Metal | Correctness | Cubemap replacement similarly retained a fallback object that no lookup path exposed. The complete previous cube now remains visible without exposing partial candidate faces. | `MTL` |
| G3D-229 | Software | Correctness | An explicit BLEND material switched to the opaque/depth-writing path whenever an individual fragment reached alpha one, making occlusion depend on texel alpha. Opaque-write selection now follows the draw's invariant blend classification. | `PROD` |
| G3D-230 | Software | Bug | An additive fragment at alpha one overwrote color instead of adding and wrote depth, unlike all native backends. Additive draws now always use the additive, non-depth-writing branch. | `PROD` |
| G3D-231 | Software | Correctness | Reduced-resolution rendering multiplied a signed width by four after validating only pixel-count allocation, so an extreme internal extent could trigger signed stride overflow. A checked RGBA stride helper now gates allocation, clear, and target resolution. | `PROD` |
| G3D-232 | Software | Performance | Parallel color rasterization freed its merged projected-triangle array after every draw, forcing allocator traffic and geometric regrowth for stable meshes. The context now retains and reuses that scratch capacity. | `PROD` |
| G3D-233 | Software | Performance | The parallel shadow pass independently repeated the same merged-triangle allocation churn. It now owns a separate reusable context scratch array, released during context teardown. | `PROD` |
| G3D-234 | Metal | Correctness | RGBA replacement-entry allocation failure returned nil even though the prior complete texture remained cached. The resolver now returns that resident texture without mutating cache ownership. | `MTL` |
| G3D-235 | Metal | Correctness | Native compressed replacement-entry allocation failure had the same avoidable loss of the prior mip range for the current draw. It now remains visible. | `MTL` |
| G3D-236 | Metal | Correctness | Cubemap replacement-entry allocation failure likewise hid the resident complete cube. The resolver now binds it while leaving later retry possible. | `MTL` |
| G3D-237 | Metal | Correctness | Post-FX readback assigned but never checked the compositor result, then replaced it with the active texture after commit. An encoder failure could therefore read a stale texture; readback now rejects the failed composite and releases its copied chain. | `MTL` |
| G3D-238 | Software | Correctness | Non-finite depth-probe coordinates consumed a frame-local slot before being rejected, so malformed requests could exhaust valid probe capacity and returned misleading nonnegative handles. Requests now reject non-finite coordinates before incrementing the slot count. | `PROD` |
| G3D-239 | Software | Performance | Parallel color clipping allocated and freed every worker-local triangle list on every draw even though mesh workloads normally retain stable sizes. The context now lends each task a retained scratch allocation and takes it back after every success or failure path. | `PROD` |
| G3D-240 | Software | Performance | Parallel shadow clipping independently repeated the same per-worker allocation churn. Shadow tasks now use a separate retained context-owned scratch set, released at context teardown. | `PROD` |
| G3D-241 | OpenGL | Correctness | Full-screen helper passes disabled scissor testing, but the supposedly restoring framebuffer-state snapshot omitted that enable bit. Nested UI/2D clipping could therefore remain disabled after a 3D helper pass. Capture and restoration now preserve the caller's scissor-test state. | `GL` |
| G3D-242 | Metal | Correctness | The required cluster-table fallback buffer was documented as zero-filled but newly allocated shared storage was published without initialization. Allocation now validates mapped contents, clears the complete table, and only then publishes the dummy. | `MTL` |
| G3D-243 | Metal | Resource | Single and instanced draws continued after the required cluster table and fallback buffer both failed to allocate, leaving fragment buffer slot 3 unbound or stale. Both paths now abort the draw unless a valid buffer can be bound. | `MTL` |
| G3D-244 | Metal | Correctness | The instanced material path omitted the BRDF integration LUT at fragment texture slot 18, unlike the single-draw path. Fresh encoders could sample an unbound resource and later draws could inherit stale state. Instanced draws now bind the required LUT explicitly. | `MTL` |
| G3D-245 | Metal | Resource | Single and instanced draws continued when allocation of the required opaque-depth fallback texture failed, allowing the soft-particle shader to access an unbound or stale slot 16. Both paths now require a valid real or dummy depth texture. | `MTL` |
| G3D-246 | Metal | Resource | The shadow-atlas fallback had the same continue-without-binding failure at fragment texture slot 17. Both draw paths now abort unless the real atlas or a valid dummy texture is bound. | `MTL` |
| G3D-247 | Metal | Bug | The four-entry cluster cache rewrote shared buffer contents on eviction and across frames while earlier encoded or in-flight draws could still reference those bytes. Cache misses now allocate fresh immutable backing storage, retain it through command-buffer completion, and publish only the new handle into the last-four revision lookup. | `MTL` |
| G3D-248 | OpenGL | Build correctness | The frame implementation called the static render-target readback helper before its declaration because that prototype lived in a later implementation fragment. Strict C11 Linux builds rejected the implicit declaration and subsequent static redeclaration. The prototype now lives in the parent translation unit before all fragments, with a source-order contract and a forced Linux warning-as-error syntax check. | `GL` |
| G3D-249 | Draco importer | Portability | The payload plausibility check narrowed `SIZE_MAX` through a `size_t` cast of a 64-bit quotient, defeating the overflow branch on 32-bit targets. The comparison now widens the payload size before arithmetic. | `DRACO` |
| G3D-250 | Draco importer | Crash | Byte and varint readers dereferenced null reader/data pointers, while metadata skipping advanced offsets with wrapping addition. Readers and skips now validate pointers and remaining spans before access. | `DRACO` |
| G3D-251 | Draco importer | Correctness | A ten-byte LEB128 accepted payload bits above bit 63 and silently truncated them during the final shift. The tenth byte now permits only its single representable payload bit. | `DRACO` |
| G3D-252 | Draco importer | Bug | Tagged and raw entropy symbol-table counts narrowed arbitrary 64-bit varints into `uint32_t`. A shared checked 32-bit varint reader now rejects oversized identifiers. | `DRACO` |
| G3D-253 | Draco importer | Bug | Sequential indices and both sequential/edgebreaker attribute unique IDs had the same unchecked varint narrowing. All identifier reads now prove the 32-bit range before publication. | `DRACO` |
| G3D-254 | Draco importer | Correctness | rANS initialization accepted null storage and could overflow when adding its base state. Initialization now validates both and rejects an unrepresentable state. | `DRACO` |
| G3D-255 | Draco importer | Bug | rANS decode trusted lookup symbols, zero probabilities, inconsistent cumulative ranges, and overflowing next states. Every transition is now structurally and arithmetically validated. | `DRACO` |
| G3D-256 | Draco importer | Bug | Binary rANS accepted invalid probabilities or an under-renormalized state, and several callers ignored decode failure. The primitive and every topology/prediction caller now fail closed. | `DRACO` |
| G3D-257 | Draco importer | Hang | Tagged-symbol iteration added the component width to a `uint32_t` index, allowing wrap and an infinite loop near `UINT32_MAX`. It now advances by a bounded remaining group. | `DRACO` |
| G3D-258 | Draco importer | Out-of-bounds | Tagged bit offsets and the final rounded byte count could overflow before buffer comparison. Offset, rounding, and remaining-span checks now precede every access and cursor update. | `DRACO` |
| G3D-259 | Draco importer | Correctness | Compressed sequential connectivity decoded zigzag symbol `1` as `0` instead of `-1`, corrupting valid index deltas. All paths now use one exact zigzag decoder. | `DRACO` |
| G3D-260 | Draco importer | Correctness | Attribute descriptors accepted unknown semantics/data types, zero or excessive components, and non-boolean normalized flags. A common descriptor validator now gates both mesh encodings. | `DRACO` |
| G3D-261 | Draco importer | Buffer overflow | Portable and generic attribute scalar counts multiplied tuple count by components in `uint32_t` before allocation/indexing. Checked tuple-count and byte-product helpers now cover every allocation. | `DRACO` |
| G3D-262 | Draco importer | Correctness | Prediction scheme `NONE` is serialized as byte `0xFE`; a direct signed cast was implementation-defined and other high bytes could become accepted negative schemes. Parsing now recognizes `0xFE` explicitly and rejects the rest. | `DRACO` |
| G3D-263 | Draco importer | Correctness | Normal-octahedron and geometric-normal transforms could be paired with incompatible decoder/component layouts, desynchronizing prediction and output strides. Compatibility is now enforced before decoding. | `DRACO` |
| G3D-264 | Draco importer | Correctness | The wrap transform adjusted an out-of-range correction only once, so large corrections could remain outside the declared interval. It now applies complete modular reduction. | `DRACO` |
| G3D-265 | Draco importer | Undefined behavior | Canonical normal parameter derivation used signed left shifts at hostile quantization widths. A bounded unsigned derivation now rejects degenerate or unrepresentable moduli. | `DRACO` |
| G3D-266 | Draco importer | Undefined behavior | Octahedral-normal prediction could overflow additions, negation, and narrowing for hostile corrections. It now performs checked 64-bit arithmetic followed by canonical modular mapping. | `DRACO` |
| G3D-267 | Draco importer | Out-of-bounds | Difference prediction relied on pointer expressions the analyzer could not prove and did not return failure for invalid scalar offsets. Explicit checked offsets now gate every previous/current tuple. | `DRACO` |
| G3D-268 | Draco importer | Buffer overflow | Edgebreaker allowed face counts whose `face * 3` corner IDs fit unsigned storage but not the decoder's signed corner representation. Faces are now capped at `INT32_MAX / 3`. | `DRACO` |
| G3D-269 | Draco importer | Correctness | Edgebreaker vertex and split-vertex totals could exceed the signed identifiers used throughout traversal. Both source and total vertex counts now require `int32_t` representation. | `DRACO` |
| G3D-270 | Draco importer | Buffer overflow | Corner, vertex, traversal, split, decoder-map, prediction-stack, face-map, and point-remap allocation products were unchecked on narrow `size_t` targets. Every byte product is now validated before allocation. | `DRACO` |
| G3D-271 | Draco importer | Integer overflow | Topology split source deltas added two untrusted 64-bit values before validating the result. The addition is now checked before range comparison. | `DRACO` |
| G3D-272 | Draco importer | Hang | DFS, prediction-degree, multi-parallelogram, and geometric-normal guards computed `corner_cap * N` in 32 bits, allowing the guard limit to wrap. Guard arithmetic now uses 64 bits. | `DRACO` |
| G3D-273 | Draco importer | Undefined behavior | Texture-coordinate prediction used unchecked signed differences, products, squared norms, and projection sums. Checked 64-bit operations now reject overflow at each stage. | `DRACO` |
| G3D-274 | Draco importer | Undefined behavior | Geometric-normal prediction could overflow cross products, accumulation, absolute value of `INT64_MIN`, scaling, and sign flips. Each operation is now checked and large normals are safely rescaled. | `DRACO` |
| G3D-275 | Draco importer | Buffer overflow | Constrained-prediction flag counts narrowed 64-bit values into `uint32_t`/`size_t` after only a logical-count check. Both representation limits are now enforced before allocation and iteration. | `DRACO` |
| G3D-276 | Draco importer | Undefined behavior | Serialized normal transform maxima and quantization widths reached shifts before validation. Transform metadata now derives and validates all canonical normal parameters first. | `DRACO` |
| G3D-277 | Draco importer | Correctness | Generic float attributes admitted NaN and infinity into mesh output. FLOAT32 and FLOAT64 values must now be finite. | `DRACO` |
| G3D-278 | Draco importer | Correctness | Generic FLOAT64 attributes narrowed finite values outside the float range to infinity. The decoder now proves `[-FLT_MAX, FLT_MAX]` before conversion. | `DRACO` |
| G3D-279 | Draco importer | Correctness | Generic BOOL attributes accepted every byte value. Only canonical zero and one encodings are now accepted. | `DRACO` |
| G3D-280 | Draco importer | Correctness | UINT32, INT64, and UINT64 generic attributes silently truncated values into the decoder's `int32_t` output representation. Out-of-range values now reject the stream. | `DRACO` |
| G3D-281 | Draco importer | Undefined behavior | Dequantization shifted before checking width, negated possible `INT_MIN`, and trusted raw quantized ranges and non-finite transform values. Validation now precedes shifts/math and every output must remain finite and float-representable. | `DRACO` |
| G3D-282 | Draco importer | Buffer overflow | Normal conversion trusted its two-component input pointer/count while allocating a three-component output with unchecked products. Both spans and every quantized coordinate are now validated. | `DRACO` |
| G3D-283 | Draco importer | Buffer overflow | Copying decoded integer attributes to original form trusted decoder type, components, pointer, and allocation products. The conversion is now fully validated and byte-counted. | `DRACO` |
| G3D-284 | Draco importer | Correctness | Edgebreaker decoder zero could declare split attribute connectivity even though it owns the unsplit position topology. The invalid layout is now rejected before traversal. | `DRACO` |
| G3D-285 | Draco importer | Buffer overflow | Edgebreaker point remapping allocated output arrays from unchecked point/component products. Remap byte counts now use the shared checked tuple helper. | `DRACO` |
| G3D-286 | Draco importer | Crash | The mesh entry point dereferenced a null destination and an optional unsupported-status pointer, while attribute lookup trusted a corrupt count. Entry arguments are now optional/null-safe as documented and lookup is capped to the fixed descriptor array. | `DRACO` |
| G3D-287 | glTF materials | Crash | Texture population indexed the image table even when a parsed document had no image allocation. Image lookup now requires a non-null table as well as a valid source index. | `GLTF` |
| G3D-288 | glTF meshes | Performance | Two assignments populated locals that were unconditionally overwritten before use, adding noise to an already complex import path and static-analysis output. The dead stores are removed. | `GLTF` |
| G3D-289 | Game3D persistence | Buffer overflow | Persistent-record lookup and upsert trusted negative, over-capacity, or unbacked dense-array state, and doubling could overflow. State and checked capacity now gate every lookup and grow. | `GAME` |
| G3D-290 | Game3D streaming | Buffer overflow | Cell-flag setters/getters trusted inconsistent count/capacity storage and nullable stored strings. The table now grows with checked arithmetic and reads fail closed on invalid backing state. | `GAME` |
| G3D-291 | Game3D streaming | Out-of-bounds | Fixed loaded-event queries, clearing, and teardown trusted a corrupt logical count. Read/release paths now reject or clamp counts outside the compile-time bound. | `GAME` |
| G3D-292 | Game3D persistence | Buffer overflow | The binary persistence writer trusted `size <= capacity`, backing-pointer consistency, and non-null source bytes. It now validates its complete structural state before copying. | `GAME` |
| G3D-293 | Game3D cloth | Buffer overflow | World cloth append trusted dense-array state and overflow-prone doubling. Append now validates storage and grows through the shared checked-capacity helper. | `GAME` |
| G3D-294 | Game3D combat | Buffer overflow | Entity hitbox registration, collection, and teardown trusted corrupted count/capacity state and unchecked growth. The retained hitbox array now validates its full dense-array invariant. | `GAME` |
| G3D-295 | Game3D audio | Crash | Reverb-zone traversal and growth trusted negative, over-capacity, or unbacked array state. All reverb registry paths now validate or defensively bound the retained array. | `GAME` |
| G3D-296 | Game3D timeline | Buffer overflow | Track traversal, finalization, append, and playback trusted dense-array invariants and signed doubling. Checked growth and fail-closed storage validation now cover every playback path. | `GAME` |
| G3D-297 | Game3D third-person | Buffer overflow | Occluder-fade bookkeeping trusted counts/capacities and signed doubling. Every traversal and append now validates the dense array and uses checked capacity growth. | `GAME` |
| G3D-298 | Game3D third-person | Resource correctness | Changing or losing the camera target could leave cloned transparent materials installed indefinitely. Target transitions and missing-target updates now restore all originals. | `GAME` |
| G3D-299 | NavMesh3D | Buffer overflow | Area-name interning trusted table invariants and multiplied signed capacity before checking overflow. Count/capacity/backing state, identifier limits, and allocation bytes now gate growth. | `NAV` |
| G3D-300 | NavMesh3D | Out-of-bounds | Finalization, reverse lookup, and export traversed raw area/link/obstacle counts. Each now uses a capacity-clamped count so corrupt private state cannot escape its allocation. | `NAV` |
| G3D-301 | NavMesh3D | Out-of-bounds | Scene baking walked raw child counts while filling its traversal stack. It now uses the Scene3D defensive child-count helper before reading child slots. | `NAV` |
| G3D-302 | Skeleton3D | Buffer overflow | Alias setters/getters/lookup trusted count/capacity state and overflow-prone signed doubling. Traversal is now capacity-clamped and growth proves both element and byte capacity. | `NAV` |
| G3D-303 | Terrain3D | Buffer overflow | Authored-hole storage trusted corrupt count/capacity state and used unchecked signed doubling. Traversal is capacity-bounded and append proves signed and byte capacity before reallocating. | `TERRAIN` |
| G3D-304 | Game3D model cache | Resource/overflow | Normalized-path output length addition could wrap, and null-path fallback created a temporary reference with ambiguous ownership. Remaining-space comparisons now precede concatenation and fallback ownership is explicit. | `GAME` |
| G3D-305 | Scene3D queries | Buffer overflow | Precise-ray result growth could overflow signed capacity and `size_t` allocation bytes. Both representations are now checked before reallocating the result array. | `GAME` |
| G3D-306 | Draco | Null dereference | The raw GENERIC attribute decoder read `att->data_type` before validating the descriptor pointer. Argument validation now precedes every descriptor access, with a direct null-input regression. | `DRACO` |
| G3D-307 | Clustered lighting | Correctness | The two-pass light binning cache relied on the analyzer proving identical loop bounds before reading stack ranges. Ranges now have a defensive baseline and the helper returns a fully initialized value, preventing indeterminate state if pass control flow diverges. | `U` |
| G3D-308 | Terrain3D | Undefined behavior | Hole-mask rebuild and export subtracted one from private signed dimensions before proving they exceeded one, so a corrupt `INT32_MIN` dimension could underflow. Dimension validation now precedes both subtractions. | `TERRAIN` |
| G3D-309 | Game3D cloth | Out-of-bounds | World teardown still traversed the raw cloth count even though add/remove/tick had been hardened. Finalization now clamps traversal to backed capacity before releasing entries. | `GAME` |
| G3D-310 | Game3D persistence | Out-of-bounds | Save-state serialization traversed raw persistent-record and stream-flag counts after other access paths had been hardened, and could emit counts beyond the loader's format limit. Save now validates backing state, capacity, and format maxima before writing. | `GAME` |
| G3D-311 | Game3D persistence | Buffer overflow | Save-slot path sizing added the data-directory length, separator, slot, suffix, and terminator without checking `size_t` overflow. The complete path length is now proved before allocation and formatting. | `GAME` |
| G3D-312 | Game3D persistence | Buffer overflow | The snapshot reader checked `cursor + count` after unchecked addition and assumed non-null reader/destination storage. It now validates state and compares against the remaining span before pointer arithmetic or copies. | `GAME` |
| G3D-313 | Skeleton3D | Out-of-bounds | Source-side alias reversal during animation retargeting still traversed the raw alias count. Retargeting now uses the same capacity-clamped alias count as setters, lookup, and getters. | `NAV` |
| G3D-314 | NavMesh3D | Out-of-bounds | Off-mesh refresh, lookup, heuristic selection, segment classification, and path-workspace sizing traversed or multiplied the raw authored-link count. Every read path now uses the capacity-clamped count. | `NAV` |
| G3D-315 | NavMesh3D | Buffer overflow | Off-mesh-link and obstacle append paths accepted negative, over-capacity, or unbacked storage state before reallocating/indexing. All mutable dense arrays now reject inconsistent bookkeeping and exhausted signed counts. | `NAV` |
| G3D-316 | NavMesh3D | Out-of-bounds | A* and link reconstruction trusted derived CSR begin/end offsets and packed link indices. One checked range helper now validates the cache against triangle, link, and edge bounds and falls back to the correct full scan when it is unusable. | `NAV` |
| G3D-317 | NavMesh3D | Out-of-bounds | A corrupt triangle neighbor could index the closed set and live triangle array before any upper-bound check. A* now rejects neighbors outside the defensively bounded triangle span. | `NAV` |
| G3D-318 | NavMesh3D | Undefined behavior | The test-only obstacle injection path doubled signed capacity in an initializer before checking the overflow threshold. Growth now validates the old capacity before multiplication, matching the production append path. | `NAV` |
| G3D-319 | Game3D persistence | Correctness | The VW3DSAV1 validator accepted empty keys, noncanonical liveness bytes, and otherwise-valid snapshots with trailing data; its bounded-key helper also dereferenced a null reader. Validation now requires exact consumption, non-empty bounded keys, canonical booleans, and null-safe arguments. | `GAME` |
| G3D-320 | Game3D persistence | State corruption | Loading destroyed the live delta store before all replacement allocations succeeded, and a failed record upsert skipped its encoded pose without advancing the reader. The complete record/flag snapshot is now decoded into owned staging arrays and committed only after every allocation and parse succeeds. | `GAME` |
| G3D-321 | Game3D persistence | Performance | Snapshot loading performed a linear upsert search for every record and flag, producing quadratic work at the documented 100,000-record limit. Direct staging plus one sort makes loading `O(n log n)` and rejects duplicate keys deterministically. | `GAME` |
| G3D-322 | Game3D persistence | Data loss | Oversized cell/flag keys were silently truncated during save, and non-finite world/entity values could produce a file the loader itself rejected. Input contracts now cap keys at 255 bytes, and serialization fails instead of publishing truncated or non-finite snapshots. | `GAME` |
| G3D-323 | Game3D persistence | Portability | Floating-point bit patterns were serialized through a signed 64-bit conversion and signed values were decoded with implementation-defined unsigned-to-signed narrowing. Unsigned bit transport and an explicit, range-safe two's-complement conversion now preserve every format bit on all supported C implementations. | `GAME` |
| G3D-324 | Game3D persistence | Correctness | Runtime keys and save-slot names were measured only through their C-string views, so an embedded NUL could silently alias a different persistent identity or path. Runtime byte lengths and C-string lengths must now agree, and encoded keys reject embedded NUL bytes. | `GAME` |
| G3D-325 | Game3D persistence | Out-of-bounds | A corrupt negative entity count was treated as a full-capacity live span while refreshing persistent entities. Invalid entity bookkeeping now fails closed instead of traversing uninitialized or stale entries. | `GAME` |
| G3D-326 | Game3D audio | Out-of-bounds | Origin rebasing and occlusion updates traversed the raw positional-source count without proving that the backing array and capacity agreed. Both paths now validate the dense source array before indexing it. | `GAME` |
| G3D-327 | Game3D audio | Undefined behavior | Occlusion scheduling accepted nonpositive budgets and performed signed cursor-plus-budget arithmetic before modulo, permitting overflow and negative indices after state corruption. The cursor is normalized first and scheduling arithmetic is now bounded in 64 bits. | `GAME` |
| G3D-328 | Game3D audio | Out-of-bounds | Ambient-bed mutation and ticking trusted a signed count for two fixed-size zone/clip arrays. Negative or oversized state is now rejected on mutation and clamped for read, tick, and finalizer traversal. | `GAME` |
| G3D-329 | Game3D combat | Out-of-bounds | Hitbox-window binding accepted a negative fixed-array count, while liveness checks traversed the raw count. Mutation now requires the complete fixed-array invariant and liveness fails closed for inconsistent state. | `GAME` |
| G3D-330 | Game3D persistence | Out-of-bounds | World persistence teardown released record keys through the raw logical count even when it exceeded allocated storage. Finalization now clamps releases to valid backing capacity. | `GAME` |
| G3D-331 | Game3D streaming | Out-of-bounds | Stream teardown independently released cell/flag strings through an unchecked logical count. It now releases only the initialized portion proved by pointer, count, and capacity. | `GAME` |
| G3D-332 | Game3D streaming | Out-of-bounds | Pushing a loaded-cell event could index the fixed event array with a negative or oversized pre-existing count. Push now repairs invalid bookkeeping before shifting or appending. | `GAME` |
| G3D-333 | Game3D persistence | Buffer overflow | Snapshot writer append formed `size + count` before proving the sum representable. It now checks against `SIZE_MAX` before computing the required byte count. | `GAME` |
| G3D-334 | Game3D persistence | Hang/buffer overflow | Snapshot writer geometric growth could wrap to zero and loop forever or publish an undersized buffer. Growth now detects the doubling ceiling and advances directly to the already-validated required size. | `GAME` |
| G3D-335 | Game3D cloth | Out-of-bounds | Cloth removal and per-frame ticking still traversed raw world cloth counts independently of the append path. Both operations now reject inconsistent array bookkeeping before indexing. | `GAME` |
| G3D-336 | Game3D combat | Out-of-bounds | Hitbox victim-deduplication trusted a signed count for its fixed victim array. Lookup and remember paths now reject counts outside the compile-time bound. | `GAME` |
| G3D-337 | Game3D combat | Buffer overflow | Hit-event publication, query, clear, and teardown trusted the dynamic event array and overflow-prone growth. All hit-event paths now validate backing state and share checked capacity arithmetic. | `GAME` |
| G3D-338 | Game3D combat | Buffer overflow | Damage-event publication, query, clear, and teardown had the same independent unchecked dynamic-array failure mode. The damage buffer now validates state and grows without signed or byte overflow. | `GAME` |
| G3D-339 | Game3D audio | Crash | The audio-immersion tick dereferenced its world argument while locating the listener before checking whether the world was null. World validation now precedes every access. | `GAME` |
| G3D-340 | Game3D timeline | Undefined behavior | Sorting an empty timeline passed a null base pointer to `qsort`, a library call whose pointer contract is not guaranteed merely because the element count is zero. Sorting now runs only for two or more backed tracks. | `GAME` |
| G3D-341 | Game3D third-person | Out-of-bounds | The occluder-fade pass trusted a physics raycast's returned hit count before indexing a fixed stack buffer. Negative counts normalize to zero and excessive counts clamp to the buffer bound. | `GAME` |
| G3D-342 | Game3D third-person | Use-after-free | Fade updates used the cached cloned-material handle without revalidating its runtime class. Invalid or stale material slots are now released and skipped before any material call. | `GAME` |
| G3D-343 | NavMesh3D | Buffer overflow | Scene-bake traversal stack growth validated signed capacity but not `capacity * sizeof(pointer)` on narrow `size_t` targets. The allocation byte product is now proved before reallocation. | `NAV` |
| G3D-344 | Terrain3D | Buffer overflow | Hole-mask rasterization multiplied two signed dimensions and rounded the bit count without proving the complete `size_t` expression. It now validates the product plus rounding term before allocation. | `TERRAIN` |
| G3D-345 | Terrain3D | Undefined behavior | Extreme but finite hole coordinates reached `floor`/`ceil` followed by an out-of-range cast to `int32_t`. A saturating cell-edge helper now handles finite overflow, infinities produced by arithmetic, and NaN before conversion. | `TERRAIN` |
| G3D-346 | Game3D model cache | Buffer overflow | The pointer-dedup set used unchecked signed capacity doubling and trusted corrupted count/capacity/backing state. It now validates state and grows through the shared checked-capacity helper. | `GAME` |
| G3D-347 | Game3D model cache | Buffer overflow | Path normalization's segment-pointer table independently used unchecked signed doubling. Segment growth now proves both logical capacity and allocation bytes before appending. | `GAME` |
| G3D-348 | Scene3D queries | Correctness | Precise-ray traversal stamped equal-distance hits with a signed 32-bit order counter that could wrap and destabilize deterministic sorting. The monotonic order and stored tie-break field are now 64-bit. | `GAME` |
| G3D-349 | Game3D persistence | Performance | Transactional load zeroed staging counts after the corresponding owned pointers had already been transferred and nulled, leaving stores that could not influence either success or failure cleanup. The dead stores are removed. | `GAME` |
| G3D-350 | Game3D persistence | Correctness | Save serialization enforced per-table record limits but not the format's 64 MiB byte ceiling, so maximum-length keys could produce a successfully published snapshot that LoadState always rejected. Writer append/growth, validation, and loading now share one hard byte limit and save fails before file publication. | `GAME` |

## Compatibility and maintenance rules

- The runtime registry, scripting names, IL, and public runtime C ABI are unchanged.
- Backend snapshot structs remain internal. New sanitizers copy into backend-owned stack or
  context storage; they do not extend caller lifetimes.
- A RenderTarget3D native cache that stores a borrowed shell must install the internal
  `release_backend` hook and clear it before destroying its owning backend context. See
  [ADR 0173](../adr/0173-graphics3d-transactional-hardening-and-retained-work.md).
- Upload work is either resumable (`pending`) or terminal (`failed`); backends must not infer one
  from a zero generation or missing native handle.
- Resource replacement is stage-then-publish. On validation/allocation failure, the previous
  complete resource remains authoritative.
- Metal buffers referenced by encoded draws are immutable until command-buffer completion. Cache
  eviction may replace a lookup handle only after the old backing storage is retained by the
  command buffer's `frameBuffers` lifetime set.

## Validation record

Revalidated on 2026-07-29 for G3D-249 through G3D-350:

- The canonical macOS arm64 build completed in incremental, build-only mode with
  `ZANNA_SKIP_CLEAN=1` and `ZANNA_SKIP_TESTS=1`. Lint, audit, smoke, and install stages inside the
  build script were intentionally skipped; the platform-policy lint was run separately.
- The focused runtime slice passed 14/14 tests: Canvas3D and Scene3D, cloth, four Game3D suites,
  terrain, glTF and both Draco suites, Skeleton3D, NavMesh3D, and the Graphics3D robustness suite.
  The new white-box Draco test passed all 38 checks both normally and under combined AddressSanitizer
  and UndefinedBehaviorSanitizer instrumentation.
- All 13 affected Graphics3D translation units passed the project's warning-as-error build and
  Clang static analysis without findings.
- Cppcheck completed all 106 Graphics3D C translation units with warning, performance,
  portability, and inconclusive diagnostics enabled and repository inline suppressions honored;
  it reported no findings. Clang-format, platform-policy lint, audit-ledger continuity, and
  whitespace/error checks also passed.
- The full CTest suite was intentionally not run because other work was active in the shared
  worktree, as requested.

Revalidated on 2026-07-29 for G3D-238 through G3D-248:

- The canonical macOS arm64 build completed in incremental, build-only mode with
  `ZANNA_SKIP_CLEAN=1` and `ZANNA_SKIP_TESTS=1`. Lint, audit, smoke, and install stages inside the
  build script were intentionally skipped; the platform-policy lint was run separately.
- The focused runtime/backend slice passed 8/8 tests:
  `test_rt_canvas3d_production`, `test_rt_gltf`, `test_rt_canvas3d_gpu_paths`,
  `test_vgfx3d_backend_utils`, all three GPU backend source-contract suites, and
  `test_rt_graphics3d_robustness`.
- The complete Metal Objective-C translation unit passed the project's warning-as-error compile
  and Clang static analysis. The forced Linux/Wayland OpenGL C path passed warning-as-error syntax
  compilation and Clang static analysis. The actual D3D11 translation unit passed an x86_64
  MinGW-w64 warning-as-error syntax compile with only MSVC link pragmas and the pre-existing
  embedded-HLSL line-length diagnostic excluded.
- Cppcheck completed all 106 Graphics3D C translation units with warning, performance,
  portability, and inconclusive diagnostics enabled and repository inline suppressions honored;
  it reported no findings. The final software and OpenGL translation units were separately
  rechecked after their last scoped edits. Changed-line clang-format, platform-policy lint,
  audit-ledger continuity, and whitespace/error checks also passed.
- The full CTest suite was intentionally not run because other work was active in the shared
  worktree, as requested.

Revalidated on 2026-07-29 for G3D-103 through G3D-237:

- The canonical macOS arm64 build completed in incremental, build-only mode with
  `ZANNA_SKIP_CLEAN=1` and `ZANNA_SKIP_TESTS=1`. Lint, audit, smoke, and install stages inside the
  build script were intentionally skipped and validated separately where applicable.
- The focused runtime/backend slice passed 8/8 tests:
  `test_rt_canvas3d_production`, `test_rt_gltf`, `test_rt_canvas3d_gpu_paths`,
  `test_vgfx3d_backend_utils`, all three GPU backend source-contract suites, and
  `test_rt_graphics3d_robustness`.
- Targeted native Metal validation passed 3/3: the Ashfall authored visual gate, Ridgebound release
  scene smoke test, and Canvas3D render-scale probe.
- The platform-policy lint passed. Cppcheck reported no warning, performance, portability, or
  inconclusive findings across the changed backend translation units; separately configured
  OpenGL and D3D11 translation-unit checks were also clean.
- The complete Metal Objective-C translation unit and changed C/C++ units passed the project's
  warning-as-error syntax checks; the final Metal translation unit also passed Clang static
  analysis with no findings. The actual D3D11 translation unit passed an x86_64 MinGW-w64
  warning-as-error syntax compile (with only the pre-existing embedded-HLSL line-length diagnostic
  excluded).
- The full CTest suite was intentionally not run because other work was active in the shared
  worktree, as requested.

Revalidated on 2026-07-21:

- The canonical macOS arm64 build passed its 1,924/1,924-test default suite, runtime-surface audit,
  platform-policy lint, and host-capability smoke stages. The unavailable-audio negative test was
  the sole expected skip; the final system install was not rerun because sudo credentials were not
  available to the non-interactive validation session.
- The unfiltered `graphics3d` label passed 147/147 tests, including the Metal Ashfall visual gate
  and Metal/software Ridgebound release-scene probes; the focused backend/helper slice passed 6/6
  tests.
- Ashfall's fixed Metal frame measured 165.603 mean luminance, 97.576% lit-scene coverage, and a
  0–244 sampled range at 640x360. The same portable gate passed the software backend at 226.106
  mean luminance and 100% lit-scene coverage.
- Full ASan and UBSan suites passed. The optional TSan lane was not enabled by the sanitizer
  wrapper.
- Cppcheck completed all 105 Graphics3D translation units with warning, performance, and
  portability diagnostics enabled and no findings.
- The actual D3D11 translation unit passed an x86_64 MinGW-w64 `-Werror` syntax compile. A
  subsequent native Windows x64/MSVC run passed all 1,839 registered tests; the RTT-readback and
  viewmodel-sprite probes both initialized and exercised the D3D11 backend instead of falling back
  to software. Linux OpenGL behavior additionally relies on its source-contract/helper suites.
- Ridgebound's structured project check and all four topology, traversal, lifecycle, and
  visual/performance probes passed against the isolated build.
