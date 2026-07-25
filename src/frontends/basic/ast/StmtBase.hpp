//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/StmtBase.hpp
// Purpose: Declares the BASIC statement discriminator, polymorphic base, and
//          immutable/mutable double-dispatch visitor interfaces.
// Key invariants:
//   - Every concrete statement reports the Stmt::Kind matching its node type.
//   - accept selects the matching visitor overload without transferring ownership.
//   - Legacy extension callbacks with default bodies remain optional for
//     existing visitor implementations.
// Ownership/Lifetime:
//   - Statement subtrees are uniquely owned by StmtPtr containers.
//   - Visitors borrow statement references only for each callback.
// Links: src/frontends/basic/ast/NodeFwd.hpp,
//        src/frontends/basic/ast/StmtControl.hpp,
//        src/frontends/basic/ast/StmtDecl.hpp,
//        src/frontends/basic/ast/StmtExpr.hpp,
//        src/frontends/basic/AST.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/NodeFwd.hpp"

#include "support/source_location.hpp"

/// @file
/// @brief Declares the common BASIC statement AST and visitor contracts.

namespace il::frontends::basic {

/// @name Additional declaration nodes used by visitor signatures
/// @{
struct NamespaceDecl;
struct UsingDecl;
struct UsingStmt;
/// @}

/// @brief Visitor interface for BASIC statements.
/// @details Pure callbacks cover the original statement set. Try/catch,
///          property, enum, namespace, and using-statement callbacks default to
///          no-ops so older visitors remain source compatible.
struct StmtVisitor {
    /// @brief Enables polymorphic destruction of immutable visitors.
    virtual ~StmtVisitor() = default;
    /// @brief Visits a borrowed label statement.
    virtual void visit(const LabelStmt &) = 0;
    /// @brief Visits a borrowed PRINT statement.
    virtual void visit(const PrintStmt &) = 0;
    /// @brief Visits a borrowed channel PRINT/WRITE statement.
    virtual void visit(const PrintChStmt &) = 0;
    /// @brief Visits a borrowed BEEP statement.
    virtual void visit(const BeepStmt &) = 0;
    /// @brief Visits a borrowed CALL statement.
    virtual void visit(const CallStmt &) = 0;
    /// @brief Visits a borrowed CLS statement.
    virtual void visit(const ClsStmt &) = 0;
    /// @brief Visits a borrowed COLOR statement.
    virtual void visit(const ColorStmt &) = 0;
    /// @brief Visits a borrowed SLEEP statement.
    virtual void visit(const SleepStmt &) = 0;
    /// @brief Visits a borrowed LOCATE statement.
    virtual void visit(const LocateStmt &) = 0;
    /// @brief Visits a borrowed cursor-visibility statement.
    virtual void visit(const CursorStmt &) = 0;
    /// @brief Visits a borrowed alternate-screen statement.
    virtual void visit(const AltScreenStmt &) = 0;
    /// @brief Visits a borrowed assignment statement.
    virtual void visit(const LetStmt &) = 0;
    /// @brief Visits a borrowed constant declaration.
    virtual void visit(const ConstStmt &) = 0;
    /// @brief Visits a borrowed DIM declaration.
    virtual void visit(const DimStmt &) = 0;
    /// @brief Visits a borrowed STATIC declaration.
    virtual void visit(const StaticStmt &) = 0;
    /// @brief Visits a borrowed SHARED declaration.
    virtual void visit(const SharedStmt &) = 0;
    /// @brief Visits a borrowed REDIM statement.
    virtual void visit(const ReDimStmt &) = 0;
    /// @brief Visits a borrowed SWAP statement.
    virtual void visit(const SwapStmt &) = 0;
    /// @brief Visits a borrowed RANDOMIZE statement.
    virtual void visit(const RandomizeStmt &) = 0;
    /// @brief Visits a borrowed IF statement.
    virtual void visit(const IfStmt &) = 0;
    /// @brief Visits a borrowed SELECT CASE statement.
    virtual void visit(const SelectCaseStmt &) = 0;

    /// @brief Optionally visits a borrowed TRY/CATCH statement; default is no-op.
    virtual void visit(const TryCatchStmt &) {}

