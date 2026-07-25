//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/StmtDecl.hpp
// Purpose: Defines BASIC procedure, object/type, enum, namespace, import, and
//          statement-list declaration nodes.
// Key invariants:
//   - Each stmtKind override agrees with its concrete declaration type.
//   - Parameter, field, member, and body vectors preserve declaration order.
//   - Qualified-name representations retain canonical segments alongside
//     compatibility spellings where documented.
// Ownership/Lifetime:
//   - Declaration nodes own nested statements through StmtPtr vectors and own
//     all names and metadata by value.
//   - Visitors borrow declarations only during accept dispatch.
// Links: src/frontends/basic/ast/StmtBase.hpp,
//        src/frontends/basic/BasicTypes.hpp,
//        src/frontends/basic/AST.cpp,
//        src/frontends/basic/Parser_Stmt_OOP.cpp,
//        src/frontends/basic/Parser_Stmt_Core.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/BasicTypes.hpp"
#include "frontends/basic/ast/StmtBase.hpp"

#include <optional>
#include <string>
#include <vector>

/// @file
/// @brief Defines BASIC declaration and statement-list AST nodes.

namespace il::frontends::basic {

/// @brief Parameter in FUNCTION or SUB declaration.
struct Param {
    /// Parameter name including optional suffix.
    Identifier name;

    /// Resolved primitive type from suffix or explicit type syntax.
    Type type{Type::I64};

    /// True if parameter declared with ().
    bool is_array{false};

    /// True if parameter declared BYREF (pass-by-reference).
    bool isByRef{false};

    /// Source location of the parameter name.
    il::support::SourceLoc loc{};

    /// Qualified class name for object-typed parameters; empty for primitives.
    std::string objectClass;
};

/// @brief FUNCTION declaration with optional parameters and return type.
struct FunctionDecl : Stmt {
    /// @brief Returns this node's FUNCTION declaration discriminator.
    /// @return @ref Kind::FunctionDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::FunctionDecl;
    }

    /// Function name including suffix.
    Identifier name;

    /// Qualified namespace path segments for this procedure.
    /// Example: for A.B.C.Foo, path = {"A","B","C"}.
    std::vector<std::string> namespacePath;

    /// Canonical, fully-qualified name for this procedure (dot-joined).
    /// Example: "a.b.c.foo" (lowercased for case-insensitive language).
    std::string qualifiedName;

    /// Return type derived from name suffix.
    Type ret{Type::I64};

    /// Optional explicit return type from "AS <TYPE>".
    /// For FUNCTION without AS, keep at BasicType::Unknown.
    BasicType explicitRetType{BasicType::Unknown};

    /// Optional explicit class return type from "AS <Class>".
    /// Stored as a qualified, canonical lowercase name when present.
    std::vector<std::string> explicitClassRetQname;

    /// Ordered parameter list.
    std::vector<Param> params;

    /// Function body statements.
    std::vector<StmtPtr> body;

    /// True if this function is exported (visible to other modules).
    bool isExport = false;

    /// True if this is a foreign function (imported from another module, no body).
    bool isForeign = false;

    /// Location of trailing END FUNCTION keyword.
    il::support::SourceLoc endLoc{};

    /// @brief Dispatches this function declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this function declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief SUB declaration representing a void procedure.
struct SubDecl : Stmt {
    /// @brief Returns this node's SUB declaration discriminator.
    /// @return @ref Kind::SubDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::SubDecl;
    }

    /// Subroutine name including suffix.
    Identifier name;

    /// Qualified namespace path segments for this procedure.
    /// Example: for A.B.C.Bar, path = {"A","B","C"}.
    std::vector<std::string> namespacePath;

    /// Canonical, fully-qualified name for this procedure (dot-joined).
    /// Example: "a.b.c.bar" (lowercased for case-insensitive language).
    std::string qualifiedName;

    /// Ordered parameter list.
    std::vector<Param> params;

    /// Body statements.
    std::vector<StmtPtr> body;

    /// True if this sub is exported (visible to other modules).
    bool isExport = false;

    /// True if this is a foreign sub (imported from another module, no body).
    bool isForeign = false;

    /// @brief Dispatches this subroutine declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this subroutine declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief Sequence of statements executed left-to-right on one BASIC line.
struct StmtList : Stmt {
    /// @brief Returns this node's statement-list discriminator.
    /// @return @ref Kind::StmtList.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::StmtList;
    }

