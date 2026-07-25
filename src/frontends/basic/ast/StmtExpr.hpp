//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/StmtExpr.hpp
// Purpose: Defines BASIC expression-oriented, variable, terminal, file-channel,
//          input/output, allocation, and object-lifetime statement nodes.
// Key invariants:
//   - Each stmtKind override agrees with its concrete statement type.
//   - Required expression children are non-null after successful parsing.
//   - Argument, dimension, item, and target vectors preserve source order.
// Ownership/Lifetime:
//   - Statements uniquely own child expressions through ExprPtr/LValuePtr.
//   - Visitors borrow nodes only during accept dispatch.
// Links: src/frontends/basic/ast/StmtBase.hpp,
//        src/frontends/basic/ast/ExprNodes.hpp,
//        src/frontends/basic/AST.cpp,
//        src/frontends/basic/Parser_Stmt_IO.cpp,
//        src/frontends/basic/Parser_Stmt_Core.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/ExprNodes.hpp"
#include "frontends/basic/ast/StmtBase.hpp"

#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief Defines expression, declaration, terminal, and file-I/O statements.

namespace il::frontends::basic {

/// @brief Item within a PRINT statement.
struct PrintItem {
    /// Kind of item to output.
    enum class Kind {
        Expr,      ///< Expression to print.
        Comma,     ///< Insert a space.
        Semicolon, ///< Insert nothing.
    } kind{Kind::Expr};

    /// Expression value when @ref kind == Kind::Expr; owned.
    ExprPtr expr;
};

/// @brief PRINT statement outputting a sequence of expressions and separators.
/// Trailing semicolon suppresses the automatic newline.
/// @invariant items.size() > 0
struct PrintStmt : Stmt {
    /// @brief Returns this node's PRINT discriminator.
    /// @return @ref Kind::Print.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Print;
    }

    /// Items printed in order; unless the last item is a semicolon, a newline is appended.
    std::vector<PrintItem> items;

    /// @brief Dispatches this PRINT statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this PRINT statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief PRINT # statement that outputs to a file channel.
struct PrintChStmt : Stmt {
    /// @brief Returns this node's channel-print discriminator.
    /// @return @ref Kind::PrintCh.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::PrintCh;
    }

    /// @brief Output formatting mode for channel writes.
    enum class Mode {
        /// Apply PRINT # formatting rules.
        Print,
        /// Apply WRITE # serialization rules.
        Write,
    } mode{Mode::Print};
    /// Channel expression evaluated to select the file handle; owned and non-null.
    ExprPtr channelExpr;

    /// Expressions printed to the channel, separated by commas in source.
    std::vector<ExprPtr> args;

    /// True when a trailing newline should be emitted after printing.
    bool trailingNewline{true};

    /// @brief Dispatches this channel output to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this channel output to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief BEEP statement emitting a bell/beep sound.
struct BeepStmt : Stmt {
    /// @brief Returns this node's BEEP discriminator.
    /// @return @ref Kind::Beep.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Beep;
    }

    /// @brief Dispatches this BEEP statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this BEEP statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief CALL statement invoking a user-defined SUB or instance method.
struct CallStmt : Stmt {
    /// @brief Returns this node's CALL discriminator.
    /// @return @ref Kind::Call.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Call;
    }

    /// Invocation expression with side effects (SUB or instance method).
    /// May be a CallExpr or a MethodCallExpr.
    ExprPtr call;

    /// @brief Dispatches this CALL statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this CALL statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief CLS statement clearing the screen.
struct ClsStmt : Stmt {
    /// @brief Returns this node's CLS discriminator.
    /// @return @ref Kind::Cls.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Cls;
    }

    /// @brief Dispatches this CLS statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this CLS statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief COLOR statement changing the palette.
struct ColorStmt : Stmt {
    /// @brief Returns this node's COLOR discriminator.
    /// @return @ref Kind::Color.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Color;
    }

    /// Foreground color expression; may be null when omitted.
    ExprPtr fg;

    /// Background color expression; may be null when omitted.
    ExprPtr bg;

    /// @brief Dispatches this COLOR statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this COLOR statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief SLEEP statement blocking for a duration in milliseconds.
struct SleepStmt : Stmt {
    /// @brief Returns this node's SLEEP discriminator.
    /// @return @ref Kind::Sleep.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Sleep;
    }

    /// Millisecond duration expression; owned and non-null.
    ExprPtr ms;

    /// @brief Dispatches this SLEEP statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this SLEEP statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief LOCATE statement moving the cursor.
struct LocateStmt : Stmt {
    /// @brief Returns this node's LOCATE discriminator.
    /// @return @ref Kind::Locate.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Locate;
    }

    /// Row expression (1-based); owned and non-null.
    ExprPtr row;

    /// Column expression (1-based); owned and non-null.
    ExprPtr col;

