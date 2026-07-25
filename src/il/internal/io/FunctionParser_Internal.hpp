//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/internal/io/FunctionParser_Internal.hpp
// Purpose: Internal declarations shared between FunctionParser implementation
//          files. Contains the TokenStream class for line-based tokenization,
//          parser state wrappers, and common utility functions.
// Key invariants: Used only by FunctionParser_*.cpp files; not part of public API.
// Ownership/Lifetime: TokenStream and parser wrappers borrow their input/state.
//          ParserSnapshot owns rollback copies and restores a borrowed parser state.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Module.hpp"
#include "il/core/Param.hpp"
#include "il/internal/io/ParserState.hpp"
#include "il/internal/io/ParserUtil.hpp"
#include "support/diag_expected.hpp"
#include "zanna/parse/Cursor.h"

#include <cctype>
#include <istream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::io::detail {

using il::core::Param;
using il::core::Type;
using il::support::Diag;
using il::support::Expected;
using il::support::makeError;
using zanna::parse::Cursor;
using zanna::parse::SourcePos;

// Alias for the public ParserState to distinguish from parser_impl::ParserState
using LegacyParserState = ::il::io::detail::ParserState;
using Error = Diag;

// ============================================================================
// Token types for function body parsing
// ============================================================================

/// @brief Classifies lines encountered while parsing an IL function body.
enum class TokenKind {
    Skip,         ///< Line was blank or a comment; should be skipped.
    CloseBrace,   ///< Closing brace '}' marking the end of the function body.
    BlockLabel,   ///< A basic block label line (ending with ':').
    LocDirective, ///< A `.loc` source location directive.
    Instruction,  ///< An IL instruction line to parse.
    End,          ///< End of input reached before a closing brace.
};

// ============================================================================
// TokenStream - line-based tokenization for function bodies
// ============================================================================

/// @brief Line-based tokenizer for function body parsing.
/// @details Reads lines from the input stream, skipping comments and blank lines,
///          and classifies each line as a block label, instruction, directive, etc.
class TokenStream {
  public:
    /// @brief Bind a tokenizer to an input stream and shared parser state.
    /// @param stream Stream read incrementally; must outlive this tokenizer.
    /// @param legacy Parser state updated with physical line numbers.
    TokenStream(std::istream &stream, LegacyParserState &legacy)
        : stream_(&stream), legacy_(&legacy) {}

    /// @brief Inspect the current classified token.
    /// @return Token kind produced by the most recent advance().
    [[nodiscard]] TokenKind kind() const noexcept {
        return token_;
    }

    /// @brief Access the normalized current source line.
    /// @return Borrowed line text valid until the next advance().
    [[nodiscard]] const std::string &line() const noexcept {
        return line_;
    }

    /// @brief Access the shared legacy parser state.
    /// @return Mutable borrowed state reference.
    [[nodiscard]] LegacyParserState &legacy() noexcept {
        return *legacy_;
    }

    /// @brief Read and classify the next non-comment, non-empty line.
    /// @return True when a token line is available; false at EOF, I/O failure,
    ///         or a resource limit.
    bool advance() {
        while (readLine()) {
            ++legacy_->lineNo;
            line_ = trim(stripInlineComment(line_));
            if (line_.empty() || line_.rfind("//", 0) == 0)
                continue;
            if (!line_.empty() && line_.front() == '#')
                continue;
            if (!line_.empty() && line_.front() == '}') {
                token_ = TokenKind::CloseBrace;
                return true;
            }
            if (!line_.empty() && line_.back() == ':') {
                token_ = TokenKind::BlockLabel;
                return true;
            }
            if (line_.rfind(".loc", 0) == 0) {
                token_ = TokenKind::LocDirective;
                return true;
            }
            token_ = TokenKind::Instruction;
            return true;
        }
        token_ = TokenKind::End;
        line_.clear();
        return false;
    }

    /// @brief Report the resource dimension that stopped line reading.
    /// @return Empty view when no limit fired, otherwise a stable descriptive label.
    [[nodiscard]] std::string_view resourceLimit() const noexcept {
        return resourceLimit_;
    }

    /// @brief Report whether the most recent failed read was an I/O error.
    /// @return True for bad/fail states other than ordinary EOF.
    [[nodiscard]] bool ioError() const noexcept {
        return ioError_;
    }

