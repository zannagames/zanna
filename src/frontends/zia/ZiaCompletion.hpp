//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file ZiaCompletion.hpp
/// @brief C++ code-completion engine for the Zia language.
///
/// @details Provides `CompletionEngine`, a stateful object that accepts raw
/// source text and a cursor position and returns ranked `CompletionItem`
/// suggestions suitable for display in an IntelliSense-style popup.
///
/// ## Architecture
///
/// ```
///  source + (line,col)
///       │
///       ▼
///  extractContext()   ← backward scan: detect trigger, collect prefix
///       │
///       ▼
///  parseAndAnalyze()  ← error-tolerant Zia pipeline (stages 1–4 only)
///       │             ← one-entry cache keyed by FNV-1a source hash and path
///       ▼
///  provider dispatch  ← per TriggerKind (MemberAccess / CtrlSpace / etc.)
///       │
///       ▼
///  filterByPrefix()   ← remove non-matching items
///  rank()             ← sort by relevance (exact > prefix > contains)
///       │
///       ▼
///  vector<CompletionItem>  ← serializable to tab-delimited text
/// ```
///
/// ## Serialization
///
/// `serialize(items)` returns a newline-terminated string of tab-delimited
/// records, one per item:
///
///   label TAB insertText TAB kindInt TAB detail NEWLINE
///
/// Field content is escaped so multiline snippet text stays on one row:
/// backslash, tab, newline, and CR appear as \\, \t, \n, \r. Consumers
/// must unescape after splitting on the literal TAB/NEWLINE delimiters.
///
/// `kind` integers: Keyword=0 Snippet=1 Variable=2 Parameter=3 Field=4
/// Method=5 Function=6 Entity=7 Value=8 Interface=9 Module=10
/// RuntimeClass=11 Property=12
///
/// @see ZiaAnalysis.hpp — parseAndAnalyze() (error-tolerant partial compile)
/// @see Sema.hpp        — symbol enumeration APIs used by providers
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/ZiaAnalysis.hpp" // AnalysisResult, parseAndAnalyze()
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
/// @name Public data types
/// @{
//===----------------------------------------------------------------------===//

/// @brief Category of a completion item (maps to an icon in the UI).
enum class CompletionKind : uint8_t {
    Keyword = 0,
    Snippet = 1,
    Variable = 2,
    Parameter = 3,
    Field = 4,
    Method = 5,
    Function = 6,
    Entity = 7,
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
    CompletionKind kind{CompletionKind::Variable};
    std::string detail;           ///< Type/signature shown right-aligned in popup
    std::string documentation;    ///< Optional documentation text.
    std::string source;           ///< Source provider, e.g. "scope", "runtime", "keyword".
    std::string commitCharacters; ///< Characters that can explicitly commit the item.
    int sortPriority{100};        ///< Lower = ranked higher
    int replacementStartLine{1};
    int replacementStartColumn{0};
    int replacementEndLine{1};
    int replacementEndColumn{0};
    bool isSnippet{false};
    int cursorOffset{-1}; ///< Cursor offset after snippet insertion; -1 means after insert text.
};

/// @brief Serialize a list of items to tab-delimited text for the runtime bridge.
/// @details Format per line: label\tinsertText\tkindInt\tdetail\n
/// @param items  Completion items (already filtered and ranked).
/// @return       Newline-terminated serialized string.
std::string serialize(const std::vector<CompletionItem> &items);

/// @}

//===----------------------------------------------------------------------===//
/// @name CompletionEngine
/// @{
//===----------------------------------------------------------------------===//

/// @brief Stateful code-completion engine for Zia source files.
///
/// @details `complete()` is the primary entry point.  It accepts a full source
/// file (as a string) and a 1-based line / 0-based column position, and
/// returns up to `maxResults` ranked suggestions.
///
/// A one-entry cache avoids re-parsing the same file on consecutive
/// keystrokes.  The cache is keyed by an FNV-1a hash of the source bytes,
/// so any edit invalidates it.
///
/// ## Thread safety
///
/// Not thread-safe.  Each IDE connection should have its own engine instance.
class CompletionEngine {
  public:
    /// @brief Create an engine with an empty analysis cache and fresh source manager.
    CompletionEngine();

    /// @brief Destroy cached analysis before its source manager and diagnostic dependencies.
    ~CompletionEngine();

