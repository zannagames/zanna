---
status: active
audience: contributors
last-verified: 2026-08-28
---

# ADR 0110: Rename the Project from Viper to Zanna

Date: 2026-07-17

## Status

Accepted.

## Context

The platform was developed under the working name **Viper**. A pre-release
clearance review found the name heavily claimed across the project's own
categories: ETH Zurich's Viper is an actively maintained intermediate
verification language and toolchain with a near-identical elevator pitch;
Vyper (Ethereum's contract language, itself renamed from "Viper" in 2017) is
pronounced identically; ViperIDE is an actively maintained MicroPython IDE;
at least five game engines named Viper exist on GitHub, including one from a
commercial studio; and the `viper` package name is claimed on the major
registries. Because the product name is also user-facing language surface
(`bind Viper.*` in every program), a rename after public release would break
every user program ever written. Renaming before the first public release
was the only cheap moment.

**Zanna** (Italian: *fang*; Romanian folklore: a guardian spirit) cleared a
full search: no software or gaming products, no conflicting trademarks
found in the software/gaming classes, package names free on npm and
crates.io, and the zannagames.com domain and GitHub organization were
secured. The fang keeps the identity of the original name; Zanna and Zia
form a coherent brand family.

## Decision

Rename every project-owned identifier in one migration, with no
compatibility aliases (pre-release, no external users):

- Project, repository, and CLI binary: `viper` → `zanna`.
- Standalone BASIC tooling and examples: `vbasic` → `zbasic`,
  `vbasic-server` → `zbasic-server`, and `examples/vbasic/` →
  `examples/zbasic/`. The `zbasic` executable mirrors the standalone `zia`
  compiler interface; interactive BASIC remains available through
  `zanna repl basic`.
- Runtime namespace root: `Viper.*` → `Zanna.*` across the runtime class
  registry, all bindings, examples, goldens, and documentation.
- Environment variables, CMake options, and preprocessor identifiers:
  `VIPER_*` → `ZANNA_*`.
- Packaging: VAPS → ZAPS, project files `.vap` → `.zap`, MIME
  `text/x-vaps` → `text/x-zaps`, installer identities `org.viper.*` →
  `org.zanna.*`.
- Asset archive: VPA "Viper Pack Archive" → **ZPAK** "Zanna Pack Archive",
  extension `.vpa` → `.zpak`, magic bytes `'V','P','A','1'` →
  `'Z','P','A','K'` (the separate header version field is retained).
  Old `.vpa` archives are not readable by renamed builds; none were
  distributed.
- Terrain height sidecar magic: `viper-heightmap-v1` → `zanna-heightmap-v1`.
- IDE: ViperIDE → ZannaIDE. C++ namespace `viper` → `zanna`.
- GitHub URLs: `github.com/splanck/viper` → `github.com/zannagames/zanna`.
- The C runtime ABI prefix `rt_*`, GUI prefix `vg_*`, the Zia language, and
  the `.zia` extension are unchanged; they never encoded the product name.

ViperDOS platform support was removed in the immediately preceding commit
(the OS moved to its own repository in 2026-03); the rename therefore
carries no ViperDOS surface.

## Consequences

- The ABI manifest hashes changed (names participate in the hash); the
  reviewed pins were updated after confirming function/class/property/method
  counts were unchanged.
- Binary data embedded in text files (glTF base64 buffers) must be excluded
  from any future text sweeps; case-exact matches inside base64 are
  statistically expected and rewriting them corrupts assets.
- The prebuilt `src/zannaide/bin/zannaide` artifact still embeds old
  internal strings until its next rebuild.
- Historical documents were renamed in full; the only permitted references
  to the old name are this ADR, the migration plan
  (`misc/plans/rename-viper-to-zanna.md`), and the release-notes line.
