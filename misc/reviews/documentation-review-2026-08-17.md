---
status: active
audience: contributors
last-verified: 2026-08-17
---

# Documentation Review — 2026-08-17 (100-Document Sample)

A verification pass over a random 100-document sample of `docs/` (543 markdown
files total), checking every claim that can be mechanically confirmed against
the live registry, the built binaries, the source tree, and CTest.

The sample was drawn deterministically across all documentation areas: 54 ADRs,
16 `zannalib` guides, 6 book chapters, 5 codemaps, 4 generated runtime pages,
6 audit records, 2 release notes, and 7 assorted guides.

## Method

Every claim class below was checked against ground truth, not read for
plausibility:

- **Runtime API names** — 824 distinct `Zanna.*` names extracted from the sample
  and diffed against `zanna --dump-runtime-api` (7,964 functions / 536 classes).
  After the corrections below, every name that still does not resolve is either
  explicitly labelled retired in a Superseded note or an intentional negative
  example.
  The installed and build-tree binaries were confirmed to emit byte-identical
  registries first, so findings are not an artifact of a stale install.
- **Code samples** — 492 fenced `zia`/`basic` blocks extracted; the 89 that are
  complete programs were compiled with `zanna check --diagnostic-format=json`.
  A wider pass over the book and tutorials compiled 211 complete Zia programs.
- **C symbols** — 700 distinct `rt_*` identifiers diffed against the 11,986
  symbols declared under `src/runtime/`, `src/il/runtime/`, and `include/`.
- **Generated pages** — `docs/generated/runtime/` regenerated via
  `cmake --build build --target generate_runtime_reference`; zero diff.
- **Test names** — cited CTest names diffed against the 2,029 registered tests.
- **Paths and links** — every repo path and relative link in the sample resolved.
- **Diagnostic codes** — 19 cited codes diffed against the 188-entry catalog from
  `zanna --print-error-codes --json`.
- **CLI behaviour** — documented invocations executed and their printed output
  compared with the documented output.
- **Conventions** — `./scripts/check_docs.sh` plus frontmatter and index audits.

## Corrected — Stale Runtime API Claims

### VDOC-285 — ADR 0036 records retired format/frame aliases as live

`docs/adr/0036-format-and-frame-abbreviation-aliases.md` stated that `NumSci`,
`NumPct`, `BoolYN`, and `SetDTMax` were kept as compatibility aliases and that
"Runtime API dumps expose both names for now". Commit `f7b335148`
("feat(runtime): standardize the public API surface", 2026-07-15) removed all
four. Only `Zanna.Text.Fmt.Scientific`/`Percent`/`YesNo` and
`Zanna.Graphics.Canvas.SetMaxDeltaTime` / `Zanna.Graphics3D.Canvas3D.SetMaxDeltaTime`
remain registered. **Corrected** with a Superseded status note in the idiom of
ADR 0031 and ADR 0062.

### VDOC-286 — ADR 0044 documents a namespace that no longer exists

`docs/adr/0044-crypto-module-process-policy-api.md` documented
`Zanna.Crypto.Module.*`. Nothing under that namespace resolves; the same commit
moved the surface to `Zanna.Crypto.Compliance` and dropped the short
`EnableApprovedMode` / `DisableApprovedMode` / `IsApprovedMode` rows. The
registry now holds exactly `EnableApprovedModeForProcess`,
`DisableApprovedModeForProcess`, `IsApprovedModeForProcess`, and `Status`.
**Corrected** with a Superseded status note.

### VDOC-287 — ADR 0028 claims `Terminal.Ask` and `Terminal.InputLine` remain registered

`docs/adr/0028-terminal-option-result-input-apis.md` Decision states that
"`ReadLine`, `Ask`, and `InputLine` remain registered for source and IL
compatibility". Only `Zanna.Terminal.ReadLine` survives. The four APIs the ADR
adds (`TryReadLine`, `TryAsk`, `ReadLineResult`, `AskResult`) are all present.
**Corrected** with a partial-supersession status note.

### VDOC-288 — ADR 0061 documents an `ErrorOption` row that was never registered

`docs/adr/0061-zia-semantic-job-error-option-api.md` specifies adding
`Zanna.Zia.SemanticJob.ErrorOption` alongside a string-returning `Error`. The
implementation folded the Option accessor into the canonical name instead:
`extended_tooling.def:632` registers `Error` as `obj<Zanna.Option>(obj)` backed
by `rt_zia_semantic_job_error_option`. There is no `ErrorOption` row and no
string-returning `Error`. **Corrected** with a Superseded status note.

