//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema.hpp
/// @brief Semantic analyzer for the Zia programming language.
///
/// @details The semantic analyzer performs type checking and name resolution
/// on the AST produced by the parser. It transforms raw AST nodes into a
/// semantically valid representation with resolved types and symbols.
///
/// ## Semantic Analysis Phases
///
/// The analyzer performs several passes over the AST:
///
/// **Phase 1: Type Registration**
/// - Registers all type declarations (struct, class, interface)
/// - Builds the type hierarchy (inheritance, interface implementation)
/// - Creates entries in the type registry
///
/// **Phase 2: Declaration Analysis**
/// - Analyzes global variable declarations
/// - Analyzes function declarations (signatures)
/// - Analyzes type members (fields and methods)
///
/// **Phase 3: Body Analysis**
/// - Type-checks function and method bodies
/// - Validates statements and expressions
/// - Ensures return types match declarations
///
/// ## Type System Features
///
/// The analyzer handles:
/// - Primitive types: Integer, Number, Boolean, String, Byte
/// - User-defined types: struct types, class types, interfaces
/// - Generic types: List[T], Map[K,V], Result[T]
/// - Optional types: T? with null safety checks
/// - Function types: (A, B) -> C for closures and references
///
/// ## Symbol Resolution
///
/// Symbols are resolved in nested scopes:
/// 1. Local variables in current block
/// 2. Parameters of enclosing function
/// 3. Fields/methods of enclosing type (via self)
/// 4. Module-level functions and global variables
/// 5. Built-in runtime functions
///
/// ## Error Reporting
///
/// The analyzer reports errors for:
/// - Undefined names and types
/// - Type mismatches in expressions and assignments
/// - Invalid operations (wrong types for operators)
/// - Missing or type-mismatched return statements
/// - Invalid assignments (to immutable variables)
///
/// ## Usage Example
///
/// ```cpp
/// DiagnosticEngine diag;
/// Lexer lexer(source, fileId, diag);
/// Parser parser(lexer, diag);
/// auto module = parser.parseModule();
///
/// Sema sema(diag);
/// bool success = sema.analyze(*module);
///
/// if (success) {
///     // Use sema.typeOf() to get expression types
///     // Use sema.runtimeCallee() for runtime function resolution
/// }
/// ```
///
/// @invariant Type information is immutable after analysis.
/// @invariant All expressions have associated type information after analysis.
/// @invariant Symbol table correctly reflects scope nesting.
///
/// @see AST.hpp - AST node types
/// @see Types.hpp - Semantic type representation
/// @see Lowerer.hpp - Consumes analyzed AST for code generation
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/AST.hpp"
#include "frontends/zia/Types.hpp"
#include "frontends/zia/WarningSuppressions.hpp"
#include "frontends/zia/Warnings.hpp"
#include "frontends/zia/sema/SemaTypes.hpp"
#include "support/diagnostics.hpp"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il::frontends::zia {

// Symbol, ScopedSymbol, and Scope types are defined in
// frontends/zia/sema/SemaTypes.hpp, included above.

/// @brief Owner-qualified identity for cached method-instantiation metadata.
/// @details A generic method declaration may be resolved differently for each instantiated
///          owner, so declaration pointer identity alone is insufficient for cache keys.
struct MethodInstanceKey {
    /// Concrete semantic owner type.
    std::string ownerType;
    /// Shared source method declaration.
    const MethodDecl *decl = nullptr;

    /// @brief Compare both the concrete owner and declaration identity.
    /// @param other Key to compare.
    /// @return True when both fields identify the same method instance.
    bool operator==(const MethodInstanceKey &other) const {
        return ownerType == other.ownerType && decl == other.decl;
    }
};

/// @brief Hash combiner for @ref MethodInstanceKey.
struct MethodInstanceKeyHash {
    /// @brief Hash an owner-qualified method identity.
    /// @param key Method-instance key to hash.
    /// @return Combined hash of the owner spelling and declaration pointer.
    size_t operator()(const MethodInstanceKey &key) const {
        size_t h1 = std::hash<std::string>{}(key.ownerType);
        size_t h2 = std::hash<const MethodDecl *>{}(key.decl);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6U) + (h1 >> 2U));
    }
};

//===----------------------------------------------------------------------===//
/// @name Semantic Analyzer
/// @{
//===----------------------------------------------------------------------===//

/// @brief Semantic analyzer for Zia programs.
/// @details Performs type checking, name resolution, and semantic validation
/// on parsed AST nodes. After successful analysis, provides access to:
/// - Expression types via typeOf()
/// - Type resolution via resolveType()
/// - Runtime function resolution via runtimeCallee()
///
/// ## Analysis Process
///
/// The analyze() method performs multi-pass analysis:
/// 1. Register built-in types and functions
/// 2. Process binds (bring runtime functions or file-module exports into scope)
/// 3. Register all type declarations
/// 4. Analyze global variables
/// 5. Analyze type members (fields, methods)
/// 6. Analyze function declarations
/// 7. Type-check all function/method bodies
///
/// ## Scope Management
///
/// Scopes are managed via pushScope()/popScope(). Each scope can contain:
/// - Local variables
/// - Parameters
/// - Nested scopes inherit access to parent scopes
///
/// ## Self and Return Type Context
///
/// The analyzer tracks:
/// - currentSelfType_: The type of `self` in methods
/// - expectedReturnType_: The declared return type for return validation
///
/// @invariant Scope stack is balanced (pushScope/popScope pairs).
/// @invariant Expression type map is populated after analyze().
class Sema {
    friend class Lowerer;

  public:
    /// @brief Bound argument layout for a resolved call site.
    /// @details `fixedParamSources[i]` is the source-argument index used for
    /// parameter `i`, or -1 when the parameter is satisfied by a default.
    /// Variadic source arguments remain in source order.
    struct CallArgBinding {
        /// Source index per fixed parameter, or -1 when the default is used.
        std::vector<int> fixedParamSources;
        /// Source indexes packed into the variadic tail.
        std::vector<int> variadicSources;
    };

    /// @brief Raw-pointer exposure metadata for runtime extern signatures.
    /// @details The Zia surface maps both runtime `obj` and low-level `ptr`
    ///          to IL pointers. This metadata preserves which parameters came
    ///          from explicit runtime `ptr` tokens so safe Zia can reject raw
    ///          pointer APIs while still allowing typed runtime objects.
    /// `None` denotes an ordinary pointer parameter, `Callback` opens a function-reference
    /// bridge, and `Payload` identifies data paired with a preceding validated callback.
    enum class RuntimePointerBridgeRole { None, Callback, Payload };

    /// @brief Raw-pointer safety metadata retained from a runtime signature.
    struct RuntimePointerSafety {
        /// Whether the result exposes a raw runtime handle.
        bool rawPointerReturn{false};
        /// Raw-pointer flag for every runtime parameter.
        std::vector<bool> rawPointerParams;
        /// Safe callback/payload role for every runtime parameter.
        std::vector<RuntimePointerBridgeRole> bridgeRoles;
    };

    /// @brief Create a semantic analyzer with the given diagnostic engine.
    /// @param diag Diagnostic engine for error reporting.
    ///
    /// @details Initializes the analyzer and registers built-in types and
    /// functions. The diagnostic engine is borrowed and must outlive the
    /// analyzer.
    explicit Sema(il::support::DiagnosticEngine &diag);

    /// @brief Initialize warning infrastructure with a policy.
    /// @param policy Warning policy controlling which warnings are enabled.
    /// @details Must be called before analyze() for warning support. If not
    ///          called, warnings use the default policy (conservative set).
    void initWarnings(const WarningPolicy &policy);

    /// @brief Configure whether explicit unsafe pointer usage is allowed.
    /// @param allow True to permit unsafe pointer syntax and raw runtime APIs.
    void setAllowUnsafePointers(bool allow) {
        allowUnsafePointers_ = allow;
    }

    /// @brief Query whether unsafe pointer syntax and raw runtime APIs are enabled.
    [[nodiscard]] bool allowUnsafePointers() const {
        return allowUnsafePointers_;
    }

    /// @brief Scan one source file for inline warning suppressions.
    /// @param fileId SourceManager file identifier for the scanned file.
    /// @param source Full source text for scanning @suppress directives.
    void addWarningSuppressions(uint32_t fileId, std::string_view source);

    /// @brief Analyze a module declaration.
    /// @param module The parsed module to analyze.
    /// @return True if analysis succeeded without errors.
    ///
    /// @details Performs complete semantic analysis on the module:
    /// 1. Registers built-in symbols
    /// 2. Processes binds
    /// 3. Analyzes all declarations
    /// 4. Type-checks all bodies
    ///
    /// Even on errors, populates as much type information as possible.
    bool analyze(ModuleDecl &module);

    /// @brief Get the resolved type for an expression.
    /// @param expr The expression to look up.
    /// @return The semantic type, or nullptr if not found/analyzed.
    ///
    /// @details Call after analyze() to get expression types.
    /// Returns nullptr for expressions that couldn't be typed.
    TypeRef typeOf(const Expr *expr) const;

    /// @brief Resolve an AST type node to a semantic type.
    /// @param node The AST type node.
    /// @return The resolved semantic type.
    ///
    /// @details Handles named types, generic types, optionals, and functions.
    /// May return unknown type for unresolved types.
    TypeRef resolveType(const TypeNode *node) const;

    /// @brief Check if analysis produced errors.
    /// @return True if at least one error was reported.
    bool hasError() const {
        return hasError_;
    }

    /// @brief Get the current module being analyzed.
    /// @return The module, or nullptr if not in analyze().
    ModuleDecl *currentModule() const {
        return currentModule_;
    }

    /// @brief Get the runtime function name for a call expression.
    /// @param expr The call expression to look up.
    /// @return The dotted name (e.g., "Zanna.Terminal.Say") or empty string.
    ///
    /// @details After analysis, call expressions that invoke runtime library
    /// functions have their resolved names stored. This is used during
    /// lowering to generate the correct runtime calls.
    std::string runtimeCallee(const CallExpr *expr) const {
        auto it = runtimeCallees_.find(expr);
        return it != runtimeCallees_.end() ? it->second : "";
    }

    /// @brief Get the auto-eval getter name for an identifier expression.
    /// @param expr Identifier-like expression recorded during analysis.
    /// @return Lowered getter symbol, or an empty string when auto-evaluation is not required.
    std::string autoEvalGetter(const Expr *expr) const {
        auto it = autoEvalGetters_.find(expr);
        return it != autoEvalGetters_.end() ? it->second : "";
    }

    /// @brief Get the semantic name resolved for an identifier expression.
    /// @param expr Identifier expression to query.
    /// @return Qualified semantic name, or an empty string when none was recorded.
    std::string resolvedIdentifierName(const IdentExpr *expr) const {
        auto it = resolvedIdentNames_.find(expr);
        return it != resolvedIdentNames_.end() ? it->second : "";
    }

    /// @brief Get the mangled function name for a generic function call.
    /// @param expr The call expression to look up.
    /// @return The mangled name (e.g., "identity$Integer") or empty string.
    ///
    /// @details For generic function calls like `identity[Integer](100)`, this
    /// returns the mangled function name that the lowerer should use.
    std::string genericFunctionCallee(const CallExpr *expr) const {
        auto it = genericFunctionCallees_.find(expr);
        if (it == genericFunctionCallees_.end())
            return "";

        std::string mangledName = it->second;

        // If we're in a generic context, substitute type parameters in the mangled name
        // e.g., "identity$T" becomes "identity$Integer" when T=Integer
        if (!typeParamStack_.empty()) {
            // Find the type argument part (after $)
            size_t dollarPos = mangledName.find('$');
            if (dollarPos != std::string::npos) {
                std::string baseName = mangledName.substr(0, dollarPos);
                std::string typeArgPart = mangledName.substr(dollarPos + 1);

                // Check if the type argument is a type parameter that should be substituted
                TypeRef substType = lookupTypeParam(typeArgPart);
                if (substType && !substType->name.empty()) {
                    mangledName = baseName + "$" + substType->name;
                }
            }
        }

        return mangledName;
    }

