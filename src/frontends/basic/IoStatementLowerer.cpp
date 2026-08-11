//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/IoStatementLowerer.cpp
// Purpose: Implement I/O statement lowering extracted from Lowerer.
//          Handles lowering of BASIC terminal and file I/O statements to IL
//          and runtime helper calls.
// Key invariants: Maintains Lowerer's I/O lowering semantics exactly
// Ownership/Lifetime: Borrows Lowerer reference; coordinates with parent
// Links: src/frontends/basic/IoStatementLowerer.hpp,
//        src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/RuntimeCallHelpers.hpp,
//        src/frontends/basic/RuntimeNames.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file IoStatementLowerer.cpp
 * @brief Implements BASIC terminal, channel, and record I/O lowering.
 */

#include "IoStatementLowerer.hpp"
#include "Lowerer.hpp"
#include "RuntimeCallHelpers.hpp"
#include "RuntimeNames.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/lower/Emitter.hpp"
#include "frontends/basic/LocationScope.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"

using namespace il::frontends::basic::runtime;

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <optional>
#include <string>

namespace {
/// Width of a BASIC PRINT comma "zone": a `,` advances output to the next
/// multiple of this many columns (classic BASIC tab-stop spacing).
constexpr std::size_t kPrintZoneWidth = 14;

/// @brief Estimate the printed character width of a literal PRINT operand.
/// @details Used only to track the current output column so comma zone padding
///          can be computed at compile time. Returns @c std::nullopt for
///          non-literal expressions (width unknown until runtime), which makes
///          the caller fall back to a full zone advance.
/// @param expr Operand expression of a PRINT item.
/// @return Estimated rendered width, or nullopt when not statically known.
std::optional<std::size_t> estimatePrintWidth(const il::frontends::basic::Expr &expr) {
    using namespace il::frontends::basic;

    if (const auto *stringExpr = as<const StringExpr>(expr))
        return stringExpr->value.size();

    if (const auto *intExpr = as<const IntExpr>(expr))
        return std::to_string(intExpr->value).size();

    if (const auto *floatExpr = as<const FloatExpr>(expr)) {
        char buffer[64];
        int written = std::snprintf(buffer, sizeof(buffer), "%.15g", floatExpr->value);
        if (written < 0 || static_cast<std::size_t>(written) >= sizeof(buffer))
            return std::nullopt;
        return static_cast<std::size_t>(written);
    }

    if (const auto *boolExpr = as<const BoolExpr>(expr))
        return boolExpr->value ? std::size_t{2} : std::size_t{1};

    return std::nullopt;
}
} // namespace