    /// Optional cursor visibility expression.
    ExprPtr cursor;

    /// Optional start scan line expression.
    ExprPtr start;

    /// Optional stop scan line expression.
    ExprPtr stop;

    /// @brief Dispatches this LOCATE statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this LOCATE statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief CURSOR statement controlling cursor visibility.
struct CursorStmt : Stmt {
    /// @brief Returns this node's CURSOR discriminator.
    /// @return @ref Kind::Cursor.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Cursor;
    }

    /// True for CURSOR ON, false for CURSOR OFF.
    bool visible{true};

    /// @brief Dispatches this CURSOR statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this CURSOR statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief ALTSCREEN statement controlling alternate screen buffer.
struct AltScreenStmt : Stmt {
    /// @brief Returns this node's ALTSCREEN discriminator.
    /// @return @ref Kind::AltScreen.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::AltScreen;
    }

    /// True for ALTSCREEN ON, false for ALTSCREEN OFF.
    bool enable{true};

    /// @brief Dispatches this ALTSCREEN statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this ALTSCREEN statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief LET statement assigning to an lvalue.
struct LetStmt : Stmt {
    /// @brief Returns this node's LET discriminator.
    /// @return @ref Kind::Let.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Let;
    }

    /// Left-hand side receiving the assignment; owned and non-null.
    LValuePtr target;

    /// Right-hand side expression; owned and non-null.
    ExprPtr expr;

    /// @brief Dispatches this assignment to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this assignment to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief CONST statement declaring a constant.
/// @invariant Initializer expression must be non-null.
struct ConstStmt : Stmt {
    /// @brief Returns this node's CONST discriminator.
    /// @return @ref Kind::Const.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Const;
    }

    /// Constant name being declared.
    std::string name;

    /// Initializer expression; owned and non-null.
    ExprPtr initializer;

    /// Declared BASIC type for this constant.
    Type type{Type::I64};

    /// @brief Dispatches this constant declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this constant declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief DIM statement declaring a variable or array.
struct DimStmt : Stmt {
    /// @brief Returns this node's DIM discriminator.
    /// @return @ref Kind::Dim.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Dim;
    }

    /// Array name being declared.
    std::string name;

    /// Number of elements to allocate when @ref isArray is true; may be null for scalars.
    /// For multi-dimensional arrays, use @ref dimensions instead.
    /// @deprecated Use @ref dimensions for multi-dimensional support.
    ExprPtr size;

    /// Dimension sizes for multi-dimensional arrays (owned).
    /// For single-dimensional arrays, this contains one element (backward compatible).
    /// Empty for scalar declarations.
    std::vector<ExprPtr> dimensions;

    /// Declared BASIC type for this DIM.
    Type type{Type::I64};

    /// Optional explicit class return type from "AS <Class>" for object variables.
    /// Stored as qualified, canonical lowercase name segments when present.
    std::vector<std::string> explicitClassQname;

    /// True when DIM declares an array; false for scalar declarations.
    bool isArray{true};

    /// Resolved array extents from semantic analysis.
    /// @details Stored during semantic analysis so the lowerer can compute correct
    ///          array allocation size even when dimension expressions reference CONSTs
    ///          that would otherwise need runtime lookup. Empty if dimensions are
    ///          not all compile-time constants.
    mutable std::vector<long long> resolvedExtents;

    /// @brief Dispatches this declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief STATIC statement declaring a persistent procedure-local variable.
struct StaticStmt : Stmt {
    /// @brief Returns this node's STATIC discriminator.
    /// @return @ref Kind::Static.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Static;
    }

    /// Variable name being declared.
    std::string name;

    /// Declared BASIC type for this STATIC variable.
    Type type{Type::I64};

    /// @brief Dispatches this declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief SHARED statement declaring that names refer to module-level storage.
/// @details Classic BASIC uses SHARED within procedures to indicate that listed
///          variables refer to module-level bindings. In this implementation the
///          analyser already allows accessing module-level variables from
///          procedures, so this statement is effectively a no-op and primarily
///          exists for compatibility and better diagnostics.
struct SharedStmt : Stmt {
    std::vector<std::string> names; ///< Names listed in the SHARED statement.

    /// @brief Returns this node's SHARED discriminator.
    /// @return @ref Kind::Shared.
    [[nodiscard]] Kind stmtKind() const noexcept override {
        return Kind::Shared;
    }

    /// @brief Dispatches this declaration to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override {
        visitor.visit(*this);
    }

    /// @brief Dispatches this declaration to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override {
        visitor.visit(*this);
    }
};

/// @brief REDIM statement resizing an existing array.
struct ReDimStmt : Stmt {
    /// @brief Returns this node's REDIM discriminator.
    /// @return @ref Kind::ReDim.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::ReDim;
    }

    /// Array name whose storage is being reallocated.
    std::string name;

    /// Inclusive upper bound for single-dimensional arrays.
    ExprPtr size;

    /// Inclusive upper bounds for multidimensional arrays.
    std::vector<ExprPtr> dimensions;

    /// @brief Dispatches this resize to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this resize to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief SWAP statement for exchanging values of two variables.
