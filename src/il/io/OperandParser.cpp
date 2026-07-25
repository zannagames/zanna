//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/io/OperandParser.cpp
// Purpose: Decode textual IL operands into structured Value instances and
//          branch metadata.
// Key invariants: Operates on instructions tied to the current parser state.
// Ownership/Lifetime: Mutates instructions owned by the parser caller.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the operand parsing helpers shared by the IL instruction
///        parser.
/// @details This module tokenises operand substrings, classifies literal forms,
///          and translates them into @ref il::core::Value objects.  It also
///          manages branch argument bookkeeping to ensure control-flow edges
///          match block parameter signatures, emitting diagnostics when the
///          textual form deviates from the IL specification.

#include "il/internal/io/OperandParser.hpp"

#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Value.hpp"
#include "il/internal/io/ParserUtil.hpp"
#include "il/internal/io/TypeParser.hpp"
#include "il/runtime/signatures/Registry.hpp"

#include "support/diag_expected.hpp"
#include "zanna/il/io/OperandParse.hpp"
#include "zanna/parse/Cursor.h"

#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace il::io::detail {

using il::core::Instr;
using il::core::Type;
using il::core::Value;
using il::support::Expected;
using il::support::makeError;

namespace {
using il::io::formatLineDiag;

using Operand = Value;

/// @brief Convert a runtime-signature parameter kind to a representable IL type.
/// @param kind Runtime registry parameter category.
/// @return Corresponding IL type, or no value for unsupported F32.
std::optional<Type> typeFromRuntimeSig(il::runtime::signatures::SigParam::Kind kind) {
    using Kind = il::runtime::signatures::SigParam::Kind;
    switch (kind) {
        case Kind::I1:
            return Type(Type::Kind::I1);
        case Kind::I32:
            return Type(Type::Kind::I32);
        case Kind::I64:
            return Type(Type::Kind::I64);
        case Kind::F64:
            return Type(Type::Kind::F64);
        case Kind::Ptr:
            return Type(Type::Kind::Ptr);
        case Kind::Str:
            return Type(Type::Kind::Str);
        case Kind::F32:
            return std::nullopt;
    }
    return std::nullopt;
}

/// @brief Tracks string literal parsing state for scanners that respect quoted regions.
/// @details When scanning IL text that may contain string literals, delimiters and
///          special characters inside quoted regions must be ignored. This class
///          encapsulates the `inString` and `escape` state tracking logic.
class StringStateTracker {
    bool inString_ = false;
    bool escape_ = false;

  public:
    /// @brief Process a character and update string tracking state.
    /// @details Call this for each character in the input. Returns true if the
    ///          character is inside a string literal (including the opening/closing
    ///          quotes) and should be treated as literal content.
    /// @param c The current character.
    /// @return True if the character is part of a string literal.
    bool processChar(char c) {
        if (inString_) {
            if (escape_) {
                escape_ = false;
                return true;
            }
            if (c == '\\') {
                escape_ = true;
                return true;
            }
            if (c == '"')
                inString_ = false;
            return true;
        }
        if (c == '"') {
            inString_ = true;
            return true;
        }
        return false;
    }

