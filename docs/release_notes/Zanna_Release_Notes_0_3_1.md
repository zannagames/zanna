---
status: active
audience: public
last-verified: 2026-09-06
---

# Zanna Compiler Platform — Release Notes

> **Development Status:** Pre-Alpha. Zanna is under active development and not ready for production use.

## Version 0.3.1 — Pre-Alpha (DRAFT — unreleased)

<!-- DRAFT: release date TBD. Changes are measured from the v0.3.0-prealpha tag, cut 2026-08-28. -->

### What this release is about

v0.3.1 makes a Zanna game more dependable when it grows beyond a prototype. Ordinary MP3 music plays through instead of stopping after its opening seconds, and keeps playing when a loading screen or a slow frame holds up the game loop. Large scenes bake their lighting with the CPU available to them without changing the result, and they no longer leave later lights or soft light shapes out of the finished image.

The release also gives 3D projects more useful ways to make and show content. Secondary cameras can reuse compatible main-view shadows, models can supply masks from any chosen model axis, and a mesh can be lofted from an authored height profile. Asset preparation can retain sensible levels of detail for a finished scene instead of requiring every piece to be drawn at its highest cost. A decal stays on the animated, curved, or two-sided surface it was projected onto, a mirrored character carries its skinning and morph detail to the other side, and a game can set its own application icon.

Lighting grew to the size of the scenes people are actually building. A light can be given the distance it reaches and keep the falloff it was authored with, a lightmap can carry only bounced and ambient light so baked surfaces sit under live fixtures without being lit twice, and a venue's worth of fixtures can cast shadows — the shadow budget nearly doubled, secondary shadow maps can be sized separately from the main ones, and repeated structural geometry is submitted to the shadow pass in batches. Lens flares add light to the frame instead of covering what is behind them, and translucent images and text keep clean edges on every backend.

Several things a player hits in the first minute are fixed. A game that switches a shared window between 2D menus and a 3D world keeps its cursor, overlay, and displayed size aligned through fullscreen and resize changes. Switching away from a game clears held input instead of leaving a character walking or accidentally activating a button. Long sessions that load and unload models keep showing the content that is actually loaded, and post-processing no longer lets one camera borrow another camera's exposure or motion history.

Zanna Studio is smoother on a large project and safer when work is interrupted. Search, build output, and workspace services stay out of the way of editing; split editors stay responsive while you type; Save All and background services report failures clearly; and an interrupted workspace-wide change leaves a recoverable record instead of an ambiguous collection of temporary files.

Native builds got more trustworthy. Three ways a correct program could be compiled into a wrong one are fixed, on both supported architectures, and Linux programs that failed to link natively against a current system toolchain link and run. In the language, an inherited field no longer loses to a module global that happens to share its name. In the runtime, an object releases the things it owns when it is dropped, reference cycles between class instances can be collected, and running out of memory returns a failure instead of ending the process.

#### Highlights

