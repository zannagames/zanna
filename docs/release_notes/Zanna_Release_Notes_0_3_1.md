---
status: active
audience: public
last-verified: 2026-09-02
---

# Zanna Compiler Platform — Release Notes

> **Development Status:** Pre-Alpha. Zanna is under active development and not ready for production use.

## Version 0.3.1 — Pre-Alpha (DRAFT — unreleased)

<!-- DRAFT: release date TBD. Changes are measured from the v0.3.0-prealpha tag, cut 2026-08-28. -->

### What this release is about

v0.3.1 is a short follow-up release about making the things a game depends on behave reliably at scale. Music plays through real MP3 files and keeps playing through a long loading frame. Light baking uses the CPU available to it without changing the result, and it no longer quietly leaves part of a large or softly lit scene unbaked. Studio stays responsive while it searches or builds a large project.

The release also makes secondary camera views more practical. A picture-in-picture, security monitor, or live in-world display can reuse shadows that are still valid from the main camera, and can limit the shadow work it requests when that is the right trade-off. That gives projects more room for these views without taking time away from the main frame. Games can also present their own icon in the window and desktop shell, while projected decals hold together across the curved and animated surfaces they are meant to decorate.

Underneath, a broad correctness pass addressed failures that were most visible in long-running projects: optimized native programs can be profiled by function name, more compiler edge cases produce the intended program, resources are released consistently, and a file that changes while it is read reports an error rather than returning a partial result. The result is less time spent chasing intermittent failures and more trustworthy behavior when a project gets large.

#### Highlights

- **MP3 music now plays complete, ordinary files (new).** Zanna's from-scratch decoder supports the MPEG Layer III variants used by common encoders, including MPEG-1, MPEG-2, MPEG-2.5, variable bit rate, short blocks, and joint stereo. Music that previously stopped almost immediately now plays through.
- **Your soundtrack survives a stalled frame (new).** Music streaming refills in the background, so loading assets or a long frame no longer drains the audio buffer into silence. `Audio.Update()` still advances crossfades and playlists as before.
- **A bake uses your CPU and remains repeatable (new).** Lightmaps and probes can use multiple cores while producing the same output regardless of worker count. Interactive baking retains its bounded work per update.
- **Large, softly lit scenes bake correctly.** The old sixteen-light ceiling is gone, local lights are evaluated more efficiently, and rectangle, sphere, and volume lights bake with their authored shape instead of as points.
- **Secondary camera views cost less (new).** Opt-in shadow reuse lets an off-screen `Canvas3D` keep shadows it can safely share with the main view; a cascade limit provides a further quality/performance choice for render-target views.
- **Projected decals arrive in 3D materials (new).** Add a number, logo, or scuff through a material projector and it stays with an animated surface while receiving the scene's lighting.
- **Your game can look like itself outside the frame (new).** A 2D or 3D canvas can set its application icon from `Pixels`, so a running game appears with its own identity in supported desktop shells rather than a generic application image.
- **Decals stay put on real geometry.** Projected markings now blend across curved surfaces and render consistently on two-sided materials, so logos, uniform numbers, signage, and wear do not disappear at a hard facing angle.
- **A mirrored character is fully mirrored (new).** `Mesh3D.Mirror` reflects skinned geometry, morph targets, and left/right bone influences, so an asymmetric prop moves to the matching hand when paired with mirrored animation.
- **Profilers can name your functions (new).** Native executables include symbols by default, allowing macOS and Linux profiling tools to attribute work to your functions rather than anonymous addresses. Use `zanna build --strip-symbols` when a smaller executable matters more.
- **Studio stays responsive on a big project.** Search analysis runs in the background, results and build output arrive in small batches, and workspace indexing now reaches the end of very large projects.
- **Long-running programs are more dependable.** The compiler, runtime, renderer, and native backends received fixes for optimized-code edge cases, resource lifetime, invalid input, and interrupted file reads.
- **Zanna's own validation runs faster.** Cost-aware test scheduling roughly halved the measured time for a representative parallel test run, helping contributors get feedback sooner without running fewer tests.

### By the Numbers

| Metric | v0.3.0 | v0.3.1 | Delta |
|---|---:|---:|---:|
| Commits | — | 15 | +15 |
| Source files | 3,704 | 3,715 | +11 |
| Production SLOC | 884K | 889K | +6K |
| Test SLOC | 332K | 335K | +4K |
| Zanna Studio SLOC | 160K | 161K | +1K |
| Demo SLOC | 242K | 242K | — |

Counts use `scripts/count_sloc.sh`, excluding blank lines and comments (production 889,430; test 335,303; Zanna Studio 160,878; source files 3,715). Commit count is measured from the `v0.3.0-prealpha` tag on 2026-08-28. Demo SLOC remains unchanged at the rounded total.

---

The rest of this document is the detail, area by area.

### Audio

