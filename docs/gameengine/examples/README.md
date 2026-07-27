---
status: active
audience: public
last-verified: 2026-07-26
---

# Example Games Gallery

> 15 complete games demonstrating the Zanna Game Engine at every scale.

**Part of [Zanna Game Engine](../README.md)**

---

All examples are in [`examples/games/`](../../../examples/games/). Each game is a complete, runnable project.

---

## By Complexity

### Advanced — Full Engine Showcase

#### XENOSCAPE

**Genre:** Metroidvania sidescroller | **Language:** Zia | **LOC:** 17,005 across 26 files

The flagship game demo showcasing the full Zanna game engine. Features 25+ enemy types, 10 JSON-based levels, parallax scrolling, particle effects, lighting, abilities (dash, charge, ground pound), world map, save system, achievement tracking, and 11 WAV sound files.

**Engine features demonstrated:**
Canvas, Entity, Behavior, AnimStateMachine, PlatformerController, Camera (smooth follow + parallax), Tilemap, SpriteBatch, SoundBank + Synth, ScreenFX (shake, fade, transitions), ParticleEmitter, Lighting2D, StateMachine, SceneManager, LevelDocument, Config, GameUI, Dialogue, Timer, ObjectPool, AchievementTracker, DebugOverlay

**Source:** [`examples/games/xenoscape/`](../../../examples/games/xenoscape/)

---

### Intermediate — Focused Subsystems

#### Chess

**Genre:** Board game with AI | **Language:** Zia | **LOC:** 4,000+ across 15+ files

Chess with full move validation, check/checkmate detection, save slots, clocks, built-in lessons, settings, achievements, and an alpha-beta AI opponent. Demonstrates Canvas rendering for board games, turn-based game logic, persistence, audio, and StateMachine-driven game flow.

**Engine features:** Canvas, Input.Action, StateMachine, SaveData, AchievementTracker, SoundBank

**Source:** [`examples/games/chess/`](../../../examples/games/chess/)

---

#### Centipede

**Genre:** Arcade shooter | **Language:** Zia | **LOC:** 2,553 across 10 files

Centipede clone with 6 enemy types (centipede segments, spider, flea, scorpion), mushroom field, grid-based movement, and particle explosions.

**Engine features:** Canvas, Grid2D, Timer, SmoothValue, ScreenFX, ParticleEmitter, StateMachine, Input.Action

**Source:** [`examples/games/centipede/`](../../../examples/games/centipede/)

---

#### Crackman

**Genre:** Arcade | **Language:** Zia | **LOC:** 2,230 across 9 files

Crackman maze chase with smart ghost AI (scatter/chase/frightened modes), dot collection, power pellets, fruit bonuses, and multiple game modes.

**Engine features:** Canvas, StateMachine, ButtonGroup, Action, Grid2D, Timer, Tween, SmoothValue, ScreenFX, ParticleEmitter

**Source:** [`examples/games/crackman/`](../../../examples/games/crackman/)

---

#### Frogger

**Genre:** Arcade | **Language:** Zia | **LOC:** ~1,500

Classic Frogger with traffic lanes, grid-based movement, and AI-controlled cars.

**Engine features:** Canvas, Entity, Grid-based collision

**Source:** [`examples/games/frogger/`](../../../examples/games/frogger/)

---

### Graphics Demos

#### Graphics Show

**Genre:** Visual demo collection | **Language:** Zia | **LOC:** 8,000+ across 10+ files

A menu-driven collection of 10 visual demos: starfield with parallax, Matrix rain, plasma effect, particle systems, bouncing physics, Snake game, fireworks, Sierpinski fractal, color palette showcase.

**Engine features:** Canvas (all primitives), Randomization, Math, ParticleEmitter, Physics, Timer

**Source:** [`examples/games/graphics-show/`](../../../examples/games/graphics-show/)

---

#### Fade Test

**Genre:** Test harness | **Language:** Zia | **LOC:** 168

Minimal test for ScreenFX fade and full-screen canvas coverage. Useful as a reference for how to apply screen overlays.

**Engine features:** Canvas, ScreenFX, Input.Action

**Source:** [`examples/games/fade-test/`](../../../examples/games/fade-test/)

---

### BASIC Games — Terminal-Based

These games use Zanna BASIC and render with ANSI terminal graphics (no Canvas). They demonstrate OOP patterns and game logic without the graphics engine.

| Game | LOC | Files | Description |
|------|-----|-------|-------------|
| [VTris](../../../examples/games/vtris/) | 1,132 | 4 | Tetris with full rules, line clearing, high scores. Demonstrates 2D arrays, matrix rotation, class composition. |
| [Frogger BASIC](../../../examples/games/frogger-basic/) | 1,320 | 4 | Frogger clone. Stress test for object lifetime and nested references (Frog contains Position). |
| [Pac-Man BASIC](../../../examples/games/pacman-basic/) | 450 | 5 | Pac-Man-style maze chase with ghost pathfinding and ANSI color maze rendering. |
| [Centipede BASIC](../../../examples/games/centipede-basic/) | 450 | 5 | Centipede clone with class-based entity architecture. |

---

## Engine Feature Coverage Matrix

Which example games demonstrate which engine systems:

| Feature | XENOSCAPE | Centipede | Crackman | Chess | Graphics Show |
|---------|:---------:|:---------:|:-------:|:-----:|:------------:|
| Canvas | x | x | x | x | x |
| Entity/Behavior | x | | | | |
| AnimStateMachine | x | | | | |
| PlatformerController | x | | | | |
| Tilemap | x | | | | |
| Camera | x | | | | |
| Physics2D | x | | | | x |
| StateMachine | x | x | x | x | |
| ScreenFX | x | x | x | | |
| ParticleEmitter | x | x | x | | x |
| Audio/SoundBank | x | | | | |
| Synth | x | | | | |
| Input.Action | x | x | x | x | |
| Grid2D | | x | x | | |
| Timer | x | x | x | | x |
| Tween | | | x | | |
| SmoothValue | x | x | x | | |
| ButtonGroup | | | x | | |
| GameUI | x | | | | |
| SceneManager | x | | | | |
| LevelDocument | x | | | | |
| Config | x | x | x | x | |
| Lighting2D | x | | | | | |
| Save System | x | | | | | |
| Achievements | x | | | | | |

---

## Tutorials

Two guided walkthroughs are planned but are not yet part of the documentation set:

- **Your First Platformer** — Entity + Tilemap + Camera
- **Arcade Shooter** — Centipede-style ObjectPool + Particles

Until those are written, use the linked complete examples above and the
[game-library guides](../../zannalib/game/README.md).
---

## See Also

- [Getting Started](../getting-started.md) — Your first game in 15 minutes
- [Game Engine Overview](../README.md) — Full engine documentation hub
