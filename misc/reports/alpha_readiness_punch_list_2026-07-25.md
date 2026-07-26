# Alpha Readiness Punch List — v0.2.99 → v0.3.0-alpha

**Date:** 2026-07-25
**Basis:** Eight deep C/C++ source audits plus six subsystem readiness reviews against the
stated alpha goals (working frontend/IL/backends, complete 2D + 3D APIs, competitive scene
editors, reliable packaging, quality installers, functionally complete zannalib).
**Scope note:** protocol-security and cryptographic-hardening items are deliberately excluded
from this list and tracked separately (see "Deferred: security hardening track"). Memory-safety
defects that manifest as crashes or corruption are retained regardless of which module they
live in.
**Execution plan:** `misc/plans/alpha_push_1/README.md` sequences these findings into phases and
records the owner's scope decisions. This document is the backlog; that one is the schedule.

Severity legend:

- **P0** — ships broken or crashes/corrupts on input a normal user will encounter. Must fix.
- **P1** — infrastructure that lets P0-class regressions reach users undetected. Must fix.
- **P2** — real gap against a stated alpha goal; fix or explicitly de-scope with docs.
- **P3** — quality/consistency; fold into any convenient batch.

---

## P0 — Correctness and memory safety

### P0-1. `RESUME NEXT` silently miscompiles in both bytecode VM modes
`src/bytecode/BytecodeVM.cpp:4342-4343`, `:4365`, setup at `:4254`, `:4260-4261`

`dispatchTrap` records `nextPc = fp_->pc` (next *bytecode word*) but pairs it with the operand
depth captured at `eh.push`. When the faulting IL instruction produces a result, the next
bytecode word is that instruction's own `STORE_LOCAL`, which then pops an empty stack. The
bytecode backend has no IL-instruction-boundary concept at this site.

Reproduced with a 7-line BASIC program using `ON ERROR GOTO` / `RESUME NEXT` compiled by the
real frontend: interpreter returns `rc=0 out=[after]`; `--bytecode` and `--bc-threaded` both
fail with `operand stack underflow at STORE_LOCAL`. Also reproduces on `udiv.chk0` and
`idx.chk`.

Missed because the sole existing test (`src/tests/unit/test_bytecode_vm.cpp:1959-2032`)
hand-writes bytecode where the faulting op is followed by `LOAD_I8` rather than the
`STORE_LOCAL` that `BytecodeCompiler::storeResult` always emits — it does not model real
compiler output.

**Fix:** record the resume point at IL-instruction granularity, and rebuild the parity test
from compiler output rather than hand-written bytecode.

### P0-2. TLS `recv_record` writes up to 256 bytes past a stack buffer
`src/runtime/network/rt_tls.c:1425` (and `:1394`/`:1397` for the decrypted path)

`recv_record` takes `uint8_t *data` with **no capacity parameter**. It validates the record
length only against `TLS_MAX_CIPHERTEXT` (16640), but every caller passes a
`uint8_t data[TLS_MAX_RECORD_SIZE]` — a 16384-byte stack buffer (`rt_tls.c:2756`, `:3031`;
`session->app_buffer` at `rt_tls_internal.h:128` is the same size). The plaintext branch then
does `memcpy(data, payload, length)`.

`keys_established` is false throughout the handshake, so the plaintext branch is reachable
before any authentication — a malformed or hostile server response smashes 256 bytes of stack
with attacker-controlled data. Independently verified during this audit.

**Fix:** cap plaintext records at 2^14 and decrypted inner plaintext at 2^14+1 (RFC 8446 §5.2);
ideally pass an explicit capacity argument.

### P0-3. MP3 whole-file decode overflows the heap on a channel-count change
`src/runtime/audio/rt_mp3.c:743`, `:1067`, `:1099`

`pcm` is sized `total_samples * channels` using the **first** frame's channel count, but
`mp3_decode_frame_internal` memsets `samples_per_frame * hdr.channels * sizeof(int16_t)` at the
write offset using the **current** frame's count. The `frame_channels != channels` guard sits at
`:1099`, *after* the write.

An `.mp3` whose first frame is mono and any later frame is stereo overflows by 1152 int16
(2304 bytes) of decoded audio on the final frame. Reached from `rt_audio_decode.c:356` on any
in-memory `Sound` load. Independently verified. The streaming path uses a fixed
`frame_pcm[1152*2]` scratch and is safe.

**Fix:** decode into fixed scratch, validate channel/sample consistency, then copy.

### P0-4. Draco sequential decode: `uint32` overflow drives an unbounded heap write
`src/runtime/graphics/3d/assets/rt_gltf_draco.inc:3058`, write loop at `:891-899`

`uint32_t num_values = att->num_values * (uint32_t)eff_comps;` wraps. Allocations at
`:3094-3095` use the wrapped value (16 bytes) while `:3126` passes the **unwrapped** count into
`draco_difference_compute_original`, which writes `att->values + i * components` for ~4 billion
iterations.

