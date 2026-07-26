//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the protocol-neutral adapter from BASIC compiler APIs to
///        ICompilerBridge.
///
/// Each analysis operation uses fresh compiler state. The bridge owns a
/// long-lived completion engine for cache reuse and serializes access to it so
/// callers may safely issue requests concurrently. Returned bridge structures
/// own all strings and collections.
///
/// @see tools/lsp-common/ICompilerBridge.hpp
/// @see frontends/basic/BasicAnalysis.hpp
/// @see frontends/basic/BasicCompletion.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tools/lsp-common/ICompilerBridge.hpp"

#include <memory>
#include <mutex>

namespace il::frontends::basic {
class BasicCompletionEngine;
}

namespace zanna::server {

/// @brief Protocol-neutral facade over the BASIC compiler and IDE engines.
///
/// Each method creates a fresh SourceManager per call for isolation.
/// The BasicCompletionEngine is shared across calls for LRU cache benefits.
class BasicCompilerBridge : public ICompilerBridge {
  public:
    /// @brief Construct the bridge and its persistent completion cache.
    BasicCompilerBridge();

    /// @brief Destroy the completion engine through its implementation type.
    ~BasicCompilerBridge() override;

    /// @brief Parse and semantically analyze BASIC source.
    /// @param source Complete source buffer.
    /// @param path Virtual or filesystem path used in diagnostics.
    /// @return Owned normalized diagnostics.
    std::vector<DiagnosticInfo> check(const std::string &source, const std::string &path) override;

    /// @brief Compile BASIC source through the normal frontend pipeline.
    /// @param source Complete source buffer.
    /// @param path Virtual or filesystem path used in diagnostics.
    /// @return Success flag and owned diagnostics.
    CompileResult compile(const std::string &source, const std::string &path) override;

    /// @brief Query completion items at a source position.
    /// @param source Complete source buffer.
    /// @param line One-based cursor line.
    /// @param col One-based cursor byte column.
    /// @param path Virtual or filesystem path used by the completion engine.
    /// @return Owned protocol-neutral completion items.
    std::vector<CompletionInfo> completions(const std::string &source,
                                            int line,
                                            int col,
                                            const std::string &path) override;

    /// @brief Build Markdown hover information for a BASIC symbol.
    /// @param source Complete source buffer.
    /// @param line One-based cursor line.
    /// @param col One-based cursor byte column.
    /// @param path Virtual or filesystem path used during analysis.
    /// @return Hover Markdown, or empty text when no symbol resolves.
    std::string hover(const std::string &source,
                      int line,
                      int col,
                      const std::string &path) override;

    /// @brief Enumerate semantic symbols and their declaration locations.
    /// @param source Complete source buffer.
    /// @param path Path attached to returned symbol locations.
    /// @return Variables, procedures, and classes in frontend order.
    std::vector<SymbolInfo> symbols(const std::string &source, const std::string &path) override;

    /// @brief Compile and serialize BASIC source as IL.
    /// @param source Complete source buffer.
    /// @param path Virtual or filesystem source path.
    /// @param optimized Whether to run the O1 IL pipeline.
    /// @return Serialized IL or human-readable failure text.
    std::string dumpIL(const std::string &source, const std::string &path, bool optimized) override;

    /// @brief Parse and print the BASIC abstract syntax tree.
    /// @param source Complete source buffer.
    /// @param path Virtual or filesystem source path.
    /// @return AST dump or a no-tree marker.
    std::string dumpAst(const std::string &source, const std::string &path) override;

    /// @brief Lex BASIC source into a line-oriented token dump.
    /// @param source Complete source buffer.
    /// @param path Virtual or filesystem source path.
    /// @return Token locations, kinds, and escaped spellings.
    std::string dumpTokens(const std::string &source, const std::string &path) override;

  private:
    /// @brief Guards access to the shared BASIC completion engine cache.
    /// @details The bridge otherwise creates fresh compiler state per request. This mutex keeps the
    ///          documented thread-safe contract valid if server request handling becomes concurrent.
    mutable std::mutex completionMutex_;
    /// @brief Owned long-lived completion engine and LRU cache.
    std::unique_ptr<il::frontends::basic::BasicCompletionEngine> completionEngine_;
};

} // namespace zanna::server
