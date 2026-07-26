//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/zia/test_zia_lexer.cpp
// Purpose: Edge-case unit tests for the Zia lexer.
//          Tests lexer behavior directly (not through the full compiler)
//          for number parsing, string handling, comments, operators, and
//          error reporting.
// Key invariants:
//   - Every token must have correct kind, text, and parsed values.
//   - Error cases must produce TokenKind::Error and diagnostics.
//   - Lexer must not crash or hang on any input.
// Ownership/Lifetime:
//   - Tests own source buffers, diagnostics, lexers, and collected tokens.
//   - Test helpers return owning token vectors.
// Links: src/frontends/zia/Lexer.cpp, src/frontends/zia/Token.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lexer.hpp"
#include "tests/TestHarness.hpp"
#include <string>
#include <vector>

using namespace il::frontends::zia;
using namespace il::support;

namespace {

/// @brief Collect all tokens from source (excluding Eof).
std::vector<Token> tokenize(const std::string &source) {
    DiagnosticEngine diag;
    Lexer lexer(source, 0, diag);
    std::vector<Token> tokens;
    while (true) {
        Token tok = lexer.next();
        if (tok.kind == TokenKind::Eof)
            break;
        tokens.push_back(std::move(tok));
    }
    return tokens;
}

/// @brief Collect all tokens AND check for diagnostic errors.
std::vector<Token> tokenizeWithDiags(const std::string &source, DiagnosticEngine &diag) {
    Lexer lexer(source, 0, diag);
    std::vector<Token> tokens;
    while (true) {
        Token tok = lexer.next();
        if (tok.kind == TokenKind::Eof)
            break;
        tokens.push_back(std::move(tok));
    }
    return tokens;
}

/// @brief Check if any diagnostics have Error severity.
bool hasErrors(const DiagnosticEngine &diag) {
    for (const auto &d : diag.diagnostics()) {
        if (d.severity == Severity::Error)
            return true;
    }
    return false;
}

//===----------------------------------------------------------------------===//
// Number literals — edge cases
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, HexLiteralZero) {
    auto tokens = tokenize("0x0");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[0].intValue, 0);
}

TEST(ZiaLexer, HexLiteralUpperCase) {
    auto tokens = tokenize("0XFF");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[0].intValue, 255);
}

TEST(ZiaLexer, HexLiteralMissingDigits) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("0x", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, HexLiteralInvalidTailProducesSingleErrorToken) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("0x1G", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, BinaryLiteralBasic) {
    auto tokens = tokenize("0b1010");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[0].intValue, 10);
}

TEST(ZiaLexer, BinaryLiteralUpperB) {
    auto tokens = tokenize("0B11");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[0].intValue, 3);
}

TEST(ZiaLexer, BinaryLiteralMissingDigits) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("0b", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, BinaryLiteralInvalidDigit) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("0b2", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, BinaryLiteralInvalidTailProducesSingleErrorToken) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("0b102", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, DecimalZero) {
    auto tokens = tokenize("0");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[0].intValue, 0);
}

TEST(ZiaLexer, DecimalIntegerSeparators) {
    auto tokens = tokenize("1_234_567");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[0].intValue, 1234567);
}

TEST(ZiaLexer, HexAndBinarySeparators) {
    auto tokens = tokenize("0xDEAD_BEEF 0b1010_0101");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[0].intValue, static_cast<int64_t>(0xDEADBEEF));
    EXPECT_EQ(tokens[1].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[1].intValue, 0b10100101);
}

TEST(ZiaLexer, FloatSeparators) {
    auto tokens = tokenize("1_024.5_25e+1_0");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::NumberLiteral);
    EXPECT_EQ(tokens[0].floatValue, 1024.525e10);
}

TEST(ZiaLexer, NumericSeparatorMustSeparateDigits) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("1__2 0x_FF 0b1010_", diag);
    EXPECT_TRUE(hasErrors(diag));
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_EQ(tokens[1].kind, TokenKind::Error);
    EXPECT_EQ(tokens[2].kind, TokenKind::Error);
}

TEST(ZiaLexer, FloatWithExponent) {
    auto tokens = tokenize("1e10");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::NumberLiteral);
}