Trigger: `.glb` with `KHR_draco_mesh_compression`, sequential method, `num_points = 0x40000001`,
4 components, WRAP transform. Reached from `rt_gltf_mesh.inc:1761` on any model load, including
the async worker thread. This is the only path in the file that does not use
`rt_untrusted_count_ok`; siblings at `:3155`, `:3190`, `:3284-3289`, `:3422` share the root cause
but are gated behind multi-GB allocations.

**Fix:** 64-bit product check at `:3058` plus a `num_values`-vs-`att->num_values` consistency
assert.

### P0-5. macOS IME marked-text use-after-free
`src/lib/graphics/src/vgfx_platform_macos.m:940`

`setMarkedText:` stores an autoreleased `NSString` by direct ivar assignment
(`_markedText = text;`). `src/lib/graphics/CMakeLists.txt:131-134` adds no `-fobjc-arc`, so this
file compiles under MRC and the assignment retains nothing. The string dies when the
`@autoreleasepool` in `vgfx_platform_process_events` (`:1670`) drains; the next keystroke
messages it via `hasMarkedText` (`:964`), `markedRange` (`:970`), or `unmarkText` (`:948`).

Trigger: any multi-keystroke CJK composition spanning two event pumps. Crashes every IME user.

**Fix:** compile `vgfx_platform_macos.m` with `-fobjc-arc` — this also resolves P2-9 (the view,
delegate, and menu-tree leaks in the same file, which share the root cause).

### P0-6. Windows ConPTY blocking wait deadlocks permanently
`src/runtime/system/rt_pty.c:820-838`

`pty_poll_internal(wait=1)` drains the output pipe once, then blocks in
`WaitForSingleObject(process, INFINITE)` without draining. Once conhost's pipe fills, the child
blocks writing and never exits; the parent waits forever.

The POSIX side (`:1030-1060`) and `rt_process.c:783-816` both implement the correct
drain + 10 ms timed-wait loop — the Windows PTY path simply missed the fix. Trigger:
`Pty.Wait()` on any child emitting more than ~64 KB after the last drain.

**Fix:** port the `rt_process.c` Windows wait loop verbatim.

### P0-7. IL parser is O(N²) and trivially memory-exhaustible
`src/il/io/InstrParser.cpp:67-103`; `src/il/io/OperandParse_ValueDetail.cpp:212-213`;
`src/il/verify/InstructionChecker.cpp:684-713`

Three compounding defects, all measured:

1. `InstructionParseGuard` deep-copies all per-function SSA state (`tempIds`,
   `forwardTempNames`, `pendingBrs`, `valueNames`) for **every instruction** to support
   rollback. Measured `il-verify`: 4k inst = 0.26 s, 8k = 0.94 s, 16k = 3.74 s, 32k = **14.55 s**
   — a clean 4× per doubling. `maxInstructions` is 10M, so the declared limits bound element
   counts but not time (~100k instructions ≈ minutes).
2. A parsed SSA id sizes a container directly: `valueNames.resize(reservedId + 1)` where
   `reservedId` may be up to `maxTempsPerFunction` = 9,999,999. A **72-byte** `.il` file peaks
   at **490 MB** RSS, and `maxFunctions` allows 100,000 such functions.
3. Checked-arithmetic demotion re-walks the enclosing block per `add`/`sub`/`mul`
   (`:690-704`), with a whole-function range fixpoint fallback (`:708`) — ~35% of the quadratic
   cost; the parser is the other ~65%.

**Fix:** make the guard copy-on-write or journal-based; validate `reservedId` against actual
temp count before resizing; memoize the demotion range analysis per block (ADR 0026 already
names this as the intended follow-up).

### P0-8. `check_runtime_completeness.sh` is red on master
12 3D entry points added in recent commits (`rt_canvas3d_new_offscreen_accelerated`,
`rt_scene3d_save_text`, `rt_model3d_load_text_result`, the `rt_scene_node3d_*` prefab/light
accessors, `rt_light3d_*` spot-cone accessors) are missing from graphics-disabled builds, so
`-DZANNA_ENABLE_GRAPHICS=OFF` does not link. Red for several commits because the script is wired
into neither CTest nor any workflow.

**Fix:** add the stubs, register the script as a ctest, and gate on it.

### P0-9. x86 store-to-load forwarding ignores implicit RAX/RDX defs (wrong code)
`src/codegen/x86_64/peephole/MemoryOpt.cpp:87-101` (`definesOperandReg`), `:111-134`
(`isMemoryBarrier`), applied at `:248-288`

`definesOperandReg` detects clobbers **only through explicit operand roles**. `CQO` has zero
operands (implicit RDX def); `IDIVrm`/`DIVrm`/`MULr`/`IMULr` carry only the divisor/multiplier
(implicit RAX:RDX defs). None is listed as a barrier. So this post-RA sequence — exactly what the
allocator emits when a value lives in RDX across a division (`Allocator.cpp:944-967` spills RDX;
`LowerDiv.cpp:572-580` emits `CQO; IDIVrm`; `:428`/`:457` emit `MULr`/`IMULr` for
magic-number division) — miscompiles:

```
movq [rbp-24], rdx    ; tracked: slot -24 holds RDX
cqo                   ; implicitly defines RDX — not detected
idivq rbx             ; implicitly defines RAX:RDX — not detected
movq rdx, [rbp-24]    ; forwarded to "mov rdx, rdx", then identity-removed
```