    /// @brief Visits a borrowed WHILE statement.
    virtual void visit(const WhileStmt &) = 0;
    /// @brief Visits a borrowed DO statement.
    virtual void visit(const DoStmt &) = 0;
    /// @brief Visits a borrowed FOR statement.
    virtual void visit(const ForStmt &) = 0;
    /// @brief Visits a borrowed FOR EACH statement.
    virtual void visit(const ForEachStmt &) = 0;
    /// @brief Visits a borrowed NEXT statement.
    virtual void visit(const NextStmt &) = 0;
    /// @brief Visits a borrowed EXIT statement.
    virtual void visit(const ExitStmt &) = 0;
    /// @brief Visits a borrowed GOTO statement.
    virtual void visit(const GotoStmt &) = 0;
    /// @brief Visits a borrowed GOSUB statement.
    virtual void visit(const GosubStmt &) = 0;
    /// @brief Visits a borrowed OPEN statement.
    virtual void visit(const OpenStmt &) = 0;
    /// @brief Visits a borrowed CLOSE statement.
    virtual void visit(const CloseStmt &) = 0;
    /// @brief Visits a borrowed SEEK statement.
    virtual void visit(const SeekStmt &) = 0;
    /// @brief Visits a borrowed ON ERROR GOTO statement.
    virtual void visit(const OnErrorGoto &) = 0;
    /// @brief Visits a borrowed RESUME statement.
    virtual void visit(const Resume &) = 0;
    /// @brief Visits a borrowed END statement.
    virtual void visit(const EndStmt &) = 0;
    /// @brief Visits a borrowed INPUT statement.
    virtual void visit(const InputStmt &) = 0;
    /// @brief Visits a borrowed channel INPUT statement.
    virtual void visit(const InputChStmt &) = 0;
    /// @brief Visits a borrowed LINE INPUT channel statement.
    virtual void visit(const LineInputChStmt &) = 0;
    /// @brief Visits a borrowed RETURN statement.
    virtual void visit(const ReturnStmt &) = 0;
    /// @brief Visits a borrowed FUNCTION declaration.
    virtual void visit(const FunctionDecl &) = 0;
    /// @brief Visits a borrowed SUB declaration.
    virtual void visit(const SubDecl &) = 0;
    /// @brief Visits a borrowed statement list.
    virtual void visit(const StmtList &) = 0;
    /// @brief Visits a borrowed DELETE statement.
    virtual void visit(const DeleteStmt &) = 0;
    /// @brief Visits a borrowed constructor declaration.
    virtual void visit(const ConstructorDecl &) = 0;
    /// @brief Visits a borrowed destructor declaration.
    virtual void visit(const DestructorDecl &) = 0;
    /// @brief Visits a borrowed method declaration.
    virtual void visit(const MethodDecl &) = 0;

    /// @brief Optionally visits a borrowed property declaration; default is no-op.
    virtual void visit(const PropertyDecl &) {}

    /// @brief Visits a borrowed class declaration.
    virtual void visit(const ClassDecl &) = 0;
    /// @brief Visits a borrowed TYPE declaration.
    virtual void visit(const TypeDecl &) = 0;

    /// @brief Optionally visits a borrowed enum declaration; default is no-op.
    virtual void visit(const EnumDecl &) {}

    /// @brief Visits a borrowed interface declaration.
    virtual void visit(const InterfaceDecl &) = 0;

    /// @brief Optionally visits a borrowed namespace declaration; default is no-op.
    virtual void visit(const NamespaceDecl &) {}

    /// @brief Visits a borrowed USING declaration.
    virtual void visit(const UsingDecl &) = 0;

