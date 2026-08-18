---
status: active
audience: public
last-verified: 2026-08-17
---

# Installer and Package Release Guide

Zanna produces native toolchain installers for Windows, macOS, and Linux from
one validated `cmake --install` stage. This guide defines the supported
developer and release paths, the trust gates, generated metadata, lifecycle
guarantees, and native-host validation commands. ADR 0073 records the release
pipeline decision.

## Build from one staged tree

Use the platform build script for the full build. Do not invoke raw CMake as a
replacement for the canonical build:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_zanna_win.ps1
./scripts/build_zanna_mac.sh
./scripts/build_zanna_linux.sh
```

The installer wrappers run the same build path and then call
`zanna install-package`:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_installer.ps1 --target windows --output-dir artifacts
./scripts/build_installer.sh --target macos --macos-dmg --output-dir artifacts
./scripts/build_installer.sh --target all --output-dir artifacts
```

On Windows, the wrapper accepts only `Release` or `RelWithDebInfo`, enables
Zanna Studio when no explicit CMake setting is present, and requires the
Studio executable plus its build-identity file before packaging. The pair must
use buildinfo schema 1 and bind the exact repository version, PE32+ architecture,
byte size, and lowercase SHA-256 of `zannastudio.exe`. Passing
`--stage-dir`, `--build-dir`, or `--verify-only` (including `--name=value`
forms) selects caller-owned input and suppresses the automatic canonical
build. `--help` is side-effect free.

Before invoking `install-package`, the Windows wrapper copies the selected
`zanna.exe` into a unique private directory below the build root, verifies its
size and SHA-256, and removes only that exact file and empty directory after
the command exits. This keeps an explicit `--build-dir` package operation from
locking the executable that its internal `cmake --build` may need to relink.

The Windows package version remains numeric for installer identity and upgrade
ordering, while Studio buildinfo retains the full configured product version
(including prerelease text such as `-snapshot`). Lifecycle validation obtains
that full version from the canonical first line of the installed, package-owned
`zanna --version` output; the bounded build-detail lines that follow do not
change the product version. The validator does not compare Studio provenance
with the numeric package version.

For repeatable package work, stage once and package that immutable tree:

```bash
cmake --install build --prefix "$PWD/build/release-stage"
build/src/tools/zanna/zanna install-package \
  --stage-dir build/release-stage --target all --output-dir artifacts
```

`install-package` inspects the staged `zanna` executable and rejects a target or
architecture that conflicts with its PE, Mach-O, or ELF header. A macOS
universal stage must contain valid, in-bounds Mach-O slices; a single-slice
binary is never relabeled as universal.

## Output contract

Use `--output-file` when exactly one artifact is expected and `--output-dir`
when a target emits more than one. The legacy `-o` option is a file for one
target unless it names an existing directory; it is a directory for multiple
targets. This removes the former ambiguity where an extensionless file name was
silently treated as a directory.

Every successful invocation writes:

- one `<artifact>.sha256` sidecar per artifact;
- `SHA256SUMS` for a directory or multi-artifact output; and
- a schema-versioned JSON inventory, normally `zanna-artifacts.json`, containing
  file name, format, platform, architecture, version, byte size, SHA-256,
  verification state, trust state, and `SOURCE_DATE_EPOCH`.

Override the inventory location with `--artifact-manifest`. Metadata and
sidecars are written atomically after all selected artifacts pass. Release
generation takes a directory lock, refuses to overwrite an existing artifact
set, and removes partial outputs on failure.

Verify an artifact and its sidecar together:

```bash
zanna install-package --verify-only artifacts/zanna-toolchain.run --require-checksum
```

The Linux self-extracting toolchain is a `.run` bundle selected by
`--target linux-bundle`. Toolchain packaging does not accept an `appimage`
target or infer `.AppImage` during verification: the bundle is not an AppImage
filesystem image and has one unambiguous public name.

## Release mode

Set `SOURCE_DATE_EPOCH` to a non-negative integer, normally the release commit
timestamp, then add `--release`:

```bash
export SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)"
```

