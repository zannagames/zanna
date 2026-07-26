//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: frontends/zia/sema/SemaTypes.hpp
// Purpose: Symbol, ScopedSymbol, and Scope types used by the Zia semantic
//          analyzer. Extracted from Sema.hpp to reduce header size and
//          allow independent inclusion by IDE tooling and lowerer code.
// Key invariants:
//   - Symbol names are unique within a single Scope
//   - Scope parent pointers are immutable after construction
// Ownership/Lifetime:
//   - Scope instances are owned by Sema (via std::unique_ptr)
//   - Symbol instances are value-stored inside Scope maps
// Links: frontends/zia/Sema.hpp, frontends/zia/Types.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines Zia semantic symbols, persistent definition snapshots, and
///        lexical scope lookup.
/// @details Scope owns name-to-symbol values and borrows an immutable parent
///          link, while ScopedSymbol retains position/owner metadata after
///          transient analysis scopes have been popped.

#pragma once

#include "frontends/zia/AST.hpp"
#include "frontends/zia/Types.hpp"
#include "support/source_location.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
/// @name Symbol Information
/// @brief Structure for tracking declared symbols.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Information about a declared symbol (variable, function, type, etc.).
/// @details Represents any named symbol that can be looked up in a scope.
/// Used during semantic analysis to track declarations and their types.
///
/// ## Symbol Categories
///
/// - **Variable**: Local or global variable, can be read/written based on isFinal
/// - **Parameter**: Read-only function/method parameter
/// - **Function**: Global function that can be called
/// - **Method**: Method on a type that can be called on an object
/// - **Field**: Field in a type that can be accessed on an object
/// - **Type**: Type declaration (struct, class, interface)
struct Symbol {
    /// @brief The kind of symbol.
    /// @details Determines how the symbol can be used in expressions.
    enum class Kind {
        Variable,  ///< Local or global variable
        Parameter, ///< Function/method parameter
        Function,  ///< Global function declaration
        Method,    ///< Method in a type declaration
        Field,     ///< Field in a type declaration
        Type,      ///< Type declaration (struct, class, interface)
        Module,    ///< Imported module namespace
    };

    /// @brief The symbol kind.
    Kind kind{Kind::Variable};

    /// @brief The symbol name as declared.
    std::string name;

    /// @brief The resolved semantic type of this symbol.
    /// @details For functions/methods, this is the function type.
    /// For types, this is the type itself (e.g., classType("MyClass")).
    TypeRef type;

    /// @brief True if this symbol is immutable (declared with `final`).
    /// @details Only meaningful for Variable and Field kinds.
    bool isFinal{false};

    /// @brief True if this is an external/runtime function.
    /// @details For functions in the Zanna.* namespace, this is true.
    /// The lowerer uses this to emit extern calls instead of direct calls.
    bool isExtern{false};

    /// @brief True if this symbol is exported from its defining source file.
    /// @details Used to enforce file-private vs imported visibility for
    /// top-level declarations referenced across Zia source files.
    bool isExported{false};

    /// @brief Whether this symbol has been referenced (read) in the source.
    /// @details Used by W001 (unused-variable) to detect variables that are
    /// declared but never read. Set to true by Sema::analyzeIdent() on lookup.
    bool used{false};

    /// @brief Pointer to the AST declaration node.
    /// @details May be nullptr for built-in symbols or extern functions.
    Decl *decl{nullptr};

    /// @brief Surface parameter names for callable symbols when available.
    /// @details Runtime externs populate this from runtime headers so Zia can
    /// support named arguments consistently across user code and runtime APIs.
    std::vector<std::string> paramNames;

    /// @brief Optional documentation text for symbols without source comments.
    /// @details Runtime symbols populate this from generated runtime metadata.
    std::string documentation;

    /// @brief Source location of the definition when available.
    SourceLoc loc{};
};

/// @}

