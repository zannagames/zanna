---
status: accepted
audience: contributors
last-verified: 2026-09-17
---

# ADR 0370: Custom Shaders as Raw Per-Backend Source (`Shader3D`)

## Status

Accepted. Supersedes the "no user shader source" clause of
[ADR 0238](0238-material-shader-extensibility.md); the parameterized
über-shader direction recorded there remains a valid future layer on top of
this one, but it is no longer the only extensibility mechanism. Phase 1 (this
change) adds the `.zshader` format, the `Zanna.Graphics3D.Shader3D` runtime
object and the `Material3D` binding API. GPU backend compilation lands in the
later phases listed below.

## Context

Zanna ships six fixed shading models. A game cannot express any look outside
that set, and ADR 0238 rejected user-authored shader source on the grounds that
it would need a per-backend compiler dependency or a multi-year in-house
compiler. That premise no longer holds: every GPU backend already compiles its
own shading language at runtime with no added dependency (Metal through
`newLibraryWithSource:`, D3D11 through `D3DCompile` from the system
`d3dcompiler_47.dll` per ADR 0360, OpenGL through `glCompileShader`). The only
backend that cannot run user text is the deterministic software rasterizer.

The owner chose raw per-backend source over an engine-owned node vocabulary:
users who want a custom look write MSL, HLSL and GLSL themselves, and the engine
guarantees the shared contract (parameters, textures, inputs, outputs) around
that text.

## Decision

### File format `.zshader`

One text file per shader: an engine-parsed header, then raw sections.

```
zshader 1
name Hologram
mode surface              # surface | full
blend alpha               # opaque | alpha | additive | material (default)
cull back                 # back | front | none | material (default)
param float  scanSpeed 2.0
param float3 tint 0.2 0.8 1.0
texture noiseTex          # up to 4 user textures
[metal]
...raw MSL...
[hlsl]
...raw HLSL...
[glsl]
...raw GLSL 330...
```

- Header lines are `directive words...`; `#` starts a comment; the first
  non-blank line must be `zshader 1`.
- `param <type> <name> [defaults]` with types `float`, `float2`, `float3`,
  `float4`, `int`; at most 32 params, at most 64 float lanes, at most 4
  textures. Names are identifiers (`[A-Za-z_][A-Za-z0-9_]*`, up to 63 bytes).
- Params pack into a 64-float block in declaration order with 16-byte block
  rules: `float`/`int` take one lane, `float2` two (2-lane aligned), `float3`
  and `float4` take a fresh 4-lane block. The engine generates the per-backend
  declaration (`struct ZsParams { float scanSpeed; float3 tint; ... }` in MSL,
  `cbuffer ZsParams` in HLSL, a `uniform` block in GLSL) so user code refers to
  `p.scanSpeed` on every backend with identical layout.
- Sections are raw text passed to the backend compiler after the engine's
  prelude; a shader may carry any subset of the three.

### Modes

- `surface`: the user supplies `zs_surface(...)`, which the engine splices into
  the built-in fragment stage before lighting. Lighting, shadows, IBL, TAA,
  motion vectors, decals and clustered lights are untouched.
- `full`: the user supplies complete `zs_vertex` and/or `zs_fragment` entry
  points against the documented interface; shadow casting uses the built-in
  shadow vertex shader unless `zs_shadow_vertex` is also defined.

### Runtime API

`Zanna.Graphics3D.Shader3D`: `Load(path)`, `LoadAsset(assetPath)`,
`FromSource(text)`, `Reload()`, properties `Name`, `Mode` (0 surface, 1 full),
`Status` (0 pending, 1 ready, 2 failed, 3 unsupported backend), `IsReady`,
`Error`, `ParamCount`, `TextureCount`; methods `ParamName(i)`, `ParamType(i)`,
`TextureName(i)`, `HasBackend(name)` for `"metal"`, `"hlsl"`, `"glsl"`.

`Zanna.Graphics3D.Material3D`: `SetShader(shader)`, `Shader` (borrowed),
`ClearShader()`, `SetShaderParam(name, x)`, `SetShaderParam2/3/4(name, ...)`,
`SetShaderInt(name, i)`, `ShaderParam(name)`, `SetShaderTexture(name, source)`
with `Pixels`, `TextureAsset3D` or `RenderTarget3D`. Binding a shader resets the
material's parameter block to the declared defaults; `Clone` copies the shader,
the block and the texture slots.

Errors (exact text): a load or parse failure never traps; it is recorded in
`Status` (2) and `Error` as one of `Shader3D.Load: cannot read '<path>'`,
`Shader3D: missing 'zshader 1' header`, `Shader3D: line <n>: unknown directive
'<word>'`, `Shader3D: param '<name>' redeclared`, `Shader3D: more than 4
textures`, `Shader3D: params exceed 64 floats`. Material setters trap:
`Material3D.SetShader: shader must be a Shader3D`, `Material3D.<Setter>:
material has no shader`, `Material3D.<Setter>: unknown param '<name>'`,
`Material3D.<Setter>: param '<name>' is <type>` (component count or int
mismatch), `Material3D.SetShaderTexture: unknown texture '<name>'`.

### Honest limits (recorded, not hidden)

- The software rasterizer cannot run user source. A material whose shader has
  no section for the active backend, or whose section failed to compile, renders
  its built-in shading model. `Canvas3D.BackendSupports("custom-shaders")` is
  true only on Metal, D3D11 and OpenGL once those phases land; in phase 1 every
  backend reports it false and every parsed shader reports status 3.
- VM/native determinism is unchanged (same backend, same source, same output).
  GPU-versus-software parity is explicitly not promised for custom shaders.
- A draw never waits on a compile: compilation runs at load time off the frame,
  and the material uses the fallback until `IsReady`.

## Phases

1. Format, parser, packing, `Shader3D` object, `Material3D` binding, registry
   entries, docs, unit tests (this change).
2. Metal surface mode: über-shader split into surface and lighting functions,
   async library and pipeline build, draw-path selection, parameter and texture
   binding at fixed slots after the engine's, GPU-gated render test.
3. Metal full mode and shadow vertex hook; D3D11 surface and full (worker
   compile, DXBC bake keyed by source digest for shipped packs); OpenGL surface
   and full with the program-binary cache.
4. Assets and tooling: VSCN material `shader` reference, pack support,
   `zanna shader check`, Studio material panel with hot reload.
5. First shipped users in Legacy Baseball (jumbotron, field wear).

## Consequences

- One new runtime class, twenty-five runtime functions, ten material methods
  and one material property; the graphics manifest hash, the Baseball
  generated-inventory pins and the graphics stub count are re-pinned in the same
  change. `rt_material3d` grows by the shader slot, a 64-double block and four
  texture slots; the test-side layout mirrors follow.
- No IL, verifier or serialization change in phase 1. The VSCN material section
  gains a shader reference in phase 4 under the ADR 0295 wire rules.
- No new dependency on any platform: each backend keeps compiling its own
  language with the compiler it already uses for the built-in shaders.