- **MP3 music plays complete, ordinary files (new).** Zanna's from-scratch decoder now supports the common MPEG Layer III variants used by everyday encoders, including variable bit rate, short blocks, joint stereo, MPEG-1, MPEG-2, and MPEG-2.5.
- **Your soundtrack survives a slow frame (new).** Music streaming refills in the background, so asset loading or an unusually long frame does not drain playback into silence.
- **A bake uses your CPU and remains repeatable (new).** Lightmaps and probes can use multiple cores while producing the same output regardless of worker count. Large scenes also keep all of their local lights, including rectangular, spherical, and volume lights with their authored shape.
- **Secondary camera views are more practical (new).** A picture-in-picture, security monitor, or in-world screen can reuse compatible shadows from the main view and choose a lower shadow cascade budget without lowering the main view's quality.
- **More of your authored model can drive the material (new).** `Mesh3D.RasterizeUvAxis` can turn X, Y, or Z position into a UV-space mask, useful for applying a material treatment to a model region such as a side panel, sleeve, or depth band. `Mesh3D.HeightLoft` creates shaped geometry from a defined height profile.
- **Detailed scenes stay manageable (new).** Offline asset preparation can retain constrained levels of detail for compatible static content, while the renderer keeps complete lighting results when a scene's light list is crowded. The result is a more graceful quality/performance trade-off for large environments.
- **A light reaches as far as you said it does (new).** `Light3D.Range` is honored for point and spot lights in live shading, clustering, and local shadows, so a fixture stops where you placed its boundary instead of leaking across the scene. An authored falloff coefficient below the old floor survives instead of being clamped, and the previous attenuation curve is preserved when no range is set.
- **A baked scene can sit under live lights (new).** `LightBaker3D.IncludeDirect` lets a lightmap carry sky, emissive, and bounced light only, so baked surfaces can be combined with live analytic fixtures without being lit twice. Probe baking is unchanged.
- **A venue's worth of fixtures can cast shadows (new).** The shadow budget rises from 12 slots to 20, `Canvas3D.SetShadowAtlasResolution` sizes the secondary maps independently of the main ones, and repeated static geometry is submitted to the shadow pass in instanced batches instead of one draw per copy. If the backend cannot prepare the slots you asked for, the frame reports the dropped requests rather than rendering something wrong.
- **Lens flares add light instead of hiding the scene (new).** Flare sprites compose additively from their texture's radial alpha and smoothed visibility, and additive draws preserve destination alpha on Metal, OpenGL, Direct3D 11, and the software renderer — which also cleans up the edges of translucent images and text on Metal.
- **Projected decals and mirrored characters hold together.** Decals follow curved, animated, and two-sided surfaces more reliably, and `Mesh3D.Mirror` reflects skinning and morph detail as well as the visible shape.
- **Your game can look like itself outside the frame (new).** A 2D or 3D canvas can set its application icon from `Pixels`, so supported desktop shells show the game's identity instead of a generic image.
- **Fullscreen single-window games agree with the mouse.** When 2D menus hand a window to a 3D world, the pointer, canvas size, and overlay stay in one coordinate space through fullscreen, resize, and return to the menu. Fullscreen also works on monitor-sized and high-density framebuffers.
- **Switching away from a game does not leave input stuck.** Losing focus clears held keys, mouse buttons, text, wheel movement, and click history without inventing a release or click. Alt-tab can no longer confirm a control on the way out.
- **A model you released cannot come back as stale geometry.** Long sessions that load and unload content keep drawing the model, material, and morph state currently in use rather than a retired resource that happened to occupy the same memory.
- **Post-processing belongs to one camera at a time.** A second concurrent use of the same temporal post-processing chain is refused with a clear recoverable error, preventing cross-camera ghosting and exposure jumps. Give each active `Canvas3D` its own chain.
- **Frames cost less, and controller look is frame-rate independent.** Common character and camera updates avoid per-frame temporary allocations, while gamepad look motion now behaves consistently on faster and slower machines.
- **Studio stays responsive and recoverable on a big project.** Search and output arrive in bounded batches, split views avoid full-document work on every keystroke, and an interrupted workspace-wide rename or replace leaves a durable recovery record.
- **Native builds compile your program, not a variation on it.** Three miscompiles are fixed: a value copy was discarded while a conditional branch still read it, an x86-64 division could have its result overwritten by a forwarded store, and an AArch64 large constant could be expanded through a scratch register that already held one of the instruction's own operands.
- **Natively linked Linux binaries build against current toolchains.** The bundled linker handles the full set of global-offset-table relocation forms modern compilers emit, so programs that previously failed to link on an up-to-date Linux distribution now link and run.
- **A long session stops leaking and stops guessing.** `Zanna.Runtime.GC.Collect()` reclaims strong reference cycles between class instances, an object releases its fields when it is dropped, and running out of memory raises a failure you can handle rather than ending the process. `File.ReadAllText` reports a file that grew while it was being read instead of handing back the prefix it managed to get.
- **Dialogue and particle trails look right.** Typewriter dialogue reveals accented and non-Latin text one character at a time instead of one byte at a time, and a dialogue bubble stays inside a small canvas. Particle trails sort correctly against each other, and a short trail fades over its real length instead of vanishing early.

### By the Numbers

| Metric | v0.3.0 | v0.3.1 | Delta |
|---|---:|---:|---:|
| Commits | — | 25 | +25 |
| Source files | 3,704 | 3,717 | +13 |
| Production SLOC | 884K | 892K | +8K |
| Test SLOC | 332K | 338K | +6K |
| Zanna Studio SLOC | 160K | 162K | +2K |
| Demo SLOC | 242K | 242K | — |

