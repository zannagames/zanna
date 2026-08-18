---
status: active
audience: public
last-verified: 2026-08-17
---

# Zanna.Game.Entity

`Entity` is a lightweight 2D position, velocity, health, and tile-collision handle. Position and
velocity use centipixels (1/100 pixel); width and height use whole pixels. Its constructor is a
legacy function-namespace entry, so use the fully qualified name.

## Construction and properties

`Zanna.Game.Entity.New(x, y, width, height)` stores `x`/`y` unchanged, clamps non-positive
dimensions to 1 pixel, starts facing right, and starts active. Allocation can return null.

| Property | Access | Notes |
|---|---|---|
| `X`, `Y` | get/set | Centipixel position; setters teleport without collision. |
| `VelocityX`, `VelocityY` | get/set | Centipixel displacement per 16 ms base frame. |
| `Width`, `Height` | get | Pixel collision dimensions. |
| `Dir` | get/set | Setter maps negative values to `-1` and zero/positive values to `1`. |
| `Health`, `MaxHealth` | get/set | Unrestricted signed integers; neither setter clamps the other value. Both initially zero. |
| `Type` | get/set | Uninterpreted application tag. |
| `Active` | get/set | Passive flag only; Entity methods do not skip inactive values automatically. |
| `OnGround`, `HitLeft`, `HitRight`, `HitCeiling` | get | Collision flags recomputed each `MoveAndCollide` call; `OnGround` also reflects a persistent tile-beneath probe (VDOC-241). |

The historical abbreviations `VX`, `VY`, `HP`, and `MaxHP` are not public property names.

## Physics and collision

- `ApplyGravity(gravity, maxFall, dt)` adds `gravity * dt / 16` with truncation and saturation;
  negative `maxFall` becomes zero, and only positive downward velocity is capped.
- `MoveAndCollide(tilemap, dt)` moves X then Y with swept leading-edge tile checks. A null Tilemap
  performs saturated free movement. `dt <= 0` is a no-op and does not reset flags.
- `UpdatePhysics(tilemap, gravity, maxFall, dt)` calls the two operations above.
- `AtEdge(tilemap)` samples two pixels below the leading edge and returns false for a null map.
- `PatrolReverse(speed)` changes direction/velocity only when the last movement set a wall flag.
- `Overlaps(other)` uses half-open pixel AABBs; touching edges do not overlap.

Centipixel-to-pixel collision conversion floors negative coordinates. `MoveAndCollide` resets the
collision flags each call but then probes the tile row directly beneath the entity, so `OnGround`
stays true while resting flush on a solid tile even on a zero-displacement frame (VDOC-241).

## Example

```zia
module EntityExample;

func start() {
    var enemy = Zanna.Game.Entity.New(10000, 5000, 24, 16);
    enemy.set_Health(3);
    enemy.set_VelocityX(100);
    enemy.UpdatePhysics(null, 78, 1350, 16);
    Zanna.Terminal.SayInt(enemy.get_X());
}
```
