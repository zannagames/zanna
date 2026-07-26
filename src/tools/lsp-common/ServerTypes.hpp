//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares language-neutral value types exchanged between compiler
///        bridges and protocol handlers.
///
/// Diagnostics, source locations, navigation results, completion/signature
/// data, semantic tokens, and runtime summaries are fully owned and contain no
/// compiler-internal pointers. Source coordinates use one-based byte columns
/// unless a type documents a different convention.
///
/// @see ICompilerBridge.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zanna::server {

/// @brief Related location attached to a diagnostic (e.g., "previous definition is here").
struct DiagnosticNoteInfo {
    std::string message; ///< Human-readable note text.
    std::string file;    ///< Source file the note refers to; empty when unknown.
    uint32_t line{0};    ///< 1-based line number; 0 when unknown.
    uint32_t column{0};  ///< 1-based column number; 0 when unknown.
};

/// @brief Machine-applicable replacement attached to a diagnostic.
/// @details The range is half-open: [line:column, endLine:endColumn). When
///          endLine/endColumn are 0 the fix-it is an insertion at line:column.
struct DiagnosticFixItInfo {
    std::string message;     ///< Human-readable description of the fix.
    std::string replacement; ///< Replacement text for the range.
    uint32_t line{0};        ///< 1-based begin line.
    uint32_t column{0};      ///< 1-based begin column.
    uint32_t endLine{0};     ///< 1-based end line (exclusive); 0 for insertions.
    uint32_t endColumn{0};   ///< 1-based end column (exclusive); 0 for insertions.
};

/// @brief Structured diagnostic from a compiler frontend.
struct DiagnosticInfo {
    int severity{0};       ///< 0 = note, 1 = warning, 2 = error
    std::string message;   ///< Human-readable diagnostic text.
    std::string file;      ///< Source file the diagnostic refers to.
    uint32_t line{0};      ///< 1-based line number.
    uint32_t column{0};    ///< 1-based column number.
    std::string code;      ///< Warning/error code (e.g., "W001", "B1001")
    uint32_t endLine{0};   ///< 1-based end line of the underlined range; 0 when unknown.
    uint32_t endColumn{0}; ///< 1-based end column (exclusive); 0 when unknown.
    std::string stage;     ///< Pipeline stage (parse, sema, lower, verify, runtime); may be empty.
    std::string help;      ///< Help text or URL for this diagnostic code; may be empty.
    std::vector<DiagnosticNoteInfo> notes;   ///< Related locations, in engine order.
    std::vector<DiagnosticFixItInfo> fixits; ///< Machine-applicable fixes, in engine order.
};

/// @brief Symbol information from semantic analysis.
struct SymbolInfo {
    std::string name; ///< Symbol identifier.
    std::string kind; ///< "variable", "parameter", "function", "method", "field", "type", "module"
    std::string type; ///< Type as string (e.g., "Integer", "List[String]")
    bool isFinal{false};  ///< True if the symbol is immutable/final.
    bool isExtern{false}; ///< True if the symbol is externally declared.
    uint32_t line{0};     ///< 1-based source line for the symbol declaration, or 0 if unknown.
    uint32_t column{0};   ///< 1-based source column for the symbol declaration, or 0 if unknown.
    std::string file;     ///< Source file path for workspace-wide symbol results; may be empty.
};

/// @brief Source range in a compiler-owned file.
/// @details Coordinates are 1-based byte columns and half-open at the end.
struct SourceRangeInfo {
    std::string file;      ///< Source file path.
    uint32_t line{0};      ///< 1-based start line.
    uint32_t column{0};    ///< 1-based start column.
    uint32_t endLine{0};   ///< 1-based end line.
    uint32_t endColumn{0}; ///< 1-based end column, exclusive.
};

/// @brief Location result for definition/reference navigation.
struct LocationInfo {
    SourceRangeInfo range; ///< File and range for the target occurrence.
    std::string name;      ///< Display name for the symbol.
    std::string kind;      ///< Symbol kind string.
    bool isDefinition{false}; ///< True when the occurrence declares the symbol.
};