- The MP3 decoder now covers normal MPEG Layer III music files across MPEG-1, MPEG-2, and MPEG-2.5, including mono, stereo, joint stereo, constant and variable bit rates. It was checked against independent decoders across those formats, so imported music is no longer limited to a narrow set of test-like encodes.
- Streaming refills happen on a background worker. A loading screen, asset batch, or unusually long frame no longer causes a playing track to go silent simply because the application has not reached its next update. The realtime mixer remains focused on playback, and `Audio.Update()` continues to handle crossfades and playlist progression.

### 3D rendering and baked lighting

- Lightmap and probe baking can spread work across available CPU cores while retaining deterministic output. A bake made on a laptop can therefore be reproduced on a larger machine, and incremental `BakeStep` work remains suitable for an interactive editor.
- Baked lighting no longer stops after sixteen lights. Scenes with many local lights are evaluated efficiently, and rectangular, spherical, and volume lights preserve their authored size and orientation so their soft lighting looks like the light you placed.
- Off-screen camera views can reuse compatible shadows from the main scene with `Canvas3D.SetRenderTargetShadowInherit(true)`. `ShadowSlotsCached` reports the work reused. `SetRenderTargetShadowCascadeLimit` lets a project reduce only a render target's cascade work without reducing the main view's quality.
- Render-target passes avoid work that belongs only to the main window frame. On Metal, GPU frame timing is now available for completed presented frames, making it easier to spot frame-cost regressions while tuning.
- Headless depth checks can skip color shading on the software renderer while retaining geometry and depth behavior, making non-visual scene validation less expensive.
- Invalid transforms and concurrent background rendering teardown are handled defensively, reducing the chance that a bad asset or shutdown race produces corrupted output or a crash.
- Particle trails, dense scenes, navigation, physics, animated scene loading, and the software renderer received correctness and efficiency fixes. In practice, fuller scenes spend less work on unchanged data and avoid several edge-case rendering and simulation failures.

### 3D assets and materials

- `Mesh3D.Mirror(mesh, skeleton)` creates a reflected copy of a mesh, including skinned influences and morph targets. Combined with `Animation3D.Mirror`, it produces a genuinely opposite-handed character instead of only mirroring the animation pose.
- `Material3D` supports projected decals: a texture can be projected onto a model for markings such as uniforms, signage, logos, or wear. The decal follows animated geometry and is lit with the material beneath it across supported renderers. Decals now feather across curved and two-sided surfaces instead of cutting off abruptly; `Mesh3D.VertexNormal` exposes the mesh normals needed by advanced decal-authoring tools.
- `Canvas.SetIcon(pixels)` and `Canvas3D.SetIcon(pixels)` let a running game provide its own application icon. macOS, Windows, and X11 install it directly; packaged Wayland applications are matched to their desktop-entry identity.

### Compiled code and runtime

- Several native-code correctness fixes address stale optimized values, nested calls, and AArch64 code generation. A class member inherited from another module now reliably wins over a same-named module global, so valid project code resolves to the member developers intended.
- Native executables are easier to investigate in `sample`, Instruments, and `perf`: function symbols are present by default and can be stripped explicitly for smaller builds.
- Object fields and returned runtime values now follow consistent lifetime rules. This closes leaks and premature releases that became visible in object-heavy or long-running applications.
- Strong reference cycles between class instances (a parent that owns a child that points back, a doubly linked list, a node held by a list it owns) are now reclaimed by `Zanna.Runtime.GC.Collect()`; `weak` is no longer required for correctness, only for immediacy. Collection stays explicit.
- String literals avoid repeated allocation in hot code, common handle checks are faster, and allocation failures are handled throughout more runtime services.
- `File.ReadAllText` now reports a changed file rather than returning a silently truncated snapshot. Child-process output can be consumed in bounded portions, keeping a chatty build or tool from monopolizing an application's memory or UI.

### Zanna Studio

- Project search moves file analysis off the interface thread and publishes virtualized results, making searches across large workspaces less likely to interrupt editing.
- Build and run output is read and displayed in bounded slices, so a verbose tool does not monopolize a frame. Workspace cursors can finish scanning projects larger than the previous inherited limit.
- File-watching, autosave, recovery, background work, and process cleanup report failure states more clearly and are bounded to preserve interface responsiveness. An external workspace change is therefore more likely to appear as an actionable state than as a missing result.

### Documentation

Public references, command help, platform guidance, runtime API material, and language examples were reconciled with the current compiler and runtime behavior. This includes corrected examples for IL and BASIC output, so the documented workflows better match what users see.

### Known limitations

- This is a pre-alpha source release. APIs, diagnostics, IL rules, project formats, and tooling may change before a stable release.
- Native macOS support targets Apple Silicon. Linux targets x86-64 and AArch64; backend and platform capability details remain documented in the supported-platform guides.
- OpenGL point shadows, one Metal counter, and software-renderer reflection/HDR settings remain unavailable as documented in the 3D guides.
- MP3 frames that select the reserved Huffman codebooks 4 or 14 are rejected as unsupported data; standard encoders do not emit them.

<!-- END DRAFT -->
