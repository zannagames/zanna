---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0025: Windows Release Installer Workflow

## Status

Accepted (workflow added; manual dispatch is the first supported release path).

## Context

The packaging documentation describes a `Windows Release Installer` GitHub Actions
workflow, but the repository did not include the workflow file. Installer workflow
changes are an ADR-triggered surface because they define how release artifacts are
built, signed, and validated.

## Decision

Add `.github/workflows/windows-release-installer.yml` as a manual
`workflow_dispatch` release workflow. It builds with the canonical Windows build
script, enables Zanna Studio packaging, creates a Windows toolchain installer with
`zanna install-package`, optionally signs from repository secrets, verifies the
artifact structurally, and uploads the installer artifact.

Signing remains opt-in. PFX signing uses `ZANNA_WINDOWS_SIGN_PFX_BASE64` and
`ZANNA_WINDOWS_SIGN_PASSWORD`; certificate-store signing can be supplied by setting
`ZANNA_WINDOWS_SIGN_THUMBPRINT` in the workflow environment or repository
configuration.

## Consequences

- The documented release workflow now exists and is reproducible from a clean
  Windows runner.
- The workflow uses the repository build script instead of open-coded CMake calls,
  keeping release packaging aligned with local validation.
- Unsigned artifacts remain supported for test builds; signed release artifacts use
  the same `zanna install-package --windows-sign` path as local release builds.

## Alternatives Considered

- **Build on every push.** Rejected because installer signing and artifact upload
  are release operations, not ordinary CI for every branch.
- **Require signing secrets.** Rejected so maintainers can validate installer
  generation on forks and internal branches without private certificate material.