namespace il::frontends::basic {

using IlType = il::core::Type;
using Value = il::core::Value;
using Opcode = il::core::Opcode;
using PrintChArgString = Lowerer::PrintChArgString;

/// Convert one already-lowered PRINT#/WRITE# argument to an owned/runtime string.
PrintChArgString lowerPrintChArgToString(IoStatementLowerer &self,
                                         const Expr &expr,
                                         Lowerer::RVal value,
                                         bool quoteStrings);
/// Build the single comma-delimited string emitted by WRITE#.
Value buildPrintChWriteRecord(IoStatementLowerer &self, const PrintChStmt &stmt);

/// @copydoc IoStatementLowerer::IoStatementLowerer()
IoStatementLowerer::IoStatementLowerer(Lowerer &lowerer) : lowerer_(lowerer) {}

/// @brief Lower an OPEN statement to the runtime helper sequence.
///
/// @details Evaluates the path and channel expressions, normalises the channel
///          to a 32-bit descriptor, and invokes `rt_open_err_vstr`.  Any runtime
///          error triggers emission of a `trap.from_err` via @ref emitRuntimeErrCheck.
///
/// @param stmt Parsed OPEN statement containing operands and source location.
void IoStatementLowerer::lowerOpen(const OpenStmt &stmt) {
    if (!stmt.pathExpr || !stmt.channelExpr)
        return;

    Lowerer::RVal path = lowerer_.lowerExpr(*stmt.pathExpr);
    Lowerer::RVal channel = lowerer_.lowerExpr(*stmt.channelExpr);

    RuntimeCallBuilder(lowerer_)
        .at(stmt.loc)
        .arg(path.value)
        .argNarrow32(Value::constInt(static_cast<int64_t>(stmt.mode)))
        .argChannel(channel.value, channel.type)
        .callWithErrCheck(IlType(IlType::Kind::I32), "rt_open_err_vstr", "open");
}

/// @brief Lower a CLOSE statement that releases an open channel.
///
/// @details Normalises the channel expression, calls `rt_close_err`, and hooks
///          the result into the standard runtime error check pipeline.
///
/// @param stmt Parsed CLOSE statement.
void IoStatementLowerer::lowerClose(const CloseStmt &stmt) {
    if (!stmt.channelExpr)
        return;

    Lowerer::RVal channel = lowerer_.lowerExpr(*stmt.channelExpr);

    RuntimeCallBuilder(lowerer_)
        .at(stmt.loc)
        .argChannel(channel.value, channel.type)
        .callWithErrCheck(IlType(IlType::Kind::I32), "rt_close_err", "close");
}

/// @brief Lower a SEEK statement that repositions a file channel.
///
/// @details Converts the channel to an @c i32 descriptor, coerces the position
///          expression to @c i64, and emits a call to `rt_seek_ch_err` with
///          diagnostic handling.
///
/// @param stmt Parsed SEEK statement.
void IoStatementLowerer::lowerSeek(const SeekStmt &stmt) {
    if (!stmt.channelExpr || !stmt.positionExpr)
        return;

    Lowerer::RVal channel = lowerer_.lowerExpr(*stmt.channelExpr);
    Lowerer::RVal position = lowerer_.lowerExpr(*stmt.positionExpr);

    RuntimeCallBuilder(lowerer_)
        .at(stmt.loc)
        .argChannel(channel.value, channel.type)
        .argI64(position.value, position.type)
        .callWithErrCheck(IlType(IlType::Kind::I32), "rt_seek_ch_err", "seek");
}

/// @brief Lower a PRINT statement to a series of runtime calls.
///
/// @details Iterates over each print item, lowering expressions to the
///          appropriate runtime helper and emitting spacing semantics for commas
///          and semicolons.  The helper appends a newline when the trailing item
///          is not a semicolon, mirroring BASIC behaviour.
///
/// @param stmt Parsed PRINT statement with expression list.
void IoStatementLowerer::lowerPrint(const PrintStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    std::size_t column = 1;
    bool columnKnown = true;

    /// @brief Advances the compile-time column estimate or marks it unknown.
    /// @param width Known printed width, or no value when it cannot be predicted.
    auto updateColumn = [&](std::optional<std::size_t> width) {
        if (!columnKnown)
            return;
        if (width)
            column += *width;
        else
            columnKnown = false;
    };

    /// @brief Resets local column accounting after an emitted newline.
    auto resetColumn = [&]() {
        column = 1;
        columnKnown = true;
    };

    for (const auto &it : stmt.items) {
        switch (it.kind) {
            case PrintItem::Kind::Expr: {
                std::optional<std::size_t> widthEstimate;
                if (it.expr)
                    widthEstimate = estimatePrintWidth(*it.expr);

                Lowerer::RVal value = lowerer_.lowerExpr(*it.expr);
                if (value.type.kind == IlType::Kind::Str) {
                    // Check if expr is an lvalue (borrowed reference that needs retaining).
                    // A member access that lowered to a runtime/instance property
                    // getter returns an OWNED string already scheduled for a
                    // statement-boundary release; retaining+releasing it here would
                    // double-free (VDOC-180). Treat such values as non-borrowed.
                    bool isLvalue = as<const VarExpr>(*it.expr) ||
                                    as<const MemberAccessExpr>(*it.expr) ||
                                    as<const ArrayExpr>(*it.expr);
                    if (isLvalue && lowerer_.emitter().isDeferredReleaseTemp(value.value))
                        isLvalue = false;

                    if (isLvalue) {
                        // Retain borrowed value before passing to print
                        lowerer_.requireStrRetainMaybe();
                        lowerer_.emitCall("rt_str_retain_maybe", {value.value});
                    }

                    // Print strings via kTerminalPrintStr
                    lowerer_.emitCall(kTerminalPrintStr, {value.value});

                    if (isLvalue) {
                        // Release the temporary after print
                        lowerer_.requireStrReleaseMaybe();
                        lowerer_.emitCall("rt_str_release_maybe", {value.value});
                    }

                    updateColumn(widthEstimate);
                    break;
                }
                if (value.type.kind == IlType::Kind::F64) {
                    lowerer_.emitCall(kTerminalPrintF64, {value.value});
                    updateColumn(widthEstimate);
                    break;
                }
                // BUG-001 fix: Handle Ptr/object types by converting to string first
                if (value.type.kind == IlType::Kind::Ptr) {
                    Value strVal = lowerer_.emitCallRet(
                        IlType(IlType::Kind::Str), kCoreObjectToString, {value.value});
                    lowerer_.emitCall(kTerminalPrintStr, {strVal});
                    lowerer_.requireStrReleaseMaybe();
                    lowerer_.emitCall("rt_str_release_maybe", {strVal});
                    updateColumn(widthEstimate);
                    break;
                }
                // Booleans are handled by lowerScalarExpr which calls coerceToI64,
                // converting to BASIC logical -1/0 (True=-1, False=0).
                value = lowerer_.lowerScalarExpr(std::move(value), stmt.loc);
                lowerer_.emitCall(kTerminalPrintI64, {value.value});
                updateColumn(widthEstimate);
                break;
            }
            case PrintItem::Kind::Comma: {
                std::size_t spaces = kPrintZoneWidth;
                if (columnKnown) {
                    std::size_t offset = (column - 1) % kPrintZoneWidth;
                    spaces = kPrintZoneWidth - offset;
                    column += spaces;
                }
                std::string padding(spaces, ' ');
                std::string spaceLbl = lowerer_.getStringLabel(padding);
                Value sp = lowerer_.emitConstStr(spaceLbl);
                lowerer_.emitCall(kTerminalPrintStr, {sp});
                break;
            }
            case PrintItem::Kind::Semicolon:
                break;
        }
    }

    bool suppress_nl = !stmt.items.empty() && stmt.items.back().kind == PrintItem::Kind::Semicolon;
    if (!suppress_nl) {
        std::string nlLbl = lowerer_.getStringLabel("\n");
        Value nl = lowerer_.emitConstStr(nlLbl);
        lowerer_.emitCall(kTerminalPrintStr, {nl});
        resetColumn();
    }
}

/// @brief Convert a PRINT# argument into a runtime string representation.
///
/// @details Determines whether the expression represents a string or numeric
///          value, performs any necessary narrowing to match runtime helper
///          contracts, and optionally quotes string values for CSV emission.
///          The helper returns both the lowered string and the runtime feature
///          that must be requested for linking.
///
/// @param self I/O lowering helper whose borrowed Lowerer emits conversions.
/// @param expr AST expression being lowered.
/// @param value Result of lowering the expression.
/// @param quoteStrings Whether string arguments should be quoted for CSV mode.
/// @return Lowered string value and optional runtime feature dependency.
PrintChArgString lowerPrintChArgToString(IoStatementLowerer &self,
                                         const Expr &expr,
                                         Lowerer::RVal value,
                                         bool quoteStrings) {
    LocationScope loc(self.lowerer_, expr.loc);
    if (value.type.kind == IlType::Kind::Str) {
        if (!quoteStrings)
            return {value.value, std::nullopt};

        Value quoted = self.lowerer_.emitCallRet(
            IlType(IlType::Kind::Str), "rt_csv_quote_alloc", {value.value});
        return {quoted, il::runtime::RuntimeFeature::CsvQuote};
    }

    TypeRules::NumericType numericType = self.lowerer_.classifyNumericType(expr);
    const char *runtime = nullptr;
    il::runtime::RuntimeFeature feature = il::runtime::RuntimeFeature::StrFromDouble;

    /// @brief Coerces an integer-like value to i64, then emits checked narrowing.
    /// @param target Destination integer kind.
    auto narrowInteger = [&](IlType::Kind target) {
        value = self.lowerer_.ensureI64(std::move(value), expr.loc);
        int bits = 64;
        switch (target) {
            case IlType::Kind::I16:
                bits = 16;
                break;
            case IlType::Kind::I32:
                bits = 32;
                break;
            case IlType::Kind::I1:
                bits = 1;
                break;
            default:
                bits = 64;
                break;
        }
        value.value = self.lowerer_.emitCommon(expr.loc).narrow_to(value.value, 64, bits);
        value.type = IlType(target);
    };

    switch (numericType) {
        case TypeRules::NumericType::Integer:
            runtime = kStringFromI16;
            feature = il::runtime::RuntimeFeature::StrFromI16;
            narrowInteger(IlType::Kind::I16);
            break;
        case TypeRules::NumericType::Long:
            runtime = kStringFromI32;
            feature = il::runtime::RuntimeFeature::StrFromI32;
            narrowInteger(IlType::Kind::I32);
            break;
        case TypeRules::NumericType::Single:
            runtime = kStringFromSingle;
            feature = il::runtime::RuntimeFeature::StrFromSingle;
            value = self.lowerer_.ensureF64(std::move(value), expr.loc);
            break;
        case TypeRules::NumericType::Double:
        default:
            runtime = kStringFromDouble;
            feature = il::runtime::RuntimeFeature::StrFromDouble;
            value = self.lowerer_.ensureF64(std::move(value), expr.loc);
            break;
    }

    Value text = self.lowerer_.emitCallRet(IlType(IlType::Kind::Str), runtime, {value.value});
    return {text, feature};
}

/// @brief Concatenate PRINT# arguments into a comma-delimited record.
///
/// @details Lowers each argument to a string, requests any needed runtime
///          helpers, and concatenates values using the runtime `rt_concat`
///          helper.  When no arguments are present the helper returns an empty
///          string literal handle.
///
/// @param self I/O lowering helper whose context receives the record IL.
/// @param stmt Parsed PRINT# statement describing the arguments.
/// @return Runtime string handle suitable for writing.
Value buildPrintChWriteRecord(IoStatementLowerer &self, const PrintChStmt &stmt) {
    Value record{};
    bool hasRecord = false;
    std::string commaLbl = self.lowerer_.getStringLabel(",");
    Value comma = self.lowerer_.emitConstStr(commaLbl);

    for (const auto &arg : stmt.args) {
        if (!arg)
            continue;

        Lowerer::RVal value = self.lowerer_.lowerExpr(*arg);
        PrintChArgString lowered = lowerPrintChArgToString(self, *arg, std::move(value), true);
        if (lowered.feature)
            self.lowerer_.requestHelper(*lowered.feature);

        if (!hasRecord) {
            record = lowered.text;
            hasRecord = true;
            continue;
        }

        self.lowerer_.curLoc = arg->loc;
        record =
            self.lowerer_.emitCallRet(IlType(IlType::Kind::Str), kStringConcat, {record, comma});
        record = self.lowerer_.emitCallRet(
            IlType(IlType::Kind::Str), kStringConcat, {record, lowered.text});
    }

    if (!hasRecord) {
        std::string emptyLbl = self.lowerer_.getStringLabel("");
        record = self.lowerer_.emitConstStr(emptyLbl);
    }

    return record;
}

/// @brief Lower a PRINT# or WRITE# statement.
///
/// @details Normalises the channel, determines whether the statement is WRITE
///          (which aggregates arguments) or PRINT (which streams them
///          individually), and emits `rt_write_ch_err` or
///          `rt_println_ch_err` as required. Each call is wrapped in runtime
///          error checking.
///
/// @param stmt Parsed PRINT#/WRITE# statement.
void IoStatementLowerer::lowerPrintCh(const PrintChStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    if (!stmt.channelExpr)
        return;

    Lowerer::RVal channel = lowerer_.lowerExpr(*stmt.channelExpr);
    channel = lowerer_.normalizeChannelToI32(std::move(channel), stmt.loc);

    bool isWrite = stmt.mode == PrintChStmt::Mode::Write;

    if (stmt.args.empty()) {
        if (stmt.trailingNewline || isWrite) {
            std::string emptyLbl = lowerer_.getStringLabel("");
            Value empty = lowerer_.emitConstStr(emptyLbl);
            Value err = lowerer_.emitCallRet(
                IlType(IlType::Kind::I32), "rt_println_ch_err", {channel.value, empty});
            const char *context = isWrite ? "write" : "printch";
            lowerer_.emitRuntimeErrCheck(
                /// @brief Emits a trap for a failed channel write.
                /// @param code Runtime error code.
                err, stmt.loc, context, [&](Value code) { lowerer_.emitTrapFromErr(code); });
        }
        return;
    }

    if (isWrite) {
        Value record = buildPrintChWriteRecord(*this, stmt);
        Value err = lowerer_.emitCallRet(
            IlType(IlType::Kind::I32), "rt_println_ch_err", {channel.value, record});
        lowerer_.emitRuntimeErrCheck(
            /// @brief Emits a trap for a failed WRITE operation.
            /// @param code Runtime error code.
            err, stmt.loc, "write", [&](Value code) { lowerer_.emitTrapFromErr(code); });
        return;
    }

    for (auto it = stmt.args.begin(); it != stmt.args.end(); ++it) {
        const auto &arg = *it;
        if (!arg)
            continue;

        Lowerer::RVal value = lowerer_.lowerExpr(*arg);
        PrintChArgString lowered = lowerPrintChArgToString(*this, *arg, std::move(value), false);
        if (lowered.feature)
            lowerer_.requestHelper(*lowered.feature);

        auto nextIt = it;
        do {
            ++nextIt;
        } while (nextIt != stmt.args.end() && !*nextIt);

        bool hasNext = nextIt != stmt.args.end();
        const char *helper = "rt_write_ch_err";
        if (stmt.trailingNewline && !hasNext)
            helper = "rt_println_ch_err";

        lowerer_.curLoc = arg->loc;
        Value err =
            lowerer_.emitCallRet(IlType(IlType::Kind::I32), helper, {channel.value, lowered.text});
        lowerer_.emitRuntimeErrCheck(
            /// @brief Emits a trap for a failed channel print.
            /// @param code Runtime error code.
            err, arg->loc, "printch", [&](Value code) { lowerer_.emitTrapFromErr(code); });
    }

    if (stmt.trailingNewline) {
        /// @brief Tests whether a sparse argument slot produced output.
        /// @param expr Candidate expression pointer.
        /// @return `true` when the slot contains an expression.
        auto hasPrintedArg = std::any_of(stmt.args.begin(), stmt.args.end(), [](const auto &expr) {
            return static_cast<bool>(expr);
        });
        if (!hasPrintedArg) {
            std::string emptyLbl = lowerer_.getStringLabel("");
            Value empty = lowerer_.emitConstStr(emptyLbl);
            Value err = lowerer_.emitCallRet(
                IlType(IlType::Kind::I32), "rt_println_ch_err", {channel.value, empty});
            lowerer_.emitRuntimeErrCheck(
                /// @brief Emits a trap for a failed empty channel print.
                /// @param code Runtime error code.
                err, stmt.loc, "printch", [&](Value code) { lowerer_.emitTrapFromErr(code); });
        }
    }
}

/// @brief Lower an INPUT statement that reads from the console.
///
/// @details Prints the prompt only when it is a StringExpr literal, reads a line
///          from the runtime, splits fields when multiple variables are present,
///          and stores each field into the appropriate slot with type-specific
///          conversions and string release management. With no targets, the
///          routine returns before reading.
///
/// @param stmt Parsed INPUT statement.
void IoStatementLowerer::lowerInput(const InputStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    if (stmt.prompt) {
        if (auto *se = as<const StringExpr>(*stmt.prompt)) {
            std::string lbl = lowerer_.getStringLabel(se->value);
            Value v = lowerer_.emitConstStr(lbl);
            // Use rt_* printing helper to match unit test expectations
            lowerer_.emitCall(kTerminalPrintStr, {v});
        }
    }
    if (stmt.vars.empty())
        return;

    // Read a full line from the console using kTerminalReadLine.
    Value line = lowerer_.emitCallRet(IlType(IlType::Kind::Str), kTerminalReadLine, {});
    // Precompute store kinds for each variable to avoid repeated symbol lookups.
    /// Target storage category used by the field-store closure.
    enum class StoreKind { I64, F64, I1, Str };
    std::unordered_map<std::string, StoreKind> storeKinds;
    storeKinds.reserve(stmt.vars.size());
    for (const auto &vn : stmt.vars) {
        if (vn.empty())
            continue;
        Lowerer::SlotType sk = lowerer_.getSlotType(vn);
        StoreKind k = StoreKind::I64;
        switch (sk.type.kind) {
            case IlType::Kind::Str:
                k = StoreKind::Str;
                break;
            case IlType::Kind::F64:
                k = StoreKind::F64;
                break;
            case IlType::Kind::I1:
                k = StoreKind::I1;
                break;
            default:
                break;
        }
        storeKinds.emplace(vn, k);
    }

    /// @brief Converts or transfers one input field into named target storage.
    /// @param name Target variable name.
    /// @param field Runtime string field to store or parse.
    auto storeField = [&](const std::string &name, Value field) {
        auto storage = lowerer_.resolveVariableStorage(name, stmt.loc);
        assert(storage && "INPUT target should have storage");
        if (!storage)
            return; // Safety: skip if storage lookup fails in Release builds.
        Lowerer::SlotType slotInfo = storage->slotInfo;
        // Be robust when symbol typing is incomplete in this context: consult
        // semantic analyzer for declared types to guide conversion (BUG-080).
        if (slotInfo.type.kind != IlType::Kind::Str && slotInfo.type.kind != IlType::Kind::F64 &&
            slotInfo.type.kind != IlType::Kind::I1) {
            // Prefer precomputed kind; fallback to a quick requery if missing.
            auto it = storeKinds.find(name);
            IlType::Kind k = (it != storeKinds.end())
                                 ? (it->second == StoreKind::Str   ? IlType::Kind::Str
                                    : it->second == StoreKind::F64 ? IlType::Kind::F64
                                    : it->second == StoreKind::I1  ? IlType::Kind::I1
                                                                   : IlType::Kind::I64)
                                 : lowerer_.getSlotType(name).type.kind;
            if (k == IlType::Kind::Str) {
                slotInfo.type = IlType(IlType::Kind::Str);
            } else if (k == IlType::Kind::F64) {
                slotInfo.type = IlType(IlType::Kind::F64);
            } else if (k == IlType::Kind::I1) {
                slotInfo.type = lowerer_.ilBoolTy();
                slotInfo.isBoolean = true;
            }
        }
        Value target = storage->pointer;
        if (slotInfo.type.kind == IlType::Kind::Str) {
            lowerer_.emitStore(IlType(IlType::Kind::Str), target, field);
            return;
        }

        if (slotInfo.type.kind == IlType::Kind::F64) {
            // Convert string to double via il::frontends::basic::runtime::kConvertToDouble
            lowerer_.requestHelper(il::runtime::RuntimeFeature::ToDouble);
            Value f = lowerer_.emitCallRet(IlType(IlType::Kind::F64),
                                           il::frontends::basic::runtime::kConvertToDouble,
                                           {field});
            lowerer_.emitStore(IlType(IlType::Kind::F64), target, f);
            lowerer_.requireStrReleaseMaybe();
            lowerer_.emitCall("rt_str_release_maybe", {field});
            return;
        }

        // Convert string to integer via il::frontends::basic::runtime::kConvertToInt
        lowerer_.requestHelper(il::runtime::RuntimeFeature::ToInt);
        Value n = lowerer_.emitCallRet(
            IlType(IlType::Kind::I64), il::frontends::basic::runtime::kConvertToInt, {field});
        if (slotInfo.isBoolean) {
            Value b = lowerer_.coerceToBool({n, IlType(IlType::Kind::I64)}, stmt.loc).value;
            lowerer_.emitStore(lowerer_.ilBoolTy(), target, b);
        } else {
            lowerer_.emitStore(IlType(IlType::Kind::I64), target, n);
        }
        lowerer_.requireStrReleaseMaybe();
        lowerer_.emitCall("rt_str_release_maybe", {field});
    };

    if (stmt.vars.size() == 1) {
        storeField(stmt.vars.front(), line);
        return;
    }

    const long long fieldCount = static_cast<long long>(stmt.vars.size());
    Value fields = lowerer_.emitAlloca(static_cast<int>(fieldCount * 8));
    // Split the input line into fields using the rt_* helper
    lowerer_.requestHelper(il::runtime::RuntimeFeature::SplitFields);
    lowerer_.emitCallRet(IlType(IlType::Kind::I64),
                         il::frontends::basic::runtime::kStringSplitFieldsRaw,
                         {line, fields, Value::constInt(fieldCount)});
    lowerer_.requireStrReleaseMaybe();
    lowerer_.emitCall("rt_str_release_maybe", {line});

    for (std::size_t i = 0; i < stmt.vars.size(); ++i) {
        long long offset = static_cast<long long>(i * 8);
        Value slot = lowerer_.emitBinary(
            Opcode::GEP, IlType(IlType::Kind::Ptr), fields, Value::constInt(offset));
        Value field = lowerer_.emitLoad(IlType(IlType::Kind::Str), slot);
        storeField(stmt.vars[i], field);
    }
}

/// @brief Lower an INPUT# statement for reading a record from a channel.
///
/// @details Allocates temporary slots, performs the channel read via
///          `rt_line_input_ch_err`, splits the result into one field per target
///          (or one disposable field when no targets exist), and parses each
///          field according to its declared slot type.
///
/// @param stmt Parsed INPUT# statement.
void IoStatementLowerer::lowerInputCh(const InputChStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    Value outSlot = lowerer_.emitAlloca(8);
    lowerer_.emitStore(IlType(IlType::Kind::Ptr), outSlot, Value::null());

    Lowerer::RVal channel{Value::constInt(stmt.channel), IlType(IlType::Kind::I64)};
    channel = lowerer_.normalizeChannelToI32(std::move(channel), stmt.loc);

    Value err = lowerer_.emitCallRet(
        IlType(IlType::Kind::I32), "rt_line_input_ch_err", {channel.value, outSlot});

    lowerer_.emitRuntimeErrCheck(
        /// @brief Emits a trap when channel line input fails.
        /// @param code Runtime error code.
        err, stmt.loc, "lineinputch", [&](Value code) { lowerer_.emitTrapFromErr(code); });

    Value line = lowerer_.emitLoad(IlType(IlType::Kind::Str), outSlot);

    // Split the line into as many fields as targets, or 1 if unspecified.
    const long long fieldCount =
        static_cast<long long>(stmt.targets.empty() ? 1 : stmt.targets.size());
    Value fieldsMem = lowerer_.emitAlloca(static_cast<int>(fieldCount * 8));
    lowerer_.emitStore(IlType(IlType::Kind::Ptr), fieldsMem, Value::null());
    lowerer_.emitCallRet(IlType(IlType::Kind::I64),
                         il::frontends::basic::runtime::kStringSplitFieldsRaw,
                         {line, fieldsMem, Value::constInt(fieldCount)});
    lowerer_.requireStrReleaseMaybe();
    lowerer_.emitCall("rt_str_release_maybe", {line});

    /// @brief Parses or transfers one channel field into a resolved variable slot.
    /// @param name Target variable name.
    /// @param field Runtime string field to store or parse.
    auto parseAndStore = [&](const std::string &name, Value field) {
        auto storage = lowerer_.resolveVariableStorage(name, stmt.loc);
        if (!storage)
            return;
        Lowerer::SlotType slotInfo = storage->slotInfo;
        Value slot = storage->pointer;
        if (slotInfo.type.kind == IlType::Kind::Str) {
            lowerer_.emitStore(IlType(IlType::Kind::Str), slot, field);
            return;
        }

        Value fieldCstr =
            lowerer_.emitCallRet(IlType(IlType::Kind::Ptr), "rt_string_cstr", {field});
        Value parsedSlot = lowerer_.emitAlloca(8);
        if (slotInfo.type.kind == IlType::Kind::F64) {
            Value parseErr = lowerer_.emitCallRet(IlType(IlType::Kind::I32),
                                                  il::frontends::basic::runtime::kParseDoubleCStr,
                                                  {fieldCstr, parsedSlot});
            /// @brief Emits a trap when floating-point field parsing fails.
            /// @param code Runtime error code.
            lowerer_.emitRuntimeErrCheck(parseErr, stmt.loc, "inputch_parse", [&](Value code) {
                lowerer_.emitTrapFromErr(code);
            });
            Value parsed = lowerer_.emitLoad(IlType(IlType::Kind::F64), parsedSlot);
            lowerer_.emitStore(IlType(IlType::Kind::F64), slot, parsed);
        } else {
            Value parseErr = lowerer_.emitCallRet(IlType(IlType::Kind::I32),
                                                  il::frontends::basic::runtime::kParseInt64CStr,
                                                  {fieldCstr, parsedSlot});
            /// @brief Emits a trap when integer field parsing fails.
            /// @param code Runtime error code.
            lowerer_.emitRuntimeErrCheck(parseErr, stmt.loc, "inputch_parse", [&](Value code) {
                lowerer_.emitTrapFromErr(code);
            });
            Value parsed = lowerer_.emitLoad(IlType(IlType::Kind::I64), parsedSlot);
            if (slotInfo.isBoolean) {
                Value b =
                    lowerer_.coerceToBool({parsed, IlType(IlType::Kind::I64)}, stmt.loc).value;
                lowerer_.emitStore(lowerer_.ilBoolTy(), slot, b);
            } else {
                lowerer_.emitStore(IlType(IlType::Kind::I64), slot, parsed);
            }
        }
        lowerer_.emitCall("rt_str_release_maybe", {field});
    };

    if (stmt.targets.empty()) {
        Value field = lowerer_.emitLoad(IlType(IlType::Kind::Str), fieldsMem);
        // With no explicit targets, nothing further to store.
        lowerer_.emitCall("rt_str_release_maybe", {field});
        return;
    }

    for (std::size_t i = 0; i < stmt.targets.size(); ++i) {
        long long offset = static_cast<long long>(i * 8);
        Value slot = lowerer_.emitBinary(
            Opcode::GEP, IlType(IlType::Kind::Ptr), fieldsMem, Value::constInt(offset));
        Value field = lowerer_.emitLoad(IlType(IlType::Kind::Str), slot);
        parseAndStore(stmt.targets[i].name, field);
    }
}

/// @brief Lower a LINE INPUT# statement that reads a full line into a string.
///
/// @details Reads the line through `rt_line_input_ch_err`, stores the result in
///          the target variable when present, and propagates runtime errors.
///
/// @param stmt Parsed LINE INPUT# statement.
void IoStatementLowerer::lowerLineInputCh(const LineInputChStmt &stmt) {
    LocationScope loc(lowerer_, stmt.loc);
    if (!stmt.channelExpr || !stmt.targetVar)
        return;

    Lowerer::RVal channel = lowerer_.lowerExpr(*stmt.channelExpr);
    channel = lowerer_.normalizeChannelToI32(std::move(channel), stmt.loc);

    Value outSlot = lowerer_.emitAlloca(8);
    lowerer_.emitStore(IlType(IlType::Kind::Ptr), outSlot, Value::null());

    Value err = lowerer_.emitCallRet(
        IlType(IlType::Kind::I32), "rt_line_input_ch_err", {channel.value, outSlot});

    lowerer_.emitRuntimeErrCheck(
        /// @brief Emits a trap when channel line input fails.
        /// @param code Runtime error code.
        err, stmt.loc, "lineinputch", [&](Value code) { lowerer_.emitTrapFromErr(code); });

    Value line = lowerer_.emitLoad(IlType(IlType::Kind::Str), outSlot);

    if (const auto *var = as<const VarExpr>(*stmt.targetVar)) {
        auto storage = lowerer_.resolveVariableStorage(var->name, stmt.loc);
        if (!storage)
            return;
        Value slot = storage->pointer;
        lowerer_.emitStore(IlType(IlType::Kind::Str), slot, line);
    }
}
} // namespace il::frontends::basic
