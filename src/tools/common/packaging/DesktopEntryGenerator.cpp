//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/DesktopEntryGenerator.cpp
// Purpose: Generate freedesktop.org .desktop files and MIME type XML.
//
// Key invariants:
//   - .desktop follows [Desktop Entry] group with Type=Application.
//   - MIME XML conforms to http://www.freedesktop.org/standards/shared-mime-info.
//
// Ownership/Lifetime:
//   - Pure string generation, no side effects.
//
// Links: DesktopEntryGenerator.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements escaped freedesktop desktop-entry and MIME XML generation.
/// @details Manifest metadata is validated before emission, executable arguments
///          are parsed into literal tokens, reserved syntax is escaped for the
///          destination format, and output ordering remains deterministic.

#include "DesktopEntryGenerator.hpp"
#include "PkgUtils.hpp"

#include <cctype>
#include <sstream>
#include <utility>
#include <vector>

namespace zanna::pkg {

namespace {

/// @brief Escape a value string for inclusion in a .desktop file field.
/// @details Validates that the value is single-line, because each physical line
///          is a separate key/value record, then doubles backslashes required by
///          desktop-entry string-value syntax.
/// @param s Raw manifest field value.
/// @return Escaped single-line field text.
/// @throws std::runtime_error When @p s contains forbidden line breaks.
std::string desktopEscape(const std::string &s) {
    validateSingleLineField(s, "desktop entry field");
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\')
            out += "\\\\";
        else
            out.push_back(c);
    }
    return out;
}

/// @brief Escape one executable or argument token for a freedesktop Exec= field.
/// @details The Desktop Entry specification treats whitespace and shell-like
///          metacharacters as token separators unless the token is quoted. This
///          helper quotes tokens when needed, escapes characters with special
///          meaning inside quoted strings, and doubles literal percent signs so
///          they are not parsed as field codes.
/// @param token Literal executable path or argument token.
/// @param fieldName Name used in diagnostics.
/// @return Token text safe for direct inclusion in Exec=.
std::string desktopEscapeExecToken(const std::string &token, const char *fieldName) {
    validateSingleLineField(token, fieldName);
    if (token.empty())
        throw std::runtime_error(std::string(fieldName) + " must not be empty");
    bool needsQuote = false;
    for (unsigned char c : token) {
        if (std::isspace(c) || c == '"' || c == '\'' || c == '\\' || c == '>' || c == '<' ||
            c == '~' || c == '|' || c == '&' || c == ';' || c == '$' || c == '*' || c == '?' ||
            c == '#' || c == '(' || c == ')' || c == '`') {
            needsQuote = true;
            break;
        }
    }
    std::string escaped;
    escaped.reserve(token.size() + 2u);
    if (needsQuote)
        escaped.push_back('"');
    for (char c : token) {
        if (c == '%') {
            escaped += "%%";
            continue;
        }
        if (needsQuote && (c == '"' || c == '\\' || c == '$' || c == '`'))
            escaped.push_back('\\');
        escaped.push_back(c);
    }
    if (needsQuote)
        escaped.push_back('"');
    return escaped;
}

/// @brief Split an Exec argument string into literal argument tokens.
/// @details This parser accepts whitespace-separated tokens plus double-quoted
///          groups with backslash escapes. The resulting tokens are escaped again
///          by desktopEscapeExecToken(), so callers never append raw command text
///          into an Exec= field.
/// @param text Manifest-provided argument string.
/// @param fieldName Name used in diagnostics.
/// @return Parsed argument tokens.
std::vector<std::string> parseExecArgumentTokens(const std::string &text, const char *fieldName) {
    validateSingleLineField(text, fieldName);
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuote = false;
    bool escaping = false;
    for (char c : text) {
        if (escaping) {
            cur.push_back(c);
            escaping = false;
            continue;
        }
        if (c == '\\' && inQuote) {
            escaping = true;
            continue;
        }
        if (c == '"') {
            inQuote = !inQuote;
            continue;
        }
        if (!inQuote && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (escaping)
        cur.push_back('\\');
    if (inQuote)
        throw std::runtime_error(std::string(fieldName) + " contains an unterminated quote");
    if (!cur.empty())
        tokens.push_back(std::move(cur));
    return tokens;
}

/// @brief Escape a string for safe embedding in XML attribute values and text content.
/// @details Replaces all five predefined XML entity characters so the result is
///          safe in both quoted attributes and element text.
/// @param s Raw text to escape.
/// @return XML-safe text with reserved characters replaced by entity references.
std::string xmlEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

/// @brief Validate all file associations before generating any output.
/// @param assocs Association list to validate as one collection.
/// @details Delegates to validatePackageFileAssociations(), which checks for
///          duplicate extensions, well-formed MIME type strings, and legal
///          extension characters.
/// @throws std::runtime_error On the first invalid or duplicate association.
void validateAssociations(const std::vector<FileAssoc> &assocs) {
    validatePackageFileAssociations(assocs);
}

/// @brief Normalize freedesktop Keywords= metadata.
/// @details Keywords are a semicolon-separated list. This helper trims a
///          manifest-provided value, validates it as single-line text, and adds
///          the trailing semicolon required by desktop-file validators when the
///          caller omitted it.
/// @param keywords Raw keywords string from package metadata.
/// @return Empty string when no keywords were supplied, otherwise a
///         semicolon-terminated Keywords= value.
std::string normalizeDesktopKeywords(const std::string &keywords) {
    const std::string trimmed = trimAsciiWhitespace(keywords);
    if (trimmed.empty())
        return {};
    validateSingleLineField(trimmed, "desktop keywords");
    if (trimmed.find(',') != std::string::npos)
        throw std::runtime_error("desktop keywords must be separated with semicolons");
    if (trimmed.back() == ';')
        return trimmed;
    return trimmed + ";";
}

} // namespace

/// @brief Build a complete freedesktop.org .desktop file from the given parameters.
/// @param params Desktop metadata and execution settings to validate and render.
/// @return Complete `[Desktop Entry]` document with a trailing newline.
/// @details Validates file associations and normalizes categories before writing.
///          Appends `%f` to `Exec=` when associations or
///          @ref DesktopEntryParams::acceptsFileArgument request file opening.
///          Optional keys are omitted when their corresponding values are empty.
/// @throws std::runtime_error When any field or collection is malformed.
std::string generateDesktopEntry(const DesktopEntryParams &params) {
    validateAssociations(params.fileAssociations);
    const std::string categories =
        normalizeDesktopCategories(params.categories.empty() ? "Utility" : params.categories);
    const std::string keywords = normalizeDesktopKeywords(params.keywords);
    std::ostringstream os;
    os << "[Desktop Entry]\n";
    os << "Type=Application\n";
    os << "Name=" << desktopEscape(params.name) << "\n";
    if (!params.comment.empty())
        os << "Comment=" << desktopEscape(params.comment) << "\n";
    os << "Exec=" << desktopEscapeExecToken(params.execPath, "desktop Exec path");
    if (!params.execArguments.empty()) {
        for (const auto &arg :
             parseExecArgumentTokens(params.execArguments, "desktop Exec arguments"))
            os << " " << desktopEscapeExecToken(arg, "desktop Exec argument");
    }
    if (params.acceptsFileArgument || !params.fileAssociations.empty())
        os << " %f";
    os << "\n";
    if (!params.iconName.empty())
        os << "Icon=" << desktopEscape(params.iconName) << "\n";
    if (!categories.empty())
        os << "Categories=" << desktopEscape(categories) << "\n";
    if (!params.workingDir.empty())
        os << "Path=" << desktopEscape(params.workingDir) << "\n";
    if (!params.startupWmClass.empty())
        os << "StartupWMClass=" << desktopEscape(params.startupWmClass) << "\n";
    if (!keywords.empty())
        os << "Keywords=" << desktopEscape(keywords) << "\n";
    if (params.noDisplay)
        os << "NoDisplay=true\n";
    os << "Terminal=" << (params.terminal ? "true" : "false") << "\n";

    // MimeType field for file associations
    if (!params.fileAssociations.empty()) {
        os << "MimeType=";
        for (size_t i = 0; i < params.fileAssociations.size(); i++) {
            os << desktopEscape(params.fileAssociations[i].mimeType) << ";";
        }
        os << "\n";
    }

    return os.str();
}

/// @brief Build a shared-mime-info XML document registering each file association.
/// @param packageName Reserved package identifier; currently unused.
/// @param assocs Validated extension, MIME type, and description records.
/// @return Complete XML document, or an empty string when @p assocs is empty.
/// @details Each association produces a `<mime-type>` element with a comment and
///          glob pattern; a missing leading dot is added to the extension.
/// @throws std::runtime_error When association validation fails.
std::string generateMimeTypeXml(const std::string &packageName,
                                const std::vector<FileAssoc> &assocs) {
    if (assocs.empty())
        return {};

    (void)packageName;
    validateAssociations(assocs);

    std::ostringstream os;
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    os << "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n";

    for (const auto &a : assocs) {
        os << "  <mime-type type=\"" << xmlEscape(a.mimeType) << "\">\n";
        os << "    <comment>" << xmlEscape(a.description) << "</comment>\n";

        // Extension without leading dot for the glob pattern
        std::string ext = a.extension;
        if (!ext.empty() && ext[0] != '.')
            ext = "." + ext;
        os << "    <glob pattern=\"*" << xmlEscape(ext) << "\"/>\n";
        os << "  </mime-type>\n";
    }

    os << "</mime-info>\n";
    return os.str();
}

} // namespace zanna::pkg
