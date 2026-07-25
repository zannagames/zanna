//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Parser_Expr_Pattern.cpp
/// @brief Parses structured and expression patterns for Zia match arms.
///
/// @details Bounded speculation distinguishes wildcard, constructor, binding,
///          alternative, tuple, and literal patterns from general expressions
///          without committing diagnostics or token progress prematurely.
///          Recursive pattern parsing enforces the parser's nesting limit.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Parser.hpp"

namespace il::frontends::zia {

/// @brief Parse a match pattern, using speculation to distinguish structured patterns from
/// expressions.
/// @details Tries structured patterns (wildcard, constructor, binding) first; falls back to
///          expression pattern if not followed by a guard or fat arrow.
/// @return The parsed match arm pattern.
MatchArm::Pattern Parser::parseMatchPattern() {
    MatchArm::Pattern pattern;

    // Speculatively parse a non-expression pattern and ensure it is followed by
    // either a pipe (OR), a guard, or the fat arrow; otherwise fall back to expression pattern.
    {
        Speculation speculative(*this);
        MatchArm::Pattern candidate;
        if (parsePatternCore(candidate) &&
            (check(TokenKind::KwIf) || check(TokenKind::FatArrow) || check(TokenKind::Pipe))) {
            speculative.commit();

            // Check for OR pattern: `pat1 | pat2 | pat3`
            if (check(TokenKind::Pipe)) {
                MatchArm::Pattern orPattern;
                orPattern.kind = MatchArm::Pattern::Kind::Or;
                orPattern.subpatterns.push_back(std::move(candidate));

                while (match(TokenKind::Pipe)) {
                    MatchArm::Pattern alt;
                    if (!parsePatternCore(alt)) {
                        error("expected pattern after '|'");
                        break;
                    }
                    orPattern.subpatterns.push_back(std::move(alt));
                }

                return orPattern;
            }

            return candidate;
        }
    }

    pattern.kind = MatchArm::Pattern::Kind::Expression;
    pattern.literal = parseExpression();
    if (!pattern.literal) {
        error("expected pattern in match arm");
    }
    return pattern;
}

/// @brief Parse a core (non-expression) match pattern: wildcard, constructor, binding, literal, or
/// tuple.
/// @param[out] out The parsed pattern result.
/// @return True if a valid non-expression pattern was parsed, false otherwise.
bool Parser::parsePatternCore(MatchArm::Pattern &out) {
    if (++patternDepth_ > kMaxPatternDepth) {
        --patternDepth_;
        error("pattern nesting too deep (limit: 256)");
        return false;
    }

    struct DepthGuard {
        unsigned &d;

        /// @brief Restore the pattern-recursion counter on every exit path.
        ~DepthGuard() {
            --d;
        }
    } patternGuard_{patternDepth_};

    if (check(TokenKind::Identifier)) {
        Token nameTok = advance();
        std::string name = nameTok.text;

        if (name == "_") {
            out.kind = MatchArm::Pattern::Kind::Wildcard;
            return true;
        }

        if (name == "None") {
            out.kind = MatchArm::Pattern::Kind::Constructor;
            out.binding = std::move(name);
            return true;
        }

        if (match(TokenKind::LParen)) {
            out.kind = MatchArm::Pattern::Kind::Constructor;
            out.binding = std::move(name);

            if (!check(TokenKind::RParen)) {
                do {
                    MatchArm::Pattern subpattern;
                    if (!parsePatternCore(subpattern)) {
                        error("expected pattern in constructor pattern");
                        return false;
                    }
                    out.subpatterns.push_back(std::move(subpattern));
                } while (match(TokenKind::Comma));
            }

            if (!expect(TokenKind::RParen, ")"))
                return false;

            return true;
        }

        // Dotted identifier (e.g. Color.Red) -> enum variant literal pattern
        if (check(TokenKind::Dot)) {
            ExprPtr dotted = std::make_unique<IdentExpr>(nameTok.loc, name);
            while (match(TokenKind::Dot)) {
                Token partTok;
                if (!expect(TokenKind::Identifier, "enum variant name", &partTok))
                    return false;
                dotted = std::make_unique<FieldExpr>(nameTok.loc, std::move(dotted), partTok.text);
            }
            out.kind = MatchArm::Pattern::Kind::Literal;
            out.literal = std::move(dotted);
            return true;
        }

        out.kind = MatchArm::Pattern::Kind::Binding;
        out.binding = std::move(name);
        return true;
    }

    if (check(TokenKind::IntegerLiteral) || check(TokenKind::StringLiteral) ||
        check(TokenKind::KwTrue) || check(TokenKind::KwFalse) || check(TokenKind::KwNull)) {
        out.kind = MatchArm::Pattern::Kind::Literal;
        out.literal = parsePrimary();
        return out.literal != nullptr;
    }

    // Negative integer/number literals: treat `-42` as a literal pattern
    if (check(TokenKind::Minus) &&
        (check(TokenKind::IntegerLiteral, 1) || check(TokenKind::NumberLiteral, 1))) {
        out.kind = MatchArm::Pattern::Kind::Literal;
        out.literal = parseUnary();
        return out.literal != nullptr;
    }

    Token lparenTok;
    if (match(TokenKind::LParen, &lparenTok)) {
        std::vector<MatchArm::Pattern> elements;

        if (!check(TokenKind::RParen)) {
            do {
                MatchArm::Pattern subpattern;
                if (!parsePatternCore(subpattern)) {
                    error("expected pattern in tuple pattern");
                    return false;
                }
                elements.push_back(std::move(subpattern));
            } while (match(TokenKind::Comma));
        }

        if (!expect(TokenKind::RParen, ")"))
            return false;

        if (elements.empty())
            return false;
        if (elements.size() == 1) {
            out = std::move(elements.front());
            return true;
        }

        out.kind = MatchArm::Pattern::Kind::Tuple;
        out.subpatterns = std::move(elements);
        return true;
    }

    return false;
}

} // namespace il::frontends::zia
