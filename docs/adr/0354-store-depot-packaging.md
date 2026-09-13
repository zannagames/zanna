---
status: accepted
audience: contributors
last-verified: 2026-09-13
---

# ADR 0354: Store Depot Packaging for Platform Services (Steam First)

## Status

Accepted. Implements phase 3 of the platform services program started by
[ADR 0352](0352-platform-services-runtime-loaded-providers.md) and extended by
[ADR 0353](0353-platform-services-player-features.md).

## Context

A game that uses `Zanna.Services` on Steam ships as SteamPipe depots: plain directory trees that
`steamcmd` uploads, one per operating system. Assembling one by hand is error-prone, and each
mistake breaks something only visible after an upload:

- The Steamworks redistributable must sit where the runtime loads it (ADR 0352): beside the
  executable on Windows and Linux, and in `Contents/MacOS` inside a macOS application bundle. It
  must match the payload's operating system and every CPU architecture of the executable, and it
  must come from a supported SDK (1.61 through 1.65).
- The macOS bundle must be signed with the entitlements Steam requires,
  `com.apple.security.cs.disable-library-validation` (load the SDK library and the overlay) and
  `com.apple.security.cs.allow-dyld-environment-variables` (inject the overlay), must not enable
  App Sandbox (Steam does not support it), and needs a notarized Developer ID signature for new
  apps. Nested code (the SDK library) must be signed before its bundle.
- `steam_appid.txt` is a development-only file that Valve says must never ship.
- `.zpak` pack groups (`pack` directives) must travel with the executable, because the runtime
  discovers them beside it (and in `Contents/Resources` on macOS).
- The SteamPipe app build script maps each depot id to its content directory.

`zanna package` builds single-file installers and archives, and none of its targets produces a
directory, bundles a platform redistributable, or writes upload scripts. Other stores (GOG,
Epic, itch.io) also upload loose directories through their own tools, so the mechanism must not
be Steam-shaped internally even though Steam is the only store today.

## Decision

### Targets

`zanna package` gains three targets:

| Target | Payload | Architectures | Depot platform key |
|---|---|---|---|
| `steam-windows` | PE32+ executable | `x64` | `windows` |
| `steam-macos` | Mach-O executable in an `.app` bundle | `x64`, `arm64` (a universal prebuilt executable is accepted) | `macos` |
| `steam-linux` | ELF executable | `x64`, `arm64` | `linux` (x64), `linux-arm64` |

`steam-windows --arch arm64` fails with
`Steam Windows depots are x64-only: Valve ships no Windows arm64 Steamworks redistributable`.
As for every target, the built-in compile path only builds for the host operating system, and
other hosts require `--executable`.

The output (`-o`, default `<project>-<version>-steam`) is a SteamPipe build root directory:

```text
<root>/
  content/<platform>/        depot content for one platform key
  scripts/app_build_<app-id>.vdf
  manifests/<platform>.json  file list with sizes and SHA-256 (never inside content/)
```

A run replaces only `content/<platform>/` and `manifests/<platform>.json`, so packaging each
platform into the same root accumulates one multi-depot build. The staged tree is assembled in a
private sibling directory (`content/.<platform>-staging-*`) and renamed into place only after it
verifies; if that rename fails the previous content is restored, so a failed run leaves the
previous content untouched. The build root is never deleted on failure, even without
`--keep-failed-artifact`, because it holds other platforms' depots. The output path must not be
an existing file (`Steam depot output '<root>' exists and is not a directory`) and must not lie
inside a directory the build reads, meaning an `asset`, `pack`, or `embed` source, because the
next build would copy or pack its own output:
`Steam depot output directory '<root>' is inside asset source '<source>'`.

### Manifest directives and options

