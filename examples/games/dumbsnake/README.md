# Dumb Snake

A deliberately compact Snake clone written in Zia. Eat the pink food, grow,
and avoid the walls and your own body.

## Run

```sh
zanna run examples/games/dumbsnake
```

Controls:

- Arrow keys or WASD: steer
- P or Space: pause/resume
- R: restart
- Escape: quit

The renderer uses only Zanna's built-in Canvas API and the project has no
external assets or dependencies.

## Headless smoke probe

```sh
zanna run examples/games/dumbsnake/smoke_probe.zia
```

The probe verifies movement, turn buffering, growth, scoring, food placement,
and wall collision without opening a window.
