//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/il/verify/InstructionChecker.cpp
// Purpose: Provide the central dispatch for verifying IL instructions against
//          schema-derived opcode metadata.
// Key invariants: Verification must align with /docs/il/il-guide.md#reference and
//                 report structured diagnostics without mutating the IR. The
//                 dispatcher relies on generated tables for operand counts and
//                 types and defers specialised checks to dedicated helpers.
// Ownership/Lifetime: Operates on caller-owned verification contexts and
//                     retains no global state beyond table references.
// Links: docs/il/il-guide.md#reference, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Instruction verification dispatcher for the IL verifier.
/// @details Drives verification from schema-derived tables so that operand and
///          result validation, control-flow checks, and opcode-specific behaviour
///          stay synchronised with the shared opcode definition file.

#include "il/verify/InstructionChecker.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/analysis/IntRangeAnalysis.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/utils/CheckedIntRange.hpp"
#include "il/verify/DiagSink.hpp"
#include "il/verify/InstructionCheckUtils.hpp"
#include "il/verify/InstructionCheckerShared.hpp"
#include "il/verify/OperandCountChecker.hpp"
#include "il/verify/OperandTypeChecker.hpp"
#include "il/verify/ResultTypeChecker.hpp"
#include "il/verify/SpecTables.hpp"
#include "il/verify/TypeInference.hpp"
#include "support/diag_expected.hpp"

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace il::verify {
namespace {

using checker::checkAddrOf;
using checker::checkAlloca;
using checker::checkCall;
using checker::checkConstNull;
using checker::checkConstStr;
using checker::checkDefault;
using checker::checkGAddr;
using checker::checkGEP;
using checker::checkIdxChk;
using checker::checkLoad;
using checker::checkShift;
using checker::checkStore;
using checker::checkTrapErr;
using checker::checkTrapFromErr;
using checker::checkTrapKind;
using checker::fail;
using il::core::BasicBlock;
using il::core::Instr;
using il::core::Opcode;
using il::core::Type;
using il::core::Value;
using il::support::Expected;
using il::support::makeError;
using il::utils::addRanges;
using il::utils::exactRange;
using il::utils::IntRange;
using il::utils::mergeIncomingRange;
using il::utils::mulRanges;
using il::utils::subRanges;

/// @brief Function signature for one table-dispatched verification strategy.
using StrategyFn = Expected<void> (*)(const VerifyCtx &, const InstructionSpec &);

/// @brief Derive the result type that should be recorded for an instruction.
/// @details Consults the generated specification to determine whether the
///          opcode yields its declared type, inherits the instruction's explicit
///          type annotation, or produces a concrete kind.  Unknown categories
///          fall back to the instruction's annotated type so diagnostics remain
///          consistent.
/// @param ctx Verification context describing the instruction under review.
/// @param spec Specification entry obtained from the generated opcode table.
/// @return The type that should be recorded for downstream inference.
Type resolveResultType(const VerifyCtx &ctx, const InstructionSpec &spec) {
    using il::core::TypeCategory;

    switch (spec.resultType) {
        case TypeCategory::None:
        case TypeCategory::Void:
        case TypeCategory::Any:
        case TypeCategory::Dynamic:
        case TypeCategory::InstrType:
            return ctx.instr.type;
        default:
            if (auto expectedKind = detail::kindFromCategory(spec.resultType))
                return Type(*expectedKind);
            return ctx.instr.type;
    }
}

/// @brief Execute the default verification strategy.
/// @details Records the instruction's result type using
///          @ref resolveResultType and delegates the structural checks to the
///          shared @ref checker::checkDefault helper.
/// @param ctx Verification context describing the instruction.
/// @param spec Specification entry driving strategy selection.
/// @return Empty on success or a diagnostic-filled error on failure.
Expected<void> applyDefault(const VerifyCtx &ctx, const InstructionSpec &spec) {
    ctx.types.recordResult(ctx.instr, resolveResultType(ctx, spec));
    return checkDefault(ctx);
}

/// @brief Validate semantics specific to @c alloca instructions.
/// @details Delegates to @ref checker::checkAlloca which validates the size,
///          warns for unusually large constants, and records a pointer result.
/// @param ctx Verification context for the instruction.
/// @return Verification success or failure.
Expected<void> applyAlloca(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkAlloca(ctx);
}

/// @brief Validate @c gep (get-element-pointer) instructions.
/// @details Ensures indices and base pointers satisfy the layout rules captured
///          by the verifier helpers.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyGEP(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkGEP(ctx);
}

/// @brief Validate @c load instructions.
/// @details Enforces addressability and type constraints via the shared
///          @ref checker::checkLoad routine.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyLoad(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkLoad(ctx);
}

/// @brief Validate @c store instructions.
/// @details Confirms pointer type, constant value range, and any statically
///          provable alloca bounds using the shared checker helper.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyStore(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkStore(ctx);
}

/// @brief Validate @c addr_of instructions.
/// @details Requires a known string global and delegates result recording to
///          @ref checker::checkAddrOf.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyAddrOf(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkAddrOf(ctx);
}

/// @brief Validate @c const_str instructions.
/// @details Confirms the operand names a known string global through the shared
///          checker helper.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyConstStr(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkConstStr(ctx);
}

/// @brief Validate @c gaddr instructions.
/// @details Confirms the operand names an addressable module global.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyGAddr(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkGAddr(ctx);
}

/// @brief Validate @c const_null instructions.
/// @details Delegates to the shared checker to enforce nullable-kind rules for
///          ptr, str, error, and resumetok.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyConstNull(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkConstNull(ctx);
}

/// @brief Validate @c call instructions.
/// @details Leverages @ref checker::checkCall to resolve callee signatures and
///          enforce argument compatibility.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyCall(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkCall(ctx);
}

/// @brief Validate trap-kind instructions.
/// @details Ensures trap codes and argument payloads align with the runtime
///          specification.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyTrapKind(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkTrapKind(ctx);
}

