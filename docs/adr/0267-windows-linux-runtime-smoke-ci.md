---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0267: Gate Windows and Linux Runtime Smoke Regressions

## Status

Accepted (2026-08-17)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0135 — Runtime Cppcheck Build and CI Gate
- `scripts/build_zanna_linux.sh` — canonical Linux build entry point
- `scripts/build_zanna_win.ps1` — canonical Windows build entry point
- `docs/internals/testing.md` — test-label and slow-test policy

## Context

Zanna's release workflows exercise native Windows and Linux hosts only when a
maintainer dispatches an installer build. Pull requests otherwise had no native
Windows/Linux gate for the C runtime contracts on which Zanna Studio depends.
The generic `runtime` label is intentionally broad and contains hundreds of
tests, while the generic `smoke` label includes display and product examples;
neither defines a bounded, headless pull-request lane.

Three existing x86-64 Studio native-link probes also matched `x86_64` and
lowercase `amd64`, but not CMake's common Windows processor spelling `AMD64`.
Consequently a supported Windows configuration could advertise x86-64 native
linking without registering those tests.

Adding a workflow changes repository validation policy and therefore requires
an ADR under ADR 0006 and `AGENTS.md`.

## Decision

- Add `.github/workflows/runtime-smoke.yml` for pull requests, pushes to
  `main`, and manual dispatch. It runs independent native jobs on
  `ubuntu-24.04` and `windows-2025`.
- Both jobs build through the canonical platform script. They select only the
  `runtime-smoke-ci` CTest label, explicitly enable its native-link tests that
  also carry `slow`, and skip the unrelated install, audit, lint, generic smoke,
  and final Zanna Studio executable stages.
- Define `runtime-smoke-ci` from one reviewed manifest in
  `src/tests/CMakeLists.txt`. Configuration fails if a manifest entry is
  renamed or ceases to be registered instead of silently shrinking coverage.
- The manifest covers representative string, path, whole-file I/O, watcher,
  regex/compiled-pattern, process, GUI IDE, and language-service C runtime
  contracts; headless Studio freshness, persistence, settings, watcher,
  large-text, semantic-worker, and shutdown probes; VM runtime probes; and all
  three supported x86-64 Studio native-link probes.
- Register x86-64 native probes for `AMD64` as well as `x86_64` and `amd64`.
- Keep this lane headless and bounded. Display tests and the full CTest suite
  remain in their existing workflows and local validation paths.
- Upload CTest's failure log for seven days when either job fails. The workflow
  uses repository code and runner-provided or operating-system build tools and
  adds no product dependency.

## Consequences

- Every pull request proves the selected Studio-supporting C runtime behavior
  on both Windows and Linux rather than inferring portability from macOS or a
  manually dispatched release build.
- Windows now exercises the native completion, checked-I/O, and BASIC query
  paths that its processor spelling previously omitted.
- The exact lane is visible in one manifest, and accidental label growth cannot
  turn it into an unbounded suite.
- The jobs still compile the normal project target graph, but skip the
  multi-minute final Studio native executable and run only the selected tests.
- Changes that need broader display, packaging, sanitizer, or complete-suite
  coverage continue to use those specialized gates.

## Alternatives Considered

- **Run the entire `runtime` label on every pull request.** Rejected because it
  contains hundreds of tests and would duplicate the full-suite lanes.
- **Reuse the generic `smoke` label.** Rejected because its membership is not a
  stable runtime contract and includes display-oriented examples.
- **Run only Linux and cross-compile Windows.** Rejected because native process,
  watcher, path, exception, and linker behavior must execute on Windows.
- **Rely on manual installer workflows.** Rejected because they are not a
  continuous pull-request regression gate.
