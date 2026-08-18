---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0252: `Zanna.Text.Table` and `Mesh3D` Bounds/Merge

## Status

Accepted (2026-08-16)

## Consulted

- `src/runtime/text/rt_textwrap.c` — the existing single-string aligners
- `src/runtime/core/rt_string_advanced.c` — `PadLeft`/`PadRight` byte-width rules
- `src/runtime/text/rt_table.c` — implementation
- `src/il/runtime/defs/api/core_crypto.def`, `.../classes/io_text.def` — registry

## Context

The runtime can align *one* string — `Zanna.Text.TextWrapper.AlignLeft`,
`AlignRight`, `Center`, `Truncate` — and it has two interactive grid widgets
(`Zanna.GUI.Grid`, `Zanna.Game.UI.HudTable`). It has nothing that lays out a
table of rows as text.

Every program that emits an aligned report therefore does the same thing: it
hand-counts the column widths into a header string literal, then keeps a chain
of `Fmt.IntPad(x, 4, " ")` calls in sync with that literal by eye. The Legacy
Baseball audit measured ~420 lines of this across seven writers, with headers
like

```text
"Player                  PA  AB   R   H  2B  3B  HR RBI  BB  SO   AVG   OBP   SLG"
```

whose column stops exist only in the author's head. Adding a column means
recounting the literal and every row-builder that feeds it. The project had
independently invented a row/cell type (`LiveBoxRow { cells: List[String]; hot:
Boolean }`) but still rendered it by hand.

This is not baseball-specific: any CLI tool, log formatter, debug dump, or
report generator hits it.

## Decision

Add `Zanna.Text.Table`, sitting on the same byte-width rules as
`Zanna.String.PadLeft`/`PadRight` so a table and a hand-padded line agree for
ASCII content.

| Member | Signature |
|---|---|
| `New()` | `obj()` |
| `AddColumn(header, width, align)` | `i64(str,i64,i64)` |
| `AddColumnAuto(header, align)` | `i64(str,i64)` |
| `SetTruncate(column, on)` | `void(i64,i1)` |
| `AddRow()` | `i64()` |
| `SetCell(row, col, text)` / `GetCell(row, col)` | |
| `ClearRows()` / `SetGutter(text)` | |
| `RenderHeader()` / `RenderRule(fill)` / `RenderRow(i)` / `Render(header, rule)` | |
| `ColumnCount` / `RowCount` / `AlignLeft` / `AlignRight` / `AlignCenter` | properties |

Design points that are not arbitrary:

- **Header and body come from one width declaration.** That is the entire
  point; it removes the class of bug where the literal and the pad calls drift.
- **`AddColumnAuto` resolves at render time**, over the header plus every cell,
  so a row added after the column still counts. This is what lets a caller stop
  counting widths altogether.
- **Truncation is opt-in per column.** A cell wider than its column makes the
  row ragged by default rather than silently losing data — the ragged row is
  visible, the clipped value is not.
- **A fixed column is never narrower than its own header.** A too-small
  declaration widens rather than clipping the heading.
- **Trailing padding on the last column is trimmed.** An aligned report should
  not carry invisible whitespace to the line end, which is exactly the kind of
  thing that makes a golden fixture diff for no visible reason.
- **Nothing here sorts.** Ordering is the caller's business; mixing layout and
  ordering into one object is how the GUI grids ended up hard to reuse.

## Consequences

- Report writers can declare columns once. The measured saving in the audited
  project is ~200 of its ~420 hand-aligned lines; the rest is genuinely
  domain-specific row construction.
- Migrating an existing byte-stable report is **not** automatically neutral:
  trailing-whitespace trimming and the "header widens a too-small column" rule
  can both change output. Any adoption against a pinned fixture has to be
  verified, not assumed.
- Widths are **byte** widths, matching `PadLeft`/`PadRight`. Multi-byte UTF-8
  content will not align visually. That is a deliberate consistency choice with
  the existing padders rather than a hidden limitation; a display-width variant
  is follow-on work if a caller needs it.
- The table owns malloc'd header/cell/gutter storage and installs a finalizer,
  so it needs no explicit disposal.
