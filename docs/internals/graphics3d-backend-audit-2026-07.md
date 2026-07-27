---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Graphics3D Backend Correctness Audit (July 2026)

This audit covers the C/Objective-C renderer boundary under
`src/runtime/graphics/3d/backend/`, with focused review of the OpenGL 3.3,
Direct3D 11, Metal, and software implementations. It supplements the broader
[Graphics3D runtime hardening program](graphics3d-runtime-hardening-2026-07.md).

The review combined line-by-line ownership and arithmetic analysis, backend
parity comparisons, shader review, whole-tree cppcheck analysis, targeted unit
tests, source-contract tests for platform-excluded translation units, and
headless production renders. The 102 findings below are fixed; none changes a
registered scripting API.

## Regression suites

The evidence column uses these abbreviations:

| Tag | Coverage |
|-----|----------|
| `U` | `test_vgfx3d_backend_utils` shared validation, arithmetic, conversion, and layout cases |
| `GL` | `test_vgfx3d_backend_opengl_shared`, including concatenated OpenGL source contracts |
| `D3` | `test_vgfx3d_backend_d3d11_shared`, including concatenated D3D11 source contracts |
| `MTL` | `test_vgfx3d_backend_metal_shared`, including concatenated Metal source contracts |
| `PROD` | `test_rt_canvas3d_production` deterministic software rendering and lifetime checks |
| `FBX` | `g3d_test_fbx_ascii` plus whole-Graphics3D cppcheck analysis |
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

## Validation record

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
