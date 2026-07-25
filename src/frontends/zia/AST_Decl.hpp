//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/AST_Decl.hpp
// Purpose: Define Zia declaration nodes and their owned signature/member data.
// Key invariants:
//   * Decl::kind matches each node's concrete declaration type.
//   * ModuleDecl is the owning root for file-level declarations.
//   * Child types, expressions, statements, and declarations use unique
//     ownership unless explicitly recorded as semantic metadata.
// Ownership: Parent declarations own child AST nodes through unique_ptr values
//            and vectors of owning pointer aliases.
// References: docs/languages/zia-reference.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Declaration nodes for the Zia AST.
///
/// @details Defines all declaration AST nodes produced by the Zia parser.
/// Declarations introduce named entities that can be referenced from other
/// parts of the code. This includes:
///
///   - ModuleDecl: The top-level container holding all declarations in a file.
///   - FunctionDecl: Function definitions with name, parameters, return type, body.
///   - ClassDecl/StructDecl: User-defined types (classes/structs) with fields and methods.
///   - FieldDecl: Fields within a class or struct declaration.
///   - BindDecl: `bind` statements referencing imported modules.
///
/// The parser creates declaration nodes by recognizing top-level keywords
/// (`func`, `class`, `bind`, `extern`). The semantic analyzer registers
/// declarations in the symbol table and checks for conflicts, completeness,
/// and type correctness. The lowerer translates each declaration into the
/// corresponding IL construct (il::Function, il::ExternFunction, etc.).
///
/// @invariant Every Decl has a valid `kind` field matching its concrete type.
/// @invariant ModuleDecl is always the root; it cannot be nested.
/// @invariant Function and class names are non-empty after successful parsing.
///
/// Ownership/Lifetime: Declarations are owned by their containing ModuleDecl
/// via DeclPtr (std::unique_ptr<Decl>). The ModuleDecl itself is owned by
/// the compilation pipeline.
///
/// @see AST_Stmt.hpp — statement nodes that appear in function bodies.
/// @see Sema.hpp — registers declarations in the symbol table and type-checks.
/// @see Lowerer.hpp — translates declarations into IL functions and types.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/AST_Stmt.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace il::frontends::zia {
//===----------------------------------------------------------------------===//
/// @name Declaration Nodes
/// @brief AST nodes representing declarations that introduce named entities.
/// @details Declarations define types, functions, fields, and modules.
/// They establish names that can be referenced from other parts of the code.
/// @{
//===----------------------------------------------------------------------===//

/// @brief Enumerates all kinds of declaration nodes.
/// @details Used for runtime type identification when processing declarations.
enum class DeclKind {
    /// @brief Module declaration: the compilation unit.
    /// @see ModuleDecl
    Module,

    /// @brief Bind declaration: brings external namespaces into scope with alias.
    /// @see BindDecl
    Bind,

    /// @brief Struct type declaration: copy-semantics struct.
    /// @see StructDecl
    Struct,

    /// @brief Class type declaration: reference-semantics class.
    /// @see ClassDecl
    Class,

    /// @brief Interface declaration: abstract type contract.
    /// @see InterfaceDecl
    Interface,

    /// @brief Function declaration: global function.
    /// @see FunctionDecl
    Function,

    /// @brief Field declaration: member variable.
    /// @see FieldDecl
    Field,

    /// @brief Method declaration: member function.
    /// @see MethodDecl
    Method,

    /// @brief Constructor declaration: object initializer.
    /// @see ConstructorDecl
    Constructor,

    /// @brief Global variable declaration: module-level variable.
    /// @see GlobalVarDecl
    GlobalVar,

    /// @brief Namespace declaration: groups declarations under a qualified name.
    /// @see NamespaceDecl
    Namespace,

    /// @brief Property declaration: computed property with getter/setter.
    /// @see PropertyDecl
    Property,

    /// @brief Destructor declaration: class cleanup code.
    /// @see DestructorDecl
    Destructor,

