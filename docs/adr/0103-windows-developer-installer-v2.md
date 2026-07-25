---
status: active
audience: contributors
last-verified: 2026-07-16
---

# ADR 0103: Windows Developer Installer v2

Date: 2026-07-15
Status: Accepted

The Task Dialog and system-light presentation choice in this ADR's user
experience section is superseded by
[ADR 0175](0175-zanna-games-windows-installer-experience.md). Its native
control, accessibility, DPI, high-contrast, and small-work-area requirements
remain active.

## Context

Zanna's Windows package builder already validates staged files, hashes the
payload, rejects unsafe paths and reparse-point traversal, owns its PATH and
registry changes, and journals installation upgrades. The resulting developer
installer is nevertheless not self-contained or ready for broad use:

- release executables use the dynamic Microsoft C++ runtime without deploying
  the required runtime files;
- Authenticode signing is applied to the outer setup after the payload and
  generated uninstaller have already been embedded;
- the x64 uninstaller cannot remove itself and reports success while leaving
  residue, and removal is not journaled like installation;
- x64 and ARM64 expose different license, component, progress, and cleanup
  behavior;
- the generated bootstrap delegates lifecycle operations to an encoded Windows
  PowerShell program, which can be restricted by App Control and cannot provide
  the required native cancellation, process-in-use, or accessibility behavior;
- scope, destination, version policy, maintenance, component persistence,
  dependency status, and disk requirements are fixed or absent; and
- file associations execute source files rather than opening them safely.

Correcting these issues changes the dependency boundary between the build,
installed toolchain, package builder, bootstrap, and lifecycle engine. The
repository therefore requires an ADR before implementation.

## Decision

### Native architecture

The Windows developer installer uses a statically linked native host built from
repository sources for x64 and ARM64. The outer setup executable is that host
with an authenticated metadata and payload overlay. The overlay contains a
separately signed copy of the same host for cached maintenance, a small signed
detached cleanup helper, and the compressed, manifest-validated toolchain
payload.

The host has no Microsoft C++ runtime dependency and runs directly from the
downloaded setup image. It owns the wizard, preflight, extraction, transactions,
metadata, maintenance, update discovery, logging, and handoff to cleanup. The
cleanup helper removes the cached maintenance image and now-empty cache roots
after their processes exit. Windows PowerShell is not part of the canonical
installation path.

The installer host uses only the C++ standard library and Windows system APIs.
No external library, package download, installer framework, or generated
third-party runtime is introduced.

### Runtime self-containment

The installed command-line tools, language servers, Zanna Studio, and exported SDK
libraries retain one consistent MSVC runtime model. Release staging installs
the architecture-matched redistributable runtime files permitted by the active
Microsoft toolchain beside the executables. The package build fails when a PE
imports a non-system runtime DLL that is absent from the staged `bin`
directory. No runtime is downloaded during setup.

The native installer host is built with the static MSVC runtime so it can run
before the staged runtime exists. Runtime files retain their Microsoft
signatures and are verified as trusted inputs; Zanna does not re-sign them.

### Trust ordering

Trusted release generation uses this order:

1. copy the validated stage to a private release workspace;
2. sign every Zanna-owned payload PE in that workspace and verify the expected
   signer, while preserving Microsoft-signed runtime files unchanged;
3. generate, sign, and verify the embedded maintenance host and cleanup helper;
4. gather the final payload inventory and hashes from those signed bytes;
5. append canonical schema-3 metadata and the compressed payload to the native
   setup host;
6. sign the outer setup last; and
7. recursively verify the setup, maintenance host, cleanup helper, every
   Zanna-owned payload PE, inventories, hashes, and architecture labels.

Unsigned developer packages remain supported and are explicitly identified as
such. A trusted package can never contain an unsigned Zanna-owned executable.

### Installation and maintenance contract

The native host will:

- acquire a named mutex for the package identifier, scope, and canonical
  destination, returning 1618 when another lifecycle operation is active;
- offer current-user and all-users scope at runtime and request elevation only
  for all-users work;
- allow a validated custom destination and support Unicode, spaces, long paths,
  and non-system fixed volumes;
- calculate selected installed size, transaction staging, upgrade backup, and
  safety margin before mutation;
- compare semantic versions and distinguish install, upgrade, reinstall,
  repair, modify, and downgrade, blocking downgrade unless explicitly allowed;
- use Restart Manager to report files in use and support close, retry, cancel,
  and eligible application restart;
- persist component choices and use them as upgrade defaults;
- expose Minimal, Typical, SDK, Complete, and Custom component selections with
  accurate sizes;
- journal both installation and removal, retain the ownership manifest until
  removal commit, and recover interrupted operations;
- use a detached native cleanup process so successful uninstall removes the
  installed uninstaller and empty installation root without a reboot;
- return documented Windows lifecycle exit codes and never report completion
  while owned files remain;
- provide unique, redacted session logs plus `/quiet`, `/passive`, `/log`,
  `/norestart`, `/allowdowngrade`, and `/?`; and
- support cooperative cancellation with rollback and exit code 1602.

Add/Remove Programs will expose Modify, Repair, and Uninstall. Repair verifies
owned hashes and restores only missing or damaged content. Modify changes
selected components transactionally.

### User experience and integration