    /// @brief Get the resolved getter function name for a field expression.
    /// @param expr The field expression to look up.
    /// @return The getter name (e.g., "Zanna.Math.get_Pi" or "Counter.get_count") or empty
    /// string.
    ///
    /// @details For field expressions that resolve to runtime or user-defined property
    /// getters, this returns the lowered getter function name.
    std::string resolvedFieldGetter(const FieldExpr *expr) const {
        auto it = resolvedFieldGetters_.find(expr);
        return it != resolvedFieldGetters_.end() ? it->second : "";
    }

    /// @brief Get the resolved symbol name for a module-qualified field expression.
    /// @param expr Field expression to query.
    /// @return Qualified semantic symbol name, or an empty string when unresolved.
    std::string resolvedFieldSymbolName(const FieldExpr *expr) const {
        auto it = resolvedFieldSymbolNames_.find(expr);
        return it != resolvedFieldSymbolNames_.end() ? it->second : "";
    }

    /// @brief Get the resolved setter function name for a field assignment.
    /// @param expr Assigned field expression.
    /// @return Lowered setter symbol, or an empty string when the field is not property-backed.
    std::string resolvedFieldSetter(const FieldExpr *expr) const {
        auto it = resolvedFieldSetters_.find(expr);
        return it != resolvedFieldSetters_.end() ? it->second : "";
    }

    /// @brief Look up a property declaration for lowering property-backed access.
    /// @param ownerName Semantic receiver type where lookup begins.
    /// @param propertyName Property identifier.
    /// @param declaringOwner Optional destination for the type that declares the property.
    /// @return Property declaration, or nullptr when no visible property matches.
    const PropertyDecl *propertyDeclForLowering(const std::string &ownerName,
                                                const std::string &propertyName,
                                                std::string *declaringOwner = nullptr) const;

    /// @brief Get the resolved direct-call target for a user-defined function call.
    /// @param expr Call expression to query.
    /// @return Lowered function symbol, or an empty string when no direct target was selected.
    std::string resolvedFunctionCallee(const CallExpr *expr) const {
        auto it = resolvedFunctionCallees_.find(expr);
        return it != resolvedFunctionCallees_.end() ? it->second : "";
    }

    /// @brief Get the selected user-defined function declaration for a call.
    /// @param expr Call expression to query.
    /// @return Resolved declaration, or nullptr when the call is not a direct function call.
    FunctionDecl *resolvedFunctionDecl(const CallExpr *expr) const {
        auto it = resolvedFunctionDecls_.find(expr);
        return it != resolvedFunctionDecls_.end() ? it->second : nullptr;
    }

    /// @brief Get the resolved method declaration for a call site.
    /// @param expr Call expression to query.
    /// @return Selected method declaration, or nullptr when no method was resolved.
    MethodDecl *resolvedMethodDecl(const CallExpr *expr) const {
        auto it = resolvedMethodDecls_.find(expr);
        return it != resolvedMethodDecls_.end() ? it->second : nullptr;
    }

    /// @brief Get the concrete function type for a generic method call site.
    /// @param expr Generic method call expression.
    /// @return Instantiated callable type, or nullptr when the call is not generic.
    TypeRef genericMethodConcreteType(const CallExpr *expr) const {
        auto it = genericMethodConcreteTypes_.find(expr);
        return it != genericMethodConcreteTypes_.end() ? it->second : nullptr;
    }

    /// @brief Get the erased function type used by the lowered generic method body.
    /// @param expr Generic method call expression.
    /// @return Erased callable type, or nullptr when no erased body is involved.
    TypeRef genericMethodErasedType(const CallExpr *expr) const {
        auto it = genericMethodErasedTypes_.find(expr);
        return it != genericMethodErasedTypes_.end() ? it->second : nullptr;
    }

    /// @brief Get the owner type of a resolved method call.
    /// @param expr Method call expression.
    /// @return Semantic declaring-owner name, or an empty string when unresolved.
    std::string resolvedMethodOwnerType(const CallExpr *expr) const {
        auto it = resolvedMethodOwnerTypes_.find(expr);
        return it != resolvedMethodOwnerTypes_.end() ? it->second : "";
    }

    /// @brief Get the dispatch slot key of a resolved method call.
    /// @param expr Method call expression.
    /// @return Dispatch key, or an empty string for non-dispatched calls.
    std::string resolvedMethodSlotKey(const CallExpr *expr) const {
        auto it = resolvedMethodSlotKeys_.find(expr);
        return it != resolvedMethodSlotKeys_.end() ? it->second : "";
    }

    /// @brief Get the lowered symbol name for a method declaration.
    /// @param decl Method declaration to query.
    /// @return Lowered symbol, or an empty string before registration.
    std::string loweredMethodName(const MethodDecl *decl) const {
        auto it = loweredMethodNames_.find(decl);
        return it != loweredMethodNames_.end() ? it->second : "";
    }

    /// @brief Get the lowered symbol name for a method declaration in a specific owner type.
    /// @param ownerType Concrete semantic owner.
    /// @param decl Method declaration to query.
    /// @return Owner-specific lowered symbol, falling back to declaration-only metadata.
    std::string loweredMethodName(const std::string &ownerType, const MethodDecl *decl) const {
        auto it = ownerLoweredMethodNames_.find(MethodInstanceKey{ownerType, decl});
        if (it != ownerLoweredMethodNames_.end())
            return it->second;
        return loweredMethodName(decl);
    }

    /// @brief Get the dispatch slot key for a method declaration.
    /// @param decl Method declaration to query.
    /// @return Dispatch key, or an empty string before signature registration.
    std::string methodSlotKey(const MethodDecl *decl) const {
        auto it = methodDispatchKeys_.find(decl);
        return it != methodDispatchKeys_.end() ? it->second : "";
    }

    /// @brief Get the dispatch slot key for a method declaration in a specific owner type.
    /// @param ownerType Concrete semantic owner.
    /// @param decl Method declaration to query.
    /// @return Owner-specific dispatch key, falling back to declaration-only metadata.
    std::string methodSlotKey(const std::string &ownerType, const MethodDecl *decl) const {
        auto it = ownerMethodDispatchKeys_.find(MethodInstanceKey{ownerType, decl});
        if (it != ownerMethodDispatchKeys_.end())
            return it->second;
        return methodSlotKey(decl);
    }

    /// @brief Look up the cached signature key for a method in a specific owner type.
    /// @details Returns "" on a cache miss (key not yet computed). Callers that
    ///          need a guaranteed key should fall back to the computing overload
    ///          @ref methodSignatureKey(const MethodDecl&). Named distinctly from
    ///          the computing overloads so the lookup-vs-compute intent is clear.
    /// @param ownerType Concrete semantic owner.
    /// @param decl Method declaration to query.
    /// @return Cached semantic signature key, or an empty string on a cache miss.
    std::string cachedMethodSignatureKey(const std::string &ownerType, const MethodDecl *decl) const {
        auto it = ownerMethodSignatureKeys_.find(MethodInstanceKey{ownerType, decl});
        if (it != ownerMethodSignatureKeys_.end())
            return it->second;
        auto fallback = methodSignatureKeys_.find(decl);
        return fallback != methodSignatureKeys_.end() ? fallback->second : "";
    }

    /// @brief Get the lowered symbol name for a function declaration.
    /// @param decl Function declaration to query.
    /// @return Lowered symbol, or an empty string before registration.
    std::string loweredFunctionName(const FunctionDecl *decl) const {
        auto it = loweredFunctionNames_.find(decl);
        return it != loweredFunctionNames_.end() ? it->second : "";
    }

    /// @brief Get the resolved init overload for a new-expression.
    /// @param expr Allocation expression to query.
    /// @return Selected initializer declaration, or nullptr when field initialization is used.
    MethodDecl *resolvedInitDecl(const NewExpr *expr) const {
        auto it = resolvedInitDecls_.find(expr);
        return it != resolvedInitDecls_.end() ? it->second : nullptr;
    }

    /// @brief Get the owner type of a resolved init overload for a new-expression.
    /// @param expr Allocation expression to query.
    /// @return Semantic initializer owner, or an empty string when no overload was selected.
    std::string resolvedInitOwnerType(const NewExpr *expr) const {
        auto it = resolvedInitOwnerTypes_.find(expr);
        return it != resolvedInitOwnerTypes_.end() ? it->second : "";
    }

    /// @brief Get the resolved init overload for a constructor-style type call.
    /// @param expr Type call expression to query.
    /// @return Selected initializer declaration, or nullptr when unresolved.
    MethodDecl *resolvedTypeCallInitDecl(const CallExpr *expr) const {
        auto it = resolvedTypeCallInitDecls_.find(expr);
        return it != resolvedTypeCallInitDecls_.end() ? it->second : nullptr;
    }

    /// @brief Get the owner type of a resolved constructor-style init overload.
    /// @param expr Type call expression to query.
    /// @return Semantic initializer owner, or an empty string when unresolved.
    std::string resolvedTypeCallInitOwnerType(const CallExpr *expr) const {
        auto it = resolvedTypeCallInitOwnerTypes_.find(expr);
        return it != resolvedTypeCallInitOwnerTypes_.end() ? it->second : "";
    }

    /// @brief Get the bound argument layout for a resolved call expression.
    /// @param expr Call expression to query.
    /// @return Stable pointer to the stored binding, or nullptr when no binding was recorded.
    const CallArgBinding *callArgBinding(const CallExpr *expr) const {
        auto it = callArgBindings_.find(expr);
        return it != callArgBindings_.end() ? &it->second : nullptr;
    }

    /// @brief Get the bound argument layout for a resolved new-expression.
    /// @param expr Allocation expression to query.
    /// @return Stable pointer to the stored binding, or nullptr when no binding was recorded.
    const CallArgBinding *newArgBinding(const NewExpr *expr) const {
        auto it = newArgBindings_.find(expr);
        return it != newArgBindings_.end() ? &it->second : nullptr;
    }

    /// @brief Normalize runtime container handles to their source-language surface types.
    /// @param type Runtime-derived semantic type, possibly optional.
    /// @return Normalized list, map, or set surface type; the original type otherwise.
    TypeRef normalizeRuntimeSurfaceType(TypeRef type) const {
        if (!type)
            return nullptr;
        if (type->kind == TypeKindSem::Optional) {
            TypeRef inner = normalizeRuntimeSurfaceType(type->innerType());
            return inner ? types::optional(inner) : type;
        }
        if (type->kind != TypeKindSem::Ptr || type->name.empty())
            return type;
        if (type->name == "Zanna.Collections.List")
            return types::list(types::unknown());
        if (type->name == "Zanna.Collections.Map")
            return types::map(types::string(), types::unknown());
        if (type->name == "Zanna.Collections.Set")
            return types::set(types::unknown());
        return type;
    }

    /// @brief Look up and normalize the return type of a function by name.
    /// @param name Function name such as `Zanna.Random.NextInt` or `MyLib.helper`.
    /// @return Normalized return type, or nullptr when no function symbol exists.
    /// @details Works for runtime externs and user-defined functions; non-function legacy
    ///          symbols are normalized directly.
    TypeRef functionReturnType(const std::string &name) {
        Symbol *sym = lookupSymbol(name);
        if (!sym || sym->kind != Symbol::Kind::Function)
            return nullptr;
        TypeRef t = sym->type;
        // When registered with param types, sym->type is a full function type.
        // Unwrap it to return the actual return type so callers can decide
        // between emitCall (void) and emitCallRet (non-void).
        if (t && t->kind == TypeKindSem::Function)
            return normalizeRuntimeSurfaceType(t->returnType());
        return normalizeRuntimeSurfaceType(t);
    }

    /// @brief Find an extern (runtime) function by name.
    /// @param name The function name (e.g., "Zanna.GUI.App.get_ShouldClose").
    /// @return The symbol if found and is extern, nullptr otherwise.
    ///
    /// @details Used by the lowerer to resolve runtime property getters.
    Symbol *findExternFunction(const std::string &name) {
        Symbol *sym = lookupSymbol(name);
        return (sym && sym->isExtern) ? sym : nullptr;
    }

    /// @brief Look up the type of a variable by name.
    /// @param name The variable name.
    /// @return The variable's type, or nullptr if not found.
    TypeRef lookupVarType(const std::string &name);