| Directive | Arity | Rule |
|---|---|---|
| `steam-app-id <id>` | once | Decimal integer in `1..4294967295`, stored without leading zeros; required by `steam-*` targets |
| `steam-redist <dir>` | once | Steamworks SDK `redistributable_bin` directory, or a directory containing `redistributable_bin/` or `sdk/redistributable_bin/`; project-relative or absolute, may use `..` (it is a developer tool path like `windows-sign-pfx`) |
| `steam-depot <platform> <depot-id>` | per platform | Platform `windows`, `macos`, `linux`, or `linux-arm64`; depot id `1..4294967295`; each platform and each depot id at most once |
| `steam-build-description <text>` | once | Single line without `"` or `\`; default `<project> <version>` |
| `steam-set-live <branch>` | once | Characters `A-Z a-z 0-9 _ . -`; `default` (any case) is rejected because Steam sets the default branch live only through the App Admin panel |

`--steam-redist <path>` overrides `steam-redist` for one command; relative values resolve from the
current working directory. It is rejected for other targets:
`--steam-redist applies only to --target steam-windows, steam-macos, or steam-linux`.

Manifest diagnostics:

- `Steam depot packaging requires steam-app-id in zanna.project` (a `steam-*` target without one)
- `invalid steam-app-id '<v>'; expected an integer in 1..4294967295`
- `steam-depot requires <platform> <depot-id>; got '<value>'`
- `invalid steam-depot platform '<p>'; expected windows, macos, linux, or linux-arm64`
- `invalid steam-depot id '<v>'; expected an integer in 1..4294967295`
- `duplicate steam-depot platform '<p>'`
- `steam-depot id <id> is already mapped to <platform>`
- `steam-build-description must not contain '"' or '\'`
- `invalid steam-set-live branch '<b>'; use letters, digits, '_', '.', or '-'`
- `steam-set-live cannot be 'default': Steam sets the default branch live only through the App Admin panel`

### Store profiles

The packager keeps a table of store profiles (`StoreDepotBuilder.hpp`). A profile names the store
and supplies, per platform and architecture, the redistributable's SDK-relative source and staged
file name, the platforms it rejects, the export names a supported redistributable must contain, a
marker string present in executables that link the provider, required and forbidden macOS
entitlements, files forbidden in content, the default build-root suffix, and hooks for store
configuration: validation, app and depot ids, build description and live branch, the build-script
path and writer, and the upload command shown after a build. Adding a store adds a profile, its
directives, and its targets; staging, verification, and manifests are shared.

The Steam profile:

| Platform key | SDK file (under `redistributable_bin/`) | Staged at |
|---|---|---|
| `windows` | `win64/steam_api64.dll` | `steam_api64.dll` beside `<exe>.exe` |
| `macos` | `osx/libsteam_api.dylib` | `<Name>.app/Contents/MacOS/libsteam_api.dylib` |
| `linux` | `linux64/libsteam_api.so` | `libsteam_api.so` beside `<exe>` |
| `linux-arm64` | `linuxarm64/libsteam_api.so` | `libsteam_api.so` beside `<exe>` |

Required export names are the ten core exports of ADR 0352. The provider marker is
`SteamAPI_InitFlat`, the name the Zanna Steam provider resolves at run time, which appears in an
executable exactly when native linking kept the provider.

### Redistributable and executable checks

These run for dry runs and builds:

1. Resolve the redistributable file; a missing one reports
   `Steam redistributable not found: expected <file> (from <steam-redist|--steam-redist> '<value>')`.
2. Inspect it: a Windows DLL must be a PE32+ image with the DLL flag for x64; a macOS library must
   be a Mach-O dynamic library whose slices include every architecture of the executable; a Linux
   library must be an ELF shared object for the target machine. Failures:
   `Steam redistributable '<file>' is not <a PE32+ DLL|a Mach-O dynamic library|an ELF shared object>`
   and `Steam redistributable '<file>' does not contain <arch> code`. The executable itself must
   contain the selected architecture: `executable '<path>' does not contain <arch> code`.
3. It must contain every required export name:
   `Steam redistributable '<file>' is not a Steamworks SDK 1.61-1.65 redistributable: export name '<name>' not found`.
4. An executable that contains the provider marker but has no configured redistributable fails:
   `the executable uses the Zanna.Services Steam provider, but no Steamworks redistributable is configured; set steam-redist or pass --steam-redist`.
   An executable without the marker may ship without a redistributable (Steam features that need
   no code, such as the overlay and Auto-Cloud, still work); when one is configured anyway it is
   staged and the command warns
   `warning: <exe> does not use Zanna.Services; the staged Steamworks redistributable is not loaded`.