Release mode rejects `--no-verify`, `--windows-sign-no-verify`, debug Windows
toolchains, missing trust credentials, dirty or unknown source state, missing
immutable source commit metadata, and an invalid or absent
`SOURCE_DATE_EPOCH`. Source archives must configure both
`ZANNA_SOURCE_COMMIT=<lowercase-hex>` and `ZANNA_SOURCE_STATE=clean`. Its trust
requirements are platform-specific:

### Windows

Windows release output requires Authenticode signing. Stable identity is
reserved for `--release`; unsigned/local output defaults to the separate
`development` channel, identifier, display name, and install directory. Prefer
an Authenticode certificate already imported into the certificate store.

Provision the update-signing public key before packaging. This key is separate
from Authenticode and may be exported without creating a placeholder manifest:

```powershell
$env:ZANNA_WINDOWS_UPDATE_SIGN_PASSWORD = '<secret>'
.\scripts\new-windows-update-manifest.ps1 -PublicKeyOnly `
  -PfxPath .\private\zanna-update-signing.pfx `
  -PublicKeyOutput .\build\zanna-update-public-key.json
$updateKey = Get-Content .\build\zanna-update-public-key.json -Raw | ConvertFrom-Json
```

Build the signed installer with the pinned public key and the canonical stable
identity:

```powershell
$env:ZANNA_WINDOWS_SIGN_THUMBPRINT = '<SHA-1 thumbprint>'
$env:ZANNA_WINDOWS_TIMESTAMP_URL = "https://timestamp.digicert.com"
zanna install-package --stage-dir build\release-stage --target windows `
  --output-file artifacts\zanna-toolchain-windows-x64.exe `
  --windows-channel stable `
  --windows-documentation-url https://docs.example.test/zanna/windows `
  --windows-update-manifest-url https://updates.example.test/zanna/stable/x64.txt `
  --windows-update-rsa-modulus $updateKey.modulus `
  --windows-update-rsa-exponent $updateKey.exponent `
  --windows-sign --release
```

A PFX can instead be supplied with `ZANNA_WINDOWS_SIGN_PFX` and
`ZANNA_WINDOWS_SIGN_PASSWORD`. That path passes the password to `signtool` and
therefore also requires `ZANNA_WINDOWS_SIGN_PASSWORD_ARGV_OK=1`; importing the
PFX into the ephemeral user certificate store avoids that exposure. Timestamp
URLs must use HTTPS. Signing is followed by `signtool verify /pa /all /tw /v`.

Nested signing is deliberate: the packager signs and verifies every
Zanna-owned staged PE, the embedded maintenance host, and detached cleanup
helper before hashing and compressing them. Microsoft runtime DLLs retain their
Microsoft signatures. The outer setup is signed last, then recursive
verification checks both signatures and structure through every overlay layer.
Zanna Studio is signed exactly once; because Authenticode changes the PE bytes,
the package builder rebinds only the already-validated `Size` and `SHA256`
buildinfo fields to the exact signed bytes installed in the payload.

After the final installer is placed at its public HTTPS URL, author its bounded
canonical update manifest. The download and release-notes URLs must use the
same scheme, host, and port as the manifest URL:

```powershell
$artifact = Resolve-Path artifacts\zanna-toolchain-windows-x64.exe
.\scripts\new-windows-update-manifest.ps1 `
  -OutputPath artifacts\stable-x64.txt `
  -ManifestUrl https://updates.example.test/zanna/stable/x64.txt `
  -Channel stable -Architecture x64 -Version 1.2.3 `
  -DownloadUrl https://updates.example.test/zanna/zanna-1.2.3-x64.exe `
  -DownloadSha256 (Get-FileHash $artifact -Algorithm SHA256).Hash `
  -ReleaseNotesUrl https://updates.example.test/zanna/1.2.3.html `
  -PfxPath .\private\zanna-update-signing.pfx
