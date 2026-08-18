---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0214: Render Project-Owned 3D Material Previews

## Status

Accepted (2026-07-28)

## Context

ADRs 0198 through 0204 let a project describe its gameplay camera, sky,
lighting, fog, environment, and post-processing without executing project code
inside Studio. ADRs 0212 and 0213 extend that composition to runtime-built
environment pieces and procedural water.

That still leaves a common scene-versus-game gap. Ashfall's canonical VSCN
stores portable tint materials plus typed `surf` metadata. At runtime,
`TextureLib.mat` interprets that metadata and adds generated albedo and normal
maps, surface-specific PBR values, environment reflection, SSR intent, and
level-colored emissive state. Studio previously rendered the portable
placeholder. Geometry, camera, and atmosphere could all be correct while the
mission still looked like a flat blockout rather than the game.

Serializing every derived runtime material into the scene would duplicate the
game's material source of truth. Running arbitrary project material factories
inside Studio would be unbounded and unsafe. Teaching Studio Ashfall's `surf`
key or importing its module would make the product project-specific.

Studio needs a bounded, declarative bridge from canonical typed metadata to
render-only material state. The bridge must preserve scene bytes, material
objects, selection, dirty state, and history, and it must use only the existing
portable runtime surface.

## Decision

### Component-schema version 16

`scene-components.json` version 16 adds an optional top-level
`materialPreviews3D` array of at most 128 ordered rules. Each rule requires:

- a portable `matchProperty`; and
- an exact Boolean, integer, or string `matchValue`.

Studio compares both metadata kind and value. A missing, wrong-typed, or
unequal value leaves the rule inactive. The first matching rule in declaration
order owns a node. Duplicate property/kind/value match identities reject the
complete schema.

Each rule may override any of these material inputs:

- safe project-relative `albedoMap`, `normalMap`,
  `metallicRoughnessMap`, `ambientOcclusionMap`, and `emissiveMap` paths;
- `metallic`, `roughness`, and `ambientOcclusion` in `[0, 1]`;
- `normalScale` in `[0, 8]`;
- one complete fixed `emissiveR/G/B` group in `[0, 1]`;
- one complete `emissiveRProperty/GProperty/BProperty` group, resolved from
  exact float metadata on the canonical scene root;
- an optional mapped-color `emissiveMultiplier` in `[0, 64]`;
- `emissiveIntensity` in `[0, 64]`;
- Boolean `useProjectEnvironment`; and
- Boolean `ssrEnabled`.

Fixed and root-mapped emissive colors are mutually exclusive. A multiplier is
valid only with a complete mapped group. Every rule must contain at least one
material override.

Texture references are limited to 1,024 characters and the existing portable
PNG, JPEG, BMP, GIF, or strict KTX2 formats. Absolute paths, backslashes,
drive/URI separators, and parent traversal reject the complete schema.
Runtime loading retains the material inspector's bounded source and decode
contract; one configured texture that is missing, oversized, or undecodable
omits that node's complete overlay and reports truthful preview status.

Versions 1–15 that contain `materialPreviews3D`, fractional numeric matches,
partial groups, nonportable paths, out-of-range values, empty override rules,
or duplicate match identities reject atomically.

### Clone, submit, and restore

For each matched node, Studio clones the canonical `Material3D` and applies
only the fields named by the rule. The canonical albedo tint and every
unspecified imported/material field survive. A node without a canonical
material starts from a white PBR material.

Configured maps and root emissive properties resolve completely before the
clone publishes. `useProjectEnvironment` attaches the current project sky
cubemap after that environment has been rebuilt. No rule may partially publish
when one of its required resources is unavailable.

Immediately before one shaded `Canvas3D` submission, Studio records every
matched node's canonical material reference and temporarily substitutes the
preview clone. It submits the canonical scene, project prefabs, and procedural
surfaces in the explicit shared frame established by ADR 0213, then restores
every recorded reference immediately after `Canvas3D.End`. Pure Wireframe
never applies material overlays; Shaded and Shaded+Wire do.

The preview clones and texture cache belong only to the active editor. They do
not enter the canonical `SceneGraph`, hierarchy, picking, selection, VSCN
serialization, revision, dirty state, or undo/redo history. Accepted scene
edits rebuild cheap clones from current canonical materials and metadata while
retaining decoded project textures. Loading another scene or schema resets the
cache.

### Ashfall

Ashfall declares one version-16 rule for each `surf` value 0 through 9. Its
in-tree preview generator runs the production `TextureLib` and writes the
exact 256-pixel albedo and normal images for every mapped surface. The rules
copy the runtime roughness, metallic, anti-crush emissive floor, level accent
mapping, environment-reflection, and SSR values. Pure accent surfaces remain
textureless exactly as they do in the game.

The mission scene remains the source of geometry, tint, and typed surface
classification. The generated images and schema remain the source of
presentation only; neither changes runtime loading or canonical scene bytes.

## Consequences

- A metadata-driven game can make Studio's shaded scene use the same material
  vocabulary as runtime without linking or executing project code.
- Canonical placeholder materials remain portable and byte-stable while the
  editor gains production albedo, normals, PBR, emissive, and environment
  presentation.
- Missing project resources fail visibly and per node rather than publishing a
  misleading partial material.
- Version 16 is cross-platform, bounded, zero-dependency, and additive.
  Versions 1–15 and projects without rules retain their behavior.
- The decision adds no runtime C ABI, IL, VSCN, workflow, dependency, or
  platform-adapter change. It consumes existing `Material3D`, texture, cubemap,
  and synchronous `Canvas3D` APIs.
- Probes must cover version gates, typed match identity, path and numeric
  bounds, complete emissive groups, structured-authoring preservation, exact
  Ashfall runtime inputs, transient pointer restoration, and canonical
  content/history isolation.

## Alternatives Considered

- **Bake the derived materials into every VSCN.** Rejected because it
  duplicates runtime-owned presentation and drifts whenever the material
  factory changes.
- **Execute a project material callback in Studio.** Rejected because arbitrary
  project code is not a bounded, deterministic authoring contract.
- **Teach Studio Ashfall's `surf` metadata.** Rejected because metadata names
  and surface taxonomies belong to projects, not the product.
- **Replace canonical materials when a scene opens.** Rejected because a
  preview must not dirty, serialize, or alter the user's authored objects.
- **Apply whichever matching rule appears last.** Rejected because ordered
  first-match behavior is stable, cheap, and consistent with existing preview
  selection rules.
