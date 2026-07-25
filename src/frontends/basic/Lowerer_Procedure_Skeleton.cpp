//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer_Procedure_Skeleton.cpp
// Purpose: Block scheduling, skeleton construction, and slot allocation.
//
// Phase: Block Scheduling (runs after metadata collection, before emission)
//
// Key Invariants:
// - Each unique source line gets a dedicated basic block
// - Synthetic line numbers are assigned to unlabeled statements
// - Entry block contains parameter materialization
// - Exit block is reserved for cleanup and return
// - Local slots are allocated in deterministic order (booleans, then others)
//
// Ownership/Lifetime: Operates on borrowed Lowerer instance.
//
//===----------------------------------------------------------------------===//

/**
 * @file Lowerer_Procedure_Skeleton.cpp
 * @brief Implements virtual lines, block skeletons, local slots, and GOSUB storage.
 *
 * Block pointers are borrowed from the active function and may be invalidated
 * when blocks are appended; stable indexes are recorded in ProcedureContext.
 * Symbol iteration is sorted before allocation for cross-platform determinism.
 */

#include "frontends/basic/LineUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/LoweringPipeline.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"

#include "zanna/il/IRBuilder.hpp"

#include <algorithm>
#include <cassert>
#include <vector>

#ifdef DEBUG
#include <unordered_set>
#endif

using namespace il::core;

