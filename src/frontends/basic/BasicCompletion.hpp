//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BasicCompletion.hpp
// Purpose: Code-completion engine for the Zanna BASIC language.
// Key invariants:
//   - Reuses same CompletionKind/CompletionItem types as Zia
//   - One-entry LRU cache keyed by FNV-1a source hash plus file path
//   - Provider architecture: keywords, snippets, builtins, scope symbols, members
// Ownership/Lifetime:
//   - CompletionEngine owns the cache (BasicAnalysisResult)
//   - All returned items are fully owned
// Links: src/frontends/basic/BasicAnalysis.hpp,
//        src/frontends/basic/BasicCompletion.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file BasicCompletion.hpp
 * @brief Declares cached, ranked code completion for BASIC source buffers.
 *
 * Completion combines static language data with analyzer symbol tables, OOP
 * metadata, and runtime-class registry entries. Every returned item owns its
 * display, insertion, detail, and documentation strings.
 */

#pragma once

#include "frontends/basic/BasicAnalysis.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace il::frontends::basic {

/// @brief Category of a completion item (matches Zia's CompletionKind for LSP mapping).
/// @details Numeric values are stable because language-server adapters map them
///          to protocol completion categories.
enum class CompletionKind : uint8_t {
    Keyword = 0,
    Snippet = 1,
    Variable = 2,
    Parameter = 3,
    Field = 4,
    Method = 5,
    Function = 6,
    Entity = 7, // Class
    Value = 8,
    Interface = 9,
    Module = 10,
    RuntimeClass = 11,
    Property = 12,
};

/// @brief A single code-completion suggestion.
struct CompletionItem {
    std::string label;      ///< Text shown in the popup list
    std::string insertText; ///< Text inserted into the editor buffer
    ///< Semantic category used by clients and deduplication.
    CompletionKind kind{CompletionKind::Variable};
    std::string detail;          ///< Type/signature shown right-aligned
    int sortPriority{100};       ///< Lower = ranked higher
    std::string documentation{}; ///< Optional documentation shown by IDE clients.
};

/// @brief Stateful code-completion engine for BASIC source files.
///
/// `complete()` accepts a full source file and a 1-based cursor position,
/// returning ranked suggestions. A one-entry LRU cache avoids re-parsing
/// identical source/path pairs on consecutive requests.
class BasicCompletionEngine {
  public:
    /// @brief Construct an engine with an empty cache and owned SourceManager.
    BasicCompletionEngine();
    /// @brief Destroy the cached analysis and engine-owned source manager state.
    ~BasicCompletionEngine();

    /// @brief Compute completions for source at (line, col).
    /// @details Reuses an analysis when the FNV-1a source hash and file path
    ///          match the cached entry. Member access uses only member providers;
    ///          general completion merges semantic symbols, builtins, keywords,
    ///          and snippets before filtering, deduplication, and ranking.
    /// @param source     Full source text of the file being edited.
    /// @param line       1-based line number of the cursor.
    /// @param col        1-based column of the cursor.
    /// @param filePath   Virtual path for SourceManager.
    /// @param maxResults Maximum items returned; non-positive means unlimited.
    /// @return           Filtered, ranked completion items.
    /// @note Invalid or out-of-range cursor coordinates are clamped to the
    ///       selected line or the final available source position.
    std::vector<CompletionItem> complete(std::string_view source,
                                         int line,
                                         int col,
                                         std::string_view filePath = "<editor>",
                                         int maxResults = 50);

    /// @brief Discard the cached analysis result.
    /// @post The next complete() call reparses even if source and path match
    ///       the formerly cached request.
    void clearCache();

  private:
    /// @brief Trigger kind for completion dispatch.
    enum class TriggerKind : uint8_t {
        CtrlSpace,    ///< Explicit request — provide all in-scope symbols
        MemberAccess, ///< Dot ('.') — enumerate members of LHS object
    };

    /// @brief Parsed context at the completion cursor.
    struct Context {
        ///< Provider mode selected from the token immediately before the prefix.
        TriggerKind trigger{TriggerKind::CtrlSpace};
        std::string triggerExpr; ///< Expression left of '.'
        std::string prefix;      ///< Typed prefix after trigger
    };