/// @brief Validate @c trap_from_error instructions.
/// @details Delegates to the shared checker to confirm operand layout and error
///          codes.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyTrapFromErr(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkTrapFromErr(ctx);
}

/// @brief Validate @c trap_error instructions.
/// @details Confirms operand arity and message payload rules through the shared
///          helper.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyTrapErr(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkTrapErr(ctx);
}

/// @brief Validate index-check instructions emitted for bounds checking.
/// @details Delegates to @ref checker::checkIdxChk to ensure operand ordering
///          and type annotations follow the runtime contract.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyIdxChk(const VerifyCtx &ctx, const InstructionSpec &) {
    return checkIdxChk(ctx);
}

/// @brief Validate floating-to-signed casts with round-to-even semantics.
/// @details Checks that the result type is one of the supported signed integer
///          widths before recording the type for downstream inference.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyCastFpToSiRteChk(const VerifyCtx &ctx, const InstructionSpec &) {
    const auto kind = ctx.instr.type.kind;
    if (!detail::isSupportedIntegerWidth(kind))
        return fail(ctx, "cast result must be i16, i32, or i64");
    ctx.types.recordResult(ctx.instr, ctx.instr.type);
    return {};
}

/// @brief Validate floating-to-unsigned casts with round-to-even semantics.
/// @details Mirrors @ref applyCastFpToSiRteChk but applies to unsigned targets.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyCastFpToUiRteChk(const VerifyCtx &ctx, const InstructionSpec &) {
    const auto kind = ctx.instr.type.kind;
    if (!detail::isSupportedIntegerWidth(kind))
        return fail(ctx, "cast result must be i16, i32, or i64");
    ctx.types.recordResult(ctx.instr, ctx.instr.type);
    return {};
}

/// @brief Validate signed-integer narrowing casts.
/// @details Ensures the destination type is within the supported set before
///          recording the result.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyCastSiNarrowChk(const VerifyCtx &ctx, const InstructionSpec &) {
    const auto kind = ctx.instr.type.kind;
    if (!detail::isNarrowingTargetWidth(kind))
        return fail(ctx, "narrowing cast result must be i16 or i32");
    ctx.types.recordResult(ctx.instr, ctx.instr.type);
    return {};
}

/// @brief Validate unsigned-integer narrowing casts.
/// @details Mirrors the signed variant but applies to unsigned conversions.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyCastUiNarrowChk(const VerifyCtx &ctx, const InstructionSpec &) {
    const auto kind = ctx.instr.type.kind;
    if (!detail::isNarrowingTargetWidth(kind))
        return fail(ctx, "narrowing cast result must be i16 or i32");
    ctx.types.recordResult(ctx.instr, ctx.instr.type);
    return {};
}

/// @brief Validate one operand of a width-annotated integer binary operation.
/// @details Constants must fit the selected width; other values must have a
///          known inferred kind exactly matching that width.
/// @param ctx Verification context containing the operation.
/// @param index Operand index to validate.
/// @param expectedKind Selected i16, i32, or i64 width.
/// @return Success when compatible; otherwise a range or type diagnostic.
Expected<void> validateIntegerBinaryOperand(const VerifyCtx &ctx,
                                            size_t index,
                                            il::core::Type::Kind expectedKind) {
    const Value &operand = ctx.instr.operands[index];
    if (operand.kind == Value::Kind::ConstInt) {
        if (!detail::fitsInIntegerKind(operand.i64, expectedKind))
            return fail(ctx, "integer binary constant operand out of range");
        return {};
    }

    bool missing = false;
    const Type actual = ctx.types.valueType(operand, &missing);
    if (missing)
        return fail(ctx, "integer binary operand type is unknown");
    if (actual.kind != expectedKind)
        return fail(ctx, "integer binary operands must match result type");
    return {};
}

/// @brief Validate checked integer binary ops whose width comes from the instruction type.
/// @details Structural checks already enforce operand arity and operand types against
///          the instruction type; this strategy rejects non-integer annotations
///          and records the declared integer result width.
/// @param ctx Verification context for the instruction.
/// @return Success or diagnostic error.
Expected<void> applyIntegerBinary(const VerifyCtx &ctx, const InstructionSpec &) {
    Type::Kind kind = ctx.instr.type.kind;
    if (kind == Type::Kind::Void) {
        std::optional<Type::Kind> inferred;
        for (const Value &operand : ctx.instr.operands) {
            if (operand.kind == Value::Kind::ConstInt)
                continue;

            bool missing = false;
            const Type actual = ctx.types.valueType(operand, &missing);
            if (missing)
                return fail(ctx, "integer binary operand type is unknown");
            if (!detail::isSupportedIntegerWidth(actual.kind)) {
                return fail(ctx,
                            "operand type mismatch: integer binary operands must be i16, i32, or "
                            "i64");
            }
            if (inferred && *inferred != actual.kind) {
                return fail(ctx,
                            "operand type mismatch: integer binary operands must have matching "
                            "widths");
            }
            inferred = actual.kind;
        }
        kind = inferred.value_or(Type::Kind::I64);
    }
    if (!detail::isSupportedIntegerWidth(kind))
        return fail(ctx, "integer binary result must be i16, i32, or i64");
    for (size_t index = 0; index < ctx.instr.operands.size(); ++index) {
        if (auto result = validateIntegerBinaryOperand(ctx, index, kind); !result)
            return result;
    }
    ctx.types.recordResult(ctx.instr, Type(kind));
    return {};
}

/// @brief Validate shift instructions.
/// @details Records the result type and delegates to @ref checker::checkShift
///          for any semantic rules beyond the generated structural checks.
/// @param ctx Verification context for the instruction.
/// @param spec Specification entry driving strategy selection.
/// @return Success for an i64 result, or a diagnostic for another width.
Expected<void> applyShift(const VerifyCtx &ctx, const InstructionSpec &spec) {
    const auto kind = ctx.instr.type.kind;
    if (kind != Type::Kind::I64)
        return fail(ctx, "shift result must be i64");
    ctx.types.recordResult(ctx.instr, resolveResultType(ctx, spec));
    return checkShift(ctx);
}