/// @brief Single text edit produced by a workspace-level refactoring.
struct TextEditInfo {
    SourceRangeInfo range; ///< File and range to replace.
    std::string newText;   ///< Replacement text.
};

/// @brief Result of a semantic rename request.
struct RenameResult {
    bool success{false};              ///< Whether semantic rename completed.
    std::string reason;              ///< Empty when success is true.
    std::vector<TextEditInfo> edits; ///< Replacement edits grouped by file at the LSP boundary.
};

/// @brief Parameter metadata for signature help.
struct SignatureParameterInfo {
    std::string label;         ///< Full parameter label, usually "name: Type".
    std::string documentation; ///< Optional parameter documentation.
};

/// @brief One callable signature shown in signature help.
struct SignatureInfo {
    std::string label;         ///< Full signature label.
    std::string documentation; ///< Optional signature documentation.
    std::vector<SignatureParameterInfo> parameters; ///< Parameters in display order.
};

/// @brief Signature help payload for the active call expression.
struct SignatureHelpInfo {
    bool available{false}; ///< Whether the bridge found a containing callable.
    int activeSignature{0}; ///< Zero-based selected signature index.
    int activeParameter{0}; ///< Zero-based selected parameter index.
    std::vector<SignatureInfo> signatures; ///< Candidate signatures in preference order.
};

/// @brief Semantic token type indices shared by the bridge and LSP legend.
enum class SemanticTokenType : uint32_t {
    Namespace = 0, ///< Namespace or module name.
    Type = 1,      ///< General type name.
    Class = 2,     ///< Class name.
    Enum = 3,      ///< Enumeration name.
    Interface = 4, ///< Interface or protocol name.
    Function = 5,  ///< Free or module-level function.
    Method = 6,    ///< Member function.
    Variable = 7,  ///< Local or global variable.
    Parameter = 8, ///< Callable parameter.
    Property = 9,  ///< Object property or field.
    Keyword = 10,  ///< Language keyword.
    Number = 11,   ///< Numeric literal.
    String = 12,   ///< String literal.
    Operator = 13, ///< Operator token.
};

/// @brief Semantic token emitted by a compiler bridge.
/// @details Coordinates are 1-based byte columns; LspHandler converts them to
///          zero-based UTF-16 semantic-token deltas.
struct SemanticTokenInfo {
    uint32_t line{0};   ///< One-based source line.
    uint32_t column{0}; ///< One-based starting byte column.
    uint32_t length{0}; ///< Token length in source bytes.
    SemanticTokenType type{SemanticTokenType::Variable}; ///< Legend token type.
    uint32_t modifiers{0}; ///< Bitset of semantic-token modifiers; currently zero.
};

/// @brief Completion item from a completion engine.
struct CompletionInfo {
    std::string label;         ///< Text shown in the completion list.
    std::string insertText;    ///< Text inserted when the item is accepted.
    int kind{0};               ///< Maps to CompletionKind int (0-12)
    std::string detail;        ///< Secondary detail (e.g., type/signature).
    int sortPriority{0};       ///< Lower values sort earlier in the list.
    std::string documentation; ///< Optional Markdown documentation.
};

/// @brief Runtime class summary.
struct RuntimeClassSummary {
    std::string qname;    ///< Fully-qualified class name.
    int propertyCount{0}; ///< Number of properties on the class.
    int methodCount{0};   ///< Number of methods on the class.
    std::string summary;  ///< Short authored class description.
    std::string details;  ///< Long authored Markdown class description.
};

/// @brief Runtime member (method or property).
struct RuntimeMemberInfo {
    std::string name;       ///< Member name.
    std::string memberKind; ///< "method" or "property"
    std::string signature;  ///< Method signature or property type
};

/// @brief Full compilation result.
struct CompileResult {
    bool succeeded{false};                   ///< True when compilation produced no errors.
    std::vector<DiagnosticInfo> diagnostics; ///< Diagnostics emitted during compilation.
};

} // namespace zanna::server
