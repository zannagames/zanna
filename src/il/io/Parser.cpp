//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the façade entry point that wires together the IL text parser.
// The heavy lifting lives in dedicated module/function/instruction helpers;
// this translation unit coordinates them while keeping the public header light.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Top-level textual IL parser implementation.
/// @details Provides the @ref il::io::Parser::parse method used by command-line
///          tools and embedders.  The function streams line-by-line, delegates to
///          specialised helpers, and validates required directives like the
///          module version banner.

#include "il/io/Parser.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"

#include "il/internal/io/ModuleParser.hpp"
#include "il/internal/io/ParserUtil.hpp"
#include "support/diag_expected.hpp"

#include <sstream>
#include <string>

namespace il::io::detail {
il::support::Expected<void> parseModuleHeader_E(std::istream &is,
                                                std::string &line,
                                                ParserState &st);
}

namespace il::io {
namespace {

/// @brief Restores a module to its pre-parse shape unless parsing commits.
/// @details Text parsing appends declarations directly to the caller-owned
///          module. This scope guard snapshots scalar metadata and collection
///          lengths so an early diagnostic cannot leave a partially parsed
///          module visible to the caller. Existing IR objects are never copied
///          or replaced.
class ParseTransaction {
  public:
    /// @brief Capture the state needed to roll back mutations to @p module.
    /// @param module Caller-owned module about to receive parsed declarations;
    ///               it must outlive this transaction.
    explicit ParseTransaction(il::core::Module &module)
        : module_(module), version_(module.version), target_(module.target),
          externCount_(module.externs.size()), globalCount_(module.globals.size()),
          functionCount_(module.functions.size()), symbolCount_(module.symbols.size()) {}

    /// @brief Roll back externally visible parse mutations unless committed.
    /// @details Restores metadata, truncates appended declarations, and attempts
    ///          to return the symbol table to its original size. Symbol-table
    ///          synchronization failures are absorbed because a destructor must
    ///          not emit a second failure during diagnostic propagation.
    ~ParseTransaction() {
        if (!committed_) {
            module_.version = std::move(version_);
            module_.target = std::move(target_);
            module_.externs.resize(externCount_);
            module_.globals.resize(globalCount_);
            module_.functions.resize(functionCount_);
            try {
                module_.symbols.truncate(symbolCount_);
            } catch (...) {
                // Structural rollback is complete. A failed mutex operation may
                // retain unreachable interned spellings but cannot expose IR.
            }
        }
    }

    /// @brief Preserve all mutations made during the guarded parse.
    /// @details Once committed, destruction becomes a no-op.
    void commit() noexcept {
        committed_ = true;
    }

  private:
    il::core::Module &module_;
    std::string version_;
    std::optional<std::string> target_;
    std::size_t externCount_;
    std::size_t globalCount_;
    std::size_t functionCount_;
    std::size_t symbolCount_;
    bool committed_{false};
};

/// @brief Read one physical input line while enforcing a byte budget.
/// @details The terminating newline is consumed but not stored. If the line
///          exceeds @p maxBytes, its remainder is drained so the stream remains
///          positioned at the next line and @p tooLong is set. A final
///          unterminated line is still reported as input.
/// @param is Stream from which to consume characters.
/// @param line Receives the bounded line contents without the newline.
/// @param maxBytes Maximum number of bytes retained for one physical line.
/// @param tooLong Set to true when additional bytes had to be discarded.
/// @return True when a line, including a final unterminated line, was read;
///         false when the stream produced no characters.
bool readBoundedLine(std::istream &is,
                     std::string &line,
                     std::size_t maxBytes,
                     bool &tooLong) {
    line.clear();
    tooLong = false;
    char ch = '\0';
    while (is.get(ch)) {
        if (ch == '\n')
            return true;
        if (line.size() >= maxBytes) {
            tooLong = true;
            while (is.get(ch) && ch != '\n') {
            }
            return true;
        }
        line.push_back(ch);
    }
    return !line.empty();
}

/// @brief Construct the standard diagnostic for an exhausted parser budget.
/// @param lineNo One-based physical source line at which the limit was observed.
/// @param resource Human-readable name of the exhausted resource.
/// @return A failed expected value carrying the line-qualified diagnostic.
il::support::Expected<void> resourceLimitError(unsigned lineNo, std::string_view resource) {
    return il::support::Expected<void>{
        il::support::makeError({}, formatLineDiag(lineNo, std::string("resource limit exceeded: ") +
                                                             std::string(resource)))};
}

} // namespace

/// @brief Parse a textual IL module from a stream.
/// @details Creates a @ref ParserState bound to the destination module, then
///          pulls the source stream line-by-line.  Each iteration increments the
///          current line counter, strips comments and preprocessor directives,
///          and defers to @ref detail::parseModuleHeader_E for directive and
///          instruction handling.  Any diagnostic returned by the helper is
///          propagated verbatim, ensuring callers observe the first failure.
/// @param is Stream providing textual IL.
/// @param m Module populated with parsed definitions.
/// @param limits Resource budgets applied to physical input and resulting IR.
/// @return Empty Expected when parsing succeeds or the diagnostic describing the
///         first encountered error. On failure, @p m is restored to its
///         pre-call state.
il::support::Expected<void> Parser::parse(std::istream &is,
                                         il::core::Module &m,
                                         const ParserLimits &limits) {
    ParseTransaction transaction{m};
    detail::ParserState st{m, limits};
    std::string line;
    bool lineTooLong = false;
    while (readBoundedLine(is, line, limits.maxLineBytes, lineTooLong)) {
        if (static_cast<std::size_t>(st.lineNo) >= limits.maxLines)
            return resourceLimitError(st.lineNo, "physical lines");
        ++st.lineNo;
        if (lineTooLong)
            return resourceLimitError(st.lineNo, "line bytes");
        if (st.lineNo == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            line.erase(0, 3);
        }
        line = trim(stripInlineComment(line));
        if (line.empty())
            continue;

        if (auto result = detail::parseModuleHeader_E(is, line, st); !result)
            return result;

        if (m.functions.size() > limits.maxFunctions)
            return resourceLimitError(st.lineNo, "functions");
        if (m.externs.size() > limits.maxExterns)
            return resourceLimitError(st.lineNo, "extern declarations");
        if (m.globals.size() > limits.maxGlobals)
            return resourceLimitError(st.lineNo, "global declarations");

        if (st.totalBlocks > limits.maxBlocks)
            return resourceLimitError(st.lineNo, "basic blocks");
        if (st.totalInstructions > limits.maxInstructions)
            return resourceLimitError(st.lineNo, "instructions");
    }
    if (is.bad() || (is.fail() && !is.eof())) {
        return il::support::Expected<void>{
            il::support::makeError({}, formatLineDiag(st.lineNo, "input stream read failure"))};
    }
    if (!st.sawVersion) {
        std::ostringstream oss;
        oss << "line " << st.lineNo << ": missing 'il' version directive";
        return il::support::Expected<void>{il::support::makeError({}, oss.str())};
    }
    transaction.commit();
    return {};
}

} // namespace il::io