    /// @brief Check if an unfinished string or escape sequence remains.
    /// @return True if the string state is incomplete at end of input.
    [[nodiscard]] bool hasUnfinishedString() const {
        return escape_ || inString_;
    }
};

/// @brief Find the balanced top-level parenthesis pair in operand text.
/// @param state Parser state supplying line diagnostics.
/// @param instr Instruction whose source location is reported.
/// @param text Operand text to scan.
/// @param startIndex First byte considered.
/// @param context Human-readable construct name.
/// @return Opening/closing offsets or a structured mismatch diagnostic.
static Expected<std::pair<size_t, size_t>> findTopLevelParenRange(ParserState &state,
                                                                  const Instr &instr,
                                                                  const std::string &text,
                                                                  size_t startIndex,
                                                                  const char *context) {
    size_t lp = std::string::npos;
    size_t rp = std::string::npos;
    size_t depth = 0;
    StringStateTracker stringState;

    for (size_t index = startIndex; index < text.size(); ++index) {
        char c = text[index];
        if (stringState.processChar(c))
            continue;

        if (c == '(') {
            if (lp == std::string::npos) {
                lp = index;
                depth = 1;
            } else {
                ++depth;
            }
            continue;
        }

        if (c == ')') {
            if (lp == std::string::npos || depth == 0) {
                return Expected<std::pair<size_t, size_t>>{
                    il::io::makeLineErrorDiag(instr.loc, state.lineNo, "mismatched ')'")};
            }
            --depth;
            if (depth == 0) {
                rp = index;
                break;
            }
            continue;
        }
    }

    if (lp == std::string::npos || rp == std::string::npos || depth != 0) {
        std::ostringstream oss;
        oss << "malformed " << context;
        return Expected<std::pair<size_t, size_t>>{makeError(instr.loc, oss.str())};
    }

    return Expected<std::pair<size_t, size_t>>{std::make_pair(lp, rp)};
}

/// @brief Split operand text at a delimiter outside strings and parentheses.
/// @param state Parser state supplying line diagnostics.
/// @param instr Instruction whose source location is reported.
/// @param text Text to split.
/// @param delim Top-level delimiter byte.
/// @param context Human-readable construct name.
/// @return Trimmed non-empty segments or a structured syntax diagnostic.
static Expected<std::vector<std::string>> splitTopLevel(ParserState &state,
                                                        const Instr &instr,
                                                        const std::string &text,
                                                        char delim,
                                                        const char *context) {
    std::vector<std::string> tokens;
    std::string current;
    StringStateTracker stringState;
    size_t depth = 0;

    const bool whitespaceOnly = trim(text).empty();

    for (char c : text) {
        if (stringState.processChar(c)) {
            current.push_back(c);
            continue;
        }

        if (c == '(') {
            current.push_back(c);
            ++depth;
            continue;
        }
        if (c == ')') {
            if (depth == 0) {
                return Expected<std::vector<std::string>>{
                    il::io::makeLineErrorDiag(instr.loc, state.lineNo, "mismatched ')'")};
            }
            current.push_back(c);
            --depth;
            continue;
        }
        if (c == delim && depth == 0) {
            std::string trimmed = trim(current);
            if (trimmed.empty() && !whitespaceOnly) {
                std::ostringstream msg;
                msg << "malformed " << context;
                return Expected<std::vector<std::string>>{
                    il::io::makeLineErrorDiag(instr.loc, state.lineNo, msg.str())};
            }
            if (!trimmed.empty())
                tokens.push_back(std::move(trimmed));
            current.clear();
            continue;
        }

        current.push_back(c);
    }

    if (stringState.hasUnfinishedString()) {
        std::ostringstream msg;
        msg << "malformed " << context;
        return Expected<std::vector<std::string>>{
            il::io::makeLineErrorDiag(instr.loc, state.lineNo, msg.str())};
    }
    if (depth != 0) {
        return Expected<std::vector<std::string>>{
            il::io::makeLineErrorDiag(instr.loc, state.lineNo, "mismatched ')'")};
    }

    std::string trimmed = trim(current);
    if (!trimmed.empty())
        tokens.push_back(std::move(trimmed));

    return Expected<std::vector<std::string>>{std::move(tokens)};
}

/// @brief Create a failed Expected at the parser's current location.
/// @tparam T Expected success type.
/// @param state Parser state supplying location and line.
/// @param message Diagnostic body.
/// @return Error-valued Expected.
template <typename T> Expected<T> makeSyntaxError(ParserState &state, std::string message) {
    return Expected<T>{il::io::makeLineErrorDiag(state.curLoc, state.lineNo, std::move(message))};
}

/// @brief Parse optional bracketed call effect attributes.
/// @param state Parser state supplying diagnostics.
/// @param instr Call instruction whose CallAttr fields are updated.
/// @param text Trailing text after the argument list.
/// @return Success for empty/valid text, or an attribute syntax/name diagnostic.
Expected<void> parseCallAttrs(ParserState &state, Instr &instr, const std::string &text) {
    const std::string attrsText = trim(text);
    if (attrsText.empty())
        return {};

    if (attrsText.front() != '[' || attrsText.back() != ']') {
        return Expected<void>{
            il::io::makeLineErrorDiag(instr.loc, state.lineNo, "malformed call attributes")};
    }

    std::string body = trim(attrsText.substr(1, attrsText.size() - 2));
    if (body.empty()) {
        return Expected<void>{
            il::io::makeLineErrorDiag(instr.loc, state.lineNo, "empty call attribute list")};
    }

    for (char &ch : body)
        if (ch == ',')
            ch = ' ';

    std::istringstream ss(body);
    std::string attr;
    while (ss >> attr) {
        if (attr == "nothrow") {
            instr.CallAttr.nothrow = true;
        } else if (attr == "readonly") {
            instr.CallAttr.readonly = true;
        } else if (attr == "pure") {
            instr.CallAttr.pure = true;
        } else {
            std::ostringstream oss;
            oss << "unknown call attribute '" << attr << "'";
            return Expected<void>{il::io::makeLineErrorDiag(instr.loc, state.lineNo, oss.str())};
        }
    }

    return {};
}

/// @brief Parse and attach an explicit call.indirect signature.
/// @param state Parser state supplying diagnostics.
/// @param instr Indirect-call instruction to update.
/// @param body Return type and parenthesized parameter list.
/// @return Success or a structured delimiter/type/vararg diagnostic.
Expected<void> parseIndirectSignature(ParserState &state, Instr &instr, const std::string &body) {
    const size_t lp = body.find('(');
    const size_t rp = body.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp < lp ||
        !trim(body.substr(rp + 1)).empty()) {
        return Expected<void>{il::io::makeLineErrorDiag(
            instr.loc, state.lineNo, "malformed call.indirect signature")};
    }

