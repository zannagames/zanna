# macOS malformed and large source evidence — 2026-08-21

- Source revision: `cf71e21735ca75e184891f56494c05052e0048d5` plus this recommendation worktree.
- Host: macOS 26.6 (25G72), Apple Silicon arm64, APFS.
- Focused run: admission, navigation, and edge-corpus probes — passed (8.91 seconds total).

The model probe validates complete-file strict UTF-8 and embedded-NUL checks,
including an invalid surrogate encoding and a NUL beyond the former prefix
sniff boundary. Malformed UTF-8 can only become a bounded read-only metadata
preview through the document manager.

The rendered navigation probe creates an actual invalid UTF-8 `.zia` file and
an actual 4,000,001-byte `.zia` file. It opens both through Studio's shared GUI
file-open command and verifies both are rejected before tab creation, neither
replaces the active editable buffer, and the current document and caret remain
unchanged. The same probe opens an unsupported binary through structured and
Quick Open navigation and verifies its safe read-only preview. The edge corpus
adds the checked malformed/pathological fixtures used by compiler and runtime
boundaries.