/// @brief Find a temporary definition inside one basic block.
/// @details Guarded overflow demotion is validated only when the condition and
///          guarded operation are connected by a direct predecessor edge, so a
///          local scan is enough and avoids depending on a separate def-use map.
/// @param block Block to scan.
/// @param id Temporary identifier to find.
/// @return Pointer to the defining instruction, or null when absent.
const Instr *findLocalDef(const BasicBlock &block, unsigned id) {
    for (const Instr &instr : block.instructions) {
        if (instr.result && *instr.result == id)
            return &instr;
    }
    return nullptr;
}

/// @brief Obtain a locally known range for a constant or temporary.
/// @param value Value to query.
/// @param ranges Current temporary-to-range facts.
/// @return Exact constant range, mapped temporary range, or no value.
std::optional<IntRange> rangeForValue(const Value &value,
                                      const std::unordered_map<unsigned, IntRange> &ranges) {
    if (value.kind == Value::Kind::ConstInt)
        return exactRange(value.i64);
    if (value.kind != Value::Kind::Temp)
        return std::nullopt;
    auto it = ranges.find(value.id);
    if (it == ranges.end())
        return std::nullopt;
    return it->second;
}

/// @brief Derive a signed range fact from one conditional-branch edge.
/// @details Accepts signed order comparisons and true-edge equality where one
///          operand is a temporary and the other is a constant. Comparisons
///          with the constant on the left are normalized before deriving bounds.
/// @param cmp Comparison defining the branch condition.
/// @param branchIndex Label index; zero denotes the true edge.
/// @param constrainedValue Output temporary constrained by the comparison.
/// @param range Output range known on the selected edge.
/// @return `true` when a non-overflowing supported fact was derived.
bool deriveCompareBranchRange(const Instr &cmp,
                              size_t branchIndex,
                              Value &constrainedValue,
                              IntRange &range) {
    if (cmp.operands.size() != 2)
        return false;

    Opcode op = cmp.op;
    Value variable;
    int64_t constant = 0;

    if (cmp.operands[0].kind == Value::Kind::Temp &&
        cmp.operands[1].kind == Value::Kind::ConstInt) {
        variable = cmp.operands[0];
        constant = cmp.operands[1].i64;
    } else if (cmp.operands[0].kind == Value::Kind::ConstInt &&
               cmp.operands[1].kind == Value::Kind::Temp) {
        variable = cmp.operands[1];
        constant = cmp.operands[0].i64;
        switch (op) {
            case Opcode::SCmpLT:
                op = Opcode::SCmpGT;
                break;
            case Opcode::SCmpLE:
                op = Opcode::SCmpGE;
                break;
            case Opcode::SCmpGT:
                op = Opcode::SCmpLT;
                break;
            case Opcode::SCmpGE:
                op = Opcode::SCmpLE;
                break;
            default:
                break;
        }
    } else {
        return false;
    }

    if (variable.kind != Value::Kind::Temp)
        return false;

    IntRange fact;
    const bool trueBranch = branchIndex == 0;
    switch (op) {
        case Opcode::SCmpLT:
            if (trueBranch) {
                if (constant == std::numeric_limits<int64_t>::min())
                    return false;
                fact.upper = constant - 1;
            } else {
                fact.lower = constant;
            }
            break;
        case Opcode::SCmpLE:
            if (trueBranch) {
                fact.upper = constant;
            } else {
                if (constant == std::numeric_limits<int64_t>::max())
                    return false;
                fact.lower = constant + 1;
            }
            break;
        case Opcode::SCmpGT:
            if (trueBranch) {
                if (constant == std::numeric_limits<int64_t>::max())
                    return false;
                fact.lower = constant + 1;
            } else {
                fact.upper = constant;
            }
            break;
        case Opcode::SCmpGE:
            if (trueBranch) {
                fact.lower = constant;
            } else {
                if (constant == std::numeric_limits<int64_t>::min())
                    return false;
                fact.upper = constant - 1;
            }
            break;
        case Opcode::ICmpEq:
            if (!trueBranch)
                return false;
            fact = exactRange(constant);
            break;
        default:
            return false;
    }

    constrainedValue = variable;
    range = fact;
    return true;
}

/// @brief Collect range facts established on one predecessor edge.
/// @details Seeds exact facts for constant block arguments, derives a fact from
///          a local compare feeding `cbr`, and transfers that fact to matching
///          destination parameters.
/// @param pred Predecessor block containing @p term.
/// @param term Terminator whose selected edge targets @p target.
/// @param branchIndex Label and branch-argument bundle index.
/// @param target Destination block.
/// @return Temporary range facts holding on the selected edge.
std::unordered_map<unsigned, IntRange> edgeRangesForTarget(const BasicBlock &pred,
                                                           const Instr &term,
                                                           size_t branchIndex,
                                                           const BasicBlock &target) {
    std::unordered_map<unsigned, IntRange> facts;
    if (term.labels.size() <= branchIndex)
        return facts;

    if (branchIndex < term.brArgs.size()) {
        const auto &args = term.brArgs[branchIndex];
        for (size_t i = 0; i < args.size() && i < target.params.size(); ++i)
            if (args[i].kind == Value::Kind::ConstInt)
                facts[target.params[i].id] = exactRange(args[i].i64);
    }

    if (term.op != Opcode::CBr || term.operands.size() != 1)
        return facts;

    const Value &cond = term.operands.front();
    if (cond.kind != Value::Kind::Temp)
        return facts;

    const Instr *cmp = findLocalDef(pred, cond.id);
    if (!cmp)
        return facts;

    Value constrained;
    IntRange range;
    if (!deriveCompareBranchRange(*cmp, branchIndex, constrained, range))
        return facts;

    facts[constrained.id] = range;

    if (branchIndex >= term.brArgs.size())
        return facts;

    const auto &args = term.brArgs[branchIndex];
    for (size_t i = 0; i < args.size() && i < target.params.size(); ++i) {
        if (valueEquals(args[i], constrained))
            facts[target.params[i].id] = range;
    }

    return facts;
}