```

The script rejects non-canonical or overflowing versions, HTTP, credentials,
fragments, cross-origin URLs, malformed hashes, and inaccessible RSA keys. It
writes deterministic UTF-8 without a BOM using LF records and RSA PKCS#1
SHA-256. Manifest and optional public-key outputs are staged and published as
one rollback-protected set, so a failed second publication cannot leave mixed
release metadata. Keep the PFX private; publish only the text manifest and
public key record used during package generation.

### macOS

macOS release output requires all four gates:

- `ZANNA_MACOS_APP_SIGN_IDENTITY` for every nested Mach-O executable and helper
  app;
- `ZANNA_MACOS_SIGN_IDENTITY` for the product package;
- `ZANNA_MACOS_NOTARY_PROFILE` for `notarytool`; and
- `--macos-staple` for both the package and optional disk image.

```bash
zanna install-package --stage-dir build/release-stage --target macos \
  --macos-dmg --output-dir artifacts --macos-staple --release
```

The builder verifies nested code signatures, the signed product package,
notarization results, stapled tickets, Gatekeeper package assessment, the DMG
UDIF structure, the read-only mounted image, and Gatekeeper's `open` assessment.
Use `--macos-min-version` to override the architecture-based deployment floor.

### Linux

Linux release output requires an OpenPGP key for `.deb` and `.rpm` artifacts:

```bash
export ZANNA_LINUX_SIGN_KEY="<GPG key id>"
zanna install-package --stage-dir build/release-stage --target all \
  --output-dir artifacts --release
