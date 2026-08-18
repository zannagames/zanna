---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0159: Add Typed SceneNode Metadata and VSCN v6

## Status

Accepted (2026-07-23)

## Context

`Zanna.Graphics3D.SceneNode` persists hierarchy, transforms, render resources,
camera/light state, and animation, but it has no durable game-facing data.
Consequently, Zanna Studio can construct visible geometry while lacking a
canonical way to author roles, stable gameplay IDs, spawn configuration,
trigger targets, or component parameters. Keeping that information in node
names is ambiguous and string-only. Parsing and patching VSCN inside Studio
would create a second format implementation outside the runtime.

The established 2D scene model supports null, Boolean, integer, floating-point,
and string scalars. The same kinds are sufficient as a bounded foundation for
3D gameplay data, but VSCN v5 has no node-metadata representation. Untagged JSON
would also lose the distinction between integer and integral-looking float
values, and the runtime JSON representation cannot exactly carry every `i64`
as a numeric double.

This change adds public runtime C ABI entry points and a VSCN revision, so ADR
0006 requires an explicit decision.

## Decision

Each native `SceneNode` owns a sorted table of typed metadata entries. A key is
non-empty and at most 128 bytes, a node has at most 256 entries, and a string
value is at most 64 KiB. Floating-point values must be finite. Public mutation
allocates replacement storage before publishing a change, and structurally
invalid native table bounds make public operations fail safely.

`Zanna.Graphics3D.SceneNode` gains these additive instance methods:

```text
MetadataKeys() -> Seq[String]
MetadataKind(key: String) -> String
MetadataHas(key: String) -> Boolean
MetadataGetInt(key: String, default: Integer) -> Integer
MetadataGetFloat(key: String, default: Number) -> Number
MetadataGetBool(key: String, default: Boolean) -> Boolean
MetadataGetString(key: String, default: String) -> String
MetadataSetNull(key: String) -> Boolean
MetadataSetInt(key: String, value: Integer) -> Boolean
MetadataSetFloat(key: String, value: Number) -> Boolean
MetadataSetBool(key: String, value: Boolean) -> Boolean
MetadataSetString(key: String, value: String) -> Boolean
MetadataRemove(key: String) -> Boolean
```

Their C ABI entry points are:

```c
void *rt_scene_node3d_metadata_keys(void *node);
rt_string rt_scene_node3d_metadata_kind(void *node, rt_string key);
int8_t rt_scene_node3d_metadata_has(void *node, rt_string key);
int64_t rt_scene_node3d_metadata_get_int(
    void *node, rt_string key, int64_t def);
double rt_scene_node3d_metadata_get_float(
    void *node, rt_string key, double def);
int8_t rt_scene_node3d_metadata_get_bool(
    void *node, rt_string key, int8_t def);
rt_string rt_scene_node3d_metadata_get_string(
    void *node, rt_string key, rt_string def);
int8_t rt_scene_node3d_metadata_set_null(void *node, rt_string key);
int8_t rt_scene_node3d_metadata_set_int(
    void *node, rt_string key, int64_t value);
int8_t rt_scene_node3d_metadata_set_float(
    void *node, rt_string key, double value);
int8_t rt_scene_node3d_metadata_set_bool(
    void *node, rt_string key, int8_t value);
int8_t rt_scene_node3d_metadata_set_string(
    void *node, rt_string key, rt_string value);
int8_t rt_scene_node3d_metadata_remove(void *node, rt_string key);
```

`MetadataKeys` returns keys in exact bytewise lexicographic order.
`MetadataKind` returns `null`, `bool`, `int`, `float`, or `string`, and returns
an empty string for a missing key. Typed getters return their default for a
missing or incompatible kind; `MetadataGetFloat` additionally promotes an
integer value to `Number`, matching the existing 2D scalar API. Setters and
removal return whether the requested mutation was accepted.

A scene containing at least one node metadata entry serializes as VSCN v6.
Each node may contain an optional, deterministically ordered `metadata` map.
Every value is tagged:

```json
{
  "metadata": {
    "active": {"kind": "bool", "value": true},
    "health": {"kind": "int", "value": "9223372036854775807"},
    "radius": {"kind": "float", "value": 4},
    "role": {"kind": "string", "value": "boss"},
    "target": {"kind": "null"}
  }
}
```

Integer payloads are canonical decimal strings so the complete signed `i64`
range round-trips without JSON-double precision loss. Canonical integer text
uses `0` for zero, has no leading plus sign or leading zeroes, and never uses
`-0`. Float payloads remain finite JSON numbers, so a tagged `4` still reloads
as the float kind. Null has no `value` member. Boolean and string payloads must
match their tag exactly.

The loader accepts VSCN versions 1 through 6. A `metadata` member is valid only
in v6; malformed tags, payload types, bounds, invalid structure, or non-finite
values reject the complete load transaction. Scenes without metadata retain
the previous v2/v3/v5 output-selection rules, and complete asset documents
without metadata remain v5.

`SceneAsset` carries metadata into its immutable template hierarchy and
deep-copies each metadata table during `Instantiate`, `InstantiateScene`, and
`InstantiateSceneAt`. Mutable instances therefore cannot change template
metadata or another instance while meshes, materials, lights, skeletons, and
animation resources retain their established sharing rules.

Zanna Studio exposes the table through a bounded, presentation-only Gameplay
metadata inspector. The controller validates drafts and performs one canonical
VSCN history transaction for one create, rename, type/value update, or remove.
Rejected, duplicate, stale, invalid, and already-equal edits do not add history.
The selected metadata row belongs to the owning scene document and is bounded
when restored from a session.

## Consequences

- A 3D scene can carry durable gameplay configuration independently of display
  names and rendering resources.
- Studio and games share one typed mutation and persistence implementation;
  Studio does not parse or patch VSCN.
- Metadata survives the same `SceneAsset.LoadResult(...).InstantiateScene()`
  path used by Studio and games, with instance-local mutable values.
- Existing runtime programs and VSCN v1-v5 files remain compatible. The API is
  additive, and only metadata-bearing output requires v6-aware consumers.
- Exact integer values and scalar kinds survive save/load, at the cost of a
  tagged representation that is more verbose than untyped JSON.
- Native runtime tests, malformed-format tests, generated API documentation,
  the Graphics3D guide, and Studio display/session probes must cover the new
  surface.
- This supplies raw typed data, not project-defined component schemas.
  Schema validation and reusable component authoring remain a separate layer.

## Alternatives Considered

- **Encode gameplay data in node names.** Rejected because names are
  presentation/search labels, are string-only, and cannot safely represent
  reusable component configuration.
- **Store one untyped JSON object in Studio.** Rejected because it duplicates
  parser, validation, history, and compatibility rules outside the runtime.
- **Use untagged JSON scalars.** Rejected because integer versus float kind
  becomes ambiguous and JSON doubles cannot represent every `i64` exactly.
- **Serialize every integer as a JSON number.** Rejected because values beyond
  the exact double range would silently change during load.
- **Add a fixed gameplay-component struct to SceneNode.** Rejected because
  projects require different roles and parameters; bounded scalar metadata is
  the smaller reusable persistence primitive.
