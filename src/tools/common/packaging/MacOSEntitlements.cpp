//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/MacOSEntitlements.cpp
// Purpose: Parse XML property lists into a small value tree, merge required
//          boolean entitlements, and serialize the result in the Apple plist
//          XML form accepted by codesign.
// Key invariants:
//   - The reader accepts the plist DTD subset: dict, array, string, integer,
//     real, date, data, true, and false, plus XML declarations, a DOCTYPE,
//     comments, CDATA, and the five predefined and numeric character
//     references. Anything else is a parse error with a byte offset.
//   - Nesting depth is bounded so hostile input cannot exhaust the stack.
// Ownership/Lifetime:
//   - Parsed trees and output strings are owned values local to one call.
// Links: MacOSEntitlements.hpp, docs/adr/0354-store-depot-packaging.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements macOS entitlements merging.

#include "MacOSEntitlements.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace zanna::pkg {
namespace {

/// @brief Deepest dict/array nesting accepted.
constexpr int kMaxPlistDepth = 64;

/// @brief One property list value.
struct PlistValue {
    /// @brief Value type, named after its XML element.
    enum class Kind { Dict, Array, String, Integer, Real, Date, Data, True, False };
    Kind kind{Kind::String};                                 ///< Element type.
    std::string text;                                        ///< Decoded scalar text.
    std::vector<std::pair<std::string, PlistValue>> entries; ///< Dictionary entries in order.
    std::vector<PlistValue> items;                           ///< Array items in order.
};

/// @brief Parse failure carrying a byte offset.
struct PlistParseError : std::runtime_error {
    /// @brief Construct with a message that already includes the offset.
    /// @param message Detail text.
    explicit PlistParseError(const std::string &message) : std::runtime_error(message) {}
};

/// @brief Recursive-descent reader over property list XML text.
class PlistReader {
  public:
    /// @brief Bind the reader to @p text.
    /// @param text Complete property list text.
    explicit PlistReader(std::string_view text) : text_(text) {}

    /// @brief Parse the complete document.
    /// @return The single value inside <plist>.
    /// @throws PlistParseError on malformed input.
    PlistValue parseDocument() {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF)
            pos_ = 3;
        skipMisc();
        std::string name;
        bool selfClosing = false;
        readStartTag(name, selfClosing);
        if (name != "plist" || selfClosing)
            fail("expected <plist>");
        skipMisc();
        PlistValue value = parseValue(0);
        skipMisc();
        expectEndTag("plist");
        skipMisc();
        if (pos_ != text_.size())
            fail("unexpected content after </plist>");
        return value;
    }

  private:
    /// @brief Throw a parse error at the current offset.
    /// @param what Detail text.
    [[noreturn]] void fail(const std::string &what) const {
        throw PlistParseError(what + " at byte " + std::to_string(pos_));
    }

    /// @brief Report whether the unread text starts with @p prefix.
    /// @param prefix Candidate prefix.
    /// @return True on a match.
    bool startsWith(std::string_view prefix) const {
        return text_.substr(pos_, prefix.size()) == prefix;
    }

