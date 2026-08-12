---
status: active
audience: contributors
last-verified: 2026-08-11
---

# Zanna Internals Documentation

Contributor-facing documentation: system design, subsystem code maps, formal
specifications, decision records, and guides for extending the toolchain.
User-facing documentation lives in the **[main documentation index](../README.md)**.

---

## Architecture

- [Architecture Overview](architecture.md) — System design: frontends, IL, VM, codegen
- [Code Map](codemap.md) — Source directory layout and subsystem overview
- [VM Guide](vm.md) — VM design, dispatch, profiling, and runtime ABI
- [Bytecode VM Reference](bytecode-vm.md) — Bytecode format, dispatch, runtime, and test surface
- [Backend Guide](backend.md) — x86-64 and ARM64 native code generation
- [Graphics3D Architecture](graphics3d-architecture.md) — 3D subsystem internals
- [Native Assembler](native-assembler.md) — Binary encoders and object file writers (x86_64/AArch64 × ELF/Mach-O/COFF)
- [Native Linker](native-linker.md) — Archive reading, symbol resolution, relocation, and executable output

## Code Map Deep Dives

**IL Subsystem** — [Core](codemap/il-core.md) · [Build](codemap/il-build.md) · [I/O](codemap/il-i-o.md) · [Transform](codemap/il-transform.md) · [Analysis](codemap/il-analysis.md) · [Link](codemap/il-link.md) · [Runtime](codemap/il-runtime.md) · [API](codemap/il-api.md) · [Verification](codemap/il-verification.md) · [Utilities](codemap/il-utilities.md)

**Frontends** — [Zia](codemap/front-end-zia.md) · [BASIC](codemap/front-end-basic.md) · [Common](codemap/front-end-common.md)

**Other** — [Bytecode VM](codemap/bytecode-vm.md) · [Codegen](codemap/codegen.md) · [Graphics](codemap/graphics.md) · [Graphics Stubs](codemap/runtime-graphics-stubs.md) · [VM/Runtime](codemap/vm-runtime.md) · [Runtime C Library](codemap/runtime-library-c.md) · [Support](codemap/support.md) · [Tools](codemap/tools.md) · [TUI](codemap/tui.md) · [Zanna Studio](codemap/zannastudio.md) · [Zia Server](codemap/zia-server.md) · [Docs](codemap/docs.md)

**Runtime audits** — [Graphics 2D runtime (July 2026)](graphics2d-runtime-audit-2026-07.md)

## Specifications

- [IL Guide](../il/il-guide.md) — Normative IL specification (changes require an ADR)
- [Error Model](../specs/errors.md) — Trap kinds, handler semantics, error model
- [Numeric Types](../specs/numerics.md) — Numeric types, ranges, IEEE semantics
- [Object Layout / ABI](../specs/object-layout.md) — Object layout, vtable, call ABI
- [Threading and Globals](../specs/threading-and-globals.md) — VM threading model
- [x86-64 Backend](../specs/x86_64.md) — x86-64 SysV ABI reference
- [AArch64 Backend](../specs/aarch64.md) — AArch64 backend specification
- [Compiler Specification](specifications.md) — Internal compiler behavior spec
- [Requirements](requirements.md) — Project requirements and constraints

## Architecture Decision Records

- [ADR Index](../adr/README.md) — All decision records, grouped by area

## Extending Zanna

- [Frontend How-To](frontend-howto.md) — Build your own language frontend
- [Runtime Extension How-To](runtime-extend-howto.md) — Add new runtime classes and functions
- [Adding a Runtime Class](runtime-class-howto.md) — Deep dive: 8-step guide
- [Generated Files](generated-files.md) — Auto-generated C++ sources and how to regenerate them

## Development Workflow

- [Contributor Guide](contributor-guide.md) — Style guide and contribution process
- [Documentation Style Guide](doc-style.md) — Layout, naming, and formatting rules for `docs/`
- [Testing Guide](testing.md) — Unit, golden, e2e, and performance tests
- [Source Health Guardrails](source-health.md) — Local audit baselines for high-ownership subsystems