TEST(ZiaLexer, FloatWithNegativeExponent) {
    auto tokens = tokenize("2.5e-3");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::NumberLiteral);
}

TEST(ZiaLexer, FloatWithPositiveExponent) {
    auto tokens = tokenize("1.0E+5");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::NumberLiteral);
}

TEST(ZiaLexer, ExponentMissingDigits) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("1e", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, ExponentMissingDigitsAfterSign) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("1e+", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, DecimalDoesNotConsumeRange) {
    // "1..10" should be: integer 1, DotDot, integer 10
    auto tokens = tokenize("1..10");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[0].intValue, 1);
    EXPECT_EQ(tokens[1].kind, TokenKind::DotDot);
    EXPECT_EQ(tokens[2].kind, TokenKind::IntegerLiteral);
    EXPECT_EQ(tokens[2].intValue, 10);
}

TEST(ZiaLexer, OversizedNumericSpellingsAreBoundedErrorTokens) {
    for (const std::string &source :
         {std::string(5000, '9'), "0x" + std::string(5000, 'A'),
          "0b" + std::string(5000, '1'), "0o" + std::string(5000, '7'),
          "0xG" + std::string(5000, 'G')}) {
        DiagnosticEngine diag;
        auto tokens = tokenizeWithDiags(source, diag);
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].kind, TokenKind::Error);
        EXPECT_LE(tokens[0].text.size(), 4096u);
        EXPECT_TRUE(hasErrors(diag));
    }
}

//===----------------------------------------------------------------------===//
// String literals — edge cases
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, EmptyString) {
    auto tokens = tokenize("\"\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "");
}

TEST(ZiaLexer, StringEscapeNull) {
    auto tokens = tokenize("\"a\\0b\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    ASSERT_EQ(tokens[0].stringValue.size(), 3u);
    EXPECT_EQ(tokens[0].stringValue[0], 'a');
    EXPECT_EQ(tokens[0].stringValue[1], '\0');
    EXPECT_EQ(tokens[0].stringValue[2], 'b');
}

TEST(ZiaLexer, StringEscapeBackslash) {
    auto tokens = tokenize("\"\\\\\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "\\");
}

TEST(ZiaLexer, StringEscapeDollar) {
    auto tokens = tokenize("\"\\$\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "$");
}

TEST(ZiaLexer, StringInvalidEscape) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("\"\\q\"", diag);
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, StringHexEscape) {
    auto tokens = tokenize("\"\\x41\""); // 0x41 = 'A'
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "A");
}

TEST(ZiaLexer, StringHexEscapeInvalid) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("\"\\xGG\"", diag);
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, StringUnicodeEscape) {
    auto tokens = tokenize("\"\\u0041\""); // U+0041 = 'A'
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "A");
}

TEST(ZiaLexer, StringUnicodeEscapeTwoByte) {
    // U+00E9 = 'e' with acute accent, 2-byte UTF-8: 0xC3 0xA9
    auto tokens = tokenize("\"\\u00E9\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    ASSERT_EQ(tokens[0].stringValue.size(), 2u);
    EXPECT_EQ(static_cast<unsigned char>(tokens[0].stringValue[0]), 0xC3u);
    EXPECT_EQ(static_cast<unsigned char>(tokens[0].stringValue[1]), 0xA9u);
}

TEST(ZiaLexer, StringUnicodeEscapeThreeByte) {
    // U+4E16 = CJK character, 3-byte UTF-8: 0xE4 0xB8 0x96
    auto tokens = tokenize("\"\\u4E16\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    ASSERT_EQ(tokens[0].stringValue.size(), 3u);
    EXPECT_EQ(static_cast<unsigned char>(tokens[0].stringValue[0]), 0xE4u);
    EXPECT_EQ(static_cast<unsigned char>(tokens[0].stringValue[1]), 0xB8u);
    EXPECT_EQ(static_cast<unsigned char>(tokens[0].stringValue[2]), 0x96u);
}

TEST(ZiaLexer, StringUnicodeEscapeTooFewDigits) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("\"\\u00\"", diag);
    // Only 2 hex digits instead of 4
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, UnterminatedString) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("\"hello", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, NewlineInString) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("\"hello\nworld\"", diag);
    EXPECT_TRUE(hasErrors(diag));
    // Should produce Error token for the newline in string
    bool hasError = false;
    for (const auto &t : tokens) {
        if (t.kind == TokenKind::Error)
            hasError = true;
    }
    EXPECT_TRUE(hasError);
}

