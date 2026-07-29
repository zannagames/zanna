---
status: active
audience: public
last-verified: 2026-07-29
---

# 2D Animation, Collision, And Camera
> Sprite animation clips, pixel masks, hitboxes, transforms, camera rigs, and graphics aliases for game effects.

**Part of [Graphics](README.md)**

This page covers helpers that are usually attached to game objects or cameras rather than renderer state.

## Classes

| Class | Purpose |
|-------|---------|
| `Viewport2D` | Fixed-point screen scaler with virtual size, screen size, offsets, and world/screen transforms. |
| `Transform2D` | Integer 2D transform with position, percent scale, rotation, origin, and point transforms. |
| `AnimationClip2D` | Frame range, frame delay, and loop metadata for 2D sprite animation. |
| `AnimatedSprite2D` | Runtime clip player that advances a `Sprite` frame from elapsed milliseconds. |
| `CollisionMask2D` | Dense per-pixel solid mask with alpha-threshold construction and mask overlap tests. |
| `Hitbox2D` | Axis-aligned rectangle hitbox with containment and intersection tests. |
| `CameraRig2D` | Follow-target camera controller with smoothing, deadzone forwarding, and render shake offsets. |

## Viewport Scale

- `Viewport2D.Scale` is fixed-point with `1000` representing `1.0x`. For example, `4000` means `4.0x`.
- Integer scaling snaps only scales of `1.0x` and above to whole multiples, so very small screens keep the largest fitting fractional scale instead of overflowing the viewport.
- Viewport APIs validate that their receiver is a `Viewport2D`; invalid handles return safe defaults or no-op instead of reading unrelated graphics objects.
- `Transform2D.Rotation` preserves the integer value that was assigned, but point transforms reduce it modulo 360 before trigonometry. Identity scale with a zero-modulo rotation uses exact saturating integer arithmetic.

## Animation, Collision, And Camera

```zia
var clip = AnimationClip2D.New(0, 4, 80, 1)
var animated = AnimatedSprite2D.New(sprite)
animated.SetClip(clip)
animated.Update(deltaMs)

var mask = CollisionMask2D.FromPixels(playerPixels, 1)
var hurt = Hitbox2D.New(4, 4, 8, 8)

var rig = CameraRig2D.New(camera)
rig.SetTarget(playerX, playerY)
rig.SetSmoothing(50)
rig.Update()
```

`CollisionMask2D.FromPixels` requires a valid `Pixels` object and marks pixels solid when alpha is greater than or equal to the threshold. A threshold of `0` means "any non-zero alpha", so fully transparent pixels remain empty.
`AnimatedSprite2D.New` requires a valid `Sprite`; invalid or null handles return `null` instead of creating a player that would fail during `Update`.
`AnimatedSprite2D` starts stopped until a valid clip is set. `Play()` only starts when a valid sprite and clip are available, and restarts a finished non-looping clip from its first effective frame. `Stop()` stops playback and resets the sprite to the clip's first frame.
`CameraRig2D.New` accepts a `Camera` or `null`, and `SetCamera` ignores invalid non-camera handles. `SetSmoothing` is a percent value clamped to `0..100`; internally it is converted to the lower-level camera smooth-follow scale. Shake offsets and render coordinates use saturating integer arithmetic at the int64 limits.

## Related Game Utilities

Particle emission and 2D lighting are exposed by the live runtime as
`Zanna.Game.ParticleEmitter` and `Zanna.Game.Lighting2D`; they are not separate
`Zanna.Graphics` alias classes.
