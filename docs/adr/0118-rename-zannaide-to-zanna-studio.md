---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0118: Rename ZannaIDE to Zanna Studio

Date: 2026-07-17

## Status

Accepted.

## Context

The IDE reached its current name mechanically: ADR 0110 case-mapped ViperIDE
to ZannaIDE during the project-wide rename and explicitly deferred product
branding ("'Zanna Studio' display-branding can come later"). The IDE is the
platform's flagship product — the first thing a new user launches — and is
about to receive a full brand reface and capability program
(`misc/plans/zannastudio/`). "ZannaIDE" is a mechanical compound, not a
product name; "Zanna Studio" reads as a flagship creative tool and matches
the IDE's direction as the platform's integrated development product.

The rename is still cheap: pre-release, no external users, and the only
persisted user state is per-user IDE configuration, which can be migrated.

## Decision

Rename the product to **Zanna Studio** with binary **`zannastudio`**
(`zannastudio.exe` on Windows), in one migration with no compatibility
aliases, plus a one-time configuration migration:

- Source tree `src/zannaide/` → `src/zannastudio/`; CMake target
  `zannaide_native` → `zannastudio_native`; option `ZANNA_INSTALL_ZANNAIDE`
  → `ZANNA_INSTALL_ZANNASTUDIO`; helper `cmake/WriteZannaIDEBuildInfo.cmake`
  → `WriteZannaStudioBuildInfo.cmake`.
- Windows installer component id `zannaide` → `zannastudio`; display name
  "Zanna Studio"; Start Menu shortcut `Zanna Studio.lnk`; icon
  `bin/zannastudio.ico`; file associations launch `bin\zannastudio.exe`.
- Linux packaging `zannastudio.desktop` (`Name=Zanna Studio`), executable
  `/usr/bin/zannastudio`. macOS continues to stage the binary inside
  `Zanna Toolchain.app` (no separate IDE bundle), with the stable command named
  `zannastudio`. ADR 0149 refines the macOS payload layout so that command
  launches a sibling executable named `Zanna Studio` for correct Cocoa identity.
- Project/bundle id `org.zanna-lang.zannaide` → `org.zanna-lang.zannastudio`.
- Per-user configuration moves to `%APPDATA%\ZannaStudio`,
  `~/Library/Application Support/ZannaStudio`, and
  `$XDG_CONFIG_HOME/zannastudio` (or `~/.config/zannastudio`). On first
  launch, if the new directory has no `settings.ini`, state files are
  **copied** (not moved) from the old `ZannaIDE` platform directory, or the
  older `~/.zannaide`, whichever is found first; if copying fails the old
  directory is adopted in place. Project-local names change to
  `.zannastudio-trash` and `.<name>.zannastudio-tmp-<ms>`; legacy
  `.zannaide-*` entries stay hidden from the explorer (dot-prefix exclusion)
  and a legacy trash directory remains manually recoverable in place.
- Test names `zia_*zannaide*` → `zia_*zannastudio*` and ctest label
  `zannaide` → `zannastudio`; the Windows toolchain-installer E2E contract
  and PowerShell validator pin the new names.
- Docs and site: `docs/internals/codemap/zannaide.md` →
  `codemap/zannastudio.md` and `misc/site/showcase/zannaide.html` →
  `showcase/zannastudio.html`, with inbound links updated.

Deliberately unchanged: the generic `ZANNA_IDE_*` build-environment family
(`ZANNA_IDE_ARCH`, `ZANNA_IDE_OUTPUT`, …) — tooling knobs, not product
naming; the generic script filenames `scripts/build_ide.sh` /
`build_ide_win.ps1`; and historical records — release notes, blog posts, and
ADR 0110 keep "ZannaIDE" as shipped history.

CI note: the three release-installer workflows referenced the renamed CMake
option; updating those three lines was explicitly approved as a scoped
exception to the no-CI-edits rule, in the same change as the option rename.

## Consequences

- Every current reference updates in one migration; a residue grep
  (`zannaide`, excluding the historical keep-list) must return clean and
  stays cheap to re-run.
- Existing users' settings, sessions, breakpoints, and recovery files carry
  forward automatically; the old configuration directory is left intact so a
  rollback build still finds it.
- The Windows toolchain installer's component id changes; because there are
  no released installers, no upgrade path from a `zannaide` component is
  required.
- Later phases of the Zanna Studio program (brand reface, iconography,
  accessibility, editor depth) build on the new identity without further
  renames.