A dry run cannot inspect an executable it has not built. With a prebuilt `--executable` every
check runs; otherwise check 2 compares the redistributable with the selected `--arch` (a
universal macOS executable needs every slice, which only the build can see) and check 4 waits for
the build.

### Content layout

Common to every platform: the executable; the redistributable; `asset` directives (beside the
executable on Windows and Linux, under `Contents/Resources/<target>` on macOS); and every
non-empty `pack` group generated from the project as `<project>-<pack>.zpak` into the directory
the runtime scans (beside the executable, or `Contents/Resources`).

- **Windows:** `<exe>.exe` (Authenticode-signed when `windows-sign` is requested, using the
  existing signing options), `windows-dll` files and adjacent non-system DLL imports discovered as
  the Windows installer does. The redistributable is not an import and keeps Valve's signature.
- **macOS:** the same bundle the `macos` target stages (Info.plist, PkgInfo, icon, resources),
  plus the redistributable in `Contents/MacOS`. When the sign mode is `adhoc` or `developer-id`,
  the redistributable is signed first with the same identity (`--timestamp` for Developer ID), then
  the bundle is signed with the merged entitlements and verified with
  `codesign --verify --deep --strict`; notarization and stapling follow the existing options.
  `none` and `preserve` leave the bundle unsigned and apply no entitlements.
- **Linux:** `<exe>` with mode 0755 and the redistributable with mode 0755.

Windows and Linux content paths must be unique ignoring case, because players install depots onto
case-insensitive file systems: `Steam depot content path collision: <path>`. On Windows every
path segment must also be a valid Windows file name. Inside a macOS bundle, generated resources
and the redistributable must not replace a file the bundle already stages
(`generated macOS resource would replace a staged file: <name>`,
`macOS bundle extra file would replace a staged file: <path>`).

Verification fails the build if `steam_appid.txt` appears anywhere in the content (compared
case-insensitively; asset inputs are checked before any signing or notarization starts):
`steam_appid.txt must not ship in a Steam depot (found <path>); Zanna.Services.Platform.Init sets SteamAppId itself`.
Staged content may only contain directories and regular files
(`Steam depot content must not contain symbolic links: <path>`).

### macOS entitlements merge

The signing entitlements are the project's `macos-entitlements` plist (or an empty dictionary)
with every profile-required key set to `<true/>`. The merge reads XML property lists only and
preserves every other key:

- `macOS entitlements '<file>' is a binary property list; convert it with 'plutil -convert xml1'`
- `macOS entitlements '<file>' is not a valid XML property list: <detail>`
- `macOS entitlements '<file>' set <key> to <value>, but Steam requires true`
- `macOS entitlements '<file>' enable com.apple.security.app-sandbox, which Steam does not support`

### SteamPipe build script

After staging, `scripts/app_build_<app-id>.vdf` is rewritten to list every platform key that has
both a `content/<platform>/` directory and a `steam-depot` mapping, in the order `windows`,
`macos`, `linux`, `linux-arm64`:

```text
"AppBuild"
{
	"AppID" "<app-id>"
	"Desc" "<description>"
	"ContentRoot" "../content/"
	"BuildOutput" "../output/"
	"SetLive" "<branch>"
	"Depots"
	{
		"<depot-id>"
		{
			"FileMapping"
			{
				"LocalPath" "<platform>/*"
				"DepotPath" "."
				"recursive" "1"
			}
		}
	}
}
```

The keys and layout follow Valve's sample app build script. `SetLive` appears only when
`steam-set-live` is configured. When the packaged platform has no mapping the command warns
`warning: no steam-depot is configured for <platform>; scripts/app_build_<app-id>.vdf does not upload content/<platform>`,
and when no staged platform has a mapping no script is written. Upload with
`steamcmd +login <account> +run_app_build <root>/scripts/app_build_<app-id>.vdf +quit`.

### Dry run and manifests

`--dry-run` prints the app id, platform key, depot id, content directory, launch path (the path
Steamworks launch options should use), redistributable source and staged path, entitlements, and
the build script path. `--dry-run --json` adds a `"steam"` object with the same fields to the
existing plan for `steam-*` targets only.