/// @brief Merge range facts common to every incoming edge of the current block.
/// @details Facts absent on any predecessor are discarded; matching bounds are
///          conservatively joined with @ref mergeIncomingRange.
/// @param ctx Verification context whose block is the merge target.
/// @return Facts guaranteed for all discovered incoming edges.
std::unordered_map<unsigned, IntRange> collectIncomingRanges(const VerifyCtx &ctx) {
    std::unordered_map<unsigned, IntRange> merged;
    bool sawPred = false;

    for (const BasicBlock &pred : ctx.fn.blocks) {
        if (pred.instructions.empty())
            continue;
        const Instr &term = pred.instructions.back();
        for (size_t branchIndex = 0; branchIndex < term.labels.size(); ++branchIndex) {
            if (term.labels[branchIndex] != ctx.block.label)
                continue;

            auto edgeFacts = edgeRangesForTarget(pred, term, branchIndex, ctx.block);
            if (!sawPred) {
                merged = std::move(edgeFacts);
                sawPred = true;
                continue;
            }

            for (auto it = merged.begin(); it != merged.end();) {
                auto rhs = edgeFacts.find(it->first);
                if (rhs == edgeFacts.end()) {
                    it = merged.erase(it);
                    continue;
                }
                auto combined = mergeIncomingRange(it->second, rhs->second);
                if (!combined) {
                    it = merged.erase(it);
                    continue;
                }
                it->second = *combined;
                ++it;
            }
        }
    }

    return merged;
}

/// @brief Compute a result range for a small set of arithmetic idioms.
/// @details Handles checked/plain add, subtract, multiply, nonnegative constant
///          masks, and positive constant logical shifts when operand facts are
///          sufficient and endpoint arithmetic is representable.
/// @param instr Instruction whose result is modeled.
/// @param ranges Current temporary range facts.
/// @return Proven result range, or no value when unsupported or unsafe.
std::optional<IntRange> computeInstructionRange(
    const Instr &instr, const std::unordered_map<unsigned, IntRange> &ranges) {
    if (instr.operands.size() < 2)
        return std::nullopt;

    auto lhs = rangeForValue(instr.operands[0], ranges);
    auto rhs = rangeForValue(instr.operands[1], ranges);
    switch (instr.op) {
        case Opcode::Add:
        case Opcode::IAddOvf:
            if (lhs && rhs)
                return addRanges(*lhs, *rhs);
            break;
        case Opcode::Sub:
        case Opcode::ISubOvf:
            if (lhs && rhs)
                return subRanges(*lhs, *rhs);
            break;
        case Opcode::Mul:
        case Opcode::IMulOvf:
            if (lhs && rhs)
                return mulRanges(*lhs, *rhs);
            break;
        case Opcode::And:
            if (instr.operands[1].kind == Value::Kind::ConstInt && instr.operands[1].i64 >= 0)
                return IntRange{0, instr.operands[1].i64};
            if (instr.operands[0].kind == Value::Kind::ConstInt && instr.operands[0].i64 >= 0)
                return IntRange{0, instr.operands[0].i64};
            break;
        case Opcode::LShr:
            if (instr.operands[1].kind == Value::Kind::ConstInt && instr.operands[1].i64 > 0 &&
                instr.operands[1].i64 < 64)
                return IntRange{0, std::numeric_limits<int64_t>::max()};
            break;
        default:
            break;
    }
    return std::nullopt;
}

/// @brief Compute whole-function range facts holding just before @p ctx.instr.
/// @details Seeds the block from the fixpoint value-range analysis and replays
///          the block's instructions up to (not including) the checked one.
///          This is the same prover CheckOpt uses to justify demotions, so
///          every demotion the optimizer performs re-verifies here.
/// @param ctx Verification context identifying the function, block, and limit.
/// @return Range map holding immediately before the current instruction.
zanna::analysis::RangeMap globalRangesBeforeInstruction(const VerifyCtx &ctx) {
    const zanna::analysis::IntRangeInfo info = zanna::analysis::computeIntRanges(ctx.fn);
    zanna::analysis::RangeMap ranges;
    if (const zanna::analysis::RangeMap *seeded = info.entryFor(ctx.block.label))
        ranges = *seeded;
    for (const Instr &instr : ctx.block.instructions) {
        if (&instr == &ctx.instr)
            break;
        zanna::analysis::applyRangeTransfer(instr, ranges);
        // Fill the gap the peephole srem-lowering leaves (see
        // matchPow2ModuloRange), so idiom-derived bounds flow to later uses.
        if (instr.result && ranges.find(*instr.result) == ranges.end())
            if (auto idiom = zanna::analysis::matchPow2ModuloRange(ctx.block, instr, ranges))
                ranges[*instr.result] = *idiom;
    }
    return ranges;
}

/// @brief Prove that plain add, subtract, or multiply is a safe checked-op demotion.
/// @details Replays local incoming-edge facts first, then uses whole-function
///          integer range analysis and the power-of-two modulo idiom recognized
///          by CheckOpt.
/// @param ctx Verification context for the otherwise rejected plain operation.
/// @return `true` when a verifier-visible range proof establishes no overflow.
bool isVerifiedCheckedArithmeticDemotion(const VerifyCtx &ctx) {
    if (!(ctx.instr.op == Opcode::Add || ctx.instr.op == Opcode::Sub ||
          ctx.instr.op == Opcode::Mul)) {
        return false;
    }

    auto ranges = collectIncomingRanges(ctx);
    for (const Instr &instr : ctx.block.instructions) {
        if (&instr == &ctx.instr) {
            if (computeInstructionRange(ctx.instr, ranges).has_value())
                return true;
            break;
        }

        if (auto resultRange = computeInstructionRange(instr, ranges)) {
            if (instr.result)
                ranges[*instr.result] = *resultRange;
        } else if (instr.result) {
            ranges.erase(*instr.result);
        }
    }

    // Local block facts were not enough; retry with the whole-function
    // value-range fixpoint (the prover CheckOpt's demotions rely on).
    zanna::analysis::RangeMap globalRanges = globalRangesBeforeInstruction(ctx);
    if (zanna::analysis::applyRangeTransfer(ctx.instr, globalRanges).has_value())
        return true;
    // The checked op may itself be the modulo idiom's inner subtract.
    return zanna::analysis::matchPow2ModuloRange(ctx.block, ctx.instr, globalRanges).has_value();
}

