---
status: active
audience: public
last-verified: 2026-07-26
---

# Zia Tooling

> In-process Zia compiler services for editors and IDE tooling.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Zia.Completion](#zannaziacompletion)
- [Zanna.Zia.Toolchain](#zannaziatoolchain)
- [Zanna.Zia.Document](#zannaziadocument)
- [Zanna.Zia.SemanticJob](#zannaziasemanticjob)
- [Zanna.Zia.ProjectIndex](#zannaziaprojectindex)

---

## Zanna.Zia.Completion

Completion, signature, hover, symbol, diagnostic, and semantic-token services
for an in-memory Zia source buffer.

**Type:** Static utility class

All cursor-taking methods use a 1-based `line` and a 0-based byte `col`.
Prefer the `*ForFile` forms in an editor: the path is part of the analysis
cache key and supplies the base directory for relative `bind` resolution.

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Complete` | `String(String, Integer, Integer)` | Return legacy tab-delimited completion rows. |
| `CompleteForFile` | `String(String, String, Integer, Integer)` | Path-aware legacy completion rows. |
| `Items` | `Seq(String, Integer, Integer)` | Return structured completion-item maps. |
| `ItemsForFile` | `Seq(String, String, Integer, Integer)` | Path-aware structured completion-item maps. |
| `BeginItemsForFile` | `SemanticJobHandle(String, String, Integer, Integer)` | Start an asynchronous structured completion query. |
| `SignatureHelp` | `String(String, Integer, Integer)` | Return display text for the active call, or empty. |
| `SignatureHelpForFile` | `String(String, String, Integer, Integer)` | Path-aware signature display text. |
| `SignatureInfo` | `Map(String, Integer, Integer)` | Return structured signature information. |
| `SignatureInfoForFile` | `Map(String, String, Integer, Integer)` | Path-aware structured signature information. |
| `BeginSignatureInfoForFile` | `SemanticJobHandle(String, String, Integer, Integer)` | Start an asynchronous signature query. |
| `Check` | `String(String)` | Return legacy tab-delimited diagnostics. |
| `CheckForFile` | `String(String, String)` | Return path-aware legacy diagnostics. |
| `Hover` | `String(String, Integer, Integer)` | Return human-readable information for the symbol at the cursor, or empty. |
| `HoverForFile` | `String(String, String, Integer, Integer)` | Path-aware hover text. |
| `HoverInfo` | `Map(String, Integer, Integer)` | Return structured hover information. |
| `HoverInfoForFile` | `Map(String, String, Integer, Integer)` | Path-aware structured hover information. |
| `BeginHoverInfoForFile` | `SemanticJobHandle(String, String, Integer, Integer)` | Start an asynchronous hover query. |
| `Symbols` | `String(String)` | Return serialized symbol rows. |
| `SymbolsForFile` | `String(String, String)` | Return path-aware serialized symbol rows. |
| `BeginSymbolsForFile` | `SemanticJobHandle(String, String)` | Start an asynchronous symbol query. |
| `BeginTokensForFile` | `SemanticJobHandle(String, String)` | Start an asynchronous semantic-token query. |
| `ClearCache` | `Void()` | Drop the singleton completion/signature analysis cache. |
| `IsAvailable` | `Boolean()` | True when the full editor-service bridge is linked; the weak stub returns false. |

`ClearCache` affects the synchronous `Complete*`, `Items*`, and
`Signature*` calls that share the singleton completion engine. Hover and
symbol calls parse independently, and each asynchronous job uses its own
engine.

### Completion Item Map

`Items*` and `SemanticJob.CompletionItems` return a `Seq` of maps with these
fields:

| Key | Type | Description |
|-----|------|-------------|
| `label` | String | Text displayed in the completion list. |
| `insertText` | String | Text to insert; snippets can contain newlines. |
| `kind` | Integer | Numeric completion kind, listed below. |
| `kindName` | String | Stable textual kind name. |
| `detail` | String | Type, signature, or short auxiliary text. |
| `documentation` | String | Documentation when available. |
| `source` | String | Provider such as `scope`, `runtime`, `keyword`, or `snippet`. |
| `commitCharacters` | String | Characters that may commit the item. |
| `isSnippet` | Boolean | True when `insertText` is a snippet. |
| `cursorOffset` | Integer | Byte offset for the cursor after insertion; `-1` means after all inserted text. |
| `replacementStartLine` / `replacementEndLine` | Integer | 1-based replacement lines. |
| `replacementStartColumn` / `replacementEndColumn` | Integer | 0-based, end-exclusive replacement columns. |

Completion kinds are `0` keyword, `1` snippet, `2` variable, `3` parameter,
`4` field, `5` method, `6` function, `7` entity, `8` value, `9` interface,
`10` module, `11` runtime class, and `12` property. Their `kindName` values are
the corresponding lower-case names, except `runtimeClass` uses camel case.

### Signature Information Map

`SignatureInfo*` and `SemanticJob.SignatureInfo` always return a map. With the
full service, `available` is false when no call resolves; the remaining scalar
fields are then empty or zero and the sequences are empty. The weak stub
returns the same map shape with `source = "unavailable"`.

| Key | Type | Description |
|-----|------|-------------|
| `available` | Boolean | True when an active callable resolved. |
| `display` | String | First-line display text for the active signature. |
| `name` | String | Callable name parsed from `display`. |
| `returnType` | String | Display return type, or empty. |
| `activeParameter` | Integer | 0-based active argument index. |
| `activeSignature` | Integer | 0-based active overload; currently always `0`. |
| `overloadCount` | Integer | Number of returned overload records. |
| `documentation` | String | Documentation for the active signature. |
| `source` | String | `zia` from the full service, or `unavailable` from the weak stub. |
| `parameters` | Seq | Active-signature parameter maps. |
| `overloads` | Seq | One signature map per overload. |

Each parameter map has `name`, `type`, and `documentation`. Each overload map
has `display`, `name`, `returnType`, `documentation`, and its own `parameters`
sequence.

### Hover Information Map

`HoverInfo*` and `SemanticJob.HoverInfo` return `available`, `display`,
`title`, `kind`, `name`, `type`, `documentation`, and `source`. A miss returns
the same map shape with `available = false` and empty display fields.

### Serialized Compatibility Formats

The String-returning methods are compatibility protocols:

- `Complete*`: `label<TAB>insertText<TAB>kind<TAB>detail<LF>`. Field content escapes
  backslash, tab, newline, and CR as `\\`, `\t`, `\n`, `\r`, so multiline snippets stay on
  one row; unescape after splitting on the literal delimiters.
- `Check*`: `severity<TAB>line<TAB>column<TAB>code<TAB>message<LF>`, with
  severity `0` error, `1` warning, and `2` note; line and column are 1-based.
- `Symbols*`: `name<TAB>kind<TAB>type<TAB>line<LF>`, with a 1-based source line. Only
  symbols declared in the active file are emitted.
- `SemanticJob.Tokens`: `line<TAB>start<TAB>end<TAB>kind<LF>`, using 0-based
  byte coordinates and an end-exclusive column.

Prefer the structured item, signature, hover, and Toolchain APIs for new code; the
String-returning forms remain for compatibility.

### Zia Example

```zia
module CompletionDemo;

bind Zanna.Zia.Completion as Completion;
bind Zanna.Collections.Map as Map;
bind Zanna.Collections.Seq as Seq;
bind Zanna.Terminal as Terminal;

func start() {
    var source = "module Demo;\nfu\n";
    var items = Completion.ItemsForFile(source, "demo.zia", 2, 2);
    if Seq.get_Count(items) > 0 {
        Terminal.Say(Map.GetStr(Seq.Get(items, 0), "label"));
    }
}
```

---

## Zanna.Zia.Toolchain

Structured diagnostics and compile results for Zia source buffers.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Check` | `Seq(String)` | Analyze source and return diagnostic maps. |
| `CheckForFile` | `Seq(String, String)` | Analyze source using the supplied path for diagnostics and relative `bind` resolution. |
| `BeginCheckForFile` | `SemanticJobHandle(String, String)` | Start the path-aware analysis on a semantic worker. |
| `Compile` | `Map(String)` | Compile source and return a result map. |
| `CompileForFile` | `Map(String, String)` | Compile source using the supplied path for diagnostics and relative `bind` resolution. |

### Diagnostic Map

`Check*` returns a `Seq` whose entries are `Zanna.Collections.Map` objects.
`Compile*` returns the same sequence under the `diagnostics` field.

| Key | Type | Description |
|-----|------|-------------|
| `file` | String | Normalized source path, or the requested path when no compiler file id is available. |
| `line` | Integer | 1-based start line, or 0 when unavailable. |
| `column` | Integer | 1-based start column, or 0 when unavailable. |
| `endLine` | Integer | 1-based end line, or the start line when no range is available. |
| `endColumn` | Integer | 1-based end column, or the start column when no range is available. |
| `severity` | Integer | `0` error, `1` warning, `2` note. |
| `severityName` | String | `error`, `warning`, or `note`. |
| `code` | String | Stable diagnostic code when one is available. |
| `message` | String | Human-readable diagnostic text. |
| `stage` | String | Compiler stage when available. |
| `help` | String | Extra help text when available. |
| `hasFixit` | Boolean | True when at least one machine-applicable fix is attached. |
| `fixits` | Seq | All fix-it maps; each has `message`, `replacement`, and 1-based `startLine`, `startColumn`, `endLine`, and `endColumn`. |
| `fixitMessage` | String | Compatibility copy of the first fix-it message, or empty. |
| `fixitReplacement` | String | Compatibility copy of the first replacement, or empty. |
| `fixitStartLine` / `fixitStartColumn` | Integer | Compatibility copy of the first fix-it start, or 0. |
| `fixitEndLine` / `fixitEndColumn` | Integer | Compatibility copy of the first fix-it end, or 0. |

`Check*`, `Compile*`, and diagnostics materialized from an asynchronous
`SemanticJob` all include the complete `fixits` sequence plus the legacy
first-fix fields, so the map schema does not depend on scheduling choice.

### Compile Result Map

`Compile` and `CompileForFile` return a `Zanna.Collections.Map` with:

| Key | Type | Description |
|-----|------|-------------|
| `success` | Boolean | True when compilation completed without error diagnostics. |
| `diagnostics` | Seq | `Zanna.Collections.Seq` of diagnostic maps. |
| `sourcePath` | String | Normalized primary source path. |
| `outputPath` | String | Reserved for future file-emitting compile flows. Currently empty. |
| `il` | String | Serialized IL when `success` is true; empty on failure. |

### Zia Example

```zia
module ToolingDemo;

bind Zanna.Zia.Toolchain as Toolchain;
bind Zanna.Collections.Map as Map;
bind Zanna.Collections.Seq as Seq;
bind Zanna.Terminal as Terminal;

func start() {
    var source = "module Demo;\nfunc f() -> Integer { return ; }\n";
    var result = Toolchain.CompileForFile(source, "demo.zia");

    if Map.GetBool(result, "success") == false {
        var diagnostics = Map.Get(result, "diagnostics");
        if Seq.get_Count(diagnostics) > 0 {
            var first = Seq.Get(diagnostics, 0);
            Terminal.Say(Map.GetStr(first, "file") + ":" + Map.GetInt(first, "line")
                + ": " + Map.GetStr(first, "message"));
        }
    }
}
```

### Notes

- Use `CheckForFile`, `BeginCheckForFile`, and `CompileForFile` from editors so relative `bind` paths resolve against the active document.
- The legacy `Zanna.Zia.Completion.CheckForFile` API still returns tab-delimited text for compatibility. New IDE surfaces should consume `Zanna.Zia.Toolchain` instead.
- At most two semantic workers run concurrently. A request made while both slots are occupied returns a completed job whose error is `semantic worker pool busy`.
- The weak runtime stub returns empty diagnostics, a null async-job handle, and a failed compile result with empty diagnostics, paths, and IL when the editor-service bridge is absent. Call `Zanna.Zia.Completion.IsAvailable` first: when it returns false, empty Check results mean "no analysis ran", not "clean".
- Statically linked IDE hosts must force-load `zia_editor_services` (which links `fe_zia`) so its strong `rt_zia_*` definitions override the weak runtime stubs. The repository's `zia` executable does this with the platform-equivalent whole-archive option.

---

## Zanna.Zia.Document

An incremental, process-wide source mirror for editor buffers. The mirror is
keyed by the exact path String supplied by the caller; unlike ProjectIndex,
Document does not normalize or canonicalize that key.

**Type:** Static utility class

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SyncFull` | `Void(String, String, Integer)` | Replace a path's mirror text and record the supplied revision. An empty path is ignored. |
| `SyncDelta` | `Boolean(String, String, Integer)` | Apply a batch of edit deltas sequentially and record `endRevision`. |
| `Close` | `Void(String)` | Remove the mirror for a path. |
| `Text` | `String(String)` | Return the mirrored bytes, or empty when the path is absent. |
| `Has` | `Boolean(String)` | True when a mirror exists for the path, even if its text is empty. |
| `CheckForFile` | `String(String)` | Run the legacy tab-delimited diagnostic check against mirrored text. |
| `BeginCheckForFile` | `SemanticJobHandle(String)` | Start a structured diagnostic job from mirrored text. |

`SyncDelta` consumes the compact JSON produced by
`Zanna.GUI.CodeEditor.TakeDeltas(sinceRevision)`:

```json
[
  {"r": 12, "sl": 3, "sc": 4, "el": 3, "ec": 7, "t": "new text"}
]
```

The start (`sl`, `sc`) and end (`el`, `ec`) coordinates are 0-based byte
positions, and the replaced range is half-open. Batches are applied in array
order, so each later range addresses the text produced by earlier entries.
Use the producer's JSON directly; this is a deliberately small parser, not a
general JSON interchange surface.

The safe editor loop is:

1. Call `SyncFull` on first use, after a path change, or after a cold edit.
2. Read `CodeEditor.Revision`, then call `TakeDeltas(lastSyncedRevision)`.
3. If `TakeDeltas` returns `"overflow"`, or `SyncDelta` returns false, recover
   with `SyncFull`.
4. Advance the caller's `lastSyncedRevision` only after a successful sync.
5. Call `Close` when the document is no longer needed.

`SyncDelta` validates its input: `endRevision` must not move the mirror backwards, each
delta's `r` must lie strictly after the mirror's stored revision and within `endRevision`
in increasing order, and out-of-range or reversed coordinates are rejected rather than
clamped. Any rejection returns `false`, which is the signal to recover with `SyncFull`.

`Text` returns empty for both an absent mirror and a present empty document; use `Has`
when the distinction matters. Synchronous `CheckForFile` returns empty for an absent
mirror, a clean document, or an unavailable weak bridge — combine
`Completion.IsAvailable` (bridge linked?) with `Has` (mirror exists?) to interpret an empty
result definitively, or use `BeginCheckForFile`, which returns null when no mirror exists.

### Zia Example

```zia
module DocumentDemo;

bind Zanna.Zia.Document as Document;
bind Zanna.Terminal as Terminal;

func start() {
    var path = "demo.zia";
    Document.SyncFull(path, "module Demo;\n", 1);

    var delta = "[{\"r\":2,\"sl\":1,\"sc\":0,\"el\":1,\"ec\":0,\"t\":\"func f() {}\"}]";
    if Document.SyncDelta(path, delta, 2) {
        Terminal.Say(Document.Text(path));
    }

    Document.Close(path);
}
```

---

## Zanna.Zia.SemanticJob

Pollable background language-service jobs returned by
`Zanna.Zia.Completion.Begin*ForFile` and `Zanna.Zia.Toolchain.BeginCheckForFile`.

**Type:** Static utility class operating on `SemanticJobHandle` objects

The registry exposes each handle parameter as generic `Object`; passing some
other object is treated like an invalid handle rather than rejected by the
method signature.

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `IsDone(job)` | `Boolean(Object)` | True when a valid background job has completed. See the null-handle caveat below. |
| `IsError(job)` | `Boolean(Object)` | True when a completed job has an error payload. |
| `Error(job)` | `String(Object)` | Error message for a failed job; `""` when no error is present. |
| `Kind(job)` | `Integer(Object)` | Numeric semantic job kind. |
| `Cancel(job)` | `Void(Object)` | Request cancellation; running work may finish later. |
| `CompletionItems(job)` | `Seq(Object)` | Materialize completion results for completion jobs. |
| `SignatureInfo(job)` | `Map(Object)` | Materialize signature-help results for signature jobs. |
| `HoverInfo(job)` | `Map(Object)` | Materialize hover results for hover jobs. |
| `Symbols(job)` | `String(Object)` | Materialize serialized symbol rows for symbol jobs. |
| `Tokens(job)` | `String(Object)` | Materialize serialized semantic-token rows for token jobs. |
| `Diagnostics(job)` | `Seq(Object)` | Materialize diagnostic maps for diagnostic jobs. |

`Error(job)` returns `""` for a job with no error, so pair it with `IsError(job)`
rather than testing the message for emptiness.

`Kind(job)` returns `0` unknown, `1` completion items, `2` signature info, `3`
hover info, `4` symbols, `5` diagnostics, or `6` semantic tokens. Result
materializers in the full service return an empty/default payload until the job
is complete, on error, or when called for the wrong job kind. `Cancel(job)`
marks the job cancelled; an already-running worker can continue to completion,
but its result is discarded. Letting the handle become unreachable also asks
for cancellation; no explicit destroy call is required.

Check for null immediately after every `Begin*` call. `IsDone(null)` is `false` in both the
full editor service and the weak stub, so never poll a null handle — it will never complete.
The other status methods return no error/kind for null.

Weak-stub materializers return compatibility payloads rather than uniformly
empty values: `CompletionItems(null)` contains one `source = "unavailable"`
row, and signature/hover maps use `source = "unavailable"`. The stub payloads
carry the full schema (including `cursorOffset = -1` and an empty `overloads`
Seq), so the structured map shape is link-configuration invariant; still, do
not materialize a null job unless you deliberately want a fallback payload.

---

## Zanna.Zia.ProjectIndex

Project-wide semantic navigation for Zia source buffers. The index stores
editor-supplied source text, including unsaved dirty buffers, and uses that text
when resolving `bind` imports.

**Type:** Static utility class returning an opaque
`Zanna.Zia.ProjectIndex.ProjectIndexHandle`

The public registry types the receiver parameters as generic `Object`; callers
must pass the handle returned by `New`. `IsValid` is the safe validity check.

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `New` | `ProjectIndexHandle(String)` | Create a project index rooted at the supplied project directory. |
| `IsValid` | `Boolean(Object)` | Return true when the handle still owns a native index. |
| `UpdateFile` | `Boolean(Object, String, String)` | Store current source text for a file path. This may be unsaved editor text. |
| `RemoveFile` | `Boolean(Object, String)` | Remove one file from the index; false when absent or invalid. |
| `Clear` | `Void(Object)` | Remove every indexed file. Invalid handles are ignored. |
| `Destroy` | `Void(Object)` | Dispose the native index payload. The handle becomes inert. |
| `Definition` | `Map(Object, String, String, Integer, Integer)` | Return a definition map for the identifier at `line`, `col`. |
| `References` | `Seq(Object, String, String, Integer, Integer)` | Return semantic reference maps. |
| `RenameEdits` | `Map(Object, String, String, Integer, Integer, String)` | Return workspace edits for a semantic rename without changing files. |

`Definition`, `References`, and `RenameEdits` also update the queried file with
the supplied source text before analysis. Cursor `line` is 1-based and cursor
`col` is 0-based, matching the existing completion APIs.

An empty root is treated as `.`. Other roots become lexically normalized
absolute paths. Relative file paths are resolved under that root; result paths
are normalized absolute paths except for the special `<editor>` virtual path.

### Definition Map

`Definition` returns a `Zanna.Collections.Map`.

| Key | Type | Description |
|-----|------|-------------|
| `found` | Boolean | True when a semantic definition was found. |
| `reason` | String | Miss reason: `not_found`, `invalid_index`, or `unavailable` from the weak stub. |
| `file` | String | On success, normalized source path for the definition. |
| `line` | Integer | On success, 1-based definition start line. |
| `column` | Integer | On success, 1-based definition start column. |
| `endLine` | Integer | On success, 1-based definition end line. |
| `endColumn` | Integer | On success, 1-based exclusive definition end column. |
| `editorLine` | Integer | On success, 0-based start line for `CodeEditor`. |
| `editorColumn` | Integer | On success, 0-based start column for `CodeEditor`. |
| `editorEndLine` | Integer | On success, 0-based end line for `CodeEditor`. |
| `editorEndColumn` | Integer | On success, 0-based exclusive end column for `CodeEditor`. |
| `name` | String | On success, source-visible symbol name. |
| `semanticName` | String | On success, internal semantic name used for comparison. |
| `kind` | String | On success, `variable`, `parameter`, `function`, `method`, `field`, `type`, or `module`. |
| `type` | String | On success, display type when known. |
| `ownerType` | String | On success, enclosing type for members when known. |

Miss maps contain only `found = false` and `reason`; the location and symbol
fields are success-only.

### Reference Map

`References` returns a `Seq` of maps with:

| Key | Type | Description |
|-----|------|-------------|
| `file` | String | Normalized source path containing the reference. |
| `line` / `column` | Integer | 1-based reference start. |
| `endLine` / `endColumn` | Integer | 1-based exclusive reference end. |
| `editorLine` / `editorColumn` | Integer | 0-based reference start for editor operations. |
| `editorEndLine` / `editorEndColumn` | Integer | 0-based exclusive reference end for editor operations. |
| `name` | String | Source-visible symbol name. |
| `semanticName` | String | Internal semantic name. |
| `kind` | String | Symbol kind. |
| `isDefinition` | Boolean | True when the reference is the declaration token. |

Reference lookup is semantic, not string search: it resolves identifiers through
the Zia analyzer, ignores comments and string literals, and separates shadowed
locals from globals/imports.

### Rename Result Map

`RenameEdits` returns a `Zanna.Collections.Map`.

| Key | Type | Description |
|-----|------|-------------|
| `success` | Boolean | True when edits were produced. |
| `reason` | String | Empty on success; otherwise `invalid_index`, `invalid_name`, `not_found`, `collision`, or weak-stub `unavailable`. |
| `name` | String | Original source-visible name; present on success only. |
| `newName` | String | Requested replacement; present on success only. |
| `references` | Seq | Reference maps used to build the edit set; present on success only. |
| `edits` | Seq | Workspace edit maps; present on success and failure. |

Each edit map contains `file`, `startLine`, `startColumn`, `endLine`,
`endColumn`, `editorStartLine`, `editorStartColumn`, `editorEndLine`,
`editorEndColumn`, and `newText`. Rename only returns edits; callers must apply
them to editor buffers or files themselves.

### ProjectIndex Example

```zia
module ProjectIndexDemo;

bind Zanna.Zia.ProjectIndex as ProjectIndex;
bind Zanna.Collections.Map as Map;
bind Zanna.Terminal as Terminal;

func start() {
    var index = ProjectIndex.New(".");
    var source = "module Demo;\nfunc start() { var answer = 42; answer; }\n";
    ProjectIndex.UpdateFile(index, "demo.zia", source);

    var definition = ProjectIndex.Definition(index, "demo.zia", source, 2, 37);
    if Map.GetBool(definition, "found") {
        Terminal.Say(Map.GetStr(definition, "file") + ":" + Map.GetInt(definition, "line"));
    }

    ProjectIndex.Destroy(index);
}
```

### Notes

- Always call `UpdateFile` when a buffer changes. Query calls also accept current source to cover the active dirty buffer.
- Add all open project files to the index for best reference and rename results. Files absent from the index can still be resolved from disk through normal `bind` loading, but they are not scanned for references.
- `References` returns an empty Seq for both an invalid index and an unresolved cursor; it does not return a reason field.
- Rename validates the replacement with the Zia lexer's exact identifier rules: ASCII letters/digits/underscore (no locale sensitivity), a non-keyword, and at most 1,024 bytes. A successful call therefore always produces lexer-valid output — but it still only returns edits; it never changes indexed buffers or files.
- Reference rows are ordered by normalized indexed path and then source order. ProjectIndex instances have no internal lock; serialize updates and queries for a given handle.
- `Destroy` is optional when ordinary garbage collection owns the handle, but it can release the native index early. Repeated `Destroy`, `Clear`, and invalid-handle calls are safe no-ops.
- Native IDE builds must link and force-load `zia_editor_services`; the weak runtime stub returns an invalid handle and empty or `unavailable` results.

---

## See Also

- [Collections](collections/README.md) - `Map` and `Seq` result containers
- [Diagnostics](diagnostics.md) - runtime assertion and trap helpers
- [Generated Zia surface](../generated/runtime/zia.md) - registry-derived signatures and runtime symbols
- [Documentation review findings](../../misc/reviews/documentation-review-findings.md) - open Zia tooling defects and inconsistencies
