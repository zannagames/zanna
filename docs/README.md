---
status: active
audience: public
last-verified: 2026-07-26
---

# Zanna Documentation

Zanna is a from-scratch compiler toolchain and game development platform: two language frontends ([Zia](languages/zia-reference.md) and [BASIC](languages/basic-reference.md)) lower to a shared [intermediate language](il/il-guide.md) that runs on a [VM](internals/vm.md) or compiles to [native code](internals/backend.md), on top of a comprehensive [runtime library](zannalib/README.md).

> Contributor and internals documentation (architecture, code maps, specs, ADRs) lives in the **[Internals index](internals/README.md)**.

---

## Getting Started

- [Getting Started](getting-started.md) — Build, install, and run your first program
- Platform setup: [macOS](getting-started/macos.md) · [Linux](getting-started/linux.md) · [Windows](getting-started/windows.md)
- [FAQ](faq.md) — Common questions answered
- [Installer and Package Release Guide](installer-release.md) — Native artifacts, signing, verification, and lifecycle validation

## Learning Zanna

- [Zia Tutorial](tutorials/zia-tutorial.md) — Learn Zia by example
- [BASIC Tutorial](tutorials/basic-tutorial.md) — Learn Zanna BASIC by example
- [Your First 3D Game](tutorials/3d-game-tutorial.md) — Scaffold, edit, play, and package a scene-driven 3D game
- [The Zanna Book](book/README.md) — A five-part course from first program to full applications

## Language References

- **Zia** — [Language Reference](languages/zia-reference.md) · [Grammar](languages/zia-grammar.md)
- **BASIC** — [Language Reference](languages/basic-reference.md) · [Namespaces](languages/basic-namespaces.md) · [Grammar Notes](languages/basic-grammar.md) · [Lifetime Model](languages/lifetime.md)
- **Cross-language** — [Interop](languages/interop.md) · [Arithmetic Semantics](languages/arithmetic-semantics.md)
- **Zanna IL** — [IL Guide](il/il-guide.md) (normative; includes quickstart and full reference) · [Optimization Passes](il/il-passes.md)

## Runtime Library

- [Runtime Library Index](zannalib/README.md) — All `Zanna.*` classes and methods
- [Generated API Reference](generated/runtime/README.md) — Exhaustive, source-generated class inventory
- [Runtime Architecture](zannalib/architecture.md) — Runtime design and structure
- [Memory Management](memory-management.md) — GC, allocation, and object lifetime

### Module Reference

**[Collections](zannalib/collections/README.md)** — [Sequential](zannalib/collections/sequential.md) · [Maps & Sets](zannalib/collections/maps-sets.md) · [Multi-Maps](zannalib/collections/multi-maps.md) · [Specialized](zannalib/collections/specialized.md) · [Functional](zannalib/collections/functional.md)

**[Core](zannalib/core.md)** · **[Crypto](zannalib/crypto.md)** · **[Diagnostics](zannalib/diagnostics.md)** · **[Functional](zannalib/functional.md)**

**[Game](zannalib/game/README.md)** — [Core](zannalib/game/core.md) · [Game Loop](zannalib/game/gameloop.md) · [Animation](zannalib/game/animation.md) · [Effects](zannalib/game/effects.md) · [Pathfinding](zannalib/game/pathfinding.md) · [Physics](zannalib/game/physics.md) · [Persistence](zannalib/game/persistence.md) · [Debug](zannalib/game/debug.md) · [UI](zannalib/game/ui.md)

**[Graphics](zannalib/graphics/README.md)** — [Canvas](zannalib/graphics/canvas.md) · [Scene](zannalib/graphics/scene.md) · [Pixels](zannalib/graphics/pixels.md) · [Fonts](zannalib/graphics/fonts.md)

**[Game3D](zannalib/graphics/game3d.md)** — code-first 3D game workflow helpers over `Zanna.Graphics3D`

**[Graphics3D](graphics3d-guide.md)** — 3D rendering, physics, and world systems guide

