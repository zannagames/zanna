---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0244: Classify Offline Model Bake Helpers as Internal C ABI

## Status

Accepted (2026-08-08)

## Context

`zanna asset bake` transforms imported Model3D objects before serialization.
Three C functions implement those offline steps, but the command duplicated
two declarations locally and the Model3D header omitted them. Adding the
missing declarations correctly made the strict runtime-header audit require an
explicit surface classification.

These operations are packaging policy, not language runtime behavior. Adding
them to `runtime.def` would expose mutation intended for offline asset
production to Zia/BASIC programs and would enlarge the public registry without
a gameplay use case.

## Decision

The following signatures are documented in `rt_model3d.h` and remain internal
embedding/tool ABI, deliberately absent from `runtime.def`:

- `int64_t rt_model3d_strip_meshes(void *model)`
- `int64_t rt_model3d_simplify_meshes(void *model, int64_t max_tris)`
- `int64_t rt_model3d_keep_animation_subset(void *model, ...)`

`RuntimeSurfacePolicy.inc` classifies all three as internal symbols. The asset
command includes `rt_model3d.h` for strip/simplify declarations rather than
maintaining duplicate prototypes. This decision changes no IL opcode,
frontend-visible method, serialized scene format, or runtime registry entry.

## Consequences

- Tool and runtime builds share one checked declaration for mesh stripping and
  simplification.
- The strict header audit guards their intentional internal status.
- Promoting any helper to a language API remains a separate registry and ADR
  decision with an explicit managed contract.

## Alternatives Considered

- Keep command-local declarations: rejected because signatures could drift
  without the runtime header or header audit noticing.
- Register public SceneAsset methods: rejected because these destructive
  transforms are offline bake controls, not ordinary runtime operations.
