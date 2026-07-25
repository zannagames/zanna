//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Parser_Expr.cpp
/// @brief Implements assignment, ternary, range, coalescing, uniform binary,
///        unary, and postfix expression parsing.
///
/// @details Uniform left-associative binary operators use one precedence-
///          climbing table, while lower-precedence special forms retain
///          dedicated recursive-descent stages. Postfix parsing extends an
///          existing primary through calls, indexing, fields, optional
///          chaining, propagation, casts, and type tests. Expression recursion
///          is bounded by the parser's depth guard.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Parser.hpp"

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
// Expression Parsing
//===----------------------------------------------------------------------===//

/// @brief Parse a complete expression from the lowest-precedence assignment
///        level.
/// @return Parsed expression, or null on error.
ExprPtr Parser::parseExpression() {
    return parseAssignment();
}

/// @brief Parse an expression in an unambiguous value context that permits
///        struct literals.
/// @return Parsed expression, or null on error.
ExprPtr Parser::parseExpressionAllowingStructLiterals() {
    struct StructLiteralAllowance {
        Parser &parser;
        bool saved;

        /// @brief Enable struct literals while preserving the prior parser flag.
        StructLiteralAllowance(Parser &p) : parser(p), saved(p.allowStructLiterals_) {
            parser.allowStructLiterals_ = true;
        }

        /// @brief Restore the enclosing struct-literal allowance.
        ~StructLiteralAllowance() {
            parser.allowStructLiterals_ = saved;
        }
    } allowance(*this);

    ExprPtr expr = parseExpression();
    return expr;
}

/// @brief Clone an expression that is safe to duplicate in compound assignment.
/// @param expr Candidate left-hand expression.
/// @return Deep clone for pure identifiers/literals/field/index/value
///         operators, or null when evaluation could have side effects.
/// @details Calls, allocation, await/try, and other potentially effectful forms
/// are intentionally rejected so `target += value` does not re-run target effects.
static ExprPtr clonePureExpr(Expr *expr) {
    if (!expr)
        return nullptr;

    switch (expr->kind) {
        case ExprKind::Ident: {
            auto *id = static_cast<IdentExpr *>(expr);
            return std::make_unique<IdentExpr>(id->loc, id->name);
        }
        case ExprKind::SelfExpr:
            return std::make_unique<SelfExpr>(expr->loc);
        case ExprKind::SuperExpr:
            return std::make_unique<SuperExprNode>(expr->loc);
        case ExprKind::IntLiteral: {
            auto *lit = static_cast<IntLiteralExpr *>(expr);
            return std::make_unique<IntLiteralExpr>(lit->loc, lit->value);
        }
        case ExprKind::NumberLiteral: {
            auto *lit = static_cast<NumberLiteralExpr *>(expr);
            return std::make_unique<NumberLiteralExpr>(lit->loc, lit->value);
        }
        case ExprKind::StringLiteral: {
            auto *lit = static_cast<StringLiteralExpr *>(expr);
            return std::make_unique<StringLiteralExpr>(lit->loc, lit->value);
        }
        case ExprKind::BoolLiteral: {
            auto *lit = static_cast<BoolLiteralExpr *>(expr);
            return std::make_unique<BoolLiteralExpr>(lit->loc, lit->value);
        }
        case ExprKind::NullLiteral:
            return std::make_unique<NullLiteralExpr>(expr->loc);
        case ExprKind::UnitLiteral:
            return std::make_unique<UnitLiteralExpr>(expr->loc);
        case ExprKind::Field: {
            auto *field = static_cast<FieldExpr *>(expr);
            ExprPtr base = clonePureExpr(field->base.get());
            if (!base)
                return nullptr;
            return std::make_unique<FieldExpr>(field->loc, std::move(base), field->field);
        }
        case ExprKind::Index: {
            auto *idx = static_cast<IndexExpr *>(expr);
            ExprPtr base = clonePureExpr(idx->base.get());
            ExprPtr index = clonePureExpr(idx->index.get());
            if (!base || !index)
                return nullptr;
            return std::make_unique<IndexExpr>(idx->loc, std::move(base), std::move(index));
        }
        case ExprKind::Unary: {
            auto *unary = static_cast<UnaryExpr *>(expr);
            ExprPtr operand = clonePureExpr(unary->operand.get());
            if (!operand)
                return nullptr;
            return std::make_unique<UnaryExpr>(unary->loc, unary->op, std::move(operand));
        }
        case ExprKind::Binary: {
            auto *binary = static_cast<BinaryExpr *>(expr);
            if (binary->op == BinaryOp::Assign)
                return nullptr;
            ExprPtr left = clonePureExpr(binary->left.get());
            ExprPtr right = clonePureExpr(binary->right.get());
            if (!left || !right)
                return nullptr;
            return std::make_unique<BinaryExpr>(
                binary->loc, binary->op, std::move(left), std::move(right));
        }
        case ExprKind::Ternary: {
            auto *ternary = static_cast<TernaryExpr *>(expr);
            ExprPtr condition = clonePureExpr(ternary->condition.get());
            ExprPtr thenExpr = clonePureExpr(ternary->thenExpr.get());
            ExprPtr elseExpr = clonePureExpr(ternary->elseExpr.get());
            if (!condition || !thenExpr || !elseExpr)
                return nullptr;
            return std::make_unique<TernaryExpr>(
                ternary->loc, std::move(condition), std::move(thenExpr), std::move(elseExpr));
        }
        case ExprKind::Coalesce: {
            auto *coalesce = static_cast<CoalesceExpr *>(expr);
            ExprPtr left = clonePureExpr(coalesce->left.get());
            ExprPtr right = clonePureExpr(coalesce->right.get());
            if (!left || !right)
                return nullptr;
            return std::make_unique<CoalesceExpr>(coalesce->loc, std::move(left), std::move(right));
        }
        case ExprKind::Range: {
            auto *range = static_cast<RangeExpr *>(expr);
            ExprPtr start = clonePureExpr(range->start.get());
            ExprPtr end = clonePureExpr(range->end.get());
            if (!start || !end)
                return nullptr;
            return std::make_unique<RangeExpr>(
                range->loc, std::move(start), std::move(end), range->inclusive);
        }
        default:
            return nullptr;
    }
}