    /// @brief Enum type declaration: named set of integer constants.
    /// @see EnumDecl
    Enum,

    /// @brief Type alias declaration: `type Name = TargetType;`
    /// @see TypeAliasDecl
    TypeAlias,
};

/// @brief Member visibility level.
/// @details Controls access to fields and methods from outside the type.
enum class Visibility {
    /// @brief Private: only accessible within the type.
    /// @details Default for class fields to encourage encapsulation.
    Private,

    /// @brief Public: accessible from anywhere.
    /// @details Default for value fields and exposed members.
    Public,
};

/// @brief Base class for all declaration nodes.
/// @details Declarations introduce named entities into the program.
///
/// @invariant `kind` correctly identifies the concrete subclass type.
struct Decl {
    /// @brief Identifies the concrete declaration kind for downcasting.
    DeclKind kind;

    /// @brief Source location of this declaration.
    SourceLoc loc;

    /// @brief True when this declaration is exported from its defining file.
    /// @details Exported declarations may be imported and referenced from
    /// other Zia source files via `bind`. Non-exported declarations remain
    /// file-private even though imported files are analyzed in the same unit.
    bool isExported = true;

    /// @brief Construct a declaration with kind and location.
    /// @param k The specific declaration kind.
    /// @param l Source location.
    Decl(DeclKind k, SourceLoc l) : kind(k), loc(l) {}

    /// @brief Virtual destructor for proper polymorphic cleanup.
    virtual ~Decl() = default;
};

/// @brief Function parameter specification.
/// @details Represents one parameter in a function signature, with
/// name, type, and optional default value.
struct Param {
    /// @brief Parameter name.
    std::string name;

    /// @brief Parameter type (required for function parameters).
    TypePtr type;

    /// @brief Default value expression (nullptr if required parameter).
    ExprPtr defaultValue;

    /// @brief Source location of the parameter name token.
    SourceLoc loc;

    /// @brief True if this is a variadic parameter (`...Type`).
    bool isVariadic = false;
};

/// @brief Generic type parameter specification.
/// @details Represents a type parameter in a generic declaration, with
/// an optional constraint (interface name) that the type must satisfy.
///
/// ## Examples
/// - `T` - Unconstrained type parameter
/// - `T: Comparable` - Type parameter constrained to Comparable interface
struct TypeParam {
    /// @brief Type parameter name (e.g., "T", "K", "V").
    std::string name;

    /// @brief Optional constraint interface name (empty if unconstrained).
    /// @details When non-empty, the concrete type argument must implement
    /// this interface.
    std::string constraint;

    /// @brief Construct an unconstrained type parameter.
    /// @param n Type parameter name.
    TypeParam(std::string n) : name(std::move(n)) {}

    /// @brief Construct a constrained type parameter.
    /// @param n Type parameter name.
    /// @param c Required interface or type constraint.
    TypeParam(std::string n, std::string c) : name(std::move(n)), constraint(std::move(c)) {}
};

/// @brief Global function declaration.
/// @details Defines a function at module level (not a method).
///
/// ## Example
/// ```
/// func add(a: Integer, b: Integer) -> Integer {
///     return a + b;
/// }
/// ```
struct FunctionDecl : Decl {
    /// @brief Function name.
    std::string name;

    /// @brief Generic type parameter names (e.g., [T, U]).
    std::vector<std::string> genericParams;

    /// @brief Optional constraints for generic type parameters.
    /// @details Parallel array to genericParams. If genericParamConstraints[i] is non-empty,
    /// it specifies the interface that genericParams[i] must implement.
    std::vector<std::string> genericParamConstraints;

    /// @brief Function parameters.
    std::vector<Param> params;

    /// @brief Return type (nullptr = void).
    TypePtr returnType;

    /// @brief Function body (nullptr for interface method signatures).
    StmtPtr body;

    /// @brief Function visibility.
    Visibility visibility = Visibility::Private;

    /// @brief True if this is a foreign function (imported from another module, no body).
    bool isForeign = false;

