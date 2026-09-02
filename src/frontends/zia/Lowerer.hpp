//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/Lowerer.hpp
// Purpose: Define the type-checked Zia AST to Zanna IL lowering pipeline.
// Key invariants:
//   * Lowered functions contain typed IL with terminated reachable blocks.
//   * Managed references have one explicit owner across slots, calls, and CFG edges.
// Ownership:
//   * Lowerer borrows semantic-analysis and diagnostic state.
//   * It owns transient lowering state and transfers the completed Module by value.
// References: src/frontends/zia/Sema.hpp, src/frontends/zia/Lowerer_Emit.cpp,
//        docs/il/il-guide.md
//
//===----------------------------------------------------------------------===//
///
/// @file Lowerer.hpp
/// @brief IL code generation for the Zia programming language.
///
/// @details The Lowerer transforms a type-checked Zia AST into Zanna
/// Intermediate Language (IL). This is the final stage of the frontend
/// pipeline before the IL is passed to the VM or code generator.
///
/// ## Lowering Pipeline
///
/// The lowering process follows this order:
///
/// 1. **Type Layout Computation**
///    - Compute field offsets for value and class types
///    - Build method tables for dispatch
///    - Assign class IDs to class types
///
/// 2. **Declaration Lowering**
///    - Lower global variables to IL global slots
///    - Lower function declarations to IL functions
///    - Lower type declarations (methods, constructors)
///
/// 3. **Statement and Expression Lowering**
///    - Lower each function body to IL instructions
///    - Convert control flow to basic blocks with branches
///    - Generate calls to runtime library functions
///
/// ## IL Generation Concepts
///
/// **Basic Blocks:**
/// IL uses a control flow graph of basic blocks. Each block contains
/// a sequence of instructions ending with a terminator (branch, return).
/// The BlockManager helper tracks blocks and termination state.
///
/// **Values and Slots:**
/// - SSA values: Immutable results of instructions
/// - Slots: Stack-allocated mutable storage (for var declarations)
///
/// **Type Mapping:**
/// Zia types are mapped to IL types:
/// - Integer -> i64
/// - Number -> f64
/// - Boolean -> i1
/// - String -> str (runtime string handle)
/// - Entity/Collections -> ptr (heap-allocated objects)
///
/// ## Runtime Integration
///
/// The lowerer generates calls to the Zanna runtime for:
/// - Object allocation and deallocation
/// - String operations
/// - Collection operations (List, Map, Set)
/// - I/O (Terminal functions)
/// - Type checking and casting
///
/// Runtime function names follow the pattern `Zanna.Category.Function`,
/// which are resolved to runtime symbols during linking.
///
/// ## Usage Example
///
/// ```cpp
/// // After parsing and semantic analysis
/// Sema sema(diag);
/// sema.analyze(*module);
///
/// // Lower to IL
/// Lowerer lowerer(sema);
/// auto ilModule = lowerer.lower(*module);
///
/// // The IL module can now be executed or compiled
/// ```
///
/// @invariant Input AST must be type-checked (Sema::analyze successful).
/// @invariant Produced IL is well-formed and valid.
/// @invariant All runtime calls use RuntimeNames.hpp constants.
///
/// @see AST.hpp - Input AST types
/// @see Sema.hpp - Semantic analysis results
/// @see RuntimeNames.hpp - Runtime function name constants
/// @see il/core/Module.hpp - Output IL module
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/common/BlockManager.hpp"
#include "frontends/common/ExprResult.hpp"
#include "frontends/common/LoopContext.hpp"
#include "frontends/common/StringTable.hpp"
#include "frontends/zia/AST.hpp"
#include "frontends/zia/DebugLayoutExport.hpp"
#include "frontends/zia/LowererTypes.hpp"
#include "frontends/zia/Options.hpp"
#include "frontends/zia/Sema.hpp"
#include "frontends/zia/Types.hpp"
#include "frontends/zia/ZiaSupport.hpp"
#include "il/build/IRBuilder.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "support/source_location.hpp"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::frontends::zia {

class BinaryOperatorLowerer;
class CallArgumentLowerer;
class CollectionLowerer;
class RuntimeCallBuilder;

//===----------------------------------------------------------------------===//
/// @name Result Type
/// @brief Type alias for expression lowering results.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Result of lowering an expression.
/// @details Contains both the IL value and its type information.
/// Used to track expression results through lowering.
using LowerResult = ::il::frontends::common::ExprResult;

/// @}

// Type layout structures (FieldLayout, StructTypeInfo, ClassTypeInfo,
// InterfaceTypeInfo) are defined in LowererTypes.hpp.

//===----------------------------------------------------------------------===//
/// @name IL Lowerer
/// @{
//===----------------------------------------------------------------------===//

/// @brief Lowers Zia AST to Zanna IL.
/// @details Transforms a semantically analyzed AST into IL instructions.
/// The lowerer produces a complete IL Module that can be executed by the
/// VM or compiled to native code.
///
/// ## Lowering Process
///
/// The lower() method performs:
/// 1. Type registration - compute layouts for all types
/// 2. Function lowering - lower each function declaration
/// 3. String table finalization - emit string constants
/// 4. External function declarations - declare used runtime functions
///
/// ## Control Flow Lowering
///
/// Control flow constructs are lowered to basic blocks:
/// - `if`: conditional branch to then/else blocks, merge after
/// - `while`: header block with loop condition, body, back edge
/// - `for-in`: lowered to while with iterator variable
/// - `match`: chain of conditional branches for each arm
///
/// ## Value vs Reference Handling
///
/// - Struct types: Inline storage, copied on assignment
/// - Class types: Pointer storage, reference counted
/// - Optionals of reference types: null pointer represents none
///
/// @invariant sema_ must have successfully analyzed the input module.
class Lowerer {
  public:
    /// @brief IL core type aliases for convenience.
    /// @{
    using Type = il::core::Type;
    using Value = il::core::Value;
    using BasicBlock = il::core::BasicBlock;
    using Function = il::core::Function;
    using Module = il::core::Module;
    using Opcode = il::core::Opcode;
    /// @}

    /// @brief Create a lowerer with semantic analysis results.
    /// @param sema The semantic analyzer that analyzed the input module.
    /// @param diag Diagnostic sink for lowering invariant failures.
    /// @param options Frontend options affecting generated IL and diagnostics.
    ///
    /// @details The lowerer uses sema for:
    /// - Expression type lookup (sema.typeOf)
    /// - Runtime function resolution (sema.runtimeCallee)
    explicit Lowerer(Sema &sema, il::support::DiagnosticEngine &diag, CompilerOptions options = {});

    /// @brief Lower a module to IL.
    /// @param module The parsed and analyzed module.
    /// @return The IL Module (moved from internal storage).
    ///
    /// @details Performs complete lowering:
    /// 1. Compute type layouts
    /// 2. Lower all declarations
    /// 3. Emit string constants
    /// 4. Declare external functions
    Module lower(ModuleDecl &module);

    /// @brief Export per-class field layouts for the VM debugger (ADR 0138).
    /// @details Valid after lower(): walks classTypes_ and emits, for every
    ///          instantiated class, the flattened field list (inherited first)
    ///          with byte offsets and storage kinds, keyed by the runtime class
    ///          id that rt_obj_new_i64 stamps into instances.
    /// @return Value-owned debugger layout table keyed by runtime class ID.
    [[nodiscard]] DebugClassLayoutExport collectDebugClassLayouts() const;

    /// @brief Get the current source location for IL emission.
    /// @return Location stamped onto newly emitted instructions.
    [[nodiscard]] il::support::SourceLoc sourceLocation() const noexcept {
        return curLoc_;
    }

    /// @brief Set the current source location for IL emission.
    /// @param loc Location to stamp onto subsequently emitted instructions.
    void setSourceLocation(il::support::SourceLoc loc) noexcept {
        curLoc_ = loc;
    }

  private:
    friend class BinaryOperatorLowerer;
    friend class CallArgumentLowerer;
    friend class CollectionLowerer;
    friend class RuntimeCallBuilder;

    //=========================================================================
    /// @name State
    /// @brief Internal state maintained during lowering.
    /// @{
    //=========================================================================

    /// @brief Semantic analysis results.
    Sema &sema_;

    /// @brief Diagnostic engine for error reporting.
    il::support::DiagnosticEngine &diag_;

    /// @brief Frontend compilation options.
    CompilerOptions options_;

    /// @brief The IL module being constructed.
    std::unique_ptr<Module> module_;

    /// @brief IR builder for emitting instructions.
    std::unique_ptr<il::build::IRBuilder> builder_;

    /// @brief Current function being lowered.
    Function *currentFunc_{nullptr};

    /// @brief Current function return type (semantic).
    TypeRef currentReturnType_{nullptr};

    /// @brief Basic block manager for the current function.
    ::il::frontends::common::BlockManager blockMgr_;