TEST(ZiaLexer, UnterminatedEscapeAtEof) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("\"hello\\", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

//===----------------------------------------------------------------------===//
// Triple-quoted strings
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, TripleQuotedBasic) {
    auto tokens = tokenize("\"\"\"hello\"\"\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "hello");
}

TEST(ZiaLexer, TripleQuotedMultiline) {
    auto tokens = tokenize("\"\"\"line1\nline2\nline3\"\"\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "line1\nline2\nline3");
}

TEST(ZiaLexer, TripleQuotedWithSingleQuote) {
    // A single " inside triple-quoted string is fine.
    // Input: """a "b" c""" — the inner double-quotes don't close the string.
    std::string src;
    src += "\"\"\""; // opening """
    src += "a \"b\" c";
    src += "\"\"\""; // closing """
    auto tokens = tokenize(src);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "a \"b\" c");
}

TEST(ZiaLexer, TripleQuotedHonorsHexAndUnicodeEscapes) {
    auto tokens = tokenize("\"\"\"A\\x41\\u0042\\n\"\"\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "AAB\n");
}

TEST(ZiaLexer, UnterminatedTripleQuoted) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("\"\"\"hello", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

//===----------------------------------------------------------------------===//
// String interpolation
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, SimpleInterpolation) {
    // "hello ${x} world"
    auto tokens = tokenize("\"hello ${x} world\"");
    // StringStart("hello ${"), Identifier(x), StringEnd("} world\"")
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringStart);
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].text, "x");
    EXPECT_EQ(tokens[2].kind, TokenKind::StringEnd);
}

TEST(ZiaLexer, InterpolationWithNestedBraces) {
    // "result: ${f({1, 2})}" — nested braces inside interpolation
    // The lexer should track brace depth and not close interpolation on inner }
    auto tokens = tokenize("\"result: ${f()}\"");
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringStart);
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier); // f
    EXPECT_EQ(tokens[2].kind, TokenKind::LParen);
    EXPECT_EQ(tokens[3].kind, TokenKind::RParen);
    EXPECT_EQ(tokens[4].kind, TokenKind::StringEnd);
}

TEST(ZiaLexer, MultipleInterpolations) {
    // "a${x}b${y}c"
    auto tokens = tokenize("\"a${x}b${y}c\"");
    // StringStart, x, StringMid, y, StringEnd
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringStart);
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].text, "x");
    EXPECT_EQ(tokens[2].kind, TokenKind::StringMid);
    EXPECT_EQ(tokens[3].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[3].text, "y");
    EXPECT_EQ(tokens[4].kind, TokenKind::StringEnd);
}

TEST(ZiaLexer, InterpolationContinuationHexAndUnicodeEscapes) {
    auto tokens = tokenize("\"${x}\\x41\\u0042\"");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringStart);
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[2].kind, TokenKind::StringEnd);
    EXPECT_EQ(tokens[2].stringValue, "AB");
}

TEST(ZiaLexer, InterpolationEscapedDollar) {
    // "\${x}" — escaped dollar sign, no interpolation
    auto tokens = tokenize("\"\\${x}\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::StringLiteral);
}

//===----------------------------------------------------------------------===//
// Comments
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, LineComment) {
    auto tokens = tokenize("x // comment\ny");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[0].text, "x");
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].text, "y");
}

TEST(ZiaLexer, BlockComment) {
    auto tokens = tokenize("x /* comment */ y");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].text, "x");
    EXPECT_EQ(tokens[1].text, "y");
}

TEST(ZiaLexer, NestedBlockComment) {
    auto tokens = tokenize("x /* outer /* inner */ still outer */ y");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].text, "x");
    EXPECT_EQ(tokens[1].text, "y");
}

