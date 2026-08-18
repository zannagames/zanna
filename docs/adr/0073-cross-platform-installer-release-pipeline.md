---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0073: Cross-Platform Installer Release Pipeline

Date: 2026-07-10
Status: Accepted

## Context

Zanna generates native toolchain installers for Windows, macOS, and Linux, but
the release path is not represented consistently in the repository. ADR 0025
records a Windows release workflow that is no longer present, macOS signing and
notarization do not yet validate every nested executable, and Linux package
signing is not followed by signature verification. Privileged installer smoke
tests are opt-in and slow, so an ordinary build proves package structure but not
the complete release, installation, upgrade, and removal lifecycle.

Installer workflow changes are an ADR-triggered surface. A single decision is
needed for the release policy shared by all three operating-system families.

## Decision

Add native-host release workflows for Windows, macOS, and Linux. The workflows
remain manually dispatchable while release credentials and clean-machine test
capacity are being configured. Each workflow must:

1. build with the canonical platform build script;
2. generate packages with `zanna install-package`;
3. run native structural verification for every artifact;
4. sign and verify signatures when release credentials are configured;
5. run the platform's install, functional, upgrade, and uninstall checks on a
   disposable runner or VM;
6. emit SHA-256 checksums and a machine-readable artifact manifest; and
7. upload only artifacts that pass all enabled release gates.

The Linux self-extracting toolchain format is named `linux-bundle` and uses the
`.run` suffix. It is not an AppImage filesystem image, so `install-package` does
not provide an `appimage` target or `.AppImage` verification alias.

Unsigned packages remain available for local development and pull-request
validation. A release-mode packaging path will reject verification bypasses and
will require the platform trust operations selected by the workflow:

- Windows Authenticode signing with an RFC 3161 timestamp and post-signature
  verification;
- macOS Developer ID signing for nested executable code, installer signing,
  notarization, stapling, and Gatekeeper assessment; and
- Debian/RPM signing followed by native signature verification when signing is
  enabled.

The workflows use only repository code and operating-system tools. They do not
add product dependencies. Secrets are supplied through the CI secret store and
must not be written to artifacts, logs, process metadata when an alternative is
available, or repository files.

## Consequences

- The repository once again contains the release process described by its
  documentation.
- Structural package tests and trusted release validation are explicitly
  separate, so unsigned developer artifacts remain easy to build.
- Public artifacts have a consistent checksum and provenance inventory.
- macOS, Windows, and Linux release behavior is tested on native hosts instead
  of inferred from cross-platform archive generation alone.
- Workflow changes must keep this ADR and the installer documentation current.

## Alternatives Considered

- **One cross-platform workflow job.** Rejected because signing, notarization,
  package-manager installation, and architecture validation require native host
  capabilities and different credential boundaries.
- **Rely only on structural verification.** Rejected because archive validity
  does not prove trust, dependency resolution, installation, upgrade, or clean
  removal.
- **Require credentials on every development build.** Rejected because forks
  and local contributors must be able to exercise package generation without
  access to release secrets.