```

Debian output is signed with `dpkg-sig` and must report `GOODSIG`; RPM output is
signed with `rpmsign` and must pass `rpmkeys --checksig` (or the `rpm` fallback).
The `.run` and portable tarball use the generated SHA-256 and inventory trust
records rather than package-manager signatures.

## Installer behavior and user experience

### Windows setup

The Windows installer is a statically linked native executable; neither setup
nor maintenance invokes PowerShell. The first page makes Typical the obvious
one-click choice and also offers SDK, Complete, and Customize paths. Customize
selects current-user or all-users scope, a validated Unicode/long-path
destination, components with accurate byte estimates, PATH, safe Open With
associations, and Start Menu shortcuts. Native Task Dialogs and a scrollable
customization window have been replaced in the primary journey by a native
Zanna Games shell: dark charcoal surfaces, a green/steel/teal compile rail,
code-drawn vector branding, and semantic action cards. The shell retains
system high-contrast colors, keyboard navigation, native UI Automation names,
DPI-aware geometry, and a usable scrollable small-work-area layout without
embedding a browser, framework, or raster-art dependency. Focused Windows
dialogs remain in use for errors, Restart Manager decisions, and update
results.

Before mutation, the host validates the complete schema-3 overlay inventory,
PE architecture, component graph, semantic version policy, supported Windows
floor, canonical destination, free space for payload plus transaction backup,
and files in use through Restart Manager. It rejects extra/missing payload
files, reparse traversal, unowned collisions, accidental downgrade, and a
second concurrent lifecycle operation. `/closeApplications` provides a bounded
automation path; cancellation returns 1602 and rolls back.

Install, modify, repair, upgrade, downgrade, and uninstall share a journaled
transaction engine. It stages the selected payload beside the destination,
snapshots owned PATH/registry/shortcut state, atomically swaps directories,
recovers at every commit boundary, and preserves arbitrary unowned developer
files. Repair restores exact owned hashes. A signed cached maintenance image
allows Settings > Apps to expose Modify, Repair, and Uninstall. Detached native
cleanup removes the running installed uninstaller, package cache leaf, and
empty shared cache ancestors without deleting sibling channels or requiring a
reboot. Its command-line policy accepts only explicit child paths below drive
or UNC roots, rejects device namespaces, traversal, alternate streams, reserved
DOS names, duplicates, and reparse points, and never removes directories
recursively.

The package installs its architecture-matched MSVC runtime closure beside the
tools and setup never downloads a redistributable. PATH, associations,
shortcuts, and Add/Remove Programs values carry ownership markers; uninstall
removes only exact owned values. File associations add Open With entries and do
not replace an existing default handler or execute source on open. The finish
page offers Zanna Studio, the Developer Prompt, quickstart, samples, and a copyable
verification command after post-install self-checks.

`/inspect` emits verified package identity as JSON without mutation.
`/checkForUpdates` is offline and deterministic when unconfigured; configured
packages verify the pinned signature, exact channel/architecture, same-origin
URLs, semantic version, and downloaded SHA-256 before presenting an update.
Both commands accept `/output <path>` for atomic UTF-8 JSON publication; this is
the required capture path for GUI-process automation because an unavailable or
failed inherited stream is treated as an error rather than silent success.
`/?` documents silent switches and the lifecycle exit-code contract. Unique
redacted UTF-8 logs default under `%TEMP%` and can be redirected with `/log`.

### macOS Installer and disk image

The product package supplies welcome, license, read-me, destination, and
conclusion panes with generated light/dark artwork. Distribution metadata
declares the allowed host architectures, root-volume restriction, install
domains, and minimum OS. The installed handler app also carries its minimum OS
in `Info.plist`.

The optional DMG has a generated background, volume icon, positioned package
icon, and Applications link. Finder styling is bounded so a headless builder
cannot hang indefinitely; the resulting image is always converted to and
remounted as read-only before it is accepted. Volume names and AppleScript
strings are validated and escaped, and input/output image collisions are
rejected.

### Linux packages and bundle

Debian and RPM packages use conservative runtime dependencies. Developer tools
such as CMake, make, compilers, desktop cache helpers, and manpage cache helpers
are recommendations rather than hard runtime dependencies. Packages include
copyright/README/license metadata, desktop and MIME registrations, hicolor
icons, and post-signature verification.

The `.run` bundle uses an XDG content-addressed cache keyed by the complete
payload hash. It rejects unsafe ownership, permissions, and symlink components;
serializes concurrent first extraction with a recoverable lock; extracts into
an atomic staging directory; verifies the payload before reuse; and supports
quiet and colored terminal output. It does not require FUSE.

Portable Linux tarball `install.sh` and `uninstall.sh` support `PREFIX`,
`DESTDIR`, `--dry-run`, `--force`, and `--quiet`. Installation preflights
unowned conflicts, journals same-filesystem backups, rolls back failures,
removes stale owned files on upgrade, and records the actual prefix. Uninstall
is also transactional and preserves unrelated files.

## Native release workflows

The manually dispatched workflows are:

- `.github/workflows/windows-release-installer.yml`
- `.github/workflows/macos-release-installer.yml`
- `.github/workflows/linux-release-installer.yml`

Each uses the canonical build script, packages a staged tree, verifies every
artifact and checksum, applies native trust when `release=true`, runs lifecycle
checks on a disposable host, and uploads only the resulting artifact directory.
Every native job builds a baseline containing an owned stale file, upgrades to
the final package, proves stale cleanup, preserves an unrelated sentinel,
exercises installed compilation, and uninstalls.

Workflow signing secrets are documented by their references in each YAML file.
Use automation-only signing material and ephemeral runner keychains/keyrings.
No workflow writes private keys into an artifact.

## Native validation handoff

### Windows clean VM

Build and package in a Developer PowerShell:

```powershell
$env:ZANNA_BUILD_TYPE = 'Release'
$env:ZANNA_SKIP_INSTALL = '1'
$env:ZANNA_EXTRA_CMAKE_ARGS = '-DZANNA_INSTALL_ZANNASTUDIO=ON'
.\scripts\build_zanna_win.ps1
cmake --install build --prefix "$PWD\build\release-stage" --config Release
& .\build\src\tools\zanna\Release\zanna.exe install-package --stage-dir build\release-stage --target windows --output-file build\zanna-toolchain.exe
& .\build\src\tools\zanna\Release\zanna.exe install-package --verify-only build\zanna-toolchain.exe --require-checksum
.\scripts\validate-windows-toolchain-installer.ps1 -Installer build\zanna-toolchain.exe
```

The validator gives every child a monotonic finite timeout and streams
stdout/stderr through bounded captures. Timeout and capture-limit failures
terminate the complete Windows process tree, bound and check the terminator,
and prove the root process was reaped before the next lifecycle phase.
Override the defaults with
`-ProcessTimeoutSeconds`, `-MaximumCaptureBytes`, and `-MaximumInspectBytes`
only for a known slow disposable host. It validates every installed tool as an
architecture-matched PE32+ image and checks the complete Studio
version/architecture/size/hash provenance both after installation and after a
Minimal-to-Complete component round trip.

To exercise the upgrade path, package a baseline stage containing
`share\zanna\installer-upgrade-stale.txt`, remove that file before generating
the final installer, then add
`-BaselineInstaller build\zanna-toolchain-baseline.exe` to the validation
script. Add `-RequireSignature` for a signed release candidate.

The repository's opt-in `installer_windows_toolchain_e2e` test is the stronger
developer acceptance gate. It covers deterministic rebuilds, recursive
verification, Unicode custom paths, collision rejection, legacy-manifest
recovery, Typical/Minimal/Complete modification, PATH/association/shortcut
ownership, exact-hash repair, concurrent setup, Restart Manager, installed Zia
and BASIC execution, native codegen, an external CMake SDK consumer, direct
root uninstall, and residue checks. Configure
`ZANNA_ENABLE_WINDOWS_INSTALLER_E2E=ON` and run it serially.

Configure a separate build with `ZANNA_INSTALLER_ENABLE_TEST_HOOKS=ON` to run
the same test's destructive fault matrix. Test hooks are compile-time disabled
in production hosts. The matrix proves no-mutation Windows-version and disk
preflight, cancellation, registry rollback, and recovery after forced
termination at old-directory move, new-directory move, and registry commit for
both repair and uninstall.

Release qualification requires separate native x64 and ARM64 stages. Build
each on a host with the matching Visual Studio C++ workload and MSVC libraries;
do not relabel one architecture. Recursive verification rejects a setup,
maintenance helper, cleanup helper, or payload PE whose COFF machine differs
from package metadata. Run the clean-VM lifecycle on each architecture and on
the minimum Windows 10 floor plus current Windows 11 before publication.

### Linux disposable host or VM

```bash
export ZANNA_BUILD_TYPE=Release ZANNA_SKIP_INSTALL=1
export ZANNA_EXTRA_CMAKE_ARGS=-DZANNA_INSTALL_ZANNASTUDIO=ON
./scripts/build_zanna_linux.sh
cmake --install build --prefix "$PWD/build/release-stage"
build/src/tools/zanna/zanna install-package --stage-dir build/release-stage \
  --target all --output-dir build/installers
