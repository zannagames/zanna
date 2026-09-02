---
status: active
audience: public
last-verified: 2026-09-01
---

# Zanna Compiler Platform — Release Notes

> **Development Status:** Pre-Alpha. Zanna is under active development and not ready for production use.

## Version 0.3.1 — Pre-Alpha (DRAFT — unreleased)

<!-- DRAFT: release date TBD. v0.3.0 was cut on 2026-08-28. -->

### What this release is about

v0.3.1 is a short release, and it is almost entirely about things that were supposed to already work.

The clearest case is music. Zanna decodes MP3 itself, and that decoder had never played a real song. v0.3.0 fixed four genuine defects in it, but more than two-thirds of the format's Huffman codebooks simply did not exist, so a track from any ordinary encoder stopped a fraction of a second in. The decoder is the complete ISO Layer III codec now — every codebook, MPEG-1, MPEG-2 and MPEG-2.5, joint stereo, short blocks, the bit reservoir, CBR and VBR — and it is checked against two independent decoders at roughly 80 dB, which is the 16-bit rounding floor. Alongside it, music stopped depending on your frame loop: a background streamer thread keeps every playing stream fed, so a loading screen, an asset batch, or one long frame no longer drops your soundtrack into silence while `IsPlaying()` still reports true.

Baked lighting has the same shape of story. A bake spreads across your CPU cores now and still produces byte-identical output — the same lightmap and the same probe coefficients no matter how many workers ran — and building that surfaced two correctness bugs sitting underneath it: the baker silently ignored every light past the sixteenth in a scene, and it shaded rectangle, sphere and volume lights as though they were points.

Secondary camera views are cheaper, too. A picture-in-picture, security camera, or other off-screen `Canvas3D` view can now reuse the shadows that remain correct from the main view while keeping its own sun shadows accurate. That makes a live inset substantially less likely to compete with the main frame for GPU time, and gives you a counter to see how much shadow work was reused.

Under that, the compiler and the runtime were audited against a full-size game. Several native miscompiles were fixed, and so was a Zia name-resolution bug that made an inherited field resolve to another module's global of the same name. Executables carry a symbol table by default now, so `sample`, Instruments and `perf` can tell you which of your functions is slow instead of pointing at an address. And the pieces of the runtime a long-running program leans on — string literals, traps, handle validation, whole-file reads, child-process output — were made cheaper, or made honest about failing.

Zanna Studio's remaining scale seams were closed. Project search runs off the UI thread, results publish as virtual rows rather than rebuilding the visible list, build output is consumed in bounded slices, and workspace indexing reaches the end of a large project instead of stopping at a ceiling left over from an older scanner.

Building Zanna itself got about twice as fast — not by running fewer tests, but by letting CTest keep what it learns about how long they take.

#### Highlights