    /// @brief Current source location for emitted IL instructions.
    /// @details Set by ZiaLocationScope RAII helper at statement/expression
    ///          boundaries. Stamped onto each emitted Instr in Lowerer_Emit.cpp.
    il::support::SourceLoc curLoc_{};

    /// @brief String constant table.
    ::il::frontends::common::StringTable stringTable_;

    /// @brief Loop context stack for break/continue.
    ::il::frontends::common::LoopContextStack loopStack_;

    /// @brief Active cleanup actions for non-local control flow.
    /// @details Each entry represents an enclosing try statement. Normal exits
    ///          from the try body must close the EH frame before running its
    ///          finally body; exits from catch bodies only run finally.
    struct CleanupFrame {
        /// @brief Finally body to emit for this cleanup frame, if any.
        Stmt *finallyBody{nullptr};
        /// @brief Whether an active EH frame must be popped first.
        bool popEhBeforeFinally{false};
    };

    std::vector<CleanupFrame> cleanupStack_;

    /// @brief Current namespace prefix for qualified names.
    /// @details When inside a namespace block, this contains the namespace path.
    /// Empty when at module level. Example: "MyLib.Internal"
    std::string namespacePrefix_;

    /// @brief Local variable bindings: name -> SSA value.
    /// @details Uses unordered_map for O(1) lookup instead of O(log n).
    std::unordered_map<std::string, Value> locals_;

    /// @brief Local variable types: name -> semantic type.
    /// @details Uses unordered_map for O(1) lookup instead of O(log n).
    std::unordered_map<std::string, TypeRef> localTypes_;

    /// @brief Mutable variable slots: name -> slot pointer.
    /// @details Uses unordered_map for O(1) lookup instead of O(log n).
    std::unordered_map<std::string, Value> slots_;

    /// @brief Slot snapshots at entry to each active loop.
    /// @details Break and continue use the innermost snapshot to release
    ///          managed locals created for the current iteration before the
    ///          non-local branch leaves their lexical blocks.
    std::vector<std::unordered_map<std::string, Value>> loopSlotSnapshots_;

    struct CatchErrorBinding {
        /// @brief Slot holding the caught trap kind.
        std::string kindSlot;
        /// @brief Slot holding the caught error code.
        std::string codeSlot;
        /// @brief Slot holding the caught source line.
        std::string lineSlot;
    };

    /// @brief Catch binding metadata snapshots, keyed by catch variable name.
    std::unordered_map<std::string, CatchErrorBinding> catchErrorBindings_;

    /// @brief Active catch error snapshots for bare `throw;` rethrow.
    std::vector<CatchErrorBinding> activeCatchErrors_;

    /// @brief External functions used (for declaration).
    /// @details Uses unordered_set for O(1) lookup instead of O(log n).
    std::unordered_set<std::string> usedExterns_;

    /// @brief Functions defined in this module.
    /// @details Uses unordered_set for O(1) lookup instead of O(log n).
    std::unordered_set<std::string> definedFunctions_;

    /// @brief Struct type layout information.
    /// @details Uses unordered_map for O(1) lookup instead of O(log n).
    std::unordered_map<std::string, StructTypeInfo> structTypes_;

    /// @brief Class type layout information.
    /// @details Uses unordered_map for O(1) lookup instead of O(log n).
    std::unordered_map<std::string, ClassTypeInfo> classTypes_;

    /// @brief Counter for generating unique lambda function names.
    /// @details Per-instance (not static) to prevent name collisions when
    ///          multiple Lowerer instances run in the same process (e.g., LSP).
    int lambdaCounter_{0};

    /// @brief Pending generic class instantiations that need methods lowered.
    /// @details Populated during expression lowering when generic entities are
    /// constructed, and processed after all declarations are lowered.
    std::vector<std::string> pendingClassInstantiations_;

    /// @brief Pending generic struct type instantiations that need methods lowered.
    std::vector<std::string> pendingStructInstantiations_;

    /// @brief Pending generic function instantiations that need to be lowered.
    /// @details Populated during call expression lowering when generic functions
    /// are called, and processed after all declarations are lowered.
    std::vector<std::pair<std::string, FunctionDecl *>> pendingFunctionInstantiations_;

    /// @brief Interface type information.
    /// @details Uses unordered_map for O(1) lookup instead of O(log n).
    std::unordered_map<std::string, InterfaceTypeInfo> interfaceTypes_;

    /// @brief Enum variant values: "EnumName.VariantName" -> I64 constant value.
    /// @details Populated during lowerEnumDecl(), used during field expression
    /// lowering to emit ConstInt for enum variant access.
    std::unordered_map<std::string, int64_t> enumVariantValues_;

    /// @brief True while lowering an async worker trampoline.
    bool currentAsyncWorker_{false};

    /// @brief Owned boxed or retained argument values that must be released on worker exit.
    std::vector<Value> asyncOwnedValues_;

    /// @brief Current struct type context (for self access).
    const StructTypeInfo *currentStructType_{nullptr};

    /// @brief Current class type context (for self access).
    const ClassTypeInfo *currentClassType_{nullptr};

    /// @brief Counter for assigning unique class IDs.
    int nextClassId_{1};

    /// @brief Counter for assigning unique interface IDs.
    int nextIfaceId_{1};

    /// @brief Global constant values: name -> IL value.
    /// @details Stores the lowered values of module-level constants
    /// (e.g., `Integer GAME_WIDTH = 70;`). Used during identifier
    /// resolution to replace constant references with their values.
    /// Uses unordered_map for O(1) lookup instead of O(log n).
    std::unordered_map<std::string, Value> globalConstants_;

    /// @brief Runtime global storage types: name -> semantic type.
    /// @details Stores the types of module-level mutable variables and
    /// runtime-initialized finals. Used for generating runtime storage
    /// access calls.
    /// Uses unordered_map for O(1) lookup instead of O(log n).
    std::unordered_map<std::string, TypeRef> globalVariables_;

    /// @brief Ordered runtime global initializer entries lowered at module startup.
    /// @details Preserves declaration order so later global initializers can
    ///          depend on earlier globals. Each initializer is lowered as a
    ///          real expression inside `start()`.
    struct GlobalInitializer {
        std::string name;
        TypeRef type;
        Expr *initializer = nullptr;
    };

    std::vector<GlobalInitializer> globalInitializers_;

    /// @brief Current expression lowering depth for recursion guard.
    unsigned exprLowerDepth_{0};
    /// @brief Current statement lowering depth for recursion guard.
    unsigned stmtLowerDepth_{0};
    /// @brief Maximum allowed lowering recursion depth.
    static constexpr unsigned kMaxLowerDepth = 512;

    //=========================================================================
    /// @name Deferred Release Tracking
    /// @brief Tracks temporary values that need release at statement boundary.
    /// @details When a runtime call returns a reference-counted value (string
    ///          or heap object), the result is added to deferredTemps_. At the
    ///          end of each statement, all remaining deferred temps are released.
    ///          Temps that are "consumed" (stored to a slot or returned) are
    ///          removed from the list before release.
    //=========================================================================

    /// @brief A temporary value pending release at statement boundary.
    struct DeferredRelease {
        Value value;          ///< The temporary SSA value to release.
        bool isString{false}; ///< true = rt_str_release_maybe; false = managed object release.
        size_t blockIdx{kInvalidIndex}; ///< Block where this temp was defined.
    };

    /// @brief Temporaries queued for release at the next statement boundary.
    std::vector<DeferredRelease> deferredTemps_;

    /// @brief Queue a temporary for release at statement boundary.
    void deferRelease(Value v, bool isString);

    /// @brief Emit release calls for all deferred temporaries, then clear.
    void releaseDeferredTemps();

    /// @brief Release deferred temporaries appended at or after @p first.
    /// @details Expression lowerers use this at control-flow edges so owned
    ///          values created inside one branch are reclaimed in their
    ///          defining block instead of becoming inaccessible at the merge.
    ///          Entries before @p first remain pending for the enclosing
    ///          expression or statement.
    void releaseDeferredTempsFrom(size_t first);

    /// @brief Emit the Zia-managed release sequence for a reference-counted value.
    /// @details Pointer values defer the decref so user `deinit` can run before
    ///          `rt_obj_free`. String values route directly to
    ///          `rt_str_release_maybe`.
    void emitManagedRelease(Value value, bool isString);

    /// @brief Emit the Zia-managed release sequence and return the new retain count.
    /// @details Used for explicit `Zanna.Memory.Release(...)` calls.
    Value emitManagedReleaseRet(Value value, bool isString);

    /// @brief Remove a value from the deferred list (it was consumed by a
    ///        store-to-slot or return, so it must NOT be released).
    /// @return true when a deferred entry was removed — i.e. the value carried
    ///         a transferred owned reference that the consumer now holds.
    bool consumeDeferred(Value v);