  private:
    /// @brief Read one physical line while enforcing line-count and byte budgets.
    /// @return True when `line_` contains a complete or final unterminated line.
    bool readLine() {
        line_.clear();
        resourceLimit_.clear();
        ioError_ = false;
        if (static_cast<std::size_t>(legacy_->lineNo) >= legacy_->limits.maxLines) {
            resourceLimit_ = "physical lines";
            return false;
        }
        char ch = '\0';
        while (stream_->get(ch)) {
            if (ch == '\n')
                return true;
            if (line_.size() >= legacy_->limits.maxLineBytes) {
                resourceLimit_ = "line bytes";
                while (stream_->get(ch) && ch != '\n') {
                }
                return false;
            }
            line_.push_back(ch);
        }
        if (!line_.empty())
            return true;
        ioError_ = stream_->bad() || (stream_->fail() && !stream_->eof());
        return false;
    }

    std::istream *stream_ = nullptr;
    LegacyParserState *legacy_ = nullptr;
    std::string line_;
    std::string resourceLimit_;
    bool ioError_{false};
    TokenKind token_ = TokenKind::Skip;
};

// ============================================================================
// Internal parser state wrapper
// ============================================================================

namespace parser_impl {

/// @brief Internal state wrapper that bridges TokenStream with LegacyParserState.
struct ParserState {
    il::core::Module *mod = nullptr;
    il::core::Function *fn = nullptr;
    il::core::BasicBlock *cur = nullptr;
    il::support::SourceLoc loc{};
    il::support::DiagnosticEngine *diags = nullptr;
    TokenStream *ts = nullptr;
    LegacyParserState *legacy = nullptr;

    /// @brief Refresh wrapper pointers and location from the legacy parser state.
    void refresh() {
        if (!legacy)
            return;
        mod = &legacy->m;
        fn = legacy->curFn;
        cur = legacy->curBB;
        loc = legacy->curLoc;
    }

    /// @brief Commit wrapper function/block/location changes to legacy state.
    void commit() {
        if (!legacy)
            return;
        legacy->curFn = fn;
        legacy->curBB = cur;
        legacy->curLoc = loc;
    }

    /// @brief Return the current physical parser line.
    /// @return Legacy line number, or zero when no legacy state is attached.
    [[nodiscard]] unsigned lineNo() const noexcept {
        return legacy ? legacy->lineNo : 0;
    }
};

} // namespace parser_impl

// ============================================================================
// Data structures for prototype parsing
// ============================================================================

/// @brief Parsed function prototype: return type and parameter list.
struct Prototype {
    Type retType;              ///< Declared return type of the function.
    std::vector<Param> params; ///< Ordered parameter list with types and names.
    bool isVarArg{false};      ///< True when the prototype ends with an ellipsis.
};

/// @brief Result of parsing a function prototype header line.
/// @details Contains the parsed prototype and any trailing calling convention
///          segment that follows the parameter list.
struct PrototypeParseResult {
    Prototype proto;                     ///< Parsed return type and parameters.
    std::string_view callingConvSegment; ///< Trailing text after the parameter list.
};

/// @brief Parsed function attributes.
struct Attrs {
    bool nothrow = false;
    bool readonly = false;
    bool pure = false;
    bool moduleInitializer = false;
};

/// @brief Complete parsed function header including name, prototype, and metadata.
struct FunctionHeader {
    std::string name;           ///< Function identifier.
    Prototype proto;            ///< Return type and parameter list.
    il::core::CallingConv cc{
        il::core::CallingConv::Default}; ///< Calling convention annotation.
    Attrs attrs;                ///< Parsed function attributes.
    il::support::SourceLoc loc; ///< Source location of the function declaration.
};

// ============================================================================
// Snapshot for parser state rollback on error
// ============================================================================

/// @brief Captures parser state for transactional rollback on parse failure.
/// @details On construction, saves all mutable parser state (function context,
///          SSA mappings, pending branches, function count). If the parse
///          succeeds, the caller calls discard() to commit. On destruction
///          without discard(), the snapshot restores the saved state and removes
///          any functions that were added during the failed parse.
struct ParserSnapshot {
    LegacyParserState &state;      ///< Reference to the parser state being snapshotted.
    il::core::Function *curFn;     ///< Saved current function pointer.
    il::core::BasicBlock *curBB;   ///< Saved current basic block pointer.
    il::support::SourceLoc curLoc; ///< Saved source location.
    std::unordered_map<std::string, unsigned> tempIds;       ///< Saved SSA name-to-id mappings.
    unsigned nextTemp;                                       ///< Saved next temporary ID counter.
    std::unordered_map<std::string, size_t> blockParamCount; ///< Saved block parameter counts.
    std::vector<LegacyParserState::PendingBr> pendingBrs;    ///< Saved pending branch targets.
    std::unordered_set<std::string> functionNames; ///< Saved module function-name index.
    size_t functionCount; ///< Number of functions at snapshot time.
    bool active = true;   ///< True if rollback should occur on destruction.