namespace {

/// @brief Precedence descriptor for a uniform left-associative binary operator.
struct BinOpDesc {
    BinaryOp op;
    int prec; ///< Higher binds tighter. Comparison ops share level kComparisonPrec.
};

constexpr int kComparisonPrec = 8;
constexpr int kMinBinaryPrec = 2; ///< Lowest uniform level (logical OR).

/// @brief Classify a token as a uniform binary operator.
/// @param kind Candidate operator token kind.
/// @param[out] out Receives the semantic operator and precedence.
/// @return True when @p kind belongs to the uniform precedence table.
/// @details Covers the operator levels between logical-OR (loosest) and
/// multiplicative (tightest). The looser special forms — `??`, ranges, ternary,
/// and assignment — are handled by their own parse routines, not here. This is
/// the single source of truth for binary precedence, shared by the normal
/// descent (parseCoalesce) and match-arm continuation (parseBinaryFrom).
bool binaryOpDesc(TokenKind kind, BinOpDesc &out) {
    switch (kind) {
        case TokenKind::Star: out = {BinaryOp::Mul, 11}; return true;
        case TokenKind::Slash: out = {BinaryOp::Div, 11}; return true;
        case TokenKind::Percent: out = {BinaryOp::Mod, 11}; return true;
        case TokenKind::Plus: out = {BinaryOp::Add, 10}; return true;
        case TokenKind::Minus: out = {BinaryOp::Sub, 10}; return true;
        case TokenKind::ShiftLeft: out = {BinaryOp::Shl, 9}; return true;
        case TokenKind::ShiftRight: out = {BinaryOp::Shr, 9}; return true;
        case TokenKind::Less: out = {BinaryOp::Lt, kComparisonPrec}; return true;
        case TokenKind::LessEqual: out = {BinaryOp::Le, kComparisonPrec}; return true;
        case TokenKind::Greater: out = {BinaryOp::Gt, kComparisonPrec}; return true;
        case TokenKind::GreaterEqual: out = {BinaryOp::Ge, kComparisonPrec}; return true;
        case TokenKind::EqualEqual: out = {BinaryOp::Eq, 7}; return true;
        case TokenKind::NotEqual: out = {BinaryOp::Ne, 7}; return true;
        case TokenKind::Ampersand: out = {BinaryOp::BitAnd, 6}; return true;
        case TokenKind::Caret: out = {BinaryOp::BitXor, 5}; return true;
        case TokenKind::Pipe: out = {BinaryOp::BitOr, 4}; return true;
        case TokenKind::AmpAmp:
        case TokenKind::KwAnd: out = {BinaryOp::And, 3}; return true;
        case TokenKind::PipePipe:
        case TokenKind::KwOr: out = {BinaryOp::Or, kMinBinaryPrec}; return true;
        default: return false;
    }
}

} // namespace

