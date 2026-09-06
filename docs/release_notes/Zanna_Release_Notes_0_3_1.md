---
status: active
audience: public
last-verified: 2026-09-05
---

# Zanna Compiler Platform — Release Notes

> **Development Status:** Pre-Alpha. Zanna is under active development and not ready for production use.

## Version 0.3.1 — Pre-Alpha (DRAFT — unreleased)

<!-- DRAFT: release date TBD. Changes are measured from the v0.3.0-prealpha tag, cut 2026-08-28. -->

### What this release is about

v0.3.1 makes a Zanna game more dependable when it grows beyond a prototype. Ordinary MP3 music plays through instead of stopping after its opening seconds, and keeps playing when a loading screen or a slow frame holds up the game loop. Large scenes bake their lighting with the CPU available to them without changing the result, and they no longer leave later lights or soft light shapes out of the finished image.

The release also gives 3D projects more useful ways to make and show content. Secondary cameras can reuse compatible main-view shadows, models can supply masks from any chosen model axis, and a mesh can be lofted from an authored height profile. Asset preparation can retain sensible levels of detail for a finished scene instead of requiring every piece to be drawn at its highest cost. Decals, mirrored characters, application icons, and complex venue-style scenes all render more consistently across the supported backends.

A significant part of the work came from the rough edges players notice first. A game that switches a shared window between 2D menus and a 3D world keeps its cursor, overlay, and displayed size aligned through fullscreen and resize changes. Switching away from a game clears held input instead of leaving a character walking or accidentally activating a button. Long sessions that load and unload models keep showing the content that is actually loaded, and post-processing no longer lets one camera borrow another camera's exposure or motion history.

Zanna Studio is smoother on a large project and safer when work is interrupted. Search, build output, and workspace services stay out of the way of editing; split editors stay responsive while you type; Save All and background services report failures clearly; and an interrupted workspace-wide change leaves a recoverable record instead of an ambiguous collection of temporary files. Across the compiler and runtime, the same release closes correctness and lifetime issues that previously surfaced as intermittent failures in longer-running applications.

#### Highlights

- **MP3 music plays complete, ordinary files (new).** Zanna's from-scratch decoder now supports the common MPEG Layer III variants used by everyday encoders, including variable bit rate, short blocks, joint stereo, MPEG-1, MPEG-2, and MPEG-2.5.
- **Your soundtrack survives a slow frame (new).** Music streaming refills in the background, so asset loading or an unusually long frame does not drain playback into silence.
- **A bake uses your CPU and remains repeatable (new).** Lightmaps and probes can use multiple cores while producing the same output regardless of worker count. Large scenes also keep all of their local lights, including rectangular, spherical, and volume lights with their authored shape.
- **Secondary camera views are more practical (new).** A picture-in-picture, security monitor, or in-world screen can reuse compatible shadows from the main view and choose a lower shadow cascade budget without lowering the main view's quality.
- **More of your authored model can drive the material (new).** `Mesh3D.RasterizeUvAxis` can turn X, Y, or Z position into a UV-space mask, useful for applying a material treatment to a model region such as a side panel, sleeve, or depth band. `Mesh3D.HeightLoft` creates shaped geometry from a defined height profile.
- **Detailed scenes stay manageable (new).** Offline asset preparation can retain constrained levels of detail for compatible static content, while the renderer keeps complete lighting results when a scene's light list is crowded. The result is a more graceful quality/performance trade-off for large environments.
- **Projected decals and mirrored characters hold together.** Decals follow curved, animated, and two-sided surfaces more reliably, and `Mesh3D.Mirror` reflects skinning and morph detail as well as the visible shape.
- **Your game can look like itself outside the frame (new).** A 2D or 3D canvas can set its application icon from `Pixels`, so supported desktop shells show the game's identity instead of a generic image.
- **Fullscreen single-window games agree with the mouse.** When 2D menus hand a window to a 3D world, the pointer, canvas size, and overlay stay in one coordinate space through fullscreen, resize, and return to the menu. Fullscreen also works on monitor-sized and high-density framebuffers.
- **Switching away from a game does not leave input stuck.** Losing focus clears held keys, mouse buttons, text, wheel movement, and click history without inventing a release or click. Alt-tab can no longer confirm a control on the way out.
- **A model you released cannot come back as stale geometry.** Long sessions that load and unload content keep drawing the model, material, and morph state currently in use rather than a retired resource that happened to occupy the same memory.
- **Post-processing belongs to one camera at a time.** A second concurrent use of the same temporal post-processing chain is refused with a clear recoverable error, preventing cross-camera ghosting and exposure jumps. Give each active `Canvas3D` its own chain.
- **Frames cost less, and controller look is frame-rate independent.** Common character and camera updates avoid per-frame temporary allocations, while gamepad look motion now behaves consistently on faster and slower machines.
- **Studio stays responsive and recoverable on a big project.** Search and output arrive in bounded batches, split views avoid full-document work on every keystroke, and an interrupted workspace-wide rename or replace leaves a durable recovery record.
- **Validation feedback arrives sooner.** Cost-aware test scheduling shortens a representative parallel validation run without running fewer checks.
- **Long-running applications are more dependable.** Strong cycles between Zia class instances can be collected explicitly, changed files report a read failure instead of a silent partial snapshot, native programs retain useful profiling symbols by default, and a broad set of compiler, runtime, rendering, navigation, physics, and packaging fixes reduce intermittent failures.
- **Text and presentation hold up in more cases.** Typewriter dialogue reveals accented and non-Latin text by character, dialogue bubbles remain within small canvases, and dense scenes, particle trails, and dynamically changing worlds behave more reliably.

