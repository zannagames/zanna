---
status: draft
audience: contributors
last-verified: 2026-08-17
---

# ADR 0241: Externalize Large Demos to the zannademos Repository

## Status

Proposed (decision recorded)

## Context

`examples/` had grown to ~98 MB (44 MB tracked, dominated by five game
projects) and was entangled with the toolchain repo: 127 of ~2080 ctest
tests consumed `examples/` content, two manifest audits enforced
set-equality over the whole tree, the installer shipped it wholesale, and
demo churn rode inside engine commits. The repo needs a small curated
example set; the large demos need a home that does not weigh down every
clone, build, and install of the compiler.

## Decision

**Split the tree.** The zanna repo keeps a curated example set; everything
else moves to the `zannademos` repository (github.com/zannagames/zannademos),
conventionally cloned nested at `<zanna>/zannademos/` (gitignored, same
pattern as `/zannaweb/`).

**Kept in `examples/`:** `il/`, `zbasic/`, `zia/`, `hello-lsp.zia`,
`localization/`, `embedding/`, `apiaudit/`, the whole `3d/` ladder
(`openworld_slice` is a hard fixture for `test_rt_model3d`), `games/lib`,
`games/chess`, `games/crackman`, `games/{vtris,frogger-basic,centipede-basic}`,
and `apps/paint`. Each keeper is small, self-contained, and carries test,
packaging, or tutorial weight.

**Moved:** the ashfall/xenoscape families (plus `-scenes` variants),
`3dbowling`, `ridgebound`, `baseball`, the small one-off games,
`apps/{zannasql,webserver,telnet,varc,asset_demo}`, and `sqldb-basic/`.
Fresh import, no history extraction — the zanna repo's history remains the
archaeology record; import commits cite the source SHA. The ~10 MB of
byte-identical assets duplicated between `ashfall`/`ashfall-scenes` and
`xenoscape`/`xenoscape-scenes` moved verbatim (a move is not a refactor);
deduplication inside zannademos is recorded as an optional follow-up.

**Testing contract:**

- zannademos owns its tests: `demo_tests.tsv` rows (check / run-probe /
  run-script; lanes `fast`, `full`, `perf`, `requires_display`,
  `native-arm64-macos`) executed by `scripts/run_demo_tests.sh` / `.ps1`,
  which drive an existing `zanna` binary and never build or invoke
  CMake/CTest. CTest `ENVIRONMENT`/`TIMEOUT` properties migrated into
  manifest columns. Battery-sensitive perf probes live in the opt-in
  `perf` lane.
- The zanna repo bridges via `scripts/zannademos_bridge.sh`, registered
  unconditionally as `zannademos_smoke` (fast lane, label `demos`) and
  `zannademos_full` (gated on `ZANNA_RUN_DEMOS_FULL=1`, labels
  `demos;slow`). **Absence of the clone is a visible Skipped result**
  (`SKIP_REGULAR_EXPRESSION`), never a silently missing test — the same
  idiom as the Windows installer smokes. Coverage gaps must be loud.
- Studio scene-preview probes (`scene_gameplay_preview{,_2d}`) and the
  zannaweb screenshot probe resolve their fixture scenes from
  `zannademos/games/{ashfall,xenoscape}-scenes` and print `RESULT: skip`
  when the clone is absent; their ctest registrations skip on that
  marker. Authoring trimmed in-repo fixture scenes is the recorded
  follow-up if unconditional coverage is ever required.

**Build contract:** the platform demo builders
(`build_demos_{mac,linux}.sh`, `build_demos_win.ps1`) gained
`ZANNA_DEMO_MANIFEST` / `ZANNA_DEMO_ROOT` / `ZANNA_DEMO_BIN_DIR`
overrides with unchanged defaults; zannademos wraps them
(`zannademos/scripts/build_demos.sh`) pointing at its own `demos.list`
and `bin/`. No builder logic is duplicated. The zanna repo's
`demo_projects.list` shrank to the seven keepers (still satisfying the
games>apps ratio gate); the Windows installer smoke subject moved from
xenoscape to crackman, which is also the macOS signing-smoke subject.

The Windows adapter resolves relative overrides from the zanna repository,
rejects reparse or overlapping source/output/build/manifest roots, and permits
`--clean` only for the exact external `<demo-root>\bin` ownership boundary.
Manifest and project metadata remain bounded BOM-free UTF-8 with the shared
lowercase inventory grammar. These checks preserve externalization without
expanding destructive authority to arbitrary configured output directories.

## Consequences

- Clean zanna clones build and test green with no demos present; the
  demo lanes appear as Skipped, not missing.
- Engine changes that break demos surface on machines with the nested
  clone (every full ctest runs `zannademos_smoke`) instead of inside the
  main suite; cross-repo breakage now takes two commits to fix.
- `zanna` installs ship only the curated samples.
- zannaweb screenshot capture requires the zannademos clone.