/// @brief Extend a parsed operand through uniform binary operators.
/// @param left Already-parsed left operand.
/// @param minPrec Minimum precedence accepted by this climb.
/// @return Extended expression, or null on error.
/// @details Folds tighter operators into the right operand and rejects chained
///          relational comparisons without an explicit boolean operator.
ExprPtr Parser::parseBinaryRhs(ExprPtr left, int minPrec) {
    if (!left)
        return nullptr;
    for (;;) {
        BinOpDesc desc;
        if (!binaryOpDesc(peek().kind, desc) || desc.prec < minPrec)
            break;
        Token opTok = advance();
        ExprPtr right = parseUnary();
        if (!right)
            return nullptr;
        // Precedence climb: fold any tighter-binding operators into the right
        // operand before combining with this one (all uniform ops are left-assoc).
        BinOpDesc nextDesc;
        while (binaryOpDesc(peek().kind, nextDesc) && nextDesc.prec > desc.prec) {
            right = parseBinaryRhs(std::move(right), desc.prec + 1);
            if (!right)
                return nullptr;
        }
        // Relational operators do not chain: `a < b < c` is rejected.
        if (desc.prec == kComparisonPrec) {
            BinOpDesc after;
            if (binaryOpDesc(peek().kind, after) && after.prec == kComparisonPrec) {
                errorAt(peek().loc,
                        "chained relational comparisons require explicit boolean operators");
                return nullptr;
            }
        }
        left = std::make_unique<BinaryExpr>(opTok.loc, desc.op, std::move(left), std::move(right));
    }
    return left;
}

