//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/il/io/OperandParse.hpp
// Purpose: Declare operand-level parsing helpers shared by IL readers.
// Key invariants: Helpers preserve existing OperandParser diagnostics and
//                 operand population behaviour.
// Ownership/Lifetime: Operate on parser-managed instruction/state without
//                     taking ownership.
// Links: docs/il/il-guide.md#reference, src/il/io/OperandParse_*.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/il/io/OperandParse.hpp
 * @brief Declares reusable operand parsers keyed by operand parse kinds.
 *
 * @details
 * The helpers translate textual operand fragments referenced by opcode
 * metadata into value, label, constant, or type payloads while preserving the
 * established parser diagnostics. Every operation advances a caller-owned
 * cursor and interacts with caller-owned parser and instruction state through
 * `Context`; no helper assumes ownership of those objects.
 *
 * A `ParseResult` always carries the success-or-diagnostic status and carries
 * an optional payload only for operand kinds that produce one.
 */

#pragma once

#include "il/core/Value.hpp"
#include "il/internal/io/ParserState.hpp"
#include "il/internal/io/ParserUtil.hpp"
#include "support/diag_expected.hpp"
#include "zanna/parse/Cursor.h"

#include <optional>
#include <string>
#include <utility>

namespace il::core
{
struct Instr;
} // namespace il::core

namespace zanna::il::io
{

/// @brief Shared parser context for operand helpers.
struct Context
{
    /**
     * @brief Bind operand helpers to parser state and an instruction.
     *
     * Read-only operand parsers require only `instruction`. Parsers that
     * intentionally attach metadata to the instruction, such as type-immediate
     * parsing, also require `mutableInstruction` to identify writable storage.
     *
     * @param parserState Borrowed parser state providing the current source
     *                    location, diagnostics, and SSA mappings.
     * @param instruction Borrowed instruction state visible to all parsers.
     * @param mutableInstruction Optional borrowed mutable view of the same
     *                           instruction for parsers that update metadata.
     *
     * @pre `parserState` and `instruction` outlive this context.
     * @pre When non-null, `mutableInstruction` denotes the instruction
     *      represented by `instruction` and outlives this context.
     */
    Context(::il::io::detail::ParserState &parserState,
            const ::il::core::Instr &instruction,
            ::il::core::Instr *mutableInstruction = nullptr)
        : state(parserState), instr(instruction), mutableInstr(mutableInstruction)
    {
    }

    ::il::io::detail::ParserState
        &state;               ///< Legacy parser state providing SSA maps and diagnostics.
    const ::il::core::Instr &instr; ///< Instruction state visible to read-only operand parsers.
    ::il::core::Instr
        *mutableInstr{nullptr}; ///< Instruction sink used only by mutating type parsers.
};

/// @brief Result bundle returned by operand-specific parsers.
struct ParseResult
{
    ::il::support::Expected<void> status{};   ///< Success/failure diagnostic container.
    std::optional<::il::core::Value> value{}; ///< Parsed Value operand when applicable.
    std::optional<std::string> label{};       ///< Parsed label text when applicable.

    /**
     * @brief Query whether the parser operation completed without a diagnostic.
     * @return `true` when `status` represents success.
     */
    [[nodiscard]] bool ok() const
    {
        return static_cast<bool>(status);
    }

    /**
     * @brief Query whether the operation produced a value payload.
     * @return `true` when `value` contains a parsed operand.
     */
    [[nodiscard]] bool hasValue() const
    {
        return value.has_value();
    }

    /**
     * @brief Query whether the operation produced a label payload.
     * @return `true` when `label` contains parsed label text.
     */
    [[nodiscard]] bool hasLabel() const
    {
        return label.has_value();
    }

    /**
     * @brief Report whether the result contains either supported payload kind.
     * @return `true` when `hasValue()` or `hasLabel()` is true.
     *
     * @note A successful type parser may return `false` because it attaches its
     *       result directly to `Context::mutableInstr`.
     */
    [[nodiscard]] bool consumed() const
    {
        return hasValue() || hasLabel();
    }
};

/// @brief Build a ParseResult describing a syntax error.
/// @details Populates the result with a diagnostic carrying the provided
///          message and source location taken from the parser context.
/// @param ctx Parsing context containing source location and diagnostics sink.
/// @param message Human-readable description moved into the diagnostic.
/// @return A result with a failed status and no value or label payload.
inline ParseResult syntaxError(Context &ctx, std::string message)
{
    ParseResult result;
    result.status = ::il::support::Expected<void>{
        ::il::io::makeLineErrorDiag(ctx.state.curLoc, ctx.state.lineNo, std::move(message))};
    return result;
}

/// @brief Parse a general Value operand from the supplied cursor segment.
/// @param cur Borrowed cursor positioned at the start of the operand token and
///            advanced past the recognized text on success.
/// @param ctx Shared parser context providing diagnostics and SSA mappings.
/// @return Result containing a value on success or a syntax diagnostic on
///         failure.
ParseResult parseValueOperand(zanna::parse::Cursor &cur, Context &ctx);

/// @brief Parse a branch label operand from the supplied cursor segment.
/// @param cur Borrowed cursor positioned at the beginning of the label text and
///            advanced past the recognized text on success.
/// @param ctx Shared parser context providing diagnostics and SSA mappings.
/// @return Result containing owned label text on success or a syntax diagnostic
///         on failure.
ParseResult parseLabelOperand(zanna::parse::Cursor &cur, Context &ctx);

/// @brief Parse a type literal operand and attach it to the instruction context.
/// @param cur Borrowed cursor positioned at the beginning of the type token and
///            advanced past the recognized text on success.
/// @param ctx Shared parser context providing diagnostics and instruction sink.
/// @return Result describing success or failure. On success no optional payload
///         is populated because the type is stored through `ctx.mutableInstr`.
/// @pre `ctx.mutableInstr` is non-null.
ParseResult parseTypeOperand(zanna::parse::Cursor &cur, Context &ctx);

/// @brief Parse a constant literal operand, producing a Value payload.
/// @param cur Borrowed cursor positioned at the start of the literal token and
///            advanced past the recognized text on success.
/// @param ctx Shared parser context providing diagnostics and SSA mappings.
/// @return Result containing the typed constant value on success or a syntax
///         diagnostic on failure.
ParseResult parseConstOperand(zanna::parse::Cursor &cur, Context &ctx);

} // namespace zanna::il::io