- **MP3 music plays (new).** The from-scratch Layer III decoder is complete: all 32 Huffman pair tables and both count1 tables, MPEG-1 with `scfsi`, MPEG-2 LSF and MPEG-2.5, mono, stereo, dual and joint (M/S and intensity) stereo, long, short, start, stop and mixed blocks, CBR and VBR, and the bit reservoir. Independent decoders agree with it at 75–82 dB across every stream tested, an rms error of about 0.7 LSB. Before this, a normal encode stopped after its first silent frame.
- **Your soundtrack survives a stalled frame (new).** A background streamer thread refills every playing music stream, so a `Sound.Load`, an asset batch, or a long frame no longer drains the half-second decode buffer into silence. The realtime mixer still only consumes buffers — it never touches the disk or the codec — and `Audio.Update()` remains what advances crossfades and playlist auto-advance.
- **A bake uses your whole CPU and comes out identical.** Lightmap and probe work is partitioned across a baker-owned thread pool and reduced back in fixed sample order, so worker count and completion order cannot change a single floating-point result. A single-core host, an allocation failure, or a call from inside a worker falls back to the serial path, and `BakeStep` keeps its bounded per-call budget so an interactive bake still yields.
- **A scene can have more than sixteen lights.** The baker copied at most sixteen lights and silently dropped the rest, so a level lit past that ceiling baked with an arbitrary subset of its lighting. The cap is gone, and finite local lights are indexed spatially so a shading point only queries the lights that can reach it.
- **Area lights bake as area lights.** Rectangle, sphere and volume lights carry their size, radius and orientation into the bake instead of collapsing to a point at their origin, so a soft key light bakes with the shape you authored.
- **Picture-in-picture views waste less shadow work (new).** Opt in with `Canvas3D.SetRenderTargetShadowInherit(true)` and an off-screen camera keeps the shadows that are safe to share from the main view while rendering its own camera-dependent sun shadows. `Canvas3D.ShadowSlotsCached` shows how many shadow slots were reused, so a live inset or monitor view is easier to tune.
- **A mirrored character is a real mirrored character (new).** `Mesh3D.Mirror(mesh, skeleton)` returns a reflected copy of a skinned mesh — geometry mirrored across the sagittal plane with winding reversed and tangent handedness corrected, morph targets mirrored, and every bone influence remapped to its left/right partner. Paired with `Animation3D.Mirror`, a right-handed character with a prop baked into one hand becomes a left-handed one holding it in the other hand, rather than a mirrored performance on an unmirrored body.
- **A profiler can name your functions (new).** Native executables carry a full, address-sorted local symbol table — Mach-O `LC_SYMTAB` entries, ELF `.symtab`/`.strtab` — so `sample`, Instruments and `perf` attribute time to your Zia and runtime functions by name. The loader never reads those entries, so it costs nothing at run time; `zanna build --strip-symbols` opts out.
- **A batch of miscompiles, gone.** Global value numbering no longer reuses a load across a write that a path can reach or across a cyclic clobber, and AArch64 was corrected for constant invalidation, parallel call moves, forwarded boolean masking, cross-block null guards, division strength reduction, exception lowering and peephole bookkeeping. Native exception lowering stays aligned between x86-64 and AArch64.
- **An inherited field beats another module's global.** A derived class whose base lived in a different module resolved an inherited field to a bound module's exported global of the same name — typed `Any` — so `world_.Foo()` failed to compile with "Type 'Any' has no member" while the lowerer happily stored into the field. A class field, declared or inherited, shadows every module-level symbol now, exactly as a declared field always did.
- **Studio stays responsive on a big project.** Search reads, hashes, matches and compiler-scans files on background workers instead of the GUI thread, publishes results as virtual rows rather than rebuilding the whole visible list, and consumes build output in bounded slices. Workspace index cursors run to completion instead of stopping at an inherited 100,000-entry ceiling that quietly omitted files from large workspaces.
- **A file that changes while you read it is an error, not a truncated string.** `File.ReadAllText` traps rather than handing back the prefix it happened to see when the file grew mid-read, matching what its bounded sibling already did.
- **Building Zanna takes about half as long.** The same 2,004-test `-j10` run measured 554.43 seconds when scheduling history was being discarded and 291.08 seconds once CTest could keep its learned costs and start the long serialized lanes first.

### By the Numbers

| Metric | v0.3.0 | v0.3.1 | Delta |
|---|---|---|---|
| Commits | — | 10 | +10 |
| Source files | 3,704 | 3,709 | +5 |
| Production SLOC | 884K | 887K | +3K |
| Test SLOC | 332K | 334K | +2K |
| Zanna Studio SLOC | 160K | 161K | +1K |
| Demo SLOC | 242K | 242K | — |

