# VSCN Scene Format (`.scene3d` / `.vscn`)

VSCN is Zanna's canonical 3D scene serialization: a UTF-8 JSON document
with `"format": "vscn"` and an integer `"version"`. `.scene3d` is the
canonical extension (ADR 0182); `.vscn` remains recognized. The
implementation of record is `src/runtime/graphics/3d/scene/`
(`rt_scene3d_vscn_save.c`, `rt_scene3d_vscn_load.c`,
`rt_scene3d_vscn_internal.h`); this page summarizes the contract those
files enforce.

## Versions

The loader accepts versions 1 through 7. The writer always emits the
**lowest** version that can carry the document's content:

| Version | Adds | Written |
| --- | --- | --- |
| 1 | Original node/mesh/material document | No (readable only) |
| 2 | Baseline for current writer output | Yes |
| 3 | `skeletons` and `animations` sections (rigged content) | Yes |
| 4 | Asset documents: non-empty `scenes` array distinguishes an asset view (`SceneAsset`) from a scene | No for scene documents (readable; asset views serialize through the model-aware saver) |
| 5 | `cameras` section, extended light fields, preserved source texture containers (ADR 0146) | Yes |
| 6 | Typed SceneNode metadata (ADR 0159) | Yes |
| 7 | Prefab reference nodes (ADR 0187) | Yes |

Consequence of lowest-version writing: a v1 or v4 scene file re-saved by
the current runtime changes its version number (content is preserved).
Version selection is content-driven only — there is no way to pin a
version.

## Document layout

Scene documents serialize sections in a fixed order:

`metadata` (root node's typed metadata, v6+) → `textures` → `cubemaps` →
`materials` → `skeletons` (rigged only) → `animations` (rigged only) →
`cameras` (v5+) → `meshes` → `nodes`.

Asset documents additionally carry `nodeAnimations`, `variantNames`, and
`scenes` in place of `nodes`. Node-animation tracks are serialized only
in asset documents.

Meshes embed vertex/index data as base64; textures embed either decoded
pixels or, from v5, the preserved source container bytes
(`{"kind": "source", "sourceBase64": ...}`). There are no external mesh
or texture references; the only reference node kind is `prefab` (v7).

## Node fields

Ordinary nodes write: `name`, `position` (xyz), `rotation` (normalized
quaternion xyzw), `scale`, `visible`, `isStatic`, `syncMode`, `hasMesh`,
`hasMaterial`, `mesh`/`material` (indices into the shared arrays),
`camera` (v5+), `light` (type, direction, position, color, intensity,
attenuation, inner/outer cosine, castsShadows; v5 adds enabled, basis,
area dimensions, radius, range, decay), `lod`/`autoLOD`, and `metadata`
(v6+, typed entries: null/bool/int/float/string).

Prefab reference nodes (v7) write **only** identity plus the reference:
`name`, `position`, `rotation`, `scale`, `visible`,
`prefab` (portable relative path), and `metadata`. Grafted content is
never serialized into the referencing file and round-trips byte-stable.

## Limits and sanitization

From `rt_scene3d_vscn_internal.h` and the loader:

- Maximum file size 256 MiB; maximum node depth 98; absolute numeric
  magnitudes clamp at 1e12.
- Typed metadata: 256 entries per node, 128-byte keys, 64 KiB string
  values.
- Light `type` clamps to the version's supported range and falls back to
  a point light when out of range.
- Prefab grafting: maximum nesting depth 8, shared per-root instance
  budget 4096, cycle detection over canonical paths.

## Prefab resolution diagnostics (ADR 0227)

Prefab resolution failures are non-fatal by design (ADR 0187): the node
loads as an empty placeholder that retains its reference. Since ADR 0227
each unresolved reference also:

- appends one bounded warning to the asset-error channel (readable
  through `Zanna.Graphics3D.AssetDiagnostics3D`) naming the node, the
  path, and the reason (`missing or unloadable source`,
  `reference cycle`, `nesting depth limit`, `instance budget exhausted`,
  `invalid path`), and
- increments the loaded scene's `SceneGraph.UnresolvedPrefabCount`,
  including occurrences nested inside successfully resolved prefabs, so
  a game can gate startup on `UnresolvedPrefabCount == 0`.

## Loading APIs

- `SceneGraph.Load(path)` — nullable result, no diagnostics.
- `SceneGraph.LoadResult(path)` / `SceneGraph.LoadTextResult(virtualPath,
  text)` — `Zanna.Result` carriers (ADR 0227); `LoadTextResult` is the
  inverse of `SceneGraph.SaveToText()` (ADR 0190) and resolves relative
  prefab references against `virtualPath`'s directory.
- `SceneAsset.Load*` — asset-document loaders including six `*Result`
  variants and `LoadTextResult`.

## What VSCN does not persist

Terrain attachments, water, sky, fog, IBL/environment, post-FX,
particles, decals, navmeshes, colliders, and physics bodies have no
typed sections; tooling conventions store them as typed metadata keys
(`terrain.*`, `env.*`, `bake.*`, `collider.*`, `nav.*` — see the Studio
workflows manual). Navmeshes and light-probe grids serialize to
`.vnavmsh` / `.vlpg` sidecar files. Promoting these conventions to typed
sections is tracked as future scene-format work.

## Fuzzing and tests

`src/tests/fuzz/fuzz_vscn_loader.cpp` fuzzes the loader (corpus under
`src/tests/fuzz/corpus/vscn_loader/`, seeded with v5 camera/light, v6
metadata, deep-hierarchy, and prefab-cycle documents).
`src/tests/unit/test_rt_scene3d.cpp` carries the round-trip, metadata,
and prefab suites in the default test gate (only its 10k spatial-index
scaling fixture stays behind the `slow` label as
`test_rt_scene3d_scale`), including a checked-in v7 fixture
(`src/tests/fixtures/runtime/prefab_world_v7.scene3d`). Byte-exact
serialization goldens for v2/v5/v6/v7 documents live under
`src/tests/golden/vscn/` and are enforced by the default-gate
`g3d_vscn_golden` test, which also proves reload byte-stability.
