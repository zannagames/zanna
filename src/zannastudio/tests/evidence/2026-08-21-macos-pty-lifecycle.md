# macOS PTY lifecycle evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus this recommendation worktree.
- Host: macOS 26.6 (25G72), Apple Silicon arm64.
- Focused native run: `test_rt_exec` — passed (5.59 seconds).
- Focused Studio run: seven terminal/process-reaper probes — passed (7.50 seconds total).

The native regression opens a real POSIX PTY session whose shell owns a live
descendant, requests termination through `rt_pty_kill`, waits for the session
leader, and verifies that the descendant no longer exists. It then opens,
waits, drains, and destroys twelve additional PTYs, verifies each final output
tail, and confirms that the process returns to its exact open-file-descriptor
baseline.

The Studio run covered terminal lifecycle, real shell open/close, hidden start,
rendering, alternate-screen restoration, the sixteen-session controller limit,
and asynchronous process reaping:

```text
zia_zannastudio_terminal
zia_zannastudio_process_reaper
zia_zannastudio_terminal_open
zia_zannastudio_terminal_hidden_start
zia_zannastudio_terminal_render
zia_zannastudio_terminal_altscreen
zia_zannastudio_terminal_multi
```
