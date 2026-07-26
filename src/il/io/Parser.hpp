//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/io/Parser.hpp
// Purpose: Declares the Parser class -- reads IL source text and constructs
//          Module objects using hand-written recursive descent. Implements the
//          complete IL grammar (version, target, externs, globals, functions,
//          instructions, operands). Inverse of Serializer.
// Key invariants:
//   - parse() is stateless; safe to call from multiple threads.
//   - On success, the output module conforms to the IL spec.
//   - On failure, returns diagnostic with precise line/column location.
// Ownership/Lifetime: Parser is a stateless facade with a static parse()
//          method. The caller owns the istream and the output Module.
// Links: docs/il/il-guide.md, il/io/Serializer.hpp,
//        il/internal/io/ParserState.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the stateless public entry point for parsing textual IL.
 *
 * @details `Parser::parse` coordinates module, function, instruction, operand,
 *          and type parsers under caller-supplied resource limits. A complete
 *          invocation is transactional: success populates the destination
 *          module, while syntax, input, or budget failure restores its original
 *          contents and returns a structured diagnostic.
 */

#pragma once

#include "il/core/fwd.hpp"
#include "il/internal/io/FunctionParser.hpp"
#include "il/internal/io/InstrParser.hpp"
#include "il/internal/io/ModuleParser.hpp"
#include "il/internal/io/ParserState.hpp"
#include "il/io/ParserLimits.hpp"
#include "support/diag_expected.hpp"

#include <istream>

namespace il::io {

/// @brief Stateless façade for converting textual IL into core IR.
/// @details Parsing is transactional: successful declarations are appended to
///          the supplied module, while any syntax, I/O, or resource-limit error
///          restores the module to its pre-call state. All per-invocation data
///          lives in the internal parser state.
class Parser {
  public:
    /// @brief Parse IL from stream into module @p m.
    /// @details Consumes the stream from its current position through end of
    ///          file, validates the required version directive, and enforces
    ///          @p limits over both physical input and generated IR.
    /// @param is Input stream containing IL text.
    /// @param m Module to populate with parsed contents.
    /// @param limits Resource budgets for this parse operation.
    /// @return Expected success or the first diagnostic on failure. Failure
    ///         leaves @p m unchanged.
    [[nodiscard]] static il::support::Expected<void> parse(
        std::istream &is,
        il::core::Module &m,
        const ParserLimits &limits = ParserLimits{});
};

} // namespace il::io