/// @brief Parse right-associative simple or compound assignment.
/// @return Assignment expression or the tighter ternary expression.
/// @details Compound assignment is desugared to assignment plus a cloned pure
///          read of the target; effectful targets are rejected to prevent
///          duplicated evaluation.
ExprPtr Parser::parseAssignment() {
    ExprPtr expr = parseTernary();
    if (!expr)
        return nullptr;

    Token eqTok;
    if (match(TokenKind::Equal, &eqTok)) {
        SourceLoc loc = eqTok.loc;
        ExprPtr value = parseExpressionAllowingStructLiterals(); // right-associative
        if (!value)
            return nullptr;
        return std::make_unique<BinaryExpr>(
            loc, BinaryOp::Assign, std::move(expr), std::move(value));
    }

    // Compound assignment operators: +=, -=, *=, /=, %=
    // Desugar: a += b  ->  a = a + b
    auto compoundOp = [&](TokenKind tk) -> BinaryOp {
        switch (tk) {
            case TokenKind::PlusEqual:
                return BinaryOp::Add;
            case TokenKind::MinusEqual:
                return BinaryOp::Sub;
            case TokenKind::StarEqual:
                return BinaryOp::Mul;
            case TokenKind::SlashEqual:
                return BinaryOp::Div;
            case TokenKind::PercentEqual:
                return BinaryOp::Mod;
            case TokenKind::ShiftLeftEqual:
                return BinaryOp::Shl;
            case TokenKind::ShiftRightEqual:
                return BinaryOp::Shr;
            case TokenKind::AmpersandEqual:
                return BinaryOp::BitAnd;
            case TokenKind::PipeEqual:
                return BinaryOp::BitOr;
            case TokenKind::CaretEqual:
                return BinaryOp::BitXor;
            default:
                return BinaryOp::Add; // unreachable
        }
    };

    Token compTok;
    if (match(TokenKind::PlusEqual, &compTok) || match(TokenKind::MinusEqual, &compTok) ||
        match(TokenKind::StarEqual, &compTok) || match(TokenKind::SlashEqual, &compTok) ||
        match(TokenKind::PercentEqual, &compTok) || match(TokenKind::ShiftLeftEqual, &compTok) ||
        match(TokenKind::ShiftRightEqual, &compTok) || match(TokenKind::AmpersandEqual, &compTok) ||
        match(TokenKind::PipeEqual, &compTok) || match(TokenKind::CaretEqual, &compTok)) {
        SourceLoc loc = compTok.loc;
        BinaryOp op = compoundOp(compTok.kind);

        // Clone the LHS for the read side of the compound operation. Reject
        // effectful targets instead of lowering them with duplicated effects.
        ExprPtr lhsClone = clonePureExpr(expr.get());
        if (!lhsClone) {
            error("compound assignment target must be a side-effect-free lvalue");
            return nullptr;
        }

        ExprPtr rhs = parseExpressionAllowingStructLiterals(); // right-associative
        if (!rhs)
            return nullptr;

        // Build: lhs = lhsClone op rhs
        auto arithExpr = std::make_unique<BinaryExpr>(loc, op, std::move(lhsClone), std::move(rhs));
        return std::make_unique<BinaryExpr>(
            loc, BinaryOp::Assign, std::move(expr), std::move(arithExpr));
    }

    return expr;
}

/// @brief Parse a ternary conditional expression.
/// @return Parsed ternary or the tighter range expression.
ExprPtr Parser::parseTernary() {
    ExprPtr expr = parseRange();
    if (!expr)
        return nullptr;

    Token qTok;
    if (match(TokenKind::Question, &qTok)) {
        SourceLoc loc = qTok.loc;
        ExprPtr thenExpr = parseExpressionAllowingStructLiterals();
        if (!thenExpr)
            return nullptr;

        if (!check(TokenKind::Colon)) {
            errorAt(peek().loc,
                    "expected ':' to complete ternary expression; if you meant the try "
                    "operator, parenthesize as '(expr?)'");
            return nullptr;
        }
        advance();

        ExprPtr elseExpr = parseExpressionAllowingStructLiterals();
        if (!elseExpr)
            return nullptr;

        return std::make_unique<TernaryExpr>(
            loc, std::move(expr), std::move(thenExpr), std::move(elseExpr));
    }

    return expr;
}

/// @brief Parse exclusive `..` or inclusive `..=` range syntax.
/// @return Parsed range or the tighter coalescing expression.
ExprPtr Parser::parseRange() {
    ExprPtr expr = parseCoalesce();
    if (!expr)
        return nullptr;

    if (check(TokenKind::DotDot) || check(TokenKind::DotDotEqual)) {
        Token opTok = advance();
        bool inclusive = opTok.kind == TokenKind::DotDotEqual;
        SourceLoc loc = opTok.loc;

        ExprPtr right = parseCoalesce();
        if (!right)
            return nullptr;

        expr = std::make_unique<RangeExpr>(loc, std::move(expr), std::move(right), inclusive);
        if (check(TokenKind::DotDot) || check(TokenKind::DotDotEqual)) {
            error("chained range expressions require parentheses");
            return nullptr;
        }
    }

    return expr;
}