### By the Numbers

| Metric | v0.3.0 | v0.3.1 | Delta |
|---|---:|---:|---:|
| Commits | — | 22 | +22 |
| Source files | 3,704 | 3,717 | +13 |
| Production SLOC | 884K | 891K | +7K |
| Test SLOC | 332K | 337K | +5K |
| Zanna Studio SLOC | 160K | 162K | +2K |
| Demo SLOC | 242K | 242K | — |

Counts use `scripts/count_sloc.sh`, excluding blank lines and comments (production 891,018; test 336,899; Zanna Studio 161,594; source files 3,717). Commit count is measured from the `v0.3.0-prealpha` tag on 2026-08-28. Demo SLOC remains the rounded combined total reported for v0.3.0.

---

The rest of this document is the detail, area by area.

### Audio

- Imported MP3 music now supports the normal Layer III files made by common tools: mono, stereo, joint stereo, constant and variable bit rates, and the MPEG-1, MPEG-2, and MPEG-2.5 families.
- Music refills on a background worker. A loading screen, asset batch, or long frame no longer makes a playing track fall silent; `Audio.Update()` still handles crossfades and playlist progression.

### 3D rendering and baked lighting

- Lightmap and probe baking can use available CPU cores while retaining deterministic output. It remains suitable for incremental editor work, and scenes with many local lights no longer stop contributing after the first sixteen.
- Render-target views can inherit compatible main-camera shadows with `Canvas3D.SetRenderTargetShadowInherit(true)` and can independently limit their cascades when a cheaper secondary view is the better choice.
- Geometry depth, thin surfaces, terrain, and clustered lighting behave more consistently across Metal, OpenGL, Direct3D 11, and the software renderer. Full light-list capacity now preserves a correct result rather than quietly losing the overflow.
- Renderer ownership checks prevent stale retained meshes, materials, scene nodes, and morph targets from being reused after content is released and replaced. Temporal post-processing has one active canvas owner, preventing a picture-in-picture or second view from inheriting the wrong history.
- Character movement, camera placement, and third-person camera work create less transient data each frame. Mouse movement remains immediate, while controller-stick look integrates by frame duration so its speed is consistent.
- Dynamic scenes receive broad reliability work across culling, draw submission, particles, motion vectors, navigation, pathfinding, physics, animated loading, and the software renderer. Dialogue reveal and anchoring are also robust for Unicode text and small canvases.

### 3D assets and materials

