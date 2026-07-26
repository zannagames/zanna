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
#include "il/runtime/RuntimeSignatures.hpp"
#include <cctype>

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

    return std::move(*module_);
}

} // namespace il::frontends::zia
