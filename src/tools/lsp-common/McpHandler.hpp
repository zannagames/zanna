//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the MCP lifecycle, tool catalog, and compiler-bridge
///        dispatch handler shared by Zanna language servers.
///
/// McpHandler implements the 2024-11-05 protocol handshake and exposes
/// language-prefixed compiler and runtime inspection tools. It borrows its
/// compiler bridge, copies configuration strings, and returns no response for
/// JSON-RPC notifications.
///
/// @see ICompilerBridge.hpp
/// @see JsonRpc.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tools/lsp-common/ICompilerBridge.hpp"
#include "tools/lsp-common/Json.hpp"
#include "tools/lsp-common/JsonRpc.hpp"

#include <optional>
#include <string>

namespace zanna::server {

/// @brief MCP protocol handler.
///
/// Handles the MCP lifecycle methods (initialize, initialized) and dispatches
/// tools/list and tools/call requests to the ICompilerBridge.
///
/// ## MCP Lifecycle
///
/// 1. Client sends `initialize` → server returns capabilities + protocol version
/// 2. Client sends `initialized` (notification) → no response
/// 3. Client sends `tools/list` → server returns tool definitions
/// 4. Client sends `tools/call` → server dispatches to ICompilerBridge, returns result
///
/// The bridge reference must remain valid for the handler's lifetime.
class McpHandler {
  public:
    /// @brief Construct a handler backed by a language-specific compiler bridge.
    /// @param bridge Compiler bridge borrowed for this handler's lifetime.
    /// @param config Server and tool naming configuration copied into the handler.
    McpHandler(ICompilerBridge &bridge, const ServerConfig &config);

    /// @brief Handle a JSON-RPC request and return a response string.
    /// @param req Parsed JSON-RPC request or notification.
    /// @return Serialized response, or an empty string for notifications.
    std::string handleRequest(const JsonRpcRequest &req);

  private:
    ICompilerBridge &bridge_;         ///< Borrowed compiler and runtime adapter.
    ServerConfig config_;            ///< Owned language and tool naming settings.
    bool initializeResponded_{false}; ///< Whether initialize produced a response.
    bool initialized_{false};         ///< Whether the client sent initialized.

    /// @brief Handle `initialize`: return protocol version and server capabilities.
    /// @param req Initialize request whose ID is echoed.
    /// @return Serialized initialization response.
    std::string handleInitialize(const JsonRpcRequest &req);

    /// @brief Handle `tools/list`: return the available tool definitions.
    /// @param req Tools-list request whose ID is echoed.
    /// @return Serialized tool-catalog response.
    std::string handleToolsList(const JsonRpcRequest &req);

    /// @brief Handle `tools/call`: validate args and dispatch to the matching callXxx.
    /// @param req Tool-call request containing name and arguments.
    /// @return Serialized tool result or JSON-RPC error.
    std::string handleToolsCall(const JsonRpcRequest &req);

    /// @brief Result payload for one MCP tool call.
    /// @details `content` is always populated for compatibility with clients that
    ///          only display text blocks. `structuredContent` is populated when the
    ///          tool's natural result is JSON data that MCP-aware agents can consume
    ///          without reparsing a text string.
    struct ToolCallResult {
        JsonValue content;                          ///< MCP `content` array.
        std::optional<JsonValue> structuredContent; ///< Optional MCP structured payload.
    };

    /// @brief Run source checking and return structured diagnostics.
    /// @param args Validated source arguments.
    /// @return Text and structured diagnostic content.
    ToolCallResult callCheck(const JsonValue &args);

    /// @brief Compile source and return status plus diagnostics.
    /// @param args Validated source arguments.
    /// @return Text and structured compilation content.
    ToolCallResult callCompile(const JsonValue &args);

    /// @brief Query completions at a source position.
    /// @param args Validated source and position arguments.
    /// @return Text and structured completion content.
    ToolCallResult callCompletions(const JsonValue &args);

    /// @brief Query hover information at a source position.
    /// @param args Validated source and position arguments.
    /// @return Text content containing hover information.
    ToolCallResult callHover(const JsonValue &args);

    /// @brief Enumerate top-level source symbols.
    /// @param args Validated source arguments.
    /// @return Text and structured symbol content.
    ToolCallResult callSymbols(const JsonValue &args);

    /// @brief Return an intermediate-language dump.
    /// @param args Validated source and optimization arguments.
    /// @return Text content containing the IL dump.
    ToolCallResult callDumpIL(const JsonValue &args);

    /// @brief Return an abstract-syntax-tree dump.
    /// @param args Validated source arguments.
    /// @return Text content containing the AST dump.
    ToolCallResult callDumpAst(const JsonValue &args);

    /// @brief Return a lexical-token dump.
    /// @param args Validated source arguments.
    /// @return Text content containing the token dump.
    ToolCallResult callDumpTokens(const JsonValue &args);

    /// @brief Enumerate runtime classes.
    /// @param args Validated empty arguments object.
    /// @return Text and structured class content.
    ToolCallResult callRuntimeClasses(const JsonValue &args);

    /// @brief Enumerate members of a runtime class.
    /// @param args Validated class-name arguments.
    /// @return Text and structured member content.
    ToolCallResult callRuntimeMembers(const JsonValue &args);

    /// @brief Search runtime APIs by keyword.
    /// @param args Validated keyword arguments.
    /// @return Text and structured search-result content.
    ToolCallResult callRuntimeSearch(const JsonValue &args);

    /// @brief Build the tool definitions for tools/list response.
    /// @return Ordered array of configured MCP tool definitions.
    JsonValue buildToolDefinitions() const;
};

} // namespace zanna::server