namespace il::frontends::basic {

using pipeline_detail::coreTypeForAstType;

// =============================================================================
// Virtual Line Assignment
// =============================================================================

/// @brief Compute or retrieve the synthetic line number for a statement.
/// @details BASIC statements may lack explicit line labels; this helper assigns
///          monotonically increasing synthetic numbers to keep block naming
///          deterministic.  When a user-provided line exists it is reused to
///          ensure diagnostics map back to the original source.
/// @param s Statement whose virtual line number is requested.
/// @return User-specified line or a generated synthetic value.
int Lowerer::virtualLine(const Stmt &s) {
    auto it = stmtVirtualLines_.find(&s);
    if (it != stmtVirtualLines_.end())
        return it->second;

    const int userLine = s.line;
    if (hasUserLine(userLine)) {
        stmtVirtualLines_[&s] = userLine;
        return userLine;
    }

    int synthLine = synthLineBase_ + (synthSeq_++);
    stmtVirtualLines_[&s] = synthLine;
    return synthLine;
}

// =============================================================================
// Procedure Skeleton Construction
// =============================================================================

/// @brief Construct the control-flow skeleton for a procedure function.
/// @details Establishes the entry block, assigns deterministic labels to each
///          source line, allocates per-line basic blocks, and records the exit
///          block index for later use.  Debug builds assert that synthetic line
///          numbers remain unique to prevent accidental block collisions.
/// @param f Function being populated.
/// @param name Procedure name used for block mangling.
/// @param metadata Metadata previously gathered for the procedure body.
void Lowerer::buildProcedureSkeleton(Function &f,
                                     const std::string &name,
                                     const ProcedureMetadata &metadata) {
    ProcedureContext &ctx = context();
    ctx.blockNames().setNamer(std::make_unique<BlockNamer>(name));
    BlockNamer *blockNamer = ctx.blockNames().namer();

    auto &entry =
        builder->addBlock(f, blockNamer ? blockNamer->entry() : mangler.block("entry_" + name));
    entry.params = f.params;

#ifdef DEBUG
    std::vector<int> keys;
    keys.reserve(metadata.bodyStmts.size());
#endif

    auto &lineBlocks = ctx.blockNames().lineBlocks();
    for (const auto *stmt : metadata.bodyStmts) {
        int vLine = virtualLine(*stmt);
        if (lineBlocks.find(vLine) != lineBlocks.end())
            continue;
        size_t blockIdx = f.blocks.size();
        if (blockNamer)
            builder->addBlock(f, blockNamer->line(vLine));
        else
            builder->addBlock(f, mangler.block("L" + std::to_string(vLine) + "_" + name));
        lineBlocks[vLine] = blockIdx;
#ifdef DEBUG
        keys.push_back(vLine);
#endif
    }

#ifdef DEBUG
    {
        std::unordered_set<int> seen;
        for (int k : keys) {
            assert(seen.insert(k).second &&
                   "Duplicate block key; unlabeled statements must have unique synthetic keys");
        }
    }
#endif

    ctx.setExitIndex(f.blocks.size());
    if (blockNamer)
        builder->addBlock(f, blockNamer->ret());
    else
        builder->addBlock(f, mangler.block("ret_" + name));
}

// =============================================================================
// Local Slot Allocation
// =============================================================================

/// @brief Allocate stack slots for all referenced locals (and optionally params).
/// @details Iterates over the symbol table, allocating IL stack storage for each
///          referenced symbol lacking a slot.  Array values receive pointer
///          slots initialised to null, booleans are zeroed, and strings are
///          seeded with the runtime empty string.  When bounds checking is
///          enabled, auxiliary slots are reserved for array lengths.
///
/// Allocation Order:
/// 1. Pass 1: Boolean scalars (1-byte slots)
/// 2. Pass 2: Arrays (pointer slots initialized to null) and other scalars
/// 3. Pass 3: Array length slots for bounds checking (if enabled)
///
/// @param paramNames Names of parameters for the current procedure.
/// @param includeParams When true, allocate slots for parameters as well as locals.
void Lowerer::allocateLocalSlots(const std::unordered_set<std::string> &paramNames,
                                 bool includeParams) {
    // Pass 1: booleans
    allocateBooleanSlots(paramNames, includeParams);

    // Pass 2: arrays and other scalars
    allocateNonBooleanSlots(paramNames, includeParams);

    // Pass 3: array length slots for bounds checking
    if (boundsChecks)
        allocateArrayLengthSlots(paramNames, includeParams);
}

/// @brief Allocate stack slots for boolean scalars.
/// @param paramNames Parameter names to optionally skip.
/// @param includeParams Whether to include parameters in allocation.
void Lowerer::allocateBooleanSlots(const std::unordered_set<std::string> &paramNames,
                                   bool includeParams) {
    // Sort symbol names for deterministic allocation order across platforms.
    std::vector<std::string> sortedNames;
    sortedNames.reserve(symbols.size());
    for (const auto &[name, info] : symbols)
        sortedNames.push_back(name);
    std::sort(sortedNames.begin(), sortedNames.end());

    for (const auto &name : sortedNames) {
        auto &info = symbols.at(name);
        if (!shouldAllocateSlot(name, info, paramNames, includeParams))
            continue;
        if (info.slotId)
            continue;

        curLoc = {};
        SlotType slotInfo = getSlotType(name);
        if (slotInfo.isArray || !slotInfo.isBoolean)
            continue;

        Value slot = emitAlloca(1);
        info.slotId = slot.id;
        builder->setValueName(slot.id, name); // surface as a named local to the debugger
        emitStore(ilBoolTy(), slot, emitBoolConst(false));
    }
}

/// @brief Allocate stack slots for arrays and non-boolean scalars.
/// @param paramNames Parameter names to optionally skip.
/// @param includeParams Whether to include parameters in allocation.
void Lowerer::allocateNonBooleanSlots(const std::unordered_set<std::string> &paramNames,
                                      bool includeParams) {
    // Sort symbol names for deterministic allocation order across platforms.
    std::vector<std::string> sortedNames;
    sortedNames.reserve(symbols.size());
    for (const auto &[name, info] : symbols)
        sortedNames.push_back(name);
    std::sort(sortedNames.begin(), sortedNames.end());

    for (const auto &name : sortedNames) {
        auto &info = symbols.at(name);
        if (!shouldAllocateSlot(name, info, paramNames, includeParams))
            continue;
        if (info.slotId)
            continue;

        curLoc = {};
        SlotType slotInfo = getSlotType(name);
        if (slotInfo.isArray) {
            Value slot = emitAlloca(8);
            info.slotId = slot.id;
            builder->setValueName(slot.id, name); // surface as a named local to the debugger
            emitStore(Type(Type::Kind::Ptr), slot, Value::null());
        } else if (!slotInfo.isBoolean) {
            Value slot = emitAlloca(8);
            info.slotId = slot.id;
            builder->setValueName(slot.id, name); // surface as a named local to the debugger
            if (slotInfo.type.kind == Type::Kind::Str) {
                Value empty = emitCallRet(slotInfo.type, "rt_str_empty", {});
                emitStore(slotInfo.type, slot, empty);
            } else if (slotInfo.isObject) {
                // Initialize object slots to null
                emitStore(Type(Type::Kind::Ptr), slot, Value::null());
            }
        }
    }
}

/// @brief Allocate auxiliary slots for array length tracking (bounds checking).
/// @param paramNames Parameter names to optionally skip.
/// @param includeParams Whether to include parameters in allocation.
void Lowerer::allocateArrayLengthSlots(const std::unordered_set<std::string> &paramNames,
                                       bool includeParams) {
    // Sort symbol names for deterministic allocation order across platforms.
    std::vector<std::string> sortedNames;
    sortedNames.reserve(symbols.size());
    for (const auto &[name, info] : symbols)
        sortedNames.push_back(name);
    std::sort(sortedNames.begin(), sortedNames.end());

    for (const auto &name : sortedNames) {
        auto &info = symbols.at(name);
        if (!info.referenced || !info.isArray)
            continue;
        bool isParam = paramNames.find(name) != paramNames.end();
        if (isParam && !includeParams)
            continue;
        if (info.arrayLengthSlot)
            continue;

        curLoc = {};
        Value slot = emitAlloca(8);
        info.arrayLengthSlot = slot.id;
    }
}

/// @brief Check if a symbol should have a slot allocated.
/// @details Filters out unreferenced symbols, static variables (which use
///          runtime storage), parameters when not included, and module-level
///          symbols while lowering a non-main procedure. Parameters are tested
///          before module-scope checks so a shadowing formal remains eligible.
/// @param name Symbol name.
/// @param info Symbol metadata.
/// @param paramNames Set of parameter names.
/// @param includeParams Whether to allocate for parameters.
/// @return True if a slot should be allocated for this symbol.
bool Lowerer::shouldAllocateSlot(const std::string &name,
                                 const SymbolInfo &info,
                                 const std::unordered_set<std::string> &paramNames,
                                 bool includeParams) const {
    if (!info.referenced)
        return false;
    if (info.isStatic)
        return false; // Static variables use module-level runtime storage

    bool isParam = paramNames.find(name) != paramNames.end();
    if (isParam && !includeParams)
        return false;

    // Skip module-level globals and constants (they resolve via runtime storage)
    // Constants use module-level storage and can't be shadowed (semantic analyzer prevents it)
    bool isMain = (context().function() && context().function()->name == "main");
    if (!isParam && !isMain && isCrossProcGlobal(name))
        return false;
    if (!isParam && !isMain && semanticAnalyzer_ && semanticAnalyzer_->isModuleLevelSymbol(name))
        return false;

    return true;
}

// =============================================================================
// GOSUB Stack Management
// =============================================================================

/// @brief Lazily materialise the stack used for GOSUB/RETURN bookkeeping.
/// @details Emits prologue allocations for the return-stack pointer and storage
///          array if they have not yet been created.  The helper temporarily
///          switches the builder's insertion point to the function entry block
///          and restores both location and block afterwards.
void Lowerer::ensureGosubStack() {
    ProcedureContext &ctx = context();
    auto &state = ctx.gosub();
    if (state.hasPrologue())
        return;

    Function *func = ctx.function();
    if (!func)
        return;

    BasicBlock *savedBlock = ctx.current();
    BasicBlock *entry = &func->blocks.front();

    auto savedLoc = curLoc;
    curLoc = {};

    // If the entry block is already terminated (GOSUB is first encountered
    // inside a compound statement such as a DO/WHILE or IF body, so the entry
    // branch to the first line block was already emitted), temporarily park the
    // terminator so that the alloca/store prologue can be appended before it.
    Instr savedTerm;
    const bool wasTerminated = entry->terminated;
    if (wasTerminated && !entry->instructions.empty()) {
        savedTerm = std::move(entry->instructions.back());
        entry->instructions.pop_back();
        entry->terminated = false;
    }

    ctx.setCurrent(entry);
    Value spSlot = emitAlloca(8);
    Value stackSlot = emitAlloca(kGosubStackDepth * 4);
    emitStore(Type(Type::Kind::I64), spSlot, Value::constInt(0));
    state.setPrologue(spSlot, stackSlot);

    if (wasTerminated) {
        entry->instructions.push_back(std::move(savedTerm));
        entry->terminated = true;
    }

    curLoc = savedLoc;
    ctx.setCurrent(savedBlock);
}

} // namespace il::frontends::basic