/// @brief Find a local temporary definition strictly before an instruction.
/// @param block Block scanned in instruction order.
/// @param limit Instruction that terminates the search.
/// @param id Temporary result identifier to find.
/// @return Definition before @p limit, or null when absent or after the limit.
const Instr *findLocalDefBefore(const BasicBlock &block, const Instr &limit, unsigned id) {
    for (const Instr &instr : block.instructions) {
        if (&instr == &limit)
            return nullptr;
        if (instr.result && *instr.result == id)
            return &instr;
    }
    return nullptr;
}

/// @brief Recognize the sign-bias term used in signed power-of-two division.
/// @details Matches `(ashr dividend, 63) & nonnegative_mask` through local
///          definitions preceding @p limit.
/// @param block Block containing the candidate idiom.
/// @param limit Instruction after the candidate definitions.
/// @param dividend Original signed dividend.
/// @param biasValue Candidate bias temporary.
/// @return `true` when the exact idiom is present.
bool isSignBiasForDividend(const BasicBlock &block,
                           const Instr &limit,
                           const Value &dividend,
                           const Value &biasValue) {
    if (dividend.kind != Value::Kind::Temp || biasValue.kind != Value::Kind::Temp)
        return false;

    const Instr *bias = findLocalDefBefore(block, limit, biasValue.id);
    if (!bias || bias->op != Opcode::And || bias->operands.size() != 2 ||
        bias->operands[0].kind != Value::Kind::Temp ||
        bias->operands[1].kind != Value::Kind::ConstInt || bias->operands[1].i64 < 0) {
        return false;
    }

    const Instr *sign = findLocalDefBefore(block, *bias, bias->operands[0].id);
    return sign && sign->op == Opcode::AShr && sign->operands.size() == 2 &&
           valueEquals(sign->operands[0], dividend) &&
           sign->operands[1].kind == Value::Kind::ConstInt && sign->operands[1].i64 == 63;
}

/// @brief Recognize a plain add introduced for the signed division bias idiom.
/// @param ctx Verification context for the otherwise rejected add.
/// @return `true` when either operand is a valid sign bias for the other.
bool isVerifiedSignBiasAddDemotion(const VerifyCtx &ctx) {
    if (ctx.instr.op != Opcode::Add || ctx.instr.operands.size() != 2)
        return false;
    return isSignBiasForDividend(
               ctx.block, ctx.instr, ctx.instr.operands[0], ctx.instr.operands[1]) ||
           isSignBiasForDividend(
               ctx.block, ctx.instr, ctx.instr.operands[1], ctx.instr.operands[0]);
}

/// @brief Validate one false-edge proof for a checked-sub demotion.
/// @details CheckOpt rewrites `%y = isub.ovf %x, K` to `%y = sub %x, K` only
///          on the false edge of `cbr (icmp.sle %x, T), overflow, work`.  The
///          false edge establishes `%x >= T + 1`; that is sufficient for
///          `x - K` when `K >= 0` and `T + 1 >= INT64_MIN + K`.
/// @param ctx Current instruction verification context.
/// @param pred Predecessor block that branches to the current block.
/// @param term Predecessor terminator.
/// @return True if the predecessor's edge proves the current `sub` cannot
///         signed-overflow.
bool falseEdgeProvesCheckedSubDemotion(const VerifyCtx &ctx,
                                       const BasicBlock &pred,
                                       const Instr &term) {
    if (term.op != Opcode::CBr || term.operands.size() != 1 || term.labels.size() < 2)
        return false;
    if (term.labels[1] != ctx.block.label)
        return false;
    if (term.labels[0] == ctx.block.label)
        return false;

    const Value &cond = term.operands.front();
    if (cond.kind != Value::Kind::Temp)
        return false;
    const Instr *cmp = findLocalDef(pred, cond.id);
    if (!cmp || cmp->op != Opcode::SCmpLE || cmp->operands.size() != 2)
        return false;

    if (ctx.instr.operands.size() != 2)
        return false;
    const Value &lhs = ctx.instr.operands[0];
    const Value &rhs = ctx.instr.operands[1];
    if (rhs.kind != Value::Kind::ConstInt || rhs.i64 < 0)
        return false;
    if (!valueEquals(cmp->operands[0], lhs))
        return false;
    if (cmp->operands[1].kind != Value::Kind::ConstInt)
        return false;

    const long long threshold = cmp->operands[1].i64;
    if (threshold == std::numeric_limits<long long>::max())
        return false;

    const long long lowerBound = threshold + 1;
    const long long requiredLowerBound = std::numeric_limits<long long>::min() + rhs.i64;
    return lowerBound >= requiredLowerBound;
}

/// @brief Decide whether a rejected signed `sub` has a verifier-visible proof.
/// @details Plain signed arithmetic remains rejected by default.  The verifier
///          accepts `sub` only when every incoming edge to the block carries the
///          exact lower-bound proof emitted by CheckOpt.
/// @param ctx Verification context for the rejected instruction.
/// @return True when all incoming edges prove signed subtraction cannot trap.
bool isVerifiedCheckedSubDemotion(const VerifyCtx &ctx) {
    if (ctx.instr.op != Opcode::Sub || ctx.instr.operands.size() != 2)
        return false;
    bool sawIncomingEdge = false;
    for (const BasicBlock &pred : ctx.fn.blocks) {
        if (pred.instructions.empty())
            continue;
        const Instr &term = pred.instructions.back();
        for (const std::string &target : term.labels) {
            if (target != ctx.block.label)
                continue;
            sawIncomingEdge = true;
            if (!falseEdgeProvesCheckedSubDemotion(ctx, pred, term))
                return false;
        }
    }
    return sawIncomingEdge;
}