    /// Ordered statements sharing the same line.
    std::vector<StmtPtr> stmts;

    /// @brief Dispatches this statement list to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this statement list to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief Constructor declaration for a CLASS.
struct ConstructorDecl : Stmt {
    /// @brief Returns this node's constructor declaration discriminator.
    /// @return @ref Kind::ConstructorDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::ConstructorDecl;
    }

    /// Access specifier (PUBLIC/PRIVATE); defaults to PUBLIC.
    Access access{Access::Public};

    /// Static flag: true for a static constructor (type initializer).
    /// Ignored for instance constructors; used later for static ctor (D4).
    bool isStatic{false};

    /// Ordered parameters for the constructor.
    std::vector<Param> params;

    /// Statements forming the constructor body.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this constructor to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this constructor to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief Destructor declaration for a CLASS.
struct DestructorDecl : Stmt {
    /// @brief Returns this node's destructor declaration discriminator.
    /// @return @ref Kind::DestructorDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::DestructorDecl;
    }

    /// Access specifier (PUBLIC/PRIVATE); defaults to PUBLIC.
    Access access{Access::Public};

    /// Statements forming the destructor body.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this destructor to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this destructor to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief Method declaration inside a CLASS.
struct MethodDecl : Stmt {
    /// @brief Returns this node's method declaration discriminator.
    /// @return @ref Kind::MethodDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::MethodDecl;
    }

    /// Method name.
    std::string name;

    /// Access specifier (PUBLIC/PRIVATE); defaults to PUBLIC.
    Access access{Access::Public};

    /// True if declared STATIC; static methods do not receive an implicit ME receiver.
    bool isStatic{false};

    /// Ordered parameters for the method.
    std::vector<Param> params;

    /// Optional return type when method yields a value.
    std::optional<Type> ret;

    /// Optional explicit class return type from "AS <Class>".
    /// Stored as a qualified, canonical lowercase name when present.
    /// Empty vector indicates primitive or no explicit return type.
    std::vector<std::string> explicitClassRetQname;

    /// True when the method introduces or participates in virtual dispatch.
    bool isVirtual{false};

    /// True when the method explicitly overrides an inherited slot.
    bool isOverride{false};

    /// True when the declaration is abstract and requires a derived implementation.
    bool isAbstract{false};

    /// True when derived classes may not override this method.
    bool isFinal{false};

    /// Statements forming the method body.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this method declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this method declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief CLASS declaration grouping fields and members.
struct ClassDecl : Stmt {
    /// @brief Returns this node's CLASS declaration discriminator.
    /// @return @ref Kind::ClassDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::ClassDecl;
    }

    /// Class name.
    std::string name;

    /// Qualified namespace path segments for this type.
    /// Example: for A.B.C.Point, path = {"A","B","C"}.
    std::vector<std::string> namespacePath;

    /// Canonical, fully-qualified name for this type (dot-joined).
    /// Example: "a.b.c.point" (lowercased for case-insensitive language).
    std::string qualifiedName;

    /// Optional base class name (bare or qualified). Resolution happens in semantic analysis.
    std::optional<std::string> baseName;

    /// Class-level modifiers.
    bool isAbstract{false};
    bool isFinal{false};

    /// Field definition within the class.
    struct Field {
        /// Source-level field name.
        std::string name;

        /// Primitive semantic type; object identity is carried separately.
        Type type{Type::I64};

        /// Access specifier (PUBLIC/PRIVATE); defaults to PUBLIC.
        Access access{Access::Public};

        /// True if field is declared STATIC.
        bool isStatic{false};

        /// Whether this field is an array.
        bool isArray{false};

        /// Array dimension extents when @ref isArray is true.
        std::vector<long long> arrayExtents;

        /// Qualified class name for object-typed fields; empty for primitives.
        std::string objectClassName;
    };

    /// Ordered fields declared on the class.
    std::vector<Field> fields;

    /// Members declared within the class (constructors, destructors, methods).
    std::vector<StmtPtr> members;

    /// Interfaces implemented by this class, each as dotted qualified segments.
    /// Example: implements A.B.I -> { {"A","B","I"} }
    std::vector<std::vector<std::string>> implementsQualifiedNames;

    /// @brief Dispatches this class declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this class declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief PROPERTY declaration inside a CLASS, with optional GET/SET bodies.
struct PropertyDecl : Stmt {
    /// @brief Returns this node's property declaration discriminator.
    /// @return @ref Kind::PropertyDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::PropertyDecl;
    }

