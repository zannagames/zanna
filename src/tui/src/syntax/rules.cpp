//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the lightweight JSON loader and regular-expression engine used by
// the TUI syntax highlighter.  The loader reads a small configuration file that
// maps patterns to styles and compiles them into a set of reusable rules.  Each
// line of text can then be highlighted by running the rules and caching the
// resulting spans for incremental updates.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the SyntaxRuleSet JSON parser and highlighting routines.
/// @details The file defines a minimal JSON parser tailored to the tool's
///          configuration format and uses it to populate regular-expression
///          rules.  Highlighting requests consult the compiled rules and cache
///          the spans per line so repeated queries remain fast.

#include "tui/syntax/rules.hpp"

#include "tui/util/color.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace zanna::tui::syntax {
namespace {
/// @brief Minimal JSON helper for parsing the syntax rule configuration.
/// @details The parser understands just enough of JSON to process arrays of
///          objects containing string and boolean values.  It operates on an
///          in-memory string and keeps track of the current index.
struct JsonParser {
    const std::string &s; ///< Borrowed complete JSON input.
    std::size_t i{0};     ///< Current byte offset within @c s.

    /// @brief Advance past any ASCII whitespace characters.
    /// @details Consumes spaces, tabs, and newlines by incrementing the parser
    ///          index.  The helper is invoked before reading every token to keep
    ///          the parsing logic straightforward.
    void skipWs() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }

    /// @brief Expect the next non-whitespace character to match @p c.
    /// @details Skips whitespace, checks the current character, and advances the
    ///          index on success.  Failure leaves the parser untouched so callers
    ///          can signal an error.
    /// @param c Delimiter byte required at the current token position.
    /// @return True when the expected delimiter is present, false otherwise.
    bool expect(char c) {
        skipWs();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }

    /// @brief Parse a JSON string literal and return its decoded contents.
    /// @details Handles common escape sequences (quotes, backslashes, newline,
    ///          carriage return, and tab).  On malformed input the function
    ///          returns an empty string, allowing callers to propagate failure.
    /// @return Decoded string bytes, or an empty string for malformed input.
    std::string parseString() {
        skipWs();
        std::string out;
        if (i >= s.size() || s[i] != '"')
            return out;
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"')
                break;
            if (c == '\\' && i < s.size()) {
                char esc = s[i++];
                switch (esc) {
                    case '"':
                        out.push_back('"');
                        break;
                    case '\\':
                        out.push_back('\\');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    default:
                        out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    /// @brief Parse a boolean literal from the input.
    /// @details Recognises `true` and `false` tokens, advancing the index when a
    ///          match is found.  Unrecognised input leaves the index unchanged
    ///          and returns false so callers can treat it as an error.
    /// @return true only for a parsed `true` literal.
    bool parseBool() {
        skipWs();
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            return true;
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            return false;
        }
        return false;
    }
};

} // namespace

/// @brief Load syntax rules from a JSON file located at @p path.
/// @details Reads the entire file into memory, feeds it through the lightweight
///          parser, and builds a list of regular-expression rules paired with
///          styles.  The routine validates the expected structure and aborts on
///          malformed input.  Successfully parsed rules replace any existing
///          configuration.
/// @param path Filesystem path of the syntax-rule JSON document.
/// @return True on success; false when the file cannot be opened or parsed.
bool SyntaxRuleSet::loadFromFile(const std::string &path) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    JsonParser p{data};
    if (!p.expect('['))
        return false;
    while (true) {
        p.skipWs();
        if (p.i >= data.size())
            return false;
        if (data[p.i] == ']') {
            ++p.i;
            break;
        }
        if (!p.expect('{'))
            return false;
        std::string regexStr;
        render::Style style{};
        while (true) {
            p.skipWs();
            std::string key = p.parseString();
            if (key.empty())
                return false;
            if (!p.expect(':'))
                return false;
            if (key == "regex") {
                regexStr = p.parseString();
            } else if (key == "style") {
                if (!p.expect('{'))
                    return false;
                while (true) {
                    p.skipWs();
                    if (p.i >= data.size())
                        return false;
                    if (data[p.i] == '}') {
                        ++p.i;
                        break;
                    }
                    std::string skey = p.parseString();
                    if (!p.expect(':'))
                        return false;
                    if (skey == "fg") {
                        (void)util::parseHexColor(p.parseString(), style.fg);
                    } else if (skey == "bold") {
                        if (p.parseBool())
                            style.attrs |= render::Attr::Bold;
                    } else {
                        return false;
                    }
                    p.skipWs();
                    if (p.i >= data.size())
                        return false;
                    if (data[p.i] == ',') {
                        ++p.i;
                        continue;
                    }
                    if (p.i >= data.size())
                        return false;
                    if (data[p.i] == '}') {
                        ++p.i;
                        break;
                    }
                }
            } else {
                return false;
            }
            p.skipWs();
            if (p.i >= data.size())
                return false;
            if (data[p.i] == ',') {
                ++p.i;
                continue;
            }
            if (p.i >= data.size())
                return false;
            if (data[p.i] == '}') {
                ++p.i;
                break;
            }
        }
        if (!regexStr.empty()) {
            rules_.push_back(SyntaxRule{std::regex(regexStr), style});
        }
        p.skipWs();
        if (p.i >= data.size())
            return false;
        if (data[p.i] == ',') {
            ++p.i;
            continue;
        }
        if (p.i >= data.size())
            return false;
        if (data[p.i] == ']') {
            ++p.i;
            break;
        }
    }
    return true;
}

/// @brief Return syntax highlight spans for @p lineNo and @p line content.
/// @details Checks the cache first to reuse previous results when the line text
///          has not changed.  Otherwise it runs every compiled regex against the
///          line, collects spans, stores them in the cache, and returns the
///          stored vector.
/// @param lineNo Zero-based line number used as the cache key.
/// @param line Current line bytes used for invalidation and matching.
/// @return Const reference to cached spans, valid until the cache entry changes.
const std::vector<Span> &SyntaxRuleSet::spans(std::size_t lineNo, const std::string &line) {
    auto it = cache_.find(lineNo);
    if (it != cache_.end() && it->second.first == line)
        return it->second.second;
    std::vector<Span> out;
    for (const auto &rule : rules_) {
        for (std::sregex_iterator m(line.begin(), line.end(), rule.pattern), e; m != e; ++m) {
            out.push_back(Span{static_cast<std::size_t>(m->position()),
                               static_cast<std::size_t>(m->length()),
                               rule.style});
        }
    }
    cache_[lineNo] = {line, out};
    return cache_[lineNo].second;
}

/// @brief Remove cached highlight spans for the specified line.
/// @details Deleting the entry forces the next @ref spans call to recompute the
///          matches, ensuring stale results do not persist after a line changes.
/// @param lineNo Zero-based cache key to erase.
void SyntaxRuleSet::invalidate(std::size_t lineNo) {
    cache_.erase(lineNo);
}

} // namespace zanna::tui::syntax