Counts via `scripts/count_sloc.sh`, which excludes blank lines and comments (production 886,997 / test 334,170 / zannastudio 160,878 / source files 3,709). Commits are measured from the `v0.3.0-prealpha` tag (2026-08-28) and include the release-link commit. Demo SLOC is the sum of both homes — 73,444 in this repo plus 168,440 in the `zannademos` repository — and is unchanged at the rounded total because the demo repository took no commits during this release. The whole release is 356 files changed, +14,069 / −3,405 lines.

---

The rest of this document will provide the detail, area by area.

### Audio

- The MP3 decoder is finished. Layer III support was partial in a way that no amount of fixing around the edges could reach: only codebooks 0–3, 5 and 6 existed, and even those three trees did not match the standard's tables; 7–13 and 15–31 fell back to a bit-width approximation that aborted the stream. Every real encode therefore stopped right after its silent header frame. The decoder now generates every Huffman tree from the ISO code tables and proves each one is a complete prefix code over exactly the standard's value square, and it implements MPEG-1 `scfsi` reuse, the MPEG-2 and MPEG-2.5 LSF scalefactor partitions, short-block reorder, correct subblock gain, final-band requantization, and bit-reservoir-safe count1 decoding with the standard's overrun rollback. Joint stereo (M/S and intensity) is applied before hybrid synthesis rather than after, odd-subband frequency inversion is restored, and the IMDCT and polyphase synthesis matrixing and window signs were corrected — that last one alone had been holding broadband output to about 13 dB even when everything above it was right. A frame that selects a reserved codebook (4 or 14) is refused as unsupported data.
- The decoder is verified, not just tested. Synthetic tone signals were encoded with LAME across MPEG-1 48/44.1/32 kHz in stereo, joint stereo, mono and VBR, MPEG-2 at 24/22.05/16 kHz and MPEG-2.5 at 12/11.025/8 kHz, plus a real 200-second track, then decoded by two independent decoders that agree with each other at 86–106 dB. Zanna's output matches them at 75.6–82.2 dB — an rms error of roughly 0.7 LSB, which is the 16-bit rounding floor. Four of those streams are embedded as regression fixtures, and the method for re-verifying after any future decoder change is written down in `docs/internals/mp3-decoder-conformance.md`.
- Streaming music no longer depends on the application thread. A `Music` stream holds only about half a second of decoded audio ahead of the mixer, and every refill used to happen inside `Audio.Update()`. Any stall longer than that ring — a loading screen's asset batch, a long frame, or a `Sound.Load` decoding a whole MP3 while holding the lock that `Update` needs — produced silence while `IsPlaying()` kept reporting true. One configurable low-duty streamer thread per audio context now performs the same bounded refill pass, with POSIX and Windows entry points, a non-fatal fallback to the old `Update`-only behavior if it cannot start, and a prompt join before platform shutdown. The realtime mixer stays consume-only: it never performs file I/O or codec work. Governed by ADR 0307.
- Refill acquisition is lock-correct. Play, stop, seek, free and detach previously waited, then relocked and refilled, which let the mixer observe a buffer mid-change; they take an atomic lock-no-refill path now, and slot refill claims are retained across forced buffer clearing so torn buffer state cannot be seen.
- Two contracts are written down rather than inferred. `Audio.Update()` is what advances crossfades and playlist auto-advance — streaming refills survive without it — and `Music.Play(loop)`'s argument is a loop flag, not a volume; set `Volume` before `Play`.

### 3D rendering and baked lighting