    /// Filled by the lowerer: fully-qualified class path segments.
    std::vector<std::string> qualifiedClass;

    /// Property simple name.
    std::string name;

    /// Declared type of the property.
    Type type{Type::I64};

    /// True if declared STATIC; static properties have no receiver.
    bool isStatic{false};

    /// Overall visibility of the property head.
    Access access{Access::Public};

    /// @brief Optional property getter body and visibility.
    struct Getter {
        /// Getter visibility, which may not exceed property visibility.
        Access access{Access::Public};

        /// Statements forming the getter implementation.
        std::vector<StmtPtr> body;

        /// Whether a GET accessor was present in source.
        bool present{false};
    } get;

    /// @brief Optional property setter body, parameter, and visibility.
    struct Setter {
        /// Setter visibility, which may not exceed property visibility.
        Access access{Access::Public};

        /// Source-level name bound to the incoming property value.
        std::string paramName{"value"};

        /// Statements forming the setter implementation.
        std::vector<StmtPtr> body;

        /// Whether a SET accessor was present in source.
        bool present{false};
    } set;

    /// @brief Dispatches this property declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this property declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief TYPE declaration defining a structured record type.
struct TypeDecl : Stmt {
    /// @brief Returns this node's TYPE declaration discriminator.
    /// @return @ref Kind::TypeDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::TypeDecl;
    }

    /// Type name.
    std::string name;

    /// Field definition within the type.
    struct Field {
        /// Source-level field name.
        std::string name;

        /// Primitive semantic type of the field.
        Type type{Type::I64};

        /// Access specifier (PUBLIC/PRIVATE); defaults to PUBLIC.
        Access access{Access::Public};
    };

    /// Ordered fields declared on the type.
    std::vector<Field> fields;

    /// @brief Dispatches this TYPE declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this TYPE declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief ENUM declaration defining a set of named integer constants.
struct EnumDecl : Stmt {
    /// @brief Returns this node's ENUM declaration discriminator.
    /// @return @ref Kind::EnumDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::EnumDecl;
    }

    /// Enum type name.
    std::string name;

    /// Individual enum member with an optional explicit integer value.
    struct Member {
        /// Member identifier.
        std::string name;

        /// Explicit value, or nullopt for sequential inference.
        std::optional<long long> value;
    };

    /// Ordered members declared in the enum.
    std::vector<Member> members;

    /// @brief Dispatches this enum declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override {
        visitor.visit(*this);
    }

    /// @brief Dispatches this enum declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override {
        visitor.visit(*this);
    }
};

/// @brief NAMESPACE declaration grouping declarations under a qualified path.
struct NamespaceDecl : Stmt {
    /// @brief Returns this node's NAMESPACE declaration discriminator.
    /// @return @ref Kind::NamespaceDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::NamespaceDecl;
    }

    /// Qualified namespace path segments in declaration order.
    std::vector<std::string> path;

    /// Declarations/body within the namespace.
    std::vector<StmtPtr> body;

    /// @brief Dispatches this namespace declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override {
        visitor.visit(*this);
    }

    /// @brief Dispatches this namespace declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override {
        visitor.visit(*this);
    }
};

/// @brief INTERFACE declaration grouping abstract member signatures.
struct InterfaceDecl : Stmt {
    /// @brief Returns this node's INTERFACE declaration discriminator.
    /// @return @ref Kind::InterfaceDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::InterfaceDecl;
    }

    /// Qualified interface name segments, e.g. ["A","B","I"].
    std::vector<std::string> qualifiedName;

    /// Abstract members (method signatures only) declared inside the interface.
    std::vector<StmtPtr> members;

    /// @brief Dispatches this interface declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this interface declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief USING directive importing a namespace at file scope.
struct UsingDecl : Stmt {
    /// @brief Returns this node's USING declaration discriminator.
    /// @return @ref Kind::UsingDecl.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::UsingDecl;
    }

    /// Namespace path segments, e.g. ["Foo","Bar","Baz"] for USING Foo.Bar.Baz.
    std::vector<std::string> namespacePath;

    /// Optional alias for the imported namespace; empty if no AS clause present.
    std::string alias;

    /// @brief Dispatches this import declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override {
        visitor.visit(*this);
    }

    /// @brief Dispatches this import declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override {
        visitor.visit(*this);
    }
};

} // namespace il::frontends::basic
