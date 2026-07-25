//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/fastpaths/FastPaths_Call.cpp
// Purpose: Fast-path pattern matching for call operations.
//          Handles call @callee(args...) → ret patterns with register and stack
//          argument marshalling, cycle detection/breaking for register moves,
//          and temporary computation into scratch registers.
// Key invariants:
//   - Call result must flow directly to a ret instruction.
//   - Arguments must be entry params, constants, or simple computations.
//   - Uses scratch registers for intermediate computations.
// Ownership/Lifetime:
//   - Stateless free functions; FastPathContext is borrowed for the call duration.
// Links: src/codegen/aarch64/fastpaths/FastPathsInternal.hpp,
//        src/codegen/aarch64/LoweringContext.hpp
//
//===----------------------------------------------------------------------===//

#include "FastPathsInternal.hpp"
#include "codegen/aarch64/A64ImmediateUtils.hpp"
#include "codegen/aarch64/LoweringContext.hpp"
#include "il/runtime/RuntimeNameMap.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include <unordered_map>
#include <unordered_set>

/**
 * @file
 * @brief Implements AArch64 direct-call-and-return fast-path lowering.
 *
 * The fast path proves that preceding IL can be safely elided or
 * rematerialized, marshals register/stack and variadic arguments, preserves
 * parameter-home semantics, resolves register-move cycles, and returns the
 * direct callee result.
 */