/// @brief Accept a plain div/rem whose divisor is range-proven safe.
/// @details Mirrors CheckOpt's range-backed demotion: the divisor's value
///          range must exclude 0, and the signed forms additionally need the
///          divisor range to exclude -1 or the dividend range to exclude
///          INT64_MIN (the INT64_MIN / -1 overflow case).
/// @param ctx Verification context for the candidate plain div/rem.
/// @return `true` when range facts exclude every trapping case.
bool isRangeProvenDivRemDemotion(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() != 2)
        return false;

    zanna::analysis::RangeMap ranges = globalRangesBeforeInstruction(ctx);
    auto divisorRange = zanna::analysis::rangeForValue(ctx.instr.operands[1], ranges);
    if (!divisorRange)
        return false;

    const bool excludesZero = (divisorRange->lower && *divisorRange->lower > 0) ||
                              (divisorRange->upper && *divisorRange->upper < 0);
    if (!excludesZero)
        return false;

    if (ctx.instr.op == Opcode::SDiv || ctx.instr.op == Opcode::SRem) {
        const bool excludesMinusOne = (divisorRange->lower && *divisorRange->lower > -1) ||
                                      (divisorRange->upper && *divisorRange->upper < -1);
        auto dividendRange = zanna::analysis::rangeForValue(ctx.instr.operands[0], ranges);
        const bool dividendNotMin = dividendRange && dividendRange->lower &&
                                    *dividendRange->lower > std::numeric_limits<int64_t>::min();
        if (!excludesMinusOne && !dividendNotMin)
            return false;
    }
    return true;
}

/// @brief Prove that a plain division or remainder is a safe checked-op demotion.
/// @details Accepts statically safe constant divisors, including the signed
///          MIN/-1 special case only for a safe constant dividend. Nonconstant
///          divisors require the range proof mirrored by
///          @ref isRangeProvenDivRemDemotion. Records the result type on success.
/// @param ctx Verification context for the otherwise rejected operation.
/// @return `true` when division by zero and signed overflow are impossible.
bool isVerifiedCheckedDivRemDemotion(const VerifyCtx &ctx) {
    switch (ctx.instr.op) {
        case Opcode::SDiv:
        case Opcode::UDiv:
        case Opcode::SRem:
        case Opcode::URem:
            break;
        default:
            return false;
    }

    if (ctx.instr.operands.size() != 2 || ctx.instr.operands[1].kind != Value::Kind::ConstInt) {
        if (isRangeProvenDivRemDemotion(ctx)) {
            ctx.types.recordResult(ctx.instr, resolveResultType(ctx, getInstructionSpec(ctx.instr.op)));
            return true;
        }
        return false;
    }

    const int64_t divisor = ctx.instr.operands[1].i64;
    if (divisor == 0)
        return false;

    const bool signedMinDivisorOverflow =
        (ctx.instr.op == Opcode::SDiv || ctx.instr.op == Opcode::SRem) && divisor == -1;
    if (!signedMinDivisorOverflow) {
        ctx.types.recordResult(ctx.instr, resolveResultType(ctx, getInstructionSpec(ctx.instr.op)));
        return true;
    }

    const Value &lhs = ctx.instr.operands[0];
    if (lhs.kind != Value::Kind::ConstInt || lhs.i64 == std::numeric_limits<int64_t>::min())
        return false;

    ctx.types.recordResult(ctx.instr, resolveResultType(ctx, getInstructionSpec(ctx.instr.op)));
    return true;
}

/// @brief Force verification failure for explicitly rejected opcodes.
/// @details First accepts a narrow set of optimizer-produced plain arithmetic
///          operations with verifier-visible safety proofs. Otherwise emits the
///          rejection message provided by the specification.
/// @param ctx Verification context for the instruction.
/// @param spec Specification entry containing the rejection message.
/// @return Success for a proven demotion; otherwise a failure diagnostic.
Expected<void> applyReject(const VerifyCtx &ctx, const InstructionSpec &spec) {
    if (isVerifiedCheckedArithmeticDemotion(ctx)) {
        ctx.types.recordResult(ctx.instr, resolveResultType(ctx, spec));
        return {};
    }
    if (isVerifiedSignBiasAddDemotion(ctx)) {
        ctx.types.recordResult(ctx.instr, resolveResultType(ctx, spec));
        return {};
    }
    if (isVerifiedCheckedSubDemotion(ctx)) {
        ctx.types.recordResult(ctx.instr, resolveResultType(ctx, spec));
        return {};
    }
    if (isVerifiedCheckedDivRemDemotion(ctx))
        return {};
    const char *message = spec.rejectMessage ? spec.rejectMessage : "opcode rejected";
    return fail(ctx, message);
}

constexpr std::array<StrategyFn, static_cast<size_t>(VerifyStrategy::Count)> kStrategyTable = {
    &applyDefault,
    &applyAlloca,
    &applyGEP,
    &applyLoad,
    &applyStore,
    &applyAddrOf,
    &applyConstStr,
    &applyGAddr,
    &applyConstNull,
    &applyCall,
    &applyTrapKind,
    &applyTrapFromErr,
    &applyTrapErr,
    &applyIdxChk,
    &applyCastFpToSiRteChk,
    &applyCastFpToUiRteChk,
    &applyCastSiNarrowChk,
    &applyCastUiNarrowChk,
    &applyIntegerBinary,
    &applyShift,
    &applyReject,
};

static_assert(kStrategyTable.size() == static_cast<size_t>(VerifyStrategy::Count),
              "strategy table must cover every enumerator");