Counts use `scripts/count_sloc.sh`, excluding blank lines and comments (production 892,091; test 337,830; Zanna Studio 161,594; source files 3,717). Commit count is measured from the `v0.3.0-prealpha` tag on 2026-08-28. Demo SLOC remains the rounded combined total reported for v0.3.0 — 74,061 in this repository plus 168,440 in the `zannademos` repository.

---

The rest of this document is the detail, area by area.

### Audio

- Imported MP3 music now supports the normal Layer III files made by common tools: mono, stereo, joint stereo, constant and variable bit rates, and the MPEG-1, MPEG-2, and MPEG-2.5 families.
- Music refills on a background worker. A loading screen, asset batch, or long frame no longer makes a playing track fall silent; `Audio.Update()` still handles crossfades and playlist progression.

### 3D rendering and baked lighting

- Lightmap and probe baking can use available CPU cores while retaining deterministic output. It remains suitable for incremental editor work, and scenes with many local lights no longer stop contributing after the first sixteen.
- Render-target views can inherit compatible main-camera shadows with `Canvas3D.SetRenderTargetShadowInherit(true)` and can independently limit their cascades when a cheaper secondary view is the better choice.
- The software renderer computes depth the same way the GPU backends do, so thin geometry and terrain stop flickering or dropping out at grazing angles, and it now applies the same clustered-light selection, so a software frame is lit like a GPU frame. When a scene's light index pool fills, the lights that overflowed still light the scene instead of being dropped.
- A mesh, material, scene node, or morph target that you released cannot be drawn again when a newly loaded resource lands at the same address — a level change no longer paints the previous level's geometry into the new one. And a temporal post-processing chain belongs to one canvas, so a picture-in-picture view cannot inherit the main camera's exposure and motion history.
- Character movement, camera placement, and third-person camera follow stop allocating temporary objects on every frame, so a long play session builds up far less garbage. Mouse look stays immediate; gamepad-stick look is integrated by frame duration, so a fast machine no longer turns the camera faster than a slow one.
- In a large world, objects are converted to camera-relative coordinates before culling rather than after, so distant geometry stops being culled away while it is still on screen, and a frame that overflows its draw budget reclaims those slots instead of losing them for the rest of the session. Continuous-collision candidates keep a deterministic full-scan fallback, a navigation mesh built from overlapping source geometry no longer duplicates triangles, and an FBX file is read from memory instead of a temporary file on disk.
- Dialogue reveals by Unicode character rather than by byte, so accented and non-Latin text appears one glyph at a time, and an anchored dialogue bubble stays inside a small canvas. Particle trails are sorted against one another and a short trail fades over its actual length.

#### Lighting at venue scale

- `Light3D.Range` now bounds point and spot lights everywhere they are used — live shading, clustering, and local shadow projection — using the same smooth cutoff the light baker already applied, so a fixture's reach in the final frame matches the one you authored. A range of zero keeps the previous attenuation curve, so existing scenes are unchanged.
- An authored attenuation coefficient is kept as written. The fallback used when no coefficient is given is separate from the numerical minimum now, so a value below the old `0.001` default is honored instead of being silently clamped. GPU area-light decay and the glTF and FBX range conversions use the same normalization.
- `LightBaker3D.IncludeDirect` (default true) can be set false to bake a lightmap that omits direct illumination at the surface it lands on while keeping sky, emissive, and bounced energy. That makes a baked atlas usable underneath live analytic fixtures — a stadium lit by real lights over pre-lit structure — without double-lighting the result. Probe-grid baking is unaffected.
- Lens flares compose additively, from the flare texture's radial alpha and a smoothed visibility term, so a flare brightens the scene behind it instead of masking it. Additive draws preserve destination alpha on Metal, OpenGL, Direct3D 11, and the software renderer, and Metal's premultiplied overlay composites as source-over — which also fixes fringing on translucent images and text.
- General shadow capacity rises from 12 slots to 20. Slots four and above can use their own tile size through `Canvas3D.SetShadowAtlasResolution(pixels)` — 64 to 4096, or zero to inherit the `EnableShadows` resolution — with `ShadowAtlasResolution` reading it back, so a 4096 primary map can sit alongside cheaper 1024 secondaries. The backend is told the required slot extent and both resolutions before any tile renders; if it cannot prepare them, the frame is left unshadowed and the dropped requests are reported rather than producing a half-correct picture. On OpenGL, primary and secondary shadow storage are separate, so the smaller secondary resolution is actually honored.
- Repeated static geometry — the same fixture, railing, or seating module placed many times — is grouped by exact draw state and submitted to the depth pass as instanced draws instead of one call per copy. Software, deformed, and particle casters keep per-mesh submission.

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

