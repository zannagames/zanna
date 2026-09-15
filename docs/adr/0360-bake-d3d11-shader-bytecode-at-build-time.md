---
status: active
audience: contributors
last-verified: 2026-09-14
---

# ADR 0360: Bake D3D11 Shader Bytecode at Build Time

## Status

Accepted (2026-09-14)

## Context

The Direct3D 11 backend compiled its 16 HLSL entry points with `D3DCompile` in
every process. Its shader cache lives only as long as the process, and D3D11 has
no system shader cache like Metal's, so every Zanna 3D program on Windows showed
a black window for 7-9 seconds before its first frame. On a Ryzen 9 7940HS a
World3D alone took 7.7-8.3 s to create, and `PSMain` accounted for about 4 s of
that. The time goes to FXC's optimizer. Compiling in parallel still took about
5 s, and skipping optimization produced 44 % larger, unoptimized bytecode.

The same compile passed only `D3DCOMPILE_ENABLE_STRICTNESS`. Without IEEE
strictness, FXC assumes floats are never NaN and folds every `isnan()` test to
false. The finite guards on skinning, morphs, lit results, motion vectors and TAA
history therefore ran on Metal and OpenGL but not on Direct3D.

Removing the startup compile adds a build-time host tool that the runtime graphics
library depends on through a generated header, and one new Windows import
(`D3DCreateBlob`) for Zanna's native linker. Both are cross-layer dependencies
(see ADR 0196 and ADR 0232 for the import-mapping precedent).

## Decision

`src/runtime/graphics/3d/backend/vgfx3d_backend_d3d11_shader_manifest.inc` is the
single description of the backend's compiled shaders:

- `VGFX3D_D3D11_SHADER_COMPILE_FLAGS` is
  `D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS`, used by every
  compile.
- `VGFX3D_D3D11_SHADER_LIST(X)` expands `X(member, source, entry, target)` once
  for each `ID3DBlob *` in the backend's shader-blob set (16 entries, `vs_5_0` or
  `ps_5_0`).
- `vgfx3d_d3d11_shader_manifest_digest()` is FNV-1a over the flags and then over
  each entry's name, target and HLSL source text.

On native Windows builds (`WIN32`, not cross-compiling, host processor equal to
target processor), `src/runtime/CMakeLists.txt` builds the host tool
`zanna_d3d11_shader_bake` from `src/tools/d3d11_shader_bake/d3d11_shader_bake.c`.
The tool compiles the manifest in parallel with the runtime's flags and writes
`${CMAKE_BINARY_DIR}/generated/runtime/d3d11/vgfx3d_backend_d3d11_bytecode.inc`.
The header defines `VGFX3D_D3D11_EMBEDDED_BYTECODE_DIGEST` and one
`vgfx3d_d3d11_dxbc_<member>[]` array per entry. The tool writes a temporary file and
renames it into place. It exits 1 on a compile or I/O failure and 2 on a usage error.
`zanna_rt_graphics_obj` depends on the generated target and compiles with
`VGFX3D_D3D11_EMBEDDED_BYTECODE=1`.

At context creation the backend compares the recorded digest with the digest of the
HLSL linked into the binary. When they are equal, it wraps each array with
`D3DCreateBlob` and never calls `D3DCompile`. When they differ, or when the build
could not run the tool, it compiles from the same manifest and flags as before.

Zanna's Windows import planner maps `D3DCreateBlob` to `d3dcompiler_47.dll`, next to
`D3DCompile`, `D3DCompile2`, `D3DCompileFromFile` and `D3DReflect`. The
dynamic-symbol policy lists it as Windows-only.

## Consequences

- World3D creation on Windows drops from 7.7-8.3 s to 0.19-0.23 s, and Legacy
  Baseball's first frame arrives at about 1.4 s instead of 9.5 s.
- NaN guards in backend HLSL survive compilation, matching Metal and OpenGL.
- Cross-architecture and cross-compiled Windows builds keep working through the
  runtime compile, with the original startup cost.
- A shader edit rebuilds the generated header through the custom command's
  dependencies. A stale header is detected by the digest and never loaded.
- `d3dcompiler_47.dll` was already a runtime dependency of the backend. No product
  dependency is added. IL, verifier rules, the runtime C ABI and serialized formats
  are unchanged.

## Alternatives Considered

- **A persistent on-disk shader cache.** Rejected: the first launch after every
  install or update would still stall, and a cache adds invalidation and write-
  permission failure modes that a build artifact does not have.
- **Parallel runtime compilation.** Rejected: still about 5 s.
- **`D3DCOMPILE_SKIP_OPTIMIZATION`.** Rejected: 44 % larger unoptimized bytecode
  on every frame to save a one-time cost.
- **Checking in compiled bytecode.** Rejected: it drifts from the HLSL silently.
  The build-time bake cannot drift and needs no new tooling outside the tree.

## Validation

`test_vgfx3d_backend_d3d11_shader_bake` compiles the backend's finite-guard pattern
and requires the NaN self-comparison to survive with the manifest flags and to vanish
without them. It checks that the embedded digest matches the linked HLSL, that every
blob is DXBC whose reflected stage matches its target, and that the baked `VSSkybox`
is byte-identical to a runtime compile. `test_vgfx3d_backend_d3d11_shared` pins the
manifest against the blob struct (every member listed exactly once) and every entry
against its HLSL source. `test_platform_import_planners` maps `D3DCompile` and
`D3DCreateBlob` to `d3dcompiler_47.dll`.