/// @brief Execute the strategy-specific verifier for an instruction.
/// @details Looks up the appropriate strategy entry and invokes it.  Unknown
///          strategies fall back to the default checker so the verifier remains
///          robust even when generated data is incomplete.
/// @param ctx Verification context describing the instruction.
/// @param spec Specification entry obtained from the opcode table.
/// @return Success or diagnostic error from the strategy implementation.
Expected<void> dispatchStrategy(const VerifyCtx &ctx, const InstructionSpec &spec) {
    const size_t index = static_cast<size_t>(spec.strategy);
    if (index >= kStrategyTable.size()) {
        ctx.types.recordResult(ctx.instr, resolveResultType(ctx, spec));
        return checkDefault(ctx);
    }
    return kStrategyTable[index](ctx, spec);
}

/// @brief Run operand/result structural checks generated from the opcode table.
/// @details Validates operand counts, operand types, and result arity before any
///          strategy-specific verification occurs.  Failures are reported as
///          diagnostics collected from the helper checkers.
/// @param ctx Verification context describing the instruction.
/// @param spec Specification entry obtained from the opcode table.
/// @return Success or diagnostic error when checks fail.
Expected<void> runStructuralChecks(const VerifyCtx &ctx, const InstructionSpec &spec) {
    detail::OperandCountChecker countChecker(ctx, spec);
    if (auto result = countChecker.run(); !result)
        return result;

    detail::OperandTypeChecker typeChecker(ctx, spec);
    if (auto result = typeChecker.run(); !result)
        return result;

    detail::ResultTypeChecker resultChecker(ctx, spec);
    return resultChecker.run();
}

/// @brief Verify an instruction using the shared context helper.
/// @details Fetches the generated specification, runs structural checks, and
///          dispatches to the appropriate verification strategy.
/// @param ctx Verification context describing the instruction.
/// @return Success or diagnostic error.
Expected<void> verifyInstruction_impl(const VerifyCtx &ctx) {
    const InstructionSpec &spec = getInstructionSpec(ctx.instr.op);
    if (auto result = runStructuralChecks(ctx, spec); !result)
        return result;
    return dispatchStrategy(ctx, spec);
}

/// @brief Verify instruction structure without consulting external metadata.
/// @details Performs operand/result count checks for front-end generated IL
///          before the full verification context is available.  Used by
///          parsing/printing routines to confirm textual IL obeys the schema.
/// @param fn Function containing the instruction.
/// @param bb Basic block owning the instruction.
/// @param instr Instruction under verification.
/// @return Success or diagnostic error.
Expected<void> verifyOpcodeSignature_impl(const il::core::Function &fn,
                                          const il::core::BasicBlock &bb,
                                          const il::core::Instr &instr) {
    const InstructionSpec &spec = getInstructionSpec(instr.op);

    const bool hasResult = instr.result.has_value();
    switch (spec.resultArity) {
        case il::core::ResultArity::None:
            if (hasResult)
                return Expected<void>(
                    makeError(instr.loc, formatInstrDiag(fn, bb, instr, "unexpected result")));
            break;
        case il::core::ResultArity::One:
            if (!hasResult)
                return Expected<void>(
                    makeError(instr.loc, formatInstrDiag(fn, bb, instr, "missing result")));
            break;
        case il::core::ResultArity::Optional:
            break;
    }

    const size_t operandCount = instr.operands.size();
    const bool variadicOperands = il::core::isVariadicOperandCount(spec.numOperandsMax);
    if (operandCount < spec.numOperandsMin ||
        (!variadicOperands && operandCount > spec.numOperandsMax)) {
        std::string message;
        if (spec.numOperandsMin == spec.numOperandsMax && !variadicOperands) {
            message = "expected " + std::to_string(static_cast<unsigned>(spec.numOperandsMin)) +
                      " operand";
            if (spec.numOperandsMin != 1)
                message += 's';
        } else if (variadicOperands) {
            message = "expected at least " +
                      std::to_string(static_cast<unsigned>(spec.numOperandsMin)) + " operand";
            if (spec.numOperandsMin != 1)
                message += 's';
        } else {
            message = "expected between " +
                      std::to_string(static_cast<unsigned>(spec.numOperandsMin)) + " and " +
                      std::to_string(static_cast<unsigned>(spec.numOperandsMax)) + " operands";
        }
        return Expected<void>(makeError(instr.loc, formatInstrDiag(fn, bb, instr, message)));
    }

    const bool variadicSucc = il::core::isVariadicSuccessorCount(spec.numSuccessors);
    if (variadicSucc) {
        if (instr.labels.empty())
            return Expected<void>(makeError(
                instr.loc, formatInstrDiag(fn, bb, instr, "expected at least 1 successor")));
    } else {
        if (instr.labels.size() != spec.numSuccessors) {
            std::string message = "expected " +
                                  std::to_string(static_cast<unsigned>(spec.numSuccessors)) +
                                  " successor";
            if (spec.numSuccessors != 1)
                message += 's';
            return Expected<void>(makeError(instr.loc, formatInstrDiag(fn, bb, instr, message)));
        }
    }

    if (variadicSucc) {
        if (!instr.brArgs.empty() && instr.brArgs.size() != instr.labels.size()) {
            return Expected<void>(makeError(
                instr.loc,
                formatInstrDiag(fn,
                                bb,
                                instr,
                                "branch arg count mismatch: expected branch argument bundle per "
                                "successor or none")));
        }
    } else {
        if (instr.brArgs.size() > spec.numSuccessors) {
            std::string message = "expected at most " +
                                  std::to_string(static_cast<unsigned>(spec.numSuccessors)) +
                                  " branch argument bundle";
            if (spec.numSuccessors != 1)
                message += 's';
            return Expected<void>(makeError(instr.loc, formatInstrDiag(fn, bb, instr, message)));
        }
        if (!instr.brArgs.empty() && instr.brArgs.size() != spec.numSuccessors) {
            std::string message = "expected " +
                                  std::to_string(static_cast<unsigned>(spec.numSuccessors)) +
                                  " branch argument bundle";
            if (spec.numSuccessors != 1)
                message += 's';
            message += ", or none";
            message = "branch arg count mismatch: " + message;
            return Expected<void>(makeError(instr.loc, formatInstrDiag(fn, bb, instr, message)));
        }
    }

    return {};
}

} // namespace