    /// @brief Look up a flow-sensitive narrowed type by narrowing key.
    /// @param key Identifier or dotted member path used by null-check analysis.
    /// @return The narrowed type, or nullptr when no active narrowing exists.
    TypeRef lookupNarrowedType(const std::string &key) const;

    /// @brief Find the declaring owner of a field visible from a type.
    /// @param typeName The fully qualified type name (may be mangled for generics).
    /// @param fieldName The field name to resolve.
    /// @return The declaring type name, or std::nullopt if no visible field exists.
    /// @details Class lookups walk base classes without copying inherited field metadata into
    ///          the child owner. Struct lookups only check the struct itself.
    std::optional<std::string> findFieldOwner(const std::string &typeName,
                                              const std::string &fieldName) const;

    /// @brief Get the type of a field for a given type.
    /// @param typeName The fully qualified type name (may be mangled for generics).
    /// @param fieldName The field name.
    /// @return The field's type, or nullptr if not found.
    /// @details Inherited class fields are resolved through @ref findFieldOwner so metadata stays
    ///          associated with the declaring class.
    TypeRef getFieldType(const std::string &typeName, const std::string &fieldName) const;

    /// @brief A resolved field: its declaring owner and its type.
    struct FieldResolution {
        std::string owner; ///< Declaring type name.
        TypeRef type;      ///< Field type.
    };

    /// @brief Resolve a field to its declaring owner and type in a single walk.
    /// @details Fast-paths the common non-inherited case with no allocation and a
    ///          single map probe; only allocates the cycle-detection set when it
    ///          must walk base classes. @ref findFieldOwner and @ref getFieldType
    ///          are thin wrappers so neither rebuilds the key or re-probes the map.
    std::optional<FieldResolution> resolveFieldEntry(const std::string &typeName,
                                                     const std::string &fieldName) const;

    /// @brief Look up method type from the cached method types.
    /// @param typeName The fully qualified type name (may be mangled for generics).
    /// @param methodName The method name.
    /// @return The method's function type, or nullptr if not found.
    TypeRef getMethodType(const std::string &typeName, const std::string &methodName) const {
        std::string key = typeName + "." + methodName;
        auto it = methodTypes_.find(key);
        return it != methodTypes_.end() ? it->second : nullptr;
    }

    /// @brief Look up a method type by declaration.
    /// @param decl Method declaration to query.
    /// @return Cached function type, or nullptr when unregistered.
    TypeRef getMethodType(const MethodDecl *decl) const {
        auto it = methodDeclTypes_.find(decl);
        return it != methodDeclTypes_.end() ? it->second : nullptr;
    }

    /// @brief Look up a method type for a specific owner type and declaration.
    /// @param ownerType Concrete semantic owner.
    /// @param decl Method declaration to query.
    /// @return Owner-specific callable type, falling back to declaration-only metadata.
    TypeRef getMethodType(const std::string &ownerType, const MethodDecl *decl) const {
        auto it = ownerMethodTypes_.find(MethodInstanceKey{ownerType, decl});
        if (it != ownerMethodTypes_.end())
            return it->second;
        return getMethodType(decl);
    }

    /// @brief Look up a function type by declaration.
    /// @param decl Function declaration to query.
    /// @return Cached callable type, or nullptr when unregistered.
    TypeRef getFunctionType(const FunctionDecl *decl) const {
        auto it = functionDeclTypes_.find(decl);
        return it != functionDeclTypes_.end() ? it->second : nullptr;
    }

    /// @brief Look up a registered class declaration by semantic type name.
    /// @param typeName Concrete or instantiated semantic class name.
    /// @return Registered class declaration or generic origin, or nullptr.
    ClassDecl *findClassDecl(const std::string &typeName) const {
        return lookupClassDeclForType(typeName);
    }

    /// @brief Look up a registered struct declaration by semantic type name.
    /// @param typeName Concrete or instantiated semantic struct name.
    /// @return Registered struct declaration or generic origin, or nullptr.
    StructDecl *findStructDecl(const std::string &typeName) const {
        return lookupStructDeclForType(typeName);
    }

    /// @brief Get the original generic declaration for an instantiated type.
    /// @param mangledName The mangled type name (e.g., "Box$Integer").
    /// @return The original declaration, or nullptr if not a generic instantiation.
    Decl *getGenericDeclForInstantiation(const std::string &mangledName) const {
        // Extract base name from mangled name (before '$')
        size_t dollarPos = mangledName.find('$');
        if (dollarPos == std::string::npos)
            return nullptr;
        std::string baseName = mangledName.substr(0, dollarPos);
        auto it = genericTypeDecls_.find(baseName);
        return it != genericTypeDecls_.end() ? it->second : nullptr;
    }

    /// @brief Check if a type name is an instantiated generic.
    /// @param typeName The type name to check.
    /// @return True if this is an instantiated generic type.
    bool isInstantiatedGeneric(const std::string &typeName) const {
        return typeName.find('$') != std::string::npos;
    }

    /// @brief Push substitution context for a mangled generic instantiation.
    /// @param mangledName The mangled name (e.g., "Container$Integer").
    /// @return True if context was pushed, false if not a generic instantiation.
    ///
    /// @details Extracts the base name and type arguments from the mangled name,
    /// looks up the generic declaration, and pushes the substitution context.
    /// Must be balanced with popTypeParams().
    bool pushSubstitutionContext(const std::string &mangledName);

    /// @brief Pop the current type parameter substitution scope.
    ///
    /// @details Called when leaving a generic context. Restores the previous
    /// substitution scope (or empty if this was the only one).
    void popTypeParams();

    /// @brief Resolve a simple type name to a semantic type.
    /// @param name The type name (e.g., "Integer", "MyClass").
    /// @param useLoc Source location of the type use for bind/export visibility checks.
    /// @return The semantic type, or unknown if not found.
    TypeRef resolveNamedType(const std::string &name, SourceLoc useLoc = {}) const;

  private:
    //=========================================================================
    /// @name Declaration Analysis
    /// @brief Methods for analyzing declarations.
    /// @{
    //=========================================================================

    /// @brief Analyze a bind declaration.
    /// @param decl The bind declaration.
    ///
    /// @details Brings runtime functions into scope based on the bind path.
    /// For example, `bind Zanna.Terminal as Term;` makes Term.Say, Term.Ask, etc. available.
    void analyzeBind(BindDecl &decl);

    /// @brief Analyze a namespace bind declaration.
    /// @param decl The bind declaration (must have isNamespaceBind=true).
    ///
    /// @details Processes namespace binds like `bind Zanna.Terminal;`:
    /// - Full bind: exposes all symbols from the namespace
    /// - Alias bind: creates a module symbol for qualified access
    /// - Selective bind: exposes only the selected symbols
    void analyzeNamespaceBind(BindDecl &decl);

    /// @brief Rebuild exported symbol maps for all bound file modules.
    void buildBoundFileExports(const std::vector<BindDecl> &binds,
                               const std::vector<DeclPtr> &decls);

    /// @brief Collect exported top-level symbols for a specific source file.
    void collectExportedSymbolsForFile(uint32_t fileId,
                                       const std::vector<DeclPtr> &decls,
                                       std::unordered_map<std::string, Symbol> &out) const;

    /// @brief Find exports for a file module visible at a source location.
    /// @param moduleName The visible module root, such as an alias or file stem.
    /// @param useLoc Location of the use whose file-local imports should be consulted.
    /// @return Export map for the module, or nullptr when no matching file module is visible.
    ///
    /// @details File binds are scoped to the importing file even though imported
    /// declarations are flattened into the compilation unit. This helper uses
    /// @p useLoc to prefer the imports declared by that file, then falls back to
    /// globally unique module exports for older call sites without source context.
    const std::unordered_map<std::string, Symbol> *findModuleExports(const std::string &moduleName,
                                                                     SourceLoc useLoc = {}) const;

    /// @brief Return true if a file-module root is visible at a source location.
    /// @param moduleName The visible module root to test.
    /// @param useLoc Location whose file-local imports should be consulted.
    bool hasModuleExports(const std::string &moduleName, SourceLoc useLoc = {}) const;

    /// @brief Derive the primary visible module name for a file bind.
    /// @details Explicit aliases take precedence, then the bound file's
    /// declared module name, then the bind path stem.
    std::string fileBindModuleName(const BindDecl &decl) const;

    /// @brief Derive the module qualifier implied by a file bind path.
    /// @details Strips directory components and the `.zia` extension from
    /// @p decl.path. Used as a compatibility alias for unaliased file binds,
    /// so `bind "../model/player";` can still qualify symbols as `player.X`
    /// even when the target file declares `module player;` or a different
    /// canonical module name.
    std::string fileBindPathStem(const BindDecl &decl) const;

    /// @brief Return every module qualifier made visible by a file bind.
    /// @details Explicit aliases produce only the alias. Unaliased binds
    /// produce the primary module name plus the path stem when they differ,
    /// preserving both declared-module and legacy path-stem qualification.
    std::vector<std::string> fileBindVisibleModuleNames(const BindDecl &decl) const;

    /// @brief Prepare module-qualified semantic names for colliding top-level declarations.
    void prepareModuleScopedTypeNames(const ModuleDecl &module);

    /// @brief Pre-register a type declaration under a semantic name.
    bool registerTypeDeclarationSymbol(Decl &decl, const std::string &semanticName);

    /// @brief Register an unresolved type alias placeholder for later fixed-point resolution.
    void registerTypeAliasPlaceholder(TypeAliasDecl &decl, const std::string &semanticName);

    /// @brief Resolve type aliases after all names in the declaration group are registered.
    void resolvePendingTypeAliases(
        const std::vector<std::pair<TypeAliasDecl *, std::string>> &aliases);

    /// @brief Register declared class inheritance and interface implementation relationships.
    void registerNominalTypeRelationships(std::vector<DeclPtr> &declarations);

    /// @brief Return the declared module identity for a source file.
    std::string moduleNameForFile(uint32_t fileId) const;

    /// @brief Return a declaration's semantic name, accounting for scoped collisions.
    std::string semanticNameForDecl(const Decl &decl, const std::string &name) const;

    /// @brief Resolve a file-local top-level declaration spelling to its semantic name.
    std::string fileScopedDeclName(uint32_t fileId, const std::string &name) const;

    /// @brief Resolve a file-local top-level type spelling to its semantic name.
    std::string fileScopedTypeName(uint32_t fileId, const std::string &name) const;

    /// @brief Check whether a symbol may be referenced from the given location.
    bool canAccessSymbol(const Symbol &sym,
                         SourceLoc useLoc,
                         const std::string &name,
                         bool viaQualifiedModule) const;

    /// @brief Emit an access-control diagnostic for an inaccessible symbol.
    void reportInaccessibleSymbol(SourceLoc useLoc,
                                  const std::string &name,
                                  const Symbol &sym,
                                  bool viaQualifiedModule);

    /// @brief Look up a symbol and enforce file-bind/export visibility.
    Symbol *lookupAccessibleSymbol(const std::string &name,
                                   SourceLoc useLoc,
                                   bool viaQualifiedModule = false);

    /// @brief Check if a namespace path refers to a valid runtime namespace.
    /// @param ns The namespace path (e.g., "Zanna.Terminal").
    /// @param loc Source location of the bind used for conflict diagnostics.
    /// @return True if the namespace exists in the runtime registry.
    bool isValidRuntimeNamespace(const std::string &ns);

    /// @brief Import all symbols from a runtime namespace into scope.
    /// @param ns The namespace path (e.g., "Zanna.Terminal").
    ///
    /// @details Walks through all registered extern symbols and imports
    /// those that match the namespace prefix. Nested namespaces are not
    /// imported (e.g., binding Zanna.Graphics doesn't import Zanna.Graphics.Color.Red).
    void importNamespaceSymbols(const std::string &ns, SourceLoc loc = {});

    /// @brief Analyze a global variable declaration.
    /// @param decl The global variable declaration.
    ///
    /// @details Type-checks the initializer and registers the variable
    /// in the global scope.
    void analyzeGlobalVarDecl(GlobalVarDecl &decl);

    /// @brief Analyze a struct type declaration.
    /// @param decl The struct type declaration.
    ///
    /// @details Registers the type and analyzes all members.
    void analyzeStructDecl(StructDecl &decl);