    /// @brief True when @p name is a user-declared local whose slot holds an
    ///        owned string reference (slot-ownership invariant).
    bool isOwnedStringSlot(const std::string &name) const;

    /// @brief Release every visible user-declared string slot (function exit).
    /// @details Every named string slot owns one reference (params retain at
    ///          bind, declarations/assignments retain on store), so function
    ///          exit releases each one exactly once. Internal slots (match,
    ///          loop machinery, __zia_return) keep move semantics and are
    ///          excluded by keying on localTypes_.
    void releaseLocalStringSlots();

    /// @brief Release string slots created inside the current lexical block.
    /// @param liveBefore Slot names visible before the block was entered.
    void releaseBlockStringSlots(const std::unordered_map<std::string, Value> &liveBefore);

    /// @brief Store a string value into a slot under the slot-ownership
    ///        discipline: the slot ends up owning exactly one reference.
    /// @details Owned temporaries (deferred call results) move in — their
    ///          deferred entry is consumed. Borrowed values (slot loads,
    ///          params, tuple fields) are retained. When @p releaseDisplaced
    ///          is true the previous occupant is released first; pass false
    ///          for freshly created (zeroed) slots.
    void storeOwnedStringToSlot(const std::string &name, Value value, bool releaseDisplaced);

    /// @brief Release the owned reference of one string slot (before removeSlot).
    void releaseOwnedStringSlot(const std::string &name);

    /// @brief True when @p name is a user-declared local whose slot owns a
    ///        managed object reference.
    bool isOwnedObjectSlot(const std::string &name) const;

    /// @brief Release every visible user-declared managed-object slot.
    void releaseLocalObjectSlots();

    /// @brief Release managed-object slots created inside the current lexical block.
    /// @param liveBefore Slot names visible before the block was entered.
    void releaseBlockObjectSlots(const std::unordered_map<std::string, Value> &liveBefore);

    /// @brief Store a managed object into an owning local slot.
    /// @details Owned temporaries move into the slot. Borrowed values are
    ///          retained. Reassignment releases the displaced reference.
    void storeOwnedObjectToSlot(const std::string &name, Value value, bool releaseDisplaced);

    /// @brief Release the owned reference of one managed-object slot.
    void releaseOwnedObjectSlot(const std::string &name);

    /// @brief Enter a loop and snapshot the slots that survive its body exits.
    void pushLoopScope(size_t breakTarget, size_t continueTarget);

    /// @brief Leave the innermost loop after its body has been lowered.
    void popLoopScope();

    /// @brief Release managed slots created inside the innermost loop body.
    void releaseCurrentLoopBodySlots();

    /// @brief Check if a semantic type is refcounted and needs release.
    bool needsRelease(TypeRef type) const;

    /// @brief Check if a semantic type is a string (vs heap object).
    bool isStringType(TypeRef type) const;

    /// @}
    //=========================================================================
    /// @name Block Management
    /// @brief Methods for managing basic blocks.
    /// @{
    //=========================================================================

    /// @brief Create a new basic block.
    /// @param base Base name for the block (e.g., "if.then").
    /// @return Index of the created block.
    size_t createBlock(const std::string &base);

    /// @brief Set the current block for instruction emission.
    /// @param blockIdx Index of the block to make current.
    void setBlock(size_t blockIdx);

    /// @brief Get a block by index.
    /// @param idx Block index.
    /// @return Reference to the BasicBlock.
    BasicBlock &getBlock(size_t idx) {
        return blockMgr_.getBlock(idx);
    }

    /// @brief Check if current block is terminated.
    /// @return True if the current block has a terminator.
    bool isTerminated() const {
        return blockMgr_.isTerminated();
    }

    /// @}
    //=========================================================================
    /// @name Declaration Lowering
    /// @brief Methods for lowering declarations.
    /// @{
    //=========================================================================

    /// @brief Lower any declaration (dispatcher).
    /// @param decl The declaration to lower.
    void lowerDecl(Decl *decl);

    /// @brief Lower a function declaration.
    /// @param decl The function declaration.
    void lowerFunctionDecl(FunctionDecl &decl);

    /// @brief Lower an `async func` declaration: emits an async worker
    ///        trampoline (`name__async_worker`) plus a public wrapper that
    ///        packages arguments into a heap env and starts the task.
    /// @details Pre-computed signature info is passed in from lowerFunctionDecl
    ///          to avoid re-resolving the function type.
    void lowerAsyncFunctionDecl(FunctionDecl &decl,
                                const std::string &mangledName,
                                const std::vector<il::core::Param> &params,
                                TypeRef returnType,
                                il::core::Type ilReturnType,
                                TypeRef declaredReturnType,
                                const std::vector<TypeRef> &cachedParamTypes);

    /// @brief Get or create StructTypeInfo for a type.
    /// @param typeName The type name (may be mangled for generics).
    /// @return Pointer to StructTypeInfo, or nullptr if not found.
    ///
    /// @details For instantiated generic types, lazily creates the info
    /// from the original generic declaration and substituted field types.
    const StructTypeInfo *getOrCreateStructTypeInfo(const std::string &typeName);

    /// @brief Get or create ClassTypeInfo for an class type.
    /// @param typeName The type name (may be mangled for generics).
    /// @return Pointer to ClassTypeInfo, or nullptr if not found.
    ///
    /// @details For instantiated generic types, lazily creates the info
    /// from the original generic declaration and substituted field types.
    const ClassTypeInfo *getOrCreateClassTypeInfo(const std::string &typeName);

    /// @brief Lower a struct type declaration.
    /// @param decl The struct type declaration.
    void lowerStructDecl(StructDecl &decl);

    /// @brief Lower an class type declaration.
    /// @param decl The class type declaration.
    void lowerClassDecl(ClassDecl &decl);

    /// @brief Pre-pass: register all class/struct type layouts without lowering methods.
    /// @details Ensures all type layouts are available in classTypes_/structTypes_
    ///          before any method bodies are lowered, fixing forward-reference issues
    ///          where class A's methods reference class B declared later.
    void registerAllTypeLayouts(std::vector<DeclPtr> &declarations);

    /// @brief Pre-register every enum's variant values before the main
    ///        lowering pass, so methods can reference enum members defined
    ///        later in the same file.
    void registerAllEnumValues(std::vector<DeclPtr> &declarations);
    /// @brief Pre-register all `final` constant declarations before the main lowering pass.
    /// @details Ensures that `final` constants are available in globalConstants_ before any
    ///          class/function method bodies are lowered, fixing forward-reference issues
    ///          where an class method references a `final` defined later in the same file.
    void registerAllFinalConstants(std::vector<DeclPtr> &declarations);
    /// @brief Pre-register mutable module variables so function bodies resolve them
    ///        regardless of declaration order (see Lowerer_Decl.cpp).
    void registerAllGlobalVariables(std::vector<DeclPtr> &declarations);

    /// @brief Register a single class type's field layout without lowering methods.
    /// @param decl The class declaration.
    void registerClassLayout(ClassDecl &decl);

    /// @brief Compute field offsets for an class declaration's own fields.
    /// @param decl The class declaration.
    /// @param info The class type info to populate with field layouts.
    /// @param qualifiedName The fully qualified type name.
    void computeClassFieldLayout(ClassDecl &decl,
                                 ClassTypeInfo &info,
                                 const std::string &qualifiedName);

    /// @brief Build vtable entries from an class declaration's methods and properties.
    /// @param decl The class declaration.
    /// @param info The class type info to populate with vtable slots.
    /// @param qualifiedName The fully qualified type name.
    void buildClassVtable(ClassDecl &decl, ClassTypeInfo &info, const std::string &qualifiedName);

    /// @brief Copy inherited fields, totalSize, and vtable from parent class.
    /// @param info The child class type info to populate.
    /// @param parent The parent class type info to copy from.
    void inheritClassMembers(ClassTypeInfo &info, const ClassTypeInfo &parent);

    /// @brief Register a single struct type's field layout without lowering methods.
    /// @param decl The value declaration.
    void registerStructLayout(StructDecl &decl);

    /// @brief Register a single interface's dispatch metadata without lowering default methods.
    /// @param decl The interface declaration.
    void registerInterfaceLayout(InterfaceDecl &decl);

    /// @brief Try to evaluate an initializer expression to a compile-time constant.
    /// @param init The expression to fold.
    /// @return The folded constant value, or nullopt if not foldable.
    std::optional<il::core::Value> tryFoldNumericConstant(Expr *init);

    /// @brief Emit vtable global for an class type.
    /// @param info The class type info with vtable entries.
    void emitVtable(const ClassTypeInfo &info);

    /// @brief Lower an interface declaration.
    /// @param decl The interface declaration.
    void lowerInterfaceDecl(InterfaceDecl &decl);

    /// @brief Lower an interface default method body.
    void lowerInterfaceDefaultMethodDecl(MethodDecl &decl, const std::string &interfaceName);