    /// @brief True if this is an async function (returns Future).
    bool isAsync = false;

    /// @brief True if this overrides a parent method.
    bool isOverride = false;

    /// @brief Construct a function declaration.
    /// @param l Source location.
    /// @param n Function name.
    FunctionDecl(SourceLoc l, std::string n) : Decl(DeclKind::Function, l), name(std::move(n)) {}
};

/// @brief Field declaration within a value or class type.
/// @details Defines a member variable with type, visibility, and modifiers.
///
/// ## Modifiers
/// - `final`: Field cannot be reassigned after construction
/// - `weak`: For class types, creates a weak reference (no ref counting)
/// - `expose`/`hide`: Controls visibility (public/private)
struct FieldDecl : Decl {
    /// @brief Field name.
    std::string name;

    /// @brief Field type.
    TypePtr type;

    /// @brief Initial value expression (nullptr = default/required in constructor).
    ExprPtr initializer;

    /// @brief Field visibility.
    Visibility visibility = Visibility::Private;

    /// @brief True if field cannot be reassigned.
    bool isFinal = false;

    /// @brief True if this is a weak reference (class types only).
    bool isWeak = false;

    /// @brief True if this is a static (type-level) field.
    bool isStatic = false;

    /// @brief Construct a field declaration.
    /// @param l Source location.
    /// @param n Field name.
    FieldDecl(SourceLoc l, std::string n) : Decl(DeclKind::Field, l), name(std::move(n)) {}
};

/// @brief Method declaration within a value or class type.
/// @details Defines a member function. Methods have access to `self`.
///
/// ## Example
/// ```
/// class Player {
///     func heal(amount: Integer) {
///         self.health = self.health + amount;
///     }
/// }
/// ```
struct MethodDecl : Decl {
    /// @brief Method name.
    std::string name;

    /// @brief Generic type parameter names.
    std::vector<std::string> genericParams;

    /// @brief Optional constraints for generic type parameters.
    /// @details Parallel array to genericParams. If genericParamConstraints[i] is non-empty,
    /// it specifies the interface that genericParams[i] must implement.
    std::vector<std::string> genericParamConstraints;

    /// @brief Method parameters (does not include implicit `self`).
    std::vector<Param> params;

    /// @brief Return type (nullptr = void).
    TypePtr returnType;

    /// @brief Method body.
    StmtPtr body;

    /// @brief Method visibility.
    Visibility visibility = Visibility::Private;

    /// @brief True if this overrides a parent method.
    bool isOverride = false;

    /// @brief True if this is a static (type-level) method.
    bool isStatic = false;

    /// @brief Construct a method declaration.
    /// @param l Source location.
    /// @param n Method name.
    MethodDecl(SourceLoc l, std::string n) : Decl(DeclKind::Method, l), name(std::move(n)) {}
};

/// @brief Property declaration with computed getter and optional setter.
/// @details Declares a property with explicit get/set accessors.
///
/// ## Example
/// ```
/// class Circle {
///     expose radius: Number;
///     property area: Number {
///         get { return 3.14159 * self.radius * self.radius; }
///     }
/// }
/// ```
struct PropertyDecl : Decl {
    /// @brief Property name.
    std::string name;

    /// @brief Property type.
    TypePtr type;

    /// @brief Getter body (nullptr if write-only).
    StmtPtr getterBody;

    /// @brief Setter body (nullptr if read-only).
    StmtPtr setterBody;

    /// @brief Setter parameter name (defaults to "value").
    std::string setterParam = "value";

    /// @brief Property visibility.
    Visibility visibility = Visibility::Private;

    /// @brief True if this is a static property.
    bool isStatic = false;

    /// @brief Construct a property declaration.
    /// @param l Source location.
    /// @param n Property name.
    PropertyDecl(SourceLoc l, std::string n) : Decl(DeclKind::Property, l), name(std::move(n)) {}
};

