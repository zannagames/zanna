---
status: active
audience: contributors
last-verified: 2026-08-28
---

# ADR 0305: Separate Background Search Analysis from Virtual Tool-Result Publication

## Status

Accepted

## Context

Studio project search bounded files and bytes per frame, but each selected file
was still read, hashed, split, matched or compiler-scanned, and fully rendered
on the GUI thread. File and byte counts do not bound pathological regex or
compiler work, and thousands of matching rows could still consume a frame.
Workspace-symbol commands also retained a synchronous whole-workspace fallback.

Search result presentation crosses the `commands` to `ui` layer, while source
analysis depends on `services`, `basic`, and `editor` compiler adapters. The
architecture guard requires any new cross-layer ownership to be deliberate.

## Decision

Add a command-owned `SearchFileJob` that processes one immutable file request
through `Async.RunOwned`. Disk reads, source hashing, line splitting,
literal/regex matching, and Zia/BASIC symbol extraction occur on that worker.
Its result is a sequence of display-neutral maps; it never borrows GUI,
location-store, document-manager, or editor objects. Open-document text is
snapshotted on the GUI thread before queueing.

Add a separate Search controller publication layer. It is the only new layer
that translates worker records into tool rows and navigation locations. Each
pump publishes at most 64 rows and spends at most 4 ms in row publication; the
overall cooperative pump uses an 8 ms soft scheduling budget. Canceled and
superseded generations are discarded. Remove the synchronous whole-workspace
symbol path.

References uses a filtered visible-row projection over its durable row model
and binds `GUI.VirtualList` at the shared 256-row threshold. Stable row IDs and
navigation data remain in the durable model, so filtering, selection, trimming,
and transitions between concrete and virtual presentation preserve behavior.

Finally, move build-job, Save-All, and BASIC-query result types into focused
leaf modules. Tighten the architecture baseline for `file_commands.zia` from
986 to 974 lines and for `basic_workspace_query_job.zia` from 773 to 770 lines;
the expanded `build_system.zia` remains within its 450-line target.

## Consequences

- Source complexity and disk latency no longer directly monopolize the GUI
  thread during text or workspace-symbol search.
- Result rendering has an independent deterministic row and time budget.
- There is one production workspace-symbol pipeline and one cancellation model.
- References can retain up to the bounded model cap without realizing every GUI
  row, including when a filter changes the visible projection.
- Architecture debt decreases rather than granting new line-count exemptions.

## Alternatives Considered

Adding only a wall-clock check around the existing per-file loop cannot preempt
one expensive file. Publishing worker results directly from the callback would
race GUI and location state. Rebuilding every References row after each append
would virtualize storage but introduce quadratic incremental publication.