    /// @brief Register type member signatures for cross-module resolution.
    /// @tparam T Decl type (ClassDecl, StructDecl, or InterfaceDecl)
    /// @param decl The type declaration.
    /// @param includeFields Whether to register field types (false for interfaces).
    template <typename T> void registerTypeMembers(T &decl, bool includeFields = true);

    /// @brief Register class member signatures for cross-module resolution.
    void registerClassMembers(ClassDecl &decl);

    /// @brief Register struct type member signatures for cross-module resolution.
    void registerStructMembers(StructDecl &decl);

    /// @brief Register interface member signatures for cross-module resolution.
    void registerInterfaceMembers(InterfaceDecl &decl);

    /// @brief Pre-pass: Eagerly resolve types of final constants from literal initializers.
    /// @param declarations The declaration list to process.
    /// @details Scans for GlobalVarDecl with isFinal and literal initializer, updating
    ///          the symbol's type from unknown() to the concrete type. This allows
    ///          forward references to final constants in class/function bodies.
    void registerFinalConstantTypes(std::vector<DeclPtr> &declarations);

    /// @brief Pass 2: Register member signatures (fields, methods) for type declarations.
    /// @param declarations The declaration list to process.
    void registerMemberSignatures(std::vector<DeclPtr> &declarations);

    /// @brief Pass 3: Analyze declaration bodies (functions, types, globals).
    /// @param declarations The declaration list to process.
    void analyzeDeclarationBodies(std::vector<DeclPtr> &declarations);

    /// @brief Analyze an class type declaration.
    /// @param decl The class type declaration.
    ///
    /// @details Registers the type, resolves inheritance, and analyzes members.
    void analyzeClassDecl(ClassDecl &decl);

    /// @brief Analyze an interface declaration.
    /// @param decl The interface declaration.
    ///
    /// @details Registers the interface type and its method signatures.
    void analyzeInterfaceDecl(InterfaceDecl &decl);

    /// @brief Analyze an enum declaration.
    /// @param decl The enum declaration.
    ///
    /// @details Validates variant names are unique, resolves explicit values,
    /// auto-increments gaps, and registers each variant in fieldTypes_.
    void analyzeEnumDecl(EnumDecl &decl);

    /// @brief Validate that a type correctly implements all declared interfaces.
    /// @param typeName The name of the implementing type.
    /// @param loc The source location for error reporting.
    /// @param interfaces List of interface names the type claims to implement.
    ///
    /// @details Checks that all interface methods are implemented with matching
    /// signatures and public visibility. Registers successful implementations.
    void validateInterfaceImplementations(const std::string &typeName,
                                          const SourceLoc &loc,
                                          const std::vector<std::string> &interfaces);

    /// @brief Analyze a namespace declaration.
    /// @param decl The namespace declaration.
    ///
    /// @details Processes all declarations within the namespace, prefixing
    /// their names with the namespace path. Supports nested namespaces.
    void analyzeNamespaceDecl(NamespaceDecl &decl);

    /// @brief Compute the qualified name for a declaration.
    /// @param name The unqualified name.
    /// @return The fully qualified name including namespace prefix.
    ///
    /// @details If currently inside a namespace, prepends the namespace path.
    /// Example: inside "MyLib", name "Parser" becomes "MyLib.Parser".
    std::string qualifyName(const std::string &name) const;

    /// @brief Analyze a function declaration.
    /// @param decl The function declaration.
    ///
    /// @details Analyzes the signature and body, validating return types.
    void analyzeFunctionDecl(FunctionDecl &decl);

    /// @brief Analyze a field declaration within a type.
    /// @param decl The field declaration.
    /// @param ownerType The type containing this field.
    void analyzeFieldDecl(FieldDecl &decl, TypeRef ownerType);

    /// @brief Analyze a property declaration within a type.
    void analyzePropertyDecl(PropertyDecl &decl, TypeRef ownerType);

    /// @brief Analyze a destructor declaration within an class type.
    void analyzeDestructorDecl(DestructorDecl &decl, TypeRef ownerType);

    /// @brief Analyze a method declaration within a type.
    /// @param decl The method declaration.
    /// @param ownerType The type containing this method.
    void analyzeMethodDecl(MethodDecl &decl, TypeRef ownerType);

    /// @brief Find a property declaration by owner type and property name.
    const PropertyDecl *findPropertyDecl(const std::string &ownerName,
                                         const std::string &propertyName) const;

    /// @brief Build a semantic function type for a function declaration.
    TypeRef functionTypeForDecl(const FunctionDecl &decl) const;

    /// @brief Build a semantic function type for a method declaration.
    TypeRef methodTypeForDecl(const MethodDecl &decl) const;

    /// @brief Build the exact overload signature key for a function declaration.
    std::string functionSignatureKey(const FunctionDecl &decl) const;

    /// @brief Build the exact overload signature key for a method declaration.
    std::string methodSignatureKey(const MethodDecl &decl) const;

    /// @brief Build the exact overload signature key for a resolved method type.
    std::string methodSignatureKey(const MethodDecl &decl, TypeRef methodType) const;

    /// @brief Build the dispatch slot key for a method declaration.
    std::string methodDispatchKey(const MethodDecl &decl) const;

    /// @brief Build the dispatch slot key for a resolved method type.
    std::string methodDispatchKey(const MethodDecl &decl, TypeRef methodType) const;

    /// @brief Register a function overload and assign its lowered symbol name.
    bool registerFunctionOverload(const std::string &name,
                                  FunctionDecl *decl,
                                  TypeRef funcType,
                                  SourceLoc loc);

    /// @brief Register a method overload and assign its lowered symbol name.
    bool registerMethodOverload(const std::string &ownerType,
                                MethodDecl *decl,
                                TypeRef methodType,
                                SourceLoc loc);

    /// @brief Collect method overloads visible on a type.
    std::vector<MethodDecl *> collectMethodOverloads(const std::string &typeName,
                                                     const std::string &methodName,
                                                     bool includeInherited = true) const;

    /// @brief Resolve a function overload for a call site.
    FunctionDecl *resolveFunctionOverload(const std::string &name,
                                          const std::vector<TypeRef> &argTypes,
                                          SourceLoc loc,
                                          std::string *loweredName = nullptr,
                                          bool viaQualifiedModule = false);

    /// @brief Resolve a method overload for a call site.
    MethodDecl *resolveMethodOverload(const std::string &ownerType,
                                      const std::string &methodName,
                                      const std::vector<TypeRef> &argTypes,
                                      SourceLoc loc,
                                      std::string *resolvedOwnerType = nullptr,
                                      bool includeInherited = true);

    /// @brief Resolve a function overload using full call-site argument binding.
    FunctionDecl *resolveFunctionCallOverload(const std::string &name,
                                              CallExpr *expr,
                                              SourceLoc loc,
                                              std::string *loweredName = nullptr,
                                              CallArgBinding *bindingOut = nullptr,
                                              bool viaQualifiedModule = false);

    /// @brief Resolve a method overload using full call-site argument binding.
    MethodDecl *resolveMethodCallOverload(const std::string &ownerType,
                                          const std::string &methodName,
                                          CallExpr *expr,
                                          SourceLoc loc,
                                          std::string *resolvedOwnerType = nullptr,
                                          bool includeInherited = true,
                                          CallArgBinding *bindingOut = nullptr);

    /// @brief Find an inherited exact-signature method for override validation.
    MethodDecl *findInheritedExactMethod(const std::string &ownerType,
                                         const MethodDecl &decl) const;

    /// @brief Check whether a name has multiple user-defined overloads.
    bool hasOverloadedFunctionName(const std::string &name) const;

    /// @brief Find the ClassDecl for a type name, or nullptr if @p typeName
    ///        is not a known user class.
    ClassDecl *lookupClassDeclForType(const std::string &typeName) const;
    /// @brief Find the StructDecl for a type name, or nullptr if @p typeName
    ///        is not a known user struct.
    StructDecl *lookupStructDeclForType(const std::string &typeName) const;

    /// @brief Initialize all runtime function type mappings.
    /// @details Registers all Zanna.* namespace functions as extern symbols
    /// in the global scope. Called once during Sema construction.
    void initRuntimeFunctions();

    /// @brief Register an external (runtime) function.
    /// @param name The fully qualified function name (e.g., "Zanna.Terminal.Say").
    /// @param returnType The function's return type.
    /// @param paramTypes Optional vector of parameter types.
    ///
    /// @details Creates a Symbol with isExtern=true and registers it in scope.
    /// Used for runtime library functions that have no AST declaration.
    /// When paramTypes are provided, the symbol's type is a function type.
    void defineExternFunction(const std::string &name,
                              TypeRef returnType,
                              const std::vector<TypeRef> &paramTypes = {},
                              const std::vector<std::string> &paramNames = {},
                              std::optional<RuntimePointerSafety> pointerSafety = std::nullopt,
                              const std::string &documentation = {});

    /// @}
    //=========================================================================
    /// @name Statement Analysis
    /// @brief Methods for analyzing statements.
    /// @{
    //=========================================================================

    /// @brief Analyze any statement (dispatches to specific methods).
    /// @param stmt The statement to analyze.
    void analyzeStmt(Stmt *stmt);

    /// @brief Analyze a block statement.
    /// @param stmt The block statement.
    ///
    /// @details Creates a new scope and analyzes all statements within.
    void analyzeBlockStmt(BlockStmt *stmt);

    /// @brief Analyze a variable declaration statement.
    /// @param stmt The variable statement.
    void analyzeVarStmt(VarStmt *stmt);

    /// @brief Analyze an if statement.
    /// @param stmt The if statement.
    void analyzeIfStmt(IfStmt *stmt);

    /// @brief Analyze a while statement.
    /// @param stmt The while statement.
    void analyzeWhileStmt(WhileStmt *stmt);

    /// @brief Analyze a C-style for statement.
    /// @param stmt The for statement.
    void analyzeForStmt(ForStmt *stmt);

    /// @brief Analyze a for-in statement.
    /// @param stmt The for-in statement.
    void analyzeForInStmt(ForInStmt *stmt);

    /// @brief Analyze a return statement.
    /// @param stmt The return statement.
    ///
    /// @details Validates that the return struct type matches the expected
    /// return type of the enclosing function.
    void analyzeReturnStmt(ReturnStmt *stmt);

    /// @brief Analyze a guard statement.
    /// @param stmt The guard statement.
    void analyzeGuardStmt(GuardStmt *stmt);

    /// @brief Analyze a match statement.
    /// @param stmt The match statement.
    void analyzeMatchStmt(MatchStmt *stmt);

    /// @brief Track coverage details for match exhaustiveness checks.
    struct MatchCoverage {
        bool hasIrrefutable = false;
        bool coversNull = false;
        bool coversSome = false;
        bool coversResultOk = false;
        bool coversResultErr = false;
        std::set<int64_t> coveredIntegers;
        std::set<bool> coveredBooleans;
        std::set<std::string> coveredEnumVariants;
    };

    /// @brief Analyze a match pattern and collect bindings/coverage.
    bool analyzeMatchPattern(const MatchArm::Pattern &pattern,
                             TypeRef scrutineeType,
                             MatchCoverage &coverage,
                             std::unordered_map<std::string, TypeRef> &bindings);

    /// @brief Compute a common type for two branches.
    TypeRef commonType(TypeRef lhs, TypeRef rhs);

    /// @brief Determine whether a statement always exits the current scope.
    bool stmtAlwaysExits(Stmt *stmt);

    /// @}
    //=========================================================================
    /// @name Expression Analysis
    /// @brief Methods for analyzing expressions.
    /// @details Each method analyzes a specific expression type and returns
    /// the inferred type. The type is also stored in exprTypes_.
    /// @{
    //=========================================================================

    /// @brief Analyze any expression (dispatches to specific methods).
    /// @param expr The expression to analyze.
    /// @return The inferred type, or unknown type on error.
    TypeRef analyzeExpr(Expr *expr);

    /// @brief Analyze an integer literal.
    /// @return types::integer()
    TypeRef analyzeIntLiteral(IntLiteralExpr *expr);

    /// @brief Analyze a floating-point literal.
    /// @return types::number()
    TypeRef analyzeNumberLiteral(NumberLiteralExpr *expr);