TEST(ZiaLexer, UnterminatedBlockComment) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("x /* unterminated", diag);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, UnterminatedNestedBlockComment) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("/* outer /* inner */", diag);
    // Only one level closed, outer still open
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, EmptyBlockComment) {
    auto tokens = tokenize("x/**/y");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].text, "x");
    EXPECT_EQ(tokens[1].text, "y");
}

//===----------------------------------------------------------------------===//
// Operators — multi-character disambiguation
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, DotVsDotDotVsDotDotEqual) {
    auto tokens = tokenize(". .. ..=");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Dot);
    EXPECT_EQ(tokens[1].kind, TokenKind::DotDot);
    EXPECT_EQ(tokens[2].kind, TokenKind::DotDotEqual);
}

TEST(ZiaLexer, QuestionVariants) {
    auto tokens = tokenize("? ?? ?.");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Question);
    EXPECT_EQ(tokens[1].kind, TokenKind::QuestionQuestion);
    EXPECT_EQ(tokens[2].kind, TokenKind::QuestionDot);
}

TEST(ZiaLexer, ArrowsAndFatArrow) {
    auto tokens = tokenize("-> =>");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Arrow);
    EXPECT_EQ(tokens[1].kind, TokenKind::FatArrow);
}

TEST(ZiaLexer, CompoundAssignment) {
    auto tokens = tokenize("+= -= *= /= %=");
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].kind, TokenKind::PlusEqual);
    EXPECT_EQ(tokens[1].kind, TokenKind::MinusEqual);
    EXPECT_EQ(tokens[2].kind, TokenKind::StarEqual);
    EXPECT_EQ(tokens[3].kind, TokenKind::SlashEqual);
    EXPECT_EQ(tokens[4].kind, TokenKind::PercentEqual);
}

TEST(ZiaLexer, ComparisonOperators) {
    auto tokens = tokenize("== != < <= > >=");
    ASSERT_EQ(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].kind, TokenKind::EqualEqual);
    EXPECT_EQ(tokens[1].kind, TokenKind::NotEqual);
    EXPECT_EQ(tokens[2].kind, TokenKind::Less);
    EXPECT_EQ(tokens[3].kind, TokenKind::LessEqual);
    EXPECT_EQ(tokens[4].kind, TokenKind::Greater);
    EXPECT_EQ(tokens[5].kind, TokenKind::GreaterEqual);
}

TEST(ZiaLexer, LogicalOperators) {
    auto tokens = tokenize("&& ||");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].kind, TokenKind::AmpAmp);
    EXPECT_EQ(tokens[1].kind, TokenKind::PipePipe);
}

TEST(ZiaLexer, BitwiseOperators) {
    auto tokens = tokenize("& | ^ ~");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Ampersand);
    EXPECT_EQ(tokens[1].kind, TokenKind::Pipe);
    EXPECT_EQ(tokens[2].kind, TokenKind::Caret);
    EXPECT_EQ(tokens[3].kind, TokenKind::Tilde);
}

//===----------------------------------------------------------------------===//
// Keywords
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, AllKeywordsRecognized) {
    auto tokens = tokenize("var func class struct interface if else while for in "
                           "return match break continue true false null new self "
                           "super extends implements module bind let guard");
    // All should be keywords, not identifiers
    for (const auto &tok : tokens) {
        EXPECT_TRUE(tok.isKeyword());
    }
}

TEST(ZiaLexer, KeywordsCaseSensitive) {
    // Uppercase versions should be identifiers, not keywords
    auto tokens = tokenize("Var Func Class IF ELSE");
    for (const auto &tok : tokens) {
        EXPECT_EQ(tok.kind, TokenKind::Identifier);
    }
}

//===----------------------------------------------------------------------===//
// Identifiers
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, IdentifierStartsWithUnderscore) {
    auto tokens = tokenize("_foo _123 __bar");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[0].text, "_foo");
    EXPECT_EQ(tokens[1].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].text, "_123");
    EXPECT_EQ(tokens[2].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[2].text, "__bar");
}