    /// @brief Lower an enum declaration: register variant values.
    /// @param decl The enum declaration.
    void lowerEnumDecl(EnumDecl &decl);

    /// @brief Emit interface registration and itable binding for all interfaces.
    /// @details Emits a __zia_iface_init function that:
    ///   1. Registers each class via rt_register_class_with_base_rs
    ///   2. Registers each interface via rt_register_interface_direct_rs
    ///   3. For each implementing class, allocates an itable, populates it
    ///      with function pointers, and binds it via rt_bind_interface
    void emitItableInit();

    /// @brief ADR 0315: emit `__zia_layout_init`, which registers every class's
    ///        strong object-typed field offsets with the runtime
    ///        (rt_obj_class_layout_begin / rt_obj_class_layout_add_slot) so the
    ///        cycle collector can traverse class instances. Always emitted (a
    ///        `ret void` stub when no class has a strong slot) and called from
    ///        the entry prologue right after the destructor hook is installed.
    void emitClassLayoutInit();

    /// @brief One managed slot inside an object or boxed value payload.
    struct ManagedSlot {
        size_t offset;
        int64_t kind; ///< 1 = object reference, 2 = string.
    };

    /// @brief Flatten the managed (reference-bearing) slots of a semantic type.
    /// @details Recurses through inline structs, fixed arrays and tuples,
    ///          accumulating byte offsets; strings are kind 2, object-like
    ///          references (classes, interfaces, functions, Any, runtime
    ///          handles) kind 1. Shared by boxed value-type registration and
    ///          the class layout map.
    void collectManagedSlots(TypeRef type, size_t baseOffset, std::vector<ManagedSlot> &out);

    /// @brief Resolve the lowered name of an interface's default method body
    ///        (returns "" when the interface method is abstract).
    std::string defaultInterfaceMethodName(const InterfaceTypeInfo &ifaceInfo,
                                           MethodDecl *ifaceMethod);

    /// @brief Emit a small adapter function that translates an interface
    ///        wrapper `self` pointer back to the inline struct payload and
    ///        forwards to the struct's implementing method.
    /// @return The adapter's lowered function name (cached/idempotent).
    std::string emitStructInterfaceAdapter(const StructTypeInfo &structInfo,
                                           const InterfaceTypeInfo &ifaceInfo,
                                           MethodDecl *ifaceMethod,
                                           MethodDecl *implMethod);

    /// @brief Emit the destructor dispatcher used by managed release paths.
    /// @details Generates `__zia_dtor_dispatch(self: Ptr) -> Void`, which
    ///          switches on `rt_obj_class_id(self)` and invokes the matching
    ///          synthesized `Type.__dtor` for Zia entities.
    void emitDestructorDispatch();

    /// @brief Lower a namespace declaration.
    /// @param decl The namespace declaration.
    /// @details Processes all declarations within the namespace, using
    /// qualified names for code generation.
    void lowerNamespaceDecl(NamespaceDecl &decl);

    /// @brief Compute qualified name for code generation.
    /// @param name The unqualified name.
    /// @return The fully qualified name including namespace prefix.
    std::string qualifyName(const std::string &name) const;

    /// @brief Compute a declaration's lowered owner/type name.
    std::string declarationName(const Decl &decl, const std::string &name) const;

    /// @brief Lower a global variable declaration.
    /// @param decl The global variable declaration.
    /// @details Handles module-level constants by storing their values in
    /// globalConstants_ for later resolution during identifier lowering.
    void lowerGlobalVarDecl(GlobalVarDecl &decl);

    /// @brief Emit ordered mutable global initializers into the current `start()` body.
    void emitGlobalInitializers();

    /// @brief Lower a method declaration within a type.
    /// @param decl The method declaration.
    /// @param typeName The enclosing type name.
    /// @param isClass True if this is a class method.
    void lowerMethodDecl(MethodDecl &decl, const std::string &typeName, bool isClass = false);

    /// @brief Lower a property declaration by synthesizing get_/set_ methods.
    void lowerPropertyDecl(PropertyDecl &decl, const std::string &typeName, bool isClass = false);

    /// @brief Lower a destructor declaration by emitting a __dtor function.
    /// @details Synthesizes `TypeName.__dtor(self: Ptr) -> Void` that runs user
    ///          body then releases reference-typed fields.
    void lowerDestructorDecl(DestructorDecl &decl, const std::string &typeName);

    /// @brief True when a field holds a managed reference the destructor must release.
    bool fieldNeedsRelease(const FieldLayout &field);

    /// @brief True when instances of @p info need a `__dtor` (user `deinit`, releasable
    ///        fields, or a base class that needs one).
    bool classNeedsDestructor(const ClassTypeInfo &info);

    /// @}
    //=========================================================================
    /// @name Statement Lowering
    /// @brief Methods for lowering statements.
    /// @{
    //=========================================================================

    /// @brief Lower any statement (dispatcher).
    /// @param stmt The statement to lower.
    void lowerStmt(Stmt *stmt);

    /// @brief Lower a block statement.
    /// @param stmt The block statement.
    void lowerBlockStmt(BlockStmt *stmt);

    /// @brief Lower an expression statement.
    /// @param stmt The expression statement.
    void lowerExprStmt(ExprStmt *stmt);

    /// @brief Lower a variable declaration statement.
    /// @param stmt The variable statement.
    void lowerVarStmt(VarStmt *stmt);

    /// @brief Lower an if statement.
    /// @param stmt The if statement.
    void lowerIfStmt(IfStmt *stmt);

    /// @brief Lower a while statement.
    /// @param stmt The while statement.
    void lowerWhileStmt(WhileStmt *stmt);

    /// @brief Lower a C-style for statement.
    /// @param stmt The for statement.
    void lowerForStmt(ForStmt *stmt);

    struct RangeModifierInfo; // defined below

    /// @brief Lower a for-in statement.
    /// @param stmt The for-in statement.
    void lowerForInStmt(ForInStmt *stmt);

    /// @brief Lower a for-in over an integer range (with optional step / reversed).
    void lowerForInRange(ForInStmt *stmt, RangeExpr *rangeExpr, const RangeModifierInfo &rangeInfo);

    /// @brief Lower a single-iteration tuple-destructuring for-in
    ///        (`for (a, b) in twoElementTuple`).
    void lowerForInTuple(ForInStmt *stmt, TypeRef iterableType);

    /// @brief Lower a for-in over a List collection (with optional index binding).
    void lowerForInList(ForInStmt *stmt, TypeRef iterableType);

    /// @brief Lower a for-in over a String, yielding one-character Strings.
    void lowerForInString(ForInStmt *stmt);

    /// @brief Lower a for-in over a Map collection (iterates `Map.Keys()`).
    void lowerForInMap(ForInStmt *stmt, TypeRef iterableType);

    /// @brief Lower a for-in over any sequence (typed runtime `Seq<T>` or a
    ///        collection converted via `*.toSeq()`).
    /// @param seqValue        The already-lowered sequence pointer.
    /// @param elemType        Surface element type.
    /// @param rawStringElements True when the sequence stores raw rt_string
    ///        (e.g., from `Map.Keys()`) so kSeqGetStr is used instead of
    ///        kSeqGet + unbox.
    /// @param labelPrefix     Block-name prefix used for diagnostics.
    void lowerForInSeq(LowerResult seqValue,
                       ForInStmt *stmt,
                       TypeRef elemType,
                       bool rawStringElements,
                       const std::string &labelPrefix);

    /// @brief Lower a return statement.
    /// @param stmt The return statement.
    void lowerReturnStmt(ReturnStmt *stmt);

    /// @brief Lower a break statement.
    /// @param stmt The break statement.
    void lowerBreakStmt(BreakStmt *stmt);

    /// @brief Lower a continue statement.
    /// @param stmt The continue statement.
    void lowerContinueStmt(ContinueStmt *stmt);

    /// @brief Register a deferred cleanup action for the current block.
    /// @param stmt The defer statement.
    void lowerDeferStmt(DeferStmt *stmt);

    /// @brief Lower a guard statement.
    /// @param stmt The guard statement.
    void lowerGuardStmt(GuardStmt *stmt);

    /// @brief Lower a match statement.
    /// @param stmt The match statement.
    void lowerMatchStmt(MatchStmt *stmt);

    /// @brief Lower a try/catch/finally statement.
    /// @param stmt The try statement.
    void lowerTryStmt(TryStmt *stmt);

    /// @brief Lower a throw statement.
    /// @param stmt The throw statement.
    void lowerThrowStmt(ThrowStmt *stmt);

    /// @brief Emit an `eh.pop` instruction for the active protected region.
    void emitEhPop();

    /// @brief Emit all active cleanup frames for a non-local exit.
    /// @details Runs innermost-to-outermost cleanups and exposes only the
    ///          still-outer frames while lowering each finally body, so control
    ///          transfers inside a finally do not recursively run the same
    ///          cleanup again.
    void emitActiveCleanups();

