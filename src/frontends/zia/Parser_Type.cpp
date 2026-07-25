//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Parser_Type.cpp
/// @brief Type parsing implementation for the Zia parser.
///
/// @details Parses qualified named types, List shorthand, generic arguments,
///          bounded fixed arrays, tuples, function signatures, and recursively
///          nested optional suffixes while enforcing the type-depth limit.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Parser.hpp"

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
// Type Parsing
//===----------------------------------------------------------------------===//

/// @brief Parse a type, including any trailing optional (`?`) suffixes.
/// @return The parsed type, or nullptr on error.
/// @details Guards against runaway recursion (kMaxTypeDepth, 256). Parses a base type via
///          parseBaseType(), then wraps it in an OptionalType for each trailing `?` (so `T??`
///          nests two optionals).
TypePtr Parser::parseType() {
    if (++typeDepth_ > kMaxTypeDepth) {
        --typeDepth_;
        error("type nesting too deep (limit: 256)");
        return nullptr;
    }

    TypePtr base = parseBaseType();
    if (!base) {
        --typeDepth_;
        return nullptr;
    }

    // Check for optional suffix ?
    while (match(TokenKind::Question)) {
        base = std::make_unique<OptionalType>(base->loc, std::move(base));
    }

    --typeDepth_;
    return base;
}

/// @brief Parse a base (non-optional) type.
/// @return The parsed type node, or nullptr on error.
/// @details Recognizes: `[T]` list shorthand (desugars to `List[T]`); qualified named types
///          (`Module.Type`); fixed-size arrays `T[N]` (non-negative integer literal count);
///          generic instantiations `T[Arg, ...]`; tuple types `(A, B)`; and function types
///          `(A, B) -> C`.
TypePtr Parser::parseBaseType() {
    // [Type] shorthand for List[Type] — e.g., [Integer] desugars to List[Integer]
    if (check(TokenKind::LBracket)) {
        Token lbTok = advance(); // consume '['
        SourceLoc loc = lbTok.loc;
        auto innerType = parseType(); // recurse: [[T]] → List[List[T]]
        if (!innerType)
            return nullptr;
        if (!expect(TokenKind::RBracket, "]"))
            return nullptr;
        std::vector<TypePtr> args;
        args.push_back(std::move(innerType));
        return std::make_unique<GenericType>(loc, "List", std::move(args));
    }

    // Named type (possibly qualified: Module.Type or Module.SubModule.Type)
    if (check(TokenKind::Identifier)) {
        Token nameTok = advance();
        SourceLoc loc = nameTok.loc;
        std::string name = nameTok.text;

        // Handle qualified type names: Module.Type, Zanna.Collections.List, etc.
        while (match(TokenKind::Dot)) {
            if (!check(TokenKind::Identifier)) {
                error("expected identifier after '.' in qualified type name");
                return nullptr;
            }
            Token nextTok = advance();
            name += ".";
            name += nextTok.text;
        }

        // Check for fixed-size array (T[N]) or generic parameters (T[Type, ...])
        if (check(TokenKind::LBracket)) {
            // Peek past '[' to see if the next token is an integer literal.
            // If so, parse as a fixed-size array type: T[N].
            // Otherwise fall through to the generic type path: T[Type, ...].
            if (peek(1).kind == TokenKind::IntegerLiteral) {
                advance();                  // consume '['
                Token countTok = advance(); // consume N
                if (countTok.requiresNegation || countTok.intValue < 0) {
                    error("fixed-size array count must be a non-negative Integer literal");
                    return nullptr;
                }
                // Cap the count so later `offset + count * elementSize` layout math
                // cannot overflow. The limit is far above any realistic inline array.
                constexpr long long kMaxFixedArrayElements = 1LL << 28; // 268,435,456
                if (countTok.intValue > kMaxFixedArrayElements) {
                    error("fixed-size array count exceeds the maximum of 268435456 elements");
                    return nullptr;
                }
                if (!expect(TokenKind::RBracket, "]"))
                    return nullptr;
                auto elemType = std::make_unique<NamedType>(loc, std::move(name));
                return std::make_unique<FixedArrayType>(
                    loc, std::move(elemType), static_cast<size_t>(countTok.intValue));
            }

            advance(); // consume '['
            std::vector<TypePtr> args;

            do {
                TypePtr arg = parseType();
                if (!arg)
                    return nullptr;
                args.push_back(std::move(arg));
            } while (match(TokenKind::Comma));

            if (!expect(TokenKind::RBracket, "]"))
                return nullptr;

            return std::make_unique<GenericType>(loc, std::move(name), std::move(args));
        }

        return std::make_unique<NamedType>(loc, std::move(name));
    }

    // Tuple or function type: (A, B) or (A, B) -> C
    Token lparenTok;
    if (match(TokenKind::LParen, &lparenTok)) {
        SourceLoc loc = lparenTok.loc;
        std::vector<TypePtr> elements;

        if (!check(TokenKind::RParen)) {
            do {
                TypePtr elem = parseType();
                if (!elem)
                    return nullptr;
                elements.push_back(std::move(elem));
            } while (match(TokenKind::Comma));
        }

        if (!expect(TokenKind::RParen, ")"))
            return nullptr;

        // Check for function type
        if (match(TokenKind::Arrow)) {
            TypePtr returnType = parseType();
            if (!returnType)
                return nullptr;

            return std::make_unique<FunctionType>(loc, std::move(elements), std::move(returnType));
        }

        if (elements.empty()) {
            error("bare '()' is only valid as a function parameter list or the Unit literal");
            return nullptr;
        }
        if (elements.size() == 1) {
            error("single-element tuple types require at least two elements");
            return nullptr;
        }

        // Tuple type
        return std::make_unique<TupleType>(loc, std::move(elements));
    }

    error("expected type");
    return nullptr;
}

} // namespace il::frontends::zia