    bool retOk = true;
    Type retType = il::io::parseType(trim(body.substr(0, lp)), &retOk);
    if (!retOk) {
        return Expected<void>{il::io::makeLineErrorDiag(
            instr.loc, state.lineNo, "unknown call.indirect return type")};
    }

    std::vector<Type> params;
    bool isVarArg = false;
    std::string paramsText = body.substr(lp + 1, rp - lp - 1);
    auto pieces = splitTopLevel(state, instr, paramsText, ',', "call.indirect signature");
    if (!pieces)
        return Expected<void>{pieces.error()};

    const auto &tokens = pieces.value();
    for (size_t index = 0; index < tokens.size(); ++index) {
        const std::string token = trim(tokens[index]);
        if (token == "...") {
            if (isVarArg || index + 1 != tokens.size()) {
                return Expected<void>{il::io::makeLineErrorDiag(
                    instr.loc, state.lineNo, "call.indirect variadic marker must appear last")};
            }
            isVarArg = true;
            continue;
        }

        bool ok = true;
        Type paramType = il::io::parseType(token, &ok);
        if (!ok || paramType.kind == Type::Kind::Void) {
            return Expected<void>{il::io::makeLineErrorDiag(
                instr.loc, state.lineNo, "unknown call.indirect parameter type")};
        }
        params.push_back(paramType);
    }

    instr.setIndirectSignature(retType, std::move(params), isVarArg);
    if (instr.result && retType.kind != Type::Kind::Void)
        instr.type = retType;
    return {};
}

} // namespace

/// @brief Create an operand parser bound to the current parser state and instruction.
/// @param state Mutable parser state borrowed for this parser's lifetime.
/// @param instr Mutable instruction populated in place.
/// @note instr_ aliases the caller-owned instruction so operands are populated in-place.
OperandParser::OperandParser(ParserState &state, Instr &instr) : state_(state), instr_(instr) {}