The reload vanishes and RDX silently keeps the remainder. RDX is the 3rd register in the
allocation pool (`TargetX64.cpp:42-52`), so values do land there.

**Verified during this audit:** `MovFolding.cpp:121-138` `defRegMask` contains the correct
implicit-def switch for precisely these opcodes; `MemoryOpt.cpp` has no equivalent. When the
earlier DCE dividend bug was fixed, the fix landed in `DCE.cpp:217-238` and `MovFolding.cpp` but
not here — a second, weaker copy of the implicit-def model that diverged.

**Fix:** model implicit defs in `definesOperandReg`, or treat CQO/IDIV/DIV/MUL/IMUL as barriers.

### P0-10. x86 RA "transparent successor" carve-out drops spill homes (wrong code)
`src/codegen/x86_64/ra/Allocator.cpp:494-521` (`canCarryIntoNextBlock`), `:220-239`
(`blockReadsNoVRegs_`), `:257-275`, `:1075-1083`, `:1229-1237`

The carve-out permits extra successors that "never read a virtual register" (intended for trap
stubs), but `blockReadsNoVRegs_` is **purely syntactic**: a forwarding block containing only
`JMP L`, or only physical-register code such as a no-arg runtime `CALL`, qualifies — even though
control flows *through* it into blocks that do read vregs.

The spill-home pre-pass then skips the carry block, so a vreg live along
`A →(JCC) F(jmp only) → T` gets no spill home and no store: it is released without spilling when
it dies in the fallthrough chain, and when `T` uses it `processRegOperand` sees
`!hasPhys && !needsSpill` and hands out a **fresh register with no reload** — garbage on the
F→T path.

Secondary defect: `:229` checks `mem->base.isPhys` but never `mem->index`, so a block whose only
vreg is a memory *index* register is also misclassified as vreg-free.

**Fix:** require `liveIn(succ).empty()` — liveness is already computed at `:216` — instead of the
syntactic scan.

### P0-11. aarch64 peephole `usesReg` omits SP-relative argument stores (wrong code)
`src/codegen/aarch64/peephole/PeepholeCommon.cpp:180-365`

This is a hand-maintained parallel of `ra::operandRoles` whose **default is "not used"**. Diffed
against `MOpcodeDef.inc`, it is missing `StrRegSpImm`/`StrFprSpImm` — the outgoing **stack
argument** stores emitted at `InstrLowering.cpp:417` for the 9th and later arguments — plus
`FRintN`. These read `ops[0]`/`ops[1]`, but `usesReg` reports false.

The RA's own table is complete and even throws on unclassified operands
(`ra/OperandRoles.cpp:221`); only the peephole copy is gapped. Consumers that will treat a live
stack-argument source register as dead: `IdentityElim.cpp:71-83`, `MemoryOpt.cpp:304-309`
(`tryMaddFusion`), `CopyPropDCE.cpp:813-848`, `BranchOpt.cpp:303-311`/`:374-384`.

Miscompiling shape: `mov x10, x22; mov x0, x10; str x10,[sp,#0]; bl f` — the fold marks x10 dead,
kills the first move, and the callee receives a stale stack argument.

**Fix:** route these passes through `ra::operandRoles`, or at minimum add the missing cases and
flip the default to conservative.

---

## P1 — Quality infrastructure (the trust problem)

### P1-1. `NDEBUG` silently neuters ~280 test files
`src/tests/cmake/TestHelpers.cmake:207`

`zanna_add_test` never forces `-UNDEBUG`, and nothing in the root `CMakeLists.txt` removes it.
41 files in `src/tests/vm` and 238 in `src/tests/unit` use bare `assert()` with **zero**
`#undef NDEBUG`. `docs/internals/testing.md:48` documents `ZANNA_BUILD_TYPE=RelWithDebInfo` as
supported, and CMake adds `-DNDEBUG` for that config — in that lane those files pass vacuously,
including the *entire* bytecode-VM parity gate (`test_full_program_parity.cpp:204`, whose only
oracle is `assert(false && ...)`).

This finding undermines confidence in every other test number in this document. Fix first.

### P1-2. No continuous CI runs tests
The only workflow on push/PR is `runtime-static-analysis.yml`, which runs `cppcheck-runtime` and
no ctest at all. All three build-and-test workflows are `workflow_dispatch:`-gated. Every
regression gate for a ~1.6M-LOC project reduces to one person running
`scripts/build_zanna_unix.sh` on one Apple Silicon Mac.

**Requires a decision:** the CI-workflow freeze in `CLAUDE.md:114` currently forbids this fix.

### P1-3. The VM-vs-native differential gate runs on exactly one target
`src/tests/e2e/CMakeLists.txt:565` gates the 33-program byte-diff corpus behind
`arm64 AND APPLE`. The in-tree comment at `:569-579` records that standing this gate up
**found four wrong-code bugs immediately**. The same gate has never been pointed at x86-64 or
Windows, where equivalence rests on 3 hand-written scenarios
(`src/tests/codegen/x86_64/test_diff_vm_native.cpp:41`) — on a backend with ~9 documented
register-allocator/peephole miscompiles across four releases.

