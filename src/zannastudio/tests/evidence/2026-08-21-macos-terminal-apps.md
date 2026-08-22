# macOS integrated terminal application evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus this recommendation worktree.
- Host: macOS 26.6 (25G72), Apple Silicon arm64.
- Applications: Apple Vim 9.1 (patches 1–1752), less 668, htop 3.5.1.
- Focused run: `terminal_apps_probe.zia` — passed (1.252 seconds).

The probe launches each installed application as a real child through
`TerminalSession` and `Pty.OpenWithEnvOverlayResult`, and appends the resulting
PTY byte stream to Studio's terminal-mode `OutputPane`.

- Vim opens a real temporary file, redraws it, receives a live 80x24 to 100x30
  resize, accepts normal-mode/insert-mode input, writes the file, exits with
  `:wq`, and leaves the expected saved text on disk.
- Less pages a 240-line UTF-8 file, performs an interactive `/` search for a
  late-file marker, redraws after a 96x28 resize, and exits with `q`.
- Htop paints a real process view, exposes its `Tasks` display, redraws after a
  104x32 resize, and exits with `q`.

Each scenario seeds a distinct primary-screen sentinel before the application
enters its alternate screen and verifies that the sentinel is restored after
exit. The probe has also been registered as the focused
`zia_zannastudio_terminal_apps` test; hosts without the explicitly required
applications report a skip rather than substituting canned escape sequences.