/// @brief Parse a single operand token into a Value representation.
///
/// Handles constants, temporaries, globals, null, and quoted string literals.
/// When parsing fails an error diagnostic is produced referencing the current
/// parser line.
///
/// @param tok Token extracted from the operand text.
/// @return Parsed value or an error diagnostic.
Expected<Value> OperandParser::parseValueToken(const std::string &tok) const {
    zanna::parse::Cursor cursor{tok, zanna::parse::SourcePos{state_.lineNo, 0}};
    zanna::il::io::Context ctx{state_, instr_};
    auto parsed = zanna::il::io::parseValueOperand(cursor, ctx);
    if (!parsed.ok())
        return Expected<Value>{parsed.status.error()};
    if (!parsed.hasValue())
        return makeSyntaxError<Value>(state_, "missing operand");
    return Expected<Value>{std::move(*parsed.value)};
}

/// @brief Split a comma-separated operand list while respecting nested constructs.
///
/// Tracks string literals, escape sequences, and parenthesis depth so nested
/// expressions do not break the split.  When malformed input is detected, an
/// error diagnostic is returned referencing the instruction being parsed.
///
/// @param text Raw operand text after the mnemonic.
/// @param context Human-readable description for error messages.
/// @return Vector of trimmed tokens or an error diagnostic.
Expected<std::vector<std::string>> OperandParser::splitCommaSeparated(const std::string &text,
                                                                      const char *context) const {
    return splitTopLevel(state_, instr_, text, ',', context);
}

/// @brief Parse operands for call-style instructions.
///
/// Extracts the callee name, decodes each argument, and appends them to the
/// instruction.  The function verifies balanced parentheses, rejects trailing
/// junk after the argument list, and reports clear diagnostics on malformed
/// text.
///
/// @param text Operand substring following the mnemonic.
/// @return Success or an error diagnostic.
Expected<void> OperandParser::parseCallOperands(const std::string &text) {
    const size_t at = text.find('@');
    if (at == std::string::npos) {
        return Expected<void>{
            makeError(instr_.loc, formatLineDiag(state_.lineNo, "malformed call"))};
    }
    if (!trim(text.substr(0, at)).empty()) {
        return Expected<void>{
            makeError(instr_.loc, formatLineDiag(state_.lineNo, "malformed call"))};
    }

    auto parens = findTopLevelParenRange(state_, instr_, text, at + 1, "call");
    if (!parens)
        return Expected<void>{parens.error()};
    const size_t lp = parens.value().first;
    const size_t rp = parens.value().second;

    instr_.setDirectCallee(state_.m, trim(text.substr(at + 1, lp - at - 1)));
    if (!isValidILIdentifier(instr_.callee)) {
        return Expected<void>{
            il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, "malformed call name")};
    }
    auto attrs = parseCallAttrs(state_, instr_, text.substr(rp + 1));
    if (!attrs)
        return attrs;

    std::string args = text.substr(lp + 1, rp - lp - 1);
    auto tokens = splitCommaSeparated(args, "call");
    if (!tokens)
        return Expected<void>{tokens.error()};
    for (const auto &token : tokens.value()) {
        auto argVal = parseValueToken(token);
        if (!argVal)
            return Expected<void>{argVal.error()};
        instr_.operands.push_back(std::move(argVal.value()));
    }
    // Calls carry a dynamic result type. Resolve any known direct callee now so
    // verifier precollection sees the same type that call checking will enforce.
    instr_.type = il::core::Type(il::core::Type::Kind::Void);
    if (instr_.result) {
        // Look up externs first
        for (const auto &ext : state_.m.externs) {
            if (ext.name == instr_.callee) {
                instr_.type = ext.retType;
                break;
            }
        }
        // Also check internal functions
        if (instr_.type.kind == il::core::Type::Kind::Void) {
            for (const auto &fn : state_.m.functions) {
                if (fn.name == instr_.callee) {
                    instr_.type = fn.retType;
                    break;
                }
            }
        }
        if (instr_.type.kind == il::core::Type::Kind::Void) {
            for (const auto &sig : il::runtime::signatures::all_signatures()) {
                if (sig.name != instr_.callee || sig.rets.size() != 1)
                    continue;
                if (auto retType = typeFromRuntimeSig(sig.rets.front().kind))
                    instr_.type = *retType;
                break;
            }
        }
    }
    return {};
}

