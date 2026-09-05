---
status: active
audience: public
last-verified: 2026-09-04
---

# Zanna Compiler Platform — Release Notes

> **Development Status:** Pre-Alpha. Zanna is under active development and not ready for production use.

## Version 0.3.1 — Pre-Alpha (DRAFT — unreleased)

<!-- DRAFT: release date TBD. Changes are measured from the v0.3.0-prealpha tag, cut 2026-08-28. -->

### What this release is about

v0.3.1 is a short follow-up release about making the things a game depends on behave reliably at scale. Music plays through real MP3 files and keeps playing through a long loading frame. Light baking uses the CPU available to it without changing the result, and it no longer quietly leaves part of a large or softly lit scene unbaked. Studio stays responsive while it searches or builds a large project.

The release also makes secondary camera views more practical. A picture-in-picture, security monitor, or live in-world display can reuse shadows that are still valid from the main camera, and can limit the shadow work it requests when that is the right trade-off. That gives projects more room for these views without taking time away from the main frame. Games can also present their own icon in the window and desktop shell, while projected decals hold together across the curved and animated surfaces they are meant to decorate.

Underneath, a broad correctness pass addressed failures that were most visible in long-running projects: optimized native programs can be profiled by function name, more compiler edge cases produce the intended program, resources are released consistently, and a file that changes while it is read reports an error rather than returning a partial result. The result is less time spent chasing intermittent failures and more trustworthy behavior when a project gets large.

The last work before the cut came from playing games built on the engine rather than from reading its code. A single-window game — 2D menus that hand the window to a 3D world — now keeps the mouse, the canvas size, and the overlay you draw in one coordinate space through fullscreen, resize, and the return of the window, so clicks land where you point instead of drifting further off the further you move from the corner. Alt-tabbing away no longer leaves a movement key or a mouse button held down, and it can no longer press a button in your UI on the way out. Inside the frame, meshes and post-processing chains have owners the renderer can verify, so a model you released cannot come back as stale geometry and two cameras cannot contaminate each other's motion history and exposure. Character and camera updates stopped allocating every frame, and looking around no longer speeds up with the frame rate. In Studio, typing in a split view stopped copying the whole document on every keystroke, and a multi-file edit interrupted by a crash now leaves a durable record of exactly what it had staged and replaced.

#### Highlights

