---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0279: Share One Mutable Document Buffer and Separate Native Tool Hosts

Date: 2026-08-20

Status: Accepted

## Context

ADR 0154 rejected same-document split views because attaching two independent
`EditorBuffer` values to one `Document` creates divergent text and undo
histories. That safety rule was correct, but it left Split Editor unlike mature
workbenches: a source could not be viewed at two independent scroll positions.

Studio also supported attached docks and an in-window floating panel only. The
GUI runtime already registers multiple live `Zanna.GUI.App` instances and saves
and restores each App's focus, capture, tooltip, font, and native-window state.
Studio had no host-neutral tool model or secondary-window lifecycle to use that
capability. Widget ownership therefore conflated a tool's data with its primary
workbench placement.

Same-document session persistence and the new app-to-UI host edges alter
cross-layer ownership contracts, so an ADR is required. No runtime C ABI is
added or changed.

## Decision

Every `Document` owns a `SharedDocumentBuffer` model containing canonical text,
a monotonic generation, the live view count, and the physical view that owns
mutation. `Document.content` remains a compatibility mirror for scene and older
command code, but text-editor publication updates both values atomically.

A same-document split has exactly one mutable GUI `EditorBuffer`. The focused
pane owns that buffer and its single undo history. The inactive pane contains a
read-only text mirror with independent cursor and scroll state. When focus
moves, Studio detaches the canonical buffer from the old pane and attaches it
to the new pane, restores each pane's view state, and refreshes the old pane as
the read-only mirror. Content generations update the mirror before autosave,
recovery, indexing, or semantic snapshots. Opening an unrelated document while
such a pair is active closes the pair first rather than manufacturing a second
mutable owner.

Structured session records may write equal `splitFirstIndex` and
`splitSecondIndex` values. Equal indexes mean two views of the same restored
`Document`; distinct indexes retain the ADR 0154 two-document behavior. This is
backward compatible with existing session keys and creates no duplicate tab or
recovery identity.

Detached tool state is represented by `ToolWindowModel`, which has no GUI
handles. `NativeToolWindowHost` owns a secondary `Zanna.GUI.App` and projects
the model into that App's widgets. It makes the secondary App current only
while polling, synchronizing, and rendering it, and restores the primary App
before returning. Output generations distinguish append-only suffixes from
bounded replacement snapshots, avoiding whole-log replay during streaming.
Closing the secondary window returns the tool to the primary workbench without
closing Studio. The first hosted tool is Output; the boundary is intentionally
generic for later Problems, Search, terminal, and scene hosts.

## Consequences

- Two panes can display and independently navigate one document without
  divergent text or undo histories.
- The inactive same-document view is deliberately read-only; clicking it moves
  the canonical mutable buffer and then enables editing there.
- Same-document split topology round-trips through existing session INI keys.
- Output can live in a true native, movable, multi-monitor window while its
  bounded model continues receiving build output in the primary application.
- Secondary App destruction is explicit and precedes primary GUI teardown.
- Tool models can gain additional native hosts without moving widget handles
  across application trees.
- Mirroring a changed source currently materializes canonical text once for the
  inactive view. The operation is revision-gated and never runs on idle frames.
