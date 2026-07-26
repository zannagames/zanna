//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the reusable LSP 3.17 lifecycle and editor-feature handler.
///
/// LspHandler combines an owned DocumentStore and copied ServerConfig with
/// borrowed compiler and transport adapters. It negotiates bridge-supported
/// capabilities, enforces protocol lifecycle state, and publishes diagnostics
/// for full-text document synchronization.
///
/// @see ICompilerBridge.hpp
/// @see DocumentStore.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tools/lsp-common/DocumentStore.hpp"
#include "tools/lsp-common/ICompilerBridge.hpp"
#include "tools/lsp-common/Json.hpp"
#include "tools/lsp-common/JsonRpc.hpp"

#include <string>

namespace zanna::server {

class Transport;

/// @brief Language-neutral LSP lifecycle and request dispatcher.
///
/// Handles the LSP lifecycle (initialize, shutdown, exit) and request/notification
/// dispatch for editor features (completion, hover, diagnostics, document symbols).
///
/// Parameterized via ICompilerBridge (language-specific compiler) and ServerConfig
/// (language-specific strings: server name, diagnostic source, etc.).
/// The referenced bridge and transport must outlive the handler.
class LspHandler {
  public:
    /// @brief Construct a handler backed by language and transport adapters.
    /// @param bridge Compiler bridge borrowed for this handler's lifetime.
    /// @param transport Notification transport borrowed for this handler's lifetime.
    /// @param config Language-specific server strings copied into the handler.
    LspHandler(ICompilerBridge &bridge, Transport &transport, const ServerConfig &config);

    /// @brief Dispatch one parsed JSON-RPC request or notification.
    /// @param req Validated message to process through the LSP lifecycle.
    /// @return Serialized JSON-RPC response, or an empty string when no response
    ///         is permitted or required.
    std::string handleRequest(const JsonRpcRequest &req);

    /// @brief Test whether shutdown has been requested.
    /// @return @c true after processing the LSP @c shutdown method.
    [[nodiscard]] bool shutdownRequested() const {
        return shutdownRequested_;
    }

  private:
    ICompilerBridge &bridge_;       ///< Borrowed language-specific compiler adapter.
    Transport &transport_;         ///< Borrowed outbound protocol transport.
    ServerConfig config_;          ///< Owned language-specific server metadata.
    DocumentStore store_;          ///< Owned open-document contents and versions.
    bool initializeResponded_{false}; ///< Whether an initialize response was sent.
    bool clientInitialized_{false};   ///< Whether the initialized notification arrived.
    bool shutdownRequested_{false};   ///< Whether shutdown was acknowledged.

    /// @brief Handle `initialize`: advertise server capabilities to the client.
    /// @param req Initialize request whose ID is echoed.
    /// @return Serialized capability response or lifecycle error.
    std::string handleInitialize(const JsonRpcRequest &req);

    /// @brief Handle `shutdown`: mark the server for exit and acknowledge.
    /// @param req Shutdown request whose ID is echoed.
    /// @return Serialized null success response.
    std::string handleShutdown(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/didOpen`: store the document and publish diagnostics.
    /// @param req Full-text open notification.
    void handleDidOpen(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/didChange`: update the document and re-diagnose.
    /// @param req Full-text change notification.
    void handleDidChange(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/didClose`: drop the document from the store.
    /// @param req Close notification identifying the document URI.
    void handleDidClose(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/completion`: return completion items at a position.
    /// @param req Completion request.
    /// @return Serialized completion or error response.
    std::string handleCompletion(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/hover`: return hover text for the symbol at a position.
    /// @param req Hover request.
    /// @return Serialized hover, null, or error response.
    std::string handleHover(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/documentSymbol`: list the document's symbols.
    /// @param req Document-symbol request.
    /// @return Serialized symbol-array or error response.
    std::string handleDocumentSymbol(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/definition`: return the target symbol location.
    /// @param req Definition request.
    /// @return Serialized location, null, or error response.
    std::string handleDefinition(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/references`: return known references for a symbol.
    /// @param req References request and declaration-inclusion context.
    /// @return Serialized location-array or error response.
    std::string handleReferences(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/rename`: return a workspace edit for semantic rename.
    /// @param req Rename request including the replacement identifier.
    /// @return Serialized workspace edit, null, or error response.
    std::string handleRename(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/signatureHelp`: return call signature help.
    /// @param req Signature-help request.
    /// @return Serialized signature data, null, or error response.
    std::string handleSignatureHelp(const JsonRpcRequest &req);

    /// @brief Handle `workspace/symbol`: return indexed workspace symbols.
    /// @param req Workspace-symbol query request.
    /// @return Serialized symbol-array or error response.
    std::string handleWorkspaceSymbol(const JsonRpcRequest &req);

    /// @brief Handle `textDocument/semanticTokens/full`: return full semantic tokens.
    /// @param req Full semantic-token request.
    /// @return Serialized delta-encoded token data or error response.
    std::string handleSemanticTokensFull(const JsonRpcRequest &req);

    /// @brief Compile the document for @p uri and publish its diagnostics notification.
    /// @param uri Open document URI used for lookup and notification routing.
    void publishDiagnostics(const std::string &uri);

    /// @brief Send a best-effort `window/logMessage` notification to the client.
    /// @param type LSP MessageType value: 1 error, 2 warning, 3 info, 4 log.
    /// @param message Human-readable text for the editor log.
    void logMessage(int type, const std::string &message);

    /// @brief Map CompletionInfo kind → LSP CompletionItemKind.
    /// @param kind Compiler-bridge completion-kind ordinal.
    /// @return LSP CompletionItemKind integer, defaulting to Text.
    static int completionKindToLsp(int kind);

    /// @brief Map SymbolInfo kind string → LSP SymbolKind.
    /// @param kind Lowercase bridge symbol-kind name.
    /// @return LSP SymbolKind integer, defaulting to Variable.
    static int symbolKindToLsp(const std::string &kind);
};

} // namespace zanna::server
