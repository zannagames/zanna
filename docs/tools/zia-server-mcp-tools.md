---
status: active
audience: public
last-verified: 2026-07-26
---

# Zia Server — MCP Tool Specification

Detailed specification for each MCP tool exposed by `zia-server --mcp`. The
same schemas are exposed by `zbasic-server --mcp` with `basic/` substituted for
the `zia/` tool-name prefix. This document is intended for AI agents and MCP
client implementors.

`zia-server --lsp` exposes additional editor-only features (`definition`, `references`,
`rename`, `signatureHelp`, `workspace/symbol`, and `semanticTokens/full`). MCP remains
the 11 source/runtime tools listed here; use [Zia Language Server Reference](zia-server.md)
for the LSP capability matrix.

All tools are invoked via JSON-RPC 2.0 `tools/call`:

```json
{"jsonrpc":"2.0","id":N,"method":"tools/call","params":{"name":"TOOL_NAME","arguments":{...}}}
```

Responses follow the MCP content format:

```json
{"jsonrpc":"2.0","id":N,"result":{"content":[{"type":"text","text":"RESULT_STRING"}]}}
```

---

## zia/check

Type-check Zia source code and return diagnostics (no code generation).

**Input Schema:**
```json
{
  "type": "object",
  "properties": {
    "source": {"type": "string", "description": "Zia source code"},
    "path": {"type": "string", "description": "Virtual file path (optional)"}
  },
  "required": ["source"]
}
```

**Result:** JSON array of diagnostics. Each diagnostic has:
- `severity`: 0 (note), 1 (warning), 2 (error)
- `message`: Human-readable message
- `line`: 1-based line number
- `column`: 1-based column number
- `code`: Warning/error code (e.g., `"W001"`, `"V-ZIA-UNDEFINED"`)
- `endLine` / `endColumn`: 1-based exclusive end of the underlined range; `0` when no concrete range is known
- `stage`: pipeline stage that emitted the diagnostic (`parse`, `sema`, `lower`, `verify`, ...); may be empty
- `help`: optional help text for the code; may be empty
- `notes`: array of related locations `{message, file, line, column}` (e.g., "previous definition is here")
- `fixits`: array of machine-applicable replacements `{message, replacement, line, column, endLine, endColumn}` over half-open 1-based ranges; `endLine`/`endColumn` of `0` means insertion at the start position. Did-you-mean suggestions for undefined identifiers arrive here.

Use `zanna explain <code>` (or `zanna --print-error-codes --json`) to resolve diagnostic codes to descriptions.

**Example:**
```jsonl
→ {"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"zia/check","arguments":{"source":"module T;\nfunc start() { var x = unknown; }"}}}
← {"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"[{\"severity\":2,\"message\":\"undeclared identifier 'unknown'\",\"line\":2,\"column\":26,\"code\":\"\"}]"}]}}
```

---

## zia/compile

Full compilation including IL code generation. Returns success status and diagnostics.

**Input Schema:** Same as `zia/check`.

**Result:** JSON object:
- `succeeded`: boolean
- `diagnostics`: array (same format as `zia/check`)

**Example:**
```jsonl
→ {"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"zia/compile","arguments":{"source":"module T;\nfunc start() { Zanna.Terminal.Say(\"hi\"); }"}}}
← {"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"succeeded\":true,\"diagnostics\":[]}"}]}}
```

---

## zia/completions

Get code completions at a cursor position.

**Input Schema:**
```json
{
  "type": "object",
  "properties": {
    "source": {"type": "string", "description": "Zia source code"},
    "line": {"type": "integer", "description": "Cursor line (1-based)"},
    "col": {"type": "integer", "description": "Cursor column (1-based)"},
    "path": {"type": "string", "description": "Virtual file path (optional)"}
  },
  "required": ["source", "line", "col"]
}
```

**Result:** JSON object with a `completions` array. Each completion item has:
- `label`: Display text
- `insertText`: Text to insert
- `kind`: Completion kind integer (0=Keyword, 1=Snippet, 2=Variable, 3=Parameter, 4=Field, 5=Method, 6=Function, 7=Entity, 8=Value, 9=Interface, 10=Module, 11=RuntimeClass, 12=Property)
- `detail`: Type or signature detail
- `documentation`: Optional Markdown documentation. Runtime-class descriptions
  come from the canonical runtime definition registry.

---

## zia/hover

Get type information for the symbol at a cursor position.

**Input Schema:** Same as `zia/completions`.

**Result:** Markdown type/signature information, including authored runtime-class
documentation when applicable, or `"(no type information)"` if nothing is found.

---

## zia/symbols

List all top-level declarations in Zia source.

**Input Schema:** Same as `zia/check`.

**Result:** JSON array of symbols:
- `name`: Symbol name
- `kind`: `"variable"`, `"function"`, `"method"`, `"field"`, `"type"`, `"module"`, `"parameter"`
- `type`: Type as string (e.g., `"Integer"`, `"() -> Void"`)

---

## zia/dump-il

Dump the compiled IL (intermediate language).

**Input Schema:**
```json
{
  "type": "object",
  "properties": {
    "source": {"type": "string", "description": "Zia source code"},
    "path": {"type": "string", "description": "Virtual file path (optional)"},
    "optimized": {"type": "boolean", "description": "Apply O1 optimization (default: false)"}
  },
  "required": ["source"]
}
```

**Result:** IL text output, or error message prefixed with `"Compilation failed:"`.

---

## zia/dump-ast

Dump the abstract syntax tree.

**Input Schema:** Same as `zia/check`.

**Result:** Indented AST text representation.

---

## zia/dump-tokens

Dump the token stream.

**Input Schema:** Same as `zia/check`.

**Result:** Tab-separated token list, one per line: `line:col\ttext`.

---

## zia/runtime-classes

List all Zanna runtime classes with member counts and canonical documentation.

**Input Schema:** Empty object `{}`.

**Result:** JSON object with a `classes` array. Each class has:
- `qname`: Fully qualified name (e.g., `"Zanna.Terminal"`)
- `propertyCount`: Number of properties
- `methodCount`: Number of methods
- `documentation`: `{summary, details, format}` where `format` is `"markdown"`

---

## zia/runtime-methods

List methods and properties for a specific runtime class.

**Input Schema:**
```json
{
  "type": "object",
  "properties": {
    "className": {"type": "string", "description": "Fully qualified class name (e.g., \"Zanna.Terminal\")"}
  },
  "required": ["className"]
}
```

**Result:** JSON array:
- `name`: Member name
- `memberKind`: `"method"` or `"property"`
- `signature`: Method signature or property type string

Returns empty array if class not found.

---

## zia/runtime-search

Search Zanna runtime APIs by keyword (case-insensitive substring match).

**Input Schema:**
```json
{
  "type": "object",
  "properties": {
    "keyword": {"type": "string", "description": "Search keyword"}
  },
  "required": ["keyword"]
}
```

**Result:** JSON array of matching entries (same format as `zia/runtime-methods`), including class names that match. The `name` field uses dotted notation (e.g., `"Zanna.Terminal.Say"`).

---

## Error Responses

Tools return standard JSON-RPC 2.0 errors:

| Code | Meaning |
|------|---------|
| -32700 | Parse error (malformed JSON) |
| -32600 | Invalid request (missing method) |
| -32601 | Method/tool not found |
| -32602 | Invalid parameters (missing required argument) |
| -32603 | Internal error (compiler crash) |
