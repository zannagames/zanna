//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements shallow completeness classification for Zia and BASIC REPL input.
/// @details Zia classification balances braces, parentheses, and brackets while
///          respecting strings, escapes, and line comments. BASIC classification
///          strips comments, splits colon-separated statements outside strings,
///          and tracks recognized block-opening and block-closing keywords.
///          Both classifiers identify empty submissions and dot-prefixed meta
///          commands without compiling source.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplInputClassifier.cpp
// Purpose: Implementation of REPL input classification for Zia.
// Key invariants:
//   - Braces/brackets inside string literals are not counted.
//   - Escape sequences inside strings are handled (\" does not end string).
// Ownership/Lifetime:
//   - Stateless; all classification is per-call.
// Links: src/repl/ReplInputClassifier.hpp
//
//===----------------------------------------------------------------------===//

#include "ReplInputClassifier.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>
#include <vector>

namespace zanna::repl {

// ---------------------------------------------------------------------------
// Helpers shared by Zia and BASIC classifiers
// ---------------------------------------------------------------------------

/// @brief Determine whether a string contains only whitespace.
/// @param input String to inspect.
/// @return `true` for empty input or when every byte is classified as whitespace.
static bool isAllWhitespace(const std::string &input) {
    for (char c : input) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

/// @brief Locate the first non-whitespace byte.
/// @param input String to scan.
/// @return Byte offset of the first non-space, or `input.size()`.
static size_t findFirstNonSpace(const std::string &input) {
    size_t pos = 0;
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
        ++pos;
    return pos;
}

/// @brief Match a BASIC keyword at a byte offset.
/// @details Comparison is case-insensitive and requires end-of-input or a
///          whitespace, opening-parenthesis, or colon boundary afterward.
/// @param line Source line to inspect.
/// @param pos Candidate keyword offset.
/// @param kw Null-terminated ASCII keyword.
/// @return `true` when spelling and trailing boundary match.
static bool matchKeywordCI(const std::string &line, size_t pos, const char *kw) {
    size_t kwLen = std::strlen(kw);
    if (pos + kwLen > line.size())
        return false;
    for (size_t i = 0; i < kwLen; ++i) {
        if (std::toupper(static_cast<unsigned char>(line[pos + i])) !=
            std::toupper(static_cast<unsigned char>(kw[i])))
            return false;
    }
    // Must be followed by whitespace, '(', or end of string (word boundary)
    if (pos + kwLen < line.size()) {
        char next = line[pos + kwLen];
        return std::isspace(static_cast<unsigned char>(next)) || next == '(' || next == ':';
    }
    return true;
}

/// @brief Determine whether @p pos is at the beginning of a BASIC word token.
/// @param line Source line being inspected.
/// @param pos Candidate token offset.
/// @return True when the preceding character is not an identifier character.
static bool hasBasicWordStartBoundary(const std::string &line, size_t pos) {
    if (pos == 0)
        return true;
    unsigned char prev = static_cast<unsigned char>(line[pos - 1]);
    return !(std::isalnum(prev) || prev == '_');
}

/// @brief Strip BASIC single-line comments while respecting string literals.
/// @details Apostrophe comments are recognized outside strings. REM comments are
///          recognized as a word token outside strings, which covers both
///          whole-line comments and comments after a colon-separated statement.
/// @param line Raw physical BASIC line.
/// @return Line content before the first comment marker.
static std::string stripBasicComment(const std::string &line) {
    bool inStr = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            if (inStr && i + 1 < line.size() && line[i + 1] == '"') {
                ++i;
                continue;
            }
            inStr = !inStr;
            continue;
        }
        if (inStr)
            continue;
        if (line[i] == '\'')
            return line.substr(0, i);
        if (hasBasicWordStartBoundary(line, i) && matchKeywordCI(line, i, "REM"))
            return line.substr(0, i);
    }
    return line;
}

