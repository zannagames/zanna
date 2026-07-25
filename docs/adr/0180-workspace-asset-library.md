---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0180: Add Workspace Asset Libraries

## Status

Accepted (2026-07-24)

## Context

Studio's scene editors choose assets through a bounded, searchable project
browser (`scene_asset_browser.zia`), which discovers files by extension over
the multi-root workspace index. That answers "what files exist" but not
"which assets matter, what are they for, and how should they be imported":
there is no tagging, no way to distinguish the canonical tileset from a
scratch export, and every layer-atlas assignment re-derives tile dimensions
from the scene instead of from the asset's own known grid. The project
component schema system (ADR 0160) already established the pattern for
root-local, bounded, atomically written project metadata with external
conflict detection and file-scoped undo — the asset library reuses that
machinery rather than inventing new persistence.

## Decision

An optional `asset-library.json` at a workspace root describes that root's
notable assets:

```json
{
  "version": 1,
  "entries": [
    {
      "path": "assets/tiles/biome-tile-atlas.png",
      "tags": ["tileset", "release"],
      "tileWidth": 64, "tileHeight": 64,
      "notes": "Canonical biome atlas; frame order matches TILE_* ids."
    }
  ]
}
```

Contract:

- Root `version` is integer `1`; `entries` holds at most 4,096 objects.
- `path` (required) is a root-relative portable path, unique within the file
  without regard to case; entries whose file is currently missing are
  retained and shown with a missing badge, never dropped silently.
- `tags` is 0..32 portable identifiers of at most 64 characters each.
- `tileWidth`/`tileHeight` (optional, 1..4096) record the asset's native
  frame grid; `notes` is display text of at most 1,024 characters.
- The complete file is limited to 1 MB. A malformed file is rejected whole
  with a contextual message — never a partially parsed library.
- Unknown version-1 members are preserved on every retained object; writes
  are atomic rooted replacements guarded by expected bytes with external
  conflict detection, and library editing owns an independent 20-snapshot
  file undo/redo — all the exact ADR 0160 mechanics, shared, not copied.

Studio behavior:

- The asset browser gains library awareness: tag filter chips, a
  library-only toggle, and entry badges. Discovery remains index-backed and
  bounded; the library annotates results and pins entries whose files
  resolve, it never replaces filesystem discovery.
- Entries are created and edited in place from the browser (tag editing,
  grid fields, notes) through the shared transaction machinery. Library
  edits never touch scene documents, scene history, or dirty state.
- When a library entry with `tileWidth`/`tileHeight` is assigned as a layer
  atlas and the scene's tile dimensions differ, Studio surfaces the asset's
  native grid as an informational default for new scenes and a visible
  mismatch note for existing ones. It never resizes a scene implicitly.
- Ownership follows ADR 0160's root rules: a scene consults the library of
  its owning workspace root; unsaved scenes use a library only in
  single-root workspaces.

The library is editor metadata. Runtimes and games do not read it, and no
scene content references it; removing the file loses tags and defaults,
nothing else.

## Consequences

- Authors can mark canonical assets, filter to them, and stop re-entering
  frame grids that the asset already dictates.
- Reusing the schema-file machinery keeps a second project-metadata format
  from growing its own persistence bugs; both files share tests for
  conflict, undo, and preservation behavior.
- Missing-file retention means renames surface as visible badges instead of
  silent library shrinkage.
- A Studio probe must pin: parse/reject rules, tag filtering, badge states,
  atomic write + external-conflict + undo behavior through the shared
  machinery, grid-default surfacing without implicit scene mutation, and
  multi-root ownership.

## Alternatives Considered

- **Per-asset sidecar files (`foo.png.meta`).** Rejected: thousands of tiny
  files pollute project trees and diffs; a single bounded library file
  matches the established project-metadata pattern.
- **Embedding import settings in scene JSON.** Rejected: import settings
  describe the asset, not any one scene; duplication across scenes would
  drift.
- **A binary cache/database.** Rejected: project metadata must be
  diff-reviewable text like every other Zanna project file.
- **Extending `scene-components.json` with an assets array.** Rejected: the
  component schema file has one job (typed gameplay templates); mixing
  concerns would couple two unrelated editing surfaces to one undo history.