/// @brief Snapshot of a symbol definition for position-based hover lookup.
/// @details Captured during analysis by defineSymbol(). Persists after scopes
/// are popped, enabling IDE queries for local variables, parameters, and fields.
struct ScopedSymbol {
    Symbol symbol;         ///< The full symbol metadata (kind, name, type, etc.)
    SourceLoc loc;         ///< Position of the defining declaration/statement
    std::string ownerType; ///< Class name when inside class body (empty otherwise)
    uint32_t scopeId{0};   ///< Lexical scope that owns the definition
};

//===----------------------------------------------------------------------===//
/// @name Scope Management
/// @brief Class for managing symbol scopes.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Scope for symbol lookup.
/// @details Represents a lexical scope containing symbol definitions.
/// Scopes are linked to parent scopes to enable nested lookup.
///
/// ## Scope Hierarchy
///
/// Scopes form a tree structure:
/// - Global scope (module-level)
///   - Function scope
///     - Block scope (if, while, for bodies)
///       - Nested block scope
///
/// Symbol lookup proceeds from innermost to outermost scope.
///
/// @invariant A scope's parent pointer is set at construction and never changes.
/// @invariant Symbol names are unique within a single scope.
class Scope {
  public:
    /// @brief Create a scope with an optional parent.
    /// @param parent The enclosing scope, or nullptr for global scope.
    /// @param id Stable identifier assigned by semantic analysis.
    /// @param depth Lexical nesting depth, with zero denoting global scope.
    explicit Scope(Scope *parent = nullptr, uint32_t id = 0, size_t depth = 0)
        : parent_(parent), id_(id), depth_(depth) {}

    /// @brief Define a symbol in this scope.
    /// @param name The symbol name.
    /// @param symbol The symbol information.
    ///
    /// @details If a symbol with the same name already exists in this scope,
    /// it is replaced. Bindings in parent scopes are not affected.
    /// @post A local lookup of @p name returns the supplied symbol value.
    void define(const std::string &name, Symbol symbol);

    /// @brief Look up a symbol by name in this scope and ancestors.
    /// @param name The symbol name to find.
    /// @return Pointer to the symbol if found, nullptr otherwise.
    ///
    /// @details Searches this scope first, then parent scopes recursively.
    /// Returns the first match found (innermost scope wins for shadowing).
    Symbol *lookup(const std::string &name);

    /// @brief Look up a symbol only in this scope (not ancestors).
    /// @param name The symbol name to find.
    /// @return Pointer to the symbol if found in this scope, nullptr otherwise.
    ///
    /// @details Used to check for redefinition in the current scope.
    Symbol *lookupLocal(const std::string &name);

    /// @brief Get the parent scope.
    /// @return The parent scope, or nullptr for the global scope.
    Scope *parent() const {
        return parent_;
    }

    /// @brief Stable scope identifier assigned by Sema.
    /// @return Identifier used to correlate scope snapshots and definitions.
    uint32_t id() const {
        return id_;
    }

    /// @brief Lexical nesting depth (0 for global scope).
    /// @return Number of enclosing lexical scope levels.
    size_t depth() const {
        return depth_;
    }

    /// @brief Check if any symbol name starts with the given prefix.
    /// @param prefix Prefix compared at byte offset zero; an empty prefix
    ///        matches any non-empty symbol table.
    /// @return True when at least one local symbol begins with @p prefix.
    bool hasSymbolWithPrefix(const std::string &prefix) const {
        for (const auto &[name, _] : symbols_) {
            if (name.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    /// @brief Get all symbols defined in this scope (not ancestors).
    /// @return Const reference to the symbols map.
    /// @details Used by completion tools and unused-variable checks.
    const std::unordered_map<std::string, Symbol> &getSymbols() const {
        return symbols_;
    }

  private:
    /// @brief The enclosing scope.
    Scope *parent_{nullptr};

    /// @brief Stable scope identifier assigned by Sema.
    uint32_t id_{0};

    /// @brief Lexical nesting depth.
    size_t depth_{0};

    /// @brief Symbols defined in this scope.
    std::unordered_map<std::string, Symbol> symbols_;
};

/// @}

} // namespace il::frontends::zia