/// @brief Parse a null-coalescing expression (a ?? b) and everything tighter.
/// @details The uniform binary operators (logical-OR down to multiplicative) are
/// handled by the shared precedence-climbing routine parseBinaryRhs(); `??`
/// (right-associative) is layered on top here.
/// @return The parsed expression, potentially wrapping a CoalesceExpr.
/// @brief Parse left-associative null-coalescing expressions.
/// @return Parsed coalescing chain or tighter uniform binary expression.
ExprPtr Parser::parseCoalesce() {
    ExprPtr left = parseUnary();
    if (!left)
        return nullptr;
    ExprPtr expr = parseBinaryRhs(std::move(left), kMinBinaryPrec);
    if (!expr)
        return nullptr;

    Token opTok;
    if (match(TokenKind::QuestionQuestion, &opTok)) {
        SourceLoc loc = opTok.loc;
        ExprPtr right = parseCoalesce();
        if (!right)
            return nullptr;

        expr = std::make_unique<CoalesceExpr>(loc, std::move(expr), std::move(right));
    }

    return expr;
}

/// @brief Parse prefix unary operators and their operand.
/// @return Parsed unary expression or postfix expression.
/// @details Enforces the expression nesting limit and recognizes negation,
///          logical/bitwise not, and address-of forms.
ExprPtr Parser::parseUnary() {
    if (++exprDepth_ > kMaxExprDepth) {
        --exprDepth_;
        error("expression nesting too deep (limit: 256)");
        return nullptr;
    }

    ExprPtr result;

    // await expr -- parsed as a unary prefix like ! or -
    if (check(TokenKind::KwAwait)) {
        Token awaitTok = advance();
        SourceLoc loc = awaitTok.loc;
        ExprPtr operand = parseUnary();
        if (!operand) {
            --exprDepth_;
            return nullptr;
        }
        --exprDepth_;
        return std::make_unique<AwaitExpr>(loc, std::move(operand));
    }

    if (check(TokenKind::Minus) || check(TokenKind::Bang) || check(TokenKind::Tilde) ||
        check(TokenKind::KwNot) || check(TokenKind::Ampersand)) {
        Token opTok = advance();
        UnaryOp op;
        switch (opTok.kind) {
            case TokenKind::Minus:
                op = UnaryOp::Neg;
                break;
            case TokenKind::Bang:
            case TokenKind::KwNot:
                op = UnaryOp::Not;
                break;
            case TokenKind::Tilde:
                op = UnaryOp::BitNot;
                break;
            case TokenKind::Ampersand:
                op = UnaryOp::AddressOf;
                break;
            default:
                --exprDepth_;
                error("expected unary operator");
                return nullptr;
        }
        SourceLoc loc = opTok.loc;

        // Special case: handle -9223372036854775808 (INT64_MIN)
        // The literal 9223372036854775808 can't be represented as int64_t,
        // but when negated it becomes INT64_MIN which is valid.
        if (op == UnaryOp::Neg && check(TokenKind::IntegerLiteral) && peek().requiresNegation) {
            advance(); // consume the integer literal
            --exprDepth_;
            // Return INT64_MIN directly as an integer literal
            return std::make_unique<IntLiteralExpr>(loc, INT64_MIN);
        }

        ExprPtr operand = parseUnary();
        if (!operand) {
            --exprDepth_;
            return nullptr;
        }

        result = std::make_unique<UnaryExpr>(loc, op, std::move(operand));
    } else {
        result = parsePostfix();
    }

    --exprDepth_;
    return result;
}

/// @brief Continue parsing postfix and binary operators from an existing
///        expression.
/// @param startExpr Initial left operand.
/// @return Fully extended expression, or null on error.
ExprPtr Parser::parsePostfixAndBinaryFrom(ExprPtr startExpr) {
    // Parse postfix operators on the starting expression
    ExprPtr expr = parsePostfixFrom(std::move(startExpr));
    if (!expr)
        return nullptr;
    // Continue with binary operators (but not assignment)
    return parseBinaryFrom(std::move(expr));
}