Cross-layer conformance (`CrossLayerArithTests.cpp:28`) and all 11 game/3D native probes are
`APPLE`-gated too. Highest value-per-effort item in this document.

### P1-4. Known bugs have their regression tests suppressed
- `src/tests/basic/CMakeLists.txt:1261-1278` — BUG-094 commented out; BUG-058 carries
  *"intentionally not registered until bug is fixed to keep CI green."*
- `src/tests/vm/CMakeLists.txt:231` — flaky trap-metadata test excluded via `NO_CTEST`.
- `src/tests/CMakeLists.txt:478` — `test_codegen_arm64_run_ret42` (exit-code propagation)
  disabled "to keep CI stable."
- `src/tests/CMakeLists.txt:620` — an audit check left in place producing 1,056 false positives.

### P1-5. Deep-inspection lanes are all opt-in, therefore off
Sanitizers (`ci_full_sanitizer.sh`, documented as "local opt-in checks"), all 26 fuzz harnesses
(`src/tests/fuzz/CMakeLists.txt:6` early-returns unless `ZANNA_ENABLE_FUZZ=ON`, so **zero** of
the ~2,017 default tests are fuzz), and coverage (no threshold) are absent from the canonical
gate — against ~574K LOC of C/C++ parsing untrusted input.

**Fuzz coverage gaps this audit specifically implicates:** every P0 memory-safety finding above
sits in code with no harness. Media is the weakest area — the only harness is
`fuzz_image_decoders.cpp` (PNG/JPEG/GIF first frame). Unfuzzed: BMP, animated GIF, WAV, **MP3**
(P0-3), Ogg/Vorbis, Theora, AVI, BDF/PSF, `vg_ttf`, `rt_tilemap_io`, `rt_tiled_import`,
`rt_scene_editor`, plus the bytecode/object-file readers and the archive reader.

### P1-6. Perf regression gate is defeated
`scripts/benchmark.sh:751` invokes `benchmark_compare.sh` with `|| true`, discarding its
regression exit code. The baseline (`misc/benchmarks/baseline.jsonl`) is from 2026-03-08,
macOS-arm64 only, and `perf`-labeled tests are excluded from the default lane. A 2× interpreter
slowdown would ship silently.

---

## P2 — Gaps against stated alpha goals

### P2-1. `.scene3d` carries no physics or gameplay data
The format covers nodes, meshes, materials, lights, cameras, animation, LOD, prefabs, and typed
metadata — but **no** colliders, bodies, triggers, spawns, navmesh, terrain, water, particles,
or audio emitters. Colliders exist only as an ADR-0185 metadata convention that the runtime
never reads (`grep collider.kind src/runtime/ src/il/runtime/defs/` → zero hits;
`src/zannastudio/src/ui/scene_collider_3d.zia:36-39` states the boundary explicitly).

Consequence: every game hand-writes an adapter. `ashfall-scenes` carries ~3,100 LOC in `world/`
(a bespoke `af.kind` dispatcher plus per-box `PhysicsBody3D` construction at
`world/level_base.zia:394-398`) against 26,926 lines of authored `.scene3d`. The engine's own
`BodyDef` type goes entirely unused by the scene path.

**This is the single largest gap between the current state and "any 3D game authored in the
editor."**

### P2-2. Streaming leaks the whole stream if `Update()` stops being called
`rt_game3d_streaming.inc:2669-2674`, sole caller `rt_game3d_world_api.inc:758`

Staging jobs take a strong retain released only in `game3d_stream_stage_commit`, reachable only
from `game3d_stream_stage_drain()`, called only from `rt_game3d_world_stream_update`. The asset
commit queue by contrast drains from three places. Any level transition, unmount, or trap out of
the frame loop leaks the stream plus all resident cells and staged buffers for the process
lifetime. Studio mounts/unmounts per edit session.

### P2-3. Async asset worker finalizes GC objects off-thread
`rt_game3d_asset_load.inc:814-820` calls `game3d_asset_async_job_free` on the worker thread,
running `rt_obj_release_check0` → `game3d_asset_handle_finalize`, cascading into
`Entity3D`/`ModelTemplate`/`Scene3D` finalizers, none thread-safe. The streaming subsystem
solves this exact problem with a CAS fallback stack (`rt_game3d_streaming.inc:2343-2346`); the
asset path has no equivalent. Reachable via a 32-byte malloc failure, deterministically
triggerable through the existing test hook.

### P2-4. No 2D text beyond ASCII
The built-in font is an 8×8 bitmap covering ASCII 32–126 (`rt_font.c:40-53`); non-ASCII renders
as `?` (`rt_pixels_draw.c:22`). Custom fonts only via `BitmapFont.LoadBdf/LoadPsf`. A
dependency-free TTF rasterizer already exists at `src/lib/gui/src/font/vg_ttf.c` but is wired
only to the GUI toolkit.

**No non-English 2D game can ship.** Highest value-per-effort item in the 2D track — this is
wiring, not new code.