    /// @brief Extract completion context from source at (line, col).
    /// @param src Full source buffer.
    /// @param line One-based requested line; values below one select line one.
    /// @param col One-based requested column; values below one select line start.
    /// @return Trigger kind, typed prefix, and optional dot receiver.
    Context extractContext(std::string_view src, int line, int col) const;

    // --- Providers ---
    // Each provider contributes one category of suggestions; complete() merges
    // their outputs before post-processing. All filter against @p prefix.

    /// @brief Suggest BASIC language keywords matching @p prefix.
    /// @param prefix Case-insensitive label prefix.
    /// @return Owned keyword completion items.
    std::vector<CompletionItem> provideKeywords(const std::string &prefix) const;
    /// @brief Suggest multi-line code snippets (e.g. FOR/NEXT skeletons).
    /// @param prefix Case-insensitive snippet-label prefix.
    /// @return Owned snippet completion items.
    std::vector<CompletionItem> provideSnippets(const std::string &prefix) const;
    /// @brief Suggest intrinsic/builtin function names matching @p prefix.
    /// @param prefix Case-insensitive builtin-name prefix.
    /// @return Owned builtin completion items.
    std::vector<CompletionItem> provideBuiltins(const std::string &prefix) const;
    /// @brief Suggest variables, constants, and procedures exposed by analysis.
    /// @param sema Analyzer holding the resolved symbol tables to enumerate.
    /// @param prefix Case-insensitive symbol-name prefix.
    /// @return Owned symbol completion items with available type details.
    std::vector<CompletionItem> provideScopeSymbols(const SemanticAnalyzer &sema,
                                                    const std::string &prefix) const;
    /// @brief Suggest members of the object left of a `.` member-access trigger.
    /// @param sema Analyzer used to resolve the trigger expression's type.
    /// @param ctx Parsed completion context describing the trigger expression.
    /// @return Matching user-class or runtime-class member items.
    std::vector<CompletionItem> provideMemberCompletions(const SemanticAnalyzer &sema,
                                                         const Context &ctx) const;
    /// @brief Suggest members of a runtime class resolved purely by name.
    /// @param className Fully-qualified runtime class (e.g. "Zanna.String").
    /// @param prefix Case-insensitive member-name prefix.
    /// @return Matching runtime methods followed by matching properties, or an
    ///         empty vector when the class is unknown.
    std::vector<CompletionItem> provideRuntimeMembers(const std::string &className,
                                                      const std::string &prefix) const;

    // --- Post-processing ---

    /// @brief Drop items whose label does not contain @p prefix case-insensitively.
    /// @param items Completion list filtered in place.
    /// @param prefix Substring to retain; empty leaves @p items unchanged.
    void filterByPrefix(std::vector<CompletionItem> &items, const std::string &prefix) const;
    /// @brief Rank items by exact match, prefix affinity, priority, and label.
    /// @param items Completion list reordered in place.
    /// @param prefix Case-insensitive match text used for affinity tiers.
    void rank(std::vector<CompletionItem> &items, const std::string &prefix) const;
    /// @brief Remove duplicate suggestions sharing the same label and kind.
    /// @param items Completion list deduplicated in place, preserving the first
    ///              occurrence of each case-sensitive label/kind key.
    void deduplicate(std::vector<CompletionItem> &items) const;

    // --- Cache ---

    /// @brief FNV-1a 64-bit hash used to key the one-entry analysis cache.
    /// @param data Source bytes to hash.
    /// @return Standard 64-bit FNV-1a digest.
    static uint64_t fnv1a(std::string_view data);

    /// @brief One-entry LRU cache of the last analyzed source buffer.
    struct Cache {
        uint64_t hash{0};                            ///< FNV-1a hash of cached source
        std::string filePath;                        ///< Path the cached result was analyzed under
        std::unique_ptr<BasicAnalysisResult> result; ///< Cached parse+sema result (may be null)
    };

    ///< Last analysis result, reused across matching source/path requests.
    Cache cache_; ///< Last analysis result, reused across keystrokes
    ///< Source manager used by the current cached analysis.
    std::unique_ptr<il::support::SourceManager> sm_; ///< Source manager backing cached analyses
};

} // namespace il::frontends::basic
