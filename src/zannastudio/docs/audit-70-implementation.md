# Zanna Studio 70-Item Audit Implementation Report

Last verified against source: 2026-08-20.

This report closes the 70 recommendations from the Studio and supporting
runtime audit. “Implemented” means the defect or design gap has a concrete
source change and focused regression evidence. It does not mean every manual
desktop-platform release row has been signed off; those rows deliberately
remain pending in the [platform sign-off matrix](platform-signoff.md).

The work spans Studio Zia, the native C/C++ runtime, ZannaGFX/GUI wake plumbing,
the debugger adapter, generated runtime bindings, ADRs, tests, and current-state
documentation. Public ABI and cross-layer decisions are recorded in ADRs 0275
through 0283, with the existing workspace-edit and shared-document decisions
updated where appropriate.

## Outcome

- Recommendations 1–69 are implemented with automated regression coverage.
- Recommendation 70 is implemented as a machine-validated 48-row matrix (42
  applicable rows and six backend-specific `not-applicable` rows). Its manual
  Windows/macOS/Linux evidence remains honestly `pending` until release hosts
  perform the procedures.
- No external product dependency was introduced.
- The closeout respected the shared-worktree constraint: incremental targets
  and focused tests only; no full repository rebuild and no full CTest run.

## P0 correctness and data safety

| # | Status | Implemented outcome | Primary evidence |
| --- | --- | --- | --- |
| 1 | Implemented | Windows workspace commits now use handle-relative rename/replacement semantics instead of renaming over a pre-created destination. Sidecars are opened and renamed through stable parent handles. | `runtime/io/rt_ide_primitives.cpp`; `RTIdeWorkspaceTests`; ADR 0151 |
| 2 | Implemented | Rollback retains an unrestored backup, emits `edit.rollback`, and reports the exact recovery sidecar instead of deleting the last good copy. | `rollbackWorkspaceWrites`; rollback failure-injection cases in `RTIdeWorkspaceTests` |
| 3 | Implemented | A pending write owns its reserved backup and partial temp before staging begins; every failure path removes only safe-to-remove sidecars. | `PendingWorkspaceWrite` lifecycle and sidecar-cleanliness regression |
| 4 | Implemented | Workspace-edit entry points reject non-sequence input with an `edit.type` diagnostic. Invalid input can no longer become a successful zero-edit transaction. | native validation tests in `RTIdeWorkspaceTests` |
| 5 | Implemented | Open-document edits convert line/column coordinates to UTF-8 byte offsets once, validate in byte space, and build each result without codepoint/byte mixing. | `services/workspace_edit_algorithms.zia`; Unicode rename cases in `phase0_phase1_probe.zia` |
| 6 | Implemented | Embedded Play now supplies an actual sequence of `NAME=VALUE` entries; runtime environment validators reject wrong container types. | `commands/build_commands.zia`; `build/build_system.zia`; `RTExecTests` |
| 7 | Implemented | Embedded Play launches with `Process.StartWithEnvOverlay`, preserving inherited PATH, home, loader, certificate, and broker state. | `BuildSystem.StartWithEnvironment`; `zia_zannastudio_phase2_phase3` |
| 8 | Implemented | Recovery uses an exclusive managed `Zanna.IO.FileLease`; process lifetime, nonblocking acquisition, and explicit release are defined by ADR 0276. | `core/recovery_store_base.zia`; `RTFileExtTests`; ADR 0276 |
| 9 | Implemented | Every Studio instance owns a distinct recovery namespace. Cleanup and lock release affect only that namespace or an explicitly claimed orphan. | `core/recovery_store.zia`; `zia_zannastudio_recovery` |
| 10 | Implemented | Swap names use SHA-256 over the full identity plus process/document nonces; metadata carries and validates instance/document identities. | `recovery_store_base.zia`; `recovery_probe.zia` |
| 11 | Implemented | Session records persist `recoveryPresent` independently of payload text, so a deliberately empty dirty buffer is restored as empty. | `session_manager.zia`; `session_restore_layer.zia`; recovery probes |
| 12 | Implemented | Location IDs remain monotonic across clears and are resolved through an ID map, preventing stale output rows from aliasing new locations. | `services/locations.zia`; `zia_zannastudio_phase0_phase1` |
| 13 | Implemented | Dirty open buffers are authoritative over watcher disk events. Watcher updates are coalesced and reapply the live overlay rather than replacing it with disk text. | `editor/project_index_updates.zia`; watcher and linked-workspace probes |
| 14 | Implemented | Open-document synchronization records active identities/revisions and prunes closed overlays cooperatively, restoring their current disk state. | `project_index_updates.zia`; `zia_zannastudio_workspace_watcher` |