- Off-screen camera views can inherit reusable shadows. `Canvas3D.SetRenderTargetShadowInherit(true)` is designed for a live picture-in-picture or monitor view: it avoids repeating shadow work that can safely be shared while keeping the view's own sun shadows accurate. `ShadowSlotsCached` shows the slots served without another render, so the savings are visible while you tune. If reuse is not available, Zanna automatically renders the complete shadow pass.
- A bake is parallel and still deterministic. Each lightmap path derives its seed from immutable triangle, texel and sample coordinates rather than from a stream that advances across samples, so disjoint sample ranges can be evaluated on a baker-owned thread pool and reduced on the caller in sample-index order. Worker count and completion order cannot alter floating-point accumulation, so the same scene bakes to the same atlas on a four-core laptop and a thirty-two-core workstation. Probe grids partition the same way, with the deterministic breadth-first invalid-probe fill staying serial after the barrier. A single-core host, a pool-creation failure, or a call made from inside a pool worker transparently uses the serial implementation, and the pool is joined and released when the baker is finalized. `BakeStep` keeps its fixed 1,024-path budget, so an incremental bake still returns to your frame. Governed by ADR 0308.
- The sixteen-light cap is gone. `LightBaker3D.AddLight` returned without a word once sixteen lights had been added, so a level lit past that point baked with whichever lights happened to be registered first — and nothing reported it. The light snapshot grows on demand instead, and an allocation failure traps rather than dropping a light.
- Local lights are indexed instead of scanned. Lights with finite bounds go into a deterministic AABB hierarchy that a shading point traverses; directional and effectively unbounded lights stay in a compact global list that is always evaluated. A scene with many small lights no longer pays for all of them at every sample.
- Area lights bake with their shape. Rectangle, sphere and volume lights carry width, height, radius and their orientation basis through the bake snapshot, and shading uses the closest point on the emitter rather than its origin, so an authored soft light bakes soft instead of behaving like a point at its center.
- OpenGL rejects transforms it cannot invert. The shared OpenGL paths gained a finite, scale-aware Gauss-Jordan 4×4 inverse, and the deferred and render-pass consumers refuse a non-finite or singular transform instead of drawing through whatever the old routine produced.
- Animation blending, FBX constraints, navigation, physics, mesh publication and render-path handling were hardened against the revised baking and transform contracts.
- Concurrent teardown of the internal worker-to-main-thread commit queue is explicit. `Free` used to close and immediately reclaim the wrapper, so a producer still holding the raw handle could begin an enqueue into memory being freed. Teardown is two-phase now — close while producers may still hold the handle, stop or join them, then free — with close idempotent, queued ownership preserved, and post-close enqueues failing without taking their payload. Governed by ADR 0308.
- `Material3D.SetDecalMap` / `SetDecalProjector` / `SetDecalOpacity` (ADR 0312): a projected
  decal layer composited over the albedo before lighting through a model-space (pre-skin)
  box projector, so a number, logo or scuff rides a skinned surface and is lit, shadowed
  and normal-mapped like the cloth it sits on. Identical on Metal, D3D11, OpenGL and the
  software rasterizer; runtime-only (not persisted into VSCN).
- `Canvas3D.SetDepthOnlyShading` / `Canvas3D.DepthOnlyShading` (ADR 0311): headless
  verification probes whose gates never read pixels can ask the software backend to
  keep the vertex stage, depth test and opaque depth writes but skip fragment shading
  and colour writes. Draw, culling and hitch counters are unchanged; GPU backends
  ignore the flag; capture paths clear it before reading pixels.

### 3D assets

- `Mesh3D.Mirror(mesh, skeleton)` returns a new mesh mirrored across the sagittal plane, registered beside `Transform` as both a function and a method. Positions, normals and tangents are reflected with winding reversed and tangent `w` negated; morph target deltas are mirrored with their names and weights preserved; and every bone influence — the four vertex slots and the 5–8 side stream — is remapped to its sagittal partner through the mesh's palette, using the same resolution order `Animation3D.Mirror` uses: exact name side-token swap, then humanoid role side flip, then self. `Animation3D.Mirror` alone gave you a left-handed *performance*; a character with an asymmetric prop baked into its geometry, such as a fielder whose glove lives in the left-hand mesh, still swung the gloved hand. Now both halves mirror. The `skeleton` argument may be null, in which case the mesh's attached skeleton is used, and a handle that is not a `Skeleton3D` yields null rather than trapping. A mesh with no bone weights mirrors as plain geometry, LOD levels are mirrored per level by the caller, and the source mesh is never modified. Governed by ADR 0306.