    /// @brief Emit cleanup frames added at or after @p startIndex.
    void emitCleanupsFrom(size_t startIndex);

    /// @brief Run catch-body cleanup frames before emitting a throw.
    /// @details Throws from try bodies are handled by the active EH frame, but
    ///          throws from catch bodies must explicitly run their `finally`
    ///          clauses before propagating to an outer handler.
    void emitCatchBodyCleanupsBeforeThrow();

    /// @}
    //=========================================================================
    /// @name Expression Lowering
    /// @brief Methods for lowering expressions.
    /// @{
    //=========================================================================

    /// @brief Lower any expression (dispatcher).
    /// @param expr The expression to lower.
    /// @return The lowered result value and type.
    LowerResult lowerExpr(Expr *expr);

    /// @brief Lower an integer literal.
    /// @return LowerResult with i64 constant.
    LowerResult lowerIntLiteral(IntLiteralExpr *expr);

    /// @brief Lower a number (float) literal.
    /// @return LowerResult with f64 constant.
    LowerResult lowerNumberLiteral(NumberLiteralExpr *expr);

    /// @brief Lower a string literal.
    /// @return LowerResult with pointer to string constant.
    LowerResult lowerStringLiteral(StringLiteralExpr *expr);

    /// @brief Lower a boolean literal.
    /// @return LowerResult with i64 constant (0 or 1).
    LowerResult lowerBoolLiteral(BoolLiteralExpr *expr);

    /// @brief Lower a null literal.
    /// @return LowerResult with null pointer.
    LowerResult lowerNullLiteral(NullLiteralExpr *expr);

    /// @brief Lower a unit literal.
    /// @return LowerResult with void/unit placeholder.
    LowerResult lowerUnitLiteral(UnitLiteralExpr *expr);

    /// @brief Lower an identifier expression.
    /// @return LowerResult with the variable's value.
    LowerResult lowerIdent(IdentExpr *expr);

    /// @brief Lower an assignment expression (BinaryOp::Assign).
    /// @details Handles identifier, field, index, and global assignment targets
    ///          with type coercion and optional conversions.
    LowerResult lowerAssignment(BinaryExpr *expr);

    /// @brief Lower an `ident = value` assignment — handles local slots,
    ///        method-implicit fields (on `currentStructType_`/`currentClassType_`),
    ///        and module-scoped globals.
    LowerResult lowerIdentAssignment(BinaryExpr *expr,
                                     IdentExpr *ident,
                                     LowerResult right,
                                     TypeRef rightType);

    /// @brief Lower `base[index] = value` for List, Map, and FixedArray targets.
    LowerResult lowerIndexAssignment(BinaryExpr *expr,
                                     IndexExpr *indexExpr,
                                     LowerResult right,
                                     TypeRef rightType);

    /// @brief Lower `base.field = value` for module-static, class, struct,
    ///        and runtime-class property setters.
    LowerResult lowerFieldAssignment(BinaryExpr *expr,
                                     FieldExpr *fieldExpr,
                                     LowerResult right,
                                     TypeRef rightType);

    /// @brief Lower a binary expression.
    /// @return LowerResult with the operation result.
    LowerResult lowerBinary(BinaryExpr *expr);

    /// @brief Lower a short-circuit And/Or expression with lazy evaluation.
    /// @return LowerResult with the boolean result.
    LowerResult lowerShortCircuit(BinaryExpr *expr);

    /// @brief Lower a range expression to a list value.
    LowerResult lowerRange(RangeExpr *expr);

    struct RangeModifierInfo {
        RangeExpr *range{nullptr};
        bool reversed{false};
        Expr *stepArg{nullptr};
    };

    /// @brief Collect a range expression plus chained .rev()/.step(n) modifiers.
    bool collectRangeModifierChain(Expr *expr, RangeModifierInfo &out) const;

    /// @brief Lower a range expression plus already-validated modifier state to a list.
    LowerResult lowerRangeWithModifiers(RangeExpr *expr, bool reversed, Expr *stepArg);

    /// @brief Lower a unary expression.
    /// @return LowerResult with the operation result.
    LowerResult lowerUnary(UnaryExpr *expr);

    /// @brief Lower a ternary conditional expression.
    /// @return LowerResult with the selected branch value.
    LowerResult lowerTernary(TernaryExpr *expr);

    /// @brief Lower an if-expression (`if cond { then } else { else }`).
    /// @return LowerResult with the selected branch value.
    LowerResult lowerIfExpr(IfExpr *expr);

    /// @brief Lower a call expression.
    /// @return LowerResult with the call result.
    LowerResult lowerCall(CallExpr *expr);

    /// @brief Emit a runtime call with type-aware return-value handling
    ///        (specialised callee selection + unbox when the runtime returns
    ///        a Ptr that needs to be a value type at the surface).
    LowerResult emitRuntimeCallResult(const std::string &calleeName,
                                      TypeRef surfaceType,
                                      Type ilSurfaceType,
                                      const std::vector<Value> &callArgs);

    /// @brief Emit an explicit Memory.Release / heap-release call (matches
    ///        the runtime API for the argument's actual type: String,
    ///        user-defined class, or generic Ptr).
    LowerResult emitExplicitMemoryRelease(Value argValue, TypeRef argType);

    /// @brief Lower a generic function call.
    /// @param mangledName The instantiated function name (e.g., "identity$Integer").
    /// @param expr The call expression.
    /// @return LowerResult with the call result.
    LowerResult lowerGenericFunctionCall(const std::string &mangledName, CallExpr *expr);

    /// @brief Lower an instantiation of a generic function.
    /// @param mangledName The instantiated function name (e.g., "identity$Integer").
    /// @param decl The generic function declaration.
    void lowerGenericFunctionInstantiation(const std::string &mangledName, FunctionDecl *decl);

    /// @brief Lower a field access expression.
    /// @return LowerResult with the field value.
    LowerResult lowerField(FieldExpr *expr);

    /// @brief Lower a new expression (object creation).
    /// @return LowerResult with pointer to new object.
    LowerResult lowerNew(NewExpr *expr);

    /// @brief Lower `new T(...)` for a runtime (catalog) class — resolves the
    ///        constructor from RuntimeRegistry and marshals arguments.
    LowerResult lowerNewRuntimeClass(NewExpr *expr, TypeRef type);

    /// @brief Lower `new T(...)` for a user struct (stack-allocated value type).
    /// @return std::nullopt if @p type is not a known struct (fall through to class).
    std::optional<LowerResult> lowerNewStruct(NewExpr *expr, TypeRef type);

    /// @brief Lower `new T(...)` for a user class (heap object + vtable + ctor).
    LowerResult lowerNewClass(NewExpr *expr, TypeRef type);

    /// @brief Lower a struct-literal expression (`TypeName { field = val, ... }`).
    /// @return LowerResult with pointer to the initialized struct type on stack.
    LowerResult lowerStructLiteral(StructLiteralExpr *expr);

    /// @brief Lower a null coalesce expression.
    /// @return LowerResult with the coalesced value.
    LowerResult lowerCoalesce(CoalesceExpr *expr);

    /// @brief Lower an optional chain expression.
    /// @return LowerResult with optional result value.
    LowerResult lowerOptionalChain(OptionalChainExpr *expr);

    /// @brief Lower a method call through optional chaining.
    /// @return Optional-wrapped method result, or void for void methods.
    LowerResult lowerOptionalMethodCall(OptionalChainExpr *callee, CallExpr *expr);

    /// @brief Lower a list literal expression.
    /// @return LowerResult with pointer to new list.
    LowerResult lowerListLiteral(ListLiteralExpr *expr);

    /// @brief Lower a map literal expression.
    /// @return LowerResult with pointer to new map.
    LowerResult lowerMapLiteral(MapLiteralExpr *expr);

    /// @brief Lower a set literal expression.
    /// @return LowerResult with pointer to new set.
    LowerResult lowerSetLiteral(SetLiteralExpr *expr);

    /// @brief Lower a tuple literal expression.
    /// @return LowerResult with pointer to tuple on stack.
    LowerResult lowerTuple(TupleExpr *expr);

    /// @brief Lower a tuple index access expression.
    /// @return LowerResult with the element value.
    LowerResult lowerTupleIndex(TupleIndexExpr *expr);

    /// @brief Lower a block expression.
    /// @return LowerResult with the block's value (or void).
    LowerResult lowerBlockExpr(BlockExpr *expr);

    /// @brief Lower a match expression.
    /// @return LowerResult with the match result value.
    LowerResult lowerMatchExpr(MatchExpr *expr);

    /// @brief Lower an index expression.
    /// @return LowerResult with the element value.
    LowerResult lowerIndex(IndexExpr *expr);