### P2-5. Windows installer journal can destroy the previous installation
`src/tools/windows_installer/WindowsInstallerLifecycle.cpp:3292-3293`, recovery at `:3240-3246`

Between `moveDirectory(newRoot, installRoot)` and `writeJournal(NewActive)` — both write-through,
so the rename can be durable while the journal is not — a crash leaves state `OldMoved` with
`installRoot` present. Recovery skips the restore (`:3241` requires `!exists(installRoot)`) and
then `removeTreeChecked(paths.directory)` deletes the transaction dir **including `old/`**.

Result: new tree active, metadata never applied (fresh installs get no ARP entry; upgrades keep
stale ARP/shortcuts/PATH), previous version unrecoverable. The injection hook
`maybeInjectFailure("after-new-move")` sits *after* the journal write, so this window is
currently untestable. Same shape in `performUninstall` (`:3349-3356`).

### P2-6. macOS installer has no rollback, no collision preflight, no disk-space check
`grep -c rollback src/tools/common/packaging/MacOSPackageBuilder.cpp` → **0**. Windows rejects
unowned collisions before mutating; macOS does not. Validator-script asymmetry mirrors this:
Windows 1,131 lines, Linux 92 lines (orphaned — referenced by no doc/test/workflow), macOS none.

### P2-7. macOS ships no Studio `.app`
Per ADR 0149, Studio stages as a bare Mach-O plus a shell launcher — deliberately not a bundle.
The only `/Applications` entry is a file-handler helper. macOS users get no Dock icon, no
Launchpad entry, no double-click launch; Windows gets a Start Menu shortcut and a finish-page
launch button.

### P2-8. Third-party game installers render Zanna's branding
`WindowsInstallerTheme.cpp:488` `drawInstallerBackdrop` unconditionally paints `L"ZANNA"`,
`L"DEVELOPER PLATFORM"`, and `L"CODE. CREATE. COMPILE. CONQUER."`. The app installer reuses the
same host binary (`WindowsPackageBuilder.cpp:2127`); the wizard branches on `productKind` only
for finish-page actions (`WindowsInstallerWizard.cpp:1547-1561`), never for branding. A shipped
indie game's setup advertises Zanna.

### P2-9. macOS graphics layer compiles without ARC
`src/lib/graphics/CMakeLists.txt:131-134` — only three runtime `.m` files get `-fobjc-arc`
(`src/runtime/CMakeLists.txt:520-522`). `vgfx_platform_macos.m` contains zero retain/release
calls, so beyond P0-5 it leaks the `VGFXView` and `VGFXWindowDelegate` on every window
create/destroy (`:1534-1567`), and an entire ~12-object menu tree per `set_title` call
(`:2063`, `:546-555`) — so a title-updating app leaks a menu tree per update.

### P2-10. Audio has no device-loss recovery on Windows or Linux
`vaud_platform_win32.c:546-604` — after 8 consecutive failures (exactly what
`AUDCLNT_E_DEVICE_INVALIDATED` produces on endpoint removal or default-device change) the worker
sets `running=0` and exits permanently. No `IMMNotificationClient` exists anywhere in
`src/lib/audio`. Same terminal pattern on ALSA (`vaud_platform_linux.c:286-295`) for `-ENODEV`
on USB-headset unplug. macOS AudioQueue follows the default device correctly.

**Trigger:** unplug the active output device → audio dead until app restart.

### P2-11. Timezone database has five zones expiring end-2026
`rt_tzdata_generated.inc:37` embeds UTC, Etc/UTC, Asia/Tokyo, America/New_York, Australia/Sydney,
with DST transitions only for 2025–2026; `rt_timezone.c` never consults the host tzdb. The header
states the subset exists for runtime determinism tests. Any tool doing real dates in
Europe/Berlin is wrong today, and New_York/Sydney become wrong on 2027-01-01.

### P2-12. `Option`/`Result` contract is ~80% landed on the highest-traffic APIs
49 `*Result` and 28 `*Option` names out of 7,593 functions. Still trapping rather than returning
a value: `Data.Json.Parse` (`rt_json_parse.c:173`), the entire `IO.File`/`IO.Dir` surface,
`Network.Http.Get/Post` (`rt_network_http_api.c:103,117`), `IO.Archive.Open`, `IO.Assets.Load`,
`Csv/Toml/Ini.Parse`. `Xml`/`Yaml`/`Serialize` already have `ParseResult` forms, so the pattern
is proven — this is application, not design.

### P2-13. Runtime stability metadata is a string-prefix heuristic
`src/tools/zanna/main.cpp:568-583` `inferRuntimeStability()` classifies all of `Zanna.GUI.*`,
`Zanna.Game3D.*`, `Zanna.Graphics3D.*`, `Zanna.Zia.*`, `Zanna.Basic.*` as `preview` — including
the entire 1,131-function GUI toolkit that every desktop tool is built on.
`inferRuntimeMigrationTarget()` (`:554-560`) returns `{}` unconditionally;
`inferRuntimeOwnership()` (`:703-706`) is self-documented as advisory "until source annotations
land." No `RT_STABILITY`/`RT_CAPABILITY` exists in any `.def`.

