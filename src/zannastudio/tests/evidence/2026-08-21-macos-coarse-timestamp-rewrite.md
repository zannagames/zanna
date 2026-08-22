# macOS coarse-timestamp same-size rewrite evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus this recommendation worktree.
- Host/filesystem: macOS 26.6 (25G72), APFS temporary directory.
- Focused native run: `test_rt_ide_workspace` — passed (0.60 seconds).
- Focused Studio run: `zia_zannastudio_workspace_watcher` — passed (1.88 seconds).

The native integration captures `src/main.zia`'s exact filesystem modification
time, performs an equal-length content rewrite, restores the original time,
asserts that the visible timestamp is unchanged, and verifies that the bounded
content-sample hash changes. The Studio watcher integration then proves that
the paged fallback fingerprint observes an equal-length immediate rewrite and
publishes a changed project fingerprint.