- `Mesh3D.RasterizeUvAxis` produces a UV-space coordinate map from the model's X, Y, or Z axis. It is useful when a material needs a mask that follows a model region without hand-painting one.
- `Mesh3D.HeightLoft` builds shaped mesh geometry from an explicit height profile. Asset preparation can also retain constrained levels of detail for compatible authored geometry, giving large scenes a controlled quality/performance path.
- `Mesh3D.Mirror(mesh, skeleton)` mirrors the visible mesh, skinning influences, and morph targets, so a paired asymmetric prop can genuinely move to the opposite hand.
- `Material3D` projected decals follow animated geometry and preserve lighting on curved and two-sided surfaces, making markings such as numbers, logos, signage, and wear more dependable.
- `Canvas.SetIcon(pixels)` and `Canvas3D.SetIcon(pixels)` let a game provide its own application icon on supported desktop platforms.

### Windows, fullscreen, and input

- A game that uses one window for 2D menus and a 3D world keeps its pointer, 3D extent, and 2D overlay in the same public coordinate space. The alignment remains correct across fullscreen, resize, scale changes, and handing the window back to the menu.
- Fullscreen accepts monitor-sized and high-density framebuffers rather than retaining a stretched window-sized frame.
- Focus loss clears held input and transient input state without generating a release or click. Returning to the game begins from a clean physical press.

### Compiled code and runtime

- Native-code and optimizer fixes make valid programs behave more consistently across supported architectures, including nested calls and inherited members. Native executables carry symbols by default for useful profiling; `zanna build --strip-symbols` remains available when size takes priority.
- Object fields and returned runtime values follow more consistent lifetime rules. `Zanna.Runtime.GC.Collect()` can reclaim strong reference cycles between class instances, while preserving explicit collection.
- Repeated string literals and common handle checks cost less in hot paths. Allocation failures are handled more consistently, and `File.ReadAllText` reports a file that changed during reading instead of returning a partial snapshot.

### Zanna Studio

- Search, build/run output, indexing, file watching, autosave, terminal sessions, debugger dispatch, and background work are bounded and report failure states more clearly, keeping a large workspace responsive.
- Split editors replay small changes rather than copying and relaying the whole document after every keystroke, and language services follow the same edit history independently.
- Workspace-wide rename and replace operations retain a durable recovery journal until they complete. If a crash or power interruption occurs, the staged and replaced files can be identified and recovered; successful edits leave no journal behind.
- Save All, workspace services, and embedded play receive additional reliability work, including a safe fallback for restricted macOS environments.
- Cost-aware validation scheduling reduces unnecessary waiting in a representative parallel test run without dropping checks.

### Packaging

- Windows installer creation no longer fails for payload layouts whose embedded offset previously oscillated during finalization.
- macOS disk images sign the app after it reaches its final location in the image, preserving a valid signature when the image filesystem normalizes a resource name.

### Documentation

Public references, command help, platform guidance, runtime API material, and language examples were reconciled with the current compiler and runtime behavior. The Game3D guidance now explains the single-window fullscreen coordinate contract and the separate responsibility of an application that chooses to letterbox a fixed design space.

### Known limitations

- This is a pre-alpha source release. APIs, diagnostics, IL rules, project formats, and tooling may change before a stable release.
- Native macOS support targets Apple Silicon. Linux targets x86-64 and AArch64; backend and platform capability details remain documented in the supported-platform guides.
- OpenGL point shadows, one Metal counter, and software-renderer reflection/HDR settings remain unavailable as documented in the 3D guides.
- MP3 frames that select the reserved Huffman codebooks 4 or 14 are rejected as unsupported data; standard encoders do not emit them.
- A `PostFX3D` chain can be attached to only one `Canvas3D` at a time. Projects that want the same post-processing on several active canvases need one chain per canvas; the second attachment is refused through `PostFX3D.LastError` rather than trapping.
- In a fullscreen single-window game, the adopted 3D canvas reports the window's public extent rather than the 2D canvas's designed size. Fitting a fixed design space inside that extent, and any letterboxing it implies, is the application's responsibility.

<!-- END DRAFT -->
