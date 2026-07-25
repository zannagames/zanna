//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/print/Print_Stmt_IO.cpp
// Purpose: Emit BASIC I/O statements for the AST printer.
// Whitespace invariants: Helpers mirror legacy spacing, ensuring prefixes,
//   separators, and channel markers appear exactly as before.
// Ownership/Lifetime: Context owns no state; statements are caller-owned.
// Notes: Channel formatting relies on PrintStyle conventions.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements canonical serialization of BASIC console and channel I/O
///        statements.

#include "frontends/basic/print/Print_Stmt_Common.hpp"

namespace il::frontends::basic::print_stmt {

/// @brief Render a `PRINT` statement and its mixed item list.
/// @details Iterates through the collected items, emitting either expressions or
///          literal comma/semicolon separators to preserve spacing semantics. A
///          leading `(PRINT` prefix is added and the list is closed with a
///          trailing parenthesis.
/// @param stmt PRINT statement describing printable items.
/// @param ctx Printer context responsible for expression rendering.
void printPrint(const PrintStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(PRINT";
    for (const auto &item : stmt.items) {
        os << ' ';
        switch (item.kind) {
            case PrintItem::Kind::Expr:
                ctx.printExpr(*item.expr);
                break;
            case PrintItem::Kind::Comma:
                os << ',';
                break;
            case PrintItem::Kind::Semicolon:
                os << ';';
                break;
        }
    }
    os << ')';
}

/// @brief Print `PRINT#`/`WRITE#` channel statements with arguments and flags.
/// @details Chooses the verb based on @ref PrintChStmt::mode, prints the channel
///          using style hooks, and serialises each optional argument. Null
///          entries emit the style-defined null marker. The helper appends a tag
///          when the statement suppresses the trailing newline.
/// @param stmt Channel-based print/write statement.
/// @param ctx Printer context mediating style-specific formatting.
void printPrintChannel(const PrintChStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    if (stmt.mode == PrintChStmt::Mode::Write) {
        os << "(WRITE#";
    } else {
        os << "(PRINT#";
    }
    ctx.style.writeChannelPrefix();
    ctx.printOptionalExpr(stmt.channelExpr.get());
    ctx.style.writeArgsPrefix();
    bool first = true;
    for (const auto &arg : stmt.args) {
        ctx.style.separate(first);
        if (arg) {
            ctx.printExpr(*arg);
        } else {
            ctx.style.writeNull();
        }
    }
    ctx.style.writeArgsSuffix();
    if (!stmt.trailingNewline) {
        ctx.style.writeNoNewlineTag();
    }
    os << ')';
}

/// @brief Emit an `OPEN` statement documenting mode, path, and channel.
/// @details The helper prints the symbolic mode, its numeric code for debugging,
///          and then defers to @ref Context::printOptionalExpr for optional
///          operands so absent expressions appear as the style's null token.
/// @param stmt OPEN statement with mode and channel metadata.
/// @param ctx Printer context used to format child expressions.
void printOpen(const OpenStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(OPEN mode=" << openModeToString(stmt.mode) << '(' << static_cast<int>(stmt.mode)
       << ") path=";
    ctx.printOptionalExpr(stmt.pathExpr.get());
    ctx.style.writeChannelPrefix();
    ctx.printOptionalExpr(stmt.channelExpr.get());
    os << ')';
}

/// @brief Render a `CLOSE` statement with its optional channel operand.
/// @details Delegates channel formatting to the context, relying on the style to
///          insert the `#` prefix when appropriate.
/// @param stmt CLOSE statement referencing a channel.
/// @param ctx Printer context that writes the stream decorations.
void printClose(const CloseStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(CLOSE";
    ctx.style.writeChannelPrefix();
    ctx.printOptionalExpr(stmt.channelExpr.get());
    os << ')';
}

/// @brief Emit a `SEEK` statement describing channel and target position.
/// @details Prints the channel using style hooks and appends `pos=` before
///          serialising the position expression to match the debugger-friendly
///          format used across BASIC printer output.
/// @param stmt SEEK statement containing channel and position expressions.
/// @param ctx Printer context providing formatting helpers.
void printSeek(const SeekStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(SEEK";
    ctx.style.writeChannelPrefix();
    ctx.printOptionalExpr(stmt.channelExpr.get());
    os << " pos=";
    ctx.printOptionalExpr(stmt.positionExpr.get());
    os << ')';
}

/// @brief Render an `INPUT` statement, including optional prompt and targets.
/// @details The helper prints the prompt expression when present and then joins
///          variable names with commas, mirroring traditional BASIC syntax. The
///          printer purposely keeps expressions and identifiers distinct to
///          simplify reading golden test fixtures.
/// @param stmt INPUT statement listing prompt and variable names.
/// @param ctx Printer context used for expression emission.
void printInput(const InputStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(INPUT";
    bool firstItem = true;
    auto writeItemPrefix = [&] {
        if (firstItem) {
            os << ' ';
            firstItem = false;
        } else {
            os << ", ";
        }
    };
    if (stmt.prompt) {
        writeItemPrefix();
        ctx.printExpr(*stmt.prompt);
    }
    for (const auto &name : stmt.vars) {
        writeItemPrefix();
        os << name;
    }
    os << ')';
}

/// @brief Emit an `INPUT#` statement for channel-based input.
/// @details Prints the numeric channel identifier and the target variable name,
///          using style hooks to insert the canonical channel prefix.
/// @param stmt Channel input statement containing channel and target metadata.
/// @param ctx Printer context that formats the stream decorations.
void printInputChannel(const InputChStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(INPUT#";
    ctx.style.writeChannelPrefix();
    os << stmt.channel;
    os << " targets=";
    if (stmt.targets.empty()) {
        ctx.style.writeNull();
    } else {
        for (std::size_t i = 0; i < stmt.targets.size(); ++i) {
            if (i)
                os << ',';
            os << stmt.targets[i].name;
        }
    }
    os << ')';
}

/// @brief Render a `LINE INPUT#` statement capturing channel and destination.
/// @details Emits the channel operand, then prints either the supplied target
///          expression or the style-defined null marker when the AST is missing
///          a destination.
/// @param stmt Line input statement for channels.
/// @param ctx Printer context responsible for optional expression formatting.
void printLineInputChannel(const LineInputChStmt &stmt, Context &ctx) {
    auto &os = ctx.stream();
    os << "(LINE-INPUT#";
    ctx.style.writeChannelPrefix();
    ctx.printOptionalExpr(stmt.channelExpr.get());
    os << " target=";
    if (stmt.targetVar) {
        ctx.printExpr(*stmt.targetVar);
    } else {
        ctx.style.writeNull();
    }
    os << ')';
}

} // namespace il::frontends::basic::print_stmt
