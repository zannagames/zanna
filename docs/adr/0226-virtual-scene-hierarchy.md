---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0226: Virtual Scene Hierarchy Model

- Status: Accepted
- Date: 2026-07-29
- Deciders: Zanna Studio maintainers
- Tags: zannastudio, gui-runtime, scale

## Context

The 3D scene editor's hierarchy pane retained one `GUI.TreeView` node per
presented row. Every structural refresh destroyed and recreated the full
widget row set, so the pane's practical ceiling was the retained-widget
cost, pinned at `MAX_EDITOR_NODES = 4096`. Mission-scale content wants
tens of thousands of rows with the same interaction contract: multi-
select, drag reorder/reparent, inline rename, pick-lock and visibility
badges, type icons, search, and reveal-on-select.

`GUI.VirtualTree` (rt_gui_ide) already projected an id-keyed external
model through `TreeView`'s viewport provider, but it lacked the runtime
surface Studio needs: removal, multi-selection, per-row icon/style,
application-directed drops, inline edit, reveal, and point lookup.

## Decision

1. **Extend the virtual-tree runtime surface** with twelve id-keyed
   operations: `RemoveNode`, `SelectIds`, `GetSelectedIds`,
   `SetNodeIcon`, `SetNodeStyle`, `SetNodeLoaded`, `TakeDropAction`,
   `RevealId`, `BeginEdit`, `TakeEditCommit`, and `RowIdAt`, plus a
   frozen `vg_treeview_virtual_row_t` presentation extension
   (`icon_name`, `icon_text`, `text_color`, `flags` with `SELECTED` and
   `DIM` bits). The widget grows virtual-mode multi-select click
   actions (toggle/range via modifiers), row-aware virtual drag with a
   latched BEFORE/INTO/AFTER drop consumed by poll, inline editing over
   virtual rows, and a scrollbar-aware point-to-row resolver.
2. **Studio's 3D hierarchy projects through one `hierarchyModel`**
   (`GUI.VirtualTree`) bound to the existing `hierarchyTree`. Model ids
   are the flat indices of the build that declared them, so the
   selection/drop/rename contracts keep their historical index strings.
   `RefreshHierarchy` unbinds, removes the previous top-level subtrees,
   redeclares rows through `AppendHierarchy` (icon, dim pick-lock
   style, eager `SetNodeLoaded`), and rebinds once — every mutation is
   an O(1) model edit while detached, and the single rebind re-projects
   the flattened order.
3. **`MAX_EDITOR_NODES` rises 4096 → 65,536** with the truthful
   clipping flag unchanged past the ceiling.
4. **Ordering contract**: `GetSelectedIds` reports the primary id
   first, then the remaining selection in flat order. `TakeDropAction`
   and `TakeEditCommit` resolve transient rows to ids at poll time, so
   consumers read them before mutating the model.

## Scale fixes the ceiling exposed

Raising the ceiling surfaced three latent quadratic costs, all fixed here:

- `rt_virtual_tree_add_node` reserved `size+1` buckets per insert; libc++'s
  `rehash` also shrinks, so bulk declaration oscillated bucket counts
  (~800x at 65k inserts). Reservation is now geometric.
- The viewport renderer resolved each node's parent row with a linear
  `FindFlatIndex` scan per node per frame (O(n^2) across a frame). The
  bounded walk presents parents before children, so a nonzero flat depth
  now proves parent presence in O(1).
- The parent-target dropdown added one widget item per eligible node.
  It now stops at `MAX_PARENT_TARGET_OPTIONS` (512) with a truthful
  tooltip; larger scenes reparent through hierarchy row drops.

A related runtime characteristic is recorded, not changed: flat
`SceneGraph.Add` walks the root sibling chain per call (~84 s for 65k
flat roots); the capacity fixture builds a 256x255 tree instead, which
also exercises real depth.

## Consequences

- Hierarchy scale is bounded by the id-keyed C model, not retained
  widgets; a 65,536-row document opens unclipped and one row past the
  ceiling clips truthfully (`zia_zannastudio_scene_capacity_65k` slow probe). The
  2,000-node probe's tripwires tightened from 120 s to 30 s open /
  15 s edit / 15 s undo with `METRIC:` lines for trend reading.
- Expansion capture reads the model's visible rows: an expanded row
  hidden under a collapsed ancestor re-collapses after a structural
  refresh. This is a deliberate fidelity trade against retained-node
  capture; interactive expand/collapse state is otherwise preserved by
  identity or path exactly as before.
- Inline rename commits travel by row id through `TakeEditCommit`
  (`ApplyHierarchyRename(index, text)`), never by retained handle.
- The retained TreeView path stays fully supported for every other
  tree (file explorer, linked workspaces); virtual mode is opt-in per
  control.

## Verification

`test_gui_runtime_manifest` re-baselined (1161 functions / 1052
methods); `zia_zannastudio_scene_editor_3d`,
`zia_zannastudio_scene_hierarchy_affordances`, light/camera/collider authoring
probes, `zia_zannastudio_scene_capacity` and the new
`zia_zannastudio_scene_capacity_65k` cover selection, drops, rename, badges, icons,
clipping, and budgets.