    /// @brief Skip whitespace, comments, processing instructions, and a DOCTYPE.
    void skipMisc() {
        for (;;) {
            while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                           text_[pos_] == '\r' || text_[pos_] == '\n'))
                ++pos_;
            if (startsWith("<!--")) {
                skipPast("-->", "unterminated comment");
            } else if (startsWith("<?")) {
                skipPast("?>", "unterminated processing instruction");
            } else if (startsWith("<!DOCTYPE")) {
                skipDoctype();
            } else {
                return;
            }
        }
    }

    /// @brief Advance past the next occurrence of @p terminator.
    /// @param terminator Text that ends the construct.
    /// @param what Error detail when it is missing.
    void skipPast(std::string_view terminator, const char *what) {
        const size_t end = text_.find(terminator, pos_);
        if (end == std::string_view::npos)
            fail(what);
        pos_ = end + terminator.size();
    }

    /// @brief Skip a DOCTYPE declaration, including an internal subset.
    void skipDoctype() {
        int brackets = 0;
        for (; pos_ < text_.size(); ++pos_) {
            if (text_[pos_] == '[')
                ++brackets;
            else if (text_[pos_] == ']')
                --brackets;
            else if (text_[pos_] == '>' && brackets <= 0) {
                ++pos_;
                return;
            }
        }
        fail("unterminated DOCTYPE");
    }

    /// @brief Read a start tag such as `<name attr="v">` or `<name/>`.
    /// @param name Receives the element name.
    /// @param selfClosing Receives whether the tag ends with `/>`.
    void readStartTag(std::string &name, bool &selfClosing) {
        if (pos_ >= text_.size() || text_[pos_] != '<' || startsWith("</"))
            fail("expected an element");
        ++pos_;
        const size_t nameStart = pos_;
        while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) ||
                                       text_[pos_] == '-' || text_[pos_] == '_'))
            ++pos_;
        if (pos_ == nameStart)
            fail("expected an element name");
        name = std::string(text_.substr(nameStart, pos_ - nameStart));
        char quote = 0;
        for (; pos_ < text_.size(); ++pos_) {
            const char c = text_[pos_];
            if (quote) {
                if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '>') {
                selfClosing = true;
                pos_ += 2;
                return;
            } else if (c == '>') {
                selfClosing = false;
                ++pos_;
                return;
            } else if (c == '<') {
                break;
            }
        }
        fail("unterminated <" + name + "> tag");
    }

    /// @brief Consume `</name>`.
    /// @param name Expected element name.
    void expectEndTag(const std::string &name) {
        const std::string tag = "</" + name;
        if (!startsWith(tag))
            fail("expected </" + name + ">");
        pos_ += tag.size();
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\r' || text_[pos_] == '\n'))
            ++pos_;
        if (pos_ >= text_.size() || text_[pos_] != '>')
            fail("expected </" + name + ">");
        ++pos_;
    }

    /// @brief Append the UTF-8 encoding of @p cp to @p out.
    /// @param out Destination.
    /// @param cp Unicode scalar value.
    void appendUtf8(std::string &out, uint32_t cp) {
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            fail("invalid character reference");
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    /// @brief Decode one character reference starting at `&`.
    /// @param out Destination for the decoded text.
    void readReference(std::string &out) {
        const size_t end = text_.find(';', pos_);
        if (end == std::string_view::npos || end - pos_ > 12)
            fail("unterminated character reference");
        const std::string_view ref = text_.substr(pos_ + 1, end - pos_ - 1);
        if (ref == "lt")
            out.push_back('<');
        else if (ref == "gt")
            out.push_back('>');
        else if (ref == "amp")
            out.push_back('&');
        else if (ref == "quot")
            out.push_back('"');
        else if (ref == "apos")
            out.push_back('\'');
        else if (ref.size() > 1 && ref[0] == '#') {
            const bool hex = ref[1] == 'x' || ref[1] == 'X';
            const std::string_view digits = ref.substr(hex ? 2 : 1);
            if (digits.empty())
                fail("invalid character reference");
            uint32_t cp = 0;
            for (char c : digits) {
                uint32_t digit = 0;
                if (c >= '0' && c <= '9')
                    digit = static_cast<uint32_t>(c - '0');
                else if (hex && c >= 'a' && c <= 'f')
                    digit = static_cast<uint32_t>(c - 'a' + 10);
                else if (hex && c >= 'A' && c <= 'F')
                    digit = static_cast<uint32_t>(c - 'A' + 10);
                else
                    fail("invalid character reference");
                cp = cp * (hex ? 16u : 10u) + digit;
                if (cp > 0x10FFFF)
                    fail("invalid character reference");
            }
            appendUtf8(out, cp);
        } else {
            fail("unknown entity '&" + std::string(ref) + ";'");
        }
        pos_ = end + 1;
    }

    /// @brief Read character data up to the closing tag of @p name.
    /// @param name Element being read.
    /// @return Decoded text.
    std::string readText(const std::string &name) {
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '&') {
                readReference(out);
            } else if (startsWith("<![CDATA[")) {
                const size_t start = pos_ + 9;
                const size_t end = text_.find("]]>", start);
                if (end == std::string_view::npos)
                    fail("unterminated CDATA section");
                out.append(text_.substr(start, end - start));
                pos_ = end + 3;
            } else if (startsWith("<!--")) {
                skipPast("-->", "unterminated comment");
            } else if (c == '<') {
                expectEndTag(name);
                return out;
            } else {
                out.push_back(c);
                ++pos_;
            }
        }
        fail("unterminated <" + name + ">");
    }

    /// @brief Parse one value element.
    /// @param depth Current nesting depth.
    /// @return Parsed value.
    PlistValue parseValue(int depth) {
        if (depth > kMaxPlistDepth)
            fail("nesting is too deep");
        std::string name;
        bool selfClosing = false;
        readStartTag(name, selfClosing);
        PlistValue value;
        if (name == "true" || name == "false") {
            value.kind = name == "true" ? PlistValue::Kind::True : PlistValue::Kind::False;
            if (!selfClosing) {
                skipMisc();
                expectEndTag(name);
            }
            return value;
        }
        if (name == "dict") {
            value.kind = PlistValue::Kind::Dict;
            if (selfClosing)
                return value;
            std::set<std::string> keys;
            for (;;) {
                skipMisc();
                if (startsWith("</")) {
                    expectEndTag("dict");
                    return value;
                }
                std::string keyTag;
                bool keySelfClosing = false;
                readStartTag(keyTag, keySelfClosing);
                if (keyTag != "key")
                    fail("expected <key> in <dict>, found <" + keyTag + ">");
                std::string key = keySelfClosing ? std::string() : readText("key");
                if (!keys.insert(key).second)
                    fail("duplicate key '" + key + "'");
                skipMisc();
                PlistValue entry = parseValue(depth + 1);
                value.entries.emplace_back(std::move(key), std::move(entry));
            }
        }
        if (name == "array") {
            value.kind = PlistValue::Kind::Array;
            if (selfClosing)
                return value;
            for (;;) {
                skipMisc();
                if (startsWith("</")) {
                    expectEndTag("array");
                    return value;
                }
                value.items.push_back(parseValue(depth + 1));
            }
        }
        if (name == "string")
            value.kind = PlistValue::Kind::String;
        else if (name == "integer")
            value.kind = PlistValue::Kind::Integer;
        else if (name == "real")
            value.kind = PlistValue::Kind::Real;
        else if (name == "date")
            value.kind = PlistValue::Kind::Date;
        else if (name == "data")
            value.kind = PlistValue::Kind::Data;
        else
            fail("unsupported element <" + name + ">");
        if (!selfClosing)
            value.text = readText(name);
        return value;
    }

    std::string_view text_; ///< Document text.
    size_t pos_{0};         ///< Read offset.
};