### Compiled code and the native toolchain

- An inherited field shadows a bound module's global. A derived class whose base lived in another module resolved an inherited field name to that module's exported global instead of to the field. Because such a global is typed `Any`, reading `world_.Foo()` failed to compile with "Type 'Any' has no member" while the lowerer went on storing into the field — a diagnostic that pointed nowhere near the cause. A class field, declared or inherited, now shadows every module-level symbol in both reads and assignments, matching what a declared field already did through the class scope.
- Global value numbering stopped reusing stale loads. A load could be replaced by an earlier one across a write that some path reached, and across a clobber inside a cycle, so optimized code read a value the program had already overwritten. Both cases are blocked now.
- AArch64 code generation was corrected in seven places: constant invalidation, parallel moves at call boundaries, forwarded boolean masking, cross-block null guards, division strength reduction, exception lowering and peephole bookkeeping. Native exception lowering is kept aligned between x86-64 and AArch64 rather than drifting apart.
- Valid nested calls inside loops no longer raise a false ownership error after compilation optimizations.
- Native executables are profilable by default. Mach-O images carry address-sorted `N_SECT` local symbols in `LC_SYMTAB` with correct symbol partitions and section ordinals; ELF images gained `.symtab` and `.strtab`. `sample`, Instruments and `perf` therefore attribute samples to your functions by name. The dynamic loader never reads these entries, so stripping changes nothing at run time — `zanna build --strip-symbols` writes only the entry point and imports if you want a smaller binary. Linker offset, section, relocation and emitted-image validation were strengthened alongside, with focused regression coverage.
- A miscompile can be bisected without rebuilding the compiler. `ZANNA_IL_OPT_KEEP_FUNCS=<file>` restores every IL function *not* named in the file to its pre-pipeline body after the optimizer runs, so with a program-level oracle — VM output versus native output — you can find which optimized function changes the answer in log₂(N) builds. A family of AArch64 `ZANNA_NO_*` switches skips one pipeline stage or one peephole sub-stage each for the same purpose. None of them are consulted at `-O0`, and they are documented in `docs/internals/backend.md`.
- The in-house Mach-O linker lowers `SUBTRACTOR` + `UNSIGNED` relocation pairs into
  signed symbol differences (32- and 64-bit) with no rebase or bind bookkeeping, so
  optimized C objects carrying `__eh_frame` label differences, jump tables and C++
  typeinfo tables now link. A lone `SUBTRACTOR` or an undefined subtrahend is still a
  hard diagnostic. The macOS import planner also knows `__sincos_stret`.
- Every class releases its reference fields when it dies (ADR 0313). A class without
  `deinit` used to get no destructor at all, so its strings, objects, collections and
  runtime handles leaked on every instance death; a ten-line object-churn loop grew
  without bound on the VM and in native binaries, and a game that rebuilt its 3D stage
  per shot capture reached 14.8 GB. The lowerer now synthesizes `<Type>.__dtor` for any
  class with a releasable field (own or inherited); a derived destructor releases only its
  own fields and chains to `Base.__dtor`, so the base `deinit` body finally runs for
  derived instances and nothing is released twice. The dispatcher is a binary search over
  class ids instead of a linear chain.

### Runtime

- Every runtime function that returns a managed reference now declares who owns the result
  (ADR 0314). The compiler used to guess from names: a result was owned only when the symbol
  ended in `_new`/`.New`/`.Clone` or contained `.From`, so `Mesh3D.Box`, `Material3D.PBR`,
  `World3D.WithCamera`, every loader, retarget and screenshot — 502 functions — were retained on
  store and never released, while `Entity3D.DetachFromBone` matched `.From` and had a reference it
  never owned released. String results had the mirror bug: `Result.UnwrapStr`, `Option.UnwrapStr`,
  `Lazy.GetStr` and the clip-name accessors return a string another object owns, and the compiler
  released them anyway. `runtime.def` rows returning `obj`, `seq` or `str` now carry `owned` or
  `borrowed`; `rtgen` refuses a row without it; the manifest's `ownership` field reports it. The
  Option/Result/Lazy combinators, `ConcurrentMap.GetOr`, `Seq.Fold`, `Parallel.Reduce` and the GUI
  sub-handle wrappers hand back one caller-owned reference on every path instead of a fresh object
  on one path and a borrowed one on another.