`manifests/<platform>.json` records `schema_version` 1, the store, platform key, architecture
(`universal` for a two-slice macOS executable) and the executable's architecture list, version,
app and depot ids (`null` when unmapped), launch path, executable and redistributable content
paths, trust (the macOS signing trust label, `authenticode` or `unsigned` on Windows, `unsigned`
on Linux), and every content file sorted by path with its size and SHA-256.

The launch path is what Steamworks launch options name: `<exe>.exe`, `<Name>.app`, or `<exe>`.

### Temporary build output

The compile path of every `zanna package` target now builds into a private temporary directory
that is removed afterwards (or kept, and named, with `--keep-failed-artifact` after a failure).
Previously the temporary executable was written straight into the system temporary directory and
any `.zpak` files the build produced beside it were never removed. Store depots take their packs
from that directory; with a prebuilt `--executable` the packs are generated into a second private
directory.

### Fixes found while building this

- **Host check.** The compile path refused `dmg` on macOS and `rpm` and `linux-bundle` on Linux
  unless `--executable` was given, because it compared target names with the host target instead
  of operating systems. It now compares the operating system a target's executable runs on, which
  also admits `steam-*` targets on their own hosts.
- **Pack discovery on macOS and Linux.** The runtime compared the last four characters of each
  file name with the five-character suffix `.zpak`, so automatic discovery of packs beside the
  executable (and in `Contents/Resources`) never matched anything outside Windows. It now matches
  the same names as the Windows `*.zpak` pattern. Packs mounted explicitly were unaffected.
- **Notarization ZIP names.** Notarization zipped the bundle with paths relative to the staging
  directory, so the DMG path (which signs the copy on the mounted volume) produced entries
  starting with `..`. Entries are now named relative to the bundle's parent.
- `--linux-sign-key` with a non-Linux target is now rejected before compiling rather than after.
- **Asset registry types.** `Zanna.IO.Assets.List`, `LoadBytes`, and `Load` were registered as
  untyped objects, so Zia inferred the `Assets` class for their results: `.Count` and `.Length`
  did not type-check, and a member used on a `Load` result was resolved against the wrong class.
  They now declare `seq<str>()`, `obj<Zanna.Collections.Bytes>(str)`, and
  `obj<Zanna.Core.Object>(str)` (`Load` decodes to `Pixels`, `Sound`, or `Bytes` by extension,
  so callers narrow with `as`); the C functions are unchanged.

## Consequences

- A Steam release is `zanna package --target steam-<os>` per operating system into one root and
  one `steamcmd` command, with the layout, entitlement, redistributable, and development-file
  mistakes above caught before upload.
- Steam-specific knowledge lives in one profile and the `steam-*` directives; other stores reuse
  staging, verification, and manifests.
- Now that discovery works everywhere, every `.zpak` beside a native executable mounts at startup
  on all three operating systems, as on Windows before. Explicitly mounting one of those packs
  again succeeds without mounting a duplicate.
- Repository hygiene is unchanged: the redistributable is read from the developer's SDK and copied
  into the output, never into Zanna.
- The existing single-file targets (`macos`, `dmg`, `windows`, `linux`, `rpm`, `linux-bundle`,
  `tarball`) still omitted `.zpak` pack groups when this record was written; that gap predated
  it and is closed by [ADR 0355](0355-package-formats-ship-pack-groups.md).
- Uploading, launch options, and depot and package configuration in Steamworks remain manual.

## Alternatives Considered

- **A generic `store` target with `--store steam`.** Rejected for the command line: every store has
  its own directives and upload tool, and `steam-macos` is what developers expect. The profile
  table keeps the implementation store-neutral.
- **Emit `steam_appid.txt` for development staging.** Rejected: `Platform.Init` already exports
  `SteamAppId`, and a generated file risks shipping; the verifier rejects it instead.
- **Keep Valve's signature on the macOS redistributable.** Rejected: a re-signed library carries
  the game's Developer ID and timestamp, which notarization expects of all nested code; the
  entitlements are still required for the overlay.
- **Only accept project-relative redistributable paths.** Rejected: the SDK usually lives outside
  the game repository, as signing material does.
- **Parse full property lists with a general plist library.** Rejected under the zero-dependency
  rule; entitlements are flat dictionaries of booleans and strings, and binary plists are
  refused with a conversion hint.
