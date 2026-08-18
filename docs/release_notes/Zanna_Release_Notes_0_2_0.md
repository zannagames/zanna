---
status: active
audience: public
last-verified: 2026-08-17
---

# Zanna Compiler Platform - Release Notes

> **Development Status**: Pre-Alpha  
> These are early development releases. Zanna is under active development and not ready for production use.  
> Future milestones will define supported releases when appropriate.

## Version 0.2.0 - Pre-Alpha (January 2026)

### Release Overview

Version 0.2.0 opens a new development phase focused on runtime expansion and frontend stability. This release renames
ZannaLang to **Zia**, adds a **GUI widget library**, introduces **simplified CLI tools**, and expands the runtime
with networking, input handling, and game development infrastructure.

### Platform Changes

#### ZannaLang Renamed to Zia

The ZannaLang frontend has been renamed to **Zia**. This includes:

- New file extension: `.zia` (replaces `.zanna`)
- New compiler tool: `zia`
- Updated documentation: `zia-getting-started.md`, `zia-reference.md`

Example:

```zia
module Hello;

func start() {
    Zanna.Terminal.Say("Hello from Zia!");
}
```

Run with:

```bash
./build/src/tools/zia/zia hello.zia
```

#### Simplified CLI Tools

New user-friendly compiler drivers replace verbose `zanna` subcommands:

| Old Command | New Command |
|-------------|-------------|
| `zanna front basic -run file.bas` | `vbasic file.bas` |
| `zanna front zia -run file.zia` | `zia file.zia` |
| `zanna -run file.il` | `ilrun file.il` |

The `zanna` tool remains available for advanced use cases.

### New Features

#### GUI Widget Library (`Zanna.GUI.*`)

A new cross-platform GUI widget library (~26,000 lines):

- `App` — Application window with event loop
- `VBox` / `HBox` — Vertical and horizontal layout containers
- `Button` — Clickable buttons with labels
- `Slider` — Value sliders
- `Theme` — Light and dark theme support
- IDE-oriented widgets for building development tools

Example:

```zia
func main() {
    var app = Zanna.GUI.App.New("My App", 800, 600);
    Zanna.GUI.Theme.SetDark();
    
    var container = Zanna.GUI.VBox.New();
    container.SetSpacing(8.0);
    app.Root.AddChild(container);
    
    var button = Zanna.GUI.Button.New("Click Me");
    container.AddChild(button);
    
    app.Run();
}
```

#### Audio Library

Basic audio playback support (~3,000 lines) for sound effects and music.

#### Networking (`Zanna.Network.*`)

TCP and UDP networking with HTTP client support:

- `Tcp` — TCP client with send/receive, timeouts, line-based protocols
- `TcpServer` — TCP server with listen/accept
- `Udp` — UDP sockets with multicast support
- `Dns` — Name resolution, reverse lookup, local address queries
- `Http` — Simple HTTP GET/POST/HEAD requests
- `HttpReq` / `HttpRes` — Full HTTP client with headers and body handling
- `Url` — URL parsing, encoding, query parameter manipulation

#### Input Handling (`Zanna.Input.*`)

Keyboard, mouse, and gamepad input for interactive applications:

- `Keyboard` — Polling and event-based keyboard input, modifier state, text input mode
- `Mouse` — Position, button state, wheel, capture/release, delta movement
- `Pad` — Gamepad support with analog sticks, triggers, buttons, vibration

#### Game Development (`Zanna.Graphics.*`)

Sprite, tilemap, and camera support for 2D games:

- `Sprite` — Animated sprites with multiple frames, collision detection, origin point
- `Tilemap` — Tile-based level rendering with configurable tile size
- `Camera` — Viewport scrolling, class following, world/screen coordinate conversion
- `Color` — RGB/HSL conversion, lerp, brighten/darken utilities

#### I/O Additions (`Zanna.IO.*`)

- `Archive` — ZIP file creation, extraction, and inspection
- `Compress` — Gzip and deflate compression/decompression
- `MemStream` — In-memory binary stream with typed read/write
- `Watcher` — Filesystem change notifications

#### Threading Additions (`Zanna.Threads.*`)

- `Gate` — Semaphore with permit counting
- `Barrier` — N-party synchronization barrier
- `RwLock` — Reader-writer lock with writer preference

#### Crypto Additions (`Zanna.Crypto.*`)

- `KeyDerive` — PBKDF2-SHA256 key derivation
- `Rand` — Cryptographically secure random bytes and integers

#### Text Processing (`Zanna.Text.*`)

- `Pattern` — Regular expression matching, replacement, and splitting
- `Template` — Simple template rendering with placeholder substitution

### New Demos

**Zia:**
- `paint/` — Full-featured paint application with brushes, tools, color palette, and layers
- `vedit/` — Text editor demonstrating the GUI widget library
- `sql/` — Embedded SQL database with REPL
- `telnet/` — Telnet client and server
- `gfx_centipede/` — Graphical Centipede game
- `ladders/` — Platform game demo

**BASIC + Zia:**
- `sqldb/` — SQL database implementation in both languages

### Project Statistics

| Metric              | v0.1.3  | v0.2.0  | Change   |
|---------------------|---------|---------|----------|
| Total Lines (LOC)   | 369,000 | 566,000 | +197,000 |
| Runtime Classes     | 44      | 70      | +26      |
| GUI Library         | —       | 26,000  | New      |
| Audio Library       | —       | 3,000   | New      |
| Zia Source Files    | —       | 130     | New ext  |

### Documentation

- Zia language reference and getting started guide
- "The Zanna Bible" — comprehensive programming book (in progress)
- Network library reference (TCP, UDP, HTTP, DNS, URL)
- Input handling guide (keyboard, mouse, gamepad)
- Graphics additions (sprite, tilemap, camera, color)
- Performance analysis documentation
- CLI redesign documentation

### Architecture

```text
┌──────────────┐  ┌──────────────┐
│ BASIC Source │  │  Zia Source  │
│    (.bas)    │  │    (.zia)    │
└──────┬───────┘  └──────┬───────┘
       │                 │
       ▼                 ▼
┌─────────────────────────────────────────────────────┐
│                     Zanna IL                        │
└─────────────────────────┬───────────────────────────┘
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
    ┌──────────┐    ┌──────────┐    ┌──────────┐
    │    VM    │    │  x86-64  │    │ AArch64  │
    └──────────┘    └──────────┘    └──────────┘
```

### Breaking Changes

1. **ZannaLang renamed to Zia**: Update file extensions from `.zanna` to `.zia`
2. **New CLI tools**: Use `vbasic`, `zia`, `ilrun` instead of `zanna` subcommands

### Migration from v0.1.3

#### Updating ZannaLang Files

Rename your files and update the tool:

```bash
# Old
./build/src/tools/zanna/zanna front zannalang -run program.zanna

# New
mv program.zanna program.zia
./build/src/tools/zia/zia program.zia
```

#### Updating BASIC Invocations

```bash
# Old
./build/src/tools/zanna/zanna front basic -run program.bas

# New
./build/src/tools/vbasic/vbasic program.bas
```

### v0.2.x Roadmap

This release opens the v0.2.x development phase, which will focus on:

- Runtime library expansion and stability
- BASIC and Zia frontend hardening
- GUI library maturation
- macOS native code generation improvements
- Additional test coverage

---

*Zanna Compiler Platform v0.2.0 (Pre-Alpha)*  
*Released: January 2026*  
*Note: This is an early development release. Future milestones will define supported releases when appropriate.*