## Reliability and cross-platform hardening

| # | Status | Implemented outcome | Primary evidence |
| --- | --- | --- | --- |
| 15 | Implemented | Process handles own complete trees: dedicated POSIX process groups and Windows Job Objects are terminated and drained through the existing lifecycle surface. | `runtime/system/rt_process.c`; `RTExecTests` |
| 16 | Implemented | Debugger commands enter a bounded newline-framed queue; partial `WriteStdin` progress removes only accepted bytes and retries the remainder. | `build/debug_session_state.zia`; `zia_zannastudio_debug` |
| 17 | Implemented | Terminal input uses the same bounded partial-write discipline, preserving paste and escape-sequence tails. | `terminal/terminal_session.zia`; terminal multi-session probe |
| 18 | Implemented | Evaluate and variables requests carry monotonic request IDs that the adapter echoes; replies are accepted only by the matching pending request. | `debug_session_base.zia`; `tools/zanna/DebugAdapter.cpp`; ADR 0275 |
| 19 | Implemented | Watch evaluation has a bounded deadline. Lost/stale responses expire without blocking later watch refreshes. | `debug_session_state.zia`; debug probe; ADR 0275 |
| 20 | Implemented | Debug control carry is capped at 1 MiB, framed with byte-safe slicing, and emits bounded visible diagnostics for malformed or oversized records. | `debug_session.zia`; `debug_probe.VerifyProtocolFraming` |
| 21 | Implemented | The synchronous two-second `FetchChildren` production path was removed. Variable expansion is request-driven and pumped asynchronously; blocking behavior is confined to probes. | debug session/view split and focused debug tests |
| 22 | Implemented | Native edit validation caps records (100,000), files (20,000), per-file input (64 MiB), aggregate source (256 MiB), replacement (128 MiB), output (256 MiB), and metadata (1 MiB/file). | `rt_ide_primitives.cpp`; boundary tests |
| 23 | Implemented | Root traversal and target access use no-follow, component-by-component, handle-relative operations (`openat`/`renameat` on POSIX and parent handles on Windows), closing the documented path-component race. | workspace access helpers; ADR 0151 |
| 24 | Implemented | Replacement preserves ownership, mode, timestamps, flags, bounded xattrs/ACL metadata, macOS resource metadata, and Windows file-object metadata/alternate streams. Metadata failure aborts before commit. | native metadata regressions in `RTIdeWorkspaceTests` |
| 25 | Implemented | Apply results distinguish primary failure, successful rollback, and manual-recovery-required rollback; diagnostic maps include retained sidecar paths. | `rollbackWorkspaceWrites`; Studio diagnostic formatting |
| 26 | Implemented | Edit grouping uses `File.IdentityKey` (volume/file ID on Windows; device/inode on POSIX), with lexical normalization only as a fallback. Hardlink, symlink/reparse, case, and separator aliases collapse before overlap checks. | `services/path_identity.zia`; `RTFileExtTests`; ADR 0282 |
| 27 | Implemented | Settings save retries a bounded compare-exchange loop and reapplies Studio-owned sections to a freshly read file, retaining unrelated concurrent-instance sections. | `core/settings_save_layer.zia`; settings migration probe |
| 28 | Implemented | Session restore enforces entry, byte, and elapsed-time budgets and exposes progress/cancellation between slices. Expensive folder/file work yields to paint. | `core/session_restore_layer.zia`; phase 0/1 probe |
| 29 | Implemented | Base64 recovery payloads at or above 64 KiB decode in an owned worker with immutable scalar input; only the UI pump publishes the completed model. Cancellation never invalidates worker input. | `core/session_recovery_decode.zia`; deterministic large-recovery probe |
| 30 | Implemented | Session pruning checks compare-exchange success, preserves the source file on conflict, and surfaces a restore warning instead of silently pretending stale state was removed. | `session_restore_layer.CommitPrunedSession`; phase 0/1 probe |
| 31 | Implemented | Disk metadata installs a content-hash baseline only after a successful bounded read. Read failure retains an unknown baseline and a visible I/O state. | `core/document_disk_state.zia`; phase 0/1 probe |
| 32 | Implemented | Text admission uses strict full-input UTF-8 validation with bounded size/binary checks; malformed sequences and unsuitable encodings are rejected before editing or save. | `core/text_admission.zia`; `runtime/text/rt_codec.c`; `RTCodecTests`; ADR 0277 |
| 33 | Implemented | Saves and multi-file edits prepare native transactions once, inspect validation, and consume the same immutable snapshot token at commit, eliminating duplicate reads and narrowing races. | `core/document_save_transaction.zia`; `Zanna.Workspace.PreparedEdit`; ADR 0280 |
| 34 | Implemented | Rename/delete preflight uses boundary-aware `path_identity` same-or-child logic rather than raw prefix comparison. | `commands/file_commands.zia`; tree-move/phase probes |
| 35 | Implemented | `safe_io` mutations return stable typed categories, native detail, retryability, recovery paths, and user-facing messages while retaining compatibility Boolean wrappers. | `services/safe_io_results.zia`; `safe_io_mutations.zia`; `zia_zannastudio_safe_io` |