    /// @brief Compute completions for source at (line, col).
    /// @param source     Full source text of the file being edited.
    /// @param line       1-based line number of the cursor.
    /// @param col        0-based column of the cursor (chars from start of line).
    /// @param filePath   Virtual path used when registering with SourceManager.
    /// @param maxResults Maximum number of items returned (0 = unlimited).
    /// @return           Filtered, ranked completion items.
    std::vector<CompletionItem> complete(std::string_view source,
                                         int line,
                                         int col,
                                         std::string_view filePath = "<editor>",
                                         int maxResults = 50);

    /// @brief Return display text for the call signature active at (line, col).
    /// @details Parses the call expression immediately before the cursor,
    /// resolves it through scope symbols, member types, and runtime classes,
    /// then formats one or more callable signatures. Returns empty when no
    /// callable can be resolved.
    /// @param source Full source text of the file being edited.
    /// @param line One-based cursor line.
    /// @param col Zero-based cursor column.
    /// @param filePath Virtual source path used during analysis.
    /// @return Newline-delimited signature descriptions, or an empty string.
    std::string signatureHelp(std::string_view source,
                              int line,
                              int col,
                              std::string_view filePath = "<editor>");

    /// @brief Discard the cached AnalysisResult (forces re-parse next call).
    void clearCache();

  private:
    //=========================================================================
    /// @name Context extraction
    /// @{
    //=========================================================================

    /// @brief Describes what triggered the completion request.
    enum class TriggerKind : uint8_t {
        CtrlSpace,    ///< Explicit request — provide all in-scope symbols
        MemberAccess, ///< Dot ('.') — enumerate members of LHS type
        AfterNew,     ///< 'new ' keyword — provide constructible type names
        AfterColon,   ///< ': ' in a type annotation — provide type names
        AfterReturn,  ///< 'return ' — provide scope symbols + keywords
    };

    /// @brief Parsed context at the completion cursor.
    struct Context {
        TriggerKind trigger{TriggerKind::CtrlSpace};
        /// Expression to the left of '.', e.g. "shell.app" for "shell.app.X"
        std::string triggerExpr;
        /// Identifier chars typed after the trigger (may be empty)
        std::string prefix;
        /// Column at which prefix begins (insertion point for replacement)
        int replaceStart{0};
        /// 1-based line and 0-based column of the completion request.
        int line{1};
        int col{0};
    };

    /// @brief Extract completion context from source at (line, col).
    /// @param src Full source buffer.
    /// @param line One-based cursor line.
    /// @param col Zero-based cursor column.
    /// @return Trigger classification, prefix, receiver text, and replacement coordinates.
    Context extractContext(std::string_view src, int line, int col) const;

    /// @}
    //=========================================================================
    /// @name Providers
    /// @{
    //=========================================================================

    /// @brief Completion items for language keywords matching @p prefix.
    /// @param prefix Typed identifier prefix.
    /// @return Keyword items after case-insensitive prefix filtering.
    std::vector<CompletionItem> provideKeywords(const std::string &prefix) const;
    /// @brief Completion items for code snippets/templates matching @p prefix.
    /// @param prefix Typed identifier prefix.
    /// @return Snippet items after case-insensitive prefix filtering.
    std::vector<CompletionItem> provideSnippets(const std::string &prefix) const;

    /// @brief Completion items for symbols visible at the cursor.
    /// @param sema Completed semantic analyzer.
    /// @param prefix Typed identifier prefix.
    /// @param fileId Cursor file identifier.
    /// @param line One-based cursor line.
    /// @param col Zero-based cursor column.
    /// @return Nearest-scope-first variables, parameters, functions, and other visible symbols.
    std::vector<CompletionItem> provideScopeSymbols(
        const Sema &sema, const std::string &prefix, uint32_t fileId, int line, int col) const;

    /// @brief Resolve a member-access trigger and enumerate the receiver's members.
    /// @param sema Completed semantic analyzer.
    /// @param ctx Member-access completion context.
    /// @return File-module, runtime namespace/class, or semantic receiver members.
    std::vector<CompletionItem> provideMemberCompletions(const Sema &sema,
                                                         const Context &ctx) const;

    /// @brief Completion items for known type names matching @p prefix.
    /// @param sema Completed semantic analyzer.
    /// @param prefix Typed identifier prefix.
    /// @return Declared type items after filtering.
    std::vector<CompletionItem> provideTypeNames(const Sema &sema, const std::string &prefix) const;