### P2-14. Diagnostics quality on the Zia frontend
Every undefined identifier is reported **exactly twice** (no dedup in `DiagnosticEngine::report`;
single emission site at `Sema_Expr.cpp:377`), and the message blames the root namespace rather
than the unresolved member — `Zanna.Nope.DoesNotExist(1)` reports `Undefined identifier: Zanna`.
Parse codes are assigned by substring-matching message text
(`Parser_Tokens.cpp:46-74`), so rewording a message silently changes its stable code. Zia sema
uses the generic `V-ZIA-SEMA` for 266 of 273 emissions versus BASIC's 87 distinct `B####` codes.
There is also no `--max-errors` cap and no JSON diagnostics from the plain CLI path
(`cmd_front_zia.cpp:54` only calls `printAll()`).

### P2-15. BASIC documented correctness bug
`docs/languages/basic-namespaces.md:1213-1228` states lowercase qualified calls like `a.b.f()`
fail when defined as `A.B.F()`, and calls it a bug. `src/frontends/basic/ProcRegistry.cpp:257`
now claims case-insensitive canonicalization. Either the doc is stale or the fix is partial —
15-minute resolution, but it is the only self-documented correctness bug in a shipping language.

---

## P3 — Quality and consistency

- **P3-1.** Cursor types 6–12 clamped to arrow on macOS (`vgfx_platform_macos.m:2274`) and
  Windows (`vgfx_platform_win32.c:2609`) despite both files' own tables supporting 0–12; X11 is
  correct. Breaks GRAB/GRABBING, RESIZE_NWSE/NESW, CROSSHAIR, HELP, NOT_ALLOWED — most editor
  cursors.
- **P3-2.** Win32 `PeekMessageW` filters by HWND (`vgfx_platform_win32.c:2015`), so
  `wait_events` can busy-spin at 100% CPU on undrainable thread messages.
- **P3-3.** Windows DPI: process opts into `SYSTEM_AWARE` (`:459-482`), so the fully-written
  `WM_DPICHANGED` handler (`:1189-1221`) is dead code — cross-monitor drags render blurry.
  Either move to `PER_MONITOR_AWARE_V2` or delete the handler.
- **P3-4.** X11 `Clipboard.HasText` performs a full clipboard transfer
  (`vgfx_platform_linux.c:3348-3359`), blocking the UI up to ~2 s per call against a frozen
  owner. Answer TARGETS instead.
- **P3-5.** Three more instances of the trap-then-continue pattern in
  `src/vm/ops/Op_TrapEh.cpp:138-145`, `:146-155`, `:361-368` (OOB read / end-iterator deref
  after a non-`noreturn` trap). Unreachable from verified IL today; defense-in-depth.
- **P3-6.** `LoopUnroll` rewrites a preheader terminator without verifying it is an
  unconditional `br` (`LoopUnroll.cpp:665-668`, `findPreheader` at `:121-142`), and erases loop
  blocks without checking out-of-loop uses of loop-defined temps (`:674-678`). Mitigated in the
  default O2 pipeline by `loop-simplify` running first, but reachable via
  `zanna il-opt --passes loop-unroll`.
- **P3-7.** `CheckOpt` constant `idx.chk` fold has signed-overflow UB (`CheckOpt.cpp:378-379`);
  every other fold in the file uses `addOverflows`/`subOverflows`.
- **P3-8.** `Peephole` emits I64-only opcodes when expanding narrow checked div/rem
  (`Peephole.cpp:469-491`, `:509-602`), producing IL the result-type checker rejects.
- **P3-9.** `maxTempsPerFunction` bypassable for non-`%tN`-shaped temp names
  (`OperandParse_ValueDetail.cpp:194-221`).
- **P3-10.** `zanna init "My Game"` scaffolds `project My Game` unquoted
  (`cmd_init.cpp:179`), which the loader then rejects (`project_loader.cpp:1568-1575`);
  `zanna init 'my#app'` silently truncates to `my`. First-touch UX defect.
- **P3-11.** `--stack-size` accepts negatives via `strtoull` wraparound
  (`cmd_run.cpp:548-558`); `--max-steps` already uses `from_chars` correctly.
- **P3-12.** Uncaught-exception exposures on realistic paths: `cmd_init.cpp:162,164`
  (`fs::current_path` from a deleted cwd), `cmd_run.cpp:1192` and `cmd_package.cpp:1786`
  (`generateTempAssetPath` with an unwritable `TMPDIR`) — `std::terminate`, no diagnostic.
- **P3-13.** `zanna run` routes to the IL runner if *any* pre-`--` argument ends in `.il`
  (`cmd_run.cpp:1248-1254`), so `--stdin-from=input.il prog.zia` misdispatches.
- **P3-14.** Non-release `install-package` failures leave partial artifacts when a builder
  throws (`cmd_install_package.cpp:2097,2483,2814`); `zanna package` cleans unconditionally.
- **P3-15.** Temp files and stage dirs are not cleaned on signals (no handler anywhere in
  `src/tools`); Ctrl-C during build/package leaks into `$TMPDIR`. Names are pid+tick+counter with
  `O_CREAT|O_EXCL`, so this is hygiene only.