/// @brief Escape character data for XML output.
/// @param text Raw text.
/// @return Escaped text.
std::string xmlText(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out.push_back(c);
    }
    return out;
}

/// @brief Serialize one value with tab indentation.
/// @param out Destination stream.
/// @param value Value to write.
/// @param indent Current indentation depth.
void writeValue(std::ostringstream &out, const PlistValue &value, int indent) {
    const std::string tabs(static_cast<size_t>(indent), '\t');
    switch (value.kind) {
        case PlistValue::Kind::Dict:
            if (value.entries.empty()) {
                out << tabs << "<dict/>\n";
                return;
            }
            out << tabs << "<dict>\n";
            for (const auto &[key, entry] : value.entries) {
                out << tabs << "\t<key>" << xmlText(key) << "</key>\n";
                writeValue(out, entry, indent + 1);
            }
            out << tabs << "</dict>\n";
            return;
        case PlistValue::Kind::Array:
            if (value.items.empty()) {
                out << tabs << "<array/>\n";
                return;
            }
            out << tabs << "<array>\n";
            for (const auto &item : value.items)
                writeValue(out, item, indent + 1);
            out << tabs << "</array>\n";
            return;
        case PlistValue::Kind::True:
            out << tabs << "<true/>\n";
            return;
        case PlistValue::Kind::False:
            out << tabs << "<false/>\n";
            return;
        case PlistValue::Kind::String:
            out << tabs << "<string>" << xmlText(value.text) << "</string>\n";
            return;
        case PlistValue::Kind::Integer:
            out << tabs << "<integer>" << xmlText(value.text) << "</integer>\n";
            return;
        case PlistValue::Kind::Real:
            out << tabs << "<real>" << xmlText(value.text) << "</real>\n";
            return;
        case PlistValue::Kind::Date:
            out << tabs << "<date>" << xmlText(value.text) << "</date>\n";
            return;
        case PlistValue::Kind::Data:
            out << tabs << "<data>" << xmlText(value.text) << "</data>\n";
            return;
    }
}

