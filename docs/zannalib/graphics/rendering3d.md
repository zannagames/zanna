---
status: active
audience: public
last-verified: 2026-09-01
---

# 3D Rendering, Animation, and Environment
> Camera, lighting, animation, audio, particles, post-processing, terrain, navigation, and environment helpers for the Zanna.Graphics3D namespace.

**Part of [Zanna Runtime Library](../README.md) › [Graphics](README.md)**

---

This page documents the `Zanna.Graphics3D` runtime surface for classes not covered by [3D Physics](physics3d.md). For mesh loading, material authoring, scene graphs, and the full 3D asset pipeline, see the [Graphics 3D Guide](../../graphics3d-guide.md).

For the higher-level code-first game workflow, see [Game3D](game3d.md).
`Zanna.Game3D` is implemented in the same C runtime layer and wraps the lower
level rendering, physics, input, audio, and final-frame contracts documented
here.

## Asset Load Results and Diagnostics

Script-facing content loaders do not trap for bad or missing content. For new
code, prefer the Result-returning SceneAsset APIs:
`SceneAsset.LoadResult`, `LoadAssetResult`, `LoadAnimationResult`,
`LoadAnimationAssetResult`, `LoadNodeAnimationResult`, and
`LoadNodeAnimationAssetResult`. Missing files, unreadable files, wrong magic
bytes, truncated payloads, corrupt structure, unsupported formats, invalid clip
indexes, and excessive content sizes return `Err(message)`. Successful loads
return `Ok(value)`. Traps remain reserved for programmer errors such as `null` or
invalid argument handles.

Null-returning loaders (`FBX.Load`, `GLTF.Load`, `Mesh3D.FromObj`, and the
option-taking `SceneAsset.LoadWithOptions`/`LoadWithOptionsEx`) record their
diagnostics on `Zanna.Graphics3D.AssetDiagnostics3D`. Successful partial
degradation, such as an OBJ material whose albedo texture is missing, returns
the loaded asset and records warnings.

Warnings accumulate per outer load and are capped. The first 16 warnings are
initially visible. If more arrive, the first 15 remain stable and slot 15
becomes an `N more suppressed` summary; the suppressed count includes the
displaced sixteenth warning. Use
`AssetDiagnostics3D.LoadWarningCount`, `AssetDiagnostics3D.GetLoadWarning(index)`, or
`AssetDiagnostics3D.GetLoadWarnings()` to inspect partial degradation.

`AssetDiagnostics3D.GetImportReport()` returns the same degradation as one JSON
document: structured counters plus the warning strings, e.g.
`{"skippedPrimitives":1,"truncatedInfluenceVertices":0,"outOfRangeJointVertices":0,`
`"ignoredExtensions":0,"bakedCubicSplineChannels":0,"suppressedWarnings":0,"warnings":[...]}`.
The report is always emitted as one complete strict-JSON document, even when
every warning byte needs escaping. Valid UTF-8 is preserved; malformed warning
bytes are represented with `\u00xx` escapes. Counters cover primitives skipped
for unsupported modes, vertices whose bone influences were truncated or
referenced out-of-range joints, optional extensions the loader ignored, and
skeletal CUBICSPLINE channels baked to sampled keys.

| Loader | Content failure behavior | Partial degradation |
|--------|--------------------------|---------------------|
| `SceneAsset.LoadResult(path)` / `LoadAssetResult(path)` | Returns `Err(message)` for missing, unreadable, unsupported, malformed, truncated, or oversized `.vscn`, `.fbx`, `.gltf`, `.glb`, `.obj`, and `.stl` content | Preserves lower-level warnings from material texture and dependency loads |
| `SceneAsset.LoadAnimationResult(path, index)` / `LoadNodeAnimationResult(path, index)` and asset variants | Returns `Err(message)` for failed asset loads or absent/out-of-range animation clips | Preserves lower-level warnings from dependency loads |
| `SceneAsset.LoadWithOptions(path, forceTangents)` / `LoadResultWithOptions` | `LoadWithOptions` returns `null` and sets warnings for routine content failures; `LoadResultWithOptions` returns `Err(message)` | `forceTangents = true` generates tangents for every UV0-mapped glTF primitive even when its material has no normal map at load time — for materials that gain normal maps after import (FBX ignores the option) |
| `SceneAsset.LoadWithOptionsEx(path, options)` | Returns `null` for routine content failures | `options` is a comma-separated flag string; unknown flags are ignored. `forceTangents` — as above. `eightInfluences` — keep up to 8 bone influences per vertex (the strongest 4 in the vertex record, influences 5-8 on a per-mesh side stream applied by CPU skinning; such meshes bypass the GPU skinning fast path). `compressAnimations` — tolerance-based keyframe reduction on imported clips (keys reconstructible by lerp/slerp are dropped; cubic keys and endpoints always survive; dropped counts appear in `AssetDiagnostics3D.GetImportReport()` as `compressedAnimationKeysDropped`). `compactStreams` — every imported mesh opts into the compact 48-byte GPU static-cache vertex encoding (see `Mesh3D.CompactStreams`) |
| `FBX.Load(path)` | Returns `null` for missing, unreadable, wrong-magic, truncated, malformed, unsupported, or oversized FBX content | Missing textures and malformed optional morph normal/tangent streams are omitted with warnings; bounded IK may retain its closest finite pose with a warning |
| `GLTF.Load(path)` / `GLTF.LoadAsset(path)` | Returns `null` for missing roots, unreadable roots, wrong JSON/GLB magic, malformed JSON, corrupt buffers/accessors, missing required buffers, unsupported dependencies, or oversized content | Missing or unreadable material images leave that texture slot empty and add warnings |
| `Mesh3D.FromObj(path)` | Returns `null` for missing files, invalid face indices, invalid numeric tokens, empty geometry, malformed syntax, or oversized accumulators | None |
| `Mesh3D.FromStl(path)` | Returns `null` for missing files, unreadable files, wrong magic, truncated binary payloads, malformed ASCII payloads, or oversized files | Degenerate triangles are skipped as before |
| `SceneGraph.Load(path)` | Returns `null` for missing, unreadable, non-JSON, malformed, corrupt, or oversized `.vscn` content | None |
| `Pixels.Load(path)` and image loads reached from materials | Return `null` for missing, unreadable, wrong-magic, corrupt, unsupported, or oversized PNG/JPEG/BMP/GIF content | Material loaders catch this and record a warning instead of failing the whole model |

FBX files are limited to 256 MiB by default. Set `ZANNA_FBX_MAX_FILE_BYTES`
to opt into a larger host-specific ceiling; values above the runtime's 1 GiB
hard cap are clamped. A separate 1 GiB aggregate load budget covers file bytes,
typed graph metadata/properties, expanded arrays, hash/adjacency indexes, and
generated animation samples. `ZANNA_FBX_MAX_LOAD_BYTES` can only lower that
aggregate budget. Exhaustion releases the staged document and returns `null`.

ASCII FBX files (the standard `; FBX` comment header, or bare node text) use a
length-bounded, brace-scoped lexer/parser and populate the same typed graph as
binary FBX. Multiple models, geometries, materials, skeletons, connections, and
animations therefore use the same extractor and transform/curve semantics in
both encodings. Comments, quoted decoys, and closed sibling scopes cannot
satisfy lookups. Names are dynamically sized for display, while numeric FBX IDs
remain the only object/animation identity. A supported FBX input must contain a
valid typed `Objects`/`Connections` graph. Every Geometry record declared as
type `Mesh` must decode to at least one valid, non-degenerate triangle;
malformed, non-finite, or empty declared mesh geometry rejects the complete load
as corrupt instead of publishing a successful empty asset. Geometry type
`Shape` and other non-mesh records remain available to their dedicated import
passes. `unsupported` is reserved for a recognized feature or representation
the runtime does not implement, not malformed content claiming a supported FBX
structure.

Static scene nodes, skeleton binds, and keyed poses use one composer for pivot,
pre/post rotation, rotation order, geometric transform, and inheritance mode.
Constant segments preserve the immediately preceding FBX tick, linear segments
remain linear, and cubic Hermite segments subdivide adaptively to the configured
time/value error bound. Any budget or key-capacity failure discards the staged
clip instead of returning a truncated animation.

### FBX scene and animation fidelity

The dependency-free FBX importer handles binary FBX 7100–7700 and brace-scoped
ASCII FBX through the same typed object/connection graph. Its current fidelity
boundary is explicit:

| FBX content | Imported behavior |
|---|---|
| Meshes and materials | Polygon meshes, layer attributes, material bindings, textures, multiple mesh Models, and per-polygon material slots are imported. Multi-material geometry becomes material-specific child meshes. Rational open/closed/periodic NURBS surfaces and linear, Bézier, quadratic Bézier, cardinal, and B-spline patches are tessellated into ordinary meshes with UVs and derivative normals. |
| Scene hierarchy | Every non-skeleton `Model` becomes a named `SceneNode3D`. Full local transforms, pivots, pre/post rotations, geometric transforms, inheritance mode, parent hierarchy, and Z-up basis conversion are preserved. Numeric FBX IDs and stable `Objects` ordinals establish identity; names are display/fallback data only. |
| Skeletons and skinning | Bone hierarchy, bind transforms, the strongest four influences per vertex, and `AnimationStack` skeletal clips are imported. |
| Cameras | Perspective/orthographic projection, vertical or horizontal FOV (with animated aperture/focal-length conversion), aspect, clipping planes, orthographic zoom, evaluated world transform, and `LookAtProperty`/`UpVectorProperty` targets become `Camera3D` objects attached to their authored nodes. Transform and scalar projection curves drive independent camera clones after scene instantiation. FBX contributes one scene, so all cameras belong to scene index `0`. |
| Lights | Point, directional, spot, rectangle-area, sphere-area, and volume attributes become node-local native `Light3D` objects. Color, percent intensity, enabled/shadow flags, spot cones, oriented emitter basis/dimensions, radius, range, and none/linear/quadratic/cubic decay are retained. |
| Animation layers | Every stack composes connected layers in source order, including mute/solo, animated layer weight, additive/override/override-passthrough channels, rotation accumulation, and additive/multiplicative scale accumulation. The same evaluator feeds skeletal, object, morph, camera, and constraint samples. |
| Constraints | Position, rotation, scale, parent, aim, and single-chain IK constraints are resolved by numeric object ID and baked into static poses and animation samples. Source/overall weights, axis masks, maintain-offset state, world/up targets, poles, and authored axes are honored; active dependency cycles and structurally invalid graphs reject the load transactionally. |
| Object animation | Non-skeleton local translation/rotation/scale curves become one `NodeAnimation3D` per animation stack. Sampling preserves constant edges, authored linear keys, adaptive cubic motion, transform inheritance, quaternion continuity, animation-layer composition, and constraint dependencies. Channels bind by stable source-node index and play automatically on an instantiated `SceneAsset`. |
| Blend shapes | Float/double position, normal, and tangent deltas plus 32/64-bit control-point indexes are remapped through triangulation. Static `DeformPercent`, progressive `FullWeights`, animated deform curves, multiple channels, and material-split clones are preserved as `MorphTarget3D` plus node weight channels. |

Progressive morph channels interpolate base → first shape before the first full
weight, cross-fade adjacent shapes between full-weight thresholds, and hold the
last shape fully active beyond the final threshold. Invalid or non-increasing
thresholds use deterministic 100-percent steps and add a warning. Generated
animation keys include every crossed threshold so runtime linear interpolation
cannot skip a progressive-morph corner.

`Zanna.Graphics3D.Fbx` exposes `MeshCount`, `AnimationCount`,
`NodeAnimationCount`, `CameraCount`, and `MaterialCount`, with matching
`GetMesh`, `GetSkeleton`, `GetAnimation`, `GetAnimationName`,
`GetNodeAnimation`, `GetNodeAnimationName`, `GetCamera`, `GetMaterial`, and
`GetMorphTarget` methods. Loading the same file through `SceneAsset` carries the
hierarchy, camera membership, lights, skeletal clips, node/object clips, and
morph data into instancing and VSCN v5 bake/reload.

The remaining boundaries are explicit. A surface with an active trim region is
rejected until trim curves have a runtime representation. Unknown custom
constraint classes may be reported but are not claimed as evaluated built-ins.
Malformed control nets, invalid knots, cyclic constraints, non-finite layer or
camera curves, impossible light dimensions, and generated data beyond the
document budgets reject the complete FBX transaction. Rectangle/sphere shadows
currently use the existing center-point shadow approximation, and volume lights
do not claim shadow slots.

glTF extension support is explicit:

| Extension | `extensionsRequired` | Optional `extensionsUsed` | Degradation behavior |
|-----------|----------------------|---------------------------|----------------------|
| `KHR_texture_transform` | Supported | Supported | UV transform metadata is imported |
| `KHR_materials_emissive_strength` | Supported | Supported | Emissive intensity is imported |
| `KHR_materials_unlit` | Supported | Supported | Material uses unlit shading |
| `KHR_materials_specular` | Supported | Supported | Specular factors/textures are imported |
| `KHR_materials_pbrSpecularGlossiness` | Supported | Supported | Legacy spec-gloss converts to metallic-roughness (`roughness = 1 - glossiness`, metallic from the max specular component vs dielectric 0.04). A specularGlossinessTexture that decodes to CPU pixels also converts per-texel into a synthesized metallic-roughness map (factors baked in); the source texture additionally binds to the specular slot |
| `KHR_lights_punctual` | Supported | Supported | Punctual lights are imported |
| `KHR_materials_variants` | Supported | Supported | Variant names import onto the `SceneAsset` (`VariantCount`/`GetVariantName`); per-primitive material mappings apply through `ApplyVariant` |
| `EXT_meshopt_compression` | Supported | Supported | Compressed bufferViews decode transparently (ATTRIBUTES/TRIANGLES/INDICES modes plus OCTAHEDRAL/QUATERNION/EXPONENTIAL filters); data-less placeholder fallback buffers are accepted; malformed streams fail the load with a named Corrupt diagnostic |
| `KHR_mesh_quantization` | Supported | Supported | Quantized POSITION (any 8/16-bit integer type), NORMAL/TANGENT (normalized signed byte/short), and TEXCOORD (any 8/16-bit integer type) attributes — including morph-target deltas — decode to floats per the accessor rules; dequantization node scales apply through the scene hierarchy as authored |
| `KHR_texture_basisu` | Supported | Supported | KTX2 textures decode fully on the CPU: BasisLZ/ETC1S (supercompression scheme 1) and UASTC LDR 4x4 (scheme 0/Zstd/ZLIB) both decode to RGBA8 pixels |
| `KHR_materials_clearcoat` | Not supported as required | Optional, factor-level lobe | A real second specular lobe (GGX at the coat roughness, Schlick F0 0.04, Kelemen visibility, energy-conserving base attenuation) renders on all four backends from `clearcoatFactor`/`clearcoatRoughnessFactor`; the coat textures are validated but not sampled (the GL fragment stage is at its 16-unit floor) |
| `KHR_materials_transmission` | Not supported as required | Optional approximation supported | Transmission factor is recorded (custom param 3) and drives the volume absorption tint; the surface remains opaque (no refraction pass) |
| `KHR_materials_ior` | Not supported as required | Optional, applied | The index of refraction replaces the fixed 0.04 dielectric F0 in the PBR branch on all four backends (custom param 4) |
| `KHR_materials_sheen` | Not supported as required | Optional, factor-level lobe | The RGB sheen color folds to a luminance intensity (custom param 0) with roughness in param 7; a Charlie-distribution lobe with Neubelt-Pettineo visibility renders on all four backends plus an irradiance-scaled ambient term |
| `KHR_materials_anisotropy` | Not supported as required | Optional, factor-level lobe | `anisotropyStrength`/`anisotropyRotation` (custom params 8/9) drive an elliptical GGX distribution along the rotated tangent direction on the GPU backends; the software rasterizer renders isotropic (its lighting path has no tangent frame) and the anisotropy texture is validated but not sampled |
| `KHR_materials_volume` | Not supported as required | Optional approximation supported | `thicknessFactor` plus a Beer-Lambert absorption coefficient folded from `attenuationColor`/`attenuationDistance` tint transmitted light on all four backends (custom params 5/6) |
| `KHR_draco_mesh_compression` | Supported | Supported | Sequential and edgebreaker encodings decode fully: rANS entropy coding, standard + valence traversal, topology splits, attribute seams, depth-first and prediction-degree traversers, difference/parallelogram/constrained-multi/tex-coords/geometric-normal prediction, wrap/octahedron transforms, quantized positions/texcoords/normals, skinning attributes. Draco point clouds fail the load with a named diagnostic |
| Other extensions | Not supported | Ignored with a load warning | Visual result may miss extension-specific material, geometry, animation, lighting, or texture behavior |

Unsupported required extensions fail the load and name the extension list, for
example `GLTF.Load: requires EXT_texture_webp (unsupported)`.

A glTF node's scalar `extras` entries import as typed `SceneNode` metadata:
strings, integral/fractional numbers, booleans, and null become string, int,
float, bool, and null values. Nested objects and arrays, oversized keys or
strings, non-finite numbers, and entries beyond the metadata limit are skipped
with one bounded warning per node. `AssetDiagnostics3D.GetImportReport` and
`zanna asset validate` expose the warning, and VSCN persistence carries the
accepted metadata through later instantiation.

Plain `.gltf` input is length-aware and rejects embedded NUL bytes. Synchronous
and preload `.glb` loads use the same bounded GLB 2.0 chunk iterator, so missing
JSON/BIN chunks, invalid order, truncated payloads, and trailing bytes fail the
same way. Recognized integer and boolean-like fields require one complete exact
JSON token and the schema-appropriate JSON type. The raw scanner accepts only
the four JSON whitespace bytes and well-formed UTF-8 scalar encodings; malformed
continuations, overlong encodings, surrogate encodings, and out-of-range code
points fail the load. Escaped surrogate pairs are validated. A decoded U+0000
cannot be returned through a C-string-backed field.

Container lookup is transactional: malformed object/array suffixes invalidate
an earlier candidate instead of exposing a valid prefix, exact caller-supplied
ranges cannot be escaped, and duplicate queried object keys are rejected as
ambiguous. Root definition arrays, scene index arrays, attribute maps,
morph-target maps, and recognized extension payloads retain their distinct
schema types; valid sparse-accessor `indices`, animation `target`,
`KHR_materials_variants`, and `EXT_meshopt_compression` forms remain accepted.

Each imported texture slot preserves wrap plus independent minification,
magnification, and mip-filter axes through Canvas3D and every backend. UV0 and
UV1 are supported only when the primitive actually carries the selected stream;
a missing stream or UV2+ fails with an asset diagnostic instead of silently
sampling UV0.

glTF/GLB and FBX also retain exact PNG, JPEG, GIF, BMP, and KTX2 bytes whenever
the complete encoded payload is available from a data URI, buffer view,
embedded FBX `Content`, packed asset, preload bundle, or dependency file.
Materials still render through decoded Pixels (or native KTX2 blocks), but the
unresolved slot remains a source-backed `TextureAsset3D`. This provenance is
bounded, included in `TextureAsset3D.RetainedBytes`, and carried through VSCN
v5. If native code mutates the decoded surface, the stale encoded source is
invalidated and the next save stores the current RGBA8 texels.

Writable `Zanna.Graphics3D` properties expose `get_X` / `set_X` runtime
accessors and language-level property assignment where supported, such as
`Material3D.set_Roughness(material, value)` or `material.Roughness = value`.

---

## Camera And Rendering

### Canvas3D Render-Settings Readback (ADR 0233)

Every write-only Canvas3D render setting now has a read-only property peer
over the retained state, so options menus and inspectors can render their own
current values:

| Property | Type | Mirrors |
|----------|------|---------|
| `ShadowBias` / `ShadowSlopeBias` / `ShadowStrength` | Double | `SetShadowBias` / `SetShadowSlopeBias` / `SetShadowStrength` |
| `ShadowQuality` / `ShadowCascades` / `ShadowBudget` / `ClusterLightBudget` | Integer | The matching `Set*` methods |
| `BackfaceCull` / `FrustumCulling` / `OcclusionCulling` / `TextureStreaming` / `ForceCpuSkinning` | Boolean | The matching `Set*` methods |
| `TextureStreamingBias` | Double | `SetTextureStreamingBias` |
| `MaxDeltaTime` | Integer | `SetMaxDeltaTime` (milliseconds; `0` = uncapped) |
| `FogEnabled` | Boolean | True between `SetFog` and `ClearFog` |
| `FogNear` / `FogFar` | Double | The sanitized `SetFog` distances |
| `FogColor` / `AmbientColor` | Object (`Vec3`) | Fresh snapshots of the retained colors |
| `Skybox` / `RenderTarget` / `PostFX` | Object | Borrowed retained handles (`CubeMap3D` / `RenderTarget3D` / `PostFX3D`), `null` when unbound |
| `ClipRectActive` | Boolean | True while an overlay clip rect is set |
| `ClipRectX` / `ClipRectY` / `ClipRectWidth` / `ClipRectHeight` | Integer | The retained overlay clip rect (zeros while inactive) |