for artifact in build/installers/*.deb build/installers/*.rpm \
                build/installers/*.run build/installers/*.tar.gz; do
  build/src/tools/zanna/zanna install-package --verify-only "$artifact" --require-checksum
done
ctest --test-dir build --output-on-failure -R '^linux_toolchain_bundle_smoke$'
sudo env ZANNA_RUN_LINUX_INSTALLER_SMOKE=1 \
  ctest --test-dir "$PWD/build" --output-on-failure \
  -R '^linux_toolchain_(deb|rpm)_installer_smoke$'
```

The privileged package-manager tests refuse to run when a Zanna package is
already installed. Use only a disposable machine: they intentionally install
under `/usr`, perform a same-version baseline-to-current upgrade, build and run
an installed CMake/native-codegen consumer, remove the package, and check that
unrelated content survives.

### macOS disposable host

```bash
export ZANNA_BUILD_TYPE=Release ZANNA_SKIP_INSTALL=1
./scripts/build_zanna_mac.sh
ctest --test-dir build --output-on-failure -R '^macos_toolchain_installer_smoke$'
sudo env ZANNA_RUN_MACOS_INSTALLER_SMOKE=1 \
  ctest --test-dir "$PWD/build" --output-on-failure \
  -R '^macos_toolchain_installer_smoke$'
```

The privileged macOS test refuses a host with an existing installation, asks
Installer.app to evaluate the Distribution choices, installs into `/usr/local`,
upgrades from a baseline package when `ZANNA_BASELINE_PACKAGE` is supplied,
proves stale-owned cleanup and unrelated-file preservation, checks all commands,
CMake discovery and native codegen, runs the installed uninstaller, and verifies
package receipt removal.