/// @brief Parse binary operators from a pre-parsed left-hand expression.
/// @details Used by parsePostfixAndBinaryFrom to handle match arm patterns that
///          begin with an already-parsed primary expression. Mirrors normal
///          expression parsing for non-assignment operators so this helper does
///          not accept grammar forms rejected by parseAssignment().
/// @param expr The left-hand expression to extend with binary operators.
/// @return The extended expression.
/// @brief Continue uniform binary parsing from an existing left operand.
/// @param expr Initial left operand.
/// @return Extended expression, or null on error.
ExprPtr Parser::parseBinaryFrom(ExprPtr expr) {
    // Fold the uniform binary-operator ladder (multiplicative..logical-or) with
    // the shared precedence-climbing routine, then handle the looser special
    // forms (`??`, ranges, ternary) and reject assignment in this context.
    expr = parseBinaryRhs(std::move(expr), kMinBinaryPrec);
    if (!expr)
        return nullptr;

    Token opTok;
    while (match(TokenKind::QuestionQuestion, &opTok)) {
        SourceLoc loc = opTok.loc;
        ExprPtr right = parseCoalesce();
        if (!right)
            return nullptr;
        expr = std::make_unique<CoalesceExpr>(loc, std::move(expr), std::move(right));
    }
    if (check(TokenKind::DotDot) || check(TokenKind::DotDotEqual)) {
        Token rangeTok = advance();
        SourceLoc loc = rangeTok.loc;
        ExprPtr right = parseCoalesce();
        if (!right)
            return nullptr;
        expr = std::make_unique<RangeExpr>(
            loc, std::move(expr), std::move(right), rangeTok.kind == TokenKind::DotDotEqual);
        if (check(TokenKind::DotDot) || check(TokenKind::DotDotEqual)) {
            error("chained range expressions require parentheses");
            return nullptr;
        }
    }
    if (match(TokenKind::Question, &opTok)) {
        SourceLoc loc = opTok.loc;
        ExprPtr thenExpr = parseExpressionAllowingStructLiterals();
        if (!thenExpr)
            return nullptr;
        if (!check(TokenKind::Colon)) {
            errorAt(peek().loc,
                    "expected ':' to complete ternary expression; if you meant the try "
                    "operator, parenthesize as '(expr?)'");
            return nullptr;
        }
        advance();
        ExprPtr elseExpr = parseExpressionAllowingStructLiterals();
        if (!elseExpr)
            return nullptr;
        expr = std::make_unique<TernaryExpr>(
            loc, std::move(expr), std::move(thenExpr), std::move(elseExpr));
    }
    if (check(TokenKind::Equal) || check(TokenKind::PlusEqual) || check(TokenKind::MinusEqual) ||
        check(TokenKind::StarEqual) || check(TokenKind::SlashEqual) ||
        check(TokenKind::PercentEqual) || check(TokenKind::ShiftLeftEqual) ||
        check(TokenKind::ShiftRightEqual) || check(TokenKind::AmpersandEqual) ||
        check(TokenKind::PipeEqual) || check(TokenKind::CaretEqual)) {
        errorAt(peek().loc, "assignment is not valid in this expression context");
        return nullptr;
    }
    return expr;
}