- **MP3 music now plays complete, ordinary files (new).** Zanna's from-scratch decoder supports the MPEG Layer III variants used by common encoders, including MPEG-1, MPEG-2, MPEG-2.5, variable bit rate, short blocks, and joint stereo. Music that previously stopped almost immediately now plays through.
- **Your soundtrack survives a stalled frame (new).** Music streaming refills in the background, so loading assets or a long frame no longer drains the audio buffer into silence. `Audio.Update()` still advances crossfades and playlists as before.
- **A bake uses your CPU and remains repeatable (new).** Lightmaps and probes can use multiple cores while producing the same output regardless of worker count. Interactive baking retains its bounded work per update.
- **Large, softly lit scenes bake correctly.** The old sixteen-light ceiling is gone, local lights are evaluated more efficiently, and rectangle, sphere, and volume lights bake with their authored shape instead of as points.
- **Secondary camera views cost less (new).** Opt-in shadow reuse lets an off-screen `Canvas3D` keep shadows it can safely share with the main view; a cascade limit provides a further quality/performance choice for render-target views.
- **Projected decals arrive in 3D materials (new).** Add a number, logo, or scuff through a material projector and it stays with an animated surface while receiving the scene's lighting.
- **Your game can look like itself outside the frame (new).** A 2D or 3D canvas can set its application icon from `Pixels`, so a running game appears with its own identity in supported desktop shells rather than a generic application image.
- **A fullscreen single-window game agrees with your mouse (new).** When a 2D canvas hands its window to a 3D world, the pointer, the canvas's `Width`/`Height`, and the 2D overlay drawn over the frame are one space, and they stay one space through `Fullscreen()`, `Windowed()`, `Resize()`, and the return of the window. The scale skew that used to open up after the first fullscreen switch — small near the top-left corner, worse everywhere else — is gone on all three platforms.
- **Switching away from your game doesn't leave a key stuck down (new).** Losing window focus clears held keys, held mouse buttons, pending text, wheel motion, and click history, without synthesizing a release or a click. Alt-tab can no longer confirm a button on its way out, movement no longer latches on, and the next real click behaves normally.
- **Decals stay put on real geometry.** Projected markings now blend across curved surfaces and render consistently on two-sided materials, so logos, uniform numbers, signage, and wear do not disappear at a hard facing angle.
- **A mirrored character is fully mirrored (new).** `Mesh3D.Mirror` reflects skinned geometry, morph targets, and left/right bone influences, so an asymmetric prop moves to the matching hand when paired with mirrored animation.
- **A model you released can't come back as stale geometry.** Meshes, materials, scene nodes, and morph targets carry an identity the GPU caches check before reusing anything, so a long session of loading and unloading content cannot draw the model that used to live at that address.
- **Post-processing belongs to one camera at a time.** Sharing a `PostFX3D` chain between two canvases made each inherit the other's motion history and exposure — ghosting and brightness jumps whenever the views alternated. A second concurrent attachment is now refused with a recoverable error instead, and a chain reattaches cleanly once it is detached.
- **Frames cost less, and looking around is frame-rate independent.** Character movement, camera placement, and the third-person boom stopped allocating temporary vectors every frame, and look input is scaled by frame duration in one place — including gamepad stick look — so a camera tuned at 60 fps behaves the same at 144.
- **Profilers can name your functions (new).** Native executables include symbols by default, allowing macOS and Linux profiling tools to attribute work to your functions rather than anonymous addresses. Use `zanna build --strip-symbols` when a smaller executable matters more.
- **Studio stays responsive on a big project.** Search analysis runs in the background, results and build output arrive in small batches, and workspace indexing now reaches the end of very large projects.
- **A split view keeps up with your typing (new).** Two views of one document replay bounded edits instead of copying the whole file after every keystroke, so a split stays smooth on a large source file and language services keep their own view of the same edit history.
- **An interrupted multi-file edit leaves a trail you can follow (new).** A workspace-wide rename or replace records what it staged and replaced before it touches the first file, so a crash or power loss leaves an identifiable recovery record rather than unnamed temporary files. A successful edit still leaves nothing behind.
- **Long-running programs are more dependable.** The compiler, runtime, renderer, and native backends received fixes for optimized-code edge cases, resource lifetime, invalid input, and interrupted file reads.
- **A batch of things you can feel.** Dialogue reveals accented and non-Latin text by character instead of by byte, a dialogue bubble stays inside a small canvas, fullscreen works on monitor-sized framebuffers, particle trails and dense scenes hold up while the scene is changing under them, a Windows installer builds instead of failing to settle on its payload layout, and a macOS disk image is signed after the bundle reaches its final home.
- **Zanna's own validation runs faster.** Cost-aware test scheduling roughly halved the measured time for a representative parallel test run, helping contributors get feedback sooner without running fewer tests.

### By the Numbers

| Metric | v0.3.0 | v0.3.1 | Delta |
|---|---:|---:|---:|
| Commits | — | 20 | +20 |
| Source files | 3,704 | 3,717 | +13 |
| Production SLOC | 884K | 891K | +7K |
| Test SLOC | 332K | 336K | +4K |
| Zanna Studio SLOC | 160K | 162K | +2K |
| Demo SLOC | 242K | 242K | — |

