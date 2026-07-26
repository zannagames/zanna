//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the protocol-neutral Zia compiler and editor-service bridge.
/// @details Exposes compiler functionality through owned transport-friendly
///          records while isolating callers from frontend and runtime internals.
// Key invariants:
//   - All methods are thread-safe (each creates fresh SourceManager/DiagnosticEngine)
//   - CompletionEngine is long-lived for LRU cache benefits
//   - Results are returned as simple structs, not compiler-internal types
// Ownership/Lifetime:
//   - CompilerBridge owns the CompletionEngine
//   - All returned data is fully owned (no dangling pointers)
// Links: frontends/zia/ZiaAnalysis.hpp, frontends/zia/ZiaCompletion.hpp,
//        frontends/zia/Compiler.hpp, tools/lsp-common/ICompilerBridge.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tools/lsp-common/ICompilerBridge.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace il::frontends::zia {
class CompletionEngine;
}

namespace zanna::server {

/// @brief Protocol-agnostic facade wrapping Zia compiler APIs.
///
/// Each method creates a fresh SourceManager per call for isolation.
/// The CompletionEngine is shared across calls to benefit from its LRU cache.
class CompilerBridge : public ICompilerBridge {
  public:
    /// @brief Construct the persistent completion engine and project index.
    CompilerBridge();

    /// @brief Release all persistent compiler-bridge resources.
    ~CompilerBridge() override;

    // ── Analysis ──

    /// @brief Parse and semantically analyze a document.
    /// @param source Complete Zia document text.
    /// @param path Logical document path used for locations.
    /// @return Owned frontend diagnostics.
    std::vector<DiagnosticInfo> check(const std::string &source, const std::string &path) override;

    /// @brief Compile a document through IL lowering and verification.
    /// @param source Complete Zia document text.
    /// @param path Logical document path used for locations.
    /// @return Compilation success and owned diagnostics.
    CompileResult compile(const std::string &source, const std::string &path) override;

    // ── IDE Features ──

    /// @brief Add or replace an open document in workspace state.
    /// @param path Document path or editor URI.
    /// @param source Complete current document text.
    void updateDocument(const std::string &path, const std::string &source) override;

    /// @brief Remove an open document from workspace state.
    /// @param path Document path or editor URI to remove.
    void removeDocument(const std::string &path) override;

    /// @brief Compute completion candidates at a cursor position.
    /// @param source Complete current document text.
    /// @param line One-based cursor line.
    /// @param col One-based cursor column.
    /// @param path Document path or editor URI.
    /// @return Owning completion records.
    std::vector<CompletionInfo> completions(const std::string &source,
                                            int line,
                                            int col,
                                            const std::string &path) override;

    /// @brief Produce Markdown hover information at a cursor position.
    /// @param source Complete current document text.
    /// @param line One-based cursor line.
    /// @param col One-based cursor column.
    /// @param path Document path or editor URI.
    /// @return Markdown payload, or an empty string when unresolved.
    std::string hover(const std::string &source,
                      int line,
                      int col,
                      const std::string &path) override;

    /// @brief Enumerate declarations owned by one document.
    /// @param source Complete current document text.
    /// @param path Document path or editor URI.
    /// @return Document symbol records with declaration positions.
    std::vector<SymbolInfo> symbols(const std::string &source, const std::string &path) override;

    /// @brief Report definition-lookup availability.
    /// @return true when a runtime project index exists.
    bool supportsDefinition() const override;

    /// @brief Report reference-lookup availability.
    /// @return true when reference requests are exposed.
    bool supportsReferences() const override;

    /// @brief Report semantic-rename availability.
    /// @return true when rename requests are exposed.
    bool supportsRename() const override;

    /// @brief Report signature-help availability.
    /// @return true for this bridge.
    bool supportsSignatureHelp() const override;

    /// @brief Report workspace-symbol availability.
    /// @return true for this bridge.
    bool supportsWorkspaceSymbols() const override;

    /// @brief Report semantic-token availability.
    /// @return true for this bridge.
    bool supportsSemanticTokens() const override;

