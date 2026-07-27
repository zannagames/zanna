---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Zia Language Server

Source files for `zia-server`, the dual-protocol language server for Zia (`src/tools/zia-server/`).

## Overview

- **Total source files**: 3 (.hpp/.cpp) in `zia-server/` + 19 shared in `lsp-common/`
- **Architecture**: 3 layers — Transport/JSON-RPC → Protocol Handlers → Compiler Bridge
- **Protocols**: MCP (newline-delimited) and LSP (Content-Length framed)

## Source Files (`src/tools/zia-server/`)

The foundation layer (JSON, transports, protocol handlers, document store) was extracted to
`src/tools/lsp-common/` for reuse by `vbasic-server`. See [codemap/tools.md](tools.md) for the
shared files.

### Zia-Specific Files

| File | Purpose |
|------|---------|
| `CompilerBridge.hpp` | Zia-specific compiler bridge (wraps `fe_zia` APIs) |
| `CompilerBridge.cpp` | Analysis, completion, IL dump, runtime queries |
| `main.cpp` | CLI parsing, auto-detection, event loop for MCP/LSP |

## Dependencies

```text
zia-server → zia_server_lib → fe_zia, zia_editor_services, il_full, il_transform
                             → zanna_support (diagnostics, source manager)
                             → il_runtime (RuntimeClasses, RuntimeRegistry)
```

## Data Flow

```text
stdin → Transport.readMessage()
      → JsonValue::parse()
      → parseRequest() → JsonRpcRequest
      → McpHandler/LspHandler.handleRequest()
      → CompilerBridge.method()
      → fe_zia API (parseAndAnalyze, compile, CompletionEngine)
        or zia_editor_services (ProjectIndex-backed LSP navigation/rename)
      → response string
      → Transport.writeMessage() → stdout
```

## Test Files

| File | Tests | Purpose |
|------|-------|---------|
| `test_json.cpp` | 60 | JSON parser/emitter: types, nesting, escapes, round-trip |
| `test_transport.cpp` | 18 | MCP/LSP framing, JSON-RPC parse/build |
| `test_compiler_bridge.cpp` | 24 | Bridge methods against real Zia snippets |
| `test_mcp_handler.cpp` | 18 | MCP lifecycle + tool dispatch integration |
| `test_lsp_handler.cpp` | 20 | LSP lifecycle + feature handler integration |