namespace zanna::codegen::aarch64::fastpaths {

using il::core::Opcode;

namespace {

/// @brief Move descriptor for register-to-register marshalling.
struct Move {
    PhysReg dst;
    PhysReg src;
};

/// @brief Scratch register pool for temporary computations.
/// @details Uses only globally reserved scratch registers so fast-path call
///          marshalling cannot clobber values assigned by register allocation.
constexpr std::size_t kScratchPoolSize = 2;
const PhysReg scratchPool[kScratchPoolSize] = {kScratchGPR, kScratchGPR2};

/**
 * @brief Resolves an IL temporary to its entry-parameter index.
 * @param bb Entry block whose parameters are searched.
 * @param argOrder ABI argument order retained for the shared helper signature.
 * @param v Candidate IL value.
 * @param[out] outIdx Parameter index on success.
 * @return `true` when @p v names an entry parameter.
 */
bool isParamTemp(const il::core::BasicBlock &bb,
                 const std::array<PhysReg, kMaxGPRArgs> &argOrder,
                 const il::core::Value &v,
                 unsigned &outIdx) {
    if (v.kind != il::core::Value::Kind::Temp)
        return false;
    int p = indexOfParam(bb, v.id);
    if (p >= 0) {
        outIdx = static_cast<unsigned>(p);
        return true;
    }
    return false;
}

/**
 * @brief Builds entry-parameter to home-alloca associations.
 *
 * Only stores whose destination was defined by an `Alloca` in the same block
 * and whose value is a temporary are considered. The first home observed for
 * a parameter is retained.
 *
 * @param bb Block to scan.
 * @return Map from parameter temporary ID to alloca-result temporary ID.
 */
std::unordered_map<unsigned, unsigned> buildParamHomeAllocaMap(const il::core::BasicBlock &bb) {
    std::unordered_set<unsigned> localAllocas;
    for (const auto &instr : bb.instructions) {
        if (instr.op == Opcode::Alloca && instr.result)
            localAllocas.insert(*instr.result);
    }

    std::unordered_map<unsigned, unsigned> homes;
    for (const auto &instr : bb.instructions) {
        if (instr.op != Opcode::Store || instr.operands.size() < 2)
            continue;
        if (instr.operands[0].kind != il::core::Value::Kind::Temp ||
            instr.operands[1].kind != il::core::Value::Kind::Temp)
            continue;
        if (!localAllocas.contains(instr.operands[0].id))
            continue;
        homes.try_emplace(instr.operands[1].id, instr.operands[0].id);
    }
    return homes;
}

/**
 * @brief Rematerializes a supported pure producer into a physical scratch register.
 *
 * Handles entry-parameter integer arithmetic, shifts, and comparisons in
 * register/register or parameter/immediate form.
 *
 * @param prod Producer instruction to reproduce.
 * @param dstReg Physical destination register.
 * @param bb Entry block used to resolve parameter operands.
 * @param argOrder Integer ABI argument-register order.
 * @param[in,out] bbMir Block receiving rematerialization instructions.
 * @return `true` when the producer shape was supported and emitted.
 */
bool computeTempTo(const il::core::Instr &prod,
                   PhysReg dstReg,
                   const il::core::BasicBlock &bb,
                   const std::array<PhysReg, kMaxGPRArgs> &argOrder,
                   MBasicBlock &bbMir) {
    /// @brief Emits a three-register operation from two entry parameters.
    auto rr_emit = [&](MOpcode opc, unsigned p0, unsigned p1) {
        const PhysReg r0 = argOrder[p0];
        const PhysReg r1 = argOrder[p1];
        bbMir.instrs.push_back(
            MInstr{opc, {MOperand::regOp(dstReg), MOperand::regOp(r0), MOperand::regOp(r1)}});
    };

    /// @brief Emits or legalizes a parameter/immediate operation.
    auto ri_emit = [&](MOpcode opc, unsigned p0, long long imm) {
        const PhysReg r0 = argOrder[p0];
        if (opc == MOpcode::AddRI || opc == MOpcode::SubRI || opc == MOpcode::AddOvfRI ||
            opc == MOpcode::SubOvfRI) {
            emitLegalizedSignedImmArith(
                bbMir,
                MOperand::regOp(dstReg),
                MOperand::regOp(r0),
                imm,
                (opc == MOpcode::AddRI || opc == MOpcode::AddOvfRI) ? SignedImmArithKind::Add
                                                                    : SignedImmArithKind::Sub,
                (opc == MOpcode::AddOvfRI || opc == MOpcode::SubOvfRI) ? MOpcode::AddOvfRI
                                                                       : MOpcode::AddRI,
                (opc == MOpcode::AddOvfRI || opc == MOpcode::SubOvfRI) ? MOpcode::SubOvfRI
                                                                       : MOpcode::SubRI,
                (opc == MOpcode::AddOvfRI || opc == MOpcode::SubOvfRI) ? MOpcode::AddOvfRRR
                                                                       : MOpcode::AddRRR,
                (opc == MOpcode::AddOvfRI || opc == MOpcode::SubOvfRI) ? MOpcode::SubOvfRRR
                                                                       : MOpcode::SubRRR,
                /// @brief Materializes a non-encodable immediate in reserved `x16`.
                [&](long long materializedImm) {
                    const MOperand scratch = MOperand::regOp(PhysReg::X16);
                    bbMir.instrs.push_back(
                        MInstr{MOpcode::MovRI, {scratch, MOperand::immOp(materializedImm)}});
                    return scratch;
                });
            return;
        }
        bbMir.instrs.push_back(
            MInstr{opc, {MOperand::regOp(dstReg), MOperand::regOp(r0), MOperand::immOp(imm)}});
    };

    // RR patterns: both operands are entry params
    if (prod.op == Opcode::Add || prod.op == Opcode::IAddOvf || prod.op == Opcode::Sub ||
        prod.op == Opcode::ISubOvf || prod.op == Opcode::Mul || prod.op == Opcode::IMulOvf ||
        prod.op == Opcode::And || prod.op == Opcode::Or || prod.op == Opcode::Xor) {
        if (prod.operands.size() != 2)
            return false;
        if (prod.operands[0].kind == il::core::Value::Kind::Temp &&
            prod.operands[1].kind == il::core::Value::Kind::Temp) {
            int i0 = indexOfParam(bb, prod.operands[0].id);
            int i1 = indexOfParam(bb, prod.operands[1].id);
            if (i0 >= 0 && i1 >= 0 && static_cast<std::size_t>(i0) < kMaxGPRArgs &&
                static_cast<std::size_t>(i1) < kMaxGPRArgs) {
                MOpcode opc = MOpcode::AddRRR;
                if (prod.op == Opcode::Add)
                    opc = MOpcode::AddRRR;
                else if (prod.op == Opcode::IAddOvf)
                    opc = MOpcode::AddOvfRRR;
                else if (prod.op == Opcode::Sub)
                    opc = MOpcode::SubRRR;
                else if (prod.op == Opcode::ISubOvf)
                    opc = MOpcode::SubOvfRRR;
                else if (prod.op == Opcode::Mul)
                    opc = MOpcode::MulRRR;
                else if (prod.op == Opcode::IMulOvf)
                    opc = MOpcode::MulOvfRRR;
                else if (prod.op == Opcode::And)
                    opc = MOpcode::AndRRR;
                else if (prod.op == Opcode::Or)
                    opc = MOpcode::OrrRRR;
                else if (prod.op == Opcode::Xor)
                    opc = MOpcode::EorRRR;
                rr_emit(opc, static_cast<unsigned>(i0), static_cast<unsigned>(i1));
                return true;
            }
        }
    }

    // RI patterns: param + imm for add/sub/shift
    if (prod.op == Opcode::Shl || prod.op == Opcode::LShr || prod.op == Opcode::AShr ||
        prod.op == Opcode::Add || prod.op == Opcode::IAddOvf || prod.op == Opcode::Sub ||
        prod.op == Opcode::ISubOvf) {
        if (prod.operands.size() != 2)
            return false;
        const auto &o0 = prod.operands[0];
        const auto &o1 = prod.operands[1];
        if (o0.kind == il::core::Value::Kind::Temp && o1.kind == il::core::Value::Kind::ConstInt) {
            int ip = indexOfParam(bb, o0.id);
            if (ip >= 0 && static_cast<std::size_t>(ip) < kMaxGPRArgs) {
                if (prod.op == Opcode::Shl)
                    ri_emit(MOpcode::LslRI, static_cast<unsigned>(ip), o1.i64);
                else if (prod.op == Opcode::LShr)
                    ri_emit(MOpcode::LsrRI, static_cast<unsigned>(ip), o1.i64);
                else if (prod.op == Opcode::AShr)
                    ri_emit(MOpcode::AsrRI, static_cast<unsigned>(ip), o1.i64);
                else if (prod.op == Opcode::Add)
                    ri_emit(MOpcode::AddRI, static_cast<unsigned>(ip), o1.i64);
                else if (prod.op == Opcode::IAddOvf)
                    ri_emit(MOpcode::AddOvfRI, static_cast<unsigned>(ip), o1.i64);
                else if (prod.op == Opcode::Sub)
                    ri_emit(MOpcode::SubRI, static_cast<unsigned>(ip), o1.i64);
                else if (prod.op == Opcode::ISubOvf)
                    ri_emit(MOpcode::SubOvfRI, static_cast<unsigned>(ip), o1.i64);
                return true;
            }
        } else if (o1.kind == il::core::Value::Kind::Temp &&
                   o0.kind == il::core::Value::Kind::ConstInt) {
            // Only commutative ops (add) can swap operands.
            // Shifts are NOT commutative: `const << param` != `param << const`.
            // Sub with const first is also not supported.
            if (prod.op == Opcode::Shl || prod.op == Opcode::LShr || prod.op == Opcode::AShr ||
                prod.op == Opcode::Sub || prod.op == Opcode::ISubOvf)
                return false;
            int ip = indexOfParam(bb, o1.id);
            if (ip >= 0 && static_cast<std::size_t>(ip) < kMaxGPRArgs) {
                if (prod.op == Opcode::Add)
                    ri_emit(MOpcode::AddRI, static_cast<unsigned>(ip), o0.i64);
                else if (prod.op == Opcode::IAddOvf)
                    ri_emit(MOpcode::AddOvfRI, static_cast<unsigned>(ip), o0.i64);
                return true;
            }
        }
    }

    // Compare patterns: produce 0/1 in dstReg via cmp + cset
    if (prod.op == Opcode::ICmpEq || prod.op == Opcode::ICmpNe || prod.op == Opcode::SCmpLT ||
        prod.op == Opcode::SCmpLE || prod.op == Opcode::SCmpGT || prod.op == Opcode::SCmpGE ||
        prod.op == Opcode::UCmpLT || prod.op == Opcode::UCmpLE || prod.op == Opcode::UCmpGT ||
        prod.op == Opcode::UCmpGE) {
        if (prod.operands.size() != 2)
            return false;
        const auto &o0 = prod.operands[0];
        const auto &o1 = prod.operands[1];
        const char *cc = lookupCondition(prod.op);
        if (!cc)
            return false;
        if (o0.kind == il::core::Value::Kind::Temp && o1.kind == il::core::Value::Kind::Temp) {
            int i0 = indexOfParam(bb, o0.id);
            int i1 = indexOfParam(bb, o1.id);
            if (i0 >= 0 && i1 >= 0 && static_cast<std::size_t>(i0) < kMaxGPRArgs &&
                static_cast<std::size_t>(i1) < kMaxGPRArgs) {
                const PhysReg r0 = argOrder[i0];
                const PhysReg r1 = argOrder[i1];
                bbMir.instrs.push_back(
                    MInstr{MOpcode::CmpRR, {MOperand::regOp(r0), MOperand::regOp(r1)}});
                bbMir.instrs.push_back(
                    MInstr{MOpcode::Cset, {MOperand::regOp(dstReg), MOperand::condOp(cc)}});
                return true;
            }
        }
        if (o0.kind == il::core::Value::Kind::Temp && o1.kind == il::core::Value::Kind::ConstInt) {
            int i0 = indexOfParam(bb, o0.id);
            if (i0 >= 0 && static_cast<std::size_t>(i0) < kMaxGPRArgs) {
                const PhysReg r0 = argOrder[i0];
                bbMir.instrs.push_back(
                    MInstr{MOpcode::CmpRI, {MOperand::regOp(r0), MOperand::immOp(o1.i64)}});
                bbMir.instrs.push_back(
                    MInstr{MOpcode::Cset, {MOperand::regOp(dstReg), MOperand::condOp(cc)}});
                return true;
            }
        }
    }
    return false;
}

} // namespace

/**
 * @brief Attempts a direct call whose result is returned immediately.
 *
 * Complex argument banks, floating-point values, varargs, strings, and boolean
 * results use generalized call lowering after recreating parameter homes.
 * Simpler integer-only calls use a physical-register move plan with cycle
 * breaking and limited pure-producer rematerialization.
 *
 * @param[in,out] ctx Fast-path state and output MIR.
 * @return Completed MIR function on a safe match, otherwise `std::nullopt`.
 */
std::optional<MFunction> tryCallFastPaths(FastPathContext &ctx) {
    if (ctx.fn.blocks.empty())
        return std::nullopt;

    const auto &bb = ctx.fn.blocks.front();
    auto &bbMir = ctx.bbOut(0);

    // =========================================================================
    // call @callee(args...) feeding ret
    // =========================================================================
    // Pattern: call @callee(args...) -> %r; ret %r
    // Marshals arguments into ABI registers/stack, emits bl, then ret
    if (ctx.fn.blocks.size() != 1 || bb.instructions.size() < 2 || bb.params.empty())
        return std::nullopt;

    const auto &binI = bb.instructions[bb.instructions.size() - 2];
    const auto &retI = bb.instructions.back();

    if (binI.op != Opcode::Call || retI.op != Opcode::Ret || !binI.result || retI.operands.empty())
        return std::nullopt;

    // The fast path replaces the WHOLE function body with (param spills +) the
    // final call + ret, re-materializing only the values that call consumes.
    // That is sound only when every preceding instruction is part of the
    // canonical entry sequence or a pure producer the marshaller can
    // recompute: allocas, entry-param home stores, loads of those param homes,
    // string-constant materializations, and side-effect-free arithmetic or
    // compare producers. Anything else (nested calls, other stores, ...) has
    // observable effects the replacement would silently drop — bail to the
    // generic lowering. Checked-overflow producers additionally must feed the
    // call itself so their trap semantics survive the re-materialization.
    {
        const auto paramHomes = buildParamHomeAllocaMap(bb);
        std::unordered_set<unsigned> homeAllocas;
        for (const auto &entry : paramHomes)
            homeAllocas.insert(entry.second);
        /// @brief Tests whether a preceding producer supplies a direct call argument.
        auto feedsCall = [&](const il::core::Instr &pre) {
            if (!pre.result)
                return false;
            for (const auto &arg : binI.operands) {
                if (arg.kind == il::core::Value::Kind::Temp && arg.id == *pre.result)
                    return true;
            }
            return false;
        };
        for (std::size_t i = 0; i + 2 < bb.instructions.size(); ++i) {
            const auto &pre = bb.instructions[i];
            switch (pre.op) {
                case Opcode::Alloca:
                case Opcode::ConstStr:
                    continue;
                case Opcode::Store:
                    if (pre.operands.size() == 2 &&
                        pre.operands[0].kind == il::core::Value::Kind::Temp &&
                        homeAllocas.contains(pre.operands[0].id) &&
                        pre.operands[1].kind == il::core::Value::Kind::Temp &&
                        indexOfParam(bb, pre.operands[1].id) >= 0)
                        continue;
                    return std::nullopt;
                case Opcode::Load:
                    if (!pre.operands.empty() &&
                        pre.operands[0].kind == il::core::Value::Kind::Temp &&
                        homeAllocas.contains(pre.operands[0].id))
                        continue;
                    return std::nullopt;
                case Opcode::IAddOvf:
                case Opcode::ISubOvf:
                    if (feedsCall(pre))
                        continue;
                    return std::nullopt;
                case Opcode::Add:
                case Opcode::Sub:
                case Opcode::Shl:
                case Opcode::LShr:
                case Opcode::AShr:
                case Opcode::ICmpEq:
                case Opcode::ICmpNe:
                case Opcode::SCmpLT:
                case Opcode::SCmpLE:
                case Opcode::SCmpGT:
                case Opcode::SCmpGE:
                case Opcode::UCmpLT:
                case Opcode::UCmpLE:
                case Opcode::UCmpGT:
                case Opcode::UCmpGE:
                    continue;
                default:
                    return std::nullopt;
            }
        }
    }

    const auto &retV = retI.operands[0];
    if (retV.kind != il::core::Value::Kind::Temp || retV.id != *binI.result || binI.callee.empty())
        return std::nullopt;

    std::string mappedCallee = binI.callee;
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(binI.callee))
        mappedCallee = std::string(*mapped);
    const bool hasKnownVarArg =
        ctx.knownVarArgNamedArgCounts &&
        (ctx.knownVarArgNamedArgCounts->find(mappedCallee) !=
             ctx.knownVarArgNamedArgCounts->end() ||
         ctx.knownVarArgNamedArgCounts->find(binI.callee) != ctx.knownVarArgNamedArgCounts->end());
    const bool isVarArg = il::runtime::isVarArgCallee(mappedCallee) ||
                          il::runtime::isVarArgCallee(binI.callee) || hasKnownVarArg;
    const bool needsGenericResultSemantics =
        binI.type.kind == il::core::Type::Kind::Str || binI.type.kind == il::core::Type::Kind::I1;

