---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0142: Complete FBX Scene and Animation Import

## Status

Accepted (2026-07-20).

## Context

Zanna's dependency-free FBX reader already parses binary FBX 7100–7700 and
brace-scoped ASCII FBX into the same typed object/property/connection graph. It
extracts mesh geometry, materials, textures, skeletons, skin weights, skeletal
clips, and a hierarchy of non-skeleton `Model` objects. The remaining scene
information was present in that graph but was not published:

- camera and light `NodeAttribute` objects were ignored;
- animation curves were retained only when their target `Model` was a skeleton
  bone;
- blend shapes accepted only double-precision position deltas and ignored
  normal/tangent deltas, channel defaults, progressive full-weight thresholds,
  and animated `DeformPercent` curves; and
- the FBX-to-`SceneAsset` adapter had no camera or node-animation handoff.

These omissions made an apparently successful FBX import materially different
from the authored scene. Completing the path adds runtime C ABI and registry
rows, so repository policy requires an ADR before implementation.

## Decision

### One typed graph and transactional publication

Binary and ASCII FBX continue to use one extractor. Camera, light, object
animation, and morph animation support must not add a text-substring fallback
or an external dependency. Numeric FBX object IDs and connection endpoints are
the only identity keys; display names are never used to resolve a binding.

New retained arrays, camera/light objects, morph payloads, and generated samples
are staged with the existing aggregate FBX load budget. A structurally invalid
typed graph, allocation failure, invalid required relationship, or
generated-sample overflow rejects the load and releases the complete staged
asset. Malformed optional normal/tangent shape arrays are omitted with bounded
warnings, matching the importer's existing missing-texture degradation policy.
No partially populated camera, node-clip, or morph table is published.

### Stable scene-node identity

Each imported non-skeleton `Model` node receives an `import_index` equal to its
zero-based ordinal in the typed `Objects` sequence. Generated material-split
children retain `-1`. `NodeAnimation3D` channels store this index as their
primary target identity and retain the namespace-stripped model name only as a
human-readable fallback. This preserves correct animation targeting when FBX
contains duplicate or long equal-prefix names and remains stable across
`SceneAsset.Instantiate`, VSCN v4 bake, and reload.

### Cameras

A `Model` connected to a `NodeAttribute` of type `Camera` creates one
`Camera3D`. The importer reads perspective/orthographic projection mode,
vertical or horizontal field of view, focal-length fallback, aspect dimensions,
near/far planes, and orthographic zoom from the attribute, with the model as a
property fallback for exporter variants. Projection values pass through the
same finite/range sanitizers as native `Camera3D` construction.

The camera eye and orientation come from the model's evaluated world matrix.
Local `-Z` is forward and local `+Y` is up. A connected
`LookAtProperty`/`UpVectorProperty` model overrides those derived vectors when
present. Z-up conversion uses the same full matrix basis conversion as scene
nodes. FBX has one authored scene in the current container, so every imported
camera belongs to scene zero and is retained in both the FBX and `SceneAsset`
inventories.

### Lights

A `Model` connected to a `NodeAttribute` of type `Light` receives a retained
node-local `Light3D`. Color, intensity (FBX percent converted to a unit scalar),
light type, shadow flag, spot inner/outer angles, and available attenuation
range are imported. Point, directional, and spot types map directly. Area and
volume lights have no native shape/volume representation; they are imported as
point-light approximations and emit a bounded asset warning rather than being
silently discarded. Node traversal transforms the local origin and local `-Z`
direction into world space at draw time, so object animation also moves the
light.

### Object transform animation

For every `AnimationStack`, curve nodes targeting a non-skeleton `Model`'s
`Lcl Translation`, `Lcl Rotation`, or `Lcl Scaling` produce a
`NodeAnimation3D` clip. The clip uses the stack's complete reachable curve time
range and is normalized to start at zero. Object samples use the existing FBX
transform-stack composer, including rotation order, pivots, pre/post rotation,
geometric transform, Z-up conversion, and inheritance mode.

Constant curves retain a sample at the immediately preceding representable FBX
tick. Linear curves retain authored keys. Cubic Hermite curves use the existing
adaptive one-millisecond and mixed absolute/relative error bounds before being
stored as runtime linear/slerp samples. Parent curve times are included whenever
they affect a child's evaluated local pose. Quaternion signs are made
continuous before publication.