Counts use `scripts/count_sloc.sh`, excluding blank lines and comments (production 890,841; test 336,269; Zanna Studio 161,594; source files 3,717). Commit count is measured from the `v0.3.0-prealpha` tag on 2026-08-28. Demo SLOC remains unchanged at the rounded total.

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
- A retained mesh carries an identity the renderer can trust. GPU geometry caches matched on a raw memory address, so a mesh or morph target released while the backend still held its cached upload could be mistaken for a new one that happened to reuse the same address — and the frame would draw the old geometry, tangents, or blend shapes. Meshes, materials, scene nodes, and morph targets now carry an allocation identity that must match before any cache entry is reused, on Metal, OpenGL, Direct3D 11, and the software renderer alike, and anything without a verifiable identity is uploaded fresh. A long session that loads and unloads content shows the model you loaded.
- A post-processing chain has one canvas owner. `PostFX3D` keeps temporal anti-aliasing history, the previous camera, auto-exposure adaptation, and its scratch buffers inside the chain, so attaching one chain to a main view and a picture-in-picture made each view inherit the other's motion and brightness — visible as ghosting and exposure jumps when the two alternated, and a data race when they rendered concurrently. `Canvas3D.SetPostFX` now refuses a second concurrent attachment, leaves both existing bindings untouched, and explains the refusal through `PostFX3D.LastError`; detaching, replacing, or finalizing a canvas resets the chain so it can be attached elsewhere with clean history. **If your project shares one chain across canvases, give each canvas its own chain.** Auto-exposure also adapts on the canvas's measured frame time now, so brightness settles at the same rate whatever the frame rate.
- Character and camera updates stopped allocating. First- and third-person movement, node-and-body synchronization, camera placement, and the third-person boom sweep each created temporary vectors and quaternions once or twice per frame just to move numbers between layers; they pass components directly now. No scripting API changed — there is simply less garbage behind an ordinary frame.
- Looking around is frame-rate independent. Controller look input is scaled by frame duration in one place, mouse deltas stay immediate, and gamepad stick look is integrated over the frame, so a camera tuned on one machine turns at the same speed on a faster or slower one.
- Dialogue reveals by character, not by byte. A typewriter reveal advanced through UTF-8 bytes and cut accented and non-Latin text into broken glyphs mid-reveal; it advances by Unicode codepoint now, and an anchored dialogue bubble stays inside a small canvas instead of running past its edge.
- Headless depth checks can skip color shading on the software renderer while retaining geometry and depth behavior, making non-visual scene validation less expensive.
- Invalid transforms and concurrent background rendering teardown are handled defensively, reducing the chance that a bad asset or shutdown race produces corrupted output or a crash.
- Particle trails, dense scenes, scene culling and spatial queries, draw submission, motion vectors, navigation and pathfinding, physics and convex hulls, animated scene loading, and the software renderer received correctness and efficiency fixes, with particular attention to scenes that change while they are being rendered. In practice, fuller scenes spend less work on unchanged data and avoid several edge-case rendering and simulation failures.

### 3D assets and materials

- `Mesh3D.Mirror(mesh, skeleton)` creates a reflected copy of a mesh, including skinned influences and morph targets. Combined with `Animation3D.Mirror`, it produces a genuinely opposite-handed character instead of only mirroring the animation pose.
- `Material3D` supports projected decals: a texture can be projected onto a model for markings such as uniforms, signage, logos, or wear. The decal follows animated geometry and is lit with the material beneath it across supported renderers. Decals now feather across curved and two-sided surfaces instead of cutting off abruptly; `Mesh3D.VertexNormal` exposes the mesh normals needed by advanced decal-authoring tools.
- `Canvas.SetIcon(pixels)` and `Canvas3D.SetIcon(pixels)` let a running game provide its own application icon. macOS, Windows, and X11 install it directly; packaged Wayland applications are matched to their desktop-entry identity.

### Windows, fullscreen, and input