    /// @brief Lower a try expression (propagate operator).
    /// @return LowerResult with unwrapped value or propagated null/error.
    LowerResult lowerTry(TryExpr *expr);

    /// @brief Lower a force-unwrap expression (expr!).
    /// @return LowerResult with unwrapped value; traps if null at runtime.
    LowerResult lowerForceUnwrap(ForceUnwrapExpr *expr);

    /// @brief Lower an await expression — calls Future.Get() on the operand.
    /// @return LowerResult with the resolved value (Ptr type).
    LowerResult lowerAwait(AwaitExpr *expr);

    /// @brief Lower a lambda expression.
    /// @return LowerResult with closure pointer.
    LowerResult lowerLambda(LambdaExpr *expr);

    /// @brief Lower an as (type cast) expression.
    /// @return LowerResult with value cast to target type.
    LowerResult lowerAs(AsExpr *expr);

    /// @brief Lower an is (type check) expression.
    /// @return LowerResult with boolean (I64) indicating type match.
    LowerResult lowerIsExpr(IsExpr *expr);

    /// @}
    //=========================================================================
    /// @name Instruction Emission Helpers
    /// @brief Low-level helpers for emitting IL instructions.
    /// @{
    //=========================================================================

    /// @brief Emit a binary arithmetic/comparison instruction.
    /// @param op The opcode (e.g., Opcode::IAdd, Opcode::ICmpEq).
    /// @param ty The result type.
    /// @param lhs Left operand value.
    /// @param rhs Right operand value.
    /// @return The result value.
    Value emitBinary(Opcode op, Type ty, Value lhs, Value rhs);

    /// @brief Emit a unary instruction.
    /// @param op The opcode (e.g., Opcode::INeg).
    /// @param ty The result type.
    /// @param operand The operand value.
    /// @return The result value.
    Value emitUnary(Opcode op, Type ty, Value operand);

    /// @brief Widen a Byte (i32) value to Integer (i64).
    /// @details Uses bitwise AND to zero-extend the value.
    /// @param value The i32 value to widen.
    /// @return The widened i64 value.
    Value widenByteToInteger(Value value);

    /// @brief Widen any supported integral IL value to i64.
    Value widenIntegralToI64(Value value, Type valueType);

    /// @brief Return whether a semantic Map uses the integer-keyed runtime backing store.
    /// @param mapType Semantic receiver type, expected to be a Map.
    /// @return True when @p mapType is `Map[Integer, T]`; false for string-keyed maps and
    ///         non-map types.
    bool usesIntegerMapRuntime(TypeRef mapType) const;

    /// @brief Coerce a lowered map key to the runtime ABI expected by its map backing store.
    /// @param keyValue Lowered key value.
    /// @param keyIlType IL type of @p keyValue.
    /// @param mapType Semantic map type used to select the runtime backing store.
    /// @return @p keyValue unchanged for string-keyed maps, or widened to i64 for IntMap.
    Value coerceMapKeyForRuntime(Value keyValue, Type keyIlType, TypeRef mapType);

    /// @brief Compare a pointer-like value against null.
    /// @param ptr Pointer or string value to check.
    /// @param ptrType IL type of @p ptr; Str and Ptr are supported.
    /// @return An i1 value that is true when @p ptr is non-null.
    /// @details The current IL comparison opcodes operate on integer operands, so this helper
    ///          stores the pointer-sized value in a temporary slot, reloads it as i64, and
    ///          compares the bits with zero.
    Value emitPointerIsNonNull(Value ptr, Type ptrType);

    /// @brief Emit an index bounds check when runtime bounds checks are enabled.
    Value emitIndexCheck(Value index, Value lowerInclusive, Value upperExclusive);

    /// @brief Emit a trap-backed check that a range step is strictly positive.
    Value emitPositiveStepCheck(Value stepValue);

    /// @brief Narrow an Integer (i64) value to Byte (i32) with overflow checking.
    /// @param value The i64 value to narrow.
    /// @return The narrowed i32 value.
    Value narrowIntegerToByte(Value value);

    /// @brief Apply sema-approved coercions to a lowered value.
    /// @param value The lowered IL value.
    /// @param valueIlType The current IL type of @p value.
    /// @param sourceType The semantic source type.
    /// @param targetType The semantic target type.
    /// @return The coerced value and IL type.
    LowerResult coerceValueToType(Value value,
                                  Type valueIlType,
                                  TypeRef sourceType,
                                  TypeRef targetType);

    /// @brief Lower explicit source arguments in source order.
    std::vector<LowerResult> lowerSourceArgs(const std::vector<CallArg> &args);

    /// @brief Return source-argument indexes in the semantically resolved order.
    std::vector<int> orderedArgSources(const std::vector<CallArg> &args,
                                       const Sema::CallArgBinding *binding) const;

    /// @brief Build final call arguments from a resolved argument binding.
    std::vector<Value> lowerResolvedArgs(const std::vector<CallArg> &args,
                                         const std::vector<TypeRef> &paramTypes,
                                         const std::vector<Param> *params,
                                         const Sema::CallArgBinding *binding);

    /// @brief Build final call arguments for a resolved call expression.
    std::vector<Value> lowerResolvedCallArgs(CallExpr *expr,
                                             const std::vector<TypeRef> &paramTypes,
                                             const std::vector<Param> *params);

    /// @brief Build final call arguments for a resolved new-expression.
    std::vector<Value> lowerResolvedNewArgs(NewExpr *expr,
                                            const std::vector<TypeRef> &paramTypes,
                                            const std::vector<Param> *params);

    /// @brief Pad missing arguments with default parameter values.
    /// @details Looks up the function declaration and lowers default expressions
    ///          for any trailing parameters that have default values and are missing.
    /// @param calleeName The function name.
    /// @param args The current argument values (may be padded in-place).
    /// @param callExpr The original call expression for arg count comparison.
    void padDefaultArgs(const std::string &calleeName,
                        std::vector<Value> &args,
                        CallExpr *callExpr);

    /// @brief Emit a function call with return value.
    /// @param retTy The expected return type.
    /// @param callee The function name.
    /// @param args The argument values.
    /// @return The return value.
    Value emitCallRet(Type retTy, const std::string &callee, const std::vector<Value> &args);

    /// @brief Emit a void function call.
    /// @param callee The function name.
    /// @param args The argument values.
    void emitCall(const std::string &callee, const std::vector<Value> &args);

    /// @brief Emit a void indirect function call.
    /// @param funcPtr The function pointer value.
    /// @param args The argument values.
    void emitCallIndirect(Value funcPtr, const std::vector<Value> &args);

    /// @brief Emit an indirect function call with return value.
    /// @param retTy The return type.
    /// @param funcPtr The function pointer value.
    /// @param args The argument values.
    /// @return The result value.
    Value emitCallIndirectRet(Type retTy, Value funcPtr, const std::vector<Value> &args);

    /// @brief Emit a function call with automatic void/return handling.
    /// @param callee The function name.
    /// @param args The argument values.
    /// @param returnType The return type (handles void automatically).
    /// @return LowerResult with return value (or dummy for void).
    LowerResult emitCallWithReturn(const std::string &callee,
                                   const std::vector<Value> &args,
                                   Type returnType);

    /// @brief Convert a value to a string representation.
    /// @param val The value to convert.
    /// @param sourceType The semantic type of the value.
    /// @return The string value (or original if already string).
    Value emitToString(Value val, TypeRef sourceType);

    /// @brief Emit an unconditional branch.
    /// @param targetIdx The target block index.
    void emitBr(size_t targetIdx);

    /// @brief Emit a conditional branch.
    /// @param cond The condition value (i64, 0 = false).
    /// @param trueIdx Block to branch to if true.
    /// @param falseIdx Block to branch to if false.
    void emitCBr(Value cond, size_t trueIdx, size_t falseIdx);

    /// @brief Emit a return instruction with value.
    /// @param val The value to return.
    void emitRet(Value val);

    /// @brief Emit a void return instruction.
    void emitRetVoid();

    /// @brief Emit a string constant load.
    /// @param globalName The global name of the string constant.
    /// @return Pointer value to the string.
    Value emitConstStr(const std::string &globalName);

    /// @brief Emit the canonical empty-string constant.
    Value emitEmptyString();

    /// @brief Get the next unique temporary ID.
    /// @return A unique temporary name counter.
    unsigned nextTempId();

    /// @brief Attach a source/debug name to an SSA value when it is unique.
    /// @param id SSA temporary identifier.
    /// @param name Preferred source-level name.
    void nameTemp(unsigned id, const std::string &name);

    /// @brief Emit a GEP (get element pointer) instruction.
    /// @param ptr The base pointer.
    /// @param offset The byte offset to add.
    /// @return The offset pointer.
    Value emitGEP(Value ptr, int64_t offset);

    /// @brief Emit a Load instruction.
    /// @param ptr The pointer to load from.
    /// @param type The type to load.
    /// @return The loaded value.
    Value emitLoad(Value ptr, Type type);