    /// @brief Optionally visits a borrowed USING statement; default is no-op.
    virtual void visit(const UsingStmt &) {}
};

/// @brief Visitor interface for mutable BASIC statements.
/// @details Mirrors @ref StmtVisitor while permitting node payload mutation.
///          Optional extension callbacks retain the same default no-op behavior.
struct MutStmtVisitor {
    /// @brief Enables polymorphic destruction of mutable visitors.
    virtual ~MutStmtVisitor() = default;
    /// @brief Visits a borrowed mutable label statement.
    virtual void visit(LabelStmt &) = 0;
    /// @brief Visits a borrowed mutable PRINT statement.
    virtual void visit(PrintStmt &) = 0;
    /// @brief Visits a borrowed mutable channel PRINT/WRITE statement.
    virtual void visit(PrintChStmt &) = 0;
    /// @brief Visits a borrowed mutable BEEP statement.
    virtual void visit(BeepStmt &) = 0;
    /// @brief Visits a borrowed mutable CALL statement.
    virtual void visit(CallStmt &) = 0;
    /// @brief Visits a borrowed mutable CLS statement.
    virtual void visit(ClsStmt &) = 0;
    /// @brief Visits a borrowed mutable COLOR statement.
    virtual void visit(ColorStmt &) = 0;
    /// @brief Visits a borrowed mutable SLEEP statement.
    virtual void visit(SleepStmt &) = 0;
    /// @brief Visits a borrowed mutable LOCATE statement.
    virtual void visit(LocateStmt &) = 0;
    /// @brief Visits a borrowed mutable cursor-visibility statement.
    virtual void visit(CursorStmt &) = 0;
    /// @brief Visits a borrowed mutable alternate-screen statement.
    virtual void visit(AltScreenStmt &) = 0;
    /// @brief Visits a borrowed mutable assignment statement.
    virtual void visit(LetStmt &) = 0;
    /// @brief Visits a borrowed mutable constant declaration.
    virtual void visit(ConstStmt &) = 0;
    /// @brief Visits a borrowed mutable DIM declaration.
    virtual void visit(DimStmt &) = 0;
    /// @brief Visits a borrowed mutable STATIC declaration.
    virtual void visit(StaticStmt &) = 0;
    /// @brief Visits a borrowed mutable SHARED declaration.
    virtual void visit(SharedStmt &) = 0;
    /// @brief Visits a borrowed mutable REDIM statement.
    virtual void visit(ReDimStmt &) = 0;
    /// @brief Visits a borrowed mutable SWAP statement.
    virtual void visit(SwapStmt &) = 0;
    /// @brief Visits a borrowed mutable RANDOMIZE statement.
    virtual void visit(RandomizeStmt &) = 0;
    /// @brief Visits a borrowed mutable IF statement.
    virtual void visit(IfStmt &) = 0;
    /// @brief Visits a borrowed mutable SELECT CASE statement.
    virtual void visit(SelectCaseStmt &) = 0;

    /// @brief Optionally visits mutable TRY/CATCH; default is no-op.
    virtual void visit(TryCatchStmt &) {}

    /// @brief Visits a borrowed mutable WHILE statement.
    virtual void visit(WhileStmt &) = 0;
    /// @brief Visits a borrowed mutable DO statement.
    virtual void visit(DoStmt &) = 0;
    /// @brief Visits a borrowed mutable FOR statement.
    virtual void visit(ForStmt &) = 0;
    /// @brief Visits a borrowed mutable FOR EACH statement.
    virtual void visit(ForEachStmt &) = 0;
    /// @brief Visits a borrowed mutable NEXT statement.
    virtual void visit(NextStmt &) = 0;
    /// @brief Visits a borrowed mutable EXIT statement.
    virtual void visit(ExitStmt &) = 0;
    /// @brief Visits a borrowed mutable GOTO statement.
    virtual void visit(GotoStmt &) = 0;
    /// @brief Visits a borrowed mutable GOSUB statement.
    virtual void visit(GosubStmt &) = 0;
    /// @brief Visits a borrowed mutable OPEN statement.
    virtual void visit(OpenStmt &) = 0;
    /// @brief Visits a borrowed mutable CLOSE statement.
    virtual void visit(CloseStmt &) = 0;
    /// @brief Visits a borrowed mutable SEEK statement.
    virtual void visit(SeekStmt &) = 0;
    /// @brief Visits a borrowed mutable ON ERROR GOTO statement.
    virtual void visit(OnErrorGoto &) = 0;
    /// @brief Visits a borrowed mutable RESUME statement.
    virtual void visit(Resume &) = 0;
    /// @brief Visits a borrowed mutable END statement.
    virtual void visit(EndStmt &) = 0;
    /// @brief Visits a borrowed mutable INPUT statement.
    virtual void visit(InputStmt &) = 0;
    /// @brief Visits a borrowed mutable channel INPUT statement.
    virtual void visit(InputChStmt &) = 0;
    /// @brief Visits a borrowed mutable LINE INPUT channel statement.
    virtual void visit(LineInputChStmt &) = 0;
    /// @brief Visits a borrowed mutable RETURN statement.
    virtual void visit(ReturnStmt &) = 0;
    /// @brief Visits a borrowed mutable FUNCTION declaration.
    virtual void visit(FunctionDecl &) = 0;
    /// @brief Visits a borrowed mutable SUB declaration.
    virtual void visit(SubDecl &) = 0;
    /// @brief Visits a borrowed mutable statement list.
    virtual void visit(StmtList &) = 0;
    /// @brief Visits a borrowed mutable DELETE statement.
    virtual void visit(DeleteStmt &) = 0;
    /// @brief Visits a borrowed mutable constructor declaration.
    virtual void visit(ConstructorDecl &) = 0;
    /// @brief Visits a borrowed mutable destructor declaration.
    virtual void visit(DestructorDecl &) = 0;
    /// @brief Visits a borrowed mutable method declaration.
    virtual void visit(MethodDecl &) = 0;