### VDOC-289 — `zannalib/crypto.md` teaches retired AES aliases

Two claims were wrong: `Encrypt`/`Decrypt` described as live "AES-CBC
compatibility helpers", and "The old `Zanna.Crypto.Aes.Encrypt`, `Decrypt`,
`DecryptResult`, and `TryDecrypt` names remain as compatibility aliases". None
of the four is registered. The namespace was also misspelled `EncryptCBC` where
the registry has `EncryptCbc`. **Corrected**.

### VDOC-290 — `zannalib/core.md` names a non-existent introspection class

`Zanna.Core.ValueType` does not exist; the hook is
`Zanna.Runtime.Unsafe.ValueType`, which the preceding bullet in the same file
already named correctly. **Corrected**.

### VDOC-291 — ADR 0177 and `zannalib/game/scene.md` use wrong class namespaces

Both cited `Zanna.Graphics2D.Camera` and `Zanna.Game2D.Lighting2D`. The
registered classes are `Zanna.Graphics.Camera`
(`defs/classes/localization.def:963`) and `Zanna.Game.Lighting2D`
(`defs/classes/game_ui.def:275`). **Corrected in both files**.

## Corrected — Broken Code Samples

### VDOC-292 — `zannalib/audio.md` example fails the IL verifier

The mix-group example called `Zanna.Input.Keyboard.WasPressed(Zanna.Input.Keyboard.KeySpace)`.
`KeySpace` does not exist; the key constant is `Zanna.Input.Key.Space` (ADR 0040).
The block failed BASIC lowering with a `call arg type mismatch`. **Corrected**;
the block now compiles clean.

### VDOC-293 — `zannalib/core.md` calls a non-existent String method

`PRINT "abc".Cmp("abd")` fails with `E_NO_SUCH_METHOD no such method 'CMP' on
'Zanna.String'`. The method documented in the same file's table at line 499 is
`Compare(other)`. **Corrected**.

### VDOC-294 — `tutorials/basic-tutorial.md` file-I/O examples do not compile

`LINE INPUT #n, VAR$` does not implicitly declare its target (unlike console
`LINE INPUT`), so both the File Operations example and the File Copy Utility
failed with `B1001 unknown variable`. The canonical fixture
`src/tests/golden/basic/eof_compare.bas` declares `DIM LINE$` first.
**Corrected**; both examples now compile clean.

### VDOC-295 — `book/26-performance.md` documents profiling flags on the wrong tool

`zanna -run --count program.il` and `zanna -run --time program.il` print the
usage banner and run nothing. `--count` and `--time` belong to `ilrun`, and its
own help places the file before the flags. **Corrected** to
`ilrun program.il --count` / `--time`. The neighbouring `zanna run --profile`
claim is accurate — it prints `[SUMMARY] instr=… time_ms=…` exactly as
documented.

## Corrected — Stale Structural References

### VDOC-296 — `codemap/front-end-common.md` lists deleted files and omits current ones

The "Runtime Registry" section listed `RuntimeRegistry.cpp`/`.hpp`, deleted from
`src/frontends/common/` by commit `9cc6ca2cc`. The four files that actually
carry that role — `CollectionMethodCatalog.{cpp,hpp}` and
`RuntimeMethodResolver.{cpp,hpp}` — were absent from the codemap entirely.
**Corrected**; the section is now "Runtime Metadata" and the file total of 23
matches the tree exactly.

### VDOC-297 — `codemap/il-build.md` declares a non-existent extern

The `IRBuilder` usage sample declared `rt_print`. No such symbol exists; the
`(str) -> void` printer is `rt_print_str`, as every IL sample in
`docs/il/il-guide.md` uses. **Corrected**.

### VDOC-298 — `internals/native-linker.md` cites a non-existent smoke test

`native_smoke_3dbowling_build_arm64` is not registered.
`scripts/run_cross_platform_smoke.sh:185` runs `native_smoke_chess_ai_arm64`,
`native_smoke_crackman_movement_arm64`, and
`native_smoke_zannastudio_completion_arm64`. **Corrected** to the real names.

### VDOC-299 — ADR 0226 cites unqualified probe names

`scene_hierarchy_affordances`, `scene_capacity`, and `scene_capacity_65k` are
registered as `zia_zannastudio_scene_*`, and the same sentence already spelled
`zia_zannastudio_scene_editor_3d` in full. **Corrected** for consistency.

### VDOC-300 — ADR 0266 gives the wrong baseline path

The Decision cited `scripts/architecture_baseline.tsv`; the file is at
`src/zannastudio/scripts/architecture_baseline.tsv`, which the ADR's own
Consulted section already spells correctly. **Corrected**.