/// @brief Apply postfix operators to a base expression.
/// @details Handles call, subscript, member access, optional chaining, is/as casts,
///          and try expressions in a loop until no more postfix operators match.
/// @param expr The base expression to extend.
/// @return The expression with all postfix operations applied.
/// @brief Continue parsing postfix operations from an existing expression.
/// @param expr Base expression to extend.
/// @return Expression with all following postfix operations applied.
/// @details Handles calls, subscripts, member and optional-member access,
///          postfix propagation/unwrap, and `as`/`is` type operations.
ExprPtr Parser::parsePostfixFrom(ExprPtr expr) {
    while (true) {
        Token opTok;
        if (match(TokenKind::LParen, &opTok)) {
            // Function call
            SourceLoc loc = opTok.loc;
            std::vector<CallArg> args;
            if (!parseCallArgs(args))
                return nullptr;
            if (!expect(TokenKind::RParen, ")"))
                return nullptr;

            expr = std::make_unique<CallExpr>(loc, std::move(expr), std::move(args));
        } else if (match(TokenKind::LBracket, &opTok)) {
            // Index
            SourceLoc loc = opTok.loc;
            std::vector<ExprPtr> indexes;
            ExprPtr index = parseExpression();
            if (!index)
                return nullptr;
            indexes.push_back(std::move(index));

            while (match(TokenKind::Comma)) {
                ExprPtr nextIndex = parseExpression();
                if (!nextIndex)
                    return nullptr;
                indexes.push_back(std::move(nextIndex));
            }

            if (!expect(TokenKind::RBracket, "]"))
                return nullptr;

            if (indexes.size() == 1) {
                index = std::move(indexes.front());
            } else {
                index = std::make_unique<TupleExpr>(loc, std::move(indexes));
            }
            expr = std::make_unique<IndexExpr>(loc, std::move(expr), std::move(index));
        } else if (match(TokenKind::Dot, &opTok)) {
            // Field access or tuple index
            SourceLoc loc = opTok.loc;

            // Check for tuple index access: tuple.0, tuple.1, etc.
            if (check(TokenKind::IntegerLiteral)) {
                int64_t index = peek().intValue;
                if (index < 0) {
                    errorAt(peek().loc, "tuple index must be non-negative");
                    return nullptr;
                }
                advance(); // consume integer literal
                expr = std::make_unique<TupleIndexExpr>(
                    loc, std::move(expr), static_cast<size_t>(index));
            } else if (checkIdentifierLike()) {
                std::string field = peek().text;
                advance(); // consume identifier
                expr = std::make_unique<FieldExpr>(loc, std::move(expr), std::move(field));
            } else {
                error("expected field name after '.'");
                return nullptr;
            }
        } else if (match(TokenKind::QuestionDot, &opTok)) {
            // Optional chain
            SourceLoc loc = opTok.loc;
            if (!checkIdentifierLike()) {
                error("expected field name after '?.'");
                return nullptr;
            }
            std::string field = peek().text;
            advance(); // consume identifier

            expr = std::make_unique<OptionalChainExpr>(loc, std::move(expr), std::move(field));
        } else if (match(TokenKind::KwIs, &opTok)) {
            // Type check
            SourceLoc loc = opTok.loc;
            TypePtr type = parseType();
            if (!type)
                return nullptr;

            expr = std::make_unique<IsExpr>(loc, std::move(expr), std::move(type));
        } else if (match(TokenKind::KwAs, &opTok)) {
            // Type cast
            SourceLoc loc = opTok.loc;
            TypePtr type = parseType();
            if (!type)
                return nullptr;

            expr = std::make_unique<AsExpr>(loc, std::move(expr), std::move(type));
        } else if (check(TokenKind::Question)) {
            // Try expression: expr? - propagate null/error
            // Note: This is different from optional type T? or ternary a ? b : c
            if (isExpressionStart(peek(1).kind))
                break;

            Token qTok = advance();
            SourceLoc loc = qTok.loc;
            expr = std::make_unique<TryExpr>(loc, std::move(expr));
        } else if (check(TokenKind::Bang)) {
            // Force-unwrap: expr! - asserts non-null, traps if null
            Token bangTok = advance();
            SourceLoc loc = bangTok.loc;
            expr = std::make_unique<ForceUnwrapExpr>(loc, std::move(expr));
        } else {
            break;
        }
    }

    return expr;
}

/// @brief Parse a primary expression followed by every postfix operation.
/// @return Parsed postfix expression, or null on error.
ExprPtr Parser::parsePostfix() {
    ExprPtr expr = parsePrimary();
    if (!expr)
        return nullptr;
    return parsePostfixFrom(std::move(expr));
}

} // namespace il::frontends::zia
