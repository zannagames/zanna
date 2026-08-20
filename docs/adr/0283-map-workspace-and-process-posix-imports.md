---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0283: Map Workspace Durability and Process Ownership POSIX Imports

## Status

Accepted (2026-08-20)

## Context

Zanna's runtime is linked into native applications from in-tree static
archives. The in-tree native linker deliberately accepts only reviewed loader
imports rather than treating every unresolved name as a system symbol.

Workspace transaction hardening now uses descriptor-relative reads, ownership
and metadata preservation, advisory leases, and extended attributes. Complete
process-tree ownership also configures POSIX spawn attributes and process
groups. Those runtime objects therefore reference `pread`, `fchown`, `flock`,
the descriptor xattr family, and `posix_spawnattr_*`. macOS additionally uses
`fchflags` to preserve file flags.

The platform SDKs already provide those calls, and runtime unit tests linked by
the host toolchain passed. A clean native Zanna Studio build nevertheless
failed at the Zanna-owned link because its fixed dynamic-symbol policy did not
admit the new archive closure. This is a cross-layer dependency between runtime
adapters and the native linker and must not drift silently.

## Decision

The shared POSIX dynamic-symbol policy admits these exact imports on Linux and
macOS:

- `pread`, `fchown`, and `flock`;
- `flistxattr`, `fgetxattr`, and `fsetxattr`;
- `posix_spawnattr_init`, `posix_spawnattr_destroy`,
  `posix_spawnattr_getflags`, `posix_spawnattr_setflags`, and
  `posix_spawnattr_setpgroup`.

The policy admits `fchflags` only for macOS. Its presence in the macOS-exclusive
set is both a positive platform contract and a negative filter for Linux and
Windows.

The platform import-planner regression enumerates this closure. Future runtime
changes that add a system import must extend the applicable fixed policy and
its platform-scoping test in the same change. The existing runtime import audit
remains the broad archive-closure check.

## Consequences

- Native applications can link the hardened workspace-edit and process runtime
  through Zanna's own linker on supported POSIX hosts.
- Typos and foreign-platform APIs remain hard link failures; the policy does
  not gain a permissive prefix or arbitrary fallback.
- No product dependency, IL change, runtime C ABI change, or serialized-format
  change is introduced.
- A host-toolchain runtime unit test alone is not sufficient evidence when a
  change adds system imports; the native-link policy test is also required.

## Alternatives Considered

- **Allow every unresolved libc-looking name.** Rejected because misspellings
  and foreign APIs would become loader-time failures instead of deterministic
  link diagnostics.
- **Add in-tree forwarding wrappers for each call.** Rejected because the
  supported system libraries already define the required ABI and wrappers
  would only move the same dependency.
- **Revert descriptor metadata and process-group hardening.** Rejected because
  it would reopen data-loss, metadata-stripping, and orphan-process defects.