/// @brief Destructor declaration for class types.
/// @details Defines cleanup code that runs when a class instance is destroyed.
/// At most one destructor is allowed per class. The lowerer synthesizes a
/// `__dtor_TypeName` IL function that runs the user body, then releases
/// reference-typed fields.
///
/// ## Example
/// ```
/// class Connection {
///     expose String host;
///     deinit {
///         // cleanup resources
///     }
/// }
/// ```
struct DestructorDecl : Decl {
    /// @brief Destructor body.
    StmtPtr body;

    /// @brief Construct a destructor declaration.
    /// @param l Source location.
    DestructorDecl(SourceLoc l) : Decl(DeclKind::Destructor, l) {}
};

/// @brief Constructor declaration for class types.
/// @details Defines how to initialize a new instance of an class type.
///
/// ## Example
/// ```
/// class Player {
///     new(name: String, health: Integer) {
///         self.name = name;
///         self.health = health;
///     }
/// }
/// ```
struct ConstructorDecl : Decl {
    /// @brief Constructor parameters.
    std::vector<Param> params;

    /// @brief Constructor body.
    StmtPtr body;

    /// @brief Constructor visibility.
    Visibility visibility = Visibility::Public;

    /// @brief Construct a constructor declaration.
    /// @param l Source location.
    ConstructorDecl(SourceLoc l) : Decl(DeclKind::Constructor, l) {}
};

/// @brief Module-level variable declaration.
/// @details Defines a global variable accessible throughout the module.
///
/// ## Example
/// ```
/// var globalCounter: Integer = 0;
/// final MAX_SIZE = 100;
/// ```
struct GlobalVarDecl : Decl {
    /// @brief Variable name.
    std::string name;

    /// @brief Variable type (nullptr = inferred).
    TypePtr type;

    /// @brief Initializer expression (nullptr = default).
    ExprPtr initializer;

    /// @brief True if immutable.
    bool isFinal = false;

    /// @brief Construct a global variable declaration.
    /// @param l Source location.
    /// @param n Variable name.
    GlobalVarDecl(SourceLoc l, std::string n) : Decl(DeclKind::GlobalVar, l), name(std::move(n)) {}
};

/// @brief Struct type declaration (copy semantics).
/// @details Defines a struct type with copy-on-assignment semantics.
/// Struct types are passed by value and have no identity.
///
/// ## Example
/// ```
/// struct Point {
///     expose x: Number;
///     expose y: Number;
///
///     func distance(other: Point) -> Number { ... }
/// }
/// ```
struct StructDecl : Decl {
    /// @brief Type name.
    std::string name;

    /// @brief Generic type parameter names.
    std::vector<std::string> genericParams;

    /// @brief Optional constraints for generic type parameters.
    std::vector<std::string> genericParamConstraints;

    /// @brief Implemented interface names.
    std::vector<std::string> interfaces;

    /// @brief Member declarations (fields and methods).
    std::vector<DeclPtr> members;

    /// @brief Construct a struct type declaration.
    /// @param l Source location.
    /// @param n Type name.
    StructDecl(SourceLoc l, std::string n) : Decl(DeclKind::Struct, l), name(std::move(n)) {}
};

/// @brief Class type declaration (reference semantics).
/// @details Defines a class type with reference semantics and identity.
/// Class types are heap-allocated and passed by reference.
///
/// ## Example
/// ```
/// class Player extends Character implements Moveable {
///     hide health: Integer;
///     expose name: String;
///
///     new(name: String) {
///         super.new();
///         self.name = name;
///         self.health = 100;
///     }
/// }
/// ```
struct ClassDecl : Decl {
    /// @brief Type name.
    std::string name;

    /// @brief Generic type parameter names.
    std::vector<std::string> genericParams;

    /// @brief Optional constraints for generic type parameters.
    std::vector<std::string> genericParamConstraints;

    /// @brief Parent class name (empty = no inheritance).
    std::string baseClass;

    /// @brief Implemented interface names.
    std::vector<std::string> interfaces;

    /// @brief Member declarations (fields, methods, constructor).
    std::vector<DeclPtr> members;