- Objects freed by the runtime run their Zia destructor (ADR 0313). A list element, a map
  value or a boxed `Any` whose last reference the runtime dropped used to be reclaimed by
  C code with no knowledge of `deinit`, so its fields leaked and its body never ran. The
  compiler now installs one program-wide hook, `rt_obj_set_class_dtor_hook(@__zia_dtor_dispatch)`,
  as the first statement of the entry point; `rt_obj_free` invokes it for every payload with
  a positive class id before any per-object finalizer. Native binaries pass the real
  address; the bytecode and tree-walking VMs bridge the call to a re-entrant trampoline
  that only runs on the owning module's thread. Compiled release sites no longer call the
  dispatcher themselves, so there is exactly one destruction path.
- A string literal costs one allocation for the life of the process. `rt_str_from_lit()` — what native code calls for every evaluation of a string literal — returns one immortal string per literal site, created on first use and cached by the literal's address, so a literal inside a hot loop allocates nothing and its generated retain/release are no-ops. Empty-string and concatenation ownership contracts are preserved, and the VM keeps its own per-module literal cache.
- Traps are cheaper on macOS. The Darwin trap and finalizer-recovery paths use mask-free `setjmp`/`longjmp` adapters, removing the signal-mask work that the default variants perform on every save and restore.
- Handle validation got a fast path. Heap and string handle checks accelerate on recently seen entries, and the reference VM can validate module-variable and tracked raw-allocation ranges without slowing ordinary native or bytecode execution.
- Allocation failure is handled rather than assumed away across collections, I/O, networking, threading, OOP, media and runtime services.
- `File.ReadAllText` refuses a torn read. A file that grows between the size query and the last read used to come back as a silent prefix; it traps with "file changed while reading" now, matching the bounded `ReadAllTextBounded` sibling. A caller asking for a whole-file snapshot either receives every observed byte or an error.
- `ProcessHandle.ReadOutputResultBounded(maxBytes, maxChunks)` reads an ordered prefix of a child's captured output — `{ chunks, truncated, emittedBytes, remainingBytes, hasMore }` — and leaves the rest queued natively, so a noisy build can be drained across frames instead of materializing up to 16 MiB of managed strings and maps in one call. Nonpositive arguments select 64 KiB and 64 chunks; requests are capped at 16 MiB and 4,096 chunks. The existing `ReadOutputResult` keeps its consume-everything behavior. Governed by ADR 0304.
- Launching a process owns its environment. On POSIX, inherited and overlay launches borrowed `environ` pointers after releasing synchronization, so a concurrent `Environment.SetVariable` could replace that storage during executable lookup or `posix_spawn`. Every inherited entry, and the PATH fallback, is copied under the runtime's private environment lock now, and only owned snapshots are used after the lock is released. Governed by ADR 0304.

### Zanna Studio