/// @brief Split a BASIC line into colon-separated statements.
/// @details Colons inside string literals are preserved as part of the current
///          statement. Doubled quote escapes are skipped so they do not toggle
///          string state.
/// @param line BASIC line after comment stripping.
/// @return Individual statement segments in source order.
static std::vector<std::string> splitBasicStatements(const std::string &line) {
    std::vector<std::string> statements;
    bool inStr = false;
    size_t start = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            if (inStr && i + 1 < line.size() && line[i + 1] == '"') {
                ++i;
                continue;
            }
            inStr = !inStr;
            continue;
        }
        if (!inStr && line[i] == ':') {
            statements.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    statements.push_back(line.substr(start));
    return statements;
}

/// @brief Trim trailing whitespace from a string in place.
/// @param s String to mutate.
static void trimRight(std::string &s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
}

/// @brief Test whether a BASIC physical line ends inside a string literal.
/// @details Comment markers outside strings stop the scan. Doubled quotes inside
///          a string are treated as escaped quotes and do not close the string.
/// @param line Raw BASIC source line.
/// @return True when an opening quote remains unmatched at line end.
static bool basicLineEndsInsideString(const std::string &line) {
    bool inStr = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            if (inStr && i + 1 < line.size() && line[i + 1] == '"') {
                ++i;
                continue;
            }
            inStr = !inStr;
            continue;
        }
        if (inStr)
            continue;
        if (line[i] == '\'')
            break;
        if (hasBasicWordStartBoundary(line, i) && matchKeywordCI(line, i, "REM"))
            break;
    }
    return inStr;
}

/// @brief Apply one BASIC statement segment to the block-depth counter.
/// @details The classifier is intentionally shallow: it detects statement-level
///          block openers and closers but does not parse expressions. Segments
///          that do not affect block structure leave @p blockDepth unchanged.
/// @param statement A single BASIC statement, with comments already removed.
/// @param[in,out] blockDepth Mutable block-depth counter; unmatched closing
///                statements may make it negative and are treated as complete.
static void applyBasicStatementDepth(std::string statement, int &blockDepth) {
    trimRight(statement);
    size_t pos = findFirstNonSpace(statement);
    if (pos >= statement.size())
        return;

    if (matchKeywordCI(statement, pos, "END")) {
        size_t afterEnd = pos + 3;
        while (afterEnd < statement.size() &&
               std::isspace(static_cast<unsigned char>(statement[afterEnd])))
            ++afterEnd;

        if (matchKeywordCI(statement, afterEnd, "IF") ||
            matchKeywordCI(statement, afterEnd, "SUB") ||
            matchKeywordCI(statement, afterEnd, "FUNCTION") ||
            matchKeywordCI(statement, afterEnd, "CLASS") ||
            matchKeywordCI(statement, afterEnd, "TYPE") ||
            matchKeywordCI(statement, afterEnd, "SELECT") ||
            matchKeywordCI(statement, afterEnd, "TRY") ||
            matchKeywordCI(statement, afterEnd, "PROPERTY") ||
            matchKeywordCI(statement, afterEnd, "NAMESPACE") ||
            matchKeywordCI(statement, afterEnd, "GET") ||
            matchKeywordCI(statement, afterEnd, "SET")) {
            --blockDepth;
        }
        return;
    }

    if (matchKeywordCI(statement, pos, "NEXT") || matchKeywordCI(statement, pos, "WEND") ||
        matchKeywordCI(statement, pos, "LOOP")) {
        --blockDepth;
        return;
    }

    if (matchKeywordCI(statement, pos, "SUB") || matchKeywordCI(statement, pos, "FUNCTION")) {
        ++blockDepth;
        return;
    }

    if (matchKeywordCI(statement, pos, "IF")) {
        bool foundThen = false;
        for (size_t i = pos + 2; i < statement.size(); ++i) {
            if (matchKeywordCI(statement, i, "THEN")) {
                size_t afterThen = i + 4;
                while (afterThen < statement.size() &&
                       std::isspace(static_cast<unsigned char>(statement[afterThen])))
                    ++afterThen;
                if (afterThen >= statement.size())
                    ++blockDepth;
                foundThen = true;
                break;
            }
        }
        if (!foundThen)
            ++blockDepth;
        return;
    }

    if (matchKeywordCI(statement, pos, "DO") || matchKeywordCI(statement, pos, "WHILE") ||
        matchKeywordCI(statement, pos, "FOR") || matchKeywordCI(statement, pos, "SELECT") ||
        matchKeywordCI(statement, pos, "CLASS") || matchKeywordCI(statement, pos, "TYPE") ||
        matchKeywordCI(statement, pos, "TRY") || matchKeywordCI(statement, pos, "PROPERTY") ||
        matchKeywordCI(statement, pos, "NAMESPACE")) {
        ++blockDepth;
    }
}