    /// @brief Analyze a string literal.
    /// @return types::string()
    TypeRef analyzeStringLiteral(StringLiteralExpr *expr);

    /// @brief Analyze a boolean literal.
    /// @return types::boolean()
    TypeRef analyzeBoolLiteral(BoolLiteralExpr *expr);

    /// @brief Analyze a null literal.
    /// @return types::optional(types::unknown()) - inferred from context
    TypeRef analyzeNullLiteral(NullLiteralExpr *expr);

    /// @brief Analyze a unit literal.
    /// @return types::unit()
    TypeRef analyzeUnitLiteral(UnitLiteralExpr *expr);

    /// @brief Analyze an identifier expression.
    /// @return The type of the referenced symbol.
    TypeRef analyzeIdent(IdentExpr *expr);

    /// @brief Analyze a self expression.
    /// @return The current self type.
    TypeRef analyzeSelf(SelfExpr *expr);

    /// @brief Analyze a binary expression.
    /// @return The result type based on operator and operands.
    TypeRef analyzeBinary(BinaryExpr *expr);

    /// @brief Type-check an arithmetic binary operator (`+ - * / %`).
    /// @details Also recognises `String + value` and `value + String` as
    ///          concatenation, and emits W010 (division-by-zero) for literal
    ///          zero divisors of `/` and `%`.
    TypeRef checkArithmeticBinary(BinaryExpr *expr, TypeRef leftType, TypeRef rightType);

    /// @brief Type-check a comparison binary operator
    ///        (`== != < <= > >=`). Emits W005 for float equality and W011 for
    ///        redundant boolean-literal comparisons.
    TypeRef checkComparisonBinary(BinaryExpr *expr, TypeRef leftType, TypeRef rightType);

    /// @brief Type-check a short-circuit logical operator (`and`, `or`).
    TypeRef checkLogicalBinary(BinaryExpr *expr, TypeRef leftType, TypeRef rightType);

    /// @brief Type-check a bitwise / shift operator
    ///        (`<< >> & | ^`). Emits W017 for `^` (XOR vs `**`) and W018 for
    ///        `&` (bitwise AND vs `+`-concatenation).
    TypeRef checkBitwiseBinary(BinaryExpr *expr, TypeRef leftType, TypeRef rightType);

    /// @brief Validate an assignment binary expression and record any setter
    ///        / final-field / narrowing book-keeping the lowerer needs.
    /// @return The assigned variable's type (or `unknown` on error).
    TypeRef recordBinaryAssignment(BinaryExpr *expr, TypeRef leftType, TypeRef rightType);

    /// @brief Analyze a unary expression.
    /// @return The result type based on operator and operand.
    TypeRef analyzeUnary(UnaryExpr *expr);

    /// @brief Analyze a ternary conditional expression.
    /// @return The common type of the branches.
    TypeRef analyzeTernary(TernaryExpr *expr);

    /// @brief Analyze an if-expression (`if cond { then } else { else }`).
    /// @return The common type of the branches.
    TypeRef analyzeIfExpr(IfExpr *expr);

    /// @brief Analyze a struct-literal expression (`TypeName { field = val, ... }`).
    /// @return The struct type named by the struct literal.
    TypeRef analyzeStructLiteral(StructLiteralExpr *expr);

    /// @brief Analyze a function/method call expression.
    /// @return The return type of the called function.
    TypeRef analyzeCall(CallExpr *expr);

    /// @brief Analyze a List functional combinator call (map/filter/reduce/…).
    /// @details Returns the combinator's result type, or std::nullopt if @p field
    /// is not a combinator or the receiver is not a List. Handled before generic
    /// argument analysis so the closure argument can be target-typed from the
    /// element type. @p baseType must be the already-analyzed receiver type.
    std::optional<TypeRef> analyzeListCombinatorCall(CallExpr *expr,
                                                     const std::string &field,
                                                     TypeRef baseType);

    /// @brief Bind an extern (runtime) call's argument types against the symbol's
    ///        parameter specs and record the binding on the call expression.
    /// @return true if binding succeeded or if @p sym is not an extern; false on
    ///         a binding error (a diagnostic will already have been emitted).
    bool bindExternCallOnCall(CallExpr *expr,
                              const std::string &calleeName,
                              Symbol *sym,
                              size_t skipLeadingParams = 0);

    /// @brief Try to bind a single-argument call to a "terminal text" runtime
    ///        function (i.e., one that auto-stringifies its argument).
    /// @param outType Receives the (possibly normalized) return type on success.
    /// @return true if the call was bound as a terminal-text call.
    bool tryBindTerminalTextCall(CallExpr *expr,
                                 const std::string &calleeName,
                                 Symbol *sym,
                                 TypeRef &outType);

    /// @brief Whether a dotted callee (e.g., `Foo.bar()`) should defer to the
    ///        qualified-name lookup path because its root names a module,
    ///        imported namespace, or alias rather than a value.
    bool shouldDeferDottedCalleeToQualifiedLookup(const CallExpr *expr) const;

    /// @brief Refine a recognized runtime call's declared return type using
    ///        the receiver / first-argument element type.
    /// @details For runtime collection accessors (`Seq.Get`, `Map.Values`, …)
    ///          the registry-declared return type is generic (opaque `Ptr` or
    ///          `Seq<unknown>`). This helper specialises that fallback to the
    ///          receiver's element / key / value type when known, so callers
    ///          see typed results instead of opaque pointers.
    /// @param expr        The call expression (used to look up arg/receiver types).
    /// @param calleeName  Canonical runtime name (`Zanna.Collections.Seq.Get`).
    /// @param fallback    Return type to use when no specialisation applies.
    TypeRef refineRuntimeCallReturnType(const CallExpr *expr,
                                        const std::string &calleeName,
                                        TypeRef fallback) const;

    /// @brief Validate call argument count and types against a function signature.
    /// @param expr The call expression (for argument values and locations).
    /// @param funcType The resolved function type with parameter types.
    /// @param calleeName Name of the function for error messages.
    void validateCallArgs(CallExpr *expr, TypeRef funcType, const std::string &calleeName);

    /// @brief Analyze an index expression.
    /// @return The element type of the collection.
    TypeRef analyzeIndex(IndexExpr *expr);

    /// @brief Analyze a field access expression.
    /// @return The type of the accessed field.
    TypeRef analyzeField(FieldExpr *expr);

    /// @brief Look up a static field of @p ownerName for @p expr, enforcing
    ///        visibility. Returns nullptr if @p ownerName has no such static
    ///        field; returns `unknown` if visibility blocks access.
    TypeRef resolveStaticField(FieldExpr *expr, const std::string &ownerName);

    /// @brief Resolve a field access whose base is a Module type
    ///        (e.g., `colors.initColors`, `Canvas.New`, `Zanna.Graphics.X`).
    /// @return The resolved member type, or `unknown` on error/not-found.
    TypeRef resolveModuleFieldAccess(FieldExpr *expr, TypeRef baseType);

    /// @brief Resolve a field/method/property access on a Class or Struct.
    /// @return The resolved member type, or `unknown` on error/not-found.
    TypeRef resolveClassStructFieldAccess(FieldExpr *expr, TypeRef baseType);

    /// @brief Resolve a paren-less member access on a runtime class (a `Zanna.`-
    ///        prefixed Ptr base): an RT_PROP getter, a method accessed without
    ///        call parentheses (diagnosed with a fix-it), or a genuine unknown
    ///        member (diagnosed like every other field-access branch). Mirrors
    ///        `resolveClassStructFieldAccess` so runtime-class access is a
    ///        complete, self-diagnosing resolver rather than a silent fall-through.
    /// @return The resolved property type, or `unknown` on error/not-found.
    TypeRef resolveRuntimeClassFieldAccess(FieldExpr *expr, TypeRef baseType);

    /// @brief Analyze an optional chain expression.
    /// @return An optional type wrapping the field type.
    TypeRef analyzeOptionalChain(OptionalChainExpr *expr);

    /// @brief Analyze a force-unwrap expression: expr!
    /// @return The inner (non-optional) type of the operand.
    TypeRef analyzeForceUnwrap(ForceUnwrapExpr *expr);

    /// @brief Analyze a null coalesce expression.
    /// @return The non-optional type of the result.
    TypeRef analyzeCoalesce(CoalesceExpr *expr);

    /// @brief Analyze a postfix try/propagate expression.
    /// @return The inner type of the optional operand.
    TypeRef analyzeTry(TryExpr *expr);

    /// @brief Analyze an is-expression (type check).
    /// @return types::boolean()
    TypeRef analyzeIs(IsExpr *expr);

    /// @brief Analyze an as-expression (type cast).
    /// @return The target type.
    TypeRef analyzeAs(AsExpr *expr);

    /// @brief Analyze a range expression.
    /// @return types::list(types::integer()) for iteration.
    TypeRef analyzeRange(RangeExpr *expr);

    /// @brief Analyze a match expression.
    /// @return The common type of all arm bodies.
    TypeRef analyzeMatchExpr(MatchExpr *expr);

    /// @brief Analyze a new expression (object creation).
    /// @return The class type being constructed.
    TypeRef analyzeNew(NewExpr *expr);

    /// @brief Analyze a lambda expression.
    /// @return A function type matching the lambda's signature.
    TypeRef analyzeLambda(LambdaExpr *expr);

    /// @brief Analyze a list literal expression.
    /// @return types::list(elementType)
    TypeRef analyzeListLiteral(ListLiteralExpr *expr);

    /// @brief Analyze a map literal expression.
    /// @return types::map(keyType, valueType)
    TypeRef analyzeMapLiteral(MapLiteralExpr *expr);

    /// @brief Analyze a set literal expression.
    /// @return types::set(elementType)
    TypeRef analyzeSetLiteral(SetLiteralExpr *expr);

    /// @brief Analyze a tuple literal expression.
    /// @return types::tuple(elementTypes)
    TypeRef analyzeTuple(TupleExpr *expr);

    /// @brief Analyze a tuple index access expression.
    /// @return The type of the element at the given index.
    TypeRef analyzeTupleIndex(TupleIndexExpr *expr);

    /// @brief Analyze a block expression (statements followed by optional value).
    /// @return The type of the block's value expression, or Void if none.
    TypeRef analyzeBlockExpr(BlockExpr *expr);

    /// @}
    //=========================================================================
    /// @name Type Resolution
    /// @brief Methods for resolving type annotations.
    /// @{
    //=========================================================================

    /// @brief Resolve a type node to a semantic type.
    /// @param node The AST type node.
    /// @return The resolved semantic type.
    ///
    /// @details Handles named, generic, optional, function, and tuple types.
    TypeRef resolveTypeNode(const TypeNode *node);

    /// @}
    //=========================================================================
    /// @name Scope Management
    /// @brief Methods for managing variable scopes.
    /// @{
    //=========================================================================

    /// @brief Push a new scope onto the scope stack.
    ///
    /// @details Creates a new scope with the current scope as parent.
    /// Must be balanced with popScope().
    void pushScope(SourceLoc startLoc = {});

    /// @brief Pop the current scope from the scope stack.
    ///
    /// @details Returns to the parent scope. All symbols in the popped
    /// scope become inaccessible.
    void popScope(SourceLoc endLoc = {});

    /// @brief Define a symbol in the current scope.
    /// @param name The symbol name.
    /// @param symbol The symbol information.
    /// @param locOverride Optional source location for symbols without a decl pointer
    ///        (local variables, parameters). If invalid, falls back to symbol.decl->loc.
    bool defineSymbol(const std::string &name, Symbol symbol, SourceLoc locOverride = {});

    /// @brief Look up a symbol by name.
    /// @param name The symbol name.
    /// @return Pointer to the symbol, or nullptr if not found.
    Symbol *lookupSymbol(const std::string &name);

    /// @brief Recover a variable's declared Optional[T] surface type when flow
    ///        narrowing currently exposes only T.
    /// @details Optional-sensitive operators like `?.`, `!`, and `match` on
    ///          `null` should preserve the declared optional surface even when
    ///          a local flow fact says the current value is non-null.
    TypeRef declaredOptionalSurfaceType(Expr *expr, TypeRef analyzedType);