## Platform Implementation Notes

- [Cross-Platform Differences](../cross-platform/platform-differences.md) — macOS vs Linux vs Windows behavior
- [Cross-Platform Checklist](../cross-platform/platform-checklist.md) — Compliance tracking
- [Linux Platform Implementation](../linux-platform.md) — Wayland/X11, ALSA, inotify, PTY, and cgroup-aware adapters
- [Wayland Hardening Audit](../wayland-hardening-audit.md) — Native Wayland capability status and remaining work
- [Windows Runtime Reliability Audit](../windows-runtime-reliability-audit.md) — Win32 and D3D11 adapter hardening and validation

## Subsystem Audits

Living ledgers that track findings and their resolution for high-risk subsystems.

- [Runtime Hardening Audit (July 2026)](runtime-hardening-audit-2026-07.md) — Resolution ledger for 64 runtime findings
- [Audio Runtime Hardening Program (August 2026)](audio-runtime-hardening-2026-08.md) — Resolution ledger for 100 WAV, Ogg, Vorbis, MP3, streaming, and mixer findings
- [Audio Runtime Hardening, Round Two (August 2026)](audio-runtime-hardening-round-two-2026-08.md) — Resolution ledger for 100 additional decoder-wrapper, synth, effects, soundbank, MusicGen, playlist, and crossfade findings
- [GUI Runtime Hardening Program (August 2026)](gui-runtime-hardening-2026-08.md) — Resolution ledger for the first 100 GUI C runtime findings
- [GUI Runtime Hardening, Round Two (August 2026)](gui-runtime-hardening-round-two-2026-08.md) — Resolution ledger for 100 additional text, numeric, and retained-state boundary findings
- [GUI Runtime Hardening, Round Three (August 2026)](gui-runtime-hardening-round-three-2026-08.md) — Resolution ledger for 100 transactional text-boundary findings plus supporting consistency migrations
- [Graphics3D Runtime Hardening Program (July 2026)](graphics3d-runtime-hardening-2026-07.md) — 48-item audit of `src/runtime/graphics/3d`
- [Graphics3D Backend Correctness Audit (July 2026)](graphics3d-backend-audit-2026-07.md) — OpenGL, D3D11, Metal, and software renderer boundary review
- [Graphics3D Animation and Navigation Audit (August 2026)](graphics3d-animation-navigation-audit-2026-08.md) — Resolution ledger for 104 IK, morph-target, and navigation findings
- [Graphics3D Runtime Correctness Audit Follow-up (August 2026)](graphics3d-runtime-audit-followup-2026-08.md) — Resolution ledger for 114 foundational runtime findings
- [Graphics3D Game-Core Correctness Audit (August 2026)](graphics3d-game-core-audit-2026-08.md) — Resolution ledger for 186 Game3D, controller, hierarchy, diagnostics, and boundary findings
- [Graphics3D Core Runtime Deep Audit (August 2026)](graphics3d-core-runtime-deep-audit-2026-08.md) — Resolution ledger for 163 animation, skinning, cloth, navmesh, and VSCN findings
- [Graphics3D Runtime Integrity Audit (August 2026)](graphics3d-runtime-integrity-audit-2026-08.md) — Resolution ledger for 185 behavior, transient ownership, temporal history, physics, loader, and particle findings
- [Graphics3D World Runtime Integrity Audit (August 2026)](graphics3d-world-runtime-integrity-audit-2026-08.md) — Resolution ledger for 160 Water3D, Vegetation3D, and Particles3D ownership, retained-state, allocation, and hot-path findings
- [Graphics3D Runtime Hardening, Round Two (August 2026)](graphics3d-runtime-hardening-round-two-2026-08-09.md) — Resolution ledger for 105 World3D, Timeline3D, Canvas3D readback, and BC6H findings
- [Graphics3D Runtime Hardening, Round Three (August 2026)](graphics3d-runtime-hardening-round-three-2026-08-10.md) — Resolution ledger for 100 PostFX3D ownership, numeric, packed-buffer, temporal, and backend-chain findings