/// @brief Describe a value for conflict diagnostics.
/// @param value Existing value.
/// @return "false", "a string", "an array", and similar.
std::string describeValue(const PlistValue &value) {
    switch (value.kind) {
        case PlistValue::Kind::Dict:
            return "a dictionary";
        case PlistValue::Kind::Array:
            return "an array";
        case PlistValue::Kind::String:
            return "a string";
        case PlistValue::Kind::Integer:
            return "an integer";
        case PlistValue::Kind::Real:
            return "a real number";
        case PlistValue::Kind::Date:
            return "a date";
        case PlistValue::Kind::Data:
            return "data";
        case PlistValue::Kind::True:
            return "true";
        case PlistValue::Kind::False:
            return "false";
    }
    return "an unknown value";
}

/// @brief Report whether @p text contains only XML whitespace.
/// @param text Candidate text.
/// @return True for empty or whitespace-only text.
bool isBlank(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    });
}

} // namespace

/// @brief Merge required boolean entitlements into property list text.
/// @param sourceText Existing plist text, or empty for none.
/// @param sourceLabel File name for diagnostics.
/// @param requiredTrue Keys forced to true.
/// @param forbiddenTrue Keys that must not be true.
/// @param storeName Store display name for diagnostics.
/// @return Merged XML and added keys.
EntitlementsMergeResult mergeMacOSEntitlements(std::string_view sourceText,
                                               const std::string &sourceLabel,
                                               const std::vector<std::string> &requiredTrue,
                                               const std::vector<std::string> &forbiddenTrue,
                                               const std::string &storeName) {
    const std::string prefix = "macOS entitlements '" + sourceLabel + "' ";
    PlistValue root;
    root.kind = PlistValue::Kind::Dict;
    if (!isBlank(sourceText)) {
        if (sourceText.substr(0, 6) == "bplist")
            throw std::runtime_error(prefix + "is a binary property list; convert it with 'plutil "
                                              "-convert xml1'");
        try {
            root = PlistReader(sourceText).parseDocument();
        } catch (const PlistParseError &ex) {
            throw std::runtime_error(prefix + "is not a valid XML property list: " + ex.what());
        }
        if (root.kind != PlistValue::Kind::Dict)
            throw std::runtime_error(prefix +
                                     "is not a valid XML property list: the top-level value "
                                     "must be a dictionary");
    }

    for (const auto &[key, value] : root.entries) {
        if (std::find(forbiddenTrue.begin(), forbiddenTrue.end(), key) != forbiddenTrue.end() &&
            value.kind == PlistValue::Kind::True)
            throw std::runtime_error(prefix + "enable " + key + ", which " + storeName +
                                     " does not support");
        if (std::find(requiredTrue.begin(), requiredTrue.end(), key) != requiredTrue.end() &&
            value.kind != PlistValue::Kind::True)
            throw std::runtime_error(prefix + "set " + key + " to " + describeValue(value) +
                                     ", but " + storeName + " requires true");
    }

    EntitlementsMergeResult result;
    for (const auto &key : requiredTrue) {
        const bool present = std::any_of(root.entries.begin(),
                                         root.entries.end(),
                                         [&](const auto &entry) { return entry.first == key; });
        if (present)
            continue;
        PlistValue trueValue;
        trueValue.kind = PlistValue::Kind::True;
        root.entries.emplace_back(key, std::move(trueValue));
        result.addedKeys.push_back(key);
    }

    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        << "<plist version=\"1.0\">\n";
    writeValue(out, root, 0);
    out << "</plist>\n";
    result.xml = out.str();
    return result;
}

} // namespace zanna::pkg