### VDOC-301 — ADRs 0018–0024 cite a retired review document

All seven cite `misc/plans/zannastudio/gui-runtime-additions.md`. The document
lived at `misc/plans/viperide/gui-runtime-additions.md` and was removed by
commit `affd5f6f0` (2026-07-15). Only ADR 0024 was sampled, but a partial fix
would leave six siblings inconsistent, so **all seven were corrected** to record
the review as retired while keeping the R1–R7 recommendation ids.

## Corrected — Convention and Index Conformance

### VDOC-302 — Ten ADRs missing from the ADR index

`docs/internals/doc-style.md` requires every ADR to have a row in
`docs/adr/README.md`. ADRs 0233–0241 and 0257 had none. **Corrected**; all 267
ADRs are now indexed.

### VDOC-303 — ADR 0257 missing frontmatter, failing the docs gate

`./scripts/check_docs.sh` failed on this file. **Corrected**; its heading was
also normalized from the em-dash form to the template's `# ADR NNNN: Title`
(265 of 267 ADRs already use it).

### VDOC-304 — ADR 0252 untagged code fence

Second `check_docs.sh` failure. **Corrected** to a `text` fence.
`./scripts/check_docs.sh` now reports ALL DOC CHECKS PASSED.

### VDOC-305 — Ten documents use undeclared `audience:` values

The style guide declares `audience: public | contributors`. Eight files used
`developers`, one `internal`, one `developers, users`. **Corrected** across all
ten: `docs/languages/interop.md` to `public` (it is indexed in the user-facing
`docs/README.md`), the rest to `contributors`.

## Corrected — Stale Metrics

### VDOC-306 — `gameengine/examples/README.md` counts are wrong throughout

The gallery claimed 15 games while listing 11, and nine of the eleven LOC/file
figures had drifted — several by more than 3×, in both directions:

| Game | Claimed | Actual |
|---|---|---|
| XENOSCAPE | 17,005 / 26 files | 38,802 / 68 files |
| Chess | 4,000+ / 15+ files | 8,219 / 24 files |
| Crackman | 2,230 / 9 files | 7,042 / 30 files |
| Graphics Show | 8,000+ / 10+ files | 3,675 / 14 files |
| Frogger | ~1,500 | 752 / 1 file |
| Centipede | 2,553 / 10 files | 2,552 / 12 files |
| Fade Test | 168 | 167 |
| VTris | 1,132 | 1,721 |
| Frogger BASIC | 1,320 | 1,691 |
| Pac-Man BASIC | 450 | 1,514 |
| Centipede BASIC | 450 | 1,688 |

Three feature counts were also wrong:

- XENOSCAPE's "10 JSON-based levels" — `LEVEL_COUNT = 10` is correct, but levels
  are authored in `level.zia` as tilemaps plus spawn records; the only `.json`
  in the project is a runtime config. Its WAV count is 12, not 11.
- Centipede's "6 enemy types" — the parenthetical in the same sentence already
  listed the four that exist (`centipede.zia`, `spider.zia`, `flea.zia`,
  `scorpion.zia`).
- Graphics Show's "10 visual demos" — `main.zia:132-139` registers eight menu
  entries; the sentence then listed only nine, one of which (`colors.zia`) is
  not reachable from the menu at all.

**All corrected.**

Two markdown defects in the same file were also fixed: three feature-matrix rows
(`Lighting2D`, `Save System`, `Achievements`) carried a seventh cell in a
six-column table, and a horizontal rule with no preceding blank line was
rendering the paragraph above it as a heading.

## Fixed in Code

Three findings were defects in the implementation rather than the prose. In each
case the documentation described the intended design correctly, so the repair
went into the code and the docs were left alone (except where a new contract
needed recording).

### VDOC-307 — `DECLARE FOREIGN FUNCTION` rejected by the missing-return check

Both `docs/tutorials/basic-tutorial.md` and `docs/languages/interop.md` document
bodyless foreign declarations, and the parser and lowerer support them
(`Parser_Stmt_Core.cpp:769`, `Lowerer_Procedure_Emit.cpp:425-426` explicitly
emit a declaration with no body). Semantic analysis nevertheless applied the
missing-return rule to them:

```text
$ zanna check t.bas
t.bas:1:17: error[B1007]: missing return in FUNCTION ZIAHELPER
DECLARE FOREIGN FUNCTION ZiaHelper(n AS LONG) AS LONG
```

