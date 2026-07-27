---
status: active
audience: contributors
last-verified: 2026-07-02
---

# ADR 0046: Game3D Prefab Loading API

## Status

Accepted

## Context

`Zanna.Game3D.Assets3D.LoadTemplate*` loads reusable cached model instances
that callers later instantiate into `Entity3D` objects. The behavior is useful,
but the word "template" is too generic in the public Game3D lifecycle. Runtime
users need the object flow to read as asset -> prefab -> entity -> world.

`Zanna.Game3D.Prefab` already owns primitive prefab factories such as `Box` and
`Sphere`, so it is the natural namespace for loading reusable prefab sources as
well.

## Decision

Expose canonical prefab-loading names:

- `Zanna.Game3D.Prefab.Load(path)`
- `Zanna.Game3D.Prefab.LoadAsset(assetPath)`
- `Zanna.Game3D.Prefab.LoadAsync(path)`
- `Zanna.Game3D.Prefab.LoadAssetAsync(assetPath)`
- `Zanna.Game3D.AssetHandle3D.GetPrefab()`

For users who prefer staying inside the asset loader namespace, also expose
readable `Assets3D.LoadPrefab*` aliases over the same C ABI functions.

The existing `Assets3D.LoadTemplate*` and `AssetHandle3D.GetTemplate` rows
remain available for compatibility and carry migration targets in the runtime
API dump.

## Consequences

- Public Game3D documentation can describe loaded reusable scene content as a
  prefab without removing the existing `SceneTemplate` runtime representation.
- Existing code that calls `LoadTemplate*` or `GetTemplate` continues to work.
- Runtime API dumps can guide tools, examples, and demos toward the canonical
  prefab names while preserving the full asset-loading feature set.