- A single-window game keeps one coordinate space. When a 2D canvas hands its window to a 3D world — the pattern behind a game whose menus are 2D and whose gameplay is 3D — the mouse position, the 3D canvas's `Width`/`Height`, and the 2D overlay drawn over the frame all report in the window's public space, and that space carries the framebuffer's aspect ratio. Previously the 2D canvas kept pushing its own presentation scale onto the window it had loaned out, and because a scale change raises no resize event, the 3D canvas went on reporting one size while the pointer reported another: after the first `Fullscreen()` on a 1080p display, a canvas that believed it was 1920×1080 was being clicked in a 1280×720 space, anchored at the top-left corner so the error grew with distance from it. The lending canvas now leaves window presentation alone for as long as the window is on loan, and the borrowing canvas re-derives its extent every frame, so size, overlay, and pointer are read from one scale in one frame no matter which side changed the window. Mode changes still work from the 2D side while the world is live, and the original space is restored when the window comes back.
- Hit tests compare the mouse directly against the 3D canvas's own `Width`/`Height`. Letterboxing a fixed design space inside that extent is your game's decision, and the Game3D guide documents how. If your project compensated for the old skew with a scale factor of its own, remove it.
- Fullscreen works on monitor-sized framebuffers. The graphics layer's dimension ceiling was raised so a large or high-density display cannot have its fullscreen resize refused — which used to leave a windowed-size frame stretched under a monitor-size window, with the pointer reporting monitor coordinates.
- Losing focus cancels input instead of latching it. Both the 2D and 3D event pumps now clear held keys, held mouse buttons, per-frame press and release edges, pending text, wheel motion, and click history when the window loses focus — deliberately without emitting a release, click, or double-click. Alt-tabbing therefore cannot activate a control on the way out, a movement key cannot stay held while you are in another application, and the first press after you come back starts clean and clicks normally.

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
- A split view no longer pays for the whole document on every keystroke. Only one of two views of the same file owns the editable buffer; refreshing the other used to copy the complete text and rebuild its layout after each edit. The second view now replays bounded changes and falls back to a full refresh only when it has fallen too far behind, and switching focus between panes reuses the buffer that is already in sync instead of rebuilding it from a full string. Language services read the same edit history independently, so neither consumer starves the other.
- A multi-file edit that is interrupted can be identified and recovered. A workspace-wide rename or replace stages complete replacement files and keeps each original beside it while the batch commits. Before the first file is replaced, it now writes a durable journal recording every original, staged, and replaced path and whether the batch had committed, and updates it as the work proceeds. A crash or power cut therefore leaves an unambiguous record instead of anonymous temporary files, while a successful edit removes the journal and the backups and leaves nothing behind.
- File-watching, autosave, recovery, background work, terminal sessions, debugger dispatch, source scanning, and process cleanup report failure states more clearly and are bounded to preserve interface responsiveness. An external workspace change is therefore more likely to appear as an actionable state than as a missing result.
- Play-in-editor works in more macOS environments. When the sandbox refuses POSIX shared memory, the editor-to-game frame channel falls back to an exclusively created, owner-only mapping in the session's temporary directory, and a macOS session that exposes no screen reports a well-defined desktop size instead of zero, so window sizing stays sensible in restricted launch contexts.

### Packaging

- Building a Windows installer no longer fails to settle. For certain payload sizes the embedded overlay offset alternated between two values and the build gave up with a convergence error; the smaller image is padded to the offset the stub already recorded, so the installer builds.
- macOS disk images sign the application after it reaches the image, not before. The disk image's filesystem can normalize a Unicode resource name while the bundle is copied onto it, which invalidated a signature applied earlier; signing now happens once the bundle is in its final location, so a downloaded image mounts and launches with an intact signature.

### Documentation

Public references, command help, platform guidance, runtime API material, and language examples were reconciled with the current compiler and runtime behavior. This includes corrected examples for IL and BASIC output, so the documented workflows better match what users see. The Game3D guide now spells out the fullscreen single-window coordinate contract, including which side owns letterboxing.

### Known limitations

- This is a pre-alpha source release. APIs, diagnostics, IL rules, project formats, and tooling may change before a stable release.
- Native macOS support targets Apple Silicon. Linux targets x86-64 and AArch64; backend and platform capability details remain documented in the supported-platform guides.
- OpenGL point shadows, one Metal counter, and software-renderer reflection/HDR settings remain unavailable as documented in the 3D guides.
- MP3 frames that select the reserved Huffman codebooks 4 or 14 are rejected as unsupported data; standard encoders do not emit them.
- A `PostFX3D` chain can be attached to only one `Canvas3D` at a time. Projects that want the same post-processing on several canvases need one chain per canvas; the second attachment is refused through `PostFX3D.LastError` rather than trapping.
- In a fullscreen single-window game, the adopted 3D canvas reports the window's public extent rather than the 2D canvas's designed size. Fitting a fixed design space inside that extent, and any letterboxing it implies, is the application's responsibility.

<!-- END DRAFT -->