    // Check for floating-point arguments (requires vreg-based lowering)
    bool hasFloatArg = false;
    for (const auto &arg : binI.operands) {
        if (arg.kind == il::core::Value::Kind::ConstFloat) {
            hasFloatArg = true;
            break;
        }
        if (arg.kind == il::core::Value::Kind::Temp) {
            int p = indexOfParam(bb, arg.id);
            if (p >= 0 && p < static_cast<int>(bb.params.size()) &&
                bb.params[static_cast<std::size_t>(p)].type.kind == il::core::Type::Kind::F64) {
                hasFloatArg = true;
                break;
            }
        }
    }

    // Use generalized vreg-based lowering when we exceed register args or have floats.
    // IMPORTANT: lowerCallWithArgs uses materializeValueToVReg which traces Load→alloca
    // chains. The fast path normally skips the IL store instructions that write entry
    // parameters to their alloca slots (those stores happen in the generic lowering at
    // LowerILToMIR.cpp:220-320). We must emit those stores here first so that the
    // alloca slots contain valid data when the loads execute.
    if (binI.operands.size() > ctx.ti.intArgOrder.size() || hasFloatArg || isVarArg ||
        needsGenericResultSemantics) {
        const auto paramHomeAllocas = buildParamHomeAllocaMap(bb);

        // Emit param→alloca stores: for each IL `store TYPE, %alloca, %param`,
        // emit the corresponding MIR store from the ABI register to the alloca's
        // FP-relative frame slot.
        std::size_t gprIdx = 0;
        std::size_t fprIdx = 0;
        std::size_t stackArgIdx = 0;
        for (std::size_t pi = 0; pi < bb.params.size(); ++pi) {
            const auto &param = bb.params[pi];
            const bool isFP = param.type.kind == il::core::Type::Kind::F64;

            const auto homeIt = paramHomeAllocas.find(param.id);
            if (homeIt == paramHomeAllocas.end())
                return std::nullopt; // Can't map param to alloca, bail

            const int offset = ctx.fb.localOffset(homeIt->second);
            if (offset == 0)
                return std::nullopt;

            if (isFP) {
                if (fprIdx < ctx.ti.f64ArgOrder.size()) {
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::StrFprFpImm,
                        {MOperand::regOp(ctx.ti.f64ArgOrder[fprIdx]), MOperand::immOp(offset)}});
                    ++fprIdx;
                } else {
                    // FP stack arg: load from caller stack, store to alloca
                    const int callerOff = 16 + static_cast<int>(stackArgIdx) * 8;
                    ++stackArgIdx;
                    bbMir.instrs.push_back(
                        MInstr{MOpcode::LdrFprFpImm,
                               {MOperand::regOp(kScratchFPR), MOperand::immOp(callerOff)}});
                    bbMir.instrs.push_back(
                        MInstr{MOpcode::StrFprFpImm,
                               {MOperand::regOp(kScratchFPR), MOperand::immOp(offset)}});
                }
            } else {
                if (gprIdx < ctx.ti.intArgOrder.size()) {
                    bbMir.instrs.push_back(MInstr{
                        MOpcode::StrRegFpImm,
                        {MOperand::regOp(ctx.ti.intArgOrder[gprIdx]), MOperand::immOp(offset)}});
                    ++gprIdx;
                } else {
                    // GPR stack arg: load from caller stack, store to alloca
                    const int callerOff = 16 + static_cast<int>(stackArgIdx) * 8;
                    ++stackArgIdx;
                    bbMir.instrs.push_back(
                        MInstr{MOpcode::LdrRegFpImm,
                               {MOperand::regOp(kScratchGPR), MOperand::immOp(callerOff)}});
                    bbMir.instrs.push_back(
                        MInstr{MOpcode::StrRegFpImm,
                               {MOperand::regOp(kScratchGPR), MOperand::immOp(offset)}});
                }
            }
        }

        LoweredCall seq{};
        std::unordered_map<unsigned, uint16_t> tempVReg;
        std::unordered_map<unsigned, RegClass> tempRegClass;
        uint16_t nextVRegId = kFirstVirtualRegId;
        if (lowerCallWithArgs(binI,
                              bb,
                              ctx.ti,
                              ctx.fb,
                              bbMir,
                              seq,
                              tempVReg,
                              tempRegClass,
                              nextVRegId,
                              ctx.knownVarArgNamedArgCounts)) {
            for (auto &mi : seq.prefix)
                bbMir.instrs.push_back(std::move(mi));
            bbMir.instrs.push_back(std::move(seq.call));
            for (auto &mi : seq.postfix)
                bbMir.instrs.push_back(std::move(mi));
            bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
            ctx.fb.finalize();
            return ctx.mf;
        }

        // lowerCallWithArgs failed AND we've already added param stores to bbMir.
        // Clear the stale instructions before falling through to nullopt.
        bbMir.instrs.clear();
        return std::nullopt;
    }

    // Single-block, marshal only entry params and const i64 to integer arg regs
    const std::size_t nargs = binI.operands.size();
    if (nargs > ctx.ti.intArgOrder.size())
        return std::nullopt;

    // Build move plan for reg->reg moves; immediates applied after
    std::vector<Move> moves;
    std::vector<std::pair<PhysReg, long long>> immLoads;
    std::vector<std::pair<std::size_t, PhysReg>> tempRegs;
    std::size_t scratchUsed = 0;
    bool supported = true;

    // Register args: plan moves/imm loads/temps for 0..nargs-1
    const std::size_t nReg = ctx.argOrder.size();
    const std::size_t nRegArgs = (nargs < nReg) ? nargs : nReg;
    for (std::size_t i = 0; i < nRegArgs; ++i) {
        const PhysReg dst = ctx.argOrder[i];
        const auto &arg = binI.operands[i];
        if (arg.kind == il::core::Value::Kind::ConstInt) {
            immLoads.emplace_back(dst, arg.i64);
        } else {
            unsigned pIdx = 0;
            if (isParamTemp(bb, ctx.argOrder, arg, pIdx) && pIdx < ctx.argOrder.size()) {
                const PhysReg src = ctx.argOrder[pIdx];
                if (src != dst)
                    moves.push_back(Move{dst, src});
            } else {
                // Attempt to compute temp into a scratch then marshal it
                if (arg.kind == il::core::Value::Kind::Temp && scratchUsed < kScratchPoolSize) {
                    auto it = std::find_if(
                        bb.instructions.begin(),
                        bb.instructions.end(),
                        [&](const il::core::Instr &I) { return I.result && *I.result == arg.id; });
                    if (it != bb.instructions.end()) {
                        const PhysReg dstScratch = scratchPool[scratchUsed];
                        if (computeTempTo(*it, dstScratch, bb, ctx.argOrder, bbMir)) {
                            tempRegs.emplace_back(i, dstScratch);
                            ++scratchUsed;
                            continue;
                        }
                    }
                }
                supported = false;
                break;
            }
        }
    }

    if (!supported)
        return std::nullopt;

    // Include temp-reg moves into overall move list
    for (auto &tr : tempRegs) {
        const PhysReg dstArg = ctx.argOrder[tr.first];
        if (dstArg != tr.second)
            moves.push_back(Move{dstArg, tr.second});
    }

    // Resolve reg moves with scratch X9 to break cycles
    /**
     * @brief Tests whether a register is still a pending move destination.
     * @param r Physical register to query.
     * @return `true` when a pending move will overwrite @p r.
     */
    auto hasDst = [&](PhysReg r) {
        for (auto &m : moves)
            if (m.dst == r)
                return true;
        return false;
    };

    while (!moves.empty()) {
        bool progressed = false;
        for (auto it = moves.begin(); it != moves.end();) {
            if (!hasDst(it->src)) {
                bbMir.instrs.push_back(
                    MInstr{MOpcode::MovRR, {MOperand::regOp(it->dst), MOperand::regOp(it->src)}});
                it = moves.erase(it);
                progressed = true;
            } else {
                ++it;
            }
        }
        if (!progressed) {
            // Break cycle using scratch register
            const PhysReg cycleSrc = moves.front().src;
            bbMir.instrs.push_back(
                MInstr{MOpcode::MovRR, {MOperand::regOp(kScratchGPR), MOperand::regOp(cycleSrc)}});
            for (auto &m : moves)
                if (m.src == cycleSrc)
                    m.src = kScratchGPR;
        }
    }

    // Apply immediates
    for (auto &pr : immLoads)
        bbMir.instrs.push_back(
            MInstr{MOpcode::MovRI, {MOperand::regOp(pr.first), MOperand::immOp(pr.second)}});

    // Single-block fast path only handles register-passed arguments. Any call
    // that needs stack marshalling is routed through the generalized lowering
    // path above, which emits explicit SubSpImm/AddSpImm around the call.
    bbMir.instrs.push_back(MInstr{MOpcode::Bl, {MOperand::labelOp(mappedCallee)}});
    bbMir.instrs.push_back(MInstr{MOpcode::Ret, {}});
    ctx.fb.finalize();
    return ctx.mf;
}

} // namespace zanna::codegen::aarch64::fastpaths