`DECLARE FOREIGN SUB` was unaffected, because the SUB path has no result-flow
callback. The Zia frontend already had the equivalent guard
(`src/frontends/zia/Sema_Decl.cpp:1168` tests `!decl.isForeign && !decl.body`);
the BASIC semantic analyzer never consulted `isForeign` at all — the flag was
read only by the parser and the lowerer.

**Fixed** in `SemanticAnalyzer_Procs.cpp::analyzeProc` by exempting foreign
declarations from the result-flow requirement, matching the Zia frontend. The
documentation was correct throughout and is unchanged.

**Test:** `src/tests/unit/test_basic_declare_foreign.cpp`
(`test_basic_declare_foreign`), six cases covering both frontend layers:

- a foreign FUNCTION analyzes with zero errors and emits no `B1007`;
- a foreign SUB analyzes cleanly (the path that already worked);
- the two together, as the interop guide pairs them;
- **a non-foreign FUNCTION without a result still reports `B1007`** — this pins
  the exemption so it cannot widen into suppressing the real diagnostic;
- a foreign FUNCTION lowers to `Import` linkage with no blocks, one parameter,
  and an `i64` result;
- a foreign SUB lowers to `Import` linkage with no blocks, no parameters, and a
  `void` result.

Three of the six fail against the unfixed analyzer and all six pass after it,
verified by reverting the guard, rebuilding, and re-running.

### VDOC-308 — `rt_tzdata_generated.inc` was hand-edited and failed its own check

`docs/internals/generated-files.md` records the invariant that the generated
include "must remain byte-for-byte reproducible from the script", and
`python3 scripts/generate_tzdata_subset.py --check` exited 1:

```text
src/runtime/localization/rt_tzdata_generated.inc is stale; run scripts/generate_tzdata_subset.py
```

Diffing the committed file against `render()` showed the drift was
**documentation comments only** — a later doc pass hand-added a Doxygen `@file`
block, an extra `Key invariants` bullet, and per-table `///` comments that the
generator did not emit. Transition data, offsets, and zone records were
identical. Regenerating would have silently deleted the comments.

**Fixed** in `scripts/generate_tzdata_subset.py`: the comment text moved into the
generator, `transition_array()` gained a doc parameter, and the header template
regained the missing invariant and `@file` block. The committed `.inc` is
**unchanged** — the generator now reproduces it exactly. Verified by round trip:
`--check` passes, regenerating produces a zero-byte diff, and `--check` passes
again afterwards. The doc's stated invariant was correct and is unchanged.

### VDOC-309 — Mixed-language projects failed silently with exit 1

Discovered while validating the VDOC-307 fix end to end: with `DECLARE FOREIGN`
analyzing and lowering correctly, the workflow documented in
`docs/languages/interop.md` still did not run. `zanna run` exited 1 and printed
nothing. Three separate defects were involved.

**Silent failure.** `compileMixedProject` builds `Diagnostic` values for its own
link/optimize failures and returns them in an `Expected`; the caller discards the
payload on the assumption that "diagnostics already printed" — true of the
single-language paths, false of the mixed path.

**Duplicate `main`.** Every BASIC module lowers its top-level statements into
`@main`, which also carries the `__mod_init$oop` call and global initialization.
Linking any BASIC library module produced `multiple modules define 'main'`. Zia
only emits `main` for `start()`, which is why the mirror case appeared to work
and the defect looked intermittent.

**Identifier case.** `Lexer.cpp` upper-cases every BASIC identifier as it is
consumed, so `Factorial` reaches the IL as `@FACTORIAL` while Zia emits
`@Factorial`. Neither direction could resolve.

**Fixed** under [ADR 0268](../../docs/adr/0268-cross-language-symbol-resolution.md):
the mixed driver now reports its own diagnostics; a library module's `main` is
renamed and wrapped in a synthesized `() -> void` module initializer, so its
initialization still runs instead of being dropped or colliding; and the linker
retries an unresolved import against a case-folded index, binding a unique match
and reporting ambiguity as an error. The fallback runs only after exact
resolution fails, so it cannot change how any already-linking program binds.

Alternatives are recorded in the ADR — notably preserving the BASIC source
spelling, rejected because the spelling is destroyed in the lexer before the
parser runs, and dropping the library `main`, rejected because it silently
discards global initialization.

**Tests:** four linker cases in `src/tests/il/ModuleLinkerTests.cpp` (unique
case-fold match, call-site rewriting, ambiguity rejected, exact match wins) and
three end-to-end projects under `src/tests/e2e/interop/` registered as
`interop_mixed_basic_entry`, `interop_mixed_zia_entry`, and
`interop_mixed_library_init`. The last one pins that library-side top-level
initialization still runs — the behaviour the naive duplicate-`main` fix would
have broken.