    /// @brief Collect captured variables from a lambda body.
    /// @param expr The expression to scan for free variables.
    /// @param lambdaLocals Names local to the lambda (params).
    /// @param captures Output vector of captured variables.
    void collectCaptures(const Expr *expr,
                         const std::set<std::string> &lambdaLocals,
                         std::vector<CapturedVar> &captures);

    /// @brief Per-call state for the capture walker. Holds the stack of
    ///        local-binding scopes and the running set of already-captured
    ///        names so we don't push duplicates.
    struct CaptureContext {
        /// @brief Create capture-walker state for a single lambda analysis.
        /// @param alreadyCaptured Names captured before this context is created.
        /// @param scopes Local scopes that shadow outer variables.
        /// @param outCaptures Destination vector receiving discovered captures.
        CaptureContext(std::set<std::string> alreadyCaptured,
                       std::vector<std::set<std::string>> scopes,
                       std::vector<CapturedVar> &outCaptures)
            : captured(std::move(alreadyCaptured)), localScopes(std::move(scopes)),
              captures(outCaptures) {}

        std::set<std::string> captured;
        std::vector<std::set<std::string>> localScopes;
        std::vector<CapturedVar> &captures;
    };

    /// @brief If @p name is not shadowed by any local in @p ctx and resolves
    ///        to a Variable/Parameter symbol, append it to `ctx.captures`.
    void recordCapture(CaptureContext &ctx, const std::string &name);

    /// @brief Add any bindings introduced by @p pattern to the innermost
    ///        local scope of @p ctx (handles Tuple/Constructor/Or recursion).
    void collectPatternBindings(CaptureContext &ctx, const MatchArm::Pattern &pattern);

    /// @brief Walk @p stmt and append free-variable references to @p ctx.
    void collectStmtCaptures(CaptureContext &ctx, const Stmt *stmt);

    /// @brief Walk @p expr and append free-variable references to @p ctx.
    void collectExprCaptures(CaptureContext &ctx, const Expr *expr);

    /// @brief Push a new type narrowing scope.
    /// @details Called when entering a branch where types may be narrowed.
    void pushNarrowingScope();

    /// @brief Pop the current type narrowing scope.
    /// @details Called when leaving a branch with narrowed types.
    void popNarrowingScope();

    /// @brief Narrow a variable's type in the current scope.
    /// @param name The variable name.
    /// @param narrowedType The narrowed (non-optional) type.
    void narrowType(const std::string &name, TypeRef narrowedType);

    /// @brief Mark a variable as definitely initialized.
    /// @param name The variable name.
    void markInitialized(const std::string &name);

    /// @brief Check if a variable has been definitely initialized.
    /// @param name The variable name.
    /// @return True if the variable has been initialized.
    bool isInitialized(const std::string &name) const;

    /// @brief Build the definite-initialization key for the visible symbol.
    /// @details Initialization is flow-sensitive and lexical-scope-sensitive:
    ///          a block-local `x` must not make an outer or later `x` appear
    ///          initialized. The key therefore combines the owning scope id
    ///          with @p name for the nearest visible symbol. When recovery is
    ///          analyzing an unresolved name, the plain name is used as a
    ///          fallback so diagnostics can continue without crashing.
    /// @param name The surface symbol name being queried or marked.
    /// @return A stable key for the currently visible symbol.
    std::string initializedSymbolKey(const std::string &name) const;

    /// @brief Save the current initialization state (for branching analysis).
    /// @return A snapshot of the currently initialized variables.
    std::unordered_set<std::string> saveInitState() const;

    /// @brief Restore initialization state (keep only variables initialized in both).
    /// @param saved The saved state from before a branch.
    /// @param branchA State after analyzing the then-branch.
    /// @param branchB State after analyzing the else-branch.
    void intersectInitState(const std::unordered_set<std::string> &branchA,
                            const std::unordered_set<std::string> &branchB);

    /// @brief Try to extract a null check from a condition expression.
    /// @param cond The condition expression.
    /// @param varName Output: the variable being null-checked.
    /// @param isNotNull Output: true if checking != null, false if == null.
    /// @return True if the condition is a null check pattern.
    bool tryExtractNullCheck(Expr *cond,
                             std::string &varName,
                             bool &isNotNull,
                             TypeRef *checkedType = nullptr);

    /// @brief Build a stable key for expressions supported by flow narrowing.
    /// @param expr Expression to key, such as `x`, `self.field`, or `obj.field`.
    /// @return Empty string when the expression is not safe to narrow.
    std::string narrowingKeyForExpr(Expr *expr) const;

    /// @}

  public:
    //=========================================================================
    /// @name Generic Type Parameter Management
    /// @brief Methods for managing type parameter substitutions in generic contexts.
    /// @{
    //=========================================================================

    /// @brief Push a new type parameter substitution scope.
    /// @param substitutions Map from type parameter names to concrete types.
    ///
    /// @details Called when entering a generic context (e.g., instantiating
    /// a generic type or function). Must be balanced with popTypeParams().
    void pushTypeParams(const std::map<std::string, TypeRef> &substitutions);

    /// @brief Look up a type parameter in the current substitution context.
    /// @param name The type parameter name (e.g., "T").
    /// @return The substituted type if found, nullptr otherwise.
    ///
    /// @details Searches from innermost to outermost substitution scope.
    /// Returns nullptr if the name is not a type parameter in any active scope.
    TypeRef lookupTypeParam(const std::string &name) const;

    /// @brief Substitute type parameters in a type using the current substitution context.
    /// @param type The type to substitute.
    /// @return The type with type parameters replaced by their substituted values.
    TypeRef substituteTypeParams(TypeRef type) const;

    /// @brief Check if currently inside a generic context.
    /// @return True if there are active type parameter substitutions.
    bool inGenericContext() const {
        return !typeParamStack_.empty();
    }

    /// @brief Generate a mangled name for a generic type instantiation.
    /// @param base The base type name (e.g., "Box").
    /// @param args The type arguments (e.g., [Integer]).
    /// @return The mangled name (e.g., "Box$Integer").
    static std::string mangleGenericName(const std::string &base, const std::vector<TypeRef> &args);

    /// @brief Register a generic type declaration for later instantiation.
    /// @param name The type name (e.g., "Box").
    /// @param decl Pointer to the AST declaration.
    void registerGenericType(const std::string &name, Decl *decl);

    /// @brief Instantiate a generic type with concrete type arguments.
    /// @param name The base type name (e.g., "Box").
    /// @param args The concrete type arguments (e.g., [Integer]).
    /// @param loc Source location for error reporting.
    /// @return The instantiated type, or unknown on error.
    TypeRef instantiateGenericType(const std::string &name,
                                   const std::vector<TypeRef> &args,
                                   SourceLoc loc);

    /// @brief Get the generic parameters from a declaration.
    /// @param decl The declaration (StructDecl, ClassDecl, or InterfaceDecl).
    /// @return Vector of type parameter names.
    static std::vector<std::string> getGenericParams(const Decl *decl);

    /// @brief Get generic parameter constraints from a declaration.
    /// @param decl The declaration (StructDecl, ClassDecl, InterfaceDecl, or FunctionDecl).
    /// @return Vector of constraint names parallel to getGenericParams().
    static std::vector<std::string> getGenericParamConstraints(const Decl *decl);

    /// @brief Validate concrete type arguments against generic constraints.
    bool validateGenericConstraints(const std::vector<std::string> &params,
                                    const std::vector<std::string> &constraints,
                                    const std::vector<TypeRef> &args,
                                    SourceLoc loc,
                                    const std::string &subjectName);

    /// @brief Analyze a generic type body with current substitutions.
    /// @param decl The generic type declaration.
    /// @param mangledName The mangled name for the instantiation.
    /// @return The instantiated type.
    TypeRef analyzeGenericTypeBody(Decl *decl, const std::string &mangledName);

  public:
    //=========================================================================
    /// @name Generic Functions
    /// @brief Methods for generic function support.
    /// @{
    //=========================================================================

    /// @brief Register a generic function declaration for later instantiation.
    /// @param name The function name (e.g., "identity").
    /// @param decl Pointer to the AST declaration.
    void registerGenericFunction(const std::string &name, FunctionDecl *decl);

    /// @brief Check if a function is a generic function.
    /// @param name The function name.
    /// @return True if the function is generic.
    bool isGenericFunction(const std::string &name) const;

    /// @brief Get a generic function declaration.
    /// @param name The function name.
    /// @return Pointer to the declaration, or nullptr if not found.
    FunctionDecl *getGenericFunction(const std::string &name) const;

    /// @brief Get a user-defined function declaration by name.
    /// @param name The function name (as declared in the program).
    /// @return Pointer to the function declaration, or nullptr if not found.
    FunctionDecl *getFunctionDecl(const std::string &name) const;

    /// @brief Get all user-defined function overloads for a name.
    std::vector<FunctionDecl *> getFunctionOverloads(const std::string &name) const;

    /// @brief Instantiate a generic function with concrete type arguments.
    /// @param name The function name (e.g., "identity").
    /// @param args The concrete type arguments (e.g., [Integer]).
    /// @param loc Source location for error reporting.
    /// @return The instantiated function type, or unknown on error.
    TypeRef instantiateGenericFunction(const std::string &name,
                                       const std::vector<TypeRef> &args,
                                       SourceLoc loc);

    /// @brief Check if a type implements a given interface.
    /// @param type The type to check.
    /// @param interfaceName The interface name.
    /// @return True if the type implements the interface.
    bool typeImplementsInterface(TypeRef type, const std::string &interfaceName) const;

    /// @}

    //=========================================================================
    /// @name Completion / Tooling APIs
    /// @brief Read-only queries into the analyzer's resolved symbol tables.
    /// @details These APIs are designed for IDE tooling (code completion,
    /// hover, go-to-definition). They are only valid after analyze() returns.
    /// @{
    //=========================================================================

    /// @brief Get all symbols visible in the module-level (global) scope.
    /// @return All symbols defined at module level (functions, top-level vars,
    ///         class constructors, and imported runtime identifiers).
    /// @note Local variables inside function bodies are not included — they
    ///       are popped from the scope stack when their block finishes.
    std::vector<Symbol> getGlobalSymbols() const;

    /// @brief Get local, parameter, member, and global symbols visible at a cursor.
    /// @param fileId Source-manager file id for the cursor file.
    /// @param line 1-based cursor line.
    /// @param col 1-based cursor column.
    /// @return Visible symbols ordered from innermost/recent definitions outward.
    /// @details Unlike getGlobalSymbols(), this consults the scoped-symbol
    ///          snapshots captured during analysis, so completion can rank
    ///          local variables and parameters ahead of globals/runtime symbols.
    std::vector<Symbol> getVisibleSymbolsAtPosition(uint32_t fileId,
                                                    uint32_t line,
                                                    uint32_t col) const;

    /// @brief Get user-defined function overloads as tooling symbols.
    /// @param name Function name as written in source.
    /// @return One function symbol per overload, preserving parameter names.
    std::vector<Symbol> getFunctionOverloadSymbols(const std::string &name) const;

    /// @brief Find the most relevant symbol at a given cursor position.
    /// @param name The identifier name to look up.
    /// @param line 1-based line number.
    /// @param col 1-based column number.
    /// @return Pointer to the matching ScopedSymbol, or nullptr if not found.
    /// @details Searches all symbols captured during analysis (including locals
    /// and parameters that are no longer in scope). Returns the innermost
    /// (most recently defined before cursor) match.
    const ScopedSymbol *findSymbolAtPosition(const std::string &name,
                                             uint32_t fileId,
                                             uint32_t line,
                                             uint32_t col) const;

    /// @brief Get all visible fields and methods of a user-defined type.
    /// @param type An class, struct, or interface type reference.
    /// @return Symbols for each exposed field and method.
    ///         For runtime class pointer types (kind == Ptr, name non-empty),
    ///         delegates to getRuntimeMembers(type->name).
    ///         Returns an empty vector for primitive or unknown types.
    std::vector<Symbol> getMembersOf(const TypeRef &type) const;