    /// @brief Emit a Store instruction.
    /// @param ptr The pointer to store to.
    /// @param val The value to store.
    /// @param type The type being stored.
    void emitStore(Value ptr, Value val, Type type);

    /// @brief Emit a field load from a struct pointer.
    /// @param field The field layout info.
    /// @param selfPtr Pointer to the struct.
    /// @return The loaded field value.
    Value emitFieldLoad(const FieldLayout *field, Value selfPtr);

    /// @brief Emit a field store to a struct pointer.
    /// @param field The field layout info.
    /// @param selfPtr Pointer to the struct.
    /// @param val The value to store.
    void emitFieldStore(const FieldLayout *field, Value selfPtr, Value val);

    /// @brief Copy an inline semantic value from one address to another.
    /// @details Handles nested structs, tuples, fixed arrays, and string
    ///          ownership. @p destInitialized controls whether old destination
    ///          string references must be released.
    void emitInlineValueCopy(TypeRef valueType,
                             Value destPtr,
                             Value sourcePtr,
                             bool destInitialized);

    /// @brief Store a lowered value into inline semantic storage.
    /// @details Aggregate values are represented by source pointers; scalar
    ///          values are stored directly with string retain/release handling.
    void emitInlineValueStore(TypeRef valueType, Value destPtr, Value value, bool destInitialized);

    /// @brief Store a value into a module-level variable slot under slot
    ///        ownership (the slot owns exactly one reference).
    /// @details Managed representations (Str and Ptr slots) transfer the
    ///          value's deferred +1 when present, retain a borrowed value
    ///          otherwise, and release the slot's previous occupant when
    ///          @p destInitialized is true. Non-managed representations store
    ///          directly. Without the retain, storing a borrowed reference
    ///          (e.g. a local variable) left the slot dangling once the
    ///          local's exit release ran (ZB-16 use-after-free).
    void emitGlobalManagedStore(Value addr, Value value, Type ilType, bool destInitialized);

    /// @brief Hoist non-escaping constant-size allocas into the entry block.
    /// @details ZB-11: the VM bump-allocates every executed `alloca`, so slots
    ///          lowered into loop bodies grow the frame each iteration until
    ///          it overflows. Runs over every completed function at the end of
    ///          lower(); escaping addresses (stored as values, passed to
    ///          calls, or forwarded as branch arguments) stay in place.
    void hoistLoopAllocasToEntry(il::core::Function &fn);

    /// @brief Move a managed reference out of a temporary result slot.
    /// @details Loads the owned handle, then clears the slot without releasing
    ///          it. The returned SSA value becomes the sole owner and is
    ///          scheduled or transferred by the caller.
    Value takeManagedValueFromSlot(Value slot, Type type);

    /// @brief Zero-initialize inline semantic storage.
    void emitInlineValueZero(TypeRef valueType, Value destPtr);

    /// @brief Allocate and zero-initialize stack storage for an inline semantic value.
    Value emitInlineValueAlloc(TypeRef valueType);

    /// @brief Deep copy a struct type (for copy-on-assign semantics).
    /// @param info The struct type info.
    /// @param sourcePtr Pointer to source value.
    /// @return Pointer to the new copy.
    Value emitStructTypeCopy(const StructTypeInfo &info, Value sourcePtr);

    /// @brief Initialize uninitialized struct storage from another struct value.
    void emitStructTypeInitialize(const StructTypeInfo &info, Value destPtr, Value sourcePtr);

    /// @brief Assign an existing struct storage location from another struct value.
    void emitStructTypeStore(const StructTypeInfo &info, Value destPtr, Value sourcePtr);

    /// @brief Allocate stack space for a struct type without initialization.
    /// @param info The struct type info.
    /// @return Pointer to the allocated (zero-initialized) space.
    Value emitStructTypeAlloc(const StructTypeInfo &info);

    /// @brief Materialize a call result into caller-owned storage when needed.
    /// @details Struct-typed calls return boxed value payloads across function
    ///          boundaries and are immediately copied back to stack storage here.
    LowerResult materializeCallResult(Value result, TypeRef semanticType, Type ilType);

    /// @brief Lower a method call.
    /// @param method The method declaration.
    /// @param typeName The type containing the method.
    /// @param selfValue The receiver value (self).
    /// @param expr The call expression for arguments.
    /// @return The call result.
    LowerResult lowerMethodCall(MethodDecl *method,
                                const std::string &typeName,
                                Value selfValue,
                                CallExpr *expr);

    /// @brief Lower a virtual method call using vtable dispatch.
    /// @param entityInfo The class type info with vtable.
    /// @param slotKey The resolved dispatch slot key.
    /// @param method The resolved method declaration.
    /// @param selfValue The receiver value (self).
    /// @param expr The call expression for arguments.
    /// @return The call result.
    LowerResult lowerVirtualMethodCall(const ClassTypeInfo &entityInfo,
                                       const std::string &slotKey,
                                       const std::string &ownerType,
                                       MethodDecl *method,
                                       Value selfValue,
                                       CallExpr *expr);

    /// @brief Lower an interface method call using class_id-based dispatch.
    /// @param ifaceInfo The interface type info.
    /// @param slotKey The resolved dispatch slot key.
    /// @param method The resolved method declaration from the interface.
    /// @param selfValue The receiver value (self).
    /// @param expr The call expression for arguments.
    /// @return The call result.
    LowerResult lowerInterfaceMethodCall(const InterfaceTypeInfo &ifaceInfo,
                                         const std::string &slotKey,
                                         const std::string &ownerType,
                                         MethodDecl *method,
                                         Value selfValue,
                                         CallExpr *expr);

    /// @brief Lower a method call on a List collection.
    /// @param baseValue The lowered list value.
    /// @param baseType The semantic type of the list.
    /// @param methodName The method name being called.
    /// @param expr The call expression for arguments.
    /// @return The call result, or nullopt if method not recognized.
    std::optional<LowerResult> lowerListMethodCall(Value baseValue,
                                                   TypeRef baseType,
                                                   const std::string &methodName,
                                                   CallExpr *expr);

    /// @brief Lower a functional combinator (map/filter/reduce/firstWhere/any/all/sum)
    ///        on a List into an inline loop that invokes the closure argument.
    /// @return The combinator result, or nullopt if @p methodName is not a combinator.
    std::optional<LowerResult> lowerListCombinator(Value baseValue,
                                                   TypeRef baseType,
                                                   const std::string &methodName,
                                                   CallExpr *expr);

    /// @brief Lower a method call on a Map collection.
    /// @param baseValue The lowered map value.
    /// @param baseType The semantic type of the map.
    /// @param methodName The method name being called.
    /// @param expr The call expression for arguments.
    /// @return The call result, or nullopt if method not recognized.
    std::optional<LowerResult> lowerMapMethodCall(Value baseValue,
                                                  TypeRef baseType,
                                                  const std::string &methodName,
                                                  CallExpr *expr);

    /// @brief Lower a method call on a Set collection.
    /// @param baseValue The lowered set value.
    /// @param baseType The semantic type of the set.
    /// @param methodName The method name being called.
    /// @param expr The call expression for arguments.
    /// @return The call result, or nullopt if method not recognized.
    std::optional<LowerResult> lowerSetMethodCall(Value baseValue,
                                                  TypeRef baseType,
                                                  const std::string &methodName,
                                                  CallExpr *expr);

    /// @brief Lower a built-in function call (print, println, toString).
    /// @param name The function name.
    /// @param expr The call expression.
    /// @return The call result, or nullopt if not a built-in.
    std::optional<LowerResult> lowerBuiltinCall(const std::string &name, CallExpr *expr);

    /// @brief Lower a struct type construction call.
    /// @param typeName The struct type name.
    /// @param expr The call expression with constructor arguments.
    /// @return The constructed value, or nullopt if not a struct type.
    std::optional<LowerResult> lowerStructTypeConstruction(const std::string &typeName,
                                                           CallExpr *expr);

    /// @brief Lower an class type construction call (Entity(args) syntax).
    /// @param typeName The class type name.
    /// @param expr The call expression with constructor arguments.
    /// @return The constructed class pointer, or nullopt if not an class type.
    std::optional<LowerResult> lowerClassTypeConstruction(const std::string &typeName,
                                                          CallExpr *expr);

    /// @}
    //=========================================================================
    /// @name Boxing/Unboxing Helpers
    /// @brief Helpers for boxing primitives for generic collections.
    /// @{
    //=========================================================================

    /// @brief Box a primitive value for collection storage.
    /// @param val The value to box.
    /// @param type The IL type of the value.
    /// @return Pointer to the boxed value.
    ///
    /// @details Allocates space for the value and stores it.
    /// Used when inserting primitives into List[T], Map[K,V], etc.
    Value emitBox(Value val, Type type);