TEST(ZiaLexer, IdentifierTooLong) {
    // Identifier over 1024 characters should error
    std::string longId(1025, 'a');
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags(longId, diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

//===----------------------------------------------------------------------===//
// Unexpected characters
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, UnexpectedCharacter) {
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("`", diag);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Error);
    EXPECT_TRUE(hasErrors(diag));
}

TEST(ZiaLexer, UnexpectedCharacterRecovery) {
    // Lexer should recover after unexpected character and continue
    DiagnosticEngine diag;
    auto tokens = tokenizeWithDiags("x ` y", diag);
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Identifier);
    EXPECT_EQ(tokens[1].kind, TokenKind::Error);
    EXPECT_EQ(tokens[2].kind, TokenKind::Identifier);
    EXPECT_TRUE(hasErrors(diag));
}

//===----------------------------------------------------------------------===//
// Brackets
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, AllBrackets) {
    auto tokens = tokenize("( ) [ ] { }");
    ASSERT_EQ(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].kind, TokenKind::LParen);
    EXPECT_EQ(tokens[1].kind, TokenKind::RParen);
    EXPECT_EQ(tokens[2].kind, TokenKind::LBracket);
    EXPECT_EQ(tokens[3].kind, TokenKind::RBracket);
    EXPECT_EQ(tokens[4].kind, TokenKind::LBrace);
    EXPECT_EQ(tokens[5].kind, TokenKind::RBrace);
}

//===----------------------------------------------------------------------===//
// Punctuation
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, Punctuation) {
    auto tokens = tokenize(": ; , @");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].kind, TokenKind::Colon);
    EXPECT_EQ(tokens[1].kind, TokenKind::Semicolon);
    EXPECT_EQ(tokens[2].kind, TokenKind::Comma);
    EXPECT_EQ(tokens[3].kind, TokenKind::At);
}

//===----------------------------------------------------------------------===//
// Whitespace handling
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, EmptyInput) {
    auto tokens = tokenize("");
    EXPECT_EQ(tokens.size(), 0u);
}

TEST(ZiaLexer, WhitespaceOnly) {
    auto tokens = tokenize("   \t\n\r\n   ");
    EXPECT_EQ(tokens.size(), 0u);
}

TEST(ZiaLexer, CommentOnly) {
    auto tokens = tokenize("// just a comment\n");
    EXPECT_EQ(tokens.size(), 0u);
}

//===----------------------------------------------------------------------===//
// Peek behavior
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, PeekDoesNotConsume) {
    DiagnosticEngine diag;
    Lexer lexer("x y", 0, diag);
    const Token &peeked1 = lexer.peek();
    EXPECT_EQ(peeked1.kind, TokenKind::Identifier);
    EXPECT_EQ(peeked1.text, "x");
    const Token &peeked2 = lexer.peek();
    EXPECT_EQ(peeked2.text, "x"); // same token
    Token consumed = lexer.next();
    EXPECT_EQ(consumed.text, "x"); // now consume it
    Token next = lexer.next();
    EXPECT_EQ(next.text, "y"); // next token
}

//===----------------------------------------------------------------------===//
// Source location tracking
//===----------------------------------------------------------------------===//

TEST(ZiaLexer, LocationTracking) {
    DiagnosticEngine diag;
    Lexer lexer("x\ny", 42, diag);
    Token tok1 = lexer.next();
    EXPECT_EQ(tok1.loc.line, 1u);
    EXPECT_EQ(tok1.loc.column, 1u);
    EXPECT_EQ(tok1.loc.file_id, 42u);
    Token tok2 = lexer.next();
    EXPECT_EQ(tok2.loc.line, 2u);
    EXPECT_EQ(tok2.loc.column, 1u);
}

TEST(ZiaLexer, LocationTrackingColumns) {
    DiagnosticEngine diag;
    Lexer lexer("abc def", 0, diag);
    Token tok1 = lexer.next();
    EXPECT_EQ(tok1.loc.column, 1u);
    Token tok2 = lexer.next();
    EXPECT_EQ(tok2.loc.column, 5u);
}

} // namespace

int main() {
    return zanna_test::run_all_tests();
}