/// @brief Parse call.indirect operands: %fnPtr(%arg1, %arg2, ...).
/// @details First operand is function pointer, remaining are arguments in parens.
Expected<void> OperandParser::parseCallIndirectOperands(const std::string &text) {
    std::string work = trim(text);
    if (!work.empty() && work.front() == '[') {
        const size_t close = work.find(']');
        if (close == std::string::npos) {
            return Expected<void>{il::io::makeLineErrorDiag(
                instr_.loc, state_.lineNo, "malformed call.indirect signature")};
        }
        auto sig = parseIndirectSignature(state_, instr_, trim(work.substr(1, close - 1)));
        if (!sig)
            return sig;
        work = trim(work.substr(close + 1));
    }

    if (work.find('(') == std::string::npos) {
        // No parens — just a function pointer with no args
        std::string fnTok = trim(work);
        if (fnTok.find(')') != std::string::npos) {
            return Expected<void>{
                il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, "malformed call.indirect")};
        }
        if (fnTok.empty()) {
            return Expected<void>{
                il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, "call.indirect missing callee")};
        }
        auto fnVal = parseValueToken(fnTok);
        if (!fnVal)
            return Expected<void>{fnVal.error()};
        instr_.operands.push_back(std::move(fnVal.value()));
        return {};
    }
    auto parens = findTopLevelParenRange(state_, instr_, work, 0, "call.indirect");
    if (!parens)
        return Expected<void>{parens.error()};
    const size_t lp = parens.value().first;
    const size_t rp = parens.value().second;
    if (!trim(work.substr(rp + 1)).empty()) {
        return Expected<void>{
            il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, "malformed call.indirect")};
    }

    // Parse function pointer before the paren
    std::string fnTok = trim(work.substr(0, lp));
    if (fnTok.empty()) {
        return Expected<void>{
            il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, "call.indirect missing callee")};
    }
    auto fnVal = parseValueToken(fnTok);
    if (!fnVal)
        return Expected<void>{fnVal.error()};
    instr_.operands.push_back(std::move(fnVal.value()));

    // Parse comma-separated args inside parens
    std::string argsText = work.substr(lp + 1, rp - lp - 1);
    auto tokens = splitCommaSeparated(argsText, "call.indirect");
    if (!tokens)
        return Expected<void>{tokens.error()};
    for (const auto &tok : tokens.value()) {
        auto argVal = parseValueToken(tok);
        if (!argVal)
            return Expected<void>{argVal.error()};
        instr_.operands.push_back(std::move(argVal.value()));
    }

    return {};
}

/// @brief Parse a single branch target segment into a label and argument list.
///
/// Supports optional "label" prefixes and nested argument lists.  String
/// literals and parentheses are tracked to avoid premature splitting.  Parsed
/// arguments are appended to @p args while the cleaned label is returned via
/// @p label.
///
/// @param segment Text describing one branch destination.
/// @param label Output parameter receiving the normalized label.
/// @param args Output parameter receiving decoded argument values.
/// @return Success or an error diagnostic describing the malformed segment.
Expected<void> OperandParser::parseBranchTarget(const std::string &segment,
                                                std::string &label,
                                                std::vector<Value> &args) const {
    std::string text = trim(segment);
    const char *mnemonic = il::core::getOpcodeInfo(instr_.op).name;
    zanna::il::io::Context ctx{state_, instr_};
    size_t lp = std::string::npos;
    StringStateTracker stringState;
    for (size_t pos = 0; pos < text.size(); ++pos) {
        char c = text[pos];
        if (stringState.processChar(c))
            continue;
        if (c == '(') {
            lp = pos;
            break;
        }
    }

    if (lp == std::string::npos) {
        zanna::parse::Cursor cursor{text, zanna::parse::SourcePos{state_.lineNo, 0}};
        auto parsedLabel = zanna::il::io::parseLabelOperand(cursor, ctx);
        if (!parsedLabel.ok())
            return Expected<void>{parsedLabel.status.error()};
        if (!parsedLabel.hasLabel())
            return makeSyntaxError<void>(state_, "malformed branch target: missing label");
        label = *parsedLabel.label;
        return {};
    }

    auto parenRange = findTopLevelParenRange(state_, instr_, text, lp, mnemonic);
    if (!parenRange)
        return Expected<void>{parenRange.error()};
    size_t rp = parenRange.value().second;

    if (!trim(text.substr(rp + 1)).empty()) {
        std::ostringstream oss;
        oss << "malformed " << mnemonic;
        return Expected<void>{il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, oss.str())};
    }

    std::string labelText = trim(text.substr(0, lp));
    zanna::parse::Cursor cursor{labelText, zanna::parse::SourcePos{state_.lineNo, 0}};
    auto parsedLabel = zanna::il::io::parseLabelOperand(cursor, ctx);
    if (!parsedLabel.ok())
        return Expected<void>{parsedLabel.status.error()};
    if (!parsedLabel.hasLabel())
        return makeSyntaxError<void>(state_, "malformed branch target: missing label");
    label = *parsedLabel.label;
    std::string argsStr = text.substr(lp + 1, rp - lp - 1);
    auto tokens = splitCommaSeparated(argsStr, mnemonic);
    if (!tokens)
        return Expected<void>{tokens.error()};
    for (const auto &token : tokens.value()) {
        auto val = parseValueToken(token);
        if (!val)
            return Expected<void>{val.error()};
        args.push_back(std::move(val.value()));
    }
    return {};
}

