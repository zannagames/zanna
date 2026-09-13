---
status: accepted
audience: contributors
last-verified: 2026-09-13
---

# ADR 0355: Every Package Format Ships the Project's Pack Groups

## Status

Accepted. Closes the gap recorded in the consequences of
[ADR 0354](0354-store-depot-packaging.md), which gave Steam depots their `.zpak` packs but left
the installer and archive targets without them.

## Context

`pack <group> <path>` and `pack-compressed <group> <path>` compile each non-empty group into
`<project>-<group>.zpak`. `zanna build` writes those files beside the executable, and the runtime
mounts every regular `*.zpak` file in the executable's directory at startup, plus
`Contents/Resources` inside a macOS application bundle. The runtime finds the directory by
resolving the running executable's real path (`/proc/self/exe` on Linux, `realpath` of
`_NSGetExecutablePath` on macOS), and it skips packs that are symbolic links.

`zanna package` compiled the packs but copied none of them into `macos`, `dmg`, `windows`,
`linux`, `rpm`, `linux-bundle`, or `tarball` output, so a packaged game started without its packed
assets. Projects worked around it by also listing each generated pack as an `asset`, which
Legacy Baseball still does and which keeps working.

The Linux FHS packages add a layout question: they install the executable as `/usr/bin/<exe>`,
and packs do not belong in `/usr/bin`.

A defect surfaced while tracing this: the self-extracting `linux-bundle` launched `AppRun`, a
symbolic link to the executable, but the extractor refuses a symbolic link as the entry point, so
every application bundle failed with `payload entry is missing or unsafe: AppRun`.

## Decision

### Generating the packs

A package run generates the pack groups once. The compile path reuses the packs the build wrote
beside the compiled binary in its private temporary directory (ADR 0354); a prebuilt
`--executable` gets them generated into another private temporary directory, removed afterwards.
Only non-empty groups produce a file. `steam-*` targets use the same step.

### Placement

| Target | Executable | Packs |
|---|---|---|
| `macos` (.zip), `dmg` | `<Name>.app/Contents/MacOS/<exe>` | `<Name>.app/Contents/Resources/<pack>` |
| `windows` | `<exe>.exe` in the install directory | beside it in the install directory |
| `tarball` | `<top>/<exe>` | `<top>/<pack>`; `install.sh` copies them with the tree |
| `linux` (.deb), `rpm` | `/usr/lib/<pkg>/<exe>`, with `/usr/bin/<exe>` a symbolic link to `../lib/<pkg>/<exe>` | `/usr/lib/<pkg>/<pack>` |
| `linux-bundle` | `usr/lib/<pkg>/<exe>`, with `usr/bin/<exe>` a symbolic link to `../lib/<pkg>/<exe>` | `usr/lib/<pkg>/<pack>` |
| `steam-*` | as ADR 0354 | as ADR 0354 |

The `linux`, `rpm`, and `linux-bundle` rows apply only when the project produces at least one
pack; without packs those targets keep the executable at `usr/bin/<exe>` exactly as before. The
symbolic link keeps `/usr/bin/<exe>` on `PATH` and in the `.desktop` entry, and because the
runtime resolves the executable's real path, it looks for packs in `/usr/lib/<pkg>`. FHS reserves
`/usr/lib/<package>` for a package's internal files, including internal binaries.

Pack files keep mode 0644 in every archive that records modes.

### Launching application bundles

`linux-bundle` writes `AppRun` as a launcher script that executes `usr/bin/<exe>` with its
arguments, instead of a symbolic link, so the extractor's entry-point check passes.

### Collisions

A pack whose destination equals another payload file fails the build:

- Windows: `Windows package install path collision: <path>` (case-insensitive, as for assets).
- `linux` and `rpm`: `duplicate linux package path: <path>`.
- `macos` and `dmg`: `generated macOS resource would replace a staged file: <name>`.
- `tarball` and `linux-bundle`: `duplicate tar entry path: <path>` (the archive writer's existing
  check).

### Verification and dry runs

Payload verification additionally requires each pack: under `Contents/Resources` in the macOS
ZIP, in the Windows installer's inner payload, at the tarball's top directory, and in the `.deb`
data archive together with `usr/lib/<pkg>/<exe>`.

`--dry-run` lists every declared group with the file name it produces
(`Pack: <group> -> <project>-<group>.zpak`), and `--dry-run --json` adds
`"packs": [{"group": "...", "file": "..."}]`. A dry run does not read the pack sources, so it
also lists groups that turn out to be empty.

## Consequences

- A project's `pack` groups reach players through every `zanna package` target without also being
  listed as assets. Projects that list prebuilt packs as assets keep working. Declaring the same
  file both ways ships it twice; where both copies land in one directory (every format except
  `linux`, `rpm`, and `linux-bundle`, whose assets live under `usr/share/<pkg>`) the collision fails the build.
- Linux packages of projects with packs install the real executable under `/usr/lib/<pkg>/`.
  Anything that looked for a regular file at `/usr/bin/<exe>` sees a symbolic link instead.
- The macOS ZIP and the Windows installer are ZIP32 containers limited to 4 GiB in total; packs
  count toward that limit, and a larger game fails with the existing ZIP64 diagnostic. The `dmg`,
  `tarball`, Linux, and store depot formats have no such limit.
- Application `linux-bundle` files launch again.

## Alternatives Considered

- **Install packs under `/usr/share/<pkg>` and teach the runtime to look there.** Rejected: the
  runtime has no package name at run time, and a second search root would change asset resolution
  for every native program.
- **Symbolic links from `/usr/bin` to packs in `/usr/share/<pkg>`.** Rejected: discovery
  deliberately ignores symbolic-link packs, and packs in `/usr/bin` do not belong there even as
  links.
- **Move the Linux executable to `/usr/lib/<pkg>` for every project.** Rejected for now: it would
  change the layout of packages that have no packs, for no benefit to them.
- **Copy packs only when a project opts in.** Rejected: a packaged game without its packs is never
  what the author wants, and `zanna build` already places them.