    /// @brief Find the definition of the symbol at a cursor.
    /// @param source Complete current document text.
    /// @param line One-based cursor line.
    /// @param col One-based cursor column.
    /// @param path Document path or editor URI.
    /// @return Definition location or nullopt.
    std::optional<LocationInfo> definition(const std::string &source,
                                           int line,
                                           int col,
                                           const std::string &path) override;

    /// @brief Find semantic references to the symbol at a cursor.
    /// @param source Complete current document text.
    /// @param line One-based cursor line.
    /// @param col One-based cursor column.
    /// @param path Document path or editor URI.
    /// @return Owning reference locations.
    std::vector<LocationInfo> references(const std::string &source,
                                         int line,
                                         int col,
                                         const std::string &path) override;

    /// @brief Compute text edits for renaming a symbol.
    /// @param source Complete current document text.
    /// @param line One-based cursor line.
    /// @param col One-based cursor column.
    /// @param path Document path or editor URI.
    /// @param newName Replacement identifier.
    /// @return Rename status, reason, and semantic edits.
    RenameResult rename(const std::string &source,
                        int line,
                        int col,
                        const std::string &path,
                        const std::string &newName) override;

    /// @brief Resolve callable overload and parameter information at a cursor.
    /// @param source Complete current document text.
    /// @param line One-based cursor line.
    /// @param col One-based cursor column.
    /// @param path Document path or editor URI.
    /// @return Signature-help response.
    SignatureHelpInfo signatureHelp(const std::string &source,
                                    int line,
                                    int col,
                                    const std::string &path) override;

    /// @brief Search symbols across the inferred workspace.
    /// @param query Case-insensitive name substring.
    /// @return Deterministically ordered matching symbols.
    std::vector<SymbolInfo> workspaceSymbols(const std::string &query) override;

    /// @brief Classify document tokens for semantic highlighting.
    /// @param source Complete current document text.
    /// @param path Document path or editor URI.
    /// @return Source-ordered semantic token spans.
    std::vector<SemanticTokenInfo> semanticTokens(const std::string &source,
                                                  const std::string &path) override;

    // ── Dump ──

    /// @brief Compile and serialize document IL.
    /// @param source Complete Zia source text.
    /// @param path Logical source path.
    /// @param optimized Whether to apply frontend optimization.
    /// @return Serialized IL or a compilation-failure description.
    std::string dumpIL(const std::string &source, const std::string &path, bool optimized) override;

    /// @brief Parse and print the document AST.
    /// @param source Complete Zia source text.
    /// @param path Logical source path.
    /// @return Printer-formatted AST or a no-AST marker.
    std::string dumpAst(const std::string &source, const std::string &path) override;

    /// @brief Lex a document into a line-oriented escaped token dump.
    /// @param source Complete Zia source text.
    /// @param path Logical source path.
    /// @return Location, kind, and spelling for every non-EOF token.
    std::string dumpTokens(const std::string &source, const std::string &path) override;

  private:
    /// @brief Guards access to the shared completion engine cache.
    /// @details Other bridge operations build fresh compiler state per call, but completions reuse
    /// a
    ///          long-lived engine for cache locality. The mutex preserves the documented
    ///          thread-safe bridge contract if the server dispatch model becomes concurrent.
    mutable std::mutex completionMutex_;
    /// @brief Persistent completion engine protected by @c completionMutex_.
    std::unique_ptr<il::frontends::zia::CompletionEngine> completionEngine_;

    /// @brief Guards project index updates and workspace symbol scans.
    mutable std::mutex projectMutex_;
    /// @brief Opaque runtime-backed project index protected by @c projectMutex_.
    void *projectIndex_{nullptr};
    /// @brief False after a runtime project-index mutation reports failure.
    /// @details Capability advertisement remains independent from this flag;
    ///          operations consult it to fail gracefully without hiding the
    ///          feature from clients that can recover after document changes.
    bool projectIndexUsable_{true};
    /// @brief Latest open document contents keyed by path or editor URI.
    std::unordered_map<std::string, std::string> openDocuments_;
    /// @brief Cached workspace symbols derived from open documents and nearby files.
    std::vector<SymbolInfo> workspaceSymbolCache_;
    /// @brief True when open document changes require rebuilding workspace symbols.
    bool workspaceSymbolCacheDirty_{true};
};

} // namespace zanna::server