- **P3-16.** Theora setup-header leak (`rt_theora.c:772-775`, `:737-745`) — ~2 MB per repeated
  `0x82` packet, ~10,000× amplification; and unbounded per-serial allocation in the Ogg demuxer
  (`rt_ogg.c:293-304`).
- **P3-17.** Unbounded CPU on nested TrueType composite glyphs (`vg_ttf.c:936-1158`) — depth is
  capped at 16 but components-per-composite and total work are not, and empty components cost
  zero memory so allocation failure never halts it.
- **P3-18.** Commit queues are never freed (`rt_g3d_commit_queue.c:102` has no caller), so the
  cancel/teardown hooks are dead code and untested.
- **P3-19.** `rt_heap_realloc` divides by zero when `elem_size==0, new_cap==0, new_len>0`
  (`rt_heap.c:1073-1088`); `rt_heap_alloc` has the guards in the correct order. Latent — only
  in-tree caller passes `sizeof(void*)` — but it is stable C ABI.
- **P3-20.** `SaveData.SetInt/SetString` swallow OOM (`rt_savedata.c:1294`, `:1304`), durably
  persisting a save file missing the key.
- **P3-21.** Windows `Exec.Capture` returns truncated output as success when a mid-capture
  `realloc` fails (`rt_exec.c:816-835`); the POSIX path traps correctly.
- **P3-22.** `Process.WriteStdin` asymmetry (`rt_process.c:1487-1541`): silent partial writes on
  POSIX (non-blocking pipe), potential deadlock on Windows (blocking pipe + undrained stdout).
- **P3-23.** POSIX PTY child calls non-async-signal-safe `execvp` between fork and exec
  (`rt_pty.c:1273-1275`), and the parent blocks forever on the status pipe if the child wedges.
  `rt_process.c` already pre-resolves PATH and uses `posix_spawn`.
- **P3-24.** Signed-overflow in the PSF glyph-size guard (`rt_bitmapfont.c:745`, `:81-83`);
  transient ~2 GB malloc before the sanity check.
- **P3-25.** `deserialize_pixels_blob` allocates before validating length
  (`rt_tilemap_io.c:712` vs `:719`) and has no pixel budget unlike PNG/JPEG/GIF/BMP.
- **P3-26.** Stale docs: `ParserUtil.cpp:301,414` claim `stoll`/`stod` usage that no longer
  exists; `rt_ogg.c:192-193` comment contradicts the code; `resolveDependencyPath`'s comment at
  `rt_tiled_import.cpp:793` claims containment the filesystem branch does not enforce;
  `examples/games/xenoscape-scenes/level.zia:41-56` still describes the replaced code-built
  approach.
- **P3-27.** `xenoscape-scenes` and `ashfall-scenes` are absent from `scripts/demo_projects.list`
  despite being the flagship scene-driven demos.
- **P3-28.** `docs/generated/runtime` is 12 functions stale versus `rtgen`.
- **P3-29.** aarch64 `foldComputeIntoTarget` (`CopyPropDCE.cpp:823-848`) breaks its deadness scan
  with `dead=true` at `Bl`/`Br`/`Ret`/`JumpTable`, never consulting Bl's implicit argument-register
  uses, Ret's x0/v0 use, the branch target's live-in, or `block.carriedExitRegs` (sibling passes
  take it as a parameter; this one does not). Correct today only by accident of the allocator's
  write-through discipline. Same block-end fall-off in `tryMaddFusion` (`MemoryOpt.cpp:304-309`).
- **P3-30.** aarch64 has **no stack probes** while Windows ARM64 is a declared target
  (`TargetAArch64.hpp:209`). x86-64 implements them on both families
  (`FrameLowering.cpp:483-527`: Win64 `__chkstk`, Unix inline page touches); `FrameBuilder.cpp` /
  `FrameCodegen.hpp` have none. A frame >4 KB — attainable with a few hundred spill slots —
  subtracts SP past the guard page in one step. Ties to decision 2 below.
- **P3-31.** aarch64 TBZ/TBNZ fusion (`BranchOpt.cpp:298-319`, `:354-376`) erases a `TST` while
  scanning only for register uses of `dstReg`, not flag readers, so a later `BCond`/`Cset`/`Csel`
  on the fallthrough path would read stale NZCV. Latent (lowering emits one consumer per
  flag-setter); the pass already has `writesFlags` at `:323` — add the `usesFlags` check.
- **P3-32.** aarch64 block-local DCE exit-live set omits callee-saved FPRs v8–v15
  (`CopyPropDCE.cpp:225-247`) which `pinnedSlotFPR_` uses cross-block. Production is safe because
  the CFG-aware path runs instead, but the legacy `target==nullptr` path (unit tests) is wrong and
  the asymmetry is a trap.
- **P3-33.** x86 scheduler models `OpRipLabel` as read-only memory regardless of opcode
  (`Scheduler.cpp:387-391`); a store through a RIP-label operand could be reordered. Unreachable
  today — add an assert.