- Registry surface: 1 new class, 18 new functions. No IL opcode, grammar, or
  verifier changes.

---

# Part 2: `Mesh3D` Bounds Readback and Geometry Merge

## Context

`Zanna.Graphics3D.Mesh3D` could build a box, sphere, cylinder, or plane,
transform one, add individual vertices and triangles, and reserve storage. It
could not do two things every procedural-geometry program needs:

- **Read its own bounds.** The runtime already maintains an AABB and bounding
  sphere for culling (`rt_canvas3d_internal.h`, `rt_mesh3d_refresh_bounds`), but
  nothing exposed them. An application normalizing an imported asset — scale a
  prop to a known height, drop a hand mesh's wrist at the origin, recentre a
  ball, find a mesh top to derive a UV band — re-scanned the vertex buffer by
  hand. The audited project had four such scans in one file.
- **Merge one mesh into another.** With no way to put mesh B's triangles into
  mesh A, an application wanting a hundred parts as **one draw call** must
  re-emit every vertex itself. Draw-call count is a first-order performance
  constraint, so this is not optional: the audited project routes ~1,000 lines
  of procedural stadium/crowd/prop geometry through a single hand-written
  batcher for exactly this reason.

## Decision

**Bounds readback** — `BoundsMin`, `BoundsMax`, `BoundsCenter`, `BoundsSize`
(each `obj<Zanna.Math.Vec3>`) and `BoundsRadius` (`f64`). Each refreshes the
cached AABB first and returns a **fresh** Vec3 snapshot, never stored storage —
which is why they are registered in `RuntimeOwnership.hpp`'s fresh-Vec3
allowlist. An empty mesh returns `null` rather than a zero box, so "no
geometry" stays distinguishable from "geometry at the origin".

**Geometry merge** — `Append(src)` merges `src`'s vertices and triangles into
the receiver, rebasing indices.

Three decisions inside `Append` that are not arbitrary:

- **No transform argument.** The proposal was `Append(mesh, mat4)`. Meshes are
  GC-managed and there is no `Mesh3D.Destroy`, so a transform variant would have
  to create and release a temporary clone inside C — refcount handling that
  cannot be verified as cheaply as it can be avoided. The Zia-side idiom keeps
  the allocation explicit and under the collector:

  ```zia
  var part = box.Clone();
  part.Transform(placement);
  stadium.Append(part);
  ```

- **Skinned and morph-target meshes are rejected, not merged.** Bone indices and
  shape deltas are meaningless once vertices from another mesh share the buffer.
  Silently producing a corrupt rig is worse than refusing.
- **The double-precision position sidecar is preserved.** If either input
  carries `positions64`, the merged mesh does too, widening the other side's
  float positions. Otherwise appending a precise mesh into a float-only one
  would quantize it silently.

Appending a mesh to itself is rejected — the source buffers move under the copy
when storage grows.

## Consequences

- Procedural geometry can be batched into one mesh through the runtime instead
  of a hand-written vertex emitter.
- **`MeshBuilder3D` was not built.** The proposed `AddBox`/`AddSphere`/
  `AddCylinder`/`AddGrid`/`set_MirrorX` builder is sugar over
  `Clone`+`Transform`+`Append` plus the existing primitive constructors; it is
  worth adding once real call sites show which conveniences matter. `AddGrid`
  in particular is still missing — `Mesh3D.Plane` produces one unsubdivided
  quad, so a curved or deformable panel is still hand-emitted.
- The chirality/`MirrorX` concern from the audit is also unaddressed: a
  right-handed source frame still has to be mirrored by the caller.
- Registry surface: 6 new members on `Zanna.Graphics3D.Mesh3D`. The 3D ABI
  manifest gate fired on both additions and was reviewed and re-pinned, which
  is the gate working as designed.

## Links

- ADR 0251 — `Zanna.Testing`, the other harness gap from the same audit
- ADR 0235 — the precedent for landing "the runtime read side of the convention
  that every game previously hand-rolled"
- `baseball/plans/58-runtime-adoption.md` — the audit that measured the gap
