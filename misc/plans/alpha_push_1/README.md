# Zanna v0.2.99 → v0.3.0-alpha — Execution Plan (Alpha Push 1)

Date prepared: 2026-07-25
Evidence baseline: eight deep C/C++ source audits and six subsystem readiness reviews against
the working tree of 2026-07-25; see `misc/reports/alpha_readiness_punch_list_2026-07-25.md`
Status: approved program; scope decisions recorded below were made by the project owner

## Context

Eight deep C/C++ audits plus six subsystem readiness reviews produced
`misc/reports/alpha_readiness_punch_list_2026-07-25.md` — 66 findings graded P0–P3, each with
`file:line` evidence and a fix direction. That document is this plan's **input backlog**; this
plan is the sequencing, governance, and verification layer over it. Do not re-derive findings.

Three things force the structure:

1. **There is no CI that runs tests.** The only push/PR workflow runs `cppcheck` and zero ctest.
   Every regression gate for ~1.6M LOC is one person running one script on one Mac. This is
   P1-2, and it is currently *forbidden* by a self-imposed freeze in `CLAUDE.md` — so lifting
   that freeze is step one, not a side quest.
2. **Three silent miscompiles ship today** (P0-9/10/11), all in the same class the project has
   fixed nine times before: a second, weaker copy of an implicit-def/use model diverging from
   the correct one. `RESUME NEXT` is also broken in both bytecode modes (P0-1).
3. **The 3D scene format carries no gameplay data** (P2-1), so "any 3D game authored in the
   editor" is not true — every game hand-writes a ~3,000-line adapter.

### Scope decisions (owner, 2026-07-25)

- **Memory-safety fixes stay in alpha.** P0-2 (TLS record overflow), P0-3 (MP3 heap overflow),
  P0-4 (Draco heap overflow) are crashes, not policy items — they ship fixed.
- **Protocol/crypto hardening is deferred** to a post-alpha track (listed at the end).
- **CI:** full build+test on Linux x64 per PR; all platforms plus sanitizer and fuzz lanes
  nightly.
- **ARM64:** Linux ARM64 and Windows ARM64 both ship **experimental, documented as untested**.
  This promotes aarch64 stack probes (punch-list P3-30) to a blocker and requires each target to
  prove it executes generated code.

### Out of scope

Multiplayer/networking above raw sockets, custom shader authoring, mobile/console/web targets,
Studio editor Phase 5 (docking). Editor Phases 1–4 are in scope (§7).

---

## 1. Governance: lift the CI freeze

The freeze has **no recorded engineering rationale** — `git log -S` traces it to the ViperOS era
(`3d5ed132f`, 2025-12-23) with no explaining commit message. Meanwhile `AGENTS.md:10-13` and the
normative `docs/adr/0006-spec-currency-and-adr-triggers.md:33-36` already express the *correct*
policy: workflow changes require an **ADR**, not abstinence. Only `CLAUDE.md` carries an absolute
ban. So this is an alignment fix, not a policy reversal.