    /// @brief Box a value with semantic type context.
    /// @param val The value to box.
    /// @param ilType The IL type.
    /// @param semanticType The semantic type for struct type detection.
    /// @return Boxed value (heap pointer).
    ///
    /// @details For struct types, heap-allocates a copy of the struct.
    /// For other types, falls back to standard emitBox() behavior.
    Value emitBoxValue(Value val, Type ilType, TypeRef semanticType);

    /// @brief Unbox a value to a primitive type.
    /// @param boxed The boxed pointer value.
    /// @param expectedType The expected IL type.
    /// @return The unboxed value with its type.
    ///
    /// @details Loads the value from the boxed pointer.
    /// Used when retrieving primitives from collections.
    LowerResult emitUnbox(Value boxed, Type expectedType);

    /// @brief Unbox a value with semantic type context.
    /// @param boxed The boxed pointer value.
    /// @param ilType The expected IL type.
    /// @param semanticType The semantic type for struct type detection.
    /// @return The unboxed value with its type.
    ///
    /// @details For struct types, copies from heap to stack for copy semantics.
    /// For other types, falls back to standard emitUnbox() behavior.
    LowerResult emitUnboxValue(Value boxed, Type ilType, TypeRef semanticType);

    /// @brief Wrap a value in optional storage (box primitives/strings when needed).
    /// @param val The value to wrap.
    /// @param innerType The semantic inner type of the optional.
    /// @return Pointer representing the optional value (null represents None).
    Value emitOptionalWrap(Value val, TypeRef innerType);

    /// @brief Unwrap an optional value to its inner IL type.
    /// @param val Pointer representing the optional value.
    /// @param innerType The semantic inner type of the optional.
    /// @return The unwrapped value with its type.
    LowerResult emitOptionalUnwrap(Value val, TypeRef innerType);

    /// @brief Wrap a value for optional field assignment if needed.
    /// @param val The value to potentially wrap.
    /// @param fieldType The field's semantic type (may or may not be optional).
    /// @param valueType The type of the value being assigned.
    /// @return The wrapped value if field is optional, otherwise the original value.
    Value wrapValueForOptionalField(Value val, TypeRef fieldType, TypeRef valueType);

    /// @brief Extend an operand value to i64 for integer comparison.
    /// @param val The value to extend.
    /// @param type The IL type of the value.
    /// @return The extended i64 value suitable for ICmpEq/ICmpNe.
    Value extendOperandForComparison(Value val, Type type);

    /// @}
    //=========================================================================
    /// @name Lowering Invariants
    /// @brief Helpers that turn missed semantic facts into compiler diagnostics.
    /// @{
    //=========================================================================

    /// @brief Report a lowerer invariant violation at @p loc.
    void reportLoweringInvariant(il::support::SourceLoc loc, std::string code, std::string message);

    /// @brief Check whether a semantic type is concrete enough for lowering.
    bool isInvalidLoweringType(TypeRef type) const;

    /// @brief Return a poison value after reporting a fatal lowering diagnostic.
    LowerResult poisonValue(il::support::SourceLoc loc, std::string code, std::string message);

    /// @}
    //=========================================================================
    /// @name Type Mapping
    /// @brief Methods for mapping Zia types to IL types.
    /// @{
    //=========================================================================

    /// @brief Map a semantic type to an IL type.
    /// @param type The Zia semantic type.
    /// @return The corresponding IL type.
    Type mapType(TypeRef type);

    /// @brief Map an IL type back to a semantic type.
    /// @param ilType The IL type.
    /// @return A corresponding semantic type (best effort).
    TypeRef reverseMapType(Type ilType);

    /// @brief Get the size in bytes for an IL type.
    /// @param type The IL type.
    /// @return Size in bytes.
    ///
    /// @details Size mapping:
    /// - i64, f64, ptr: 8 bytes
    /// - i32: 4 bytes
    /// - i16: 2 bytes
    /// - i1: 1 byte (but often stored as 8)
    static size_t getILTypeSize(Type type);

    /// @brief Get the alignment in bytes for an IL type.
    /// @param type The IL type.
    /// @return Alignment in bytes.
    ///
    /// @details Alignment ensures proper memory access for the type.
    /// Boolean (i1) aligns to 8 bytes to avoid misalignment issues
    /// when followed by pointer-sized fields.
    static size_t getILTypeAlignment(Type type);

    /// @brief Get the inline storage size for a semantic type.
    size_t getSemanticTypeSize(TypeRef type);

    /// @brief Get the inline storage alignment for a semantic type.
    size_t getSemanticTypeAlignment(TypeRef type);

    /// @brief Get the byte offset of a tuple element under inline layout.
    size_t getTupleElementOffset(TypeRef tupleType, size_t index);

    /// @brief Get total inline storage size for a tuple type.
    size_t getTupleStorageSize(TypeRef tupleType);

    /// @brief Align an offset to a given alignment boundary.
    /// @param offset The current offset.
    /// @param alignment The required alignment.
    /// @return The aligned offset (>= original offset).
    static size_t alignTo(size_t offset, size_t alignment);

    /// @}
    //=========================================================================
    /// @name Local Variable Management
    /// @brief Methods for managing function-local variables.
    /// @{
    //=========================================================================

    /// @brief Define an immutable local variable.
    /// @param name The variable name.
    /// @param value The SSA value.
    void defineLocal(const std::string &name, Value value);

    /// @brief Look up a local variable.
    /// @param name The variable name.
    /// @return Pointer to the value, or nullptr if not found.
    Value *lookupLocal(const std::string &name);

    /// @brief Create a mutable variable slot.
    /// @param name The variable name.
    /// @param type The variable type.
    /// @return Pointer value to the slot.
    Value createSlot(const std::string &name, Type type);

    /// @brief Store a value to a mutable slot.
    /// @param name The variable name.
    /// @param value The value to store.
    /// @param type The struct type.
    void storeToSlot(const std::string &name, Value value, Type type);

    /// @brief Load a value from a mutable slot.
    /// @param name The variable name.
    /// @param type The struct type.
    /// @return The loaded value.
    Value loadFromSlot(const std::string &name, Type type);

    /// @brief Remove a slot (for scope cleanup).
    /// @param name The variable name.
    void removeSlot(const std::string &name);

    /// @brief Get the self pointer for the current method.
    /// @details Checks both slots and locals for "self".
    /// @param result Output parameter for the self pointer value.
    /// @return True if self was found, false otherwise.
    bool getSelfPtr(Value &result);

    /// @}
    //=========================================================================
    /// @name Pattern Matching Helpers
    /// @brief Helpers for lowering match patterns.
    /// @{
    //=========================================================================

    struct PatternValue {
        Value value;
        TypeRef type;
    };

    /// @brief Emit control flow to test a pattern against a value.
    /// @param pattern The pattern to match.
    /// @param scrutinee The value/type being matched.
    /// @param successBlock Block to branch to on success.
    /// @param failureBlock Block to branch to on failure.
    void emitPatternTest(const MatchArm::Pattern &pattern,
                         const PatternValue &scrutinee,
                         size_t successBlock,
                         size_t failureBlock);

    /// @brief Emit bindings for a matched pattern in the current block.
    /// @param pattern The pattern to bind.
    /// @param scrutinee The value/type being matched (assumed to have matched).
    void emitPatternBindings(const MatchArm::Pattern &pattern, const PatternValue &scrutinee);

    /// @brief Load a tuple element value.
    /// @param tuple The tuple value/type.
    /// @param index The element index.
    /// @param elemType The semantic type for the element.
    /// @return The element value/type pair.
    PatternValue emitTupleElement(const PatternValue &tuple, size_t index, TypeRef elemType);

    /// @}
    //=========================================================================
    /// @name Helper Functions
    /// @brief Utility functions for lowering.
    /// @{
    //=========================================================================

    /// @brief Mangle a function name for IL.
    /// @param name The source-level function name.
    /// @return The mangled name.
    std::string mangleFunctionName(const std::string &name);

    /// @brief Get or create a global string constant.
    /// @param value The string value.
    /// @return The global name for the string.
    std::string getStringGlobal(const std::string &value);

    /// @brief Case-insensitive string comparison.
    /// @param a First string.
    /// @param b Second string.
    /// @return True if equal ignoring case.
    static bool equalsIgnoreCase(const std::string &a, const std::string &b);

    /// @brief Get the runtime helper name for module variable address lookup.
    /// @param kind The IL type kind.
    /// @return Runtime function name (e.g., "rt_modvar_addr_i64").
    std::string getModvarAddrHelper(Type::Kind kind);

    /// @brief Get the address of a global variable using runtime storage.
    /// @param name The variable name.
    /// @param type The semantic type.
    /// @return Pointer value to the variable storage.
    Value getGlobalVarAddr(const std::string &name, TypeRef type);

    /// @}
};

/// @}

} // namespace il::frontends::zia