    /// @brief Construct a class type declaration.
    /// @param l Source location.
    /// @param n Type name.
    ClassDecl(SourceLoc l, std::string n) : Decl(DeclKind::Class, l), name(std::move(n)) {}
};

/// @brief Interface declaration (abstract type contract).
/// @details Defines an interface that struct and class types can implement.
/// Interfaces declare method signatures without implementations.
///
/// ## Example
/// ```
/// interface Drawable {
///     func draw(canvas: Canvas);
///     func getBounds() -> Rect;
/// }
/// ```
struct InterfaceDecl : Decl {
    /// @brief Interface name.
    std::string name;

    /// @brief Generic type parameter names.
    std::vector<std::string> genericParams;

    /// @brief Optional constraints for generic type parameters.
    std::vector<std::string> genericParamConstraints;

    /// @brief Method signature declarations. Methods may include default bodies.
    std::vector<DeclPtr> members;

    /// @brief Construct an interface declaration.
    /// @param l Source location.
    /// @param n Interface name.
    InterfaceDecl(SourceLoc l, std::string n) : Decl(DeclKind::Interface, l), name(std::move(n)) {}
};

/// @brief Bind declaration: brings external modules or namespaces into scope.
/// @details Binds make code from other modules or the Zanna runtime namespace
/// available in the current module. Supports both file binds and namespace binds.
///
/// ## File Binds (import Zia source files)
/// - `bind "./utils";` - Relative path to another .zia file
/// - `bind "../lib/helpers";` - Parent directory path
/// - `bind "./colors" as C;` - With alias for qualified access
///
/// ## Namespace Binds (import Zanna runtime namespaces)
/// - `bind Zanna.Terminal;` - Import all symbols from namespace
/// - `bind Zanna.Graphics as G;` - With alias: G.Canvas, G.Sprite
/// - `bind Zanna.Terminal { Say, ReadLine };` - Import specific symbols only
struct BindDecl : Decl {
    /// @brief The bind path (file path OR namespace like "Zanna.Terminal").
    std::string path;

    /// @brief Bind alias (empty if no alias).
    std::string alias;

    /// @brief True if this is a runtime namespace bind, false for file bind.
    /// @details Namespace binds start with "Zanna." and don't use string literals.
    /// File binds use string literals like "./module" or "../lib/utils".
    bool isNamespaceBind = false;

    /// @brief Specific items to import (empty = import all).
    /// @details Supports selective imports for both file binds and namespace binds:
    /// `bind Zanna.Terminal { Say, ReadLine };`
    /// `bind "./math" { square, PI };`
    std::vector<std::string> specificItems;

    /// @brief Resolved imported-file id for file binds, or 0 when unresolved.
    /// @details Populated by ImportResolver after the bound file is parsed.
    uint32_t resolvedFileId = 0;

    /// @brief Declared module name of the imported file, if known.
    /// @details For file binds this preserves the imported file's `module Foo;`
    /// name so qualified access uses the actual module identity rather than the
    /// source filename. Alias binds still override this name at use sites.
    std::string resolvedModuleName;

    /// @brief Construct a bind declaration.
    /// @param l Source location.
    /// @param p The bind path (file or namespace).
    BindDecl(SourceLoc l, std::string p) : Decl(DeclKind::Bind, l), path(std::move(p)) {}
};

/// @brief Namespace declaration: groups declarations under a qualified name.
/// @details Namespaces provide hierarchical organization and prevent name collisions.
/// Declarations inside a namespace are accessed via qualified names (e.g., `MyLib.Foo`).
/// Namespaces can be nested and can span multiple files via imports.
///
/// The built-in `Zanna.*` namespaces (Zanna.Terminal, Zanna.Math, etc.) use the
/// same mechanism as user-defined namespaces - there is no special casing.
///
/// ## Example
/// ```
/// namespace MyLib {
///     class Parser { ... }
///     func parse(s: String) -> Result { ... }
/// }
///
/// // Nested namespaces
/// namespace MyLib.Internal {
///     func helper() { ... }
/// }
/// ```
///
/// ## Access
/// ```
/// var p = new MyLib.Parser();
/// var r = MyLib.parse("input");
/// ```
struct NamespaceDecl : Decl {
    /// @brief Namespace name (can be dotted, e.g., "MyLib.Internal").
    std::string name;