## Performance and scalability

| # | Status | Implemented outcome | Primary evidence |
| --- | --- | --- | --- |
| 36 | Implemented | Process capture stores tagged shared output chunks and derives per-stream/ordered views without retaining three independent 16 MiB byte copies. | `runtime/system/rt_process.c`; `RTExecTests` |
| 37 | Implemented | Same-stream chunks grow geometrically and retain capacity, removing exact-size `realloc` behavior under sustained output. | process output stress cases in `RTExecTests` |
| 38 | Implemented | Build, debug, terminal, and SCM transfer stopped handles to an owned asynchronous process reaper; UI cancellation no longer waits through destruction grace periods. | `services/process_reaper.zia`; `zia_zannastudio_process_reaper` |
| 39 | Implemented | One executable resolver validates explicit paths, PATH candidates, file kind, and executability, and produces structured launch diagnostics for Build/Run/SCM. | `build/executable_resolver.zia`; runtime-policy probe |
| 40 | Implemented | Stop intent and last successful PTY dimensions live on each `TerminalSession`; switching tabs cannot restart or suppress resize for another shell. | terminal controller/session; multi-session probe |
| 41 | Implemented | Terminal sessions support close and bounded UTF-8 rename, and the controller enforces a 16-session cap with deterministic active-session reassignment. | `terminal_controller.zia`; terminal-open and terminal-multi probes |
| 42 | Implemented | Scrollback and pending output use a fixed-capacity UTF-8 byte ring that repairs overflow boundaries and retains incomplete trailing code points until the next chunk. The VT corpus covers vim/less/htop-style streams. | `terminal_session.zia`; terminal-multi/render/altscreen probes |
| 43 | Implemented | `ScmView.SetRepo` increments a repository generation and clears active/queued jobs, pending paths/text, rows, diffs, selection, and operation state. | `scm/scm_view.zia`; SCM view probe |
| 44 | Implemented | Porcelain-v2 `-z` parsing retains exact filename bytes, including leading/trailing spaces; display escaping is a separate `DisplayPath` operation. | `scm/scm_git.zia`; SCM parser probe |
| 45 | Implemented | Git jobs carry explicit stdout/stderr truncation flags. A truncated status stream becomes an incomplete/error result and is never published as authoritative repository state. | `GitJob`, `StatusFromOutputState`; SCM probes |
| 46 | Implemented | Main and gutter diffs use `--no-ext-diff`, `--no-textconv`, and `--no-color`, making output shape independent of user Git configuration and helpers. | `scm_git.StartDiff/StartGutterDiff`; SCM diff/gutter probes |
| 47 | Implemented | Revision reads treat only the specifically expected missing side as empty; bad revisions, process errors, and truncation stay visible and prevent a misleading diff. | `scm_view_presenter.zia`; SCM history/view probes |
| 48 | Implemented | The dead in-app credential/prompt surface was removed. Fetch/push/pull are noninteractive, set `GIT_TERMINAL_PROMPT=0`, and rely on external credential helpers, GCM, Keychain, or SSH agents; docs and sign-off procedures match that security model. | `scm_git_support.zia`; status/workflow/platform docs |
| 49 | Implemented | Status/history use virtual rows, repository-generation tags, stale-result rejection, and coalesced read-only jobs. Switching repositories cannot publish an old result. | `scm_view_jobs.zia`; `scm_view_presenter.zia`; SCM view/history probes |
| 50 | Implemented | SCM exposes fetch, fast-forward-only/merge/rebase pull choices, merge/rebase abort, live operation/conflict state, cancellation, and recovery guidance. | `scm_view_controls.zia`; `scm_git.zia`; SCM view probe |
| 51 | Implemented | Directory topology reconciliation is additive: existing watchers remain live while bounded discovery adds new directories; fallback state is not reset during the gap. | `app/workspace_watcher.zia`; watcher probe |
| 52 | Implemented | Fallback fingerprints combine nanosecond metadata with bounded content sampling, detecting same-size rewrites inside a coarse timestamp tick. | `workspace_watcher_fallback.zia`; native fingerprint tests |
| 53 | Implemented | Watcher changes enqueue/coalesce content work and the language frame pumps it under absolute byte/time deadlines instead of parsing a fixed synchronous batch. | `project_index_updates.zia`; watcher and runtime-policy probes |
| 54 | Implemented | Known-file content refreshes preserve index completeness. Only topology changes or an exhausted failure path invalidate full discovery. | project-index update/enumeration split; watcher probe |
| 55 | Implemented | Transient read/stat failures retain prior symbols and enter a bounded retry queue before destructive removal; terminal failure is explicit. | `project_index_updates.zia`; watcher probe |
| 56 | Implemented | `IsWorkspaceQueryReady()` is the single readiness definition: `complete && !truncated`. Navigation and workspace-query jobs route through it. | `project_index.zia`; workspace command probes |
| 57 | Implemented | Open overlays track path/document identity and revision rather than retaining duplicate full source strings; the runtime index owns its snapshot. | `project_index_state.zia`; `project_index_updates.zia` |
| 58 | Implemented | File-index stat failures emit negative size plus a diagnostic instead of masquerading as a zero-byte file, making Studio’s failure path reachable. | native cursor emitter; `RTIdeWorkspaceTests` |
| 59 | Implemented | `Zanna.Workspace.FileIndexCursor` is an explicit owned handle with immutable traversal generation, paged results, stale-page rejection, and explicit destroy. The global eight-entry cache is gone. | `services/workspace_file_cursor.zia`; ADR 0278; native cursor tests |

