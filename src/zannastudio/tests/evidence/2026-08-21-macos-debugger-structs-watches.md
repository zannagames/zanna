# macOS debugger structures and watches evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus this recommendation worktree.
- Host: macOS 26.6 (25G72), Apple Silicon arm64.
- Focused run: eleven VM and Studio debugger tests — passed (7.32 seconds total).

The Studio probes launch the real out-of-process
`zanna run --debug-adapter` flow, stop native VM execution at source
breakpoints, and validate correlated frames, locals, evaluation, stepping, and
termination. Structured expansion covers nested user classes, class elements
inside lists, list fields inside classes, paged variable trees, and a canonical
Map keyed value. Resume invalidates stop-scoped variable references while
preserving expansion identity.

Rendered GUI harness scenarios cover watch entry by Enter and pointer,
duplicate rejection, exact selected-row removal, refresh availability, clear,
responsive dock containment, call-stack filtering, and debug-console surfaces.
The focused set was:

```text
test_vm_debug_script
test_vm_debug_src_breakpoint
test_vm_debug_src_breakpoint_unknown_file
test_vm_debug_watches
zia_zannastudio_debug
zia_zannastudio_debug_vars
zia_zannastudio_debug_fields
zia_zannastudio_debug_tree
zia_zannastudio_debug_watch_toolbar
zia_zannastudio_debug_tool_surfaces
zia_zannastudio_run_debug_view
```