One node clip per stack combines its object TRS channels and morph-weight
channels. Skeletal `Animation3D` extraction remains separate, so a stack may
publish both clip kinds and the model's default skeletal/node animators can play
them together.

### Static and animated blend shapes

Blend shapes are traversed by
`BlendShape -> BlendShapeChannel -> Geometry(Shape)` connections. Shape
position, normal, and tangent delta arrays accept both FBX float and double
storage and fan control-point data out through the mesh's triangulation remap.
The resulting `MorphTarget3D` is attached to the source mesh before material
splitting; split meshes clone and remap all morph channels.

Each channel imports `DeformPercent` and its ordered `FullWeights` thresholds.
For thresholds `q[0..n-1]` and channel percent `p`, progressive shape weights
are evaluated as follows:

- below the first threshold, shape 0 receives `p / q[0]`;
- between `q[i-1]` and `q[i]`, shapes `i-1` and `i` receive `1-t` and `t`;
- above the last threshold, the last shape remains fully active with weight `1`;
- all final values use `MorphTarget3D`'s finite `[-1, 1]` weight boundary.

Missing, non-positive, or non-increasing thresholds use deterministic
`100 * (i + 1)` fallbacks and record a warning. The same evaluator initializes
static weights and samples animation curve nodes connected to
`DeformPercent`. A generated weight channel spans the complete shape inventory
of its target mesh and is copied to every scene model that instances that
geometry.

### Public FBX extractor surface

The following additive registry rows expose the newly retained native arrays;
existing names and signatures are unchanged:

| Runtime name | C symbol | Signature |
|---|---|---|
| `Zanna.Graphics3D.Fbx.get_NodeAnimationCount` | `rt_fbx_node_animation_count` | `i64(obj)` |
| `Zanna.Graphics3D.Fbx.GetNodeAnimation` | `rt_fbx_get_node_animation` | `obj<Zanna.Graphics3D.NodeAnimation3D>(obj,i64)` |
| `Zanna.Graphics3D.Fbx.GetNodeAnimationName` | `rt_fbx_get_node_animation_name` | `str(obj,i64)` |
| `Zanna.Graphics3D.Fbx.get_CameraCount` | `rt_fbx_camera_count` | `i64(obj)` |
| `Zanna.Graphics3D.Fbx.GetCamera` | `rt_fbx_get_camera` | `obj<Zanna.Graphics3D.Camera3D>(obj,i64)` |

`SceneAsset` already exposes node-animation and per-scene camera access, so its
public surface does not change. The graphics-disabled runtime provides matching
stubs for all five new C symbols.

## Consequences

- FBX scenes retain authored cameras, punctual lights, object motion, static
  morph state, and morph animation through direct load, `SceneAsset`, and VSCN
  v4 bake/reload.
- Binary and ASCII encodings have identical extraction behavior and remain
  dependency-free on macOS, Windows, and Linux.
- Camera projection-property animation, automatic coupling of an animated FBX
  camera Model back to the standalone immutable `Camera3D`, constraints other
  than look-at/up targets, animation-layer blending modes, area-light shape
  emission, and volumetric lighting remain outside the runtime's representable
  feature set. Camera Model tracks are still retained as ordinary node clips so
  applications can consume the authored path explicitly.
  They are documented explicitly rather than reported as imported fidelity.
- The reviewed Graphics3D ABI manifest changes deliberately for five functions,
  two properties, and three methods.

## Validation

- Typed binary and ASCII fixtures cover perspective and orthographic cameras,
  camera target orientation, point/directional/spot lights, and area-light
  warning fallback.
- Existing shared transform/curve fixtures cover pivot and inheritance
  composition, constant/linear/cubic sampling, negative source time, and Z-up
  conversion; the FBX scene fixture verifies stable-index object animation and
  automatic `SceneAsset` playback.
- The blend-shape fixture covers float/double position arrays, float
  normal/tangent arrays, 32/64-bit indexes, triangulation fan-out, progressive
  static and animated weights (including both thresholds and above-final
  behavior), and material-split mesh clones. Existing aggregate-budget and
  transactional load tests cover rollback.
- Direct `Fbx` accessors, `SceneAsset` inventories, instantiation auto-binding,
  VSCN v4 bake/reload, graphics-disabled linking, generated docs, registry
  signatures, ABI manifest, platform policy, and the official build scripts are
  all included in the final gate.