## Architecture, UI, and maintainability

| # | Status | Implemented outcome | Primary evidence |
| --- | --- | --- | --- |
| 60 | Implemented | Workspace edits build per-file byte-coordinate batches, merge-sort them in O(n log n), validate adjacent ranges, and apply each file through one `StringBuilder` transaction. | `workspace_edit_algorithms.zia`; workspace-edit preview probe |
| 61 | Implemented | `LocationStore` has O(1) ID lookup, monotonic IDs, and owner-scoped eviction/publication so replaced Search/Build/Reference results release obsolete locations. | `services/locations.zia`; phase 0/1 probe |
| 62 | Implemented | Diagnostics publish stable keys/revisions and Problems updates incrementally; output consumes only appended bytes/rows and does not reparse the complete log each frame. | `build/diagnostic_publication.zia`; `commands/build_commands.zia`; diagnostic-stream probe |
| 63 | Implemented | Diagnostic parsing advances a byte cursor without rebuilding remainders, bounds each JSON record before decode, and emits a visible record when the 5,000-diagnostic cap is reached. | `build/diagnostic_parser.zia`; diagnostic-stream probe |
| 64 | Implemented | Event wait occurs before `BeginFrame`, so intentional sleep is excluded from frame latency. Perf records are queued through `Async.RunOwned` rather than appended on the render path. | `studio_application_surface_frame.zia`; `editor/perf_monitor.zia`; runtime-policy/perf probe |
| 65 | Implemented | Process and PTY activity signal a refcounted GUI wake target through platform backends; the frame loop uses absolute deadlines and language work checks them between items. | `runtime/system/rt_activity_wake.*`; ZannaGFX/GUI adapters; ADR 0281; `test_window`/`RTExecTests` |
| 66 | Implemented | `LifecycleController` is the idempotent shutdown owner. It drains producers/reapers before destroying consumers and preserves recovery state when a bounded drain times out. | `app/lifecycle_controller.zia`; shutdown-state probe |
| 67 | Implemented | `EditorScheduler` is now a registry of `ScheduledJob` records keyed by kind/path/revision/generation; parallel legacy flag/deadline fields were removed. | `editor/scheduler.zia`; phase/runtime-policy probes |
| 68 | Implemented | The ratchet was reduced from 87 to 73 tracked size debts and from 49 to 47 layer debts. Large session, SCM, project-index, scheduler, debug, document, search, and application coordinators were split by state/lifecycle ownership. | `scripts/architecture_baseline.tsv`; architecture guard; updated source map |
| 69 | Implemented | Split views share a `DocumentBuffer` with one mutable GUI owner, and detached tools separate `ToolWindowModel` from `NativeToolWindowHost`. | `core/document_buffer.zia`; `ui/native_tool_window_host.zia`; ADR 0279; split/native-window probes |
| 70 | Matrix implemented; sign-off pending | A schema-checked Windows/macOS/Linux matrix covers all requested picker, filesystem, text, SCM, terminal, process, output, debugger, accessibility, and graphics scenarios. Applicable rows begin at `pending`; backend-impossible rows are explicitly `not-applicable`. | `tests/platform_signoff.tsv`; `platform-signoff.md`; manifest probe |

