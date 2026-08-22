# macOS rename-conflict evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus this recommendation worktree.
- Host: macOS 26.6 (25G72), Apple Git 2.50.1.
- Focused run: `zia_zannastudio_scm_view` — passed (3.33 seconds).

The probe creates a real rename/rename conflict in which one source path is
renamed to distinct Current and Incoming destinations. Studio retains all
three exact unmerged rows, recognizes the externally initiated merge, selects
Current through the rendered conflict action, models the documented edit by
removing the unwanted Incoming destination, stages the complete resolution,
keeps ordinary Commit gated, continues the merge noninteractively, and verifies
that only the selected destination remains in a clean worktree.