    /// @brief Declarations within this namespace.
    std::vector<DeclPtr> declarations;

    /// @brief Construct a namespace declaration.
    /// @param l Source location.
    /// @param n Namespace name.
    NamespaceDecl(SourceLoc l, std::string n) : Decl(DeclKind::Namespace, l), name(std::move(n)) {}
};

/// @brief A single variant in an enum declaration.
/// @details Represents one named constant in an enum, with an optional
/// explicit integer value. When no value is given, the variant auto-increments
/// from the previous variant's value (starting at 0).
///
/// ## Example
/// ```
/// enum Color {
///     Red,          // value 0
///     Green,        // value 1
///     Blue = 10,    // value 10
///     Alpha,        // value 11
/// }
/// ```
struct EnumVariant {
    /// @brief Variant name (e.g., "Red", "Green").
    std::string name;

    /// @brief Explicit integer value, if provided (e.g., `Blue = 10`).
    /// @details When nullopt, the value is auto-assigned by the semantic analyzer.
    std::optional<int64_t> explicitValue;

    /// @brief Source location of the variant name.
    SourceLoc loc;
};

/// @brief Enum type declaration: a set of named integer constants.
/// @details Defines an enum type where each variant is a distinct named value.
/// Enum types are distinct from Integer — you cannot assign an Integer to an
/// enum variable without conversion. At the IL level, enum variants lower to
/// I64 constants.
///
/// ## Example
/// ```
/// enum Direction {
///     North,
///     South,
///     East,
///     West,
/// }
///
/// var d: Direction = Direction.North;
/// ```
struct EnumDecl : Decl {
    /// @brief Enum type name.
    std::string name;

    /// @brief Enum variants (ordered as declared).
    std::vector<EnumVariant> variants;

    /// @brief Enum visibility.
    Visibility visibility = Visibility::Private;

    /// @brief Construct an enum declaration.
    /// @param l Source location.
    /// @param n Enum name.
    EnumDecl(SourceLoc l, std::string n) : Decl(DeclKind::Enum, l), name(std::move(n)) {}
};

/// @brief Type alias declaration: `type Name = TargetType;`
/// @details Creates a compile-time alias that resolves to the target type
/// during semantic analysis. No runtime representation.
struct TypeAliasDecl : Decl {
    /// @brief Alias name.
    std::string name;

    /// @brief Target type the alias resolves to.
    TypePtr targetType;

    /// @brief Construct a compile-time type alias.
    /// @param l Source location of the alias declaration.
    /// @param n Alias name.
    /// @param t Owned target type annotation.
    TypeAliasDecl(SourceLoc l, std::string n, TypePtr t)
        : Decl(DeclKind::TypeAlias, l), name(std::move(n)), targetType(std::move(t)) {}
};

/// @brief Module declaration: the top-level compilation unit.
/// @details Represents an entire source file as a module with a name,
/// binds, and top-level declarations.
///
/// ## Example
/// ```
/// module MyGame;
///
/// bind Zanna.Terminal as Term;
///
/// class Player { ... }
///
/// func main() { ... }
/// ```
struct ModuleDecl : Decl {
    /// @brief Module name (from `module MyName;` declaration).
    std::string name;

    /// @brief Bind declarations.
    std::vector<BindDecl> binds;

    /// @brief Top-level declarations (types, functions, global vars).
    std::vector<DeclPtr> declarations;

    /// @brief Construct a module declaration.
    /// @param l Source location.
    /// @param n Module name.
    ModuleDecl(SourceLoc l, std::string n) : Decl(DeclKind::Module, l), name(std::move(n)) {}
};

/// @}

} // namespace il::frontends::zia