## Verified Clean

- `docs/generated/runtime/` — regenerating produced a zero-byte diff. The four
  sampled pages (`audio`, `game2d`, `threads`, `zia`) are exactly in sync with
  the registry.
- `docs/getting-started.md` — verified end to end. `zanna init --help` matches
  the options table verbatim; `zanna run examples/zbasic/ex1_hello_cond.bas`
  prints the documented `HELLO/READY/10/10`; the Zia hello program runs; the
  REPL transcript reproduces exactly (`Hello from the REPL`, `14`, `42`, `49`);
  `zanna init` produces the documented tree; all seven listed tools resolve;
  CMake ≥ 3.20 and every referenced script exist.
- `docs/installer-release.md` — every `install-package` option, env var,
  workflow file, validator script, and CTest name cited exists.
- `docs/memory-management.md` — pool size classes (64/128/256/512, 4 classes,
  `BLOCKS_PER_SLAB` 64), `RT_HEAP_IMMORTAL_REFCNT = SIZE_MAX - 1`, the intern
  table's FNV-1a open addressing at a 5/8 load factor, `ZANNA_RC_DEBUG`, and the
  full relaxed-retain / release-decrement / acquire-fence ordering protocol all
  match `rt_heap.c`, `rt_pool.c`, and `rt_string_intern.c`.
- `docs/internals/generated-files.md` — all 8 VM dispatch tables, both codegen
  encoding directories, all 5 `build/generated` outputs, the tzdata generator,
  and the verifier spec table exist as listed; 84 opcodes in `Opcode.def` match
  `zanna --dump-opcodes`.
- `docs/languages/zia-reference.md` — all 53 documented keywords are in the
  lexer's table; the `export`/`public`→`expose`, `private`→`hide`, `let`→`final`
  aliases and the scalar type names all compile.
- Codemap file counts — `front-end-common` (23), `il-build` (2), `il-i-o`
  (22 + 8 internal headers), `il-runtime` (48) all match the tree exactly.
- ADR 0248's five reordered `World3D.SetFog` call sites all exist.
- All 211 complete Zia programs across the sampled book chapters and tutorials
  compile clean.

## Not Corrected — Deliberate

- `docs/book/part1-foundations/02-first-program.md` — `Zanna.Terminal.Display`
  is an intentional negative example, annotated `// No such function`.
- `docs/release_notes/*` — release notes are archival. The 0.2.0 GUI example
  (`Button.New("Click Me")`, `app.Run()`) no longer compiles because the GUI API
  changed after 0.2.0; that is what the release recorded and it should stay.
- `docs/adr/0118-rename-zannaide-to-zanna-studio.md` — `cmake/WriteZannaIDEBuildInfo.cmake`,
  `docs/internals/codemap/zannaide.md`, and `misc/site/showcase/zannaide.html`
  are the "before" halves of `X → Y` rename statements.
- `docs/adr/0042` — `insecure_skip_certificate_verification` is explicitly a
  hypothetical future field name.
- `docs/adr/0094`–`0100` — their Links sections cite
  `misc/plans/thirdpersonupgrade/*`, a plan directory deleted by the same commit
  `affd5f6f0`. These were left alone deliberately:
  `docs/internals/graphics3d-deep-review-2026-08.md:105-108` records that the
  Tranche 1 breadcrumb cleanup repointed 53 source and test references out of
  five deleted plan directories (`thirdpersonupgrade/`, `3d_overhaul/`, `fps/`,
  `game/`, `3d/`) and explicitly left "ADR reference sections … as historical
  record". VDOC-301 is a different case: `misc/plans/zannastudio/` is not one of
  those five directories, no decision covers it, and the file never existed at
  the cited path — the `zannastudio/` spelling is an artifact of the ADR 0110
  project rename sweep rewriting a `viperide/` path that was later deleted.
- Syntax-skeleton fragments in `basic-tutorial.md` (bare `IF N = 0 THEN`,
  `DO WHILE X < 10`) and API-reference fragments in `zannalib/` that reference
  an ambient `canvas`/`root` are conventional for their context.

## Residual Inconsistency — Reported Only

`status:` frontmatter uses five values across the tree: `active` (479),
`complete` (10), `completed` (9), `draft` (7), `proposed` (1). The style guide
shows only `active` in its template and does not enumerate the permitted set, so
`complete`/`completed` were left alone — but the two spellings mean the same
thing on nineteen audit records and should be reconciled to one, either by
picking a spelling or by declaring the enum in `docs/internals/doc-style.md`.