## Supporting integration fixes found during closeout

Two additional defects appeared only when the permitted clean Studio native
build exercised the complete program:

1. The generated `__zia_iface_init` exceeded AArch64’s function-local virtual
   register namespace. The Zia lowerer now emits ordered class-registration
   helpers capped by estimated IL-result cost, while small modules keep the
   compact direct form. `ZiaIfaceDispatch.LargeClassRegistryUsesBoundedInitializationHelpers`
   is the permanent regression.
2. The native linker’s dynamic-symbol policy did not recognize the descriptor,
   xattr, process-group, and spawn-attribute libc calls used by the hardened
   runtime. ADR 0283 records the cross-layer dependency; the allowlist and
   platform scoping now cover it, with
   `DynamicSymbolPolicy.RuntimeWorkspaceAndProcessSymbolsAccepted` preventing a
   late application-link failure.

After those fixes, `./scripts/build_ide.sh --clean` produced and staged a 32 MiB
macOS arm64 Studio payload successfully.

## Validation record

The final focused validation set was:

- Whole Studio source check: clean.
- Architecture guard: 517 source units checked; 73 tracked size debts and 47
  tracked layer debts; passed.
- 24 targeted Studio CTest entries spanning recovery, safe I/O, watcher/index,
  multi-root, SCM, terminal, process reaping, diagnostics, perf/wake policy,
  debugger framing, shutdown, shared documents, native tool windows, workspace
  edits, and the sign-off manifest: all passed.
- Native `test_window`, `test_rt_file_ext`, `test_rt_codec`,
  `test_rt_ide_workspace`, and `test_rt_exec`: all passed.
- Zia large-class initializer, native-link import-policy, and complete host
  runtime-archive import-audit tests: passed.
- `git diff --check`, strict clang-format checks for the changed native files,
  generated runtime documentation, and platform-policy lint: clean.
- MinGW Windows syntax validation for the new file-identity runtime path:
  passed.
- Clean Studio-only macOS arm64 native build: passed.

This is intentionally not a claim of full platform release sign-off. The
interactive Windows/macOS/Linux rows remain the release team’s evidence gate,
and no full CTest run was performed during concurrent repository work.