    /// @brief Get all methods and properties of a runtime class.
    /// @param className Fully-qualified runtime class name (e.g. "Zanna.GUI.App").
    /// @return Symbols for each RT_METHOD (Kind::Method) and RT_PROP (Kind::Field).
    ///         Returns an empty vector if the class is not found in the registry.
    std::vector<Symbol> getRuntimeMembers(const std::string &className) const;

    /// @brief Get all user-defined type names declared in this module.
    /// @return Names of all class, struct, and interface declarations.
    std::vector<std::string> getTypeNames() const;

    /// @brief Get all bound module aliases visible in this module.
    /// @return Short names (aliases) from `bind Namespace as Alias;` declarations.
    ///         These are the prefixes users can type before `.` to access members.
    std::vector<std::string> getBoundModuleNames() const;

    /// @brief Get all exported symbols of a bound file module.
    /// @param moduleName The module name as declared in the bound file.
    /// @return All exported symbols from the module, or empty if not found.
    std::vector<Symbol> getModuleExports(const std::string &moduleName) const;

    /// @brief Get names of all file modules bound into the current source.
    /// @return Module names that can be used as `ModuleName.` completion roots.
    std::vector<std::string> getBoundFileModuleNames() const;

    /// @brief Resolve a bound alias to its full namespace path.
    /// @param alias Short alias name (e.g., "Math" from `bind Zanna.Math as Math;`).
    /// @return Full namespace string (e.g., "Zanna.Math"), or empty if not found.
    std::string resolveModuleAlias(const std::string &alias) const;

    /// @brief Enumerate runtime class names that are direct children of a namespace.
    /// @details Scans the RuntimeRegistry catalog for classes whose fully-qualified
    ///          name starts with `nsPrefix + "."` and returns the immediate child
    ///          identifier (the segment between the namespace and the next dot, or
    ///          the full suffix when there is no further nesting).
    ///          Example: nsPrefix="Zanna.GUI" → ["Canvas","App","ListBox",...]
    /// @param nsPrefix Full dotted namespace prefix (e.g. "Zanna.GUI").
    /// @return De-duplicated list of direct child class names.
    std::vector<std::string> getNamespaceClasses(const std::string &nsPrefix) const;

    /// @}

  private:
    //=========================================================================
    /// @name Error Reporting
    /// @brief Methods for reporting semantic errors.
    /// @{
    //=========================================================================

    /// @brief Report a semantic warning (legacy, uses generic V3001 code).
    /// @param loc Source location of the warning.
    /// @param message Warning message.
    void warning(SourceLoc loc, const std::string &message);

    /// @brief Report a coded semantic warning with policy and suppression checks.
    /// @param code The warning code (e.g., W001_UnusedVariable).
    /// @param loc Source location of the warning.
    /// @param message Warning message.
    /// @details Checks the warning policy (enabled/disabled, -Wall, -Wno-XXX),
    ///          inline suppression pragmas, and -Werror before emitting.
    void warn(WarningCode code, SourceLoc loc, const std::string &message);

    /// @brief Report a semantic error.
    /// @param loc Source location of the error.
    /// @param message Error message.
    void error(SourceLoc loc, const std::string &message);

    /// @brief Report a semantic error with an explicit stable diagnostic code.
    void errorWithCode(SourceLoc loc,
                       std::string code,
                       std::string message,
                       il::support::SourceRange range = {},
                       std::vector<il::support::DiagnosticNote> notes = {},
                       std::string help = {});

    /// @brief Find a close in-scope symbol name for an undefined identifier.
    std::optional<std::string> suggestSymbolName(const std::string &name) const;

    /// @brief Report a duplicate definition in the current lexical scope.
    /// @param name Symbol name being redefined.
    /// @param loc Source location of the new definition.
    /// @return false when a duplicate exists and the definition should be skipped.
    bool reportDuplicateDefinition(const std::string &name, SourceLoc loc);

    /// @brief Format overload candidates for ambiguity diagnostics.
    std::string formatOverloadCandidates(const std::vector<std::string> &candidates) const;

    /// @brief Report an undefined name error.
    /// @param loc Source location.
    /// @param name The undefined name.
    void errorUndefined(SourceLoc loc, const std::string &name);

    /// @brief Report that a runtime method was accessed without call parentheses.
    /// @details Emits `V-ZIA-METHOD-CALL` with an "add ()" fix-it when a zero-arg
    ///          overload exists (so the fix fully resolves it). Prevents the
    ///          member-access resolver from silently returning `unknown()` for a
    ///          real method (which the lowerer would miscompile).
    /// @param expr The field-access expression (its `loc` is the `.` token).
    /// @param className Fully-qualified runtime class name.
    /// @param candidates `methodCandidates` results (`"Name/arity"`) for the field.
    void errorRuntimeMethodNeedsCall(FieldExpr *expr, const std::string &className,
                                     const std::vector<std::string> &candidates);

    /// @brief Report a type mismatch error.
    /// @param loc Source location.
    /// @param expected The expected type.
    /// @param actual The actual type.
    void errorTypeMismatch(SourceLoc loc, TypeRef expected, TypeRef actual);

    /// @}
    //=========================================================================
    /// @name Built-in Registration
    /// @{
    //=========================================================================

    /// @brief Register built-in types and functions.
    ///
    /// @details Registers:
    /// - Primitive types (Integer, Number, Boolean, String, etc.)
    /// - Collection constructors (List, Map, Set)
    /// - Runtime library functions based on imports
    void registerBuiltins();

    /// @}
    //=========================================================================
    /// @name Member Variables
    /// @{
    //=========================================================================

    /// @brief Check for unused variables in the given scope and emit W001 warnings.
    /// @param scope The scope to check for unused symbols.
    /// @details Called from popScope() for function-level scopes. Warns for
    ///          Variable and Parameter symbols that were never referenced.
    ///          Skips symbols named "_" (conventional discard name).
    void checkUnusedVariables(const Scope &scope);

    /// @brief Compute an approximate lexical end location for a statement-owned scope.
    static SourceLoc scopeEndForStmt(const Stmt *stmt);

    /// @}
    //=========================================================================
    /// @name Warning Infrastructure
    /// @{
    //=========================================================================

    /// @brief Warning policy controlling which warnings are enabled.
    /// @details Null until initWarnings() is called; warn() uses default policy.
    const WarningPolicy *warningPolicy_{nullptr};

    /// @brief Source-level warning suppressions from @suppress comments.
    WarningSuppressions suppressions_;

  public:
    /// @brief Mutable access for compiler-side import scanning.
    WarningSuppressions &warningSuppressions() {
        return suppressions_;
    }

  private:
    /// @}
    //=========================================================================
    /// @name Core State
    /// @{
    //=========================================================================

    /// @brief Diagnostic engine for error reporting.
    il::support::DiagnosticEngine &diag_;

    /// @brief Whether any errors have occurred.
    bool hasError_{false};

    /// @brief Reserved compatibility flag; raw pointers are not source-visible.
    bool allowUnsafePointers_{false};

    /// @brief Current module being analyzed.
    ModuleDecl *currentModule_{nullptr};

    /// @brief Current function being analyzed (for return validation).
    FunctionDecl *currentFunction_{nullptr};

    /// @brief Current method being analyzed, if any.
    MethodDecl *currentMethod_{nullptr};

    /// @brief Type of `self` in current method context.
    /// @details Set when analyzing methods, cleared afterward.
    TypeRef currentSelfType_{nullptr};

    /// @brief Expected return type of current function/method.
    /// @details Used to validate return statements.
    TypeRef expectedReturnType_{nullptr};

    /// @brief Expected function type for the next lambda to be analyzed.
    /// @details Set by contexts that know the target function type (a variable
    /// initializer with a function-type annotation, a collection combinator
    /// argument). analyzeLambda() consumes and clears it to infer omitted
    /// parameter types. Null when no expected type is available.
    TypeRef lambdaTypeHint_{nullptr};

    /// @brief Current loop nesting depth for break/continue validation.
    int loopDepth_{0};

    /// @brief Current catch nesting depth for bare `throw;` validation.
    int catchDepth_{0};

    /// @brief Current type resolution nesting depth for recursion guard.
    unsigned typeResolveDepth_{0};
    /// @brief Maximum allowed type resolution depth.
    static constexpr unsigned kMaxTypeResolveDepth = 256;

    /// @brief Current namespace prefix for qualified names.
    /// @details When inside a namespace block, this contains the namespace path.
    /// Empty when at module level. Example: "MyLib.Internal"
    std::string namespacePrefix_;

    /// @brief All symbol definitions captured during analysis for position-based lookup.
    /// @details Populated by defineSymbol(). Persists after scopes are popped.
    std::vector<ScopedSymbol> scopedSymbols_;

    struct ScopeSnapshot {
        uint32_t id{0};
        uint32_t parentId{0};
        size_t depth{0};
        SourceLoc startLoc{};
        SourceLoc endLoc{};
    };

    std::unordered_map<uint32_t, ScopeSnapshot> scopeSnapshots_;
    uint32_t nextScopeId_{1};

    /// @brief Owned lexical scope stack (scopes_[0] is global).
    std::vector<std::unique_ptr<Scope>> scopes_;

    /// @brief The current scope for symbol lookup.
    Scope *currentScope_{nullptr};

    /// @brief Map from expression pointers to their resolved types.
    /// @details Populated during expression analysis.
    // Memoized inferred expression types. `mutable` because this is a cache, not
    // part of the analyzer's logical state: const inference helpers may populate
    // it without a const_cast.
    mutable std::unordered_map<const Expr *, TypeRef> exprTypes_;

    /// @brief Map from type names to semantic types.
    /// @details Includes both built-in types and user-defined types.
    std::unordered_map<std::string, TypeRef> typeRegistry_;

    /// @brief Source-file id to declared module identity for imported Zia files.
    std::unordered_map<uint32_t, std::string> fileModuleNames_;

    /// @brief Semantic names for top-level declarations whose short names collide.
    std::unordered_map<const Decl *, std::string> semanticDeclNames_;

    /// @brief File-local short top-level names to semantic names.
    std::unordered_map<uint32_t, std::unordered_map<std::string, std::string>> fileScopedDeclNames_;

    /// @brief Struct type declarations for pattern analysis.
    std::unordered_map<std::string, StructDecl *> structDecls_;

    /// @brief Class type declarations for pattern analysis.
    std::unordered_map<std::string, ClassDecl *> classDecls_;

    /// @brief Interface declarations for implementation checks.
    std::unordered_map<std::string, InterfaceDecl *> interfaceDecls_;

    /// @brief Enum declarations for variant resolution.
    std::unordered_map<std::string, EnumDecl *> enumDecls_;

    /// @brief Map from method signatures to function types.
    /// @details Key format: "TypeName.methodName"
    /// Used for method call resolution.
    std::unordered_map<std::string, TypeRef> methodTypes_;

    /// @brief Exact method function types keyed by declaration pointer.
    std::unordered_map<const MethodDecl *, TypeRef> methodDeclTypes_;

    /// @brief Exact method function types keyed by owner type and declaration.
    std::unordered_map<MethodInstanceKey, TypeRef, MethodInstanceKeyHash> ownerMethodTypes_;

    /// @brief Exact function types keyed by declaration pointer.
    std::unordered_map<const FunctionDecl *, TypeRef> functionDeclTypes_;

    /// @brief Type aliases: `type Name = TargetType;` resolved to TypeRef.
    std::unordered_map<std::string, TypeRef> typeAliases_;

    /// @brief Top-level function overload families keyed by qualified name.
    std::unordered_map<std::string, std::vector<FunctionDecl *>> functionOverloads_;

    /// @brief Method overload families keyed by "TypeName.methodName".
    std::unordered_map<std::string, std::vector<MethodDecl *>> methodOverloads_;

    /// @brief Lowered symbol names for function declarations.
    std::unordered_map<const FunctionDecl *, std::string> loweredFunctionNames_;

    /// @brief Lowered symbol names for method declarations.
    std::unordered_map<const MethodDecl *, std::string> loweredMethodNames_;

    /// @brief Lowered symbol names keyed by owner type and declaration.
    std::unordered_map<MethodInstanceKey, std::string, MethodInstanceKeyHash>
        ownerLoweredMethodNames_;

    /// @brief Exact signature keys for method declarations.
    std::unordered_map<const MethodDecl *, std::string> methodSignatureKeys_;