/// @brief Public Expected-returning wrapper for instruction verification.
/// @param ctx Verification context describing the instruction.
/// @return Success or diagnostic error.
Expected<void> verifyInstruction_E(const VerifyCtx &ctx) {
    return verifyInstruction_impl(ctx);
}

/// @brief Wrapper that verifies structure using the data stored in @p ctx.
/// @param ctx Verification context describing the instruction.
/// @return Success or diagnostic error.
Expected<void> verifyOpcodeSignature_E(const VerifyCtx &ctx) {
    return verifyOpcodeSignature_impl(ctx.fn, ctx.block, ctx.instr);
}

/// @brief Verify an instruction using explicit context components.
/// @details Convenience overload that constructs a @ref VerifyCtx on the fly so
///          callers without a pre-built context can run the verifier.
/// @param fn Function containing the instruction.
/// @param bb Basic block owning the instruction.
/// @param instr Instruction to verify.
/// @param externs Map of known extern declarations for call validation.
/// @param funcs Map of known functions for call validation.
/// @param types Type inference cache used to record results.
/// @param sink Diagnostic sink receiving errors and warnings.
/// @return Success or diagnostic error.
Expected<void> verifyInstruction_E(
    const il::core::Function &fn,
    const il::core::BasicBlock &bb,
    const il::core::Instr &instr,
    const std::unordered_map<std::string, const il::core::Extern *> &externs,
    const std::unordered_map<std::string, const il::core::Function *> &funcs,
    TypeInference &types,
    DiagSink &sink) {
    static const std::unordered_map<std::string, const il::core::Global *> kNoGlobals;
    VerifyCtx ctx{sink, types, externs, funcs, kNoGlobals, fn, bb, instr};
    return verifyInstruction_impl(ctx);
}

/// @brief Verify opcode structure using explicit function/block inputs.
/// @param fn Function containing the instruction.
/// @param bb Basic block owning the instruction.
/// @param instr Instruction under verification.
/// @return Success or diagnostic error.
Expected<void> verifyOpcodeSignature_E(const il::core::Function &fn,
                                       const il::core::BasicBlock &bb,
                                       const il::core::Instr &instr) {
    return verifyOpcodeSignature_impl(fn, bb, instr);
}

/// @brief Verify opcode structure and print diagnostics to a stream.
/// @details Calls the Expected-returning variant and forwards any diagnostics to
///          the provided stream for human consumption.
/// @param fn Function containing the instruction.
/// @param bb Basic block owning the instruction.
/// @param instr Instruction under verification.
/// @param err Stream that receives diagnostic messages.
/// @return True on success; false when verification failed.
bool verifyOpcodeSignature(const il::core::Function &fn,
                           const il::core::BasicBlock &bb,
                           const il::core::Instr &instr,
                           std::ostream &err) {
    if (auto result = verifyOpcodeSignature_E(fn, bb, instr); !result) {
        il::support::printDiag(result.error(), err);
        return false;
    }
    return true;
}

/// @brief Verify an instruction and print diagnostics to a stream.
/// @details Accumulates warnings in a temporary sink so both warnings and errors
///          can be emitted in order when verification fails.
/// @param fn Function containing the instruction.
/// @param bb Basic block owning the instruction.
/// @param instr Instruction under verification.
/// @param externs Map of known extern declarations for call validation.
/// @param funcs Map of known functions for call validation.
/// @param types Type inference cache used to record results.
/// @param err Stream that receives diagnostic messages.
/// @return True on success; false otherwise.
bool verifyInstruction(const il::core::Function &fn,
                       const il::core::BasicBlock &bb,
                       const il::core::Instr &instr,
                       const std::unordered_map<std::string, const il::core::Extern *> &externs,
                       const std::unordered_map<std::string, const il::core::Function *> &funcs,
                       TypeInference &types,
                       std::ostream &err) {
    CollectingDiagSink sink;
    if (auto result = verifyInstruction_E(fn, bb, instr, externs, funcs, types, sink); !result) {
        for (const auto &warning : sink.diagnostics())
            il::support::printDiag(warning, err);
        il::support::printDiag(result.error(), err);
        return false;
    }

    for (const auto &warning : sink.diagnostics())
        il::support::printDiag(warning, err);
    return true;
}

/// @brief Convenience wrapper returning Expected for signature verification.
/// @param fn Function containing the instruction.
/// @param bb Basic block owning the instruction.
/// @param instr Instruction under verification.
/// @return Success or diagnostic error.
Expected<void> verifyOpcodeSignature_expected(const il::core::Function &fn,
                                              const il::core::BasicBlock &bb,
                                              const il::core::Instr &instr) {
    return verifyOpcodeSignature_E(fn, bb, instr);
}

/// @brief Convenience wrapper returning Expected for instruction verification.
/// @param fn Function containing the instruction.
/// @param bb Basic block owning the instruction.
/// @param instr Instruction under verification.
/// @param externs Map of known extern declarations for call validation.
/// @param funcs Map of known functions for call validation.
/// @param types Type inference cache used to record results.
/// @param sink Diagnostic sink receiving errors and warnings.
/// @return Success or diagnostic error.
Expected<void> verifyInstruction_expected(
    const il::core::Function &fn,
    const il::core::BasicBlock &bb,
    const il::core::Instr &instr,
    const std::unordered_map<std::string, const il::core::Extern *> &externs,
    const std::unordered_map<std::string, const il::core::Function *> &funcs,
    TypeInference &types,
    DiagSink &sink) {
    return verifyInstruction_E(fn, bb, instr, externs, funcs, types, sink);
}

} // namespace il::verify
