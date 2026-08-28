//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/lsp-common/ICompilerBridge.hpp
// Purpose: Abstract interface for language server compiler bridges and
//          configuration for parameterizing shared handlers.
// Key invariants:
//   - All methods are pure virtual except runtime queries (shared default impl)
//   - ServerConfig parameterizes handler strings (names, prefixes, extensions)
// Ownership/Lifetime:
//   - Interface only; implementations own their compiler resources
// Links: tools/lsp-common/ServerTypes.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Declares the protocol-agnostic compiler bridge used by LSP and MCP.

#include "tools/lsp-common/ServerTypes.hpp"

#include <optional>
#include <string>
#include <vector>

namespace zanna::server {

/// @brief Configuration for parameterizing shared LSP/MCP handlers.
struct ServerConfig {
    std::string serverName; ///< "zia-server" or "zbasic-server"
    std::string version;    ///< "0.1.0"
    std::string sourceName; ///< "zia" or "zbasic" (LSP diagnostic source)
    std::string toolPrefix; ///< "zia" or "basic" (MCP tool name prefix)
    std::string defaultExt; ///< ".zia" or ".bas"
    std::string langLabel;  ///< "Zia" or "Zanna BASIC" (for tool descriptions)
};

/// @brief Abstract interface for protocol-agnostic compiler facades.
///
/// Both Zia and BASIC language servers implement this interface to provide
/// compilation, IDE features, and runtime queries through the shared
/// LSP and MCP handlers.
class ICompilerBridge {
  public:
    virtual ~ICompilerBridge() = default;

    // ── Analysis ──

    /// @brief Type-check source, return diagnostics (no codegen).
    /// @param source Current full document text.
    /// @param path Document path used for diagnostics/imports.
    /// @return Diagnostics in compiler emission order.
    virtual std::vector<DiagnosticInfo> check(const std::string &source,
                                              const std::string &path) = 0;

    /// @brief Full compilation, return success + diagnostics.
    /// @param source Current full document text.
    /// @param path Document path used for diagnostics/imports.
    /// @return Compilation status, diagnostics, and generated output metadata.
    virtual CompileResult compile(const std::string &source, const std::string &path) = 0;

    // ── IDE Features ──

    /// @brief Notify the bridge that a document is open/current in the editor.
    /// @param path Document path.
    /// @param source Current full document text.
    virtual void updateDocument(const std::string &path, const std::string &source);

    /// @brief Notify the bridge that a document has closed.
    /// @param path Document path to evict.
    virtual void removeDocument(const std::string &path);

    /// @brief Get completions at (line, col) in source.
    /// @param source Current full document text.
    /// @param line Zero-based line.
    /// @param col Zero-based character offset.
    /// @param path Document path.
    /// @return Completion candidates.
    virtual std::vector<CompletionInfo> completions(const std::string &source,
                                                    int line,
                                                    int col,
                                                    const std::string &path) = 0;

    /// @brief Get type info for the symbol at (line, col).
    /// @param source Current full document text.
    /// @param line Zero-based line.
    /// @param col Zero-based character offset.
    /// @param path Document path.
    /// @return Rendered hover content, or empty when unavailable.
    virtual std::string hover(const std::string &source,
                              int line,
                              int col,
                              const std::string &path) = 0;

    /// @brief List all top-level declarations in source.
    /// @param source Current full document text.
    /// @param path Document path.
    /// @return Top-level symbols in source order.
    virtual std::vector<SymbolInfo> symbols(const std::string &source, const std::string &path) = 0;

    /// @brief Whether the bridge can answer go-to-definition requests.
    /// @return true when definition() is implemented.
    virtual bool supportsDefinition() const;

    /// @brief Whether the bridge can answer references requests.
    /// @return true when references() is implemented.
    virtual bool supportsReferences() const;

    /// @brief Whether the bridge can compute semantic rename edits.
    /// @return true when rename() is implemented.
    virtual bool supportsRename() const;

    /// @brief Whether the bridge can answer signature-help requests.
    /// @return true when signatureHelp() is implemented.
    virtual bool supportsSignatureHelp() const;