    /// @brief Completion items exported by a bound file module.
    /// @param sema Completed semantic analyzer.
    /// @param moduleAlias Visible file-module root or alias.
    /// @param prefix Typed member prefix.
    /// @return Exported module symbols after filtering.
    std::vector<CompletionItem> provideModuleMembers(const Sema &sema,
                                                     const std::string &moduleAlias,
                                                     const std::string &prefix) const;

    /// @brief Completion items for visible bound file-module roots.
    /// @param sema Completed semantic analyzer.
    /// @param prefix Typed module prefix.
    /// @return Module items after filtering.
    std::vector<CompletionItem> provideBoundFileModules(const Sema &sema,
                                                        const std::string &prefix) const;

    /// @brief Completion items for one fully qualified runtime class.
    /// @param sema Completed semantic analyzer.
    /// @param fullClassName Qualified runtime class name.
    /// @param prefix Typed member prefix.
    /// @return Runtime methods and properties after filtering.
    std::vector<CompletionItem> provideRuntimeMembers(const Sema &sema,
                                                      const std::string &fullClassName,
                                                      const std::string &prefix) const;

    /// @brief Enumerate classes that are direct children of a runtime namespace.
    /// @details For example, with nsPrefix="Zanna.GUI", this returns items for
    ///          Canvas, App, ListBox, FloatingPanel, etc.  Handles user typing
    ///          a module alias followed by a dot (e.g. "GUI.").
    /// @param nsPrefix  Full dotted namespace path (e.g. "Zanna.GUI").
    /// @param prefix    Typed prefix filter (case-insensitive).
    /// @param sema Completed semantic analyzer supplying runtime catalog queries.
    /// @return Immediate runtime class/sub-namespace completion items.
    std::vector<CompletionItem> provideNamespaceMembers(const Sema &sema,
                                                        const std::string &nsPrefix,
                                                        const std::string &prefix) const;

    /// @}
    //=========================================================================
    /// @name Type resolution
    /// @{
    //=========================================================================

    /// @brief Resolve the Zia TypeRef for a dotted expression string.
    /// @details Walks the expression step-by-step via the visible symbol at the
    /// completion cursor, falling back to global symbols when no local match is
    /// available. For example "shell.app" first resolves `shell` from the
    /// innermost visible scope or globals, then looks up field `app` on the
    /// resulting type.
    /// @param sema Completed semantic analyzer.
    /// @param expr Dotted receiver expression.
    /// @param fileId Cursor file identifier.
    /// @param line One-based cursor line.
    /// @param col Zero-based cursor column.
    /// @return Resolved type, or nullptr/Unknown when resolution fails.
    TypeRef resolveExprType(
        const Sema &sema, const std::string &expr, uint32_t fileId, int line, int col) const;

    /// @}
    //=========================================================================
    /// @name Post-processing
    /// @{
    //=========================================================================

    /// @brief Drop items whose label does not match @p prefix (case-insensitive).
    /// @param items Mutable item vector.
    /// @param prefix Typed identifier prefix.
    void filterByPrefix(std::vector<CompletionItem> &items, const std::string &prefix) const;

    /// @brief Sort @p items by relevance to @p prefix (exact/prefix/fuzzy).
    /// @param items Mutable item vector.
    /// @param prefix Typed identifier prefix.
    void rank(std::vector<CompletionItem> &items, const std::string &prefix) const;

    /// @brief Remove duplicate completion entries (same label/kind).
    /// @param items Mutable item vector; the first item for each label is retained.
    void deduplicate(std::vector<CompletionItem> &items) const;

    /// @}
    //=========================================================================
    /// @name Cache
    /// @{
    //=========================================================================

    /// @brief Compute the 64-bit FNV-1a hash of a source buffer.
    /// @param data Source bytes.
    /// @return Deterministic 64-bit hash used by the one-entry cache.
    static uint64_t fnv1a(std::string_view data);

    /// @brief Return cached semantic analysis or rebuild it for changed input.
    /// @param source Full source buffer.
    /// @param filePath Virtual source path forming part of the cache identity.
    /// @return Borrowed pointer owned by the engine's cache.
    AnalysisResult *analyze(std::string_view source, std::string_view filePath);

    /// @brief One-entry semantic analysis cache.
    struct Cache {
        uint64_t hash{0};
        std::string filePath;
        std::unique_ptr<AnalysisResult> result;
    };

    Cache cache_;
    std::unique_ptr<il::support::SourceManager> sm_;

    /// @}
};

/// @}

} // namespace il::frontends::zia
