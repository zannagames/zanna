---
status: active
audience: public
last-verified: 2026-09-13
---

# Zanna.Game.Behavior

`Behavior` is a composable controller for the 2D `Zanna.Game.Entity` and `Tilemap` APIs. Add one
or more presets, then call `Update` with a delta in milliseconds.

## API

| Member | Behavior |
|---|---|
| `New()` | Creates an empty behavior bundle; allocation can return null. |
| `AddPatrol(speed)` | Sets horizontal velocity from the Entity's facing direction. `speed` is centipixels per 16 ms base frame. |
| `AddChase(speed, range)` | Overrides horizontal velocity while a non-coincident target is within `range` pixels. Target coordinates are pixels. |
| `AddGravity(gravity, maxFall)` | Adds `gravity * dt / 16` to vertical velocity and clamps positive fall velocity to non-negative `maxFall`. |
| `AddEdgeReverse()` | Turns a grounded Entity around when the leading-edge probe has no solid tile below it. |
| `AddWallReverse()` | Responds to the wall flags produced by the current update's collision pass. |
| `AddShoot(cooldownMs)` | Starts a timer clamped to at least 1 ms. It does not fire a projectile; it raises `ShootReady`. |
| `AddSineFloat(amplitude, speed)` | Writes a sine value to vertical velocity. Despite the historical names, `amplitude` is a velocity magnitude and `speed` advances centidegrees by `speed * dt / 16` (VDOC-240). |
| `AddAnimLoop(frameCount, msPerFrame)` | Enables a looping integer frame index; both arguments clamp to at least 1. |
| `Update(entity, tilemap, targetX, targetY, dt)` | Applies the enabled behaviors. `dt <= 0` or a null Entity is a no-op; a null Tilemap permits free movement. |
| `ShootReady` | Consuming boolean property: a true read clears the flag. Large updates coalesce missed cooldowns into one event. |
| `AnimFrame` | Current zero-based loop frame. |

The update order is gravity, patrol, chase, sine velocity, movement/collision, wall reversal, edge
reversal, shoot cooldown, then animation. Patrol and chase speeds should normally be non-negative.
`Entity.OnGround` persists across stationary frames — `MoveAndCollide` probes the tile beneath the
entity even with zero vertical displacement — so edge reversal reads a stable grounded contact
(VDOC-241).

## Example

```zia
module BehaviorExample;

func start() {
    var behavior = Zanna.Game.Behavior.New();
    behavior.AddPatrol(100);
    behavior.AddGravity(78, 1350);
    behavior.AddAnimLoop(4, 120);

    var enemy = Zanna.Game.Entity.New(10000, 5000, 24, 16);
    behavior.Update(enemy, null, 0, 0, 16);
    Zanna.Terminal.SayInt(behavior.AnimFrame);
}
```