    /// @brief Whether the bridge can list workspace symbols.
    /// @return true when workspaceSymbols() is implemented.
    virtual bool supportsWorkspaceSymbols() const;

    /// @brief Whether the bridge can provide semantic tokens.
    /// @return true when semanticTokens() is implemented.
    virtual bool supportsSemanticTokens() const;

    /// @brief Resolve the definition for the symbol at (line, col).
    /// @param source Current source text.
    /// @param line Zero-based line.
    /// @param col Zero-based character offset.
    /// @param path Document path.
    /// @return Definition location, or `std::nullopt`.
    virtual std::optional<LocationInfo> definition(const std::string &source,
                                                   int line,
                                                   int col,
                                                   const std::string &path);

    /// @brief Resolve all known references for the symbol at (line, col).
    /// @param source Current source text.
    /// @param line Zero-based line.
    /// @param col Zero-based character offset.
    /// @param path Document path.
    /// @return Reference locations.
    virtual std::vector<LocationInfo> references(const std::string &source,
                                                 int line,
                                                 int col,
                                                 const std::string &path);

    /// @brief Compute workspace edits for renaming the symbol at (line, col).
    /// @param source Current source text.
    /// @param line Zero-based line.
    /// @param col Zero-based character offset.
    /// @param path Document path.
    /// @param newName Requested identifier spelling.
    /// @return Rename edits and any rejection reason.
    virtual RenameResult rename(const std::string &source,
                                int line,
                                int col,
                                const std::string &path,
                                const std::string &newName);

    /// @brief Return signature help for the active call at (line, col).
    /// @param source Current source text.
    /// @param line Zero-based line.
    /// @param col Zero-based character offset.
    /// @param path Document path.
    /// @return Active signatures and parameter index.
    virtual SignatureHelpInfo signatureHelp(const std::string &source,
                                            int line,
                                            int col,
                                            const std::string &path);

    /// @brief Search symbols across indexed/open workspace documents.
    /// @param query Case/implementation-defined search query.
    /// @return Matching workspace symbols.
    virtual std::vector<SymbolInfo> workspaceSymbols(const std::string &query);

    /// @brief Return semantic tokens for the supplied source.
    /// @param source Current source text.
    /// @param path Document path.
    /// @return Semantic token spans in source order.
    virtual std::vector<SemanticTokenInfo> semanticTokens(const std::string &source,
                                                          const std::string &path);

    // ── Dump ──

    /// @brief Dump IL for source. If optimized, applies O1 optimization.
    /// @param source Current source text.
    /// @param path Document path.
    /// @param optimized Whether to apply O1 before printing.
    /// @return Textual IL or a bridge-specific diagnostic representation.
    virtual std::string dumpIL(const std::string &source,
                               const std::string &path,
                               bool optimized) = 0;

    /// @brief Dump AST for source.
    /// @param source Current source text.
    /// @param path Document path.
    /// @return Frontend AST dump.
    virtual std::string dumpAst(const std::string &source, const std::string &path) = 0;

    /// @brief Dump token stream for source.
    /// @param source Current source text.
    /// @param path Document path.
    /// @return Frontend token dump.
    virtual std::string dumpTokens(const std::string &source, const std::string &path) = 0;

    // ── Runtime queries (shared default implementations) ──

    /// @brief List all runtime classes with member counts.
    /// @return Runtime class summaries in catalog order.
    virtual std::vector<RuntimeClassSummary> runtimeClasses();

    /// @brief List methods and properties for a runtime class.
    /// @param className Fully qualified runtime class name.
    /// @return Runtime members, or empty for an unknown class.
    virtual std::vector<RuntimeMemberInfo> runtimeMembers(const std::string &className);

    /// @brief Search runtime APIs by keyword (case-insensitive substring match).
    /// @param keyword Non-empty name substring.
    /// @return Matching classes, methods, and properties.
    virtual std::vector<RuntimeMemberInfo> runtimeSearch(const std::string &keyword);
};

} // namespace zanna::server