struct SwapStmt : Stmt {
    /// @brief Returns this node's SWAP discriminator.
    /// @return @ref Kind::Swap.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Swap;
    }

    /// First variable to swap.
    LValuePtr lhs;

    /// Second variable to swap.
    LValuePtr rhs;

    /// @brief Dispatches this exchange to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this exchange to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief RANDOMIZE statement seeding the pseudo-random generator.
struct RandomizeStmt : Stmt {
    /// @brief Returns this node's RANDOMIZE discriminator.
    /// @return @ref Kind::Randomize.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Randomize;
    }

    /// Optional numeric seed expression, truncated to i64 when present.
    ExprPtr seed;

    /// @brief Dispatches this seeding statement to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this seeding statement to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief OPEN statement configuring a file channel.
struct OpenStmt : Stmt {
    /// @brief Returns this node's OPEN discriminator.
    /// @return @ref Kind::Open.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Open;
    }

    /// File path expression; owned and non-null.
    ExprPtr pathExpr;

    /// File mode keyword.
    enum class Mode {
        /// Open an existing file for sequential reads.
        Input,
        /// Create or truncate a file for sequential writes.
        Output,
        /// Open or create a file and write after its existing contents.
        Append,
        /// Open a file for byte-oriented access.
        Binary,
        /// Open a file for random-access records.
        Random,
    } mode{Mode::Input};

    /// File number expression; owned and non-null.
    ExprPtr channelExpr;

    /// @brief Dispatches this channel-open operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this channel-open operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief CLOSE statement closing a file channel.
struct CloseStmt : Stmt {
    /// @brief Returns this node's CLOSE discriminator.
    /// @return @ref Kind::Close.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Close;
    }

    /// Optional file channel expression; null closes all.
    ExprPtr channelExpr;

    /// @brief Dispatches this channel-close operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this channel-close operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief SEEK statement moving a file position.
struct SeekStmt : Stmt {
    /// @brief Returns this node's SEEK discriminator.
    /// @return @ref Kind::Seek.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Seek;
    }

    /// File channel expression; owned and non-null.
    ExprPtr channelExpr;

    /// Absolute file position expression.
    ExprPtr positionExpr;

    /// @brief Dispatches this seek operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this seek operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief INPUT statement to read from stdin into a variable, optionally displaying a prompt.
struct InputStmt : Stmt {
    /// @brief Returns this node's console-input discriminator.
    /// @return @ref Kind::Input.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Input;
    }

    /// Optional prompt string literal (nullptr if absent).
    ExprPtr prompt;

    /// Target variable names (each may end with '$').
    std::vector<std::string> vars;

    /// @brief Dispatches this console-input operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this console-input operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief Reference to a BASIC identifier together with its source location.
struct NameRef {
    /// Identifier text, including optional type suffix.
    Identifier name;

    /// Source location where the identifier appeared.
    il::support::SourceLoc loc{};
};

/// @brief INPUT # statement reading a field from a file channel.
struct InputChStmt : Stmt {
    /// @brief Returns this node's channel-input discriminator.
    /// @return @ref Kind::InputCh.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::InputCh;
    }

    /// Numeric file channel identifier following '#'.
    int channel{0};

    /// One or more destination variables receiving parsed fields.
    std::vector<NameRef> targets;

    /// @brief Dispatches this channel-input operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this channel-input operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief LINE INPUT # statement reading an entire line from a file channel.
struct LineInputChStmt : Stmt {
    /// @brief Returns this node's line-input discriminator.
    /// @return @ref Kind::LineInputCh.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::LineInputCh;
    }

    /// Channel expression evaluated to select the file handle; owned and non-null.
    ExprPtr channelExpr;

    /// Destination lvalue that receives the read line.
    LValuePtr targetVar;

    /// @brief Dispatches this line-input operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this line-input operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

/// @brief DELETE statement releasing an object reference.
struct DeleteStmt : Stmt {
    /// @brief Returns this node's DELETE discriminator.
    /// @return @ref Kind::Delete.
    [[nodiscard]] constexpr Kind stmtKind() const noexcept override {
        return Kind::Delete;
    }

    /// Expression evaluating to the instance to delete.
    ExprPtr target;

    /// @brief Dispatches this object-release operation to an immutable visitor.
    /// @param visitor Visitor receiving @ref StmtVisitor::visit.
    void accept(StmtVisitor &visitor) const override;

    /// @brief Dispatches this object-release operation to a mutable visitor.
    /// @param visitor Visitor receiving @ref MutStmtVisitor::visit.
    void accept(MutStmtVisitor &visitor) override;
};

} // namespace il::frontends::basic
