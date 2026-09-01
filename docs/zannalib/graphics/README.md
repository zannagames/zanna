---
status: active
audience: public
last-verified: 2026-09-01
---

# Graphics
> 2D graphics, scene management, 3D runtime surfaces, and asset import workflows.

**Part of [Zanna Runtime Library](../README.md)**

When graphics support is present, the Win32, Cocoa, and X11 backends deliver
Unicode code-point text-input events, expose UTF-8 desktop clipboard text, and
clip GUI text and images to their active surfaces. Headless/no-graphics builds
retain the API surface but use unavailable/empty stubs for these host services.

---

## Contents

| File | Contents |
|------|----------|
| [Canvas & Color](canvas.md) | Canvas drawing surface and Color utilities |
| [Fonts](fonts.md) | BitmapFont for custom BDF/PSF font rendering |
| [Images & Sprites](pixels.md) | Pixels, Sprite, SpriteAnimator, SpriteSheet, Tilemap |
| [2D Rendering and Effects](rendering2d.md) | RenderTarget2D, Surface2D, Texture2D, GpuTexture2D, Renderer2D, Material2D, Shader2D, PostProcess2D, Sampler2D, BlendState2D, SpriteRenderer2D, RenderPass2D, RenderGraph2D, Palette2D, Gradient2D, VideoPlayer |
| [2D Tilemaps and Layers](tilemaps2d.md) | TileSet2D, TileLayer2D, ObjectLayer2D, AutoTile2D, TilemapRenderer2D, TileChunkCache2D, TexturePackerAtlas, AsepriteImporter, TiledMapLoader |
| [2D Shapes, Text, and UI](shapes2d.md) | Path2D, ShapeRenderer2D, TextRenderer2D, TextLayout2D, SpriteFont, SdfFont, NineSlice2D, DebugDraw2D |
| [2D Animation, Collision, and Camera](game2d.md) | Viewport2D, ScreenScaler, Transform2D, AnimationClip2D, AnimatedSprite2D, CollisionMask2D, Hitbox2D, CameraRig2D, ParticleSystem2D, Emitter2D, Lighting2D |
| [Scene Graph](scene.md) | SceneNode, SceneGraph, SpriteBatch, TextureAtlas, Camera, SpriteAnimation |
| [Game3D](game3d.md) | `World3D`, `Entity3D`, `LayerMask`, `Input3D`, streaming/perf telemetry, native callback-loop boundaries, and code-first 3D game workflow helpers |
| [3D Physics](physics3d.md) | `PhysicsWorld3D`, `PhysicsHit3D`, `PhysicsHitList3D`, `LedgeHit3D`, `CollisionEvent3D`, `ContactPoint3D`, `Collider3D`, `PhysicsBody3D`, `Character3D`, `Ragdoll3D`, `DistanceJoint3D`, `SpringJoint3D`, `HingeJoint3D`, `RopeJoint3D`, `SixDofJoint3D`, `Cloth3D` |
| [3D Rendering, Animation, and Environment](rendering3d.md) | `SceneGraph`, `SceneNode`, authored LOD/impostors, `Camera3D`, `RenderTarget3D`, `CubeMap3D`, `Material3D`, `TextureAsset3D`, `Light3D`, `LightBaker3D`, `PostFX3D`, `RayHit3D`, `SceneAsset`, `Skeleton3D`, `Animation3D`, `AnimPlayer3D`, `AnimBlend3D`, `BlendTree3D`, `IKSolver3D`, `AnimController3D`, `MorphTarget3D`, `Particles3D`, `Decal3D`, `AssetDiagnostics3D`, `Zanna.Audio.SpatialAudio3D`, `SoundListener3D`, `SoundSource3D`, `NavMesh3D`, `NavAgent3D`, `Terrain3D`, `Water3D`, `Vegetation3D`, `Transform3D`, `Trigger3D`, `Path3D`, `InstanceBatch3D`, `Sprite3D`, `TextureAtlas3D` |
| [Graphics 3D Guide](../../graphics3d-guide.md) | `SceneAsset`, `AnimController3D`, `Canvas3D`, `SceneGraph`, `SceneNode`, `SceneGraph.SyncBindings`, `Mesh3D`, `Material3D`, `Fbx`, `Gltf`, and the higher-level 3D asset pipeline |

## See Also

- [Input](../input.md) - `Keyboard`, `Mouse`, and `Pad` for interactive input with Canvas
- [Audio](../audio.md) - Sound effects and music for games
- [Mathematics](../math.md) - `Vec2` and `Vec3` for 2D/3D graphics calculations
- [Collections](../collections/README.md) - `Bytes` for pixel data serialization
- [Graphics 3D Guide](../../graphics3d-guide.md) - higher-level rendering, materials, meshes, and scenes