**Write ADR 0195** — `docs/adr/0195-continuous-integration-for-pull-requests.md`, following the
ADR 0073 precedent (which ADR'd the release workflows and is the closest model). Register it
under **Process & Governance** in `docs/adr/README.md:15`. Use the `0000-template.md` sections;
state the lane matrix, the required gates, and that release/signing operations stay
`workflow_dispatch`.

> **Why 0195 and not 0192:** `0191` is the highest written ADR, but the approved editor program
> (`~/.claude/plans/i-think-we-made-polymorphic-mochi.md:511-512`) has reserved 0192 (2D object
> transforms), 0193 (precise raycast), and 0194 (scene hot-reload). Taking 0195 leaves that plan
> untouched. ADR README groups are not numerically sorted, so a temporary gap costs nothing.

**Edit `CLAUDE.md` at exactly three sites:**

| Line | Current | Change to |
|---|---|---|
| `:26` | Principle 2: "…No CI workflow modifications." | "…CI changes require an ADR (see ADR 0195)." |
| `:114` | "`.github/workflows/*` — No CI workflow creation/modification during zanna phase" | "`.github/workflows/*` — CI workflow changes require an ADR" |
| `:270` | "Always-green local builds (no CI modifications)" | "Always-green local builds; CI changes are ADR-gated" |

**Leave alone:** `AGENTS.md:13` and `docs/adr/0006:33-36` — they already say the right thing and
the new ADR satisfies them. **Annotate as superseded:** `misc/plans/zannastudio/README.md:103`
("No CI workflow edits beyond the explicitly waived three lines in P1").

*Exit criteria:* ADR 0192 accepted and registered; the three CLAUDE.md lines updated;
`./scripts/check_docs.sh` passes.

---

## 2. Make the test suite mean something (before any CI lane is enabled)

Standing up CI against today's suite would produce **green checkmarks on a partially inert
suite**. These land first, in this order:

1. **P1-3 — un-gate the differential corpus.** Change `src/tests/e2e/CMakeLists.txt:565` from
   `arm64 AND APPLE` to host-arch selection so the 33-program byte-diff runs on Linux x64 and
   Windows x64 too. **The CLI arch token is `x64`, not `x86_64`** (`src/tools/zanna/main.cpp:1519-1524`)
   — pass `-DARCH=x64`. Gate on the already-computed `ZANNA_BUILD_NATIVE_LINK_*` capability
   macros. Verify on macOS ARM64 first, where it should be a byte-identical no-op.
   `differential_vm_native.cmake` is already `-DARCH`-parameterized and pure `execute_process`,
   so it works verbatim on Windows with no external toolchain (native asm/link are the defaults,
   `CodegenPipeline.hpp:109-110`).
   **Expect this to find bugs** — the in-tree comment at `:569-579` records that standing this
   gate up the first time surfaced four wrong-code defects. Budget 0–6 genuine x86-64
   divergences. This is the highest-value output of the whole standup and the gate that would
   have caught P0-9 and P0-10.
2. **P1-4 — un-suppress the hidden failures.** Fix and re-register BUG-058 and BUG-094
   (`src/tests/basic/CMakeLists.txt:1261-1278`), the `NO_CTEST` flaky VM trap-metadata test
   (`src/tests/vm/CMakeLists.txt:231`), and `test_codegen_arm64_run_ret42`
   (`src/tests/CMakeLists.txt:478`). Fix the 1,056-false-positive arity check (`:620`) or delete
   it.
3. **P1-1 — the NDEBUG trap.** `zanna_add_test` (`src/tests/cmake/TestHelpers.cmake:207`) never
   forces `-UNDEBUG`. The real blast radius is **618 test translation units** under `src/tests`
   that include `assert.h`/`cassert` without `#undef NDEBUG` (only 33 guard, all in
   `src/tests/runtime`) — roughly 3× the original estimate. Under any NDEBUG config their
   oracles vanish, including the bytecode parity gate (`test_full_program_parity.cpp:204`, whose
   only oracle is `assert(false && …)`).

   **Fix:** add `-UNDEBUG` (`/UNDEBUG` under MSVC) to the `zanna_testing` INTERFACE library at
   `src/tests/CMakeLists.txt:241` — every test links it via `TestHelpers.cmake:238`, so one edit
   covers all of them, and it lands *after* the `-DNDEBUG` from `CMAKE_CXX_FLAGS_RELEASE`
   (last-wins). Add a tripwire test (`#ifdef NDEBUG #error`, plus a runtime side-effect check
   `assert(++calls == 1)`) so neither the flag nor its ordering can regress silently.

   **Sequencing note that matters:** Debug never defines NDEBUG, so this fix is a *no-op for the
   PR lane* and only changes Release/RelWithDebInfo behavior. It is therefore the single
   highest-risk change here (it re-arms 618 files' worth of oracles at once) and it should land
   **after** the PR lane is up, absorbed on a nightly cadence rather than blocking every PR.
   Building the PR lane in Debug is what makes that deferral safe.
4. **P0-8 — `check_runtime_completeness.sh`.** 12 3D entry points are missing from
   graphics-disabled builds. Fix via trap stubs or `RT_GRAPHICS_DISABLED_SOURCES`, then register
   it as a ctest (`audit;tools;requires_posix_shell`). **Not a blocker for CI standup** — the
   script is wired into neither ctest nor any workflow today, so it cannot make the first run
   red. It does mean `-DZANNA_ENABLE_GRAPHICS=OFF` doesn't link, which is worth fixing on its
   own merits.

*Exit criteria:* zero suppressed tests; the differential corpus running on x86-64;
`-UNDEBUG` enforced with a tripwire; full local gate green.

---

## 3. CI standup

The good news the audit turned up: **a test-running CI already exists mechanically.** The three
installer workflows invoke `./scripts/build_zanna_{linux,mac}.sh` / `build_zanna_win.ps1` with no
arguments, which already runs the full default ctest suite plus lint, runtime-surface audit, and
cross-platform smoke. They are simply `workflow_dispatch`-gated. So this is mostly *triggering*
existing machinery, not building new machinery.

**Keep release operations separate.** Do not add PR triggers to the installer workflows. Beyond
the signing secrets and packaging steps (ADR 0025 rejected exactly this), there is a live trap:
every lifecycle step in those files is `if: ${{ inputs.lifecycle }}`, and `inputs.*` is empty on
non-dispatch events — so a `push`/`schedule` trigger would produce a 120-minute run with *less*
coverage than the dispatch run. Add new test-only workflows instead, reusing the same
invocation pattern (job-level `ZANNA_*` env → one call to the platform build script) so the repo
keeps one build contract, not two.

**Also fix a dead trigger:** `runtime-static-analysis.yml` declares `push: branches: [main]`,
but the default branch is `master`. **That push trigger has never fired.** One-line fix.

### Lane matrix

| Workflow | Trigger | Runner | Content |
|---|---|---|---|
| `ci.yml` | `pull_request`, `push: master` | `ubuntu-24.04` | Full build + ctest, full lint, audit, smoke |
| `nightly.yml` | `schedule` + `workflow_dispatch` | matrix: `ubuntu-24.04`, `macos-15`, `windows-2025` | Full build + ctest incl. `slow`, installer smokes |
| `nightly.yml` (experimental) | same | `ubuntu-24.04-arm`, `windows-11-arm` | Build + native-execute proof; **`continue-on-error: true`** |
| `sanitizers.yml` | `schedule` + `workflow_dispatch` | `ubuntu-24.04` | ASan/UBSan, then TSan (mutually exclusive) |
| `fuzz.yml` | `schedule` + `workflow_dispatch` | `ubuntu-24.04` | `ZANNA_ENABLE_FUZZ=ON`, fixed-budget campaign per harness |

Free GitHub-hosted ARM64 runners (`ubuntu-24.04-arm`, `windows-11-arm`) are available because the
repo is public — this is what makes the experimental-ARM64 decision affordable.

All lanes: `permissions: contents: read`, `concurrency` group per-ref with
`cancel-in-progress: true` for `ci.yml` and `false` for nightly, explicit `timeout-minutes`
(start at 120 Linux/Windows, 150 macOS, matching the installer workflows' proven budgets).

### Per-lane configuration

The PR lane runs the build script with the *default* Debug config — deliberately, because Debug
sidesteps the NDEBUG trap entirely (§2 item 3):

```yaml
env:
  ZANNA_BUILD_DIR: build
  ZANNA_BUILD_TYPE: Debug          # keeps product asserts in src/il|vm|codegen live too
  ZANNA_FAST_DEBUG: "1"            # -Og + line-tables-only: disk and time
  ZANNA_SKIP_INSTALL: "1"
  ZANNA_SKIP_CLEAN: "1"            # runner tree is always fresh; clean-all is pure waste
  ZANNA_LINT_CHANGED_ONLY: "0"     # see below — the default makes lint a NO-OP in CI
  ZANNA_CTEST_TIMEOUT: "300"
  ZANNA_TEST_EXCLUDE_LABEL: quarantine
  ZANNA_EXTRA_CMAKE_ARGS: -DZANNA_INSTALL_ZANNASTUDIO=OFF -DZANNA_AUDIO_MODE=REQUIRE
steps:
  - reclaim runner disk            # MANDATORY — see below
  - apt-get install -y ninja-build ccache libx11-dev libasound2-dev xvfb
  - assert Capabilities.hpp really has GRAPHICS/AUDIO/NATIVE_LINK_X86_64 = 1
  - xvfb-run --auto-servernum --server-args='-screen 0 1920x1080x24' ./scripts/build_zanna_linux.sh
```

**Disk is the first thing that breaks, not tests.** A local Debug tree with `ZANNA_FAST_DEBUG=1`
is **14 GB**, of which `build/src/tests` is 11 GB across **935 separately-linked test
executables**. `ubuntu-24.04` runners ship ~25–29 GB free. Without a reclaim step
(`rm -rf /usr/share/dotnet /opt/ghc /usr/local/lib/android /opt/hostedtoolcache/CodeQL`) the lane
dies on ENOSPC and looks like a mystery failure. Log `df -h /` after the build.

**`ZANNA_LINT_CHANGED_ONLY=1` (the default) makes the lint stage a no-op in CI** —
`lint_platform_policy.sh:156` diffs against `HEAD`, which is empty on a clean checkout. Flipping
it to `0` is free: the full-tree lint is clean today.

**Add a capability assertion step.** There is no `ZANNA_BUILD_HAS_GRAPHICS` guard anywhere in
`src/tests/`, so the 140 `requires_display` + 147 `graphics3d` tests register *unconditionally*.
If a runner image ever drops libx11/libasound, you get failures or vacuous passes, never skips.
A 60-second `grep` on the generated `Capabilities.hpp` fails fast instead of at minute 80.

### Graphics, audio, and the 144 display tests

The current Linux installer workflow installs **neither `libx11-dev` nor `libasound2-dev`**, and
both libraries silently disable their subsystem when absent
(`src/lib/graphics/CMakeLists.txt:136-150`, `src/lib/audio/CMakeLists.txt:67-78`). So CI today
builds Zanna with graphics and audio switched off and nobody is told. That is a finding, not just
a config gap.

**Recommendation: install both, and run under `xvfb-run`.** This keeps the 140
`requires_display` + 147 `graphics3d` tests — including all 99 ZannaStudio probes, the only
regression net the 124K-LOC editor has, in the subsystem under heaviest active development.

Critically, **omitting X11 does not make those tests skip** — there is no build-capability guard
in `src/tests/`, so they register regardless and simply fail or pass vacuously. The status quo
(neither library installed) is therefore the *worst* option available, not a neutral one.

`ZANNA_GRAPHICS_BACKEND=HEADLESS` for the whole lane would let them run but bind them to the mock
platform, which would never catch an X11 or Wayland-selector regression — and `LinuxAuto` is what
actually ships to users. Headless coverage is not lost either way: `linux_headless_graphics_smoke`
(`src/tests/tools/CMakeLists.txt:146-155`) already does a nested HEADLESS configure+build and is
in the default suite. Under Xvfb, `WAYLAND_DISPLAY` is unset so the auto-selector falls through
to the X11 adapter — the real path X11 desktop users hit. Wayland stays untested in CI; that is
an accepted gap, not an oversight.

Audio needs no sound device: `zanna_apply_audio_test_environment()` stamps `ZANNA_AUDIO_SILENT=1`
onto every test and `src/lib/audio/src/vaud.c:530` honors it.

Note the label machinery already injects `ZANNA_GFX_NO_ACTIVATE=1` / `ZANNA_GFX_HIDE_WINDOWS=1`
for `requires_display` and `graphics3d` tests (`TestHelpers.cmake:136-142`), so windows won't
fight the virtual display.

### Caching

**ccache only — never cache `build/`.** `build_zanna_unix.sh:151-155` already auto-enables the
ccache compiler launcher when the binary is on PATH, so installing the package *is* the whole
integration; zero script changes on Unix.

Key on `runner.os` + `runner.arch` + config + SHA, with a prefix restore-key. Use the
`actions/cache/restore` + `actions/cache/save` split with `save` under `if: always()` — the
combined action skips its save when the job fails, which is exactly when the partial cache is
most valuable.

Why this is safe against the clean-build invariant: **ccache keys on preprocessed source plus
compiler identity, not on build-tree state.** `clean-all` exists to stop stale objects in
`build/` being reused; ccache reuses nothing from `build/`. So `ZANNA_SKIP_CLEAN=1` on a
fresh runner tree plus ccache preserves the invariant exactly. Caching `build/` is the thing that
would break it. Set `CCACHE_COMPILERCHECK=content`; do **not** add `CCACHE_SLOPPINESS=time_macros`
(there are zero `__DATE__`/`__TIME__` uses in `src/`, so it buys nothing and opens a real hole).

Expect ~0% hit cold (**70–100 min** first run on a 4-core runner), 70–90% warm (**25–40 min**).
The floor is linking 935 test executables, which ccache cannot accelerate.

**Windows ships uncached.** `build_zanna_win.ps1` has no ccache/sccache support, and MSVC's
default `/Zi` is incompatible with sccache (needs `/Z7`, a script change). It is nightly-only, so
a long run is tolerable. File sccache as a follow-up.

### ARM64 experimental lanes

The differential-corpus un-gate (§2 item 1) is what makes these lanes possible, and it delivers a
*stronger* proof than "executes one generated instruction": 33 generated programs, each byte-diffed
against the reference interpreter. Because native asm and link are the pipeline defaults, it needs
no external assembler or linker — which is precisely what makes it viable on `windows-11-arm`.

Both lanes use the same two-step shape: a **required** proof step
(`ctest -R '^differential_'`) and an **advisory** full suite (`continue-on-error: true`). That
gives a meaningful red/green signal on day one without demanding 2,000 green tests on a platform
shipping as untested.

- **Linux ARM64** (`ubuntu-24.04-arm`): `ZANNA_BUILD_NATIVE_LINK_AARCH64=1` already. The gate at
  `src/tests/CMakeLists.txt:393` already registers ~70 `test_codegen_arm64_*` tests here — they
  have simply never run outside Darwin. Expect a substantial first-run failure batch from
  Mach-O-shaped assumptions; the advisory step absorbs it.
- **Windows ARM64** (`windows-11-arm`): line 393's `AND NOT WIN32` excludes all ~70 command
  tests, so today *nothing* ARM64-specific runs. **Leave that carve-out in place** — porting 70
  command tests to Windows process/assembler semantics is its own project, not a standup item.
  The differential corpus is the proof. **P3-30 (aarch64 stack probes) gates this lane being
  honest**, since frames >4 KB walk past the guard page.

Both are documented as experimental in `docs/cross-platform/platform-differences.md` until green
for a sustained period. If even the proof step fails, that is itself a documented finding —
which is the stated goal of shipping them as "experimental, untested."

### What must land before the first workflow is enabled

1. **§1** — ADR 0195 + the three CLAUDE.md edits. Required first: workflow changes are
   ADR-gated, so this is a prerequisite, not an afterthought.
2. **`quarantine` label + `ZANNA_TEST_EXCLUDE_LABEL`.** Add `quarantine` to the whitelist in
   `TestHelpers.cmake:5-53` (next to the already-present-but-unused `windows_broken`), and add
   `ZANNA_TEST_EXCLUDE_LABEL` to both build scripts mirroring the existing `ZANNA_TEST_LABEL`
   handling (~4 lines each). This is the escape hatch everything below depends on. Note
   `SKIP_RETURN_CODE 77` is set only by the helpers, *not* by the 287 raw `add_test` calls — so a
   label, not exit-77, is the right mechanism for the Studio probes most likely to need it.
3. **`runtime-static-analysis.yml`: `main` → `master`.** One line; turns on a trigger that has
   never fired.
4. **§2 item 1** — un-gate the differential corpus. Verify on macOS ARM64 first (should be a
   no-op there).
5. **§2 item 2** — un-suppress the known failures, so a red board always means *this* PR.
6. **`ci.yml` landed non-required** — `workflow_dispatch`-only for the first few runs, or
   `continue-on-error: true`. Collect the real failure list. **Do not add branch protection yet.**
7. Triage: quarantine the flaky (with an issue link and a 30-day expiry), fix the real. Then flip
   to required and add the status check.
8. **§2 item 3 (NDEBUG) + tripwire** — deliberately *after* the PR lane, because it is a no-op in
   Debug and the highest-risk change in the plan. Verify locally with
   `ZANNA_BUILD_TYPE=Release ./scripts/build_zanna_mac.sh` first.
9. **`ci_full_sanitizer.sh --only <asan|ubsan|tsan>`** — prerequisite for the sanitizer jobs: the
   script currently builds three ~14 GB trees *sequentially in one workspace* and will ENOSPC.
   While in there, delete the `> /dev/null 2>&1` redirects on its configure/build calls — today a
   failure exits non-zero with zero diagnostics, which makes the lane un-debuggable.
10. **`nightly.yml`** — three platforms required, two ARM lanes advisory.
11. **§2 item 4** (`check_runtime_completeness.sh` fix + ctest registration) — safe to do any
    time; it is wired into nothing today.

Deferred follow-ups: un-gating `src/tests/CMakeLists.txt:393` for Windows ARM64, sccache on
Windows, a headless-Wayland compositor lane.

**The sequencing insight:** because Debug never defines NDEBUG, the assertion trap is invisible to
the PR lane. Steps 1–7 therefore stand up a meaningful, green PR gate *without* touching the
618-file assertion blast radius. Step 8 re-arms Release separately, on a nightly cadence, instead
of blocking every PR.

### First-run failure budget

Assume the first run of every lane fails, and plan for it rather than being surprised.

**Linux x64 PR lane** — no ctest has ever run on Linux in CI, so this is the big one:
- *Disk fails before tests do.* Covered above; without the reclaim step this looks like a
  mystery.
- *287 first-time display/3D tests.* Pre-emptively quarantine `zia_zannastudio_editor_hot_path`
  — it enforces two 250 ms wall-clock budgets (`src/tests/CMakeLists.txt:1239-1247`) tuned on the
  dev Mac, and a 4-core cloud VM is materially slower. `zia_zannastudio_phase2_phase3` (120 s
  timeout, spawns two compilers plus a debug adapter) is the second candidate. Better to
  quarantine these two knowingly than to discover them.
- *`linux_headless_graphics_smoke`* does a nested configure+build under a 180 s timeout with no
  ccache. If it trips, **raise the timeout rather than quarantining** — it is the only
  headless-backend coverage.
- *33 new `differential_*` at `ARCH=x64`.* Expect 0–6 genuine divergences. These are real bugs
  and the highest-value output of the standup, and they are exactly why step 6 lands
  non-required.
- *Lint* goes full-tree; verified clean today, so this is free.

**Nightly Windows x64** — the build is already proven by the dispatch-only workflow, so
config/compile risk is near zero. New risk is entirely the NDEBUG fix under Release plus 33
`differential_*` on PE/COFF, never previously exercised. **Verify the build script's ctest
failure actually propagates to the exit code** — `build_zanna_win.ps1:430-432` sets
`$validationFailed` rather than exiting immediately, so a failing suite could report success.
Confirm before trusting that lane as a gate.

**Nightly macOS arm64** — closest to the dev box, lowest risk; its only new failure source is the
NDEBUG fix under Release.

**Sanitizer lanes** will be red on first run: ~2,000 tests under ASan/UBSan on a codebase with
memory-safety fixes still in flight (§4c). Land `continue-on-error: true` and burn down. TSan is
scoped to `-L "vm|runtime"` and is the most likely to be loudly red.

**Fuzz lane** — corpus replay is deterministic and should be green immediately (all 23 corpora
exist under `src/tests/fuzz/corpus/`, and `src/tests/fuzz/CMakeLists.txt:36-41` already registers
a `_replay` ctest per harness when `ZANNA_ENABLE_FUZZ=ON`). New-crash discovery is the variable
part — upload `crash-*` artifacts on failure or the reproducer is lost.

**Posture:** run the Linux lane advisory for one week. Anything still failing at day 7 is either
fixed or quarantined with an issue link and a dated expiry. Only then attach the required status
check. The `NO_CTEST`/comment-out pattern that produced P1-4 is exactly what not to repeat.

*Exit criteria:* `ci.yml` green and required on a real PR; nightly matrix green two consecutive
nights; the x86-64 differential corpus running and green; zero tests excluded without a dated
issue.

---

## 4. Wrong code and crashes (P0)

Fix order is deliberate: miscompiles first, because they produce incorrect programs with **no
diagnostic**, which is strictly worse than a crash.

### 4a. The three codegen miscompiles — fix the *pattern*, not three sites

- **P0-9** `src/codegen/x86_64/peephole/MemoryOpt.cpp:87-101` — `definesOperandReg` walks explicit
  operands only, so store-forwarding survives `CQO`/`IDIV`/`DIV`/`MUL`/`IMUL` implicit RAX/RDX
  defs and deletes a live reload.
- **P0-10** `src/codegen/x86_64/ra/Allocator.cpp:494-521` — `canCarryIntoNextBlock` uses a
  *syntactic* vreg scan, so a `JMP`-only forwarding block counts as transparent and a value
  flowing through it gets a fresh register with no reload. Also `:229` checks `mem->base.isPhys`
  but never `mem->index`.
- **P0-11** `src/codegen/aarch64/peephole/PeepholeCommon.cpp:180-365` — hand-maintained copy of
  `ra::operandRoles` defaulting to "not used", missing `StrRegSpImm`/`StrFprSpImm` (outgoing
  stack arguments) and `FRintN`.

**Structural fix:** P0-9 and P0-11 are both divergent second copies of a model whose primary copy
is correct (`MovFolding.cpp:121-138` has the right implicit-def switch; `ra/OperandRoles.cpp:221`
even throws on unclassified operands). Route the peephole passes through the RA's authoritative
tables rather than patching three call sites, and audit for further copies while in there.
Flip the aarch64 default to conservative.

Related, same family, fold in: P3-29 (`CopyPropDCE.cpp:823-848` treats unconditional control
transfer as proof of deadness), P3-31 (TBZ/TBNZ fusion erases a `TST` without checking flag
readers), P3-32 (block-local DCE omits callee-saved v8–v15), P3-33/P3-34 (scheduler `OpRipLabel`
modeling; `nextInstrReadsFlags` end-of-block assumption — add asserts).

### 4b. `RESUME NEXT` (P0-1)

`src/bytecode/BytecodeVM.cpp:4342-4343` records the resume point at *bytecode-word* granularity
paired with the `eh.push` operand depth; when the faulting IL instruction produces a result the
next word is its own `STORE_LOCAL`, which pops an empty stack. Record the resume point at
IL-instruction granularity. **Rebuild the parity test from real compiler output** — the existing
one (`src/tests/unit/test_bytecode_vm.cpp:1959-2032`) hand-writes bytecode that doesn't model
what `BytecodeCompiler::storeResult` emits, which is why this shipped.

### 4c. Memory safety

- **P0-2** `rt_tls.c:1425` — cap plaintext records at 2^14 and decrypted inner plaintext at
  2^14+1; give `recv_record` an explicit capacity parameter (it currently has none while callers
  pass 16 KB stack buffers).
- **P0-3** `rt_mp3.c:743`/`:1099` — decode into fixed scratch, validate channel/sample
  consistency, then copy. The guard currently runs *after* the write.
- **P0-4** `rt_gltf_draco.inc:3058` — 64-bit product check plus a
  `num_values`-vs-`att->num_values` consistency assert; this also closes the sibling sites at
  `:3155`, `:3190`, `:3284-3289`, `:3422`. Use the existing `rt_untrusted_count_ok` helper that
  the rest of the file already uses.
- **P0-5** `vgfx_platform_macos.m:940` — IME marked-text UAF. **Fix by compiling the file with
  `-fobjc-arc`** (`src/lib/graphics/CMakeLists.txt:131-134`), which also closes P2-9's view,
  delegate, and per-`set_title` menu-tree leaks. Same root cause.
- **P0-6** `rt_pty.c:820-838` — port the drain + timed-wait loop from `rt_process.c:783-816`.

### 4d. Parser blowup (P0-7)

Three compounding defects in `src/il/io/` and `src/il/verify/`: the per-instruction deep-copy
rollback guard (`InstrParser.cpp:67-103`, measured 4× per doubling — 32k instructions = 14.55 s),
`valueNames.resize(reservedId + 1)` driven by a parsed token (`OperandParse_ValueDetail.cpp:212`
— a 72-byte file peaks at 490 MB RSS), and per-instruction block re-walk in demotion
(`InstructionChecker.cpp:684-713`). Make the guard journal-based, validate `reservedId` against
actual temp count, memoize the range analysis per block — ADR 0026:78 already names the memo as
the intended follow-up. Also P3-9 (`maxTempsPerFunction` bypassable for non-`%tN` names).

**ADR likely required** (precedent ADR 0111 ADR'd parser resource bounds as a parser-contract
change) if any limit tightens or any previously-accepted input becomes rejected.

*Exit criteria:* differential corpus green on all lanes; a fuzz harness exists for MP3, Draco,
and the IL parser and runs clean for a fixed budget; `RESUME NEXT` parity test built from
compiler output passes on interpreter + both bytecode modes.

---

## 5. Close the 3D scene-format gap (P2-1)

The highest-leverage product item. **Check first whether a format change is needed at all:**
ADR 0185 already defines the collider convention as *typed node metadata* (`collider.kind`,
`collider.halfX/Y/Z`, `collider.radius`, `collider.height`, `collider.trigger`) and states it
rides VSCN v6 with no format change. The gap is that **the runtime never reads it** — `grep
collider.kind src/runtime/ src/il/runtime/defs/` returns zero hits.

Preferred approach, cheapest first:

1. **Ship an official runtime adapter** for the ADR-0185 metadata so `World3D.Spawn` and streamed
   cells attach bodies automatically (`rt_game3d_world_sim.inc`, `rt_game3d_streaming.inc:1770`,
   `rt_game3d_bodydef.c`). This alone converts "scene file + adapter" into "scene file" and lets
   `examples/games/ashfall-scenes/world/level_base.zia:394-398` delete its hand-built physics.
2. **Then** a scene→gameplay binding layer: trigger volumes, spawn points, tagged markers, and a
   component registry mapping metadata prefixes to engine objects (audio source, particle
   emitter, decal, reflection probe, water, terrain).
3. **Only if** native fields (mass/friction/restitution) prove necessary, cut VSCN v8 with a full
   ADR in the Graphics3D group (precedent: 0141/v4, 0146/v5, 0159/v6, 0187/v7).

Riding along in the same subsystem: **P2-2** (streaming leaks the whole stream if `Update()`
stops — single drain point at `rt_game3d_streaming.inc:2669`, while the asset queue drains from
three places) and **P2-3** (async asset worker finalizes GC objects off-thread at
`rt_game3d_asset_load.inc:814-820`; the streaming subsystem's CAS fallback stack at `:2343-2346`
is the pattern to copy). Also **P3-18** (commit queues never freed, so cancel paths are dead
code).

Docs that must move: `docs/graphics3d-guide.md:1423` (accepted VSCN range), `:1426+`, `:1436-1438`
(ADR cross-links), `:1218-1219` (Save/Load table).

*Exit criteria:* `ashfall-scenes` loads a mission with physics, triggers, and spawns attached by
the runtime with no per-game adapter; `zia_ashfall_scene_parity` still green.

---

## 6. Runtime and platform completeness

Grouped by owner-visible outcome, not by file.

**Text and localization**
- **P2-4** Wire the existing dependency-free TTF rasterizer (`src/lib/gui/src/font/vg_ttf.c`)
  into the 2D text path and add a `BitmapFont.LoadTtf` / `Font` surface. This is *wiring*, not
  new code, and it unblocks every non-English 2D game. **ADR required** (new runtime C ABI).
  Updates `docs/zannalib/graphics/fonts.md:9,37-38,80`.
- **P2-11** Timezone data: five zones with transitions expiring 2026-12-31
  (`rt_tzdata_generated.inc:37`). Embed a real tzdata subset or read the host tzdb via a platform
  adapter.

**API contract**
- **P2-12** Apply the proven `*Result` pattern to the top-20 fallible APIs that still trap:
  `Data.Json.Parse`, `IO.File`/`IO.Dir`, `Network.Http.Get/Post`, `IO.Archive.Open`,
  `IO.Assets.Load`, `Csv/Toml/Ini.Parse`. `Xml`/`Yaml`/`Serialize` already have the shape.
- **P2-13** Replace the stability heuristic (`src/tools/zanna/main.cpp:568-583`, which classifies
  the entire 1,131-function GUI toolkit as `preview` by string prefix) with declarative
  `RT_STABILITY`/`RT_CAPABILITY` rows in the `.def` files. Then decide the GUI tier deliberately.

**Platform**
- **P2-10** Audio device-loss recovery: `IMMNotificationClient` on WASAPI
  (`vaud_platform_win32.c:546-604` currently exits the worker permanently after 8 failures) and
  `-ENODEV` recovery on ALSA (`vaud_platform_linux.c:286-295`). macOS is already correct.
- **P3-1** Cursor types 6–12 clamped to arrow on macOS/Windows despite both files' tables
  supporting 0–12. **P3-2** Win32 `PeekMessageW` HWND filter → 100% CPU spin. **P3-3** DPI:
  adopt `PER_MONITOR_AWARE_V2` (the handler is already written) or delete the dead handler.
  **P3-4** X11 `Clipboard.HasText` does a full transfer — answer TARGETS instead.
- **P3-30** aarch64 stack probes — **promoted to blocker** by the ARM64-experimental decision.
  x86-64 has them (`FrameLowering.cpp:483-527`); `FrameBuilder.cpp`/`FrameCodegen.hpp` have none.

**Frontend quality**
- **P2-14** Zia diagnostics: dedup in `DiagnosticEngine::report` (every undefined identifier is
  reported twice); report the unresolved *member* not the root namespace
  (`Sema_Expr.cpp:377`); replace substring-based code classification
  (`Parser_Tokens.cpp:46-74`); add `--max-errors`; emit JSON diagnostics from the plain CLI path.
- **P2-15** Resolve the BASIC case-sensitive qualified-call bug — either
  `docs/languages/basic-namespaces.md:1213-1228` is stale or `ProcRegistry.cpp:257` is a partial
  fix. 15 minutes, and it is the only self-documented correctness bug in a shipping language.

**Installers and packaging**
- **P2-5** Windows journal crash window (`WindowsInstallerLifecycle.cpp:3292-3293`, recovery at
  `:3240-3246`) can delete `old/` while committing without metadata. At `OldMoved` recovery with
  `installRoot` present and `newRoot` gone, treat as `NewActive` and roll back fully. Move the
  `maybeInjectFailure("after-new-move")` hook *before* the journal write so the window is
  testable. Same shape in `performUninstall` (`:3349-3356`).
- **P2-6** macOS: add unowned-collision preflight, free-space check, and manifest-journaled
  rollback — it currently has none of the three Windows guarantees.
- **P2-7** macOS Studio `.app` bundle (or document terminal-launch honestly).
- **P2-8** Gate `drawInstallerBackdrop` (`WindowsInstallerTheme.cpp:488`) on `productKind` so a
  shipped game's installer stops advertising "ZANNA — CODE. CREATE. COMPILE. CONQUER."
- Add a Linux app-packaging e2e mirroring `windows_installer_xenoscape_smoke`.

**Remaining P3** (fold into convenient batches): P3-5 through P3-8, P3-10 through P3-17,
P3-19 through P3-28. See the punch list for evidence on each.

---

## 7. Editor Phases 1–4

Continue the approved Studio editor Unity-quality program. Phase 0 is landed and gated. (That
program's plan document currently lives outside the repository as a local planning file; consider
promoting it to `misc/plans/` alongside this one so the two programs are tracked in the same
place.) Remaining:

- **Close-out of Phase 0:** 0.3b per-backend headless GPU contexts (Metal is the one genuine TODO
  in the 3D tree, `vgfx3d_backend_metal_context.inc:957`), 0.5d module splits, and the last 0.6
  surface (the 2D object-property trio → `PropertyRows`).
- **Phase 1** — context menus, cross-widget DnD, triangle-accurate picking + marquee + outlines
  (ADR 0193), creation ergonomics, 2D object transforms (ADR 0192).
- **Phase 2** — tile suite, layers, gizmo/camera polish, icons, thumbnails.
- **Phase 3** — scale/fidelity/diagnostics, undo history UI.
- **Phase 4** — play loop + `--scene-watch` hot reload.

Phase 5 (docking) stays out of alpha. The editor program keeps its reserved ADR numbers
0192–0194 unchanged (see §1).

---

## 8. Cut the release

No end-to-end release checklist exists today. Creating one is a genuine gap being filled — put it
at `misc/plans/alpha/appendices/release-checklist.md` mirroring
`misc/plans/runtime_overhaul2/appendices/release-checklist.md`.

Mechanics: edit `src/buildmeta/VERSION` → `0.3.0-alpha` (the CMake regex at `CMakeLists.txt:31-39`
handles the suffix; `IL_VERSION` stays 0.3.0 and moves only under an IL ADR); rename
`docs/release_notes/Zanna_Release_Notes_0_2_99.md` → `_0_3_0.md`, drop the DRAFT marker, set a
real date; update `docs/README.md:85-87` and `docs/zannalib/README.md:7-10`; bump `last-verified`
on every touched doc; run `./scripts/check_docs.sh` and `./scripts/update_generated_docs.sh`
(P3-28: generated runtime docs are 12 functions stale); run the
`misc/reviews/review-readiness.md:14-28` sequence; tag `v0.3.0-alpha`.

**Still gated on owner action:** Authenticode and Apple Developer ID certificate procurement.
Every trust path is code-complete but credential-empty, which blocks any public download.

---

## Deferred: post-alpha security hardening track

Recorded so nothing is lost, explicitly **not** in this plan: TLS 1.2 + P-256 interop;
certificate `pathLenConstraint`, revocation, and the POSIX/Windows `ZANNA_TLS_REQUIRE_REVOCATION`
divergence; post-handshake record-type enforcement and close_notify tracking; HPACK
instruction-order bug and decompression budgets; HTTP/2 CONTINUATION flood; HTTP/1 duplicate
framing headers; keep-alive pool poisoning; WebSocket idle and zero-length-frame loops; Wayland
clipboard SIGPIPE; Tiled filesystem-mode path traversal; installer PATH snapshot clobbering.

**One judgment call for the owner:** the HTTP/1 chunked-response bug
(`rt_network_http_response.inc:679-690` — the overflow guard sits *inside* the doubling loop and
aborts instead of doubling again) breaks **every** response chunked above 8 KB against ordinary
nginx/Node/CDN servers. That is a functional break, not hardening. Recommend promoting it into
§6 despite the security-track scoping.

---

## Verification

Per-item verification lives in the punch list's **Fix:** lines. Program-level gates:

1. `./scripts/build_zanna_unix.sh` with **no skip flags** green on macOS ARM64 and Linux x64.
2. `.\scripts\build_zanna_win.ps1` green on Windows x64.
3. The PR lane green on a real pull request; the nightly matrix green two consecutive nights.
4. `diff_vm_native` and the 33-program byte-diff corpus green on Linux x64 and Windows x64 —
   the gate that would have caught P0-9 and P0-10.
5. `zia_xenoscape_scene_parity` and `zia_ashfall_scene_parity` still byte-exact after §5.
6. `./scripts/lint_platform_policy.sh --strict`, `./scripts/audit_runtime_surface.sh --strict…`,
   `./scripts/check_runtime_completeness.sh`, `./scripts/check_docs.sh` all green and all
   ctest-registered.
7. Sanitizer lanes (ASan/UBSan/TSan) and the fuzz corpus run clean for a fixed budget on the
   parsers touched by P0-2/3/4/7.
8. Manual smoke of Studio on all three platforms against `xenoscape-scenes` and `ashfall-scenes`.
