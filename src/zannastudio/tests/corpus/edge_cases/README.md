# Zanna Studio Edge-Case Corpus

This directory is the reusable, file-backed adversarial corpus for Studio's
text-facing service layer. `manifest.ini` is the versioned inventory;
`edge_case_corpus_probe.zia` loads it under strict case-count and file-size
bounds and drives the production implementations.

Fixture files are UTF-8 with LF line endings. Cases that set `newline=crlf`
are converted in memory before execution so the checked-in data remains stable
across Git configurations while still exercising byte-exact CRLF behavior.
Inputs and expected outputs remain separate so failures show an ordinary,
reviewable fixture diff.

Supported kinds are:

- `format`: full Zia, BASIC, or plain-text formatting plus idempotence;
- `organize-binds`: deterministic leading-bind sorting and deduplication;
- `identifier`: token replacement outside strings and line comments;
- `diagnostic`: legacy, JSON, malformed, and independently chunked streams;
- `search`: literal/regex matching, Unicode boundaries, zero-width progress,
  and capture replacement;
- `keybindings`: legacy migration and structured-schema rejection behavior;
- `argv`: shell-free project argument tokenization.

To add a regression, add bounded input/expected fixtures, append one indexed
section, and increment `[corpus] count`. Case names must be unique and fixture
paths must be relative without `..` traversal. Do not place secrets, machine
paths that vary by host, or generated build output in this corpus.
