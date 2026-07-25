//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/io/FunctionParser_Body.cpp
// Purpose: Implementation of function body and basic block parsing. Handles
//          block labels, parameters, instructions, and .loc directives.
// Key invariants: Maintains SSA identifier uniqueness across blocks.
// Ownership/Lifetime: Populates blocks directly within the current function.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Parse IL function bodies, block headers, instructions, and location directives.
/// @details Body parsing is transactional through ParserSnapshot and enforces
///          per-function SSA uniqueness, branch arity, and configured resource limits.

#include "il/internal/io/FunctionParser.hpp"
#include "il/internal/io/FunctionParser_Internal.hpp"
#include "il/internal/io/InstrParser.hpp"
#include "il/internal/io/TypeParser.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <unordered_set>

namespace il::io::detail {

namespace {

/// @brief Normalises diagnostics captured from instruction parsing.
///
/// The instruction parser reports errors prefixed with "error: " and terminated by
/// trailing newlines. This helper strips that prefix and trailing newline/carriage
/// returns so that downstream diagnostics emitted through @ref
/// il::support::printDiag are consistent across call sites.
///
/// @param line Text of one instruction, in the same format emitted by the IL
/// serializer (including optional `%temp =` leading assignments).
/// @param st Parser state mutated for each decoded instruction; the helper
/// forwards to parseInstruction(), which may extend temporary mappings, update
/// pending branch bookkeeping, and capture diagnostic locations.
/// @return Empty on success; otherwise, a diagnostic normalised via
/// stripCapturedDiagMessage().
Expected<void> parseInstructionShim_E(const std::string &line, ParserState &st) {
    std::ostringstream capture;
    if (parseInstruction(line, st, capture))
        return {};
    auto message = stripCapturedDiagMessage(capture.str());
    return Expected<void>{makeError(st.curLoc, std::move(message))};
}

/// @brief Require the current token to have a particular classification.
/// @param state Parser wrapper containing the current token stream.
/// @param want Required token kind.
/// @param what Human-readable expectation appended to failures.
/// @return Success on a match, otherwise a line-prefixed unexpected-token diagnostic.
Expected<void> expect(parser_impl::ParserState &state, TokenKind want, std::string_view what) {
    if (state.ts && state.ts->kind() == want)
        return {};

    std::ostringstream oss;
    oss << "unexpected " << describeTokenKind(state.ts ? state.ts->kind() : TokenKind::Skip);
    std::string offending = describeOffendingToken(state);
    if (!offending.empty())
        oss << " '" << offending << "'";
    oss << " (expected " << what << ")";
    return lineError<void>(state.lineNo(), oss.str());
}

/// @brief Advance after an error until a synchronization token or EOF.
/// @param state Parser wrapper refreshed after scanning.
/// @param boundary Token kind at which recovery stops.
void recoverTo(parser_impl::ParserState &state, TokenKind boundary) {
    if (!state.ts)
        return;
    while (state.ts->kind() != TokenKind::End && state.ts->kind() != boundary) {
        if (!state.ts->advance())
            break;
    }
    state.refresh();
}

/// @brief Parse the current `.loc file line column` directive.
/// @param state Parser wrapper whose source location is updated and committed.
/// @return Success or a malformed-directive diagnostic.
Expected<void> parseLocDirective(parser_impl::ParserState &state) {
    if (!state.ts)
        return lineError<void>(state.lineNo(), "malformed .loc directive");

    std::istringstream ls(state.ts->line().substr(4));
    uint32_t file = 0;
    uint32_t line = 0;
    uint32_t column = 0;
    ls >> file >> line >> column;
    if (!ls)
        return lineError<void>(state.lineNo(), "malformed .loc directive");
    ls >> std::ws;
    if (ls.peek() != std::char_traits<char>::eof())
        return lineError<void>(state.lineNo(), "malformed .loc directive");
    state.loc = {file, line, column};
    state.commit();
    return {};
}

/// @brief Parse the current token as a basic-block header.
/// @param state Parser wrapper synchronized from legacy state after parsing.
/// @return Result of parseBlockHeader(), or a missing-label diagnostic.
Expected<void> parseBlock(parser_impl::ParserState &state) {
    if (!state.ts)
        return lineError<void>(state.lineNo(), "missing block label");
    std::string blockHeader = state.ts->line();
    if (!blockHeader.empty())
        blockHeader.pop_back();
    auto result = parseBlockHeader(blockHeader, *state.legacy);
    state.refresh();
    return result;
}

/// @brief Extract an instruction mnemonic after an optional result assignment.
/// @param line Normalized instruction line.
/// @return Subview containing the first opcode token, or an empty view.
std::string_view extractOpcode(std::string_view line) {
    line = trimView(line);
    if (line.empty())
        return line;
    size_t eq = line.find('=');
    if (eq != std::string_view::npos) {
        line.remove_prefix(eq + 1);
        line = trimView(line);
    }
    size_t space = line.find_first_of(" \t");
    if (space == std::string_view::npos)
        return line;
    return line.substr(0, space);
}

/// @brief Dispatch the current instruction line to the shared instruction parser.
/// @param state Parser wrapper with active legacy state and token stream.
/// @return Normalized instruction-parser result.
Expected<void> parseGenericInstr(parser_impl::ParserState &state, std::string_view) {
    if (!state.ts || !state.legacy)
        return lineError<void>(state.lineNo(), "unexpected instruction context");
    return parseInstructionShim_E(state.ts->line(), *state.legacy);
}

/// @brief Select and invoke the instruction handler for the current opcode.
/// @param state Parser wrapper containing the current instruction token.
/// @return Handler result or a missing-dispatch diagnostic.
Expected<void> parseInstr(parser_impl::ParserState &state) {
    using Handler = Expected<void> (*)(parser_impl::ParserState &, std::string_view);

    struct Dispatch {
        std::string_view opcode;
        Handler handler{nullptr};
    };

    static constexpr std::array<Dispatch, 3> kDispatchTable = {{
        Dispatch{"br", &parseGenericInstr},
        Dispatch{"ret", &parseGenericInstr},
        Dispatch{"", &parseGenericInstr},
    }};

    std::string_view opcode = state.ts ? extractOpcode(state.ts->line()) : std::string_view{};
    for (const auto &entry : kDispatchTable) {
        if ((entry.opcode.empty() || entry.opcode == opcode) && entry.handler)
            return entry.handler(state, opcode);
    }
    return lineError<void>(state.lineNo(), "instruction parser dispatch missing handler");
}

/// @brief Parse tokens through a function's closing brace.
/// @param stream Function-body token stream.
/// @param state Wrapper committed to the shared parser state as context changes.
/// @return Success after branch/temp resolution, or the first syntax, I/O, or
///         resource-limit diagnostic.
Expected<void> parseBody(TokenStream &stream, parser_impl::ParserState &state) {
    state.ts = &stream;
    state.refresh();

    while (stream.advance()) {
        state.refresh();

        if (stream.kind() == TokenKind::CloseBrace) {
            state.fn = nullptr;
            state.cur = nullptr;
            state.loc = {};
            state.commit();
            break;
        }

        if (stream.kind() == TokenKind::BlockLabel) {
            auto blockResult = parseBlock(state);
            if (!blockResult) {
                recoverTo(state, TokenKind::BlockLabel);
                return blockResult;
            }
            if (++state.legacy->totalBlocks > state.legacy->limits.maxBlocks)
                return lineError<void>(state.lineNo(),
                                       "resource limit exceeded: basic blocks");
            continue;
        }

        if (!state.cur)
            return expect(state, TokenKind::BlockLabel, "block label before instructions");

        if (stream.kind() == TokenKind::LocDirective) {
            auto locResult = parseLocDirective(state);
            if (!locResult) {
                recoverTo(state, TokenKind::BlockLabel);
                return locResult;
            }
            continue;
        }

        auto instrResult = parseInstr(state);
        if (!instrResult) {
            recoverTo(state, TokenKind::BlockLabel);
            return instrResult;
        }
        if (++state.legacy->totalInstructions > state.legacy->limits.maxInstructions)
            return lineError<void>(state.lineNo(), "resource limit exceeded: instructions");
        const core::Instr &parsedInstruction = state.cur->instructions.back();
        std::size_t valueCount = parsedInstruction.operands.size();
        for (const auto &args : parsedInstruction.brArgs)
            valueCount += args.size();
        if (valueCount > state.legacy->limits.maxValuesPerInstruction)
            return lineError<void>(state.lineNo(),
                                   "resource limit exceeded: instruction operands");
        state.refresh();
    }

    if (!stream.resourceLimit().empty())
        return lineError<void>(state.lineNo() + 1,
                               "resource limit exceeded: " +
                                   std::string(stream.resourceLimit()));

    if (stream.ioError())
        return lineError<void>(state.lineNo(), "input stream read failure");

    if (state.fn) {
        state.fn = nullptr;
        state.cur = nullptr;
        state.loc = {};
        state.commit();
        return lineError<void>(state.lineNo(), "unexpected end of file; missing '}'");
    }

    if (!state.legacy->pendingBrs.empty()) {
        const auto &unresolved = state.legacy->pendingBrs.front();
        std::ostringstream oss;
        oss << "unknown block '" << unresolved.label << "'";
        return lineError<void>(unresolved.line, oss.str());
    }

    if (!state.legacy->forwardTempNames.empty()) {
        const auto &name = *state.legacy->forwardTempNames.begin();
        std::ostringstream oss;
        oss << "unknown temp '%" << name << "'";
        return lineError<void>(state.lineNo(), oss.str());
    }

    return {};
}

} // namespace

// ============================================================================
// Block parameter parsing
// ============================================================================

/// @brief Parse one named, typed block parameter and assign or reuse its SSA ID.
/// @param paramText Parameter segment such as `%name: i64`.
/// @param st Mutable function parser state.
/// @param localNames Names already parsed in this block header.
/// @return Parsed parameter or a name/type/resource diagnostic.
Expected<Param> parseBlockParam(const std::string &paramText,
                                ParserState &st,
                                std::unordered_set<std::string> &localNames) {
    std::string q = trim(paramText);
    if (q.empty()) {
        std::ostringstream oss;
        oss << "line " << st.lineNo << ": bad param";
        if (!paramText.empty())
            oss << " '" << paramText << "'";
        else
            oss << " ''";
        oss << " (empty entry)";
        return Expected<Param>{makeError({}, oss.str())};
    }

    size_t col = q.find(':');
    if (col == std::string::npos)
        return lineError<Param>(st.lineNo, "bad param");

    std::string rawName = trim(q.substr(0, col));
    if (!rawName.empty() && rawName[0] != '%')
        return lineError<Param>(st.lineNo, "parameter name must start with '%'");

    std::string nm = rawName;
    if (!nm.empty() && nm[0] == '%')
        nm = nm.substr(1);
    if (nm.empty())
        return lineError<Param>(st.lineNo, "missing parameter name");
    if (!isValidILIdentifier(nm))
        return lineError<Param>(st.lineNo, "malformed parameter name");

    std::string tyStr = trim(q.substr(col + 1));
    bool ok = true;
    Type ty = parseType(tyStr, &ok);
    if (!ok || ty.kind == Type::Kind::Void)
        return lineError<Param>(st.lineNo, "unknown param type");

    if (!localNames.insert(nm).second) {
        std::ostringstream oss;
        oss << "duplicate parameter name '%" << nm << "'";
        return lineError<Param>(st.lineNo, oss.str());
    }

    // For entry block parameters that shadow function parameters, reuse the
    // existing ID from the function parameter. This ensures that references
    // to the parameter in instructions use the correct ID.
    if (st.curFn->blocks.empty()) {
        // This is the entry block - check if this param shadows a function param
        auto it = st.tempIds.find(nm);
        if (it != st.tempIds.end()) {
            const auto fnParamIt =
                std::find_if(st.curFn->params.begin(),
                             st.curFn->params.end(),
                             [&](const Param &param) { return param.id == it->second; });
            if (fnParamIt != st.curFn->params.end() && fnParamIt->type.kind != ty.kind) {
                return lineError<Param>(
                    st.lineNo, "entry block parameter type differs from function parameter");
            }
            // Reuse the function param ID
            return Param{nm, ty, it->second};
        }
    }

    if (st.forwardTempNames.erase(nm) > 0) {
        auto it = st.tempIds.find(nm);
        if (it == st.tempIds.end())
            return lineError<Param>(st.lineNo, "internal parser error resolving forward temp");
        Param param{nm, ty, it->second};
        return param;
    }

    if (static_cast<std::size_t>(st.nextTemp) >= st.limits.maxTempsPerFunction)
        return lineError<Param>(st.lineNo,
                                "resource limit exceeded: function temporaries");
    Param param{nm, ty, st.nextTemp};
    st.tempIds[nm] = st.nextTemp;
    if (st.curFn->valueNames.size() <= st.nextTemp)
        st.curFn->valueNames.resize(st.nextTemp + 1);
    st.curFn->valueNames[st.nextTemp] = nm;
    ++st.nextTemp;

    return param;
}

/// @brief Parse a parenthesized block-parameter list.
/// @param work Complete normalized block header.
/// @param lp Offset of the opening parenthesis.
/// @param st Mutable parser state used for SSA allocation.
/// @param bparams Receives parsed parameters in source order.
/// @return Success or the first delimiter/parameter diagnostic.
Expected<void> parseBlockParamList(const std::string &work,
                                   size_t lp,
                                   ParserState &st,
                                   std::vector<Param> &bparams) {
    size_t rp = work.find(')', lp);
    if (rp == std::string::npos)
        return lineError<void>(st.lineNo, "mismatched ')'");
    if (!trim(work.substr(rp + 1)).empty())
        return lineError<void>(st.lineNo, "unexpected characters after block parameter list");

    std::string paramsStr = work.substr(lp + 1, rp - lp - 1);
    std::stringstream pss(paramsStr);
    std::string piece;
    std::unordered_set<std::string> localNames;

    while (std::getline(pss, piece, ',')) {
        auto param = parseBlockParam(piece, st, localNames);
        if (!param)
            return Expected<void>{param.error()};
        bparams.push_back(std::move(param.value()));
    }

    return {};
}

/// @brief Resolve forward branches when their target block becomes known.
/// @param label Newly defined block label.
/// @param paramCount Number of parameters on the target.
/// @param st Parser state whose matching pending branches are removed.
/// @return Success, or an argument-count diagnostic from the original branch line.
Expected<void> resolvePendingBranches(const std::string &label,
                                      size_t paramCount,
                                      ParserState &st) {
    for (auto it = st.pendingBrs.begin(); it != st.pendingBrs.end();) {
        if (it->label == label) {
            if (it->args != paramCount)
                return lineError<void>(it->line, "bad arg count");
            it = st.pendingBrs.erase(it);
        } else {
            ++it;
        }
    }
    return {};
}

// ============================================================================
// Public API
// ============================================================================

/// @brief Parse, validate, and append a basic-block header.
/// @param header Label with optional parameters and optional `handler` prefix.
/// @param st Mutable state with an active function.
/// @return Success or a structured label, parameter, duplicate, or branch diagnostic.
Expected<void> parseBlockHeader(const std::string &header, ParserState &st) {
    std::string work = trim(header);
    if (work.rfind("handler ", 0) == 0)
        work = trim(work.substr(8));

    size_t lp = work.find('(');
    std::string label = lp != std::string::npos ? trim(work.substr(0, lp)) : trim(work);
    if (!label.empty() && label[0] == '^')
        label = label.substr(1);

    if (label.empty())
        return lineError<void>(st.lineNo, "missing block label");
    if (!isValidILIdentifier(label))
        return lineError<void>(st.lineNo, "malformed block label");

    if (st.blockParamCount.find(label) != st.blockParamCount.end()) {
        std::ostringstream oss;
        oss << "duplicate block '" << label << "'";
        return lineError<void>(st.lineNo, oss.str());
    }

    std::vector<Param> bparams;
    if (lp != std::string::npos) {
        auto paramsResult = parseBlockParamList(work, lp, st, bparams);
        if (!paramsResult)
            return paramsResult;
    }

    auto resolvedBranches = resolvePendingBranches(label, bparams.size(), st);
    if (!resolvedBranches)
        return resolvedBranches;

    st.curFn->blocks.push_back({label, bparams, {}, false});
    st.curBB = &st.curFn->blocks.back();
    st.curBB->labelSymbol = st.m.internIdentifier(label);
    st.blockParamCount[label] = bparams.size();

    return {};
}

/// @brief Parse one complete function transactionally.
/// @param is Stream positioned after the function declaration line.
/// @param header Function header text.
/// @param st Parser state and destination module.
/// @return Success after an import declaration or closing brace; failures roll
///         back all function-local and module-list mutations.
Expected<void> parseFunction(std::istream &is, std::string &header, ParserState &st) {
    ParserSnapshot snapshot{st};
    auto headerResult = parseFunctionHeader(header, st);
    if (!headerResult)
        return headerResult;

    // Import-linkage functions have no body; skip body parsing.
    if (st.curFn && st.curFn->linkage == il::core::Linkage::Import) {
        snapshot.discard();
        return {};
    }

    TokenStream tokens(is, st);
    parser_impl::ParserState local{};
    local.legacy = &st;
    local.ts = &tokens;
    local.refresh();

    auto bodyResult = parseBody(tokens, local);
    if (!bodyResult)
        return bodyResult;

    snapshot.discard();
    return {};
}

} // namespace il::io::detail