- An inherited field wins over a module global that shares its name, in both reads and assignments, so a class member is the one you get. The optimizer no longer reuses a loaded value across a write that another path can reach, which used to hand a stale value to code that ran after it.
- Native executables carry their function and object symbols by default, so a profiler or debugger shows names instead of addresses; `zanna build --strip-symbols` is there when size matters more.
- Three ways a valid program could be miscompiled are fixed. A copy could be eliminated while a use of its destination sat behind an in-block conditional branch — the kind emitted by an overflow trap or a zero test — leaving that branch reading an undefined value. On x86-64, a stored value could be forwarded into a later load across a division, silently overwriting the registers the division defines. On AArch64, expanding a wide constant could choose a scratch register that already held one of the instruction's own operands, and the instruction scheduler did not account for those expansions writing scratch at all.
- The bundled native linker keeps up with current system compilers on Linux. Every global-offset-table relocation form in the ABI is relaxed rather than only the `mov` form, plain references to symbols defined in the same link get link-time slots synthesized for them, and one more C-library symbol is classified correctly — so programs that failed to link natively on an up-to-date distribution now link and run.
- A program no longer traps at runtime on a path that is actually fine. The optimizer used to keep a provisional trap it had decided on early, even after the values feeding that operation turned out not to be what it assumed; the trap is withdrawn now.
- An object releases what it owns when it is destroyed: a class with fields holding strings, sequences, or other objects gets a destructor that frees them, base destructors run in order without double-releasing, and runtime-owned objects release their fields on their final drop. Runtime calls declare whether the value they hand back is yours to keep or borrowed, so a returned string or sequence is neither leaked nor freed twice. `Zanna.Runtime.GC.Collect()` also reclaims strong reference cycles between class instances, which reference counting alone could never free.
- A string literal used in a loop is created once and reused instead of rebuilt on every pass, and a runtime handle used repeatedly is validated against a recent-use cache, so hot loops spend less time on bookkeeping. On macOS, traps and finalizer recovery no longer pay a signal-mask system call each time.
- Running out of memory is reported as a failure you can act on across collections, files, networking, threading, and media, rather than ending the process. `File.ReadAllText` reports a file that grew while it was being read instead of returning the prefix it got.

### Zanna Studio

- Project search runs on background file workers and publishes results as virtualized rows, so a search across a large workspace no longer freezes the editor while it works.
- A long-running build or test tool cannot fill memory with its own output: incremental stdout and stderr publication is capped, and the process's environment is captured when it starts rather than re-read later.
- When the file watcher cannot watch a directory it says so and falls back to polling, instead of silently missing external edits, and autosave, crash recovery, performance monitoring, and the debugger share one background budget so they do not compete with typing.
- Split editors replay small changes rather than copying and relaying the whole document after every keystroke, and language services follow the same edit history independently.
- Workspace-wide rename and replace operations retain a durable recovery journal until they complete. If a crash or power interruption occurs, the staged and replaced files can be identified and recovered; successful edits leave no journal behind.
- Save All reports the files it could not write instead of stopping quietly, a workspace index scan walks the whole workspace rather than stopping at its paging limit, and embedded play falls back safely in restricted macOS environments.

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
- The expanded shadow capacity, the secondary shadow atlas resolution, and instanced shadow submission are verified on native Metal and the software renderer. Acceptance on native OpenGL and Direct3D 11 is still pending.
- `Canvas3D.SetShadowAtlasResolution` changes the size of secondary shadow tiles; it does not raise the backend's shadow-slot capacity.

<!-- END DRAFT -->
