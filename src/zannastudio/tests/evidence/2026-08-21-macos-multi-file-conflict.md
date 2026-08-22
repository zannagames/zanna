# macOS multi-file conflict evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus this recommendation worktree.
- Host: macOS 26.6 (25G72), Apple Git 2.50.1.
- Focused run: `zia_zannastudio_scm_view` — passed (2.77 seconds).

The probe creates a real two-branch, two-file content conflict. Source Control
retains both exact path rows, detects the externally started merge from Git's
control files, resolves the second path with Incoming and the first with
Current, requires a separate Stage for each, keeps ordinary Commit disabled
through partial and complete resolution, enables Continue only after all
conflicts are staged, and completes the merge noninteractively with a clean
status. The probe drives rendered controls through the GUI test harness rather
than calling their action methods directly.