/// @brief Validate that a branch target supplies the expected number of arguments.
///
/// Consults known block parameter counts or records unresolved branches for
/// later verification when the block is defined.
///
/// @param label Target label being checked.
/// @param argCount Number of arguments supplied with the branch.
/// @return Success or an error diagnostic when a mismatch is detected.
Expected<void> OperandParser::checkBranchArgCount(const std::string &label, size_t argCount) const {
    if (instr_.op == il::core::Opcode::EhPush)
        return {};
    auto it = state_.blockParamCount.find(label);
    if (it != state_.blockParamCount.end()) {
        if (it->second != argCount) {
            std::ostringstream oss;
            oss << "bad arg count";
            return Expected<void>{il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, oss.str())};
        }
    } else {
        state_.pendingBrs.push_back({label, argCount, state_.lineNo});
    }
    return {};
}

/// @brief Parse all branch targets for a multi-target instruction.
///
/// Splits the target list, parses each segment, records labels/arguments on the
/// instruction, and verifies argument counts against known block signatures.
///
/// @param text Textual representation of the branch operand list.
/// @param expectedTargets Number of targets required by the opcode.
/// @return Success or an error diagnostic describing the malformed operand list.
Expected<void> OperandParser::parseBranchTargets(const std::string &text, size_t expectedTargets) {
    std::string remaining = trim(text);
    const char *mnemonic = il::core::getOpcodeInfo(instr_.op).name;
    auto segments = splitCommaSeparated(remaining, mnemonic);
    if (!segments)
        return Expected<void>{segments.error()};

    const auto &segmentList = segments.value();
    if (segmentList.size() != expectedTargets) {
        std::ostringstream oss;
        oss << "malformed " << mnemonic;
        return Expected<void>{il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, oss.str())};
    }

    for (const auto &segment : segmentList) {
        std::vector<Value> args;
        std::string label;
        auto parsed = parseBranchTarget(segment, label, args);
        if (!parsed)
            return parsed;
        auto validated = validateCaseArity(std::move(label), std::move(args));
        if (!validated)
            return validated;
    }

    instr_.type = il::core::Type(il::core::Type::Kind::Void);
    return {};
}