### Depth Precision (Reversed-Z)

The GPU backends (Metal, D3D11, OpenGL) render scene passes with reversed-Z
float depth: Canvas3D negates the projection's z row per backend, depth clears
to 0 with Greater-family compares, and the skybox sits at the far plane of 0.
Float precision then concentrates in the distance — where standard-Z starves
it — eliminating distant z-fighting shimmer at open-world clip ranges (near
0.1, far 5000+). This is entirely internal: `Camera3D`, `Mat4.Perspective`,
and every public math API keep the standard GL convention, shadow maps keep
the standard convention on every backend, and the software backend stays
standard as the deterministic golden reference. Depth-consuming effects (soft
particles, SSAO, SSR, TAA) and the scene-depth probes account for the
convention internally.

### Canvas3D View-Model Pass

`Canvas3D.BeginViewModel(camera, fovYDegrees)` opens a secondary 3D pass over the
finished scene for first-person weapon/hands rendering. Call it after `End` of the
world pass and close it with `End` before HUD overlays. The pass keeps the scene's
color buffer, clears depth, and renders with the camera's view but an independent
vertical FOV (pass `<= 0.0` to keep the camera's FOV):

- view-model draws can never depth-clip against world geometry, yet still
  self-occlude correctly;
- the world FOV stays independent, so a 90° world camera can render a 54° weapon;
- draws use the canvas light set and *receive* the world pass's shadow maps but
  never cast shadows, and the skybox is not redrawn;
- post-FX applies to the composited result (weapon included), and HUD overlays
  still draw on top.

```zia
Canvas3D.Begin(canvas, worldCam)
// ... world draws ...
Canvas3D.End(canvas)
Canvas3D.BeginViewModel(canvas, worldCam, 54.0)
Canvas3D.DrawMeshSkinned(canvas, weaponMesh, weaponXform, weaponMat, pose)
Canvas3D.End(canvas)
```

### Canvas3D Frame Finalization

`Canvas3D.End()` closes the current 3D or 2D draw pass. It does not present the
frame and does not run post-processing. Finalization is the step that applies
`PostFX3D` and composites any final overlay recorded for the frame.

| Method | Signature | Description |
|--------|-----------|-------------|
| `BeginOverlay()` | `Void()` | Start recording a final 2D overlay pass |
| `EndOverlay()` | `Void()` | Finish final overlay recording |
| `ClearOverlay()` | `Void()` | Discard the current frame's recorded final overlay |
| `DrawRect3D(x, y, w, h, color)` | `Void(Integer, Integer, Integer, Integer, Integer)` | Draw a filled screen-space rectangle (used in overlay/2D passes) |
| `DrawText3D(x, y, text, color)` | `Void(Integer, Integer, String, Integer)` | Draw screen-space text (used in overlay/2D passes) |
| `Resize(w, h)` | `Void(Integer, Integer)` | Resize the canvas and active backend output targets |
| `FinalizeFrame()` | `Void()` | Apply post-FX and final overlay once, without presenting |
| `ScreenshotFinal()` | `Object()` | Finalize if needed, then capture finalized pixels |
| `TryCopyScreenshotTo(pixels)` | `Boolean(Object)` | Copy the active output into a same-size reusable `Pixels` object |
| `TryCopyScreenshotFinalTo(pixels)` | `Boolean(Object)` | Finalize if needed, then copy into a same-size reusable `Pixels` object |
| `FrameFinalized` | `Boolean` | True once the current frame has been finalized |

Use final overlays for HUD text, reticles, debug labels, and capture annotations
that must remain crisp after bloom, tonemapping, or color grading. `Flip()`
finalizes automatically if the frame has not already been finalized, so
`ScreenshotFinal()` can be followed by `Flip()` without re-running post-FX.
For repeated captures, allocate a `Pixels` destination once and call
`TryCopyScreenshotFinalTo()`. It returns false for invalid handles, mismatched
dimensions, or failed backend readback and leaves ownership with the caller.
Successful copies update the destination generation for texture-cache
invalidation; GPU canvases reuse their staging allocation across calls.

```zia
Canvas3D.Begin(canvas, cam);
Canvas3D.DrawMesh(canvas, mesh, model, material);
Canvas3D.End(canvas);

Canvas3D.BeginOverlay(canvas);
Canvas3D.DrawText2D(canvas, 12, 12, "READY", 0xFFFFFFFF);
Canvas3D.EndOverlay(canvas);

var capture = Canvas3D.ScreenshotFinal(canvas);
Canvas3D.Flip(canvas);
```

The repo-level sample `examples/3d/walk_min.zia` shows the complete Phase 0B
frame path in one small program: explicit default lighting, a primitive scene,
CPU-safe post-FX, final overlay recording, and `ScreenshotFinal()` coverage.
`examples/3d/walk_min_probe.zia` is registered as a software-backend ctest and
compares against `examples/3d/baselines/walk_min_software.png`.

### Canvas3D Window, Image, and Foliage Helpers

- **Fullscreen** — `Canvas3D.SetFullscreen(canvas, on)`, `Canvas3D.ToggleFullscreen(canvas)`,
  and `Canvas3D.get_IsFullscreen(canvas)` switch the backing window between windowed and
  native desktop fullscreen (implemented per-platform on Cocoa, Win32, and X11). The canvas
  re-syncs its size on the toggle and the per-frame projection derives aspect from the active
  output, so the view stays un-stretched. `Game3D.Keys.get_KeyF11` pairs naturally with these.
- **Overlay image blit** — `Canvas3D.DrawImage2D(canvas, x, y, w, h, pixels)` blits a `Pixels`
  image into the 2D overlay (unlit, screen-space) scaled to `w×h`. A `RenderTarget3D` is an
  accepted source too: pass the target itself (not `AsPixels`) to show a rendered texture, such
  as a top-down minimap or a picture-in-picture camera, on the HUD — the blit carries the
  target's display-referred resolve on every backend (ADR 0301), whereas `AsPixels` is the
  scene-referred readback.
  `DrawImage2DRegion(canvas, x, y, w, h, pixels, sx, sy, sw, sh)` blits only the source
  sub-rectangle — the primitive behind sprite-sheet HUD icons and nine-slice panels.
- **Overlay primitives** — the HUD layer has the full 2D-canvas drawing vocabulary:
  `DrawLine2D(x0, y0, x1, y1, color, alpha)`, `DrawFrame2D(x, y, w, h, color, alpha)`,
  `DrawRoundRect2D` / `DrawRoundFrame2D(x, y, w, h, radius, color, alpha)`,
  `DrawText2DScaled(x, y, text, color, scale)` and `MeasureText2D(text, scale)`.
  `SetClipRect2D(x, y, w, h)` restricts subsequent overlay drawing to a screen
  rectangle (clipping happens canvas-side at enqueue time, so every backend behaves
  identically — the scrolling-list building block); `ClearClipRect2D()` removes it.
- **HUD widgets** — the entire `Zanna.Game.UI.*` widget set (HudLabel, Bar, Panel,
  MenuList, Modal, sliders, tables, tooltips, text input, …) draws directly on a
  Canvas3D: pass the Canvas3D handle to any widget `Draw` call. One widget
  implementation serves both canvases (ADR 0065); custom `Font` objects currently
  render with the built-in font on Canvas3D. Forward input from `Game3D.Mouse`/`Keys`
  to the widgets' `HandleMouseClick`/`HandleKey` methods.
- **Wind foliage** — `Canvas3D.DrawMeshWind(canvas, mesh, transform, material, dirX, dirZ, strength, phase)`
  draws a mesh with a height-weighted per-vertex sway: the base (lowest local-Y) stays planted
  while the canopy bends along `(dirX, dirZ)` by `sin(phase)`. Pass per-instance `phase` offsets so
  a cluster ripples instead of pulsing. It runs on every backend because the geometry is deformed
  before submission rather than in a vertex shader.
- **Skinned draw** — `Canvas3D.DrawMeshSkinned(canvas, mesh, transform, material, animator)` accepts
  an `AnimController3D` as well as an `AnimPlayer3D`, so a state-machine pose (idle/walk crossfades)
  can skin the mesh directly.
- **Skinned crowds** — `Canvas3D.DrawInstancedSkinned(batch, players)` draws an `InstanceBatch3D`
  with one animator per instance; see the InstanceBatch3D section for the palette-packing details.

### Canvas3D Synthetic Input and Clock

Live loops call `Canvas3D.Poll()` once per frame and then read
`Zanna.Input.Keyboard`, `Zanna.Input.Mouse`, gamepad, or action APIs. `Poll()`
returns `1` while the canvas remains open and `0` when the backing window is
closed or event pumping fails. Use `PollEvent()` when low-level integrations need
raw window event types; gameplay should normally use the state APIs because they
expose coherent `WasPressed`, `WasReleased`, held, and mouse-delta state.
With the live clock selected, `Poll()` also advances `DeltaTime` so manual loops
that tick simulation before presentation do not depend on `Flip()` for timing.

Deterministic tests can switch a canvas to scripted input and fixed time:

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetInputSource(mode)` | `Void(Integer)` | `0` live, `1` synthetic, `2` live plus synthetic |
| `PushSyntheticKey(key, down)` | `Void(Integer, Boolean)` | Queue a synthetic key transition |
| `PushSyntheticMouse(dx, dy, buttons, wheel)` | `Void(Double, Double, Integer, Double)` | Queue mouse delta, button bitmask, and vertical wheel |
| `ClearSyntheticInput()` | `Void()` | Clear queues and release synthetic-held buttons/keys |
| `SetClockSource(mode)` | `Void(Integer)` | `0` live wall clock, `1` fixed synthetic dt |
| `SetSyntheticDeltaTimeSec(dt)` | `Void(Double)` | Configure fixed synthetic dt in seconds |
| `AdvanceSyntheticFrame()` | `Void()` | Advance one deterministic input/timing frame without pumping OS events |

Synthetic events are applied through the same keyboard and mouse state update
functions as live events. That means camera controllers and action bindings do
not need a test-only input path. `DeltaTime`/`DeltaTimeSec` can be zero on the
first live frame; with the synthetic clock selected, they report the configured
fixed dt after `AdvanceSyntheticFrame()`, synthetic `Poll()`, or `Flip()`.

`Canvas3D.Width` / `Height` report the active output. They follow the bound
`RenderTarget3D` during RTT passes. `WindowWidth` / `WindowHeight` always report
the backing window, and `ActiveOutputWidth` / `ActiveOutputHeight` are explicit
aliases for the active-output behavior.
`Canvas3D.Resize(width, height)` updates the backing window size and the backend
framebuffer size together; the next `Begin(camera)` uses the resized active
output aspect without mutating the camera's stored projection.

`PollEvent()` drains the per-canvas event queue in FIFO order and returns `0`
when no queued event remains.

### Canvas3D Backend Selection

`Canvas3D.New(title, width, height)` selects the platform GPU backend by
default: Metal on macOS, Direct3D 11 on Windows, and OpenGL 3.3 on Linux. If
the selected GPU backend cannot create its context, Canvas3D automatically
creates the software backend instead and emits one stderr notice for the process.
Windows on ARM64 defaults to software because affected drivers can fail during
presentation; users can still opt into D3D11 with `ZANNA_3D_BACKEND=d3d11`.

When the software backend is active, the main color raster pass and software
shadow-map pass use fixed 64x64 tiles and a context-owned worker pool. The
default worker count is `min(hardware_threads, 8)`. Set
`ZANNA_3D_SW_THREADS=1` before creating the canvas to force the exact serial
path; positive values above one are clamped to `1..8`. Tile bins preserve mesh
submission order inside each tile, so depth, alpha, blending, texture sampling,
fog, lighting, and shadow depth generation remain byte-deterministic across
worker counts.

| Property / Method | Type | Description |
|-------------------|------|-------------|
| `Backend` | `String` | Active renderer name: `software`, `metal`, `d3d11`, or `opengl` |
| `BackendFallback` | `Boolean` | True when Canvas3D fell back from the selected GPU backend to software at creation |
| `BackendSupports(name)` | `Boolean` | Tests backend capabilities; `runtime-fallback`, `backend-fallback`, and `software-fallback` report the `BackendFallback` state |

Use `BackendSupports("gpu")`, `BackendSupports("software")`, or
`BackendFallback` for control flow. String comparisons against `Backend` or
the value returned by `get_Backend` should be reserved for logs and diagnostics.

### Canvas3D Quality Profiles

`Canvas3D.SetQuality(profile)` installs a backend-safe post-FX profile:

| Profile | Value | Behavior |
|---------|-------|----------|
| Performance | `0` | Minimal CPU-safe chain |
| Balanced | `1` | CPU-safe bloom, tonemap, FXAA, and color grade |
| Cinematic | `2` | CPU-safe cinematic chain, plus SSAO/DOF/motion blur only on GPU-window post-FX |

The fallback state is inspectable for debug overlays:

| Property | Type | Description |
|----------|------|-------------|
| `QualityRequested` | `Integer` | Last requested profile |
| `QualityActive` | `Integer` | Active profile after fallback |
| `QualityFallback` | `Boolean` | True when unsupported GPU-only effects were omitted |
| `QualityFallbackReason` | `String` | Empty when no fallback occurred |

`PostFX3D.NewQuality(canvas, profile)` builds the same chain without attaching
it. Game3D quality helpers should wrap these runtime APIs rather than hand-roll
backend checks in Zia code.

### Canvas3D Lighting Helpers

`Canvas3D.SetDefaultLighting()` installs an explicit key/fill directional setup
and a readable ambient color. It is not implicit fallback lighting: scenes stay
dark after `ClearLights()` plus low ambient unless the caller opts into new
lights.

| Method / Property | Signature | Description |
|-------------------|-----------|-------------|
| `SetLight(index, light)` | `Void(Integer, Object)` | Bind or clear a retained `Light3D` slot; invalid indices or non-light handles trap |
| `ClearLights()` | `Void()` | Clear every retained canvas light slot |
| `SetDefaultLighting()` | `Void()` | Install conservative key/fill/ambient defaults |
| `LightCount` | `Integer` | Count active enabled canvas-slot lights |
| `MaxActiveLights` | `Integer` | Current active-light budget for the selected lighting path |
| `ClusteredLighting` | `Boolean` property | Clustered forward+ lighting; defaults on for GPU backends, enabling it on an unsupported backend traps |
| `TrySetClusteredLighting(enabled)` | `Boolean` method | Attempts to toggle clustered forward+ lighting and returns `False` instead of trapping when unsupported |
| `SetAmbient(r, g, b)` | `Void(Double, Double, Double)` | Set ambient color |
| `IblEnabled` | `Boolean` (read/write) | Light PBR ambient from the skybox environment via image-based lighting |
| `IblIntensity` | `Double` (read/write) | Scale the environment-lighting contribution (default `1.0`, clamped to `[0, 8]`) |
| `SetShadowCascades(count)` | `Void(Integer)` | Request cascaded shadow maps; counts above `1` require backend CSM support (default `2` on CSM-capable backends, `1` elsewhere) |
| `SetShadowDistance(d)` | `Void(Double)` | Cap the camera distance covered by directional shadow fitting; `<= 0` restores the automatic `min(camera far, 300)` default |
| `ShadowDistance` | `Double` | Configured shadow distance (`0` = automatic) |
| `SetShadowStrength(s)` | `Void(Double)` | How dark fully-occluded texels get (`0` = shadows off, `1` = black; default `0.85`) |
| `SetShadowQuality(tier)` | `Void(Integer)` | Shadow PCF tier: `0` = 4 taps, `1` = 8 (default), `2` = 16 rotated-Poisson taps |
| `SetShadowBudget(n)` | `Void(Integer)` | Cap the shadow slots a frame may use (1..20, default all) |
| `ShadowSlotsUsed` | `Integer` | Shadow slots rendered in the latest frame, cascades and cube faces included |
| `ShadowRequestsDropped` | `Integer` | Shadow-requesting lights denied a slot in the latest frame |
| `SetClusterLightBudget(n)` | `Void(Integer)` | Compact per-cluster list capacity (8..64, default 64); excess demand uses lossless full-light fallback |
| `ClusterOverflowCount` | `Integer` | Lifetime count of actually lost cluster light entries; the lossless builder contributes zero |
| `ClusterFallbackEntryCount` | `Integer` | Saturating lifetime local-list entries served by full-light capacity fallback; pressure, not lost lighting (ADR 0328) |
| `DroppedLightCount` | `Integer` | Enabled lights truncated by the fixed-forward 16-light cap this frame |

Image-based lighting (`IblEnabled`) replaces the flat ambient term on PBR
materials with real environment lighting derived from the canvas skybox:
diffuse comes from an SH-9 irradiance projection and specular from a
GGX-prefiltered mip chain sampled by roughness, combined with a CPU-precomputed 64x64 split-sum environment-BRDF table (identical on every backend; the GPU backends sample it as a texture, the software rasterizer reads it directly).
Enabling IBL and setting a skybox are cheap state changes. The environment
payload is computed lazily by the first eligible PBR draw and then reused.
Materials with an explicit `SetEnvMap` keep the legacy reflectivity-mix
behavior; unlit and Blinn-Phong materials are unaffected. All four backends
(Metal, D3D11, OpenGL, software) implement the same math.

The default path remains fixed forward lighting with `MaxActiveLights == 16`.
`BackendSupports("clustered-lighting")` gates the many-light path; enabling it
without support traps before mutating the canvas so fallback behavior stays
explicit. The software backend advertises this capability as the correctness
baseline and raises the bounded active-light payload to 64. The real Metal,
D3D11, and OpenGL backend vtables also advertise it: their main shaders and
light-upload paths consume the bounded 64-light payload, while synthetic/fake
test backends with GPU-like names do not advertise production support. The
open-world GPU smoke records a 24-light run against the 16-light fallback.
`BackendSupports("shadow-csm")` similarly gates cascaded shadows;
`SetShadowCascades(1)` preserves the non-cascaded shadow path. On supporting
software and real platform GPU backends, counts above one render the primary
directional shadow caster into up to four camera-depth cascades, publish split
metadata in the backend light payload, and keep unsupported/fake backends
trapping before mutation. The open-world GPU smoke records a 3-cascade Metal
fixture (`CSM_SHADOWS`) after the clustered-lighting probe.

Directional shadow coverage is bounded by `SetShadowDistance` instead of
spanning the whole clip range: cascade splits (a lambda-0.75 practical split
series) and the single-map orthographic fit both clamp to it, concentrating
shadow-map texels near the camera — an unbounded 5000-unit far plane would
otherwise collapse near-shadow resolution into blocky shimmer. Every backend
cross-fades the last ~12% of each cascade into the next (no visible line or
pop at the split plane) and fades directional shadows out approaching the
distance cap instead of ending on a hard edge. Scene traversal additionally
sweeps node bounds along the shadow-casting sun's travel direction before
frustum-culling, so an off-screen caster keeps its on-screen shadow instead of
popping when it leaves the view frustum; nodes admitted only by the sweep are
still frustum-culled from the main color pass.

When more point/spot lights are enabled than the active light budget allows,
the flattener keeps the most relevant ones — scored by intensity over the
shader's distance falloff from the camera, with a 10% incumbent boost so
near-ties never swap membership frame-to-frame — instead of truncating by slot
order. `DroppedLightCount` still reports the overflow.

Shadow storage is twelve slots: the first four are classic per-texture slots and
the remaining eight are tiles of a shared depth atlas on the GPU backends
(software keeps per-slot buffers; OpenGL 3.3's 16-sampler budget is fully
assigned, so GL stays on the four classic slots and never receives higher
indices). CSM cascades size against their own four-cascade limit, so a
3-cascade sun plus several shadowed spot lights coexist instead of exhausting a
shared four-slot pool. Enabled directional lights with `CastsShadows` are
selected first by luminance-weighted intensity; remaining budget goes to spot
and point lights with `CastsShadows`, ranked by intensity and distance to the
active camera. Directional lights may use cascades; spot lights use one
perspective map built from the light position, direction, outer cone angle, and
range. **Point lights cast omnidirectional shadows**: a granted point light
claims six consecutive slots (one 92-degree perspective face per axis) and the
shaders pick the face by the dominant axis of the light-to-fragment vector —
`BackendSupports("shadow-point")` reports availability (true wherever the
extended slots exist). `SetShadowBudget` caps per-frame slot spending, and
`ShadowSlotsUsed` / `ShadowRequestsDropped` make the budget observable —
overflow is never silent.

Shadow filtering uses a 16-point Poisson-disk PCF rotated per pixel, with the
tap count selected by `SetShadowQuality` (the software backend caps at 8 taps).
On the GPU backends every tap goes through a hardware comparison sampler
(`sample_compare` / `SampleCmpLevelZero` / `sampler2DShadow`), so each tap is a
filtered 2x2 depth compare rather than a point fetch.
Receivers are offset along the surface normal by about 1.5 shadow texels before
projecting into light space, and the depth compare adds a slope-proportional
per-texel bias — so `SetShadowBias` stays small (it is the constant base term)
and `SetShadowSlopeBias` scales the grazing-angle term. Because both the offset
and the slope term derive from each slot's actual texel footprint, far cascades
automatically receive proportionally larger bias. If shadows detach from their
casters ("peter-panning"), lower `SetShadowSlopeBias`; if surfaces show striping
("acne") at grazing sun angles, raise it.

`BackendSupports("native-texture:bc1")` through
`BackendSupports("native-texture:bc7")`, `native-texture:etc2`, and
`native-texture:astc` report native compressed texture upload support for the
active backend/device. Bare `bc1`/`bc3`/`bc4`/`bc5`/`bc7`/`astc`/`etc2` names
remain accepted as legacy native-upload aliases. The older `texture:*` names
report runtime CPU fallback coverage. That probe is true for either partial or
full coverage; the authoritative internal table keeps those qualities distinct.

KTX2 supercompression schemes 1 (BasisLZ/ETC1S), 2 (Zstandard), and 3 (ZLIB)
are decoded per level by the runtime's from-scratch codecs. ZLIB validates its
header, dictionary flag, bounded DEFLATE stream, exact output size, trailing
input, and Adler-32. Zstandard likewise requires the exact destination length
and complete frame consumption. Malformed KTX2 never traps: loads return null
and the diagnostic is available through
`AssetDiagnostics3D.GetLoadWarnings()`. Native mip payload lengths are
validated against the declared format/block dimensions before publication.

`TextureAsset3D.LoadKtx2` and `LoadKtx2Asset` remain permissive for recognized
partial decoders: they return a checker-backed asset with `Degraded = true` and
a stable `DegradedReason`. `LoadKtx2Strict` and `LoadKtx2AssetStrict` return
null for the same decode failure. This lets tools reject degraded content while
existing games keep their visible compatibility fallback.
`BackendSupports("anisotropy")` reports whether the active GPU backend applies
`Material3D.Anisotropy`; the software backend accepts the property but ignores
it and reports false.

| Texture format | Software / RenderTarget3D CPU path | Native GPU upload path | Undecodable CPU blocks |
|----------------|-------------------------------------|------------------------|------------------------|
| RGBA8 KTX2 | Full decode to `Pixels` | Uploaded as ordinary RGBA texture | Load fails on malformed bytes |
| BC1 KTX2 | Full DXT1 decode (opaque + punch-through) to `Pixels` | Device-dependent `native-texture:bc1` key | Load fails on malformed bytes |
| BC3 KTX2 | Full BC3/DXT5 decode to `Pixels` | Device-dependent `native-texture:bc3` key | Load fails on malformed bytes |
| BC4/BC5 KTX2 | Single/two-channel decode to `Pixels` (R replicated / RG) | Device-dependent `native-texture:bc4`/`native-texture:bc5` keys | Load fails on malformed bytes |
| BC7 KTX2 | Full modes 0-7 decode to `Pixels`; `texture:bc7` reports true | Device-dependent `native-texture:bc7` key | Permissive checker / strict failure only for malformed data |
| BC6H KTX2 (signed + unsigned) | Partial HDR block fallback; implemented modes decode to clamped `Pixels` and the capability row reports partial | Software fallback only (no native HDR block upload yet) | Permissive checker / strict failure for an unsupported or corrupt block |
| ETC2 RGBA8/EAC KTX2 | Partial individual/differential color fallback; capability row reports partial | Device-dependent `native-texture:etc2` key | Permissive checker / strict failure for an unsupported or corrupt mode |
| ASTC KTX2 | Partial LDR 2D fallback including void-extent blocks; capability row reports partial | Device-dependent `native-texture:astc` key | Permissive checker / strict failure for an unsupported or corrupt block type |

All native block-compressed mip payloads are still retained internally for
capability-gated backend upload. On the software backend, the native upload keys
are false and the `texture:*` CPU keys are true for the decoder coverage above.
`TextureAsset3D.SetResidentMipRange` first reconstructs every entering fallback
from immutable canonical backing, publishes the new window, and then releases
decoded `Pixels` outside it. `ResidentBytes` falls on eviction; `RetainedBytes`
reports the exact bounded PNG, JPEG, GIF, BMP, or KTX2 source container when one
was imported, plus reconstructable mip backing and decoded mips still alive.
Moving a mip back into the window reproduces its original pixels.
Materials retain the texture asset and resolve that active fallback at draw
time, so already-bound materials follow later residency changes; when no
fallback exists, capable GPU backends receive the resident compressed blocks
through the same upload-budget telemetry path.

`Canvas3D.SetTextureStreaming(true)` automates that window management: each
frame, the canvas estimates the on-screen texel coverage of every draw that
binds a multi-mip `TextureAsset3D` (from the draw's bounds, transform, and the
frame camera) and aggregates a per-asset minimum desired mip. Resolution
promotions apply immediately at the asset's next draw; demotions require the
lower demand to hold for ~30 consecutive frames and drop one mip level at a
time, so a panning camera never pops. Textures smaller than 128 pixels on
their longest axis, single-mip assets, and instanced batches (whose
per-instance distances are unknown) always keep full resolution. Streaming decisions ride
the existing `SetTextureUploadBudget` row-sliced upload path, and
`SetTextureStreamingBias` shifts the quality/memory trade in either direction.
While enabled, streaming overrides manual `SetResidentMipRange` calls for
assets that are drawn; disabling stops further changes without restoring
earlier windows.

| Backend | `BackendSupports("anisotropy")` | Sampler behavior |
|---------|---------------------------------|------------------|
| Software | False | Accepted and ignored; software sampling remains controlled by `texture_filter` |
| D3D11 | True | Uses `D3D11_FILTER_ANISOTROPIC` with `MaxAnisotropy` clamped to `1..16` |
| Metal | True | Uses `MTLSamplerDescriptor.maxAnisotropy` clamped to `1..16` |
| OpenGL | Extension-dependent | Uses `GL_TEXTURE_MAX_ANISOTROPY_EXT` when `GL_EXT_texture_filter_anisotropic` is present, clamped to `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT` and `16` |

`Material3D.Anisotropy = 1` disables anisotropic filtering. Values below `1`
clamp to `1`; values above `16` clamp to `16`. GPU backends cache sampler
states by wrap mode, minification/magnification/mip filter axes, and effective
anisotropy, so changed settings reuse existing sampler objects once created.

### Canvas3D Performance Telemetry

| Member | Type | Description |
|--------|------|-------------|
| `TextureUploadBytes` | `Integer` property | Texture bytes uploaded into backend storage during the latest ended frame |
| `TextureUploadPendingBytes` | `Integer` property | Texture bytes still waiting for backend texture or cubemap upload budget |
| `FrameGpuTimeUs` | `Integer` property | Latest completed backend GPU frame time in microseconds, or `0` when unsupported/not yet available |
| `DrawsSubmitted` | `Integer` property | Backend draw submissions issued since the latest public `Begin`/`Begin2D` |
| `AabbTransforms` | `Integer` property | World-AABB transform computations performed since the latest public `Begin`/`Begin2D` |
| `SortPasses` | `Integer` property | Stable deferred sort passes run since the latest public `Begin`/`Begin2D` |
| `BackendStateChanges` | `Integer` property | Material/backend state runs observed in Canvas3D submission order |
| `SetTextureUploadBudget(bytes)` | `Void(Integer)` method | Set the backend material-texture/cubemap upload byte budget per frame; negative means unlimited, `0` pauses new upload rows |
| `SetTextureStreaming(enabled)` | `Void(Boolean)` method | Enable automatic `TextureAsset3D` mip-residency streaming (default off) |
| `SetTextureStreamingBias(bias)` | `Void(Number)` method | Bias streaming's desired mip; positive drops more detail, clamped to `[-16, 16]` |
| `TextureStreamingDemotions` | `Integer` property | Lifetime count of resident-window demotions applied by streaming |
| `PassCount` | `Integer` property | Number of CPU-timed render passes (currently 4) |
| `PassCpuMs(i)` | `Number(Integer)` method | CPU milliseconds spent in pass `i` since the latest Begin/Begin2D (late overlays accumulate): `0` shadow, `1` main scene, `2` overlay, `3` backend end/present |

`TextureUploadBytes` counts actual texture cache uploads and re-uploads performed by the active
Metal, OpenGL, or D3D11 backend. Pixels-backed 2D material textures and cubemaps
are advanced in row slices under `SetTextureUploadBudget`; native compressed
`TextureAsset3D` blocks are submitted by resident mip under the same budget and
telemetry. Cache hits report no new upload bytes, software/unsupported backends
report `0`, and the value is reset at the next non-overlay frame begin. D3D11
rejects out-of-range row slices, malformed native block descriptors, and uploads
whose byte counts cannot fit D3D11 `UINT` fields before it touches GPU resources.
Its post-FX and readback paths also reject malformed effect-chain storage and
unsupported staging formats before replaying GPU passes or mapping CPU-readable
textures.

For a positive finite upload budget, small RGBA8 2D textures up to 256 KiB may
finish after large scene uploads consume the nominal allowance. This keeps
newly rasterized text and HUD images from disappearing for a frame during
world-texture streaming. A budget of `0` is still a strict pause; negative
budgets remain unlimited. Texture-required screen commands are skipped while
their image is not resident instead of drawing a temporary white rectangle.
`TextureUploadPendingBytes` reports remaining queued texture/cubemap row bytes
plus native compressed mip bytes and returns to `0` after final submissions
drain. The open-world native-compressed hitch CTest records the selected
backend's raw RGBA bytes, compressed resident/upload bytes, RAM/VRAM reduction
percentages, and final-frame tolerance after the budgeted native mip upload
drains. Use these members with
`Game3D.Assets3D.SetUploadBudget` and streaming counters to find frames where decoded asset commits are
followed by GPU texture upload pressure.

`PassCpuMs` complements `FrameGpuTimeUs` on the CPU side: the frame loop stamps
a monotonic clock at the shadow → main → overlay → backend-end boundaries, so
`PassCpuMs(1)` isolates scene submission cost from present/blit time in
`PassCpuMs(3)`. Values reset at **Begin** or **Begin2D** alongside draw counters, then accumulate late
overlay work without replacing scene timing. They read `0` before
the first `End`. Indices are stable; `PassCount` future-proofs iteration.

`FrameGpuTimeUs` is backend-owned timing telemetry. The D3D11 backend records it
with `D3D11_QUERY_TIMESTAMP` plus a disjoint query and reports the latest
completed non-disjoint sample; unsupported backends or frames without a ready
sample report `0`. The open-world perf and GPU-smoke probes include the value in
their `PERF:` / `GPU_FRAME_TIME:` lines so Windows reference runs can record CPU
frame-loop metrics and D3D11 GPU timestamp evidence together.

The CPU submission counters reset at public frame begin. `DrawsSubmitted`
counts backend mesh or instanced submissions, `AabbTransforms` exposes the
world-bounds work avoided by Canvas3D's per-frame mesh+matrix AABB cache,
`SortPasses` counts deferred stable sort stages, and `BackendStateChanges`
counts material/state runs after Canvas3D ordering when the backend exposes
stateful submission costs; the software backend reports `0`.

### Canvas3D Visibility Controls

The current visibility controls are a coarse CPU path: frustum rejection for
bounded draws, SceneGraph BVH candidate selection before Canvas3D draw sorting, and
a low-resolution screen-space coverage/depth grid for conservative occlusion
skips. SceneGraph also has an authored portal/PVS accelerator for interiors: add
visibility-zone AABBs and portal links, and `Draw` skips drawables inside
interior zones that are not reachable from the camera's current zone. These
paths are not GPU occlusion queries or Hi-Z culling.

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetFrustumCulling(enabled)` | `Void(Boolean)` | Toggle frustum rejection (default on) |
| `SetOcclusionCulling(enabled)` | `Void(Boolean)` | Toggle conservative CPU occlusion skips (independent of frustum culling) |
| `SetVSync(enabled)` | `Void(Boolean)` | Present pacing: vsync defaults on; GPU backends use their native display-sync clock, software presentation uses the window frame limiter, and disabling removes the active limiter for lowest latency |
| `VSync` | `Boolean` | Requested vsync state |
| `TrySetRenderScale(scale)` | `Boolean(Double)` | Render the window-backed 3D scene at `scale` x output size (`[0.25, 1]`) and upscale before full-size overlays, readback, and presentation; reduced scales require `BackendSupports("render-scale")`. `>= 1` requests native rendering even on fixed-scale backends. A capable backend can return `false` during an active frame/present transaction or if target allocation fails; the previous scale remains active. Explicit render targets retain their authored size. |
| `RenderScale` | `Double` | Currently requested render scale (`1` = native) |
| `DrawCount` | `Integer` | Main 3D draw submissions queued by the latest ended frame |
| `OccludedDrawCount` | `Integer` | Latest scene draw submissions skipped by visibility culling |
| `InstancedFallbackCount` | `Integer` | Instances routed through the per-draw fallback (blended/rebased batches) this frame; opaque batches use the backend instanced hook on every backend — including software — and report `0` |
| `InstancedFallbackDroppedCount` | `Integer` | Instances skipped when the bounded per-draw fallback would exceed 65,536 instances in one call |
| `OcclusionCandidateCount` | `Integer` | Opaque draw candidates tested by the CPU occlusion grid in the latest frame |
| `TextureUploadBytes` | `Integer` | Backend texture upload bytes in the latest ended frame |
| `TextureUploadPendingBytes` | `Integer` | Backend material texture and cubemap bytes still pending upload |
| `FrameGpuTimeUs` | `Integer` | Latest completed backend GPU frame time in microseconds, or `0` when unsupported |

Frustum rejection defaults on (the per-draw test is cheap; bounds are cached
per queued draw) and the two toggles are independent — disabling one leaves
the other alone. With occlusion enabled, opaque draws are tested front-to-back
against the coverage grid first and only the survivors are regrouped by
backend state, so state batching never weakens the occlusion pass. Occluders
register their actual triangles: a software Hi-Z rasterizer projects each
eligible opaque draw's geometry into a fine per-texel view-depth buffer
(perspective-correct, budget-capped per frame) and folds fully-written blocks
into the coarse grid, so near walls, rotated geometry, and other deep-AABB
occluders contribute their true silhouettes — the conservative AABB-rectangle
write survives only as the fallback for draws past the triangle budget.
Transparent draws are never used as occluders and are not rejected by the
coarse coverage grid. The lens-flare occlusion probes share the same scene depth: the software
backend answers from its z-buffer, and the GPU backends read a handful of
depth texels back asynchronously (one frame of latency, never a pipeline
stall), with per-flare temporal smoothing hiding both the latency and the
probe quantization.
`BackendSupports("hlod")` reports support for runtime-authored LOD/impostor
proxies. `BackendSupports("gpu-skinning")` reports whether skinned draws route
their bone palettes to the vertex shader (true on the Metal/OpenGL/D3D11
production backends; false on software, which stays the bit-exact CPU-skinned
reference, and false while `Canvas3D.SetForceCpuSkinning(true)` is active so
capability reports always match actual routing). GPU-skinned draws skip the
per-frame CPU vertex transform and re-upload entirely: static vertex buffers
carry bone indices/weights and only the `bone_count x 16` float palette (plus
the previous-frame palette when motion vectors are active) uploads per draw.
Telemetry: `Canvas3D.GpuSkinnedDrawCount` (lifetime GPU-routed skinned draws)
and `Canvas3D.SkinningUploadBytes` (lifetime palette bytes). Skinned draws
with extra bone influences, per-submesh bone remaps on the instanced path, or
CPU-applied morph streams stay on the CPU path — morph-then-skin ordering is
preserved by construction because morphs are applied before the routing
decision. `BackendSupports("occlusion")` reports the CPU occlusion baseline; GPU
query/Hi-Z/portal acceleration can advertise the same capability once added.
The unit lane includes a dense covered-draw fixture that queues 65 opaque draws,
submits only the front occluder, and reports 64 occlusion skips. The SceneGraph
indexed-occlusion fixture indexes 130 drawables, narrows the CPU occlusion grid
to 2 spatial candidates before Canvas3D sorting, and submits only the front draw.
The open-world slice adds a named authored dense city/forest fixture in
`visibility_dense_probe.zia`; the local macOS software Release baseline records
169 authored drawables reduced to 49 submitted draws, 120 PVS skips, 50.407%
fill-proxy reduction, and a final-frame pixel match against the no-PVS render.

SceneGraph interior PVS is authored on the scene:

| Method / Property | Signature | Description |
|-------------------|-----------|-------------|
| `AddVisibilityZone(name, min, max)` | `Integer(String, Vec3, Vec3)` | Add a world-space interior visibility zone AABB and return its index |
| `AddVisibilityPortal(from, to, bidirectional)` | `Integer(Integer, Integer, Boolean)` | Add a directed or bidirectional visibility link between zones |
| `VisibilityZoneCount` | `Integer` | Authored zone count |
| `VisibilityPortalCount` | `Integer` | Directed portal-link count |
| `PvsCulledCount` | `Integer` | Drawables skipped by the latest portal/PVS pass |
| `PortalClipping` | `Boolean` | Portal-frustum clipping (default `true`); `false` restores the legacy any-portal reachability flood-fill |
| `PortalTraversalCount` | `Integer` | Portal expansions evaluated by the latest PVS build |

By default the PVS uses portal-frustum clipping: each portal propagates
visibility only through its projected screen window (derived from the interval
between the two linked zone AABBs — their shared face when touching, the gap
slab when separated), narrowed by the window it was reached through. Zones
whose every portal chain falls outside the view — behind the camera, or
narrowed away by intervening portals — are culled even when the zone itself
overlaps the frustum. The traversal runs to a fixpoint so a wider path found
later re-expands a zone's window, and degenerate/near-plane-straddling portals
degrade to the conservative full view rather than over-culling.

Nodes that intersect no visibility zone remain visible, which keeps outdoor or
mixed scenes from disappearing when a PVS graph is only authored for interiors.
The count properties and PVS traversal clamp zone/portal counters to the live
allocation bounds, and appending zones or portals repairs malformed counters
before writing the next entry.

### Zanna.Graphics3D.SceneGraph

Hierarchical scene graph with an implicit root node and lazily recomputed world
transforms.

| Method / Property | Signature | Description |
|-------------------|-----------|-------------|
| `Root` | `SceneNode` | Implicit scene root |
| `NodeCount` | `Integer` | Total nodes including root |
| `VisibleNodeCount` | `Integer` | Drawable mesh nodes submitted by the most recent `Draw` |
| `AnimationCount` | `Integer` | Baked animation clips the scene carries (imported rigs, VSCN bakes) |
| `Add(node)` / `Remove(node)` | `Void(Object)` | Attach or detach root-level nodes |
| `TryAdd(node)` | `Boolean(Object)` | Add a node and report validation/allocation failure |
| `Find(name)` | `SceneNode(String)` | Search the scene by node name |
| `QueryAABB(min, max)` | `Seq(SceneNode)(Vec3, Vec3)` | Return visible mesh nodes whose world AABB intersects the box |
| `QuerySphere(center, radius)` | `Seq(SceneNode)(Vec3, Double)` | Return visible mesh nodes whose world AABB intersects the sphere |
| `RaycastNodes(origin, direction, maxDistance)` | `SceneNode(Vec3, Vec3, Double)` | Return the closest visible mesh node hit by the ray |
| `RaycastNodesPrecise(origin, direction, maxDistance)` | `SceneNode(Vec3, Vec3, Double)` | Triangle-accurate closest-hit raycast (ADR 0193) |
| `RaycastNodesPreciseAll(origin, direction, maxDistance)` | `Seq(SceneNode)(Vec3, Vec3, Double)` | Triangle-accurate all-hits raycast, nearest first |
| `RaycastPreciseHit(origin, direction, maxDistance)` | `RayHit3D(Vec3, Vec3, Double)` | Triangle-accurate raycast returning the hit point/normal/distance |
| `CulledCount` | `Integer` | Nodes rejected by culling in the most recent `Draw` |
| `Load(path)` / `Save(path)` | — | Whole-scene `.vscn`/`.scene3d` round trip |
| `LoadResult(path)` | `Result(String)` | `Zanna.Result` peer of `Load`: ok wraps the scene, err carries the loader diagnostic (ADR 0227) |
| `SaveToText()` / `LoadTextResult(virtualPath, text)` | — | Canonical text serialization and its Result-carrying inverse; `virtualPath` anchors relative prefab references (ADR 0190/0227) |
| `AdoptAnimations(source)` | `Integer(Object)` | Retain-append every baked animation clip carried by `source`, skipping clips already carried; returns the number adopted (ADR 0227) |
| `GetAnimation(index)` | `Object(Integer)` | Baked rigid-node clip as `NodeAnimation3D`, in carrier order; `null` when out of range or when the entry is a skeletal `Animation3D` clip |
| `GetAnimationName(index)` | `String(Integer)` | Name of the baked clip at `index`, either clip class (`""` when out of range) |
| `GetAnimationDuration(index)` | `Double(Integer)` | Duration in seconds of the baked clip at `index`, either clip class (0 when out of range) |

The clip carrier holds both clip classes: rigid-node clips
(`NodeAnimation3D`, playable through `NodeAnimator3D`) and skeletal clips
(`Animation3D`, driven by `AnimController3D`). `AnimationCount` bounds the
index; names and durations enumerate both, while `GetAnimation` returns only
the rigid class so its type is honest.

Node-animation clip and target identifiers must be strict UTF-8, contain no
embedded NUL byte, and fit the runtime identifier limit. Malformed imported
clip names read as empty and malformed channel targets are ignored. Non-looping
`NodeAnimator3D` playback stops as soon as it reaches either endpoint,
including when a frame lands exactly on time zero or the clip duration.

| `UnresolvedPrefabCount` | `Integer` | Prefab references that loaded as empty placeholders; gate startup on zero to catch broken references (ADR 0227) |
| `SetNodeTransforms(nodes, values)` | `Void(Object, Object)` | Batch-apply packed TRS values (10 floats per node: `px,py,pz,qx,qy,qz,qw,sx,sy,sz`) to a list of nodes in one runtime call |
| `Draw(canvas, camera)` | `Void(Object, Object)` | Draw visible node meshes |
| `SyncBindings(dt)` | `Void(Double)` | Push physics, animation, and binding transforms |
| `RebaseOrigin(dx, dy, dz)` | `Void(Double, Double, Double)` | Shift every root-level subtree by `-delta` while leaving the root unchanged |

`Find()` returns `null` when no node matches the name. `SceneNode.Find(name)` provides the same
absence-aware search for a subtree.

The query methods are backed by the SceneGraph BVH spatial index, with the
deterministic flat walk kept as the internal parity fallback. Transform-only
dirties, geometry revisions, and visibility changes refit the existing BVH;
hierarchy, mesh assignment, LOD, and impostor changes rebuild it lazily. Cached
storage, entry bounds, and parent/child topology are validated at query and
refit boundaries. Invalid cache state is discarded and rebuilt or sent through
the exact flat fallback, and all parent-chain and BVH walks have explicit work
budgets. Results skip hidden subtrees and only return nodes with their own mesh
bounds. Draw culling uses the same indexed candidate set, then runs the exact
selected-LOD/impostor frustum test before submitting. Raw morph spans too large
for the bounded culling scan, and invalid skeletal-palette translations, use the
conservative scene-distance ceiling rather than an undersized deformation
bound. The index stores transformed world AABBs in double precision, so
far-origin queries and raycasts keep nodes distinct even when their separation
is below single-precision world-space granularity. Mesh
vertices and backend upload remain float data. Game3D floating-origin frames
opt into a Canvas3D camera-relative upload path for double-precision `DrawMesh`
model matrices, camera frame position/view translation, and point/spot light
positions. Programmatic `Mesh3D.AddVertex` keeps an internal double-position
sidecar so identity-matrix raw world-space meshes can subtract the active camera
origin before vertex upload; generated billboard paths such as standalone
`Particles3D`, `Sprite3D`, and decal meshes likewise upload camera-relative
vertices. Caller-provided float instancing data remains limited by the precision
of the data supplied to Canvas3D. Runtime tests
keep a generated 10k drawable-node grid in the normal SceneGraph ctest lane to
guard BVH shape, transform refit, isolated-query reduction, and frame-cull
candidate reduction.

Repeated scene, navigation, animation, overlap, and ray queries retain bounded
scratch storage instead of allocating a new work buffer for every call.
Collect-all queries publish only after the complete ordered result has been
built; if a budget or allocation cannot be satisfied they return the API's
empty/failure result rather than a plausible-looking partial hit list.

`RebaseOrigin` is the low-level floating-origin primitive used by Game3D. It
keeps child-local transforms stable by moving only root-level subtrees in world
space; physics bodies, cameras, and audio listeners should be rebased through
their owning world systems as well. Query results returned before a rebase are
snapshots; run spatial queries again after the between-frame rebase boundary.

`Mesh3D.Resident`, read-only `ResidentBytes`, and read-only `RetainedBytes`
expose mesh-payload residency for streaming systems. Nonresident meshes keep
their handles and authored data, report zero resident bytes, continue to report
their retained CPU payload through `RetainedBytes`, and are skipped by
Canvas3D/SceneGraph draw paths.
SceneGraph `.vscn` save/load persists each mesh's resident flag, so authored
streaming state survives scene round trips while older files default to
resident meshes.

`Mesh3D.Simplify(mesh, targetTriangles)` returns a new deterministic QEM mesh.
The result exposes `SimplifyRequestedTriangles`,
`SimplifyAchievedTriangles`, and `SimplifyStatus` (`0` not run/stale, `1`
complete, `2` valid partial). A partial result means topology or a classified
open/attribute/material boundary prevented the exact target; it is still safe
to render. Subset placement keeps fixed vertex attributes, double positions,
both skin-influence streams, remapped morph channels, material ranges, and
retained animation references aligned. Collapses that violate the manifold
link condition or create duplicate, degenerate, or inverted faces are skipped.
Changing the returned mesh's geometry clears its simplification diagnostics.

`Mesh3D.BendArc(radius, arcDegrees)` bends a straight static mesh around a
vertical circular arc in place. It maps the authored X extent across the arc,
keeps Y unchanged, and rotates normals and tangents with the local bend frame
without changing triangle winding. Use it for curved walls, seating, rails,
and trim assembled from straight kit pieces. Radius must be finite and
positive, the arc must be in `(0, 360]`, and every vertex must remain inside
the bend radius; validation happens before mutation. Skinned and morph-target
meshes are deliberately refused.

`Mesh3D.CompactStreams` (default false) opts a mesh into the compact GPU
vertex-stream encoding: static-geometry-cache uploads on the Metal, Direct3D 11,
and OpenGL backends pack each vertex into 48 bytes (snorm16 normals/tangents,
half-float UVs, unorm8 colors and bone weights) instead of the full 92-byte
record — roughly halving static vertex VRAM and fetch bandwidth on high-poly
imports. Fixed-function vertex-attribute conversion decodes the packed fields,
so shaders are unchanged, and the CPU payload, software rendering, CPU
skinning, morphing, and physics all keep full precision. Transient (uncached)
draws, morphing meshes, and CPU-skinned draws always use the full layout.
Toggling the flag bumps the geometry revision so backend caches re-upload in
the newly selected encoding. `SceneAsset.LoadWithOptionsEx(path,
"compactStreams")` enables it for every imported mesh in one call.

`Mesh3D.ReleaseCpuScratch()` frees a mesh's rebuildable CPU side buffers — the
double-precision position sidecar used for floating-origin rebasing and the
normal-recalculation accumulator — and returns the bytes released. The
authored vertex/index payload, GPU caches, raycasts, and physics are
untouched. Call it on static meshes after load in scenes that do not use
floating-origin rebasing; a later `RebaseOrigin` on such a mesh falls back to
float precision, so streamed far-origin worlds should leave the sidecar in
place.

### Zanna.Graphics3D.SceneNode

`SceneNode` supports authored mesh LODs through `AddLod(distance, mesh)`.
Entries remain sorted by distance, duplicate distances replace the previous
mesh, and `ClearLod()` restores the base mesh at every distance. LOD selection
skips nonresident meshes and falls back to the next resident choice.

| Method / Property | Signature | Description |
|-------------------|-----------|-------------|
| `AddLod(distance, mesh)` | `Void(Double, Object)` | Use `mesh` once camera distance reaches `distance` |
| `GenerateLods(levels, ratio)` | `Void(Integer, Double)` | Synthesize up to 4 simplified LOD meshes from the base mesh via `Mesh3D.Simplify` (level *k* targets `ratio^k` of the base triangle count), register them with distance thresholds derived from the mesh radius, and enable auto LOD |
| `SetAutoLod(enabled, screenErrorPx)` | `Void(Boolean, Double)` | Select authored LODs by projected screen size instead of distance thresholds |
| `SetImpostor(distance, pixels)` | `Void(Double, Object)` | Generate an unlit textured quad proxy used at or beyond `distance`; pass `null` pixels to clear |
| `SetImpostorFrames(distance, pixels, frames)` | `Void(Double, Object, Integer)` | Multi-frame impostor: `pixels` is a horizontal strip of `frames` views captured at yaw `i*2pi/frames`; the draw path picks the frame nearest the camera bearing |
| `GetImpostorFrameIndex()` | `Integer()` | Last impostor frame index the draw path selected (0 for single-frame impostors) |
| `ClearLod()` | `Void()` | Remove authored LOD entries |
| `LodCount` | `Integer` | Number of registered LOD entries |
| `GetLodMesh(index)` | `Object(Integer)` | Borrow the mesh for an LOD entry |
| `GetLodDistance(index)` | `Double(Integer)` | Get the sorted distance threshold |
| `SetLodResident(index, resident)` | `Void(Integer, Boolean)` | Mark an LOD mesh payload resident/nonresident |
| `GetLodResident(index)` | `Boolean(Integer)` | Return whether an LOD mesh payload is resident |
| `GetLodResidentBytes(index)` | `Integer(Integer)` | Return resident bytes for an LOD mesh payload |
| `Light` | `Object` | Attached `Light3D`, or `null`; read/write and retained by the node |
| `Camera` | `Object` | Attached `Camera3D`, or `null`; read/write and cloned with the node hierarchy |

`SetAutoLod` selects among meshes registered with `AddLod`; `GenerateLods`
synthesizes that chain automatically from the base mesh using quadric-error
simplification, so downloaded models get a real LOD chain in one call. A lower `screenErrorPx` keeps the base mesh longer,
while a higher value switches to lower-detail meshes sooner. `SetImpostor`
retains the supplied `Pixels` object and builds a regular textured `Mesh3D`
proxy, so it works on the same draw path as other meshes.

`SceneNode.Light` is the typed component used by VSCN persistence and
`SceneGraph.Draw`. Assigning a valid `Light3D` retains it, assigning `null`
clears it, and a wrong-class handle is rejected without mutation. A
node-attached light's position and direction are light-local and are transformed
through the node hierarchy during draw; direct `Canvas3D.SetLight` slots use
world-space values.

### Zanna.Graphics3D.Camera3D

3D perspective or orthographic camera with smooth follow, orbit, and screen-to-ray projection.

**Type:** Instance (obj)
**Constructors:** `Camera3D.New(fov, aspect, near, far)`,
`Camera3D.WithHorizontalFov(fov, aspect, near, far)`,
`Camera3D.NewOrtho(size, aspect, near, far)`

#### Properties

| Property   | Type   | Access     | Description |
|------------|--------|------------|-------------|
| `Fov`      | Double | Read/Write | Vertical field of view in degrees |
| `Position` | Object | Read/Write | Camera position as `Vec3` |
| `Forward`  | Object | Read       | Unit forward vector as `Vec3` |
| `Right`    | Object | Read       | Unit right vector as `Vec3` |
| `Up`       | Object | Read       | Unit up vector as `Vec3` (second row of the view basis) |
| `IsOrtho`  | Boolean | Read      | True when camera is in orthographic projection mode |
| `OrthoSize` | Double | Read/Write | Orthographic half-height in world units |
| `NearPlane` / `FarPlane` | Double | Read/Write | Authored clip planes |
| `EffectiveNearPlane` / `EffectiveFarPlane` | Double | Read | Sanitized clip planes actually used for projection (near below `0.1` snaps to `0.1`) |
| `AspectRatio` | Double | Read | Aspect ratio used for projection |
| `ViewMatrix` | Object (`Mat4`) | Read | Cached view matrix exactly as submitted to the renderer |
| `ProjectionMatrix` | Object (`Mat4`) | Read | Cached render projection (reversed-Z exactly as submitted on GPU backends) |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `LookAt(pos, target, up)` | `Void(Object, Object, Object)` | Orient camera from a position toward a target using an up vector |
| `SetHorizontalFov(fov)` | `Void(Double)` | Convert a horizontal FOV in degrees to the camera's stored vertical FOV |
| `Orbit(pivot, yaw, pitch, distance)` | `Void(Object, Double, Double, Double)` | Position and orient camera on a sphere around `pivot` |
| `ScreenToRay(sx, sy, screenW, screenH)` | `Object(Integer, Integer, Integer, Integer)` | Return a normalized `Vec3` direction from a screen pixel |
| `ScreenToRayOrigin(sx, sy, screenW, screenH)` | `Object(Integer, Integer, Integer, Integer)` | World-space ray origin for the same pixel; pair with `ScreenToRay` (and `Input3D.MousePosition()`) for picking |
| `FirstPersonInit()` | `Void()` | Initialize FPS-camera state (yaw and pitch reset to zero) |
| `FirstPersonUpdate(yawDelta, pitchDelta, moveFwd, moveRight, moveUp, speed, dt)` | `Void(Double ×7)` | Apply mouse-look deltas in degrees plus signed camera-relative movement axes at `speed` world units per second |
| `Shake(amplitude, frequency, duration)` | `Void(Double, Double, Double)` | Start a procedural camera shake |
| `SmoothFollow(target, speed, minDist, maxDist, height)` | `Void(Object, Double, Double, Double, Double)` | Lerp toward a target with distance clamping |
| `SmoothLookAt(target, speed, roll)` | `Void(Object, Double, Double)` | Slerp the camera orientation toward a target |

Camera positions and FPS-style movement inputs are clamped to the runtime's safe world range before
view/projection matrices are generated. Non-finite position components fall back to `0.0`.
For game cameras, prefer `WithHorizontalFov` or `SetHorizontalFov` when the authored
value is a horizontal FOV; passing that value to `New` treats it as vertical FOV
and can produce visibly stretched edges on wide windows.
`SmoothFollow` and `SmoothLookAt` keep FPS-style yaw/pitch state synchronized with the resulting view.
For per-frame camera, listener, light, and particle update paths, reuse `Vec3`
instances with `Set(x, y, z)`, `SetX/Y/Z`, `CopyFrom(other)`, or writable
`X`/`Y`/`Z` properties before passing them to runtime setters. The runtime
setters copy component values; the pure `Vec3` arithmetic methods still return
new vectors.

```zia
bind Zanna.Graphics3D.Camera3D as Camera3D;
bind Zanna.Math.Vec3 as Vec3;

var cam = Camera3D.New(60.0, 16.0 / 9.0, 0.1, 1000.0);
cam.LookAt(Vec3.New(0.0, 2.0, -5.0), Vec3.New(0.0, 0.0, 0.0), Vec3.New(0.0, 1.0, 0.0));
cam.Shake(0.3, 15.0, 0.5);
```

```basic
DIM cam AS Zanna.Graphics3D.Camera3D
cam = NEW Zanna.Graphics3D.Camera3D(60.0, 16.0 / 9.0, 0.1, 1000.0)
DIM eye AS Zanna.Math.Vec3 = NEW Zanna.Math.Vec3(0.0, 2.0, -5.0)
DIM tgt AS Zanna.Math.Vec3 = NEW Zanna.Math.Vec3(0.0, 0.0, 0.0)
DIM up  AS Zanna.Math.Vec3 = NEW Zanna.Math.Vec3(0.0, 1.0, 0.0)
cam.LookAt(eye, tgt, up)
```

---

### Zanna.Graphics3D.RenderTarget3D

Offscreen color buffer for 3D scenes, optionally in HDR format.

**Type:** Instance (obj)
**Constructor:** `RenderTarget3D.New(width, height)`

#### Properties

| Property | Type    | Access | Description |
|----------|---------|--------|-------------|
| `IsHdr`  | Boolean | Read   | True for floating-point HDR buffers |
| `Width`  | Integer | Read   | Buffer width in pixels |
| `Height` | Integer | Read   | Buffer height in pixels |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `NewHdr(width, height)` | `Object(Integer, Integer)` | Create an HDR floating-point render target |
| `AsPixels()` | `Object()` | Download the color buffer into a new `Pixels` object (allocates per call) |
| `CopyTo(pixels)` | `Void(Object)` | Allocation-free readback into an existing same-size `Pixels`; traps `"RenderTarget3D.CopyTo: size mismatch"` on dimension disagreement |

GPU-backed render targets synchronize their CPU `Pixels` mirror lazily when `AsPixels()`,
`CopyTo()`, or a screenshot requests it. Resetting a render target or destroying its owning
`Canvas3D` detaches the backend sync callback, so later CPU readback cannot call into a stale
GPU context. For per-frame monitor/scope loops prefer `CopyTo` (reuses the caller's buffer) or,
better, bind the target directly as a material texture via
`Material3D.SetAlbedoRenderTarget` — no readback code at all.

---

### Zanna.Graphics3D.CubeMap3D

Cubemap texture resource for environment mapping and skyboxes. Use `Canvas3D.LoadCubeMap` to load six-face cubemaps from disk. Skybox sampling uses the camera forward vector for orthographic cameras and falls back to the engine's `-Z` camera direction if that vector is degenerate; GPU smoke coverage also exercises a degenerate-basis normal-map draw.

**Type:** Instance (obj)
**Constructor:** `CubeMap3D.New(size)`
**Static:** `CubeMap3D.LoadHdrPanorama(path, exposure)` — loads a Radiance `.hdr`
equirectangular panorama (flat, old-RLE, and new-RLE scanlines), projects it onto
the six cube faces through the engine's cubemap basis, and range-compresses with
Reinhard `e*x / (1 + e*x)` at the given exposure (values <= 0 default to 1) into
the 8-bit faces the IBL pipeline consumes. Resolves plain file paths first, then
the asset manager (embedded/mounted packs). Both LF and CRLF headers are
accepted, as are the signed and axis-swapped Radiance scanline orientations;
pixels are normalized into the runtime's canonical image orientation. Invalid
dimensions, truncated RLE, and malformed scanline declarations fail the load.

---

### Zanna.Graphics3D.Material3D

Surface material used by mesh, model, instanced, terrain, water, decal, and
sprite draws.

**Type:** Instance (obj)
**Constructor:** `Material3D.New()`

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `FromColor(r, g, b)` | `Object(Double, Double, Double)` | Create a diffuse-color material |
| `Textured(texture)` | `Object(Object)` | Create a material with a base `Pixels` or `TextureAsset3D` texture |
| `PBR(r, g, b)` | `Object(Double, Double, Double)` | Create a PBR material |
| `SetColor(r, g, b)` | `Void(Double, Double, Double)` | Set diffuse/base color |
| `SetTexture(texture)` / `SetAlbedoMap(texture)` | `Void(Object)` | Bind or clear the base-color texture slot |
| `SetNormalMap(texture)` | `Void(Object)` | Bind or clear a tangent-space normal map |
| `SetSpecularMap(texture)` | `Void(Object)` | Bind or clear a legacy specular map |
| `SetEmissiveMap(texture)` | `Void(Object)` | Bind or clear an emissive texture |
| `SetMetallicRoughnessMap(texture)` | `Void(Object)` | Bind or clear the packed PBR metallic-roughness map |
| `SetAmbientOcclusionMap(texture)` | `Void(Object)` | Bind or clear the ambient-occlusion map |
| `SetEnvMap(cubemap)` | `Void(Object)` | Bind or clear an environment cubemap |
| `SetAlbedoRenderTarget(rt)` | `Void(Object)` | Bind a `RenderTarget3D`'s live contents as the albedo texture |
| `ClearAlbedoRenderTarget()` | `Void()` | Detach a render-target albedo binding |
| `SetEmissiveRenderTarget(rt)` | `Void(Object)` | Bind a `RenderTarget3D`'s live contents as the emissive map (glowing monitors) |
| `GetCustomParam(index)` | `Double(Integer)` | Read a shading-model custom parameter set by `SetCustomParam` (zero out of range) |

Texture map methods accept `Pixels` or `TextureAsset3D` handles with either an
active RGBA8 fallback or retained native mip blocks. KTX2 BC1, BC3, BC4/BC5,
BC7, supported ETC2 RGBA8/EAC, and ASTC LDR void-extent texture assets expose CPU fallbacks
alongside retained native mip block payloads and mip residency byte telemetry;
unsupported compressed blocks use an 8x8 magenta/black checker fallback and
record a load warning naming the format. Native-only assets draw on GPU backends
that advertise the matching compression capability and otherwise behave as
unbound textures until a fallback-capable mip is resident.
When a `TextureAsset3D` is bound, the material retains the asset and resolves
the currently resident RGBA8 mip and native block source for each draw.

#### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Color` | Object | Read | Diffuse/base color as `Vec3` |
| `TexturePixels` | Pixels | Read | Current decoded base-color/albedo map, or null |
| `NormalMapPixels` | Pixels | Read | Current decoded normal map, or null |
| `SpecularMapPixels` | Pixels | Read | Current decoded legacy specular map, or null |
| `EmissiveMapPixels` | Pixels | Read | Current decoded emissive map, or null |
| `MetallicRoughnessMapPixels` | Pixels | Read | Current decoded packed PBR map, or null |
| `AmbientOcclusionMapPixels` | Pixels | Read | Current decoded ambient-occlusion map, or null |
| `LightmapPixels` | Pixels | Read | Current decoded baked lightmap, or null |
| `Alpha` | Double | Read/Write | Material opacity |
| `Metallic` | Double | Read/Write | PBR metallic factor |
| `Roughness` | Double | Read/Write | PBR roughness factor |
| `AmbientOcclusion` | Double | Read/Write | Ambient-occlusion factor |
| `EmissiveIntensity` | Double | Read/Write | Emissive multiplier |
| `NormalScale` | Double | Read/Write | Normal-map strength |
| `Anisotropy` | Integer | Read/Write | Texture anisotropy; `1=off`, clamps to `1..16` |
| `AlphaMode` | Integer | Read/Write | `0=opaque`, `1=mask`, `2=blend` |
| `DoubleSided` | Boolean | Read/Write | Render both triangle sides |
| `Unlit` | Boolean | Read | True when unlit shading is enabled |
| `ShadingModel` | Integer | Read | Current shading model |
| `HasTexture` | Boolean | Read | Base-color/albedo texture slot is bound |
| `HasNormalMap` | Boolean | Read | Normal-map slot is bound |
| `HasSpecularMap` | Boolean | Read | Specular-map slot is bound |
| `HasEmissiveMap` | Boolean | Read | Emissive-map slot is bound |
| `HasMetallicRoughnessMap` | Boolean | Read | PBR metallic-roughness map slot is bound |
| `HasAmbientOcclusionMap` | Boolean | Read | Ambient-occlusion map slot is bound |
| `HasEnvMap` | Boolean | Read | Environment cubemap slot is bound |
| `Reflectivity` | Double | Read/Write | Environment reflection strength |
| `Texture` / `NormalMap` / `SpecularMap` / `EmissiveMap` / `MetallicRoughnessMap` / `AmbientOcclusionMap` / `Lightmap` | Object | Read | Borrowed bound source handle for each map slot (`Pixels`, `TextureAsset3D`, or `RenderTarget3D`), or `null` when unbound. Unlike the decoded `*Pixels` readbacks, these return the exact object that was bound (ADR 0233). `SetAlbedoMap`/`SetAlbedoRenderTarget` alias the `Texture` slot; `SetEmissiveRenderTarget` aliases `EmissiveMap` |
| `EnvMap` | Object (`CubeMap3D`) | Read | Borrowed bound environment cubemap, or `null` |
| `EmissiveColor` | Object (`Vec3`) | Read | Fresh snapshot of the emissive color multiplier |
| `Shininess` | Double | Read | Retained legacy specular exponent |
| `DepthBias` / `DepthSlopeBias` | Double | Read | Retained depth-bias terms as clamped by `SetDepthBias` |

Texture setters accept `Pixels` handles, except `SetEnvMap`, which accepts a
`CubeMap3D`. Passing `NULL` clears the slot and immediately updates the matching
`Has*` property.

The read-only `*Pixels` properties expose the currently drawable decoded view
for inspection and authoring previews. Direct `Pixels` sources are returned
unchanged; `TextureAsset3D` and `RenderTarget3D` sources resolve to their current
material-facing pixels. Reading them does not replace the retained source,
discard KTX2/source-container provenance, change residency, or alter VSCN
serialization. A property can be null while a native-only or currently
non-resident texture source remains assigned.

Render-target bindings are live: the material samples the target's most recent
*completed* frame, refreshing automatically whenever a `Canvas3D.End` into the
target finishes — render the monitor's content each frame and the bound
material picks it up with no `AsPixels`/`SetTexture` churn and no per-frame
allocation. While a frame into the target is still open, draws sampling it see
the previous completed frame, which makes self-referential setups (a monitor
visible inside its own feed) safe by construction.

---

### Zanna.Graphics3D.Light3D

Directional, point, ambient, spot, rectangle-area, sphere-area, or volume scene light.

**Type:** Instance (obj)

#### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `Directional(direction, r, g, b)` | `Object(Object, Double, Double, Double)` | Infinite directional emitter |
| `Point(position, r, g, b, attenuation)` | `Object(Object, Double, Double, Double, Double)` | Point emitter with distance falloff |
| `Ambient(r, g, b)` | `Object(Double, Double, Double)` | Uniform ambient contribution |
| `Spot(position, direction, r, g, b, attenuation, inner, outer)` | `Object(Object, Object, Double, Double, Double, Double, Double, Double)` | Cone emitter; angles are degrees |
| `AreaRectangle(position, direction, width, height, r, g, b, attenuation, range)` | `Object(Object, Object, Double, Double, Double, Double, Double, Double, Double)` | Oriented one-sided rectangle emitter |
| `AreaSphere(position, radius, r, g, b, range)` | `Object(Object, Double, Double, Double, Double, Double)` | Omnidirectional sphere emitter |
| `Volume(position, radius, r, g, b, range)` | `Object(Object, Double, Double, Double, Double, Double)` | Isotropic volume emitter |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetIntensity(value)` | `Void(Double)` | Set light intensity multiplier |
| `SetColor(r, g, b)` | `Void(Double, Double, Double)` | Set light color using normalized RGB components |
| `SetAttenuation(value)` | `Void(Double)` | Set point/spot distance attenuation |
| `SetSpotCone(innerDegrees, outerDegrees)` | `Void(Double, Double)` | Sanitize and replace both spot-cone angles as one mutation |

#### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Type` | Integer | Read | `0=directional`, `1=point`, `2=ambient`, `3=spot`, `4=rectangle`, `5=sphere`, `6=volume` |
| `Color` | Object | Read | RGB `Vec3` |
| `Intensity` | Double | Read | Brightness multiplier |
| `IsEnabled` | Boolean | Read/Write | Disabled lights are skipped by rendering |
| `CastsShadows` | Boolean | Read/Write | Directional lights default to true. Point and spot lights may opt in; rectangle/sphere shadows use a center-point approximation; volume lights never claim a slot. |
| `Direction` | Object | Read/Write | Direction or rectangle normal as a normalized `Vec3` |
| `Position` | Object | Read/Write | Position `Vec3` for finite lights |
| `Attenuation` | Double | Read | Current point/spot attenuation |
| `Width` | Double | Read/Write | Rectangle width |
| `Height` | Double | Read/Write | Rectangle height |
| `Radius` | Double | Read/Write | Sphere/volume radius |
| `DecayType` | Integer | Read/Write | FBX-compatible `0=none`, `1=linear`, `2=quadratic`, `3=cubic` falloff |
| `Range` | Double | Read/Write | Finite-light range; zero selects constructor/importer default behavior |
| `InnerConeDegrees` | Double | Read | Sanitized inner angle for spot lights; zero for other types |
| `OuterConeDegrees` | Double | Read | Sanitized outer angle for spot lights; zero for other types |

All backends consume the same native area/volume parameters. Constructors and
property writes sanitize non-finite values, clamp colors and non-negative
dimensions, normalize directions, and keep spot cones finite, ordered, and at
least `0.01` degrees apart. `SetSpotCone` validates the pair together so a
caller never observes an intermediate cone.

---

### Zanna.Graphics3D.PostFX3D

Post-processing effect chain applied to a rendered scene.

**Type:** Instance (obj)
**Constructor:** `PostFX3D.New()`

#### Properties

| Property      | Type    | Access     | Description |
|---------------|---------|------------|-------------|
| `IsEnabled`   | Boolean | Read/Write | Enable or disable the entire chain |
| `EffectCount` | Integer | Read       | Number of effects currently in the chain |
| `LastError`   | String  | Read       | Last recoverable configuration error (`""` when none). Depth-aware effects now run on every backend, so bind-time refusals no longer occur for them |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddBloom(threshold, intensity, passes)` | `Void(Double, Double, Integer)` | Add bloom. `passes` selects the blur-chain depth (1–6 octaves; deeper = wider halo) |
| `AddTonemap(mode, exposure)` | `Void(Integer, Double)` | Add tone mapping (`0 = off/linear`, `1 = Reinhard`, `2 = ACES`) |
| `AddFxaa()` | `Void()` | Add FXAA anti-aliasing |
| `AddColorGrade(brightnessOffset, contrast, saturation)` | `Void(Double, Double, Double)` | Add color grading. Brightness is an additive offset centered on `0.0`; contrast and saturation are multipliers centered on `1.0` |
| `AddVignette(strength, radius)` | `Void(Double, Double)` | Add a vignette darkening effect |
| `AddSsao(radius, intensity, samples)` | `Void(Double, Double, Integer)` | Add screen-space ambient occlusion (radius in world units, 4–16 samples) |
| `AddDof(focusDist, focalRange, blurRadius)` | `Void(Double, Double, Double)` | Add depth of field |
| `AddMotionBlur(strength, samples)` | `Void(Double, Integer)` | Add motion blur |
| `AddTaa(blend)` | `Void(Double)` | Add temporal anti-aliasing. `blend` is the history weight (0.5–0.98; typical 0.9) |
| `AddSsr(intensity, maxRoughness)` | `Void(Double, Double)` | Add screen-space reflections; the software reference uses a bounded coarse depth march |
| `AddAutoExposure(minEv, maxEv, adaptSpeed)` | `Void(Double, Double, Double)` | Add temporally smoothed eye adaptation within the ordered exposure range |
| `AddColorLut(pixels, blend)` | `Void(Object, Double)` | Set the chain's retained singleton 256×16 LUT strip and blend it with the current color; a later call replaces the earlier LUT in place |
| `AddSunShafts(intensity, decay, samples)` | `Void(Double, Double, Integer)` | Add bounded screen-space radial light shafts |
| `GetEffectKind(index)` | `Integer(Integer)` | Kind of the effect at `index` in application order — one of the `PostFXEffectKind` constants; `-1` when out of range |
| `RemoveEffectAt(index)` | `Boolean(Integer)` | Remove one effect, closing the chain over the gap; `false` when out of range |
| `Clear()` | `Void()` | Remove all effects, release a retained LUT, and reset temporal adaptation/history while preserving reusable allocations |

Chain entries enumerate in application order: `EffectCount` bounds the index,
`GetEffectKind(i)` identifies each entry against the static
`Zanna.Graphics3D.PostFXEffectKind` constants (`Bloom`, `Tonemap`, `Fxaa`,
`ColorGrade`, `Vignette`, `Ssao`, `Dof`, `MotionBlur`, `Taa`, `Ssr`,
`AutoExposure`, `ColorLut`, `SunShafts`), and `RemoveEffectAt(i)` deletes a
single entry without rebuilding the rest of the chain — the editor-style
alternative to `Clear()` plus re-adding.

Bloom runs as a progressive downsample/upsample mip chain: the scene is thresholded
with a Karis-averaged 13-tap filter (suppressing single-pixel fireflies), blurred
across up to six octaves, and composited back at `intensity`. SSAO is normal-aware and
range-checked — occlusion comes from a hemispheric spiral kernel around the
reconstructed surface normal, so flat walls no longer darken and silhouettes no longer
halo. TAA jitters the projection sub-pixel each frame (Halton 2,3) and reprojects a
persistent history buffer through per-pixel motion vectors with neighborhood clamping
on GPU backends, and supersedes FXAA in the CINEMATIC quality profile when available.

**One chain runs on every backend.** The depth-aware effects (SSAO, DOF, motion blur,
TAA, SSR) have CPU implementations on the software path, so `Canvas3D.SetPostFX` no
longer refuses chains that carry them — `BackendSupports("postfx-full")` is true
everywhere and games stop gating chain *construction* by backend. The software
versions render the same phenomena at documented reduced quality: SSAO uses 8 fixed
Poisson depth taps with a 3×3 blur; DOF is a 12-tap gather scaled by the circle of
confusion; motion blur is camera-reprojection only (no per-object velocity — fast
movers diverge from the GPU look); SSR is a coarse screen-space march with no
environment fallback on miss; TAA blends a reprojected history with a 3×3
neighborhood clamp but without sub-pixel jitter. All are deterministic (fixed tap
tables, no clock). Per-backend GPU capability keys (`"taa"`, `"ssao"`, …) still
report only hardware acceleration.

Chains are bounded at 4,096 authored entries. Software effects operate on one packed
`RGBRGB...` float buffer while preserving each target pixel's alpha. Reusable frame,
scratch, and TAA-history allocations stay with the chain; `Clear()` resets temporal
history and auto exposure so a rebuilt chain cannot blend against stale topology.

GPU window backends render the scene into a linear-HDR (RGBA16F) target, so bloom and
tone mapping operate on unclamped color (`BackendSupports("hdr-scene")`). On HDR
targets an explicit `AddTonemap(0, exposure)` applies exposure plus gamma-out (sRGB
encoding) so "tonemap off" still produces display-referred output matching modes 1/2;
on LDR targets mode 0 remains a passthrough.

```zia
bind Zanna.Graphics3D.PostFX3D as PostFX3D;

var fx = PostFX3D.New();
fx.AddBloom(0.8, 1.2, 4);
fx.AddTonemap(2, 1.0);
fx.AddTaa(0.9);
fx.Enabled = true;
```

---

### Zanna.Graphics3D.RayHit3D

Mesh raycast result returned by `Canvas3D.Raycast`. Read-only value type.

**Type:** Static (none)

#### Properties

| Property        | Type    | Access | Description |
|-----------------|---------|--------|-------------|
| `Distance`      | Double  | Read   | Euclidean distance along the ray to the hit point |
| `Point`         | Object  | Read   | World-space hit point as `Vec3` |
| `Normal`        | Object  | Read   | Surface normal at the hit point as `Vec3` |
| `TriangleIndex` | Integer | Read   | Index of the hit triangle in the mesh |

Ray queries normalize non-zero directions internally. Zero-length or non-finite directions miss, and distances remain world-unit distances even when the input direction was not normalized.

---

## Asset Loading

### Zanna.Graphics3D.SceneAsset

High-level reusable model container for `.vscn`, `.fbx`, `.gltf`, `.glb`, `.obj`, and `.stl` assets. OBJ imports preserve safe relative `mtllib`/`usemtl` material groups as separate template nodes; STL imports synthesize one default-material mesh node.

For offline conversion, `zanna asset bake <input> <output.scene3d>` reloads the
written VSCN and compares its enumerable resources with the source
`SceneAsset`. Human mode warns with stable `*-count-reduced` codes when a
round-trip drops resources. `--json` emits the machine-readable
`zanna.asset-bake-report/v1` object, including source and baked counts, losses,
per-slot `textureFidelity`, and the source import diagnostics. Texture entries
distinguish exact `preserved-source`, canonical `preserved-decoded`, and
intentional `changed-after-import` fallback while checking dimensions, mips,
native format, decoded texels, and shared identity. This makes format-fidelity
decisions visible in asset pipelines instead of treating a successfully written
file as proof of a lossless conversion.

#### Properties

| Property | Type | Description |
|---|---|---|
| `MeshCount` | Integer | Number of imported meshes |
| `MaterialCount` | Integer | Number of imported materials |
| `SkeletonCount` | Integer | Number of imported skeletons |
| `AnimationCount` | Integer | Number of imported skeletal animation clips |
| `NodeAnimationCount` | Integer | Number of imported node animation clips |
| `MorphTargetCount` | Integer | Number of mesh slots with an attached `MorphTarget3D` |
| `MorphShapeCount` | Integer | Total number of static morph shapes across all meshes |
| `NodeCount` | Integer | Number of imported nodes |
| `SceneCount` | Integer | Number of immutable scenes addressable by indexed APIs |
| `VariantCount` | Integer | Number of imported `KHR_materials_variants` names |

#### Load and Persistence Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `LoadResult(path)` | `Result[SceneAsset](String)` | Load from the filesystem as `Ok(SceneAsset)` or `Err(message)` |
| `LoadAssetResult(path)` | `Result[SceneAsset](String)` | Load through `Zanna.IO.Assets` as `Ok(SceneAsset)` or `Err(message)` |
| `LoadAnimationResult(path, index)` | `Result[Animation3D](String, Integer)` | Load an imported skeletal animation clip as `Ok(Animation3D)` or `Err(message)` |
| `LoadAnimationAssetResult(path, index)` | `Result[Animation3D](String, Integer)` | Load a skeletal animation clip through `Zanna.IO.Assets` |
| `LoadNodeAnimationResult(path, index)` | `Result[NodeAnimation3D](String, Integer)` | Load an imported node animation clip as `Ok(NodeAnimation3D)` or `Err(message)` |
| `LoadNodeAnimationAssetResult(path, index)` | `Result[NodeAnimation3D](String, Integer)` | Load a node animation clip through `Zanna.IO.Assets` |
| `Save(path)` | `Integer(String)` | Atomically save the complete asset using the lowest VSCN document version that carries its content; returns `1` on success or `0` on validation/allocation/I/O failure |

`LoadAsset` and every `*AssetResult` root load accept ordinary logical asset
paths or `asset://` paths. All six `SceneAsset` formats resolve from embedded
assets, mounted ZPAK files, or the filesystem. OBJ `mtllib` files and their
supported image maps resolve relative to the original logical OBJ/MTL paths,
including when the parsers stage root bytes in temporary files. Absolute,
URI-like, and parent-traversing OBJ dependency references remain rejected.

`SceneAsset.Save` differs from `SceneGraph.Save`: the asset operation preserves
all immutable scenes and their camera membership, the complete indexed
mesh/material/skeleton/animation inventories (including resources not reachable
from scene zero), node/object/morph animation samples, material-variant names
and node mappings, and static morph position/normal/tangent deltas and weights.
Camera attachments, camera scalar animation paths, and complete directional,
punctual, area, and volume light state also round-trip. Stable source-node
indexes preserve animation targeting when node names are duplicated; all twelve
material custom parameters and per-slot anisotropy also round-trip, including
imported glTF anisotropy factors.

New VSCN output stores vertex, index, bone-map, extra-influence, and skeletal
keyframe records as explicitly tagged, padding-free little-endian data inside
Base64. The loader remains compatible with VSCN document versions 1–7 and the
older v1/v2 binary layouts, while an unknown explicit layout tag or malformed
present rig stream rejects the complete load. It also rejects malformed
lengths, non-finite values, invalid table indexes, unsupported source-container
kinds, mismatched image magic, or failed image decoding before publishing an
asset. Texture entries are discriminated `rgba8` snapshots or `source`
containers. Exact KTX2/PNG/JPEG/GIF/BMP bytes, native KTX2 format/mips, decoded
texels, shared texture identity, sampler state, UV transforms, the lightmap
slot, and cubemap faces survive when available. A decoded texture changed after
import deliberately saves its current RGBA8 form instead of stale source bytes.

#### Scene and Camera Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetSceneName(index)` | `String(Integer)` | Name for a scene index, or `""` when out of range |
| `GetCameraCount(sceneIndex)` | `Integer(Integer)` | Number of imported cameras for a scene |
| `GetCamera(sceneIndex, index)` | `Object(Integer, Integer)` | Imported `Camera3D`, or `null` when absent/out of range |
| `InstantiateSceneAt(index)` | `Object(Integer)` | Clone a scene by index as a fresh `SceneGraph` |
| `GetMorphTarget(meshIndex)` | `MorphTarget3D(Integer)` | Borrow the morph container attached to the corresponding `GetMesh` slot, or `null` |

glTF cameras are imported as `Camera3D` handles attached to their authored
scene nodes with the node's world transform applied. The immutable template's
scene camera table and node slot share one identity; each instantiated scene
clones the camera so projection and view edits remain instance-local. Cached
`SceneAsset` assets remain immutable: index `0` is the active/default scene,
secondary glTF scene roots follow it, and invalid scene indices return zero/null
rather than changing shared loader state. FBX imports preserve authored model
hierarchy where available and split polygon material assignments into
instantiable material-specific mesh nodes.

#### Lookup and Instancing Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `FindNode(name)` | `Option[SceneNode](String)` | Find a template node as `Some(node)`, or `None` |
| `Instantiate()` | `SceneNode()` | Clone the template hierarchy into a fresh node subtree |
| `InstantiateScene()` | `SceneGraph()` | Clone the default scene as a standalone scene graph |
| `InstantiateSceneAt(index)` | `SceneGraph(Integer)` | Clone an immutable scene by index |
| `GetVariantName(index)` | `String(Integer)` | Variant display name, or `""` when out of range |
| `ApplyVariant(target, index)` | `Integer(Object, Integer)` | Apply a material variant to every mapped node under `target` (a `SceneNode` from `Instantiate()` or a `SceneGraph`); returns the node count updated. Variants a primitive does not map restore its default material, so switching is reversible |
| `GenerateLods(levels, ratio)` | `Integer(Integer, Float)` | Generate 1..4 LOD levels (~`ratio^k` triangles, QEM decimation) for every template/scene mesh node and enable auto screen-error selection; each unique mesh is decimated once, nodes that already carry chains are skipped, and later `Instantiate()` clones inherit the chains. Returns the node count chained |
| `FlattenStatic(rootName, transform)` | `Seq[SceneNode](String, Mat4)` | Merge the named static template subtree (`""` = whole model) into one fresh node per material, applying an optional placement transform |

Mutating an instantiated node does not mutate the immutable template node
returned by `FindNode()`.

`FlattenStatic` walks visible static meshes in authored order, reuses their
shared materials, and names each result after its first contributing source
node so role-based naming survives the merge. Pass `null` for an identity
placement. Skinned, morph-target, and singularly transformed pieces are skipped
with `AssetDiagnostics3D` warnings; an unknown root returns an empty sequence,
and the imported template is never changed.

---

## Baked Lighting

`LightBaker3D.New(scene)` is the offline lightmap baker: flag static geometry
with `SceneNode.SetStatic(true)`, register bake lights with `AddLight`, tune
`TexelsPerUnit`/`Samples`/`Bounces`/`SetSkyColor`, then loop `BakeStep()`
until it returns `true` (chunked; `Progress` reads [0,1]). The bake is
deterministic — identical scenes and options produce identical atlases.
Bake inputs freeze when the first scene gather begins. Chart capacity, source
node bindings/transforms, bake-relevant material fields, mesh revisions, and
all allocations are validated before publication; a
terminal failure returns from `BakeStep()` with `Atlas` null and leaves chart
UVs unpublished. `Apply()` atomically and idempotently installs the atlas on
each baked node via material instances and
`Material3D.SetLightmap` (TEXCOORD_1 charts are written into the meshes);
`Atlas` exposes the page. The software renderer replaces the flat-ambient
term with `lightmap x albedo` for lightmapped draws (GPU backends keep flat
ambient until the follow-up shader batch). Bake at authoring time — never
per frame.

`LightProbeGrid3D.New(min, max, spacing)` + `Bake(baker)` bakes an SH-9
irradiance probe grid with the same tracer so dynamic objects agree with the
lightmaps; bounds must be finite and component-wise ordered, and exact-multiple
extents use `ceil(extent / spacing) + 1` probes per axis. Probes inside geometry
are detected and in-filled from the nearest valid region. `Sample(position,
normal)` renormalizes interpolation over valid probes and returns black for an
unbaked grid or invalid position (a missing/zero normal uses positive Y).
`Save`/`Load` round-trip strict, fixed-width IEEE little-endian `.vlpg` files;
malformed, non-finite, noncanonical, truncated, or overlong input leaves the
existing grid unchanged.

## Skeletal Animation

### Zanna.Graphics3D.Skeleton3D

Bone hierarchy for skeletal mesh deformation. Typically loaded alongside a model via `SceneAsset`.

**Type:** Instance (obj)
**Constructor:** `Skeleton3D.New()`

#### Properties

| Property    | Type    | Access | Description |
|-------------|---------|--------|-------------|
| `BoneCount` | Integer | Read   | Number of bones in the skeleton |
| `AliasCount`| Integer | Read   | Number of registered external bone-name aliases |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddBone(name, parentIndex, localTransform)` | `Integer(String, Integer, Object)` | Add a named bone with a local transform and parent index |
| `ComputeInverseBind()` | `Void()` | Pre-compute inverse bind pose matrices after all bones are added |
| `FindBoneOption(name)` | `Option[Integer](String)` | Return `Some(index)` for a matching bone, or `None` |
| `GetBoneName(index)` | `String(Integer)` | Return the name of bone at `index` |
| `SetBoneAlias(externalName, localName)` | `Void(String, String)` | Map an external rig's bone name (e.g. `"mixamorig:Hips"`) onto a local bone; an empty `localName` removes the alias |

Aliases registered with `SetBoneAlias` are consulted by `Animation3D.Retarget`
(in both directions: a destination alias matching the source bone name, or a
source alias resolving to a destination bone name); `FindBoneOption`
stay exact-name so typos still surface as `-1`/`None`. Re-registering an existing
external name replaces its mapping. Use this to retarget clips from downloaded
rigs whose naming convention does not match your skeleton without renaming bones.

Bone names are retained as complete runtime strings, not fixed 64-byte buffers.
Long names with equal prefixes remain distinct through lookup, retargeting,
FBX import, and VSCN save/load. Animation key times are stored as doubles so
adjacent FBX ticks remain distinct; only sub-tick arithmetic noise is replaced
as a duplicate sample.

Skinning weights are normalized consistently across CPU and GPU draw paths. Missing palettes copy
vertices through unchanged, and unused backend bone-palette slots are treated as identity transforms.
Add every bone before binding the skeleton to a mesh or constructing animation players, blenders, or
controllers; those runtime objects freeze the skeleton topology because their pose buffers are sized
from the current bone count.

---

### Zanna.Graphics3D.Animation3D

Single keyframe animation track referencing a `Skeleton3D`.

**Type:** Instance (obj)
**Constructor:** `Animation3D.New(name, durationSeconds)`

#### Properties

| Property  | Type    | Access     | Description |
|-----------|---------|------------|-------------|
| `Name`    | String  | Read       | Animation name |
| `Duration`| Double  | Read       | Total duration in seconds |
| `Looping` | Boolean | Read/Write | Whether the animation loops |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddKeyframe(boneIndex, time, translation, rotation, scale)` | `Void(Integer, Double, Object, Object, Object)` | Add a keyframe for the given bone at `time` seconds; `null` TRS parts fall back to bind pose |
| `Retarget(srcSkeleton, dstSkeleton)` | `Object(Object, Object)` | Copy channels onto matching destination bones |

`Retarget` resolves each source bone in order: explicit destination-side
`Skeleton3D.SetBoneAlias` mapping for the source bone's name, exact name match,
source-side alias reversal (a source alias naming this bone is tried against the
destination's bones and aliases), then humanoid role aliases, then matching bone
index as a fallback. It preserves clip name, duration, looping, and keyframe values, scales keyed
translations by source/destination bind-length ratio where both bones expose a comparable segment,
and leaves destination-only bones in bind pose.

---

### Zanna.Graphics3D.AnimPlayer3D

Plays a single `Animation3D` track on a `Skeleton3D` with optional crossfade.

**Type:** Instance (obj)
**Constructor:** `AnimPlayer3D.New(skeleton)`

#### Properties

| Property    | Type    | Access     | Description |
|-------------|---------|------------|-------------|
| `Speed`     | Double  | Read/Write | Playback speed multiplier |
| `IsPlaying` | Boolean | Read       | True when an animation is playing |
| `Time`      | Double  | Read/Write | Current playback time in seconds |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Play(animation)` | `Void(Object)` | Start playing an `Animation3D` |
| `Crossfade(animation, duration)` | `Void(Object, Double)` | Blend into a new animation over `duration` seconds |
| `Stop()` | `Void()` | Stop the current animation and output the bind pose |
| `Update(deltaSeconds)` | `Void(Double)` | Advance the animation by the given delta |
| `GetBoneMatrix(boneIndex)` | `Object(Integer)` | Return the current global/world matrix for `boneIndex` |

Crossfades blend all bones, including channels that exist in only one clip, against bind pose. The
fading-out clip keeps its own speed and looping behavior during the transition.

```zia
bind Zanna.Graphics3D.AnimPlayer3D as AnimPlayer3D;
bind Zanna.Graphics3D.Animation3D as Animation3D;

var player = AnimPlayer3D.New(skeleton);
player.Play(walkAnim);
player.Speed = 1.5;

// per frame
player.Update(deltaSeconds);
```

```basic
DIM player AS Zanna.Graphics3D.AnimPlayer3D = NEW Zanna.Graphics3D.AnimPlayer3D(skeleton)
player.Play(walkAnim)
player.Update(deltaSeconds)
```

---

### Zanna.Graphics3D.AnimBlend3D

Blends multiple named animation states by weight. Useful for blend trees (run/walk, aim offsets).

**Type:** Instance (obj)
**Constructor:** `AnimBlend3D.New(skeleton)`

#### Properties

| Property     | Type    | Access | Description |
|--------------|---------|--------|-------------|
| `StateCount` | Integer | Read   | Number of animation states in the blend |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddState(name, animation)` | `Integer(String, Object)` | Add a named `Animation3D` state; returns its index |
| `SetWeight(index, weight)` | `Void(Integer, Double)` | Set normalized blend weight for state at `index` |
| `SetWeightByName(name, weight)` | `Void(String, Double)` | Set blend weight by state name |
| `GetWeight(index)` | `Double(Integer)` | Get the current weight for state at `index` |
| `SetSpeed(index, speed)` | `Void(Integer, Double)` | Set playback speed for state at `index` |
| `Update(deltaSeconds)` | `Void(Double)` | Advance all states and produce the blended pose |

`Update(0.0)` recomputes the pose without advancing time. New states inherit their animation's
looping flag unless overridden by speed/time control in code.

---

### Zanna.Graphics3D.BlendTree3D

Parametric 1D/2D blendspaces over `AnimBlend3D`. `Canvas3D.DrawMeshBlended` accepts a
`BlendTree3D` anywhere it accepts an `AnimBlend3D`.

**Type:** Instance (obj)
**Constructors:** `BlendTree3D.New1D(skeleton)`, `BlendTree3D.New2D(skeleton)`

#### Properties

| Property      | Type    | Access | Description |
|---------------|---------|--------|-------------|
| `SampleCount` | Integer | Read   | Number of animation samples in the tree |
| `BlendMode`   | Integer | Read/Write | 2D weighting mode: `0` freeform-directional (default), `1` legacy inverse-distance |
| `Blend`       | `AnimBlend3D` | Read | Internal blend driven by `Update`; exposes per-sample weights via `GetWeight(index)` |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddSample(animation, x, y)` | `Integer(Object, Double, Double)` | Add an `Animation3D` sample and return its index |
| `SetParam(x, y)` | `Void(Double, Double)` | Set blend parameters and recompute weights |
| `Update(deltaSeconds)` | `Void(Double)` | Recompute weights and advance the underlying blended pose |

`New1D` linearly blends between neighboring `x` samples and clamps outside the sample range.
`New2D` defaults to freeform-directional blending: samples are Delaunay-triangulated in
parameter space and the query point is resolved to barycentric weights over its containing
triangle, so at most three clips are ever active and weights always sum to one. Points
outside the sample hull project onto the nearest hull edge. Trees with fewer than three
non-collinear samples, and trees with `BlendMode = 1`, use the legacy behavior:
exact-coordinate selection when possible, otherwise normalized inverse-distance weights
across all registered samples.

---

### Zanna.Graphics3D.IKSolver3D

Inverse-kinematics solvers for final pose adjustment before skinning.

**Type:** Instance (obj)
**Constructors:** `IKSolver3D.TwoBone(skeleton, root, mid, end)`, `IKSolver3D.LookAt(skeleton, bone)`, `IKSolver3D.FABRIK(skeleton, chain)`

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetTarget(pos)` | `Void(Object)` | Set the target as a `Vec3` |
| `SetWeight(weight)` | `Void(Double)` | Blend solver output from `0.0` to `1.0`; non-finite values become zero |
| `SetPole(pos)` | `Void(Object)` | Set a world-space pole target for `TwoBone` bend-plane control |
| `SetGroundNormal(normal)` | `Void(Object)` | Set a ground normal; the end bone's animated rotation is tilted by the model-up-to-normal delta (flat ground is a no-op) |
| `SetTargetRotation(rotation)` | `Void(Object)` | Set a model-space `Quat` orientation goal for the end bone, applied after the positional solve; wins over `SetGroundNormal` |
| `ClearTargetRotation()` | `Void()` | Remove the end-bone orientation goal |
| `Solve()` | `Void()` | Solve against the skeleton bind pose for standalone inspection |

Attach a solver through `AnimController3D.SetIKSolver(solver)` or the Game3D
wrapper `Animator3D.SetIKSolver(solver)`. Controller-bound IK runs after the
base state/blend tree and overlays are composed, then before skinning palettes
are generated. `TwoBone` and `FABRIK` use a positional chain solve and preserve
the chain root; `SetPole` controls two-bone bend direction, and `LookAt` aims
the selected bone's local +Z axis. End-bone orientation follows ADR 0286:
`SetGroundNormal` tilts the animated end-bone rotation by the shortest arc
from model +Y to the surface normal (a flat surface changes nothing), and
`SetTargetRotation` slerps the end bone toward an explicit model-space
quaternion goal by solver weight, taking precedence over the ground hint.

---

### Zanna.Graphics3D.AnimController3D

Stateful animation controller with named states, triggered transitions, animation events, root motion, and multi-layer blending.

**Type:** Instance (obj)
**Constructor:** `AnimController3D.New(skeleton)`

#### Properties

| Property           | Type    | Access | Description |
|--------------------|---------|--------|-------------|
| `CurrentState`     | String  | Read   | Name of the currently active state |
| `PreviousState`    | String  | Read   | Name of the state before the last transition |
| `IsTransitioning`  | Boolean | Read   | True while a crossfade is in progress |
| `StateCount`       | Integer | Read   | Total number of registered states |
| `RootMotionDelta`  | Object  | Read   | Accumulated root motion `Vec3` since last `ConsumeRootMotion` |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddState(name, animation)` | `Integer(String, Object)` | Register a named animation state |
| `AddTransition(from, to, duration)` | `Boolean(String, String, Double)` | Add a crossfade transition between two states |
| `Play(state)` | `Boolean(String)` | Switch immediately to the named state |
| `Crossfade(state, duration)` | `Boolean(String, Double)` | Blend into the named state over `duration` seconds |
| `Stop()` | `Void()` | Stop all animation and enter idle |
| `Update(deltaSeconds)` | `Void(Double)` | Advance the controller and produce the current pose |
| `IsStatePlaying(state)` | `Boolean(String)` | True when the named state is currently playing (active or being transitioned into) |
| `SetStateSpeed(state, speed)` | `Void(String, Double)` | Override playback speed for a state |
| `SetStateLooping(state, loop)` | `Void(String, Boolean)` | Override loop setting for a state |
| `SetAnimationLod(distance, rateHz)` | `Void(Double, Double)` | Batch animation updates at a lower deterministic rate; non-positive inputs disable throttling |
| `SetBoneLod(maxBones)` | `Void(Integer)` | Freeze bones at or after `maxBones` for deterministic bone-count LOD; non-positive values restore full-pose output |
| `SetBlendTree(tree)` | `Boolean(Object)` | Use a compatible `BlendTree3D` as the base pose source; pass `Nothing` to clear |
| `SetIKSolver(solver)` | `Boolean(Object)` | Apply a compatible `IKSolver3D` after overlays and before skinning; pass `Nothing` to clear |
| `AddEvent(state, time, name)` | `Void(String, Double, String)` | Register a named event to fire at a playback time |
| `PollEvent()` | `String()` | Dequeue the next fired event name, or empty string |
| `SetRootMotionBone(index)` | `Void(Integer)` | Designate a bone to extract root motion from; `-1` disables it |
| `ConsumeRootMotion()` | `Object()` | Read and clear the accumulated root motion `Vec3` |
| `SetLayerWeight(layer, weight)` | `Void(Integer, Double)` | Set the blend weight for an overlay layer |
| `SetLayerMask(layer, boneMask)` | `Void(Integer, Integer)` | Restrict a layer to a bone mask bitmask |
| `PlayLayer(layer, state)` | `Boolean(Integer, String)` | Play a state as a masked replace overlay |
| `PlayLayerAdditive(layer, state)` | `Boolean(Integer, String)` | Play a state as a true additive bind-pose delta overlay |
| `CrossfadeLayer(layer, state, duration)` | `Boolean(Integer, String, Double)` | Blend into a new masked replace state on an overlay layer |
| `CrossfadeLayerAdditive(layer, state, duration)` | `Boolean(Integer, String, Double)` | Blend into a true additive bind-pose delta state on an overlay layer |
| `StopLayer(layer)` | `Void(Integer)` | Stop the overlay layer |
| `GetBoneMatrix(boneIndex)` | `Object(Integer)` | Return the current global/world matrix for `boneIndex` |

Root motion is disabled by default, preserves loop-wrap deltas, and resets its accumulated
translation/rotation when disabled or switched to another bone. `Stop()` returns all layers to bind
pose while keeping state metadata intact. `PlayLayer` keeps the compatibility replace-overlay
behavior; `PlayLayerAdditive` applies `(overlayPose - bindPose) * weight` over the current base pose.
`CrossfadeLayerAdditive` uses the same additive composition while blending overlay states.
`SetAnimationLod(distance, rateHz)` accumulates elapsed time and samples only when the configured
interval elapses, then applies the full accumulated delta so playback remains deterministic.
`SetBoneLod(maxBones)` limits palette updates to the retained bone-count prefix while keeping
ancestors valid for deterministic distant-character LOD; pass `0` to restore full output.
`SetBlendTree(tree)` updates the tree with the controller tick and uses its blended local pose as
the base layer before overlay layers are applied. Root-motion extraction still comes from the
controller's state-player base layer. `SetIKSolver(solver)` requires a solver bound to the same
skeleton and applies it after overlays, before the final skin palette.

```zia
bind Zanna.Graphics3D.AnimController3D as AnimController3D;

var ctrl = AnimController3D.New(skeleton);
ctrl.AddState("idle", idleAnim);
ctrl.AddState("run", runAnim);
ctrl.AddTransition("idle", "run", 0.2);
ctrl.AddTransition("run", "idle", 0.3);
ctrl.AddEvent("run", 0.3, "footstepLeft");
ctrl.Play("idle");
ctrl.SetRootMotionBone(0);

// per frame
ctrl.Update(deltaSeconds);
var delta = ctrl.ConsumeRootMotion();
var evt = ctrl.PollEvent();
if evt == "footstepLeft" {
    Audio.Play(footstepSound);
}
```

---

### Zanna.Graphics3D.MorphTarget3D

Per-vertex morph target system for facial animation or shape blending.

**Type:** Instance (obj)
**Constructor:** `MorphTarget3D.New(mesh)`

#### Properties

| Property     | Type    | Access | Description |
|--------------|---------|--------|-------------|
| `ShapeCount` | Integer | Read   | Number of registered morph shapes |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddShape(name)` | `Integer(String)` | Add a named morph shape; returns its index |
| `SetDelta(shape, vertex, dx, dy, dz)` | `Void(Integer, Integer, Double, Double, Double)` | Set the position delta for a vertex in a shape |
| `SetNormalDelta(shape, vertex, dx, dy, dz)` | `Void(Integer, Integer, Double, Double, Double)` | Set the normal delta for a vertex in a shape |
| `SetWeight(index, weight)` | `Void(Integer, Double)` | Set blend weight `[-1.0–1.0]` for shape at `index` |
| `GetWeight(index)` | `Double(Integer)` | Get the current weight for a shape |
| `SetWeightByName(name, weight)` | `Void(String, Double)` | Set blend weight by shape name |

GPU backends clamp active morph shapes to shader-indexable limits and disable morphing on upload
failure instead of reusing stale buffers; larger shape sets should use the CPU-applied path.

---

## Particles And Effects

### Zanna.Graphics3D.Particles3D

3D particle emitter with configurable spawn, physics, color, and render properties.
Simulation uses 60 Hz fixed steps and a maximum of 60 steps per `Update`. Fractional time is
retained, while excess complete-step time is exposed through timing telemetry instead of silently
clamped.
`Draw` batches the emitter without mutating its live particle order. Additive
particles skip sorting; alpha particles preserve their temporary back-to-front
sort in instance order. Metal, D3D11, and OpenGL draw a retained unit quad from
one compact 64-byte center/right/up/color record per particle, avoiding repeated
expanded billboard vertex/index construction. Software reconstructs the same
quad through its CPU mesh path. Ribbon trails remain CPU-expanded on every
backend and can accompany the hardware billboard batch.

**Type:** Instance (obj)
**Constructor:** `Particles3D.New(maxParticles)`

#### Properties

| Property   | Type    | Access | Description |
|------------|---------|--------|-------------|
| `Count`    | Integer | Read   | Currently live particle count; terminal snapshots are excluded |
| `IsEmitting` | Boolean | Read | True while the emitter is running |
| `Rate` | Double | Read | Retained emission rate (ADR 0233; every emitter parameter below reads back the value its setter retained) |
| `LifetimeMin` / `LifetimeMax` | Double | Read | Retained lifetime range |
| `SpeedMin` / `SpeedMax` | Double | Read | Retained spawn-speed range |
| `SizeStart` / `SizeEnd` | Double | Read | Retained size-over-life range |
| `AlphaStart` / `AlphaEnd` | Double | Read | Retained alpha-over-life range |
| `ColorStart` / `ColorEnd` | Integer | Read | Retained gradient endpoints, packed `0xRRGGBB` |
| `Gravity` / `Position` / `Direction` / `EmitterSize` | Object (`Vec3`) | Read | Fresh snapshots of the retained emitter vectors |
| `Spread` | Double | Read | Retained emission-cone spread |
| `EmitterShape` | Integer | Read | Retained shape id (`0=point`, `1=sphere`, `2=box`) |
| `Stretch` | Double | Read | Retained velocity-stretch factor |
| `TrailLifetime` / `TrailSegments` | Double / Integer | Read | Retained ribbon-trail history seconds and control-point count |
| `Softness` | Double | Read | Retained soft-particle fade distance |
| `Texture` | Object (`Pixels`) | Read | Borrowed particle texture, or `null` |
| `Seed`     | Integer | Read/Write | Deterministic RNG seed for this emitter's spawn stream. Emitters no longer share a process-global sequence, so setting an explicit seed makes an effect bit-identical across runs regardless of construction order |
| `RenderFinalFrame` | Boolean | Read/Write | Keep an expired particle's exact endpoint drawable until the next valid update. Defaults to true without keeping the particle live. |
| `DroppedTime` | Double | Read | Cumulative complete-step time discarded by the one-second-per-call catch-up budget |
| `LastDroppedTime` | Double | Read | Time discarded by the latest update call, or zero when none was discarded |
| `ResidualTime` | Double | Read | Retained fractional fixed-step time, always less than 1/60 second |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `Void(Double, Double, Double)` | Set emitter world position |
| `SetDirection(x, y, z, spread)` | `Void(Double, Double, Double, Double)` | Set emission direction and cone spread in degrees |
| `SetSpeed(min, max)` | `Void(Double, Double)` | Set spawn speed range |
| `SetLifetime(min, max)` | `Void(Double, Double)` | Set particle lifetime range in seconds |
| `SetSize(min, max)` | `Void(Double, Double)` | Set particle size range |
| `SetGravity(x, y, z)` | `Void(Double, Double, Double)` | Apply per-particle gravity vector |
| `SetColor(startColor, endColor)` | `Void(Integer, Integer)` | Set per-particle color gradient (packed `0xRRGGBB`; opacity comes from `SetAlpha`) |
| `SetAlpha(start, end)` | `Void(Double, Double)` | Set per-particle alpha fade |
| `SetRate(particlesPerSecond)` | `Void(Double)` | Set continuous emission rate |
| `SetTexture(pixels)` | `Void(Object)` | Set the particle sprite texture |
| `SetEmitterShape(shape)` | `Void(Integer)` | Set emitter shape (`0 = point`, `1 = sphere`, `2 = box`) |
| `SetEmitterSize(x, y, z)` | `Void(Double, Double, Double)` | Set emitter volume extent for sphere/box shapes |
| `Start()` | `Void()` | Begin continuous emission |
| `Stop()` | `Void()` | Stop continuous emission |
| `Burst(count)` | `Void(Integer)` | Emit `count` particles immediately |
| `Clear()` | `Void()` | Remove all active particles |
| `ResetDroppedTime()` | `Void()` | Clear cumulative/latest dropped-time telemetry without changing residual time |
| `RebaseOrigin(dx, dy, dz)` | `Void(Double, Double, Double)` | Shift the emitter and live particles by `-delta` |
| `Update(deltaSeconds)` | `Void(Double)` | Advance bounded fixed-step simulation and update timing telemetry |
| `Draw(canvas3D, camera)` | `Void(Object, Object)` | Render particles to the scene |

```zia
bind Zanna.Graphics3D.Particles3D as Particles3D;

var emitter = Particles3D.New(256);
emitter.SetPosition(0.0, 1.0, 0.0);
emitter.SetDirection(0.0, 1.0, 0.0, 30.0);
emitter.SetSpeed(2.0, 6.0);
emitter.SetLifetime(0.5, 1.5);
emitter.SetColor(0xFF6622FF, 0xFF220000);
emitter.SetRate(40.0);
emitter.Start();

// per frame
emitter.Update(deltaSeconds);
emitter.Draw(canvas3d, camera);
```

---

### Zanna.Graphics3D.Decal3D

Time-limited projected decal placed in a 3D scene (bullet holes, blood splats, scorch marks).

**Type:** Instance (obj)
**Constructor:** `Decal3D.New(pixels, position, normal)`

#### Properties

| Property  | Type    | Access | Description |
|-----------|---------|--------|-------------|
| `IsExpired` | Boolean | Read | True when the decal lifetime has elapsed |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetLifetime(seconds)` | `Void(Double)` | Override the decal lifetime |
| `SetDepthBias(bias)` | `Void(Double)` | Override the constant depth bias (negative pulls toward the camera; `0` restores the automatic size-scaled bias) |
| `Update(deltaSeconds)` | `Void(Double)` | Advance the lifetime timer |

---

## Spatial Audio

SpatialAudio3D is part of the audio runtime, not the renderer. See
[Audio: Spatial Audio](../audio.md#spatial-audio) for the canonical
`Zanna.Audio.SpatialAudio3D` API and [Audio: Mix Group Effects](../audio.md#mix-group-effects)
for group-level filters, delay, and reverb.

### Zanna.Graphics3D.SoundListener3D

3D audio listener wrapper that tracks a scene node or camera position and feeds
the audio-owned spatial listener state.

**Type:** Instance (obj)
**Constructor:** `SoundListener3D.New()`

#### Properties

| Property   | Type    | Access     | Description |
|------------|---------|------------|-------------|
| `Position` | Object  | Read/Write | Listener world position as `Vec3` |
| `Forward`  | Object  | Read/Write | Listener facing direction as `Vec3` |
| `Up`       | Object  | Read/Write | Listener up direction as `Vec3`; used to derive the spatial right vector |
| `Velocity` | Object  | Read/Write | Listener velocity for Doppler as `Vec3` |
| `IsActive` | Boolean | Read/Write | Activate this listener for spatialization |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `BindNode(sceneNode)` | `Void(Object)` | Automatically track a `SceneNode` position each `SpatialAudio3D.SyncBindings` call |
| `ClearNodeBinding()` | `Void()` | Remove the node binding |
| `BindCamera(camera)` | `Void(Object)` | Automatically track a `Camera3D` position and forward |
| `ClearCameraBinding()` | `Void()` | Remove the camera binding |

---

### Zanna.Graphics3D.SoundSource3D

3D audio source positioned in world space, with range and Doppler support.
Sources are full-volume through `ReferenceDistance`, attenuate linearly until
`MaxDistance`, and compute a Doppler factor from listener/source velocity. The
mixer applies volume, pan, and playback rate: the Doppler factor multiplies the
source's user `Pitch` and is applied as the voice's resampling rate, so
fly-bys audibly bend pitch. `Occlusion` drives a smoothed lowpass sweep plus
attenuation for through-cover muffling (the game supplies the amount, e.g.
from its own line-of-sight raycasts).

**Type:** Instance (obj)
**Constructor:** `SoundSource3D.New(sound)`

#### Properties

| Property      | Type    | Access     | Description |
|---------------|---------|------------|-------------|
| `Position`    | Object  | Read/Write | Source world position as `Vec3` |
| `Velocity`    | Object  | Read/Write | Source velocity for Doppler as `Vec3` |
| `DopplerFactor` | Double | Read       | Latest computed Doppler pitch multiplier |
| `Pitch`       | Double  | Read/Write | User playback-rate multiplier; composes with Doppler (combined rate clamps to 0.25–4.0) |
| `Occlusion`   | Double  | Read/Write | 0 (open) … 1 (fully occluded): smoothed lowpass sweep + up to −6 dB |
| `ReferenceDistance` | Double | Read/Write | Full-volume radius before linear falloff begins |
| `MaxDistance` | Double  | Read/Write | Attenuation roll-off distance |
| `Volume`      | Integer | Read/Write | Base volume `0–100` |
| `Looping`     | Boolean | Read/Write | True to loop the audio |
| `IsPlaying`   | Boolean | Read       | True when the source is playing |
| `VoiceId`     | Integer | Read       | Runtime voice ID of the active playback |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Play()` | `Integer()` | Start playback; returns voice ID |
| `Stop()` | `Void()` | Stop playback |
| `BindNode(sceneNode)` | `Void(Object)` | Auto-track a `SceneNode` each `SpatialAudio3D.SyncBindings` call |
| `ClearNodeBinding()` | `Void()` | Remove node binding |

```zia
bind Zanna.Graphics3D.SoundSource3D as SoundSource3D;
bind Zanna.Graphics3D.SoundListener3D as SoundListener3D;
bind Zanna.Audio.SpatialAudio3D as SpatialAudio3D;
bind Zanna.Audio.Sound as Sound;

var listener = SoundListener3D.New();
listener.IsActive = true;
listener.BindCamera(cam);

var explosion = Sound.LoadAsset("assets/explosion.ogg");
var src = SoundSource3D.New(explosion);
src.MaxDistance = 40.0;
src.Position = Vec3.New(10.0, 0.0, 0.0);
src.Play();

// per frame
SpatialAudio3D.SyncBindings(deltaSeconds);
```

---

## Navigation

### Zanna.Graphics3D.NavMesh3D

Walkable navigation mesh built from scene geometry. Used with `NavAgent3D` for pathfinding.
`Build` rejects non-manifold shared edges, where more than two triangles own the
same undirected edge, because adjacency/pathfinding would otherwise be
ambiguous.

**Type:** Static (none)
**Constructors:** `NavMesh3D.Build(mesh, agentRadius, agentHeight)`, `NavMesh3D.Bake(scene, agentRadius, agentHeight, maxSlope, cellSize)`, `NavMesh3D.BakeTiled(scene, tileSize, agentRadius, agentHeight, maxSlope, cellSize)`, `NavMesh3D.Import(path)`

#### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `TriangleCount` | Integer | Read | Number of walkable triangles in the mesh |
| `OffMeshLinkCount` | Integer | Read | Number of authored traversal links |
| `ObstacleCount` | Integer | Read | Number of authored coarse AABB obstacles |
| `LastPathCost` | Float | Read | Weighted cost of the latest successful path query |
| `TileSize` | Float | Read | World-space tile edge length of a tiled bake, or `0.0` for `Build`/`Import` meshes |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `FindPathOption(start, end)` | `Option[Path3D](Object, Object)` | Return `Some(path)` when a route exists, or `None` |
| `SamplePosition(pos)` | `Object(Object)` | Snap `pos` to the nearest walkable position |
| `IsWalkable(pos)` | `Boolean(Object)` | True when `pos` is on the walkable surface |
| `Export(path)` | `Boolean(String)` | Serialize the baked navmesh to a `VNAVMSH2` binary file; returns false on write failure |
| `Import(path)` | `Object(String)` | Reconstruct a path-queryable navmesh from a `VNAVMSH2` or legacy `VNAVMSH1` file, or `Nothing` on a missing/corrupt file |
| `AddOffMeshLink(from, to, bidirectional)` | `Boolean(Object, Object, Boolean)` | Add a directed or bidirectional link between walkable points |
| `SetOffMeshLinkMetadata(index, kind, cost, state)` | `Boolean(Integer, String, Double, Integer)` | Attach kind/cost/state metadata to a traversal link |
| `GetOffMeshLinkKind(index)` | `String(Integer)` | Return a traversal link kind string |
| `GetOffMeshLinkTraversalCost(index)` | `Double(Integer)` | Return a traversal link cost multiplier |
| `GetOffMeshLinkState(index)` | `Integer(Integer)` | Return traversal link state flags |
| `AddObstacle(min, max)` | `Boolean(Object, Object)` | Add a coarse AABB obstacle and re-carve affected walkable triangles |
| `RemoveObstacle(index)` | `Boolean(Integer)` | Remove a coarse obstacle and re-carve affected walkable triangles |
| `UpdateObstacle(index, min, max)` | `Boolean(Integer, Object, Object)` | Move/resize a coarse obstacle and re-carve affected walkable triangles |
| `SetArea(min, max, area, cost)` | `Boolean(Object, Object, String, Double)` | Assign area/cost metadata to polygons in a volume |
| `GetArea(pos)` | `String(Object)` | Return the area name at a walkable position |
| `GetTraversalCost(pos)` | `Double(Object)` | Return the traversal-cost multiplier at a walkable position |
| `RebuildTile(tileX, tileZ)` | `Boolean(Integer, Integer)` | Rebuild one retained tiled-bake voxel source tile |
| `SetMaxSlope(degrees)` | `Void(Double)` | Override the maximum walkable slope angle |
| `SetHeuristicMode(mode)` | `Void(Integer)` | Pathfinding heuristic policy: `0` strict/optimal (default; any off-mesh link switches the search to Dijkstra), `1` always-Euclidean (much faster on large link-bearing meshes, paths may be slightly suboptimal around link shortcuts) |
| `HeuristicMode` | `Integer` (property) | Current heuristic policy (0 or 1) |
| `DebugDraw(canvas3D)` | `Void(Object)` | Draw the navmesh wireframe for debugging |

`Bake` flattens every `Mesh3D` attached under a `SceneGraph` through each node's
world transform and runs the voxel baker. Every `Bake` is tiled: it derives a
tile size from the scene extent (roughly a 16×16 tile grid, clamped to at least
4 world units per tile) and retains voxel-cell source data for each tile, so
`RebuildTile` is always available on baked meshes. `BakeTiled` does the same
with an explicit tile size. `RebuildTile(tileX, tileZ)` refreshes only that
tile's geometry, heights, and blocked state from the retained source without a
whole-scene voxel pass; `TileSize` reports the tile edge length in world units.

`AddOffMeshLink` is for authored traversal such as jumps, ladders, and
drop-downs. Both endpoints must be on current walkable polygons. The pathfinder
uses the link as an extra graph edge and includes the link endpoints in the
returned waypoint list. `SetOffMeshLinkMetadata` records link kind, cost
multiplier, and state flags; link cost contributes to A* and `LastPathCost`.
Shared-edge portals narrower than `agentRadius * 2` are not linked when the mesh
is built, so wider agents do not path through narrow authored passages. `SetArea`
tags polygons whose exact XZ footprint intersects a finite volume, and
`GetArea`/`GetTraversalCost` query that metadata. Polygon traversal costs weight
A* edges. `AddObstacle` stores a finite world-space AABB and removes polygons
whose triangle footprint intersects the obstacle volume. On tiled bakes, obstacle
adds/removes/updates re-carve only overlapped tiles; non-tiled meshes still
refilter the preserved source mesh. This remains polygon-level AABB carving
rather than clipped sub-polygons; `NavAgent3D` covers local crowd avoidance.
`Export` writes explicit little-endian vertices, source/current triangles,
blocked flags, traversal costs, area labels, authored off-mesh links, coarse
obstacles, and agent parameters. Import rebuilds adjacency and point-location
grids. Legacy `VNAVMSH1` imports are geometry/cost/blocked-state only. Tiled
voxel heightfield source arrays remain runtime-derived and are not serialized;
an imported tiled navmesh is queryable and editable from serialized triangles,
but it cannot regenerate new voxel heights from an external tile source.

`SamplePosition` uses the retained XZ query grid for off-mesh points, expanding
cell rings until a geometric lower bound proves the nearest projection. A fixed
query-work budget falls back to an exact triangle pass. Path queries lease one
of four independently mutable A* workspaces per navmesh, so read-only searches
can overlap without sharing costs, parents, closed flags, or heap entries.

---

### Zanna.Graphics3D.NavAgent3D

Pathfinding agent that moves along a `NavMesh3D` toward a target.

**Type:** Instance (obj)
**Constructor:** `NavAgent3D.New(navMesh, radius, height)`

#### Properties

| Property            | Type    | Access     | Description |
|---------------------|---------|------------|-------------|
| `Position`          | Object  | Read       | Current world position as `Vec3` |
| `Velocity`          | Object  | Read       | Current velocity as `Vec3` |
| `DesiredVelocity`   | Object  | Read       | Steering velocity before clamping |
| `HasPath`           | Boolean | Read       | True when a valid path is set |
| `RemainingDistance` | Double  | Read       | Distance remaining to the goal |
| `StoppingDistance`  | Double  | Read/Write | Distance from goal at which to stop |
| `DesiredSpeed`      | Double  | Read/Write | Maximum movement speed |
| `AutoRepath`        | Boolean | Read/Write | Automatically repath when blocked |
| `AvoidanceEnabled`  | Boolean | Read/Write | Enable same-NavMesh RVO-style steering against other enabled agents |
| `AvoidanceRadius`   | Double  | Read/Write | Local RVO radius; defaults to the agent radius and clamps to `>= 0` |
| `OnOffMeshLink`     | Boolean | Read       | True while the agent's current path segment traverses an authored off-mesh link |
| `LinkKind`          | String  | Read       | Kind string of the link being traversed (from `SetOffMeshLinkMetadata`), or `""` |
| `IsStopped`         | Boolean | Read       | True while steering is paused by `Stop()` |
| `HasTarget`         | Boolean | Read       | True while a navigation target is set |
| `Target`            | Object  | Read       | Current goal position as `Vec3` (origin when none — check `HasTarget`) |
| `PathCornerCount`   | Integer | Read       | Corners in the most recently computed path (0 when never pathed) |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetTarget(pos)` | `Void(Object)` | Set a new goal position |
| `ClearTarget()` | `Void()` | Cancel current path |
| `Update(deltaSeconds)` | `Void(Double)` | Advance agent along the path |
| `Warp(pos)` | `Void(Object)` | Teleport the agent to a position |
| `Stop()` | `Void()` | Pause steering: the target and path are kept, motion is zeroed each tick |
| `Resume()` | `Void()` | Resume steering paused by `Stop()`; the retained path continues |
| `GetPathCorner(index)` | `Object(Integer)` | World-space corner of the current path as `Vec3`, in walk order (origin when out of range) |
| `BindCharacter(character3D)` | `Void(Object)` | Drive a `Character3D` from agent velocity |
| `BindNode(sceneNode)` | `Void(Object)` | Drive a `SceneNode` position from agent |

`Stop`/`Resume` pause an agent without forgetting where it was going — the Unity
`isStopped` idiom. A stopped agent still syncs its position from bindings and
keeps `RemainingDistance` live, so HUD and AI readback stay correct while a
cutscene or dialogue holds the character in place. `PathCornerCount` /
`GetPathCorner(i)` expose the computed corner list for debug drawing and
path-preview overlays; corners persist while the agent walks (or is stopped)
and reset on `ClearTarget` or repath.

Avoidance is local and opt-in. Agents on the same `NavMesh3D` with `AvoidanceEnabled=true` solve a deterministic reciprocal-velocity-obstacle candidate set over nearby grid peers before the update drives a character or node. The solver predicts collisions over a bounded horizon, prefers the path-following velocity, and has a named 200-agent CTest baseline.

Native crowd schedulers may use the additive embedding API
`rt_navagent3d_update_batch(void *const *agents, int64_t count, double dt)`.
It canonicalizes unique valid handles by creation order, prepares the complete
selected set, snapshots every live peer once, solves against only that immutable
start-of-tick state, and then applies in the same deterministic order. The
registered `Update(deltaSeconds)` method remains the single-agent interface.

`OnOffMeshLink` and `LinkKind` let gameplay code react to authored traversal
(play a jump animation, disable gravity on a ladder): they match the agent's
current waypoint segment against the navmesh's off-mesh links, so they report
`true` only while the segment between the current and next waypoint is a link
edge. An idle agent, or one walking ordinary polygons, reports `false`/`""`.

```zia
bind Zanna.Graphics3D.NavMesh3D as NavMesh3D;
bind Zanna.Graphics3D.NavAgent3D as NavAgent3D;

// ... obtain groundMesh and deltaSeconds from the scene loop ...
var nav = NavMesh3D.Build(groundMesh, 0.5, 1.8);
var agent = NavAgent3D.New(nav);
agent.DesiredSpeed = 4.0;
agent.StoppingDistance = 0.3;
agent.AvoidanceEnabled = true;
agent.AvoidanceRadius = 0.6;
agent.SetTarget(Vec3.New(12.0, 0.0, 8.0));

// per frame
agent.Update(deltaSeconds);
```

---

## Environment

### Zanna.Graphics3D.Terrain3D

Heightmap terrain with multi-layer splat texturing, LOD, and normal generation.

**Type:** Instance (obj)
**Constructor:** `Terrain3D.New(heightmapPixels, scaleX, scaleY, scaleZ)`

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetHeightmap(pixels)` | `Void(Object)` | Replace the heightmap with new `Pixels` |
| `SetMaterial(material)` | `Void(Object)` | Assign a base `Material3D` |
| `SetScale(x, y, z)` | `Void(Double, Double, Double)` | Set horizontal and vertical scale |
| `GeneratePerlin(pixels, frequency, octaves, persistence)` | `Void(Object, Double, Integer, Double)` | Generate a Perlin heightmap into `pixels` |
| `SetSplatMap(pixels)` | `Void(Object)` | Set splat map 0 (weights layers 0-3) for multi-layer texture blending |
| `SetSplatMapAt(index, pixels)` | `Void(Integer, Object)` | Set splat map `index`: 0 weights layers 0-3, 1 weights layers 4-7 |
| `SetHole(x, z, width, depth)` | `Integer(Double, Double, Double, Double)` | Carve a rectangular hole (terrain-local units); render, nav meshes, and heightfield collision all skip the carved cells. Returns the hole index |
| `RemoveHole(index)` | `Boolean(Integer)` | Remove a hole by index (later holes shift down) |
| `ClearHoles()` | `Void()` | Remove every authored hole |
| `HoleCount` (property) | `Integer` | Number of authored holes |
| `SetSlopeLayer(layer, minDeg, maxDeg, sharpness)` | `Void(Integer, Double, Double, Double)` | Rule: weight `layer` where surface slope falls in the band (degrees) |
| `SetHeightLayer(layer, minY, maxY, sharpness)` | `Void(Integer, Double, Double, Double)` | Rule: weight `layer` where world height falls in the band |
| `RebuildSplatWeights()` | `Void()` | Regenerate both splat maps from the configured slope/height rules (deterministic CPU pass; painting afterwards overrides) |
| `SetLayerTexture(layer, pixels)` | `Void(Integer, Object)` | Set the texture for splat layer `[0–3]` |
| `SetLayerScale(layer, scale)` | `Void(Integer, Double)` | Set UV tiling scale for a splat layer |
| `GetHeightAt(worldX, worldZ)` | `Double(Double, Double)` | Sample interpolated terrain height |
| `GetNormalAt(worldX, worldZ)` | `Object(Double, Double)` | Sample terrain surface normal as `Vec3` |
| `SetLodDistances(near, far)` | `Void(Double, Double)` | Set LOD transition distances |
| `SetSkirtDepth(depth)` | `Void(Double)` | Set the tile skirt depth to hide LOD seams |

Terrain supports up to **8 splat layers**: `SetLayerTexture`/`SetLayerScale`
accept indices 0-7, splat map 0 weighs layers 0-3 and splat map 1 weighs
layers 4-7, and weights normalize across all populated channels. The realtime
per-pixel splat path carries 4 layers; content using layers 4-7 (or a second
splat map) automatically routes through the baked composite, which renders
identically on every backend. Incomplete splat sets render with the base
material/fallback texture.

Holes carve through every consumer from one rasterized cell bitmask: chunk
meshes skip carved cells at all LOD levels (conservatively — a coarse
triangle disappears when any covered fine cell is holed), `BuildNavMesh`
omits carved cells, and heightfield colliders built from the terrain report
no surface inside the footprint so physics falls through. Streamed terrain
tiles accept an optional manifest `"holes": [[x, z, width, depth], ...]`
array applied at payload instantiation (tile-local units), and the streamed
collider/nav entities pick the carve-through up automatically. Skirts at
chunk borders are unaffected by interior holes; a hole touching a chunk
border can leave a visible skirt wall inside the pit at LOD seams.

```zia
bind Zanna.Graphics3D.Terrain3D as Terrain3D;

// ... load splatPixels, grassTex, and rockTex before configuring the terrain ...
var heightmap = Pixels.New(256, 256);
var terrain = Terrain3D.New(heightmap, 1.0, 0.25, 1.0);
terrain.GeneratePerlin(heightmap, 0.05, 6, 0.5);
terrain.SetHeightmap(heightmap);
terrain.SetSplatMap(splatPixels);
terrain.SetLayerTexture(0, grassTex);
terrain.SetLayerTexture(1, rockTex);
```

---

### Zanna.Graphics3D.Water3D

Animated water plane with wave simulation, reflections, and normal mapping.

**Type:** Instance (obj)
**Constructor:** `Water3D.New(scene)`

#### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `SimDistance` | Number | Read/Write | Camera distance beyond which the per-frame CPU wave rebuild is skipped (the last simulated surface keeps rendering). `0` (default) always simulates. Large or many water bodies should set this to their visible range — a 256-resolution surface rebuilds ~66K vertices per frame otherwise |
| `Height` | Double | Read | Retained surface height (world Y) |
| `Position` | Object (`Vec3`) | Read | Fresh snapshot of the surface center and height |
| `WaveSpeed` / `WaveAmplitude` / `WaveFrequency` | Double | Read | Retained legacy single-wave parameters (ADR 0227) |
| `Color` | Object (`Vec3`) | Read | Fresh snapshot of the retained tint |
| `Alpha` / `Reflectivity` | Double | Read | Retained opacity and reflection strength |
| `Resolution` | Integer | Read | Retained grid quads per axis |
| `Texture` / `NormalMap` | Object (`Pixels`) | Read | Borrowed bound surfaces, `null` when unbound |
| `EnvMap` | Object (`CubeMap3D`) | Read | Borrowed reflection cubemap, `null` when unbound |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetHeight(y)` | `Void(Double)` | Set the water plane world Y position |
| `SetPosition(x, y, z)` | `Void(Double, Double, Double)` | Set the world-space surface center and base height |
| `SetWaveParams(speed, amplitude, frequency)` | `Void(Double, Double, Double)` | Configure the legacy single-sine-wave parameters: signed temporal phase speed, vertical amplitude, spatial angular frequency |
| `SetColor(r, g, b, a)` | `Void(Double, Double, Double, Double)` | Set base water color |
| `SetTexture(pixels)` | `Void(Object)` | Set the surface texture |
| `SetNormalMap(pixels)` | `Void(Object)` | Set the normal map for surface detail |
| `SetEnvMap(cubeMap)` | `Void(Object)` | Set the reflection `CubeMap3D` |
| `SetReflectivity(amount)` | `Void(Double)` | Set reflection strength `[0.0–1.0]` |
| `SetResolution(pixels)` | `Void(Integer)` | Set reflection render resolution |
| `AddWave(dirX, dirZ, speed, amplitude, wavelength)` | `Void(Double, Double, Double, Double, Double)` | Add a Gerstner wave: propagation direction (x, z), signed phase speed, vertical amplitude, wavelength in world units. Up to 8 waves total |
| `ClearWaves()` | `Void()` | Remove all Gerstner waves (reverts to the legacy single-sine wave) |

`Water3D.Update` represents its generated wave bases as packed
`MorphTarget3D` data and submits them through the ordinary morph-deformation
path. GPU and software backends therefore animate the same surface, retained
bounds remain conservative while it moves, and reduced graphics builds keep
the same behavior. Index topology is rebuilt only when resolution or capacity
changes.

---

### Zanna.Graphics3D.Vegetation3D

GPU-instanced foliage (grass, bushes) with density map, wind animation, and LOD.

**Type:** Instance (obj)
**Constructor:** `Vegetation3D.New(mesh)`

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetDensityMap(pixels)` | `Void(Object)` | Control placement density from a grayscale map |
| `SetWindParams(speed, strength, turbulence)` | `Void(Double, Double, Double)` | Set foliage wind sway |
| `SetLodDistances(near, far)` | `Void(Double, Double)` | Set LOD fade distances |
| `SetBladeSize(width, height, variance)` | `Void(Double, Double, Double)` | Set blade/frond dimensions |
| `SetSeed(seed)` | `Void(Integer)` | Set deterministic scatter seed for later `Populate` calls |
| `Populate(terrain, count)` | `Void(Object, Integer)` | Scatter `count` instances over a `Terrain3D` using the density map |
| `Update(deltaSeconds, camX, camY, camZ)` | `Void(Double, Double, Double, Double)` | Advance wind simulation relative to camera position |

Vegetation draws use a double-sided blade material instead of mutating the
canvas-wide backface-cull flag.

---

### Zanna.Graphics3D.Sky3D

Deterministic procedural sky: a CPU-generated cubemap evaluated from sun
direction, turbidity, and ground albedo (turbidity-tinted horizon, sun disc
and halo, night fade). `Update(canvas)` regenerates the cubemap when dirty and
installs it through the normal skybox path, so fog and image-based lighting
observe the same environment.

**Type:** Instance (obj)
**Constructor:** `Sky3D.New()`

| Method / Property | Signature | Description |
|-------------------|-----------|-------------|
| `SetSunDirection(dir)` | `Void(Object)` | Set the (normalized-on-load) direction toward the sun as a `Vec3` |
| `SetGroundAlbedo(r, g, b)` | `Void(Double, Double, Double)` | Set the ground-bounce color, clamped to `[0, 1]` |
| `SunDirection` | Object (`Vec3`) | Fresh snapshot of the normalized sun direction (ADR 0233) |
| `GroundAlbedo` | Object (`Vec3`) | Fresh snapshot of the retained ground albedo (ADR 0233) |
| `Turbidity` | Double (Read/Write) | Atmospheric haze factor |
| `Resolution` | Integer (Read/Write) | Cubemap face resolution |
| `Dirty` | Boolean (Read) | True while authored parameters are newer than the generated cubemap |
| `Cubemap` | Object (Read) | Retained generated `CubeMap3D`, `null` before the first `Update` |
| `Update(canvas)` | `Boolean(Object)` | Regenerate if dirty and install as `canvas`'s skybox (`null` skips the install) |

Pair with `TimeOfDay3D`, which drives a `Light3D` and a bound `Sky3D` from an
hour-of-day and latitude (`Hours`, `Latitude`, `SunDirection` readback, and
per-frame `Advance`); see the Graphics3D guide for the full class table.

---

## Misc 3D Helpers

### Zanna.Graphics3D.Trigger3D

AABB trigger volume that tracks entry and exit events for `PhysicsBody3D` objects.

**Type:** Instance (obj)
**Constructor:** `Trigger3D.New(minX, minY, minZ, maxX, maxY, maxZ)`

#### Properties

| Property     | Type    | Access | Description |
|--------------|---------|--------|-------------|
| `EnterCount` | Integer | Read   | Number of bodies that entered this frame |
| `ExitCount`  | Integer | Read   | Number of bodies that exited this frame |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Contains(point)` | `Boolean(Object)` | True when a world-space `Vec3` lies inside the volume |
| `Update(world)` | `Void(Object)` | Recompute enter/exit events against a `PhysicsWorld3D` |
| `SetBounds(minX, minY, minZ, maxX, maxY, maxZ)` | `Void(Double, Double, Double, Double, Double, Double)` | Resize the trigger volume |

---

### Zanna.Graphics3D.Transform3D

World-space transform (position, rotation quaternion, scale) with matrix output.

**Type:** Instance (obj)
**Constructor:** `Transform3D.New()`

#### Properties

| Property   | Type   | Access     | Description |
|------------|--------|------------|-------------|
| `Position` | Object | Read       | World position as `Vec3` |
| `Rotation` | Object | Read/Write | Rotation as quaternion `Vec4(x,y,z,w)` |
| `Scale`    | Object | Read       | Scale as `Vec3` |
| `Matrix`   | Object | Read       | Combined 4×4 transform matrix as flat `Mat4` |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `Void(Double, Double, Double)` | Set world position |
| `SetEuler(pitchDeg, yawDeg, rollDeg)` | `Void(Double, Double, Double)` | Set rotation from Euler angles in degrees |
| `SetScale(x, y, z)` | `Void(Double, Double, Double)` | Set scale |
| `Translate(delta)` | `Void(Object)` | Move by a `Vec3` offset |

Very large Euler angles and incremental rotation angles are reduced before trigonometry runs, and
the stored quaternion is kept normalized. Finite zero scale is preserved; only non-finite scale
components are replaced with a safe identity value.

---

### Zanna.Graphics3D.Path3D

World-space spline path for camera tracks, patrols, or procedural animation.

**Type:** Instance (obj)
**Constructor:** `Path3D.New()`

#### Properties

| Property     | Type    | Access     | Description |
|--------------|---------|------------|-------------|
| `Length`     | Double  | Read       | Total arc length of the path |
| `PointCount` | Integer | Read       | Number of control points |
| `Looping`    | Boolean | Write      | Whether the path wraps at the end |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddPoint(pos)` | `Void(Object)` | Append a `Vec3` control point |
| `GetPositionAt(t)` | `Object(Double)` | Interpolated `Vec3` position at normalized `t` `[0.0–1.0]` |
| `GetDirectionAt(t)` | `Object(Double)` | Tangent direction `Vec3` at normalized `t` |
| `Clear()` | `Void()` | Remove all control points |

Length sampling is capped internally, so very large point counts cannot overflow the subdivision step
calculation.

---

### Zanna.Graphics3D.InstanceBatch3D

Efficient GPU-instanced rendering of many copies of the same mesh.

**Type:** Instance (obj)
**Constructor:** `InstanceBatch3D.New(mesh, material)`

#### Properties

| Property | Type    | Access | Description |
|----------|---------|--------|-------------|
| `Count`  | Integer | Read   | Current number of instances |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Add(transform)` | `Void(Object)` | Append a `Transform3D` instance |
| `Remove(index)` | `Void(Integer)` | Remove instance at `index` |
| `Set(index, transform)` | `Void(Integer, Object)` | Replace the transform at `index` |
| `Clear()` | `Void()` | Remove all instances |

`Add` and `Set` require a valid runtime `Mat4` object with a complete matrix payload; foreign or
undersized objects are ignored. Stack-backed mesh fixtures can be drawn through the instanced path
when used by runtime systems. Draw submission sanitizes material scalars before narrowing to backend
floats. Raw instanced draw submission retains mesh/material objects for the deferred frame and
synthesizes previous instance matrices on the GPU path when explicit previous matrices are not
supplied. Motion history is separated by batch buffer identity, so keep the same instance-matrix
buffer across frames when continuous motion vectors are desired.

Every backend — including software — implements the instanced-draw hook for
opaque batches, so large batches (up to 1,048,576 instances) never fall back to
per-instance queueing or trap. `BackendSupports("instancing")` reports the hook;
`BackendSupports("hardware_instancing")` additionally requires a GPU backend.
Blended-material and floating-origin-rebased batches still route per instance
(clamped at 65,536 queued instances, with overflow visible through
`Canvas3D.InstancedFallbackDroppedCount`).

Static shadow casters can batch on GPU backends, including compatible repeated
mesh draws outside an explicit `InstanceBatch3D`. Depth-affecting material and
alpha state stay separate; software and deformed payloads retain per-mesh depth
submission. When shadows are enabled, explicit batches retain potential casters
outside the camera view because their shadows may reach visible receivers.
This conservative shared instance list can submit extra clipped main-view
instances. `ShadowMode.None` and frames without shadows retain camera culling.

**Per-instance skinned crowds.** `Canvas3D.DrawInstancedSkinned(batch, players)`
renders the batch with one animator per instance: `players` is a `Seq` holding
one `AnimPlayer3D` or `AnimController3D` per batch entry (counts must match, and
every animator must drive a skeleton with the same bone count). On GPU-skinning
backends the palettes are packed into the shared 256-bone palette slot and the
vertex shader indexes `palette[instanceId * boneCount + bone]`, so
`256 / boneCount` uniquely-posed characters ride each hardware draw — a 32-bone
crowd costs one draw per 32 characters instead of one per character. The
software backend, blending materials, eight-influence meshes, and partitioned
(>256-bone) skins fall back to one skinned draw per instance with identical
posing. Skeletons over 256 bones are rejected; palettes are snapshotted at
submission so later animation ticks cannot repose queued draws.

---

### Zanna.Graphics3D.Sprite3D

Billboard sprite placed in 3D world space, suitable for particles, UI labels, or 2D-in-3D overlays.

**Type:** Instance (obj)
**Constructor:** `Sprite3D.New(pixels)`

#### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Additive` | Boolean | Read/Write | Additive RGB blending (muzzle glows, tracers); preserves destination alpha so accumulated light does not obscure a composited scene. Default off (alpha blend) |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `Void(Double, Double, Double)` | Set world position |
| `SetScale(w, h)` | `Void(Double, Double)` | Set billboard scale in world units |
| `SetAnchor(ax, ay)` | `Void(Double, Double)` | Set anchor offset `[0.0–1.0]` |
| `SetFrame(x, y, width, height)` | `Void(Integer, Integer, Integer, Integer)` | Set the source pixel region |
| `SetColor(rgb)` | `Void(Integer)` | Packed `0xRRGGBB` tint multiplied into the texture (Particles3D convention) |
| `RebaseOrigin(dx, dy, dz)` | `Void(Double, Double, Double)` | Shift the sprite position by `-delta` |

---

### Zanna.Graphics3D.TextureAtlas3D

Texture atlas for 3D rendering with named-region management.

**Type:** Instance (obj)
**Constructor:** `TextureAtlas3D.New(width, height)`

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Add(pixels)` | `Integer(Object)` | Pack a `Pixels` image into the atlas; returns region index |
| `GetTexture()` | `Object()` | Return the backing `Pixels` buffer |

---

## Notes

- `Transform3D` is distinct from `SceneNode` — use `Transform3D` for standalone matrix math and non-scene-graph transforms; attach nodes to the scene for scene-managed transform hierarchies.
- `AnimController3D.PollEvent` returns events one at a time per call; poll it in a loop until an empty string is returned if multiple events fire in one update.
- `NavMesh3D` direct meshes are rebuilt by `NavMesh3D.Build`; `BakeTiled`
  retains voxel source data so `RebuildTile` can refresh one tile's geometry,
  while tiled obstacle edits re-carve only affected tiles. Keep baked meshes
  manifold at shared edges.
- `Particles3D.Draw` should be called inside the `Canvas3D.Begin`/`End` scene pass after opaque geometry when you want particles over the main scene.
- Deferred heap `Mesh3D` draws retain an immutable reference-counted geometry revision, so submitted bytes remain stable through `Canvas3D.End()` even if the live mesh mutates. Unchanged later frames reuse the same revision without copying vertex/index arrays. Missing normal-map tangents are a separate persistent variant of that revision and are invalidated by any position, normal, UV, or topology edit. Camera-relative rebase, skinned, and morphed paths keep explicit dynamic snapshots; public `DrawMesh` still rejects raw stack mesh payloads.
- `Ray3D.IntersectMesh` lazily retains a local-space triangle BVH per mesh geometry revision. Repeated identity/invertible-transform queries reuse it, while singular transforms and allocation failure keep the exact linear fallback and deterministic lowest-triangle tie behavior.
- `SpatialAudio3D.SyncBindings` must be called once per frame after physics/animation updates so bound sources and listeners track their nodes.

### Secondary shadow atlas resolution

`Canvas3D.SetShadowAtlasResolution(pixels)` controls the square tile size for
shadow slots 4 and above. `ShadowAtlasResolution` reads the retained override.
Zero (the default) inherits `EnableShadows` resolution; negative values become
zero, and positive values clamp to 64–4096. The first four maps retain the primary
resolution. All secondary tiles share one resolution, including any secondary
directional maps allocated there. Changing the override invalidates shadow targets
and cached passes; setting the same value preserves them. For example, a 4096
primary map with a 1024 atlas reduces secondary-map storage at the cost of shadow
detail. This setting does not increase the backend's shadow-slot capacity.