The interactive host uses native Task Dialog pages for the one-click Welcome,
license/ready confirmation, progress, maintenance, update, error, and Finish
states, plus a native scrollable customization window for scope, destination,
presets, components, and integrations. It follows system colors and
high-contrast mode, supports keyboard navigation and UI Automation, scales from
100 through 300 percent DPI, and remains usable at a 1366 by 768 display work
area.

The finish page offers Zanna Studio, a Zanna Developer Prompt, the quickstart,
samples, and a copyable verification command after a successful self-check.

File associations open files in Zanna Studio and are installed only when Zanna Studio
is selected. Explicit `Run with Zanna` and `Check with Zanna` verbs may be
offered, but source execution is never the default action.

VS Code integration is selectable only when a validated VSIX is present. Its
runtime and packaging must be implemented from repository sources without
`npm`, package downloads, or external JavaScript libraries.

Core Zanna is self-contained. Optional SDK/native-development onboarding may
detect CMake, a supported compiler, Windows SDK, Git, VS Code, and Windows
Terminal, but setup does not silently install or download those products.

Update discovery consumes a bounded canonical UTF-8/LF HTTPS manifest. The host
pins its RSA public key, verifies the PKCS#1 SHA-256 signature before parsing an
offer, requires channel and architecture equality, requires download and notes
URLs to share the manifest's HTTPS origin, and verifies the downloaded artifact
hash before it can be launched. An unconfigured package reports that state
without network access. Hosting and package-manager publication are outside
this ADR.

### Metadata and reproducibility

One canonical build identity supplies the filename, PE resources, Add/Remove
Programs version, CLI build identity, architecture, channel, commit, artifact
inventory, and update metadata. Trusted release generation rejects dirty or
mismatched stages. Unsigned local packages default to a collision-safe
`development` channel, package identifier, display name, and install directory;
the stable identity is reserved for release mode.

Setup, host, uninstaller, and installed executables declare the supported
Windows floor. PE and Add/Remove Programs metadata include complete publisher,
copyright, support, documentation, and update fields. Build-time metadata is
derived from `SOURCE_DATE_EPOCH` in UTC; installation time is recorded only at
runtime. Rebuilding from the same inputs in another path, date, or time zone
must produce identical unsigned bytes.

## Validation Requirements

Unit and structural tests cover manifest validation, PE dependency discovery,
signing order, version policy, size calculation, path validation, component
state, command-line parsing, log redaction, and reproducible metadata.

Native lifecycle tests cover x64 and ARM64, Windows 10 and 11, current-user and
all-users installation, clean machines without developer tools or an existing
VC runtime, install, modify, repair, upgrade, explicit downgrade, cancellation,
locked processes, concurrent setup, disk exhaustion, registry denial,
interrupted transactions, corrupt overlays, Unicode and long destinations, and
residue-free uninstall.

UI validation covers keyboard-only navigation, UI Automation names, Narrator,
high contrast, system light and dark colors, 100 through 300 percent DPI, and
the minimum supported work area. Release acceptance also verifies every nested
signature and performs a first-run compiler, VM, native-code, Zanna Studio, language
server, and installed CMake-package smoke.

## Compatibility and Rollout

The package identifier and ownership manifest schema receive explicit versions.
The v2 host uses canonical package metadata schema 3 and migrates installations
generated by the prior installer only after reconstructing ownership from a
fully verified cached maintenance package. It never claims arbitrary adjacent
files when an old manifest is missing.
Stable and development channels use different identifiers so they can coexist.

Existing `/quiet`, `/silent`, and `/norestart` spellings remain accepted.
Automation receives deterministic documented exit codes. Old installers remain
verifiable but are not used as release baselines after v2 becomes canonical.

## Consequences

- The canonical installer becomes independent of PowerShell policy and provides
  one behavior on x64 and ARM64.
- The helper adds one Windows-only build target and a bootstrap protocol, but it
  removes a large generated PowerShell program and duplicated architecture
  behavior.
- Trusted package generation needs a private signed-stage workspace and cannot
  treat outer signing as sufficient.
- The installed toolchain gains a small set of redistributable compiler-runtime
  files but no network or third-party library dependency.
- Maintenance, accessibility, clean removal, and failure recovery become tested
  product contracts rather than documentation claims.

## Alternatives Considered

- **Continue extending the encoded PowerShell backend.** Rejected because Group
  Policy and App Control can restrict it, and the x64/ARM64 UI and lifecycle
  divergence would continue.
- **Adopt MSI, WiX, Inno Setup, or another installer framework.** Rejected
  because it adds an external product dependency and package-download surface.
- **Statically link every installed Zanna target.** Rejected as the sole answer
  because the exported static SDK libraries and consumer runtime model must stay
  consistent; deploying the compiler runtime beside `bin` preserves that model.
- **Download the VC redistributable during setup.** Rejected because offline and
  reproducible installation is required and product downloads are prohibited.
- **Keep source execution as the default file verb.** Rejected because opening a
  downloaded source file must not execute it.
- **Use separate feature sets on x64 and ARM64.** Rejected because architecture
  is an implementation detail, not a user-visible capability tier.

## Spec Impact

This ADR changes the Windows build, packaging, signing, installation,
maintenance, and removal boundaries. It does not change IL grammar, opcodes,
verifier rules, the normative IL reference, or the runtime C ABI.
