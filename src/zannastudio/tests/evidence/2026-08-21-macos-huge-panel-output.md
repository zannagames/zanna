# macOS huge tool-panel output evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus this recommendation worktree.
- Host: macOS 26.6 (25G72), Apple Silicon arm64.
- Focused stress run: `zia_zannastudio_tool_panel_virtualization` — passed (1.79 seconds).
- Related panel run: toolbar, Debug surfaces, and console/search probes — all passed (15.17 seconds total in the retained grouped run).

The stress probe streams 10,000 Build lines in 100 incremental chunks through
the same `AppendBuildOutput` path used by live processes. It verifies the raw
pane's 10,000-line cap and exact tail, applies a broad filter, and asserts that
the filtered projection retains at most 5,000 logical rows while realizing only
the visible viewport. It selects the final row, resizes the live application
through narrow and wide layouts, verifies the virtual model and selection tail,
then applies an exact filter without losing the complete bounded source.

This work also replaced filtered Output's detached virtual counter with the
shared bounded row model. Full repaints preselect the newest retained matches
and publish the virtual projection once; the first version of the stress test
timed out at 90 seconds, while the optimized incremental version completes in
1.79 seconds. The related probes cover Search and Debug virtualization,
References filtering and durable navigation metadata, Output/References
responsive toolbar containment, wrapping, copy/clear, and console filtering.