/// @brief Parse switch-style operands consisting of a default and case list.
///
/// Reads the default branch, then iteratively parses `value -> label(args)`
/// pairs, storing both the case value and target metadata on the instruction.
/// The function enforces balanced parentheses and emits diagnostics for
/// malformed specifications.
///
/// @param text Raw operand text following the switch mnemonic.
/// @return Success or an error diagnostic.
Expected<void> OperandParser::parseSwitchTargets(const std::string &text) {
    std::string remaining = trim(text);
    const char *mnemonic = il::core::getOpcodeInfo(instr_.op).name;
    auto malformedSwitch = [&]() {
        std::ostringstream oss;
        oss << "line " << state_.lineNo << ": malformed " << mnemonic;
        return Expected<void>{makeError(instr_.loc, oss.str())};
    };

    if (remaining.empty())
        return malformedSwitch();

    bool parsingDefault = true;
    while (!remaining.empty()) {
        size_t split = remaining.size();
        size_t depth = 0;
        StringStateTracker stringState;
        for (size_t pos = 0; pos < remaining.size(); ++pos) {
            char c = remaining[pos];
            if (stringState.processChar(c))
                continue;

            if (c == '(')
                ++depth;
            else if (c == ')') {
                if (depth == 0) {
                    return Expected<void>{
                        il::io::makeLineErrorDiag(instr_.loc, state_.lineNo, "mismatched ')'")};
                }
                --depth;
            } else if (c == ',' && depth == 0) {
                split = pos;
                break;
            }
        }

        if (stringState.hasUnfinishedString())
            return malformedSwitch();

        std::string segment = trim(remaining.substr(0, split));
        if (segment.empty()) {
            return malformedSwitch();
        }

        if (parsingDefault) {
            auto parsed = parseDefaultTarget(segment);
            if (!parsed)
                return parsed;
            parsingDefault = false;
        } else {
            auto parsed = parseCaseSegment(segment, mnemonic);
            if (!parsed)
                return parsed;
        }

        if (split < remaining.size())
            remaining = trim(remaining.substr(split + 1));
        else
            break;
    }

    if (parsingDefault) {
        return malformedSwitch();
    }

    instr_.type = il::core::Type(il::core::Type::Kind::Void);
    return {};
}

/// @brief Parse the default branch component of a switch operand list.
///
/// @param segment Text describing the default destination.
/// @return Success or an error diagnostic when the target is malformed.
Expected<void> OperandParser::parseDefaultTarget(const std::string &segment) {
    std::vector<Value> args;
    std::string label;
    auto parsed = parseBranchTarget(segment, label, args);
    if (!parsed)
        return parsed;
    return validateCaseArity(std::move(label), std::move(args));
}

/// @brief Parse an individual `value -> label(args)` switch case.
///
/// @param segment Text describing the case value and destination.
/// @param mnemonic Name of the opcode for diagnostic messages.
/// @return Success or an error diagnostic when the case is malformed.
Expected<void> OperandParser::parseCaseSegment(const std::string &segment, const char *mnemonic) {
    const size_t arrow = segment.find("->");
    if (arrow == std::string::npos) {
        std::ostringstream oss;
        oss << "malformed " << mnemonic;
        return Expected<void>{makeError(instr_.loc, formatLineDiag(state_.lineNo, oss.str()))};
    }

    std::string valueText = trim(segment.substr(0, arrow));
    std::string targetText = trim(segment.substr(arrow + 2));
    if (valueText.empty() || targetText.empty()) {
        std::ostringstream oss;
        oss << "malformed " << mnemonic;
        return Expected<void>{makeError(instr_.loc, formatLineDiag(state_.lineNo, oss.str()))};
    }

    auto caseValue = parseValueToken(valueText);
    if (!caseValue)
        return Expected<void>{caseValue.error()};

    std::vector<Value> args;
    std::string label;
    auto parsed = parseBranchTarget(targetText, label, args);
    if (!parsed)
        return parsed;

    auto validated = validateCaseArity(label, args);
    if (!validated)
        return validated;
    instr_.operands.push_back(std::move(caseValue.value()));
    return {};
}

/// @brief Record a branch label and verify its argument arity for switch targets.
///
/// @param label Destination block label stripped of decorations.
/// @param args Branch arguments associated with the destination.
/// @return Success or an error diagnostic indicating an argument mismatch.
Expected<void> OperandParser::validateCaseArity(std::string label, std::vector<Value> args) {
    const size_t argCount = args.size();
    auto check = checkBranchArgCount(label, argCount);
    if (!check)
        return check;
    instr_.addBranchTarget(state_.m, std::move(label), std::move(args));
    return {};
}

} // namespace il::io::detail