    /// @brief Snapshot all state mutated during transactional function parsing.
    /// @param st Parser state restored unless discard() commits the transaction.
    explicit ParserSnapshot(LegacyParserState &st)
        : state(st), curFn(st.curFn), curBB(st.curBB), curLoc(st.curLoc), tempIds(st.tempIds),
          nextTemp(st.nextTemp), blockParamCount(st.blockParamCount), pendingBrs(st.pendingBrs),
          functionNames(st.functionNames),
          functionCount(st.m.functions.size()) {}

    /// @brief Restore captured parser fields and remove newly appended functions.
    void restore() {
        state.curFn = curFn;
        state.curBB = curBB;
        state.curLoc = curLoc;
        state.tempIds = tempIds;
        state.nextTemp = nextTemp;
        state.blockParamCount = blockParamCount;
        state.pendingBrs = pendingBrs;
        state.functionNames = functionNames;
        if (state.m.functions.size() > functionCount)
            state.m.functions.resize(functionCount);
    }

    /// @brief Commit the transaction by disabling destructor rollback.
    void discard() {
        active = false;
    }

    /// @brief Roll back the parse transaction when it remains active.
    ~ParserSnapshot() {
        if (active)
            restore();
    }
};

// ============================================================================
// Utility functions
// ============================================================================

/// @brief Trim whitespace from a string_view.
/// @param text Input view.
/// @return Subview excluding leading and trailing bytes classified as whitespace.
inline std::string_view trimView(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

/// @brief Create a line-prefixed error diagnostic.
/// @tparam T Expected success value type.
/// @param lineNo One-based physical line.
/// @param message Diagnostic body.
/// @return Failed Expected carrying `line N: message`.
template <class T> Expected<T> lineError(unsigned lineNo, const std::string &message) {
    std::ostringstream oss;
    oss << "line " << lineNo << ": " << message;
    return Expected<T>{makeError({}, oss.str())};
}

/// @brief Get source position from cursor.
/// @param cur Parser cursor.
/// @return Current cursor source position by value.
inline SourcePos cursorPos(const Cursor &cur) {
    return cur.pos();
}

/// @brief Create a syntax error with optional context.
/// @param pos Source position used for the line prefix.
/// @param msg Primary diagnostic message.
/// @param near Optional offending token appended in quotes.
/// @return Structured error diagnostic.
inline Error makeSyntaxError(SourcePos pos, std::string_view msg, std::string_view near) {
    std::ostringstream body;
    body << msg;
    if (!near.empty())
        body << " '" << near << "'";
    return lineError<void>(pos.line, body.str()).error();
}

/// @brief Normalises diagnostics captured from instruction parsing.
///
/// The instruction parser reports errors prefixed with "error: " and terminated by
/// trailing newlines. This helper strips that prefix and trailing newline/carriage
/// returns so that downstream diagnostics emitted through @ref
/// il::support::printDiag are consistent across call sites.
/// @param text Captured legacy diagnostic text.
/// @return Normalized message without the standard prefix or trailing newlines.
inline std::string stripCapturedDiagMessage(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    constexpr std::string_view kPrefix = "error: ";
    if (text.rfind(kPrefix, 0) == 0)
        text.erase(0, kPrefix.size());
    return text;
}

/// @brief Human-readable description of a token kind.
/// @param token Token classification.
/// @return Static phrase used in parser diagnostics.
inline std::string_view describeTokenKind(TokenKind token) {
    switch (token) {
        case TokenKind::CloseBrace:
            return "'}'";
        case TokenKind::BlockLabel:
            return "block label";
        case TokenKind::LocDirective:
            return "'.loc' directive";
        case TokenKind::Instruction:
            return "instruction";
        case TokenKind::End:
            return "end of function";
        case TokenKind::Skip:
            break;
    }
    return "token";
}

/// @brief Extract the text that caused a parse error.
/// @param state Parser wrapper containing the current TokenStream.
/// @return Current token text or a canonical brace/EOF spelling.
inline std::string describeOffendingToken(const parser_impl::ParserState &state) {
    if (!state.ts)
        return "";
    switch (state.ts->kind()) {
        case TokenKind::CloseBrace:
            return "}";
        case TokenKind::BlockLabel:
        case TokenKind::LocDirective:
        case TokenKind::Instruction:
            return state.ts->line();
        case TokenKind::End:
            return "<eof>";
        case TokenKind::Skip:
            break;
    }
    return "";
}

} // namespace il::io::detail