**[GUI](zannalib/gui/README.md)** — [Application](zannalib/gui/application.md) · [Core](zannalib/gui/core.md) · [Widgets](zannalib/gui/widgets.md) · [Containers](zannalib/gui/containers.md) · [Layout](zannalib/gui/layout.md)

**[I/O](zannalib/io/README.md)** — [Files](zannalib/io/files.md) · [Streams](zannalib/io/streams.md) · [Advanced](zannalib/io/advanced.md)

**[Audio](zannalib/audio.md)** · **[Input](zannalib/input.md)** · **[Localization](zannalib/localization/README.md)** · **[Math](zannalib/math.md)** · **[Network](zannalib/network.md)**

**[System](zannalib/system.md)** · **[Text](zannalib/text/README.md)** — [Formats](zannalib/text/formats.md) · [Formatting](zannalib/text/formatting.md) · [Encoding](zannalib/text/encoding.md) · [Patterns](zannalib/text/patterns.md)

**[Threads](zannalib/threads.md)** · **[Time](zannalib/time.md)** · **[Utilities](zannalib/utilities.md)** · **[Zia Tooling](zannalib/zia.md)**

**[Graphics Library (2D)](graphics-library.md)** — ZannaGFX low-level 2D API

## Game Engine

- [Game Engine Documentation](gameengine/README.md) — Complete guide to the Zanna game engine
- [Getting Started](gameengine/getting-started.md) — Your first game in 15 minutes
- [Architecture](gameengine/architecture.md) — Engine systems and zero-dependency design
- [Example Games](gameengine/examples/README.md) — Example games from arcade to Metroidvania
- [Zanna Studio](../src/zannastudio/README.md) — The IDE with built-in 2D/3D scene editors, embedded Play, and project scaffolding
- [3D Graphics Guide](graphics3d-guide.md) — The complete `Zanna.Graphics3D` API guide
- [VSCN Scene Format](specs/vscn-scene-format.md) — The `.scene3d` serialization contract

## Tools

- [CLI Tools Reference](tools/cli.md) — `zanna`, `zia`, `zbasic`, `ilrun`, `il-verify`, `il-dis`
- [REPL](tools/repl.md) — Interactive Zia/BASIC environment
- [Debugging Guide](tools/debugging.md) — VM tracing, breakpoints, and diagnostics
- [Language Server](tools/zia-server.md) — `zia-server` for AI assistants and editors (LSP + MCP)
- [MCP Tools Reference](tools/zia-server-mcp-tools.md) — Detailed MCP tool definitions

## Release Notes

Latest release: **v0.3.0** ([release notes](release_notes/Zanna_Release_Notes_0_3_0.md)).

In development: **v0.3.1-snapshot** ([draft release notes](release_notes/Zanna_Release_Notes_0_3_1.md)).

[v0.3.0](release_notes/Zanna_Release_Notes_0_3_0.md) · [v0.2.7](release_notes/Zanna_Release_Notes_0_2_7.md) · [v0.2.6](release_notes/Zanna_Release_Notes_0_2_6.md) · [v0.2.5](release_notes/Zanna_Release_Notes_0_2_5.md) · [v0.2.4](release_notes/Zanna_Release_Notes_0_2_4.md) · [v0.2.3](release_notes/Zanna_Release_Notes_0_2_3.md) · [v0.2.2](release_notes/Zanna_Release_Notes_0_2_2.md) · [v0.2.1](release_notes/Zanna_Release_Notes_0_2_1.md) · [v0.2.0](release_notes/Zanna_Release_Notes_0_2_0.md) · [v0.1.3](release_notes/Zanna_Release_Notes_0_1_3.md) · [v0.1.2](release_notes/Zanna_Release_Notes_0_1_2.md)

---

## For Contributors

The **[Internals index](internals/README.md)** covers system architecture, the VM and native backends, code maps for every subsystem, formal specifications, architecture decision records, testing, platform implementation notes, and how-to guides for extending the runtime and building new frontends.