    /// @brief Exact signature keys keyed by owner type and declaration.
    std::unordered_map<MethodInstanceKey, std::string, MethodInstanceKeyHash>
        ownerMethodSignatureKeys_;

    /// @brief Dispatch slot keys for method declarations.
    std::unordered_map<const MethodDecl *, std::string> methodDispatchKeys_;

    /// @brief Dispatch slot keys keyed by owner type and declaration.
    std::unordered_map<MethodInstanceKey, std::string, MethodInstanceKeyHash>
        ownerMethodDispatchKeys_;

    /// @brief Map from field signatures to field types.
    /// @details Key format: "TypeName.fieldName"
    std::unordered_map<std::string, TypeRef> fieldTypes_;

    /// @brief Field signatures that are declared static.
    /// @details Key format: "TypeName.fieldName"
    std::unordered_set<std::string> staticFields_;

    /// @brief Field signatures that are declared final.
    /// @details Key format: "TypeName.fieldName"
    std::unordered_set<std::string> finalFields_;

    /// @brief Map from member signatures to visibility.
    /// @details Key format: "TypeName.memberName"
    std::unordered_map<std::string, Visibility> memberVisibility_;

    /// @brief Map from call expressions to their resolved extern function names.
    /// @details Populated for calls to extern functions (runtime library).
    /// Used during lowering to emit extern calls instead of direct calls.
    std::unordered_map<const CallExpr *, std::string> runtimeCallees_;

    /// @brief Raw-pointer metadata keyed by canonical runtime function name.
    std::unordered_map<std::string, RuntimePointerSafety> runtimePointerSafety_;

    /// @brief Map from identifier expressions to zero-arg getter function names.
    /// @details Populated for property-like identifiers imported via bind
    /// (e.g., Pi → Zanna.Math.get_Pi). Lowerer emits a call instead of a load.
    std::unordered_map<const Expr *, std::string> autoEvalGetters_;

    /// @brief Map from identifier expressions to resolved semantic top-level names.
    std::unordered_map<const IdentExpr *, std::string> resolvedIdentNames_;

    /// @brief Resolved generic function call mangled names.
    /// @details Key: CallExpr pointer, Value: Mangled function name (e.g., "identity$Integer").
    /// Used by the lowerer to determine which instantiated function to call.
    std::unordered_map<const CallExpr *, std::string> genericFunctionCallees_;

    /// @brief Resolved direct-call targets for user-defined functions.
    std::unordered_map<const CallExpr *, std::string> resolvedFunctionCallees_;

    /// @brief Resolved function declarations for direct-call sites.
    std::unordered_map<const CallExpr *, FunctionDecl *> resolvedFunctionDecls_;

    /// @brief Resolved method declarations for call sites.
    std::unordered_map<const CallExpr *, MethodDecl *> resolvedMethodDecls_;

    /// @brief Concrete semantic function types for explicit generic method call sites.
    std::unordered_map<const CallExpr *, TypeRef> genericMethodConcreteTypes_;

    /// @brief Erased lowered function types for explicit generic method call sites.
    std::unordered_map<const CallExpr *, TypeRef> genericMethodErasedTypes_;

    /// @brief Owning type name for resolved method call sites.
    std::unordered_map<const CallExpr *, std::string> resolvedMethodOwnerTypes_;

    /// @brief Dispatch slot key for resolved method call sites.
    std::unordered_map<const CallExpr *, std::string> resolvedMethodSlotKeys_;

    /// @brief Resolved init declarations for new-expressions.
    std::unordered_map<const NewExpr *, MethodDecl *> resolvedInitDecls_;

    /// @brief Resolved owner type for init overloads selected on new-expressions.
    std::unordered_map<const NewExpr *, std::string> resolvedInitOwnerTypes_;

    /// @brief Resolved init declarations for constructor-style type calls.
    std::unordered_map<const CallExpr *, MethodDecl *> resolvedTypeCallInitDecls_;

    /// @brief Resolved owner type for constructor-style init overloads.
    std::unordered_map<const CallExpr *, std::string> resolvedTypeCallInitOwnerTypes_;

    /// @brief Bound argument layouts for resolved call expressions.
    std::unordered_map<const CallExpr *, CallArgBinding> callArgBindings_;

    /// @brief Bound argument layouts for resolved new-expressions.
    std::unordered_map<const NewExpr *, CallArgBinding> newArgBindings_;

    /// @brief Map from field expressions to their resolved getter names.
    std::unordered_map<const FieldExpr *, std::string> resolvedFieldGetters_;

    /// @brief Map from module-qualified field expressions to resolved symbol names.
    std::unordered_map<const FieldExpr *, std::string> resolvedFieldSymbolNames_;

    /// @brief Map from field-expression assignment targets to their resolved setter names.
    std::unordered_map<const FieldExpr *, std::string> resolvedFieldSetters_;

    /// @brief Set of bind paths seen in the current module (file binds).
    std::unordered_set<std::string> binds_;

    /// @brief Bound runtime namespaces (e.g., "Zanna.Terminal").
    /// @details Maps namespace prefix to optional alias. Empty alias means
    /// full namespace import (all symbols imported without prefix).
    std::unordered_map<std::string, std::string> boundNamespaces_;

    /// @brief Reverse map from alias to namespace for O(1) alias resolution.
    /// @details Populated alongside boundNamespaces_ for non-empty aliases.
    std::unordered_map<std::string, std::string> aliasToNamespace_;

    /// @brief Symbols imported from bound namespaces.
    /// @details Maps short name (e.g., "Say") to full qualified name
    /// (e.g., "Zanna.Terminal.Say"). Used for unqualified function calls
    /// and constructor resolution.
    std::unordered_map<std::string, std::string> importedSymbols_;

    /// @brief Fallback map from globally unique imported module names to exports.
    /// @details File-local import maps are authoritative for normal source
    /// lookups. This map preserves compatibility for context-free completion and
    /// analysis paths where no source location is available.
    std::unordered_map<std::string, std::unordered_map<std::string, Symbol>> moduleExports_;

    /// @brief File-local file-module exports keyed by importer file id and module name.
    /// @details Allows two different files to bind different modules under the same
    /// local alias without corrupting qualified lookup for either file.
    std::unordered_map<uint32_t,
                       std::unordered_map<std::string, std::unordered_map<std::string, Symbol>>>
        fileModuleExports_;

    /// @brief File-local file-module ids keyed by importer file id and visible module name.
    /// @details Used for qualified type references such as `player.Player` even when the target
    ///          type is not exported from the bound file. File-local lookup prevents two source
    ///          files that bind different modules under the same alias from sharing a stale id.
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> fileBoundModuleIds_;

    /// @brief Bound file-module aliases/names mapped to their defining file id.
    std::unordered_map<std::string, uint32_t> boundFileModuleIds_;

    /// @brief Import-all permissions keyed by importer file id, then imported file id.
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> unqualifiedFileImportAll_;

    /// @brief Selective import permissions keyed by importer file id, imported file id, item name.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::unordered_set<std::string>>>
        unqualifiedFileImportItems_;

    /// @brief Active type parameter substitutions for current generic context.
    /// @details Maps type parameter names (e.g., "T") to concrete types.
    /// Stack allows nested generic contexts (e.g., generic method in generic type).
    std::vector<std::map<std::string, TypeRef>> typeParamStack_;

    /// @brief Stack of narrowed type overrides for flow-sensitive type analysis.
    /// @details When analyzing code after a null check like `if (x != null)`,
    /// we narrow `x` from `T?` to `T`. Each entry maps variable names to their
    /// narrowed types. Pushed/popped with narrowing scopes.
    std::vector<std::unordered_map<std::string, TypeRef>> narrowedTypes_;

    /// @brief Set of variables that have been definitely initialized.
    /// @details Stores scope-qualified keys produced by initializedSymbolKey()
    ///          for variables assigned a value (either via declaration
    ///          initializer or explicit assignment). Used to warn about use of
    ///          potentially uninitialized variables without leaking state
    ///          between shadowing declarations.
    std::unordered_set<std::string> initializedVars_;

    /// @brief Cache of instantiated generic types.
    /// @details Key: "TypeName$Arg1$Arg2", Value: Instantiated TypeRef.
    /// Prevents re-instantiation of the same generic type arguments.
    std::map<std::string, TypeRef> genericInstances_;

    /// @brief Type parameter substitutions keyed by instantiated generic type name.
    std::map<std::string, std::map<std::string, TypeRef>> genericTypeSubstitutions_;

    /// @brief Original generic type declarations (uninstantiated).
    /// @details Key: Type name, Value: AST declaration pointer.
    /// Used to find the original type when instantiating.
    std::map<std::string, Decl *> genericTypeDecls_;

    /// @brief Original generic function declarations (uninstantiated).
    /// @details Key: Function name, Value: AST declaration pointer.
    /// Used to find the original function when instantiating.
    std::map<std::string, FunctionDecl *> genericFunctionDecls_;

    /// @brief Cache of instantiated generic function types.
    /// @details Key: "FuncName$Arg1$Arg2", Value: Instantiated function TypeRef.
    /// Prevents re-instantiation of the same generic function arguments.
    std::map<std::string, TypeRef> genericFunctionInstances_;

    /// @brief Type parameter substitutions keyed by instantiated generic function name.
    std::map<std::string, std::map<std::string, TypeRef>> genericFunctionSubstitutions_;

    /// @brief User-defined function declarations for default parameter lookup.
    /// @details Key: Function name, Value: AST declaration pointer.
    /// Used by validateCallArgs to exempt missing args with defaults.
    std::map<std::string, FunctionDecl *> functionDecls_;

    struct CallParamSpec {
        std::string name;
        TypeRef type;
        bool hasDefault = false;
        bool isVariadic = false;
    };

    bool bindCallArgs(const std::vector<CallArg> &args,
                      const std::vector<CallParamSpec> &params,
                      SourceLoc loc,
                      const std::string &calleeName,
                      CallArgBinding &binding,
                      int *score = nullptr,
                      bool reportErrors = false,
                      bool allowRuntimeObjectCoercion = false) const;

    bool checkRuntimePointerSafety(const std::string &calleeName,
                                   const std::vector<CallArg> &args,
                                   const std::vector<CallParamSpec> &params,
                                   const CallArgBinding &binding,
                                   size_t skipLeadingParams,
                                   SourceLoc loc) const;

    FunctionDecl *resolveFunctionArgOverload(const std::string &name,
                                             const std::vector<CallArg> &args,
                                             SourceLoc loc,
                                             std::string *loweredName,
                                             CallArgBinding *bindingOut,
                                             bool viaQualifiedModule = false);
    MethodDecl *resolveMethodArgOverload(const std::string &ownerType,
                                         const std::string &methodName,
                                         const std::vector<CallArg> &args,
                                         SourceLoc loc,
                                         std::string *resolvedOwnerType,
                                         bool includeInherited,
                                         CallArgBinding *bindingOut);

    /// @brief Build the call-argument parameter specs (name/type/default) for
    ///        a Zia function or method from its params and resolved types.
    std::vector<CallParamSpec> makeParamSpecs(const std::vector<Param> &params,
                                              const std::vector<TypeRef> &paramTypes) const;
    /// @brief Build parameter specs for an extern symbol, optionally skipping
    ///        the first @p skipLeadingParams (e.g. an implicit receiver).
    std::vector<CallParamSpec> makeExternParamSpecs(const Symbol &sym,
                                                    size_t skipLeadingParams = 0) const;
    /// @brief Build parameter specs from a struct type's fields, used to
    ///        check positional/named struct construction calls.
    std::vector<CallParamSpec> makeStructFieldSpecs(const std::string &typeName) const;
    /// @brief Build parameter specs from a class type's fields (including
    ///        inherited fields), used to check class construction calls.
    std::vector<CallParamSpec> makeClassFieldSpecs(const std::string &typeName) const;
    /// @brief Append @p typeName's class field specs (recursing into base
    ///        classes) onto @p out — the recursive helper for makeClassFieldSpecs.
    void appendClassFieldSpecs(const std::string &typeName, std::vector<CallParamSpec> &out) const;

    /// @}
};

/// @}

} // namespace il::frontends::zia
