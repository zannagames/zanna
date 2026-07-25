//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SelectCaseLowering.cpp
// Purpose: Implements the SelectCaseLowering helper for BASIC frontend lowering.
// Key invariants: Respects Lowerer block allocation and terminator rules.
// Ownership/Lifetime: Borrows Lowerer state; does not allocate persistent memory.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SelectCaseLowering.cpp
/// @brief Implements lowering of BASIC SELECT CASE statements into IL control flow.
/// @details The helper orchestrates block creation, comparison emission, and jump
///          table construction so SELECT CASE lowering can share logic across
///          numeric and string selector modes while preserving deterministic
///          control-flow graphs. Block indices are deliberately carried across
///          append and nested-lowering operations because those operations may
///          invalidate pointers into the function's block vector.

#include "frontends/basic/SelectCaseLowering.hpp"

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"

#include "zanna/il/Module.hpp"

#include <cassert>
#include <string>

namespace il::frontends::basic {

/// @brief Binds this helper to the lowerer whose state it will mutate.
/// @param lowerer Lowerer providing the active function, builder, expression
///                lowering, diagnostics, and block naming. It must outlive this
///                helper.
/// @post No block or instruction is emitted.
SelectCaseLowering::SelectCaseLowering(Lowerer &lowerer) noexcept : lowerer_(lowerer) {}

/// @brief Lowers a BASIC `SELECT CASE` into dispatch and body blocks.
/// @details Returns without side effects when the AST has no selector or the
///          context lacks a current function/block. A terminated insertion
///          block is replaced with a fresh `select_start` block. The selector
///          is then evaluated once: strings retain their value, while other
///          types are converted to signed 64-bit form and narrowed to 32 bits
///          for discrete switching. String labels use an equality chain;
///          numeric relations/ranges use comparisons followed by a switch over
///          discrete labels. Every explicit body and any modeled CASE ELSE body
///          is lowered, and the shared exit becomes current.
/// @param stmt Parsed statement borrowed for the duration of lowering. Its
///             prebuilt @ref SelectModel must use valid arm indices.
/// @post On successful lowering, the lowerer's insertion point is the common
///       SELECT exit block.
void SelectCaseLowering::lower(const SelectCaseStmt &stmt) {
    if (!stmt.selector)
        return;

    auto &ctx = lowerer_.context();
    auto *func = ctx.function();
    auto *current = ctx.current();
    if (!func || !current)
        return;

    // Defensive: if the current block is already terminated, start a fresh block
    // so the SELECT CASE lowering has a valid insertion point.
    if (current->terminated) {
        auto *namer = ctx.blockNames().namer();
        std::string label =
            namer ? namer->generic("select_start") : lowerer_.mangler.block("select_start");
        lowerer_.builder->addBlock(*func, label);
        func = ctx.function();
        current = &func->blocks.back();
        ctx.setCurrent(current);
    }

    lowerer_.curLoc = stmt.selector->loc;
    Lowerer::RVal selectorVal = lowerer_.lowerExpr(*stmt.selector);
    bool selectorIsString = selectorVal.type.kind == il::core::Type::Kind::Str;
    il::core::Value stringSelector = selectorVal.value;
    il::core::Value selWide{};
    il::core::Value sel{};
    if (!selectorIsString) {
        selectorVal = lowerer_.ensureI64(std::move(selectorVal), stmt.selector->loc);
        selWide = selectorVal.value;
        sel = lowerer_.emitCommon(stmt.selector->loc).narrow_to(selectorVal.value, 64, 32);
    }

    func = ctx.function();
    current = ctx.current();
    if (!func || !current)
        return;

    const SelectModel &model = stmt.model;
    Blocks blocks = prepareBlocks(stmt, model.hasCaseElse, model.hasNumericRanges);

    if (selectorIsString) {
        lowerStringArms(stmt, model, blocks, stringSelector);
    } else {
        lowerNumericDispatch(stmt, model, blocks, selWide, sel);
    }

    // BUG-087 fix: Pass end block INDEX instead of pointer, since nested statements
    // (like IF) can cause func->blocks vector to reallocate, invalidating pointers.
    for (size_t i = 0; i < stmt.arms.size(); ++i) {
        func = ctx.function();
        auto *armBlk = &func->blocks[blocks.armIdx[i]];
        emitArmBody(stmt.arms[i].body, armBlk, stmt.arms[i].range.begin, blocks.endIdx);
    }

    if (model.hasCaseElse) {
        func = ctx.function();
        auto *caseElseBlk = &func->blocks[*blocks.elseIdx];
        emitArmBody(stmt.elseBody, caseElseBlk, stmt.range.end, blocks.endIdx);
    }

    // BUG-087 fix: Refresh endBlk pointer after emitting arm bodies
    func = ctx.function();
    auto *endBlk = &func->blocks[blocks.endIdx];
    ctx.setCurrent(endBlk);
}

/// @brief Appends and indexes the block skeleton for one SELECT statement.
/// @details Records the current block, appends one arm block per AST arm, then
///          optionally appends CASE ELSE and dedicated switch blocks before the
///          mandatory shared exit. Labels come from the context's block namer
///          when installed and otherwise from the lowerer's mangler. Because
///          appending can reallocate storage, the original current block is
///          reacquired by index before return.
/// @param stmt Statement whose explicit arm count controls allocation.
/// @param hasCaseElse Whether a CASE ELSE entry block is required.
/// @param needsDispatch Whether the integer switch needs a dedicated block
///                      distinct from the SELECT entry.
/// @return Indices of the original entry and every appended SELECT block.
/// @pre The lowering context has both a current function and current block.
/// @post The original entry block is restored as the current insertion point.
SelectCaseLowering::Blocks SelectCaseLowering::prepareBlocks(const SelectCaseStmt &stmt,
                                                             bool hasCaseElse,
                                                             bool needsDispatch) {
    auto &ctx = lowerer_.context();
    auto *func = ctx.function();
    auto *current = ctx.current();
    assert(func && current);

    size_t curIdx = ctx.blockIndex(current);
    auto *blockNamer = ctx.blockNames().namer();

    // BUG-072 fix: Use addBlock to append SELECT blocks at the end of the function.
    // The exit block will be moved to the very end after all function lowering completes.
    // This preserves block index stability during lowering.
    size_t startIdx = func->blocks.size();

    Blocks blocks{};
    blocks.currentIdx = curIdx;
    blocks.switchIdx = curIdx;
    blocks.armIdx.resize(stmt.arms.size());

    // Append all SELECT blocks at the end using addBlock().
    for (size_t i = 0; i < stmt.arms.size(); ++i) {
        std::string label = blockNamer ? blockNamer->generic("select_arm")
                                       : lowerer_.mangler.block("select_arm_" + std::to_string(i));
        lowerer_.builder->addBlock(*func, label);
    }

    if (hasCaseElse) {
        std::string defaultLabel = blockNamer ? blockNamer->generic("select_default")
                                              : lowerer_.mangler.block("select_default");
        lowerer_.builder->addBlock(*func, defaultLabel);
        blocks.elseIdx = startIdx + stmt.arms.size();
    }

    if (needsDispatch) {
        std::string dispatchLabel = blockNamer ? blockNamer->generic("select_dispatch")
                                               : lowerer_.mangler.block("select_dispatch");
        lowerer_.builder->addBlock(*func, dispatchLabel);
        blocks.switchIdx = startIdx + stmt.arms.size() + (hasCaseElse ? 1 : 0);
    }

    std::string endLabel =
        blockNamer ? blockNamer->generic("select_end") : lowerer_.mangler.block("select_end");
    lowerer_.builder->addBlock(*func, endLabel);
    blocks.endIdx = startIdx + stmt.arms.size() + (hasCaseElse ? 1 : 0) + (needsDispatch ? 1 : 0);

    // Refresh pointers after additions (vector may have reallocated).
    func = ctx.function();
    current = &func->blocks[curIdx];
    ctx.setCurrent(current);

    for (size_t i = 0; i < stmt.arms.size(); ++i)
        blocks.armIdx[i] = startIdx + i;

    return blocks;
}

/// @brief Emits ordered string-label dispatch for SELECT CASE arms.
/// @details Builds one plan entry per modeled string label and appends a
///          fallback to CASE ELSE or the common exit. With no labels, the entry
///          block branches directly to that fallback. Otherwise each test
///          materializes the label as a runtime string and calls `rt_str_eq`;
///          the resulting boolean controls the comparison chain.
/// @param stmt Statement supplying fallback and instruction locations.
/// @param model Flattened string labels with destination arm indices.
/// @param blocks Stable indices created by @ref prepareBlocks.
/// @param stringSelector Single evaluated selector reused by all comparisons.
/// @pre Every string-label arm index addresses @p blocks.armIdx.
void SelectCaseLowering::lowerStringArms(const SelectCaseStmt &stmt,
                                         const SelectModel &model,
                                         const Blocks &blocks,
                                         il::core::Value stringSelector) {
    auto &ctx = lowerer_.context();
    auto *func = ctx.function();

    // BUG-017 fix: Use index instead of pointer to avoid invalidation
    size_t defaultIdx = blocks.elseIdx ? *blocks.elseIdx : blocks.endIdx;

    std::vector<CasePlanEntry> plan;
    plan.reserve(model.stringLabels.size() + 1);

    for (const auto &label : model.stringLabels) {
        // BUG-017 fix: Store index instead of pointer to avoid invalidation
        CasePlanEntry entry{};
        entry.kind = CasePlanEntry::Kind::StringLabel;
        entry.armIndex = label.armIndex;
        entry.targetIdx = blocks.armIdx[label.armIndex];
        entry.loc = label.loc;
        entry.strLiteral = label.value;
        plan.push_back(entry);
    }

    CasePlanEntry defaultEntry{};
    defaultEntry.kind = CasePlanEntry::Kind::Default;
    defaultEntry.targetIdx = defaultIdx;
    defaultEntry.loc = stmt.range.end;
    plan.push_back(defaultEntry);

    if (plan.size() == 1) {
        func = ctx.function();
        ctx.setCurrent(&func->blocks[blocks.currentIdx]);
        lowerer_.curLoc = stmt.loc;
        // Blocks that skip comparisons fall through directly to the default arm.
        func = ctx.function();
        auto *defaultBlk = &func->blocks[defaultIdx];
        lowerer_.emitBr(defaultBlk);
        ctx.setCurrent(defaultBlk);
        return;
    }

    /// Emits one runtime equality test between the saved selector and a
    /// materialized CASE string literal.
    ConditionEmitter emitter = [this, stringSelector](const CasePlanEntry &entry) {
        assert(entry.kind == CasePlanEntry::Kind::StringLabel);
        std::string labelStr(entry.strLiteral);
        il::core::Value labelValue = lowerer_.emitConstStr(lowerer_.getStringLabel(labelStr));
        // rt_str_eq returns i1 (boolean)
        il::core::Type i1Ty(il::core::Type::Kind::I1);
        il::core::Value result =
            lowerer_.emitCallRet(i1Ty, "rt_str_eq", {stringSelector, labelValue});
        return result;
    };

    emitCompareChain(blocks.currentIdx, plan, emitter);
}

/// @brief Emits numeric predicates and discrete-label switching.
/// @details Relations are translated to signed comparisons and ranges to an
///          inclusive pair of signed comparisons combined as a boolean AND.
///          All such predicates precede the fallback switch. When modeled ranges
///          exist, that fallback is the preallocated dispatch block; relations
///          without ranges receive a lazily allocated fallback; with no
///          predicates the SELECT entry itself contains the switch. An
///          unexpected plan kind emits diagnostic `B9005` and a false condition
///          so lowering can continue.
/// @param stmt Statement supplying selector and diagnostic source positions.
/// @param model Flattened relations, ranges, and discrete labels.
/// @param blocks Stable block indices created by @ref prepareBlocks.
/// @param selWide Signed 64-bit selector used for relation/range predicates.
/// @param selector Narrowed 32-bit selector used by `SwitchI32`.
void SelectCaseLowering::lowerNumericDispatch(const SelectCaseStmt &stmt,
                                              const SelectModel &model,
                                              const Blocks &blocks,
                                              il::core::Value selWide,
                                              il::core::Value selector) {
    auto &ctx = lowerer_.context();
    auto *func = ctx.function();
    if (!func)
        return;

    std::vector<CasePlanEntry> plan;
    plan.reserve(model.numericRelations.size() + model.numericRanges.size() + 1);

    for (const auto &rel : model.numericRelations) {
        // BUG-017 fix: Store index instead of pointer to avoid invalidation
        CasePlanEntry entry{};
        entry.armIndex = rel.armIndex;
        entry.targetIdx = blocks.armIdx[rel.armIndex];
        entry.loc = rel.loc;
        switch (rel.op) {
            case SelectModel::NumericRelation::Op::LT:
                entry.kind = CasePlanEntry::Kind::RelLT;
                entry.valueRange.second = rel.rhs;
                break;
            case SelectModel::NumericRelation::Op::LE:
                entry.kind = CasePlanEntry::Kind::RelLE;
                entry.valueRange.second = rel.rhs;
                break;
            case SelectModel::NumericRelation::Op::EQ:
                entry.kind = CasePlanEntry::Kind::RelEQ;
                entry.valueRange.first = rel.rhs;
                entry.valueRange.second = rel.rhs;
                break;
            case SelectModel::NumericRelation::Op::GE:
                entry.kind = CasePlanEntry::Kind::RelGE;
                entry.valueRange.first = rel.rhs;
                break;
            case SelectModel::NumericRelation::Op::GT:
                entry.kind = CasePlanEntry::Kind::RelGT;
                entry.valueRange.first = rel.rhs;
                break;
        }
        plan.push_back(entry);
    }

    for (const auto &range : model.numericRanges) {
        // BUG-017 fix: Store index instead of pointer to avoid invalidation
        CasePlanEntry entry{};
        entry.kind = CasePlanEntry::Kind::Range;
        entry.armIndex = range.armIndex;
        entry.targetIdx = blocks.armIdx[range.armIndex];
        entry.loc = range.loc;
        entry.valueRange.first = range.lo;
        entry.valueRange.second = range.hi;
        plan.push_back(entry);
    }

    const bool hasComparisons = !plan.empty();

    CasePlanEntry defaultEntry{};
    defaultEntry.kind = CasePlanEntry::Kind::Default;
    if (model.hasNumericRanges) {
        defaultEntry.targetIdx = blocks.switchIdx;
    } else if (hasComparisons) {
        defaultEntry.targetIdx = SIZE_MAX; // Will be allocated later
    } else {
        defaultEntry.targetIdx = blocks.switchIdx;
    }
    defaultEntry.loc = stmt.loc;
    plan.push_back(defaultEntry);

    /// Emits the signed comparison represented by one numeric plan entry.
    /// Inclusive ranges combine two one-bit comparisons through the IL's
    /// 64-bit logical operation and truncate the result back to one bit.
    ConditionEmitter emitter = [this, selWide, &stmt](const CasePlanEntry &entry) {
        assert(entry.kind != CasePlanEntry::Kind::Default);
        switch (entry.kind) {
            case CasePlanEntry::Kind::RelLT:
                return lowerer_.emitBinary(
                    il::core::Opcode::SCmpLT,
                    lowerer_.ilBoolTy(),
                    selWide,
                    il::core::Value::constInt(static_cast<long long>(entry.valueRange.second)));
            case CasePlanEntry::Kind::RelLE:
                return lowerer_.emitBinary(
                    il::core::Opcode::SCmpLE,
                    lowerer_.ilBoolTy(),
                    selWide,
                    il::core::Value::constInt(static_cast<long long>(entry.valueRange.second)));
            case CasePlanEntry::Kind::RelEQ:
                return lowerer_.emitBinary(
                    il::core::Opcode::ICmpEq,
                    lowerer_.ilBoolTy(),
                    selWide,
                    il::core::Value::constInt(static_cast<long long>(entry.valueRange.first)));
            case CasePlanEntry::Kind::RelGE:
                return lowerer_.emitBinary(
                    il::core::Opcode::SCmpGE,
                    lowerer_.ilBoolTy(),
                    selWide,
                    il::core::Value::constInt(static_cast<long long>(entry.valueRange.first)));
            case CasePlanEntry::Kind::RelGT:
                return lowerer_.emitBinary(
                    il::core::Opcode::SCmpGT,
                    lowerer_.ilBoolTy(),
                    selWide,
                    il::core::Value::constInt(static_cast<long long>(entry.valueRange.first)));
            case CasePlanEntry::Kind::Range: {
                il::core::Value ge = lowerer_.emitBinary(
                    il::core::Opcode::SCmpGE,
                    lowerer_.ilBoolTy(),
                    selWide,
                    il::core::Value::constInt(static_cast<long long>(entry.valueRange.first)));
                il::core::Value le = lowerer_.emitBinary(
                    il::core::Opcode::SCmpLE,
                    lowerer_.ilBoolTy(),
                    selWide,
                    il::core::Value::constInt(static_cast<long long>(entry.valueRange.second)));
                // The And opcode requires i64 operands; extend the booleans and truncate back.
                il::core::Value ge64 = lowerer_.emitZext1ToI64(ge);
                il::core::Value le64 = lowerer_.emitZext1ToI64(le);
                il::core::Value both64 =
                    lowerer_.emitCommon(stmt.selector->loc).logical_and(ge64, le64);
                return lowerer_.emitUnary(il::core::Opcode::Trunc1, lowerer_.ilBoolTy(), both64);
            }
            case CasePlanEntry::Kind::StringLabel:
            case CasePlanEntry::Kind::Default:
                break;
        }
        if (auto *diag = lowerer_.diagnosticEmitter()) {
            diag->emit(il::support::Severity::Error,
                       "B9005",
                       stmt.loc,
                       1,
                       "unsupported SELECT CASE plan entry reached lowering");
        }
        return lowerer_.emitBoolConst(false);
    };

    size_t switchIdx = emitCompareChain(blocks.currentIdx, plan, emitter);
    emitSwitchJumpTable(stmt, model, blocks, selector, switchIdx);
}

/// @brief Materializes an ordered comparison plan as conditional branches.
/// @details The last entry is the default. If its target is unresolved
///          (`SIZE_MAX`), a fallback block is appended and recorded in the plan.
///          For each preceding entry, a false-path block is appended unless the
///          next entry is already the default; the supplied callback emits the
///          condition in the current check block, which is then terminated by a
///          branch to the entry target or false path. All block pointers are
///          reacquired after append operations.
/// @param startIdx Index at which the first condition is emitted.
/// @param plan Mutable plan ending in a default entry. Its default target may
///             be resolved by this function.
/// @param emitCond Callable that emits one boolean for each non-default entry.
/// @return Default/fall-through block index, also left current in the context.
/// @pre @p plan is empty or its last entry has kind
///      @ref CasePlanEntry::Kind::Default.
size_t SelectCaseLowering::emitCompareChain(size_t startIdx,
                                            std::vector<CasePlanEntry> &plan,
                                            const ConditionEmitter &emitCond) {
    if (plan.empty())
        return startIdx;

    auto &ctx = lowerer_.context();
    auto *func = ctx.function();
    auto *blockNamer = ctx.blockNames().namer();

    auto &defaultEntry = plan.back();
    assert(defaultEntry.kind == CasePlanEntry::Kind::Default);

    // BUG-017 fix: Use index instead of pointer to avoid invalidation
    size_t defaultIdx = defaultEntry.targetIdx;
    if (defaultIdx == SIZE_MAX) {
        std::string label = blockNamer
                                ? blockNamer->generic(std::string(blockTagFor(defaultEntry)))
                                : lowerer_.mangler.block(std::string(blockTagFor(defaultEntry)));
        lowerer_.builder->addBlock(*func, label);
        func = ctx.function();
        defaultIdx = func->blocks.size() - 1;
        defaultEntry.targetIdx = defaultIdx;
    }

    func = ctx.function();
    auto *defaultBlk = &func->blocks[defaultIdx];
    if (defaultBlk->label.empty())
        defaultBlk->label = lowerer_.nextFallbackBlockLabel();

    size_t currentIdx = startIdx;
    for (size_t i = 0; i + 1 < plan.size(); ++i) {
        auto &entry = plan[i];

        bool needIntermediate = plan[i + 1].kind != CasePlanEntry::Kind::Default;
        size_t nextIdx = defaultIdx;
        if (needIntermediate) {
            func = ctx.function();
            std::string label = blockNamer
                                    ? blockNamer->generic(std::string(blockTagFor(plan[i + 1])))
                                    : lowerer_.mangler.block(std::string(blockTagFor(plan[i + 1])));
            lowerer_.builder->addBlock(*func, label);
            func = ctx.function();
            nextIdx = func->blocks.size() - 1;
            auto *falseTarget = &func->blocks[nextIdx];
            if (falseTarget->label.empty())
                falseTarget->label = lowerer_.nextFallbackBlockLabel();
        }

        // BUG-017 fix: Get checkBlk AFTER all addBlock calls to avoid pointer invalidation
        func = ctx.function();
        auto *checkBlk = &func->blocks[currentIdx];
        ctx.setCurrent(checkBlk);
        lowerer_.curLoc = entry.loc;
        il::core::Value cond = emitCond(entry);

        // BUG-017 fix: Refresh pointers from indices after each addBlock
        func = ctx.function();
        auto *trueTarget = &func->blocks[entry.targetIdx];
        auto *falseTarget = &func->blocks[nextIdx];

        // Each comparison produces a terminating conditional branch; no fallthrough remains.
        lowerer_.emitCBr(cond, trueTarget, falseTarget);
        currentIdx = nextIdx;
    }

    func = ctx.function();
    ctx.setCurrent(&func->blocks[defaultIdx]);
    return defaultIdx;
}

/// @brief Maps a plan-entry kind to a generated block-name stem.
/// @param entry Entry whose @ref CasePlanEntry::Kind is inspected.
/// @return A static-lifetime view: `"select_check"` for strings,
///         `"select_rel"` for relations, `"select_range"` for ranges, and
///         `"select_dispatch"` for defaults or defensive fall-through.
std::string_view SelectCaseLowering::blockTagFor(const CasePlanEntry &entry) {
    switch (entry.kind) {
        case CasePlanEntry::Kind::StringLabel:
            return "select_check";
        case CasePlanEntry::Kind::RelLT:
        case CasePlanEntry::Kind::RelLE:
        case CasePlanEntry::Kind::RelEQ:
        case CasePlanEntry::Kind::RelGE:
        case CasePlanEntry::Kind::RelGT:
            return "select_rel";
        case CasePlanEntry::Kind::Range:
            return "select_range";
        case CasePlanEntry::Kind::Default:
            return "select_dispatch";
    }
    return "select_dispatch";
}

/// @brief Terminates a block with discrete numeric SELECT dispatch.
/// @details Converts every already-normalized numeric label into one switch
///          operand and one arm-block target, preserving model order. The first
///          branch target is the default: the modeled CASE ELSE block when
///          allocated, otherwise the common exit. Empty target labels are
///          filled using the lowerer's fallback-name generator. This routine
///          performs no additional range validation or duplicate elimination.
/// @param stmt Statement supplying the switch instruction's source location.
/// @param model Normalized discrete labels and owning arm indices.
/// @param blocks Stable destination indices created by @ref prepareBlocks.
/// @param selector Signed 32-bit selector operand.
/// @param switchIdx Index of the block that receives the switch terminator.
/// @post The indexed switch block is marked terminated and remains current.
void SelectCaseLowering::emitSwitchJumpTable(const SelectCaseStmt &stmt,
                                             const SelectModel &model,
                                             const Blocks &blocks,
                                             il::core::Value selector,
                                             size_t switchIdx) {
    auto &ctx = lowerer_.context();
    auto *func = ctx.function();
    ctx.setCurrent(&func->blocks[switchIdx]);

    std::vector<std::pair<int32_t, il::core::BasicBlock *>> caseTargets;
    caseTargets.reserve(model.numericLabels.size());

    for (const auto &label : model.numericLabels) {
        auto *armBlk = &func->blocks[blocks.armIdx[label.armIndex]];
        if (armBlk->label.empty())
            armBlk->label = lowerer_.nextFallbackBlockLabel();
        caseTargets.emplace_back(label.value, armBlk);
    }

    il::core::Instr sw;
    sw.op = il::core::Opcode::SwitchI32;
    sw.type = il::core::Type(il::core::Type::Kind::Void);
    sw.operands.push_back(selector);

    auto *caseElseBlk =
        blocks.elseIdx ? &func->blocks[*blocks.elseIdx] : &func->blocks[blocks.endIdx];
    if (caseElseBlk->label.empty())
        caseElseBlk->label = lowerer_.nextFallbackBlockLabel();
    sw.addBranchTarget(caseElseBlk->label);

    for (const auto &[value, target] : caseTargets) {
        if (target->label.empty())
            target->label = lowerer_.nextFallbackBlockLabel();
        sw.operands.push_back(il::core::Value::constInt(static_cast<long long>(value)));
        sw.addBranchTarget(target->label);
    }
    sw.loc = stmt.loc;

    auto *switchBlk = ctx.current();
    switchBlk->instructions.push_back(std::move(sw));
    // Switch terminators complete the block; successors are encoded in the table.
    switchBlk->terminated = true;
}

/// @brief Lowers one CASE or CASE ELSE body into its entry block.
/// @details Installs @p entry, skips null nodes, and lowers statements in order
///          until no current block remains or the current block is terminated.
///          If control can still fall through, the common exit is reacquired by
///          index after nested lowering and a branch is emitted at @p loc.
/// @param body Ordered, caller-owned AST statement pointers.
/// @param entry Initial insertion block; it must belong to the current function.
/// @param loc Location assigned to a synthesized fall-through branch.
/// @param endBlkIdx Stable index of the common SELECT exit.
/// @post The current block is either a terminated block produced by the body or
///       an arm tail terminated by a branch to @p endBlkIdx.
void SelectCaseLowering::emitArmBody(const std::vector<StmtPtr> &body,
                                     il::core::BasicBlock *entry,
                                     il::support::SourceLoc loc,
                                     size_t endBlkIdx) {
    auto &ctx = lowerer_.context();
    ctx.setCurrent(entry);
    for (const auto &node : body) {
        if (!node)
            continue;
        lowerer_.lowerStmt(*node);
        auto *bodyCur = ctx.current();
        if (!bodyCur || bodyCur->terminated)
            break;
    }

    auto *bodyCur = ctx.current();
    if (bodyCur && !bodyCur->terminated) {
        // BUG-087 fix: Refresh endBlk pointer after lowering statements, since
        // nested control flow (IF, FOR, etc.) can cause func->blocks to reallocate.
        auto *func = ctx.function();
        auto *endBlk = &func->blocks[endBlkIdx];
        lowerer_.curLoc = loc;
        lowerer_.emitBr(endBlk);
    }
}

/// @brief Adapts SELECT lowering's control-state result to statement lowering.
/// @details Delegates to @ref Lowerer::emitSelect and, when that operation
///          returns a non-null current block, installs it in the lowering
///          context. `emitSelect` is the layer that invokes
///          @ref SelectCaseLowering.
/// @param stmt Parsed SELECT CASE statement to lower.
/// @post A non-null returned control-state block becomes the active insertion
///       point.
void Lowerer::lowerSelectCase(const SelectCaseStmt &stmt) {
    CtrlState state = emitSelect(stmt);
    if (state.cur)
        context().setCurrent(state.cur);
}

} // namespace il::frontends::basic