// ---------------------------------------------------------------------------
// Zia classifier (bracket depth)
// ---------------------------------------------------------------------------

/// @brief Classify a Zia REPL submission by lexical delimiter balance.
/// @details Dot-prefixed input is a meta command. Delimiters inside string
///          literals and `//` line comments are ignored, and backslash escapes
///          prevent a quote from closing a string.
/// @param input Accumulated Zia submission.
/// @return `Empty`, `MetaCommand`, `Incomplete` for an open string or positive
///         delimiter depth, otherwise `Complete`.
InputKind ReplInputClassifier::classify(const std::string &input) {
    if (isAllWhitespace(input))
        return InputKind::Empty;

    size_t firstNonSpace = findFirstNonSpace(input);
    if (firstNonSpace < input.size() && input[firstNonSpace] == '.')
        return InputKind::MetaCommand;

    int braceDepth = 0;
    int parenDepth = 0;
    int bracketDepth = 0;
    bool inString = false;
    bool escape = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (escape) {
            escape = false;
            continue;
        }

        if (c == '\\' && inString) {
            escape = true;
            continue;
        }

        if (c == '"') {
            inString = !inString;
            continue;
        }

        if (inString)
            continue;

        // Skip single-line comments without ignoring later accumulated lines.
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            i = input.find('\n', i + 2);
            if (i == std::string::npos)
                break;
            continue;
        }

        switch (c) {
            case '{':
                ++braceDepth;
                break;
            case '}':
                --braceDepth;
                break;
            case '(':
                ++parenDepth;
                break;
            case ')':
                --parenDepth;
                break;
            case '[':
                ++bracketDepth;
                break;
            case ']':
                --bracketDepth;
                break;
            default:
                break;
        }
    }

    if (inString || braceDepth > 0 || parenDepth > 0 || bracketDepth > 0)
        return InputKind::Incomplete;

    return InputKind::Complete;
}

// ---------------------------------------------------------------------------
// BASIC classifier (block keyword tracking)
// ---------------------------------------------------------------------------

/// @brief Classify a BASIC REPL submission by string and block completeness.
/// @details Each physical line is checked for unterminated string state,
///          stripped of apostrophe/REM comments, split at out-of-string colons,
///          and applied to a shallow block-depth counter.
/// @param input Accumulated BASIC submission.
/// @return `Empty`, `MetaCommand`, `Incomplete` for an open string or positive
///         block depth, otherwise `Complete`.
InputKind ReplInputClassifier::classifyBasic(const std::string &input) {
    if (isAllWhitespace(input))
        return InputKind::Empty;

    size_t firstNonSpace = findFirstNonSpace(input);
    if (firstNonSpace < input.size() && input[firstNonSpace] == '.')
        return InputKind::MetaCommand;

    int blockDepth = 0;
    bool unterminatedString = false;

    size_t lineStart = 0;
    while (lineStart <= input.size()) {
        size_t lineEnd = input.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = input.size();

        std::string line = input.substr(lineStart, lineEnd - lineStart);
        if (basicLineEndsInsideString(line))
            unterminatedString = true;

        for (auto statement : splitBasicStatements(stripBasicComment(line))) {
            applyBasicStatementDepth(std::move(statement), blockDepth);
        }

        lineStart = lineEnd + 1;
    }

    if (unterminatedString || blockDepth > 0)
        return InputKind::Incomplete;

    return InputKind::Complete;
}

} // namespace zanna::repl