- Project search moved off the UI thread. Search bounded files and bytes per frame, but each selected file was still read, hashed, split, matched or compiler-scanned and fully rendered on the GUI thread — and file counts do not bound a pathological pattern. Analysis runs on background file workers now, results publish as virtualized tool-result rows without rebuilding the full visible projection, and the workspace-symbol commands' synchronous whole-workspace fallback is gone. Search controller state, publication and file-command results were split so UI ownership stays explicit. Governed by ADR 0305.
- Workspace indexing reaches the end. Explicitly owned index cursors inherited the legacy stateless scanner's 100,000-entry aggregate ceiling, so a fallback scan of a large workspace could stop before reporting `done` — silently omitting files — even though it already paged under a bounded work budget. Owned cursors traverse to completion while stateless materialized enumeration stays bounded, and paging, generation checks and truncation reporting are unchanged. Governed by ADR 0303.
- Build and run output is published in slices. Studio consumes at most 64 KiB and 64 chunks per update through the new bounded read, keeps polling while there is more, and delays job completion until the queue is empty, so a chatty build cannot monopolize a frame.
- Watcher behavior is legible when it degrades. Event coalescing moved into a focused module, known watched paths are recognized, and a fallback to degraded watching is reported rather than looking like normal operation — while refresh and retry behavior is preserved. Watcher and SafeIO failures now carry structured detail through workspace, completion, navigation, save, recovery and language-tool flows, so a transient workspace change is something you can act on instead of an unexplained empty result.
- Background work shares one reviewed budget. Performance monitoring, recovery, autosave, the debugger and workspace work draw on a single set of documented limits — per-frame metadata probes, a language-tooling frame allowance, watcher directory and event caps — chosen to protect input and rendering ahead of throughput. Process reaping ownership and diagnostics were strengthened while keeping cancellation and deferred cleanup.

### Building and testing Zanna

- The test suite runs about twice as fast. The canonical Unix and Windows validation scripts deleted the build tree's `Testing` directory before every run, throwing away the weighted runtime estimates CTest uses to schedule parallel jobs, and a cache-level `CTEST_COST_DATA_FILE` override claimed to disable that history while command-line CTest generated it anyway. History is retained now, and the same 2,004-test `-j10` selection measured 554.43 seconds without it against 291.08 seconds with it. `clean-test-cache` remains as an explicit target for deliberately discarding timing data and transient logs.
- A fresh build tree schedules well too. Display-locked and D3D11-locked tests get a small baseline cost when they have no explicit one, and the shared AArch64 artifact lane is seeded below the display lane, so both long serialized chains start immediately while zero-cost tests fill the remaining workers. Existing Windows high-cost overrides and every resource-isolation contract are preserved.
- Fast Debug (`ZANNA_FAST_DEBUG`, the default local configuration) now compiles the C
  runtime object libraries with `-O2 -g`. They previously received no optimization flag
  at all — only C++ targets got `-Og` — which made the software rasterizer, skinning and
  heap paths the dominant cost of every headless 3D test and probe. Assertions stay on.
- `rt_gc_collect` recovery locals that are assigned after its `setjmp` are now
  `volatile`; an optimized build kept them in registers and the trap-recovery branch
  restored finalizers from stale values (`test_rt_gc`).

### Documentation

The audio guide describes the streaming contract as it now behaves — what `Update()` is for, what the streamer covers, and that `Music.Play`'s argument is a loop flag — and states the MP3 decoder's real scope. The guides, references, command-line help, and generated runtime API have also been checked against this snapshot, so their examples and platform guidance describe the behavior users receive. The backend, linker, memory-management, and command-line guides cover the new capabilities in the areas where developers need them.

### Known limitations

- This is a pre-alpha source release. APIs, diagnostics, IL rules, project formats, and tooling may change before a stable release.
- Native macOS support targets Apple Silicon. Linux targets x86-64 and AArch64; backend and platform capability details remain documented in the supported-platform guides.
- OpenGL point shadows, macOS GPU frame timing, one Metal counter, and software-renderer reflection/HDR settings remain unavailable as documented in the 3D guides.
- Assigning inherited `Filter` and `Wrap` properties through `GpuTexture2D` remains unsupported; use the underlying texture API where those settings are required.
- MP3 frames that select the reserved Huffman codebooks 4 or 14 are rejected as unsupported data; no standard encoder emits them.

<!-- END DRAFT -->