    /// @brief Optionally visits mutable property declarations; default is no-op.
    virtual void visit(PropertyDecl &) {}

    /// @brief Visits a borrowed mutable class declaration.
    virtual void visit(ClassDecl &) = 0;
    /// @brief Visits a borrowed mutable TYPE declaration.
    virtual void visit(TypeDecl &) = 0;

    /// @brief Optionally visits mutable enum declarations; default is no-op.
    virtual void visit(EnumDecl &) {}

    /// @brief Visits a borrowed mutable interface declaration.
    virtual void visit(InterfaceDecl &) = 0;

    /// @brief Optionally visits mutable namespace declarations; default is no-op.
    virtual void visit(NamespaceDecl &) {}

    /// @brief Visits a borrowed mutable USING declaration.
    virtual void visit(UsingDecl &) = 0;

    /// @brief Optionally visits mutable USING statements; default is no-op.
    virtual void visit(UsingStmt &) {}
};

/// @brief Base class for all BASIC statements.
/// @details Stores line and source-location metadata while concrete subclasses
///          supply their kind and double-dispatch implementation.
struct Stmt {
    /// @brief Discriminator identifying the concrete statement subclass.
    enum class Kind {
        Label,
        Print,
        PrintCh,
        Beep,
        Call,
        Cls,
        Color,
        Sleep,
        Locate,
        Cursor,
        AltScreen,
        Let,
        Const,
        Dim,
        Static,
        Shared,
        ReDim,
        Swap,
        Randomize,
        If,
        SelectCase,
        TryCatch,
        While,
        Do,
        For,
        ForEach,
        Next,
        Exit,
        Goto,
        Gosub,
        Open,
        Close,
        Seek,
        OnErrorGoto,
        Resume,
        End,
        Input,
        InputCh,
        LineInputCh,
        Return,
        FunctionDecl,
        SubDecl,
        StmtList,
        Delete,
        ConstructorDecl,
        DestructorDecl,
        MethodDecl,
        PropertyDecl,
        ClassDecl,
        TypeDecl,
        EnumDecl,
        InterfaceDecl,
        NamespaceDecl,
        UsingDecl,
        UsingStmt,
    };

    /// BASIC line number associated with this statement, or zero when absent.
    int line{0};

    /// Source location of the first token in the statement.
    il::support::SourceLoc loc{};

    /// @brief Enables polymorphic destruction of uniquely owned statements.
    virtual ~Stmt() = default;

    /// @brief Retrieve the discriminator for this statement.
    /// @return Concrete statement kind reported by the subclass.
    [[nodiscard]] virtual Kind stmtKind() const noexcept = 0;

    /// @brief Accept a visitor to process this statement.
    /// @param visitor Immutable visitor receiving the concrete callback.
    virtual void accept(StmtVisitor &visitor) const = 0;

    /// @brief Accept a mutable visitor to process this statement.
    /// @param visitor Mutable visitor receiving the concrete callback.
    virtual void accept(MutStmtVisitor &visitor) = 0;
};

} // namespace il::frontends::basic
