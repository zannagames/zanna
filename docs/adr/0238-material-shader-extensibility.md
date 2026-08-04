---
status: draft
audience: contributors
last-verified: 2026-08-03
---

# ADR 0238: Material and Shader Extensibility Direction

## Status

Proposed (decision recorded; implementation is its own program)

## Context

Zanna ships six fixed shading models with all shaders compiled in as
strings, per backend. Games cannot express a look the fixed set doesn't
cover — the defining product gap against Unity/Godot. Any extensibility
design must hold Zanna's constraints: zero external dependencies (no
SPIR-V-Cross, no DXC), 100% cross-platform (Metal/D3D11/OpenGL *and* the
deterministic software rasterizer), and VM/native determinism.

## Decision

Adopt a **parameterized über-shader ("surface graph lite")** as the
extensibility mechanism, explicitly rejecting user-authored shader source:

1. **Material programs are data, not code.** A material program is a small
   validated expression tree over a fixed node vocabulary (texture sample,
   UV transform, scalar/vector math, lerp/step/fresnel, vertex data, time)
   whose outputs feed the existing PBR/legacy lighting terms (albedo,
   normal, emissive, metallic/roughness, alpha, UV offset).
2. **One compiler per backend, in-tree.** Each GPU backend lowers the tree
   into its native shader source through the same string templates the six
   fixed models already use; the software backend interprets the tree
   per-fragment (bounded depth, fixed vocabulary — deterministic by
   construction). No runtime shader-compiler dependency is added anywhere.
3. **The six fixed models become presets** of the same vocabulary,
   guaranteeing the vocabulary is sufficient for everything shipped today
   and giving the compiler a permanent conformance suite.
4. **Serialization:** material programs serialize into VSCN materials
   (v8+) and a standalone `.vmat` for sharing; Studio authors them through
   a form-based stack (texture slots + modifier list) first, node canvas
   later.

Custom vertex deformation, compute, and user HLSL/MSL/GLSL remain out of
scope permanently under the zero-dependency constraint — the honest
boundary is "any look expressible in the vocabulary", and the vocabulary
grows by ADR.

## Why not user shader source

Accepting shader text means either shipping per-backend compilers (a
dependency or a multi-year in-house project), abandoning the software
backend's determinism, or forking the material model per backend. Every
engine that accepted arbitrary shader source paid with a permanent
compatibility matrix; a bounded vocabulary keeps every material valid on
every backend forever.

## Consequences

- Implementation phases: vocabulary + software interpreter + conformance
  presets; Metal lowering; D3D11/GL lowering; VSCN serialization; Studio
  form UI. Each is independently landable behind the preset-equivalence
  test.
- `BackendSupports("material-programs")` gates availability during
  rollout.
