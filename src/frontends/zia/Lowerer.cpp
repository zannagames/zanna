//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/Lowerer.cpp
// Purpose: Initialize and drive whole-module lowering from semantically checked
//          Zia AST nodes to owned Zanna IL.
// Key invariants:
//   * Per-module transient state is cleared before any declarations are lowered.
//   * Enum constants, final constants, and type layouts are registered before
//     declaration bodies consume them.
//   * Deferred generic instantiations finish before runtime extern declarations
//     are materialized.
// Ownership: Lowerer borrows Sema and diagnostics, owns transient builder/module
//            state during lowering, and returns the completed Module by value.
// References: docs/languages/zia-reference.md, docs/il/il-guide.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Implementation of Zia to IL code generation.
///
/// @details This file implements the Lowerer class which transforms a
/// type-checked Zia AST into Zanna IL. Key implementation details:
///
/// ## Lowering Process
///
/// The lower() method:
/// 1. Initializes module and IR builder
/// 2. Lowers all declarations to IL functions/globals
/// 3. Emits string constants via stringTable_
/// 4. Declares external runtime functions
///
/// ## Control Flow
///
/// Control flow constructs are lowered to basic blocks:
/// - if: Emit condition, conditional branch to then/else blocks, merge
/// - while: Header block (condition), body block, back-edge to header
/// - for-in: Lower to while loop with iterator variable
/// - match: Chain of conditional branches for patterns
///
/// ## Type Layout
///
/// Value and class types compute field layouts:
/// - StructTypeInfo: Inline field layout with total size
/// - ClassTypeInfo: Fields after 16-byte object header, class ID for RTTI
/// - Field offsets computed during type registration
///
/// ## Runtime Integration
///
/// Runtime calls use RuntimeNames.hpp constants. The lowerer:
/// 1. Tracks used external functions in usedExterns_
/// 2. Emits extern declarations for all used runtime functions
/// 3. Uses RuntimeSignatures.hpp for function signatures
///
/// ## Boxing/Unboxing
///
/// For generic collections (List[T], Map[K,V]):
/// - emitBox(): Allocate heap space and store primitive value
/// - emitUnbox(): Load primitive value from boxed pointer
///
/// @see Lowerer.hpp for the class interface
/// @see RuntimeNames.hpp for runtime function name constants
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace il::frontends::zia {

using namespace runtime;

/// @brief Construct a Lowerer with a reference to the semantic analyzer and compiler options.
/// @param sema The semantic analyzer providing type and symbol resolution.
/// @param diag Diagnostic engine that receives lowering invariant failures.
/// @param options Compiler options controlling code generation behaviour.
Lowerer::Lowerer(Sema &sema, il::support::DiagnosticEngine &diag, CompilerOptions options)
    : sema_(sema), diag_(diag), options_(options) {}

/// @brief Lower a complete Zia module AST to IL.
/// @details Initializes the IL module, lowers all declarations, processes pending generic
///          instantiations, emits string constants, and declares used external functions.
/// @param module The analyzed module AST to lower.
/// @return The generated IL module.
Lowerer::Module Lowerer::lower(ModuleDecl &module) {
    // Initialize state
    module_ = std::make_unique<Module>();
    builder_ = std::make_unique<il::build::IRBuilder>(*module_);
    locals_.clear();
    localTypes_.clear();
    slots_.clear();
    usedExterns_.clear();
    definedFunctions_.clear();
    structTypes_.clear();
    classTypes_.clear();
    interfaceTypes_.clear();
    enumVariantValues_.clear();
    globalConstants_.clear();
    globalVariables_.clear();
    globalInitializers_.clear();
    pendingClassInstantiations_.clear();
    pendingStructInstantiations_.clear();
    pendingFunctionInstantiations_.clear();
    asyncOwnedValues_.clear();
    deferredTemps_.clear();
    cleanupStack_.clear();
    catchErrorBindings_.clear();
    activeCatchErrors_.clear();
    namespacePrefix_.clear();
    currentFunc_ = nullptr;
    currentReturnType_ = nullptr;
    currentStructType_ = nullptr;
    currentClassType_ = nullptr;
    lambdaCounter_ = 0;
    nextClassId_ = 1;
    nextIfaceId_ = 1;

    // Setup string table emitter
    /// @brief Materializes a newly interned Zia string as a module global.
    /// @param label Stable string-table label.
    /// @param content String bytes to emit.
    stringTable_.setEmitter([this](const std::string &label, const std::string &content) {
        builder_->addGlobalStr(label, content);
    });

    // Pre-pass 0: register enum variant values so that `final` constants
    // can reference enum variants (e.g., `final PS_IDLE = PlayerState.Idle`).
    registerAllEnumValues(module.declarations);

    // Pre-pass 1: register all `final` constants so that class/function
    // method bodies can reference constants defined later in the same file.
    registerAllFinalConstants(module.declarations);
    registerAllGlobalVariables(module.declarations);

    // Pre-pass 2: register all class/struct type layouts so that field access
    // in any method body can resolve types, even for forward-declared entities.
    // This fixes BUG-FE-006 where class A's methods reference class B
    // declared later in the same file.
    registerAllTypeLayouts(module.declarations);

    // Lower all declarations
    for (auto &decl : module.declarations) {
        lowerDecl(decl.get());
    }

    // Process pending generic class instantiations
    // These were deferred during expression lowering because we couldn't
    // lower methods while inside another function's body
    while (!pendingClassInstantiations_.empty()) {
        std::string typeName = pendingClassInstantiations_.back();
        pendingClassInstantiations_.pop_back();

        auto it = classTypes_.find(typeName);
        if (it == classTypes_.end())
            continue;

        ClassTypeInfo &info = it->second;

        // Push substitution context so type parameters resolve correctly
        bool pushedContext = sema_.pushSubstitutionContext(typeName);

        // Lower all methods for this instantiated generic class
        for (auto *method : info.methods) {
            lowerMethodDecl(*method, typeName, true);
        }

        // Emit vtable
        if (!info.vtable.empty()) {
            emitVtable(info);
        }

        // Pop substitution context if we pushed one
        if (pushedContext) {
            sema_.popTypeParams();
        }
    }

    // Process pending generic struct type instantiations
    while (!pendingStructInstantiations_.empty()) {
        std::string typeName = pendingStructInstantiations_.back();
        pendingStructInstantiations_.pop_back();

        auto it = structTypes_.find(typeName);
        if (it == structTypes_.end())
            continue;

        StructTypeInfo &info = it->second;

        // Push substitution context so type parameters resolve correctly
        bool pushedContext = sema_.pushSubstitutionContext(typeName);

        // Lower all methods for this instantiated generic struct type
        for (auto *method : info.methods) {
            lowerMethodDecl(*method, typeName, false);
        }

        // Pop substitution context if we pushed one
        if (pushedContext) {
            sema_.popTypeParams();
        }
    }

    // Process pending generic function instantiations
    while (!pendingFunctionInstantiations_.empty()) {
        auto [mangledName, decl] = pendingFunctionInstantiations_.back();
        pendingFunctionInstantiations_.pop_back();

        // Lower the generic function instantiation
        lowerGenericFunctionInstantiation(mangledName, decl);
    }

    // Emit destructor dispatch after all concrete class destructors exist.
    emitDestructorDispatch();

    // Emit interface registration and itable binding
    emitItableInit();

    // Add extern declarations for used runtime functions
    for (const auto &externName : usedExterns_) {
        // Skip functions defined in this module
        if (definedFunctions_.count(externName) > 0)
            continue;

        // Skip methods defined in this module (struct type and class type methods)
        bool isLocalMethod = false;
        auto dotPos = externName.rfind('.');
        if (dotPos != std::string::npos) {
            std::string typeName = externName.substr(0, dotPos);
            if (structTypes_.find(typeName) != structTypes_.end() ||
                classTypes_.find(typeName) != classTypes_.end()) {
                isLocalMethod = true;
            }
        }
        if (isLocalMethod)
            continue;

        const auto *desc = il::runtime::findRuntimeDescriptor(externName);
        if (!desc) {
            reportLoweringInvariant({},
                                    "V-ZIA-LOWER-MISSING-RUNTIME",
                                    "runtime helper '" + externName +
                                        "' was referenced but no runtime descriptor exists");
            continue;
        }
        builder_->addExtern(
            std::string(desc->name), desc->signature.retType, desc->signature.paramTypes);
    }

    // Reclaim loop-body variable slots (ZB-11): the VM bump-allocates every
    // executed `alloca`, so a slot lowered into a loop body grows the frame
    // each iteration until it overflows. Hoist provably non-escaping
    // constant-size allocas into the entry block so each slot is carved once
    // per activation; the in-place initializing stores keep declaration
    // semantics.
    for (auto &fn : module_->functions)
        hoistLoopAllocasToEntry(fn);

    return std::move(*module_);
}

/// @brief Hoist non-escaping constant-size allocas out of non-entry blocks.
/// @param fn Completed IL function to rewrite in place.
/// @details A slot may move only when every transitive use of its address is a
///          load address, a store destination, or a GEP base (GEP results are
///          tracked back to their root slot). Addresses that are stored as
///          values, passed to calls, or forwarded as branch arguments keep
///          their original position — sharing one entry slot across loop
///          iterations could otherwise be observed. Hoisted slots land
///          immediately before the entry block's terminator, which dominates
///          every other block.
void Lowerer::hoistLoopAllocasToEntry(il::core::Function &fn) {
    using il::core::BasicBlock;
    using il::core::Instr;
    using il::core::Opcode;
    using ValueK = il::core::Value;
    if (fn.blocks.size() < 2)
        return;

    // Exception-handling regions keep their allocas in place: the bytecode
    // backend's liveness does not track uses across unwind edges, so a slot
    // carved in the entry block but only used inside a handler would read a
    // reused register after the unwind.
    for (auto &bb : fn.blocks) {
        for (const Instr &in : bb.instructions) {
            switch (in.op) {
                case Opcode::EhPush:
                case Opcode::EhPop:
                case Opcode::EhEntry:
                case Opcode::ResumeSame:
                case Opcode::ResumeNext:
                case Opcode::ResumeLabel:
                    return;
                default:
                    break;
            }
        }
    }

    struct Site {
        BasicBlock *block;
        size_t index;
        unsigned temp;
    };
    std::vector<Site> sites;
    for (size_t b = 1; b < fn.blocks.size(); ++b) {
        BasicBlock &bb = fn.blocks[b];
        for (size_t i = 0; i < bb.instructions.size(); ++i) {
            const Instr &in = bb.instructions[i];
            if (in.op == Opcode::Alloca && in.result && in.operands.size() == 1 &&
                in.operands[0].kind == ValueK::Kind::ConstInt)
                sites.push_back({&bb, i, *in.result});
        }
    }
    if (sites.empty())
        return;

    // Escape analysis over address temps: rootOf maps each tracked temp to
    // the alloca slot it derives from; any disallowed use poisons the root.
    std::unordered_map<unsigned, unsigned> rootOf;
    std::unordered_set<unsigned> badRoots;
    for (const Site &s : sites)
        rootOf[s.temp] = s.temp;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &bb : fn.blocks) {
            for (const Instr &in : bb.instructions) {
                for (const auto &argVec : in.brArgs) {
                    for (const auto &arg : argVec) {
                        if (arg.kind != ValueK::Kind::Temp)
                            continue;
                        auto it = rootOf.find(arg.id);
                        if (it != rootOf.end() && badRoots.insert(it->second).second)
                            changed = true;
                    }
                }
                for (size_t oi = 0; oi < in.operands.size(); ++oi) {
                    const auto &v = in.operands[oi];
                    if (v.kind != ValueK::Kind::Temp)
                        continue;
                    auto it = rootOf.find(v.id);
                    if (it == rootOf.end())
                        continue;
                    bool ok = false;
                    if ((in.op == Opcode::Load || in.op == Opcode::Store) && oi == 0) {
                        ok = true;
                    } else if (in.op == Opcode::GEP && oi == 0 && in.result) {
                        ok = true;
                        auto inserted = rootOf.emplace(*in.result, it->second);
                        if (inserted.second)
                            changed = true;
                    }
                    if (!ok && badRoots.insert(it->second).second)
                        changed = true;
                }
            }
        }
    }

    std::vector<Instr> hoisted;
    // Erase per block in descending index order so indices stay valid.
    for (auto rit = sites.rbegin(); rit != sites.rend(); ++rit) {
        if (badRoots.count(rit->temp))
            continue;
        auto &insts = rit->block->instructions;
        hoisted.push_back(insts[rit->index]);
        // Scrub the source location: the slot now executes at function entry,
        // and keeping the body line would let a source breakpoint on that
        // line resolve to an entry-block PC (debugger would stop before the
        // function's locals exist).
        hoisted.back().loc = {};
        insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(rit->index));
    }
    if (hoisted.empty())
        return;

    BasicBlock &entry = fn.blocks[0];
    size_t insertAt = entry.instructions.size();
    for (size_t i = 0; i < entry.instructions.size(); ++i) {
        if (il::core::getOpcodeInfo(entry.instructions[i].op).isTerminator) {
            insertAt = i;
            break;
        }
    }
    entry.instructions.insert(entry.instructions.begin() + static_cast<std::ptrdiff_t>(insertAt),
                              hoisted.begin(),
                              hoisted.end());
}

} // namespace il::frontends::zia