- **P3-34.** x86 `nextInstrReadsFlags` (`PeepholeCommon.hpp:436-449`) assumes flags are dead when
  falling off the end of a block's instruction vector; MOVri→XOR, ADD#0 removal, and IMUL→SHL all
  depend on it. Sound only while lowering never separates a flag-setter from its consumer across a
  block boundary. Worth an explicit invariant/assert.

---

## Deferred: security hardening track (explicitly out of scope for this list)

Recorded so they are not lost, to be scheduled after alpha: TLS 1.2 + P-256 interop; certificate
`pathLenConstraint` and revocation handling (including the POSIX/Windows divergence on
`ZANNA_TLS_REQUIRE_REVOCATION`); post-handshake record-type enforcement and close_notify
tracking; HPACK instruction-order bug and decompression-bomb budgets; HTTP/2 CONTINUATION flood;
HTTP/1 duplicate framing headers; chunked-response 8 KB regression; keep-alive pool poisoning;
WebSocket idle/zero-length-frame loops and handshake deadlines; Wayland clipboard SIGPIPE
termination; Tiled filesystem-mode path traversal; installer PATH snapshot clobbering.

Note: the HTTP/1 chunked-response bug (`rt_network_http_response.inc:679-690`, where the
overflow guard sits inside the doubling loop and aborts instead of doubling again) is a
**functional** break — any response chunked above 8 KB fails — so it may warrant promotion into
the alpha list even under this scoping.

---

## Deferred: editor program (tracked separately)

Phase 0 of the Unity-quality program is landed and gated. Remaining before the editors "roughly
compete with Unity" for the core loop: **0.3b** per-backend headless GPU contexts (Metal is the
one genuine TODO in the 3D tree, `vgfx3d_backend_metal_context.inc:957`), **0.5d** module
splits, the last **0.6** surface (2D object-property trio), then **Phase 1** (context menus,
cross-widget DnD, triangle-accurate picking + marquee + outlines, creation ergonomics, 2D object
transforms ADR 0192), **Phase 2** (tile suite, layers, gizmo/camera polish, icons, thumbnails),
**Phase 3** (scale/diagnostics/undo UI), **Phase 4** (play loop + `--scene-watch` hot reload,
ADR 0194). Phase 5 (docking) is deferrable past first alpha.

---

## Decisions required before this plan can be sequenced

1. **Lift the CI-workflow freeze** (`CLAUDE.md:114`) — P1-2 is unfixable without it.
2. **Alpha target matrix** — macOS x86-64 is now correctly documented as unsupported. Do
   Windows ARM64 and Linux ARM64 ship as "experimental", or not at all? Neither has ever
   executed a generated instruction in a test.
3. **Multiplayer and custom shaders** — build a minimal answer or state out-of-scope in
   `docs/gameengine/README.md` and `docs/graphics3d-guide.md`. Silent omission is the worst
   option.
4. **GUI toolkit stability tier** — `stable` or `preview`, decided deliberately rather than by
   string prefix (P2-13).
5. **Signing certificates** — Authenticode and Apple Developer ID procurement. Every trust path
   is code-complete but credential-empty; blocks any public download.
6. **macOS Studio `.app`** — build a real bundle, or document terminal-launch honestly (P2-7).
7. **Bytecode VM** — ship marked experimental (already opt-in and already refuses the debugger),
   or invest in real coverage. P0-1 must be fixed either way if it ships at all.

---

## Suggested execution order

1. **Trust the ground:** P1-1 (NDEBUG), P0-8 (red gate + register it), P1-4 (un-suppress and fix),
   P1-3 (de-`APPLE`-gate the differential corpus, add the x86-64 runner), P1-6 (un-defeat the
   perf gate). Cheap, and everything downstream depends on the numbers being real.
2. **Fix the wrong-code P0s first** — P0-9, P0-10, P0-11 (silent miscompiles reaching every
   natively-compiled program) and P0-1 (`RESUME NEXT` ships broken). These produce incorrect
   programs with no diagnostic, which is strictly worse than a crash. Note P0-9 and P0-11 are both
   *divergent second copies* of an implicit-def/use model whose primary copy is correct — audit
   for further copies while fixing, and consider collapsing them onto `operandRoles`.
3. **Then the remaining P0s:** P0-7 (front end unusable on large input) and the four
   memory-safety defects (P0-2 through P0-5) plus P0-6.
4. **Close the 3D scene-format gap** (P2-1) — highest product leverage against the stated alpha
   goal; P2-2 and P2-3 ride along in the same subsystem.
5. **Runtime punch list:** P2-4 (TTF wiring), P2-11 (tzdata), P2-12 (Result forms), P2-13
   (stability metadata), P2-10 (audio device loss).
6. **Ship vehicle:** P2-5 through P2-8, plus certificate procurement.
7. **Editor Phases 1–4**, in parallel with the above where staffing allows.

Items 1 and 2 are mutually reinforcing: P1-3 (de-`APPLE`-gating the differential corpus) is the
gate that would have caught P0-9 and P0-10 on x86-64, and standing it up the first time already
found four wrong-code bugs. Fixing the miscompiles without also landing the gate leaves the next
one equally invisible.
