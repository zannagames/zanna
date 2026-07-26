//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PlistGenerator.cpp
// Purpose: Generate macOS Info.plist XML and PkgInfo for .app bundles.
//
// Key invariants:
//   - XML output uses explicit entity escaping for &, <, >, in values.
//   - Empty optional fields are omitted from output.
//
// Ownership/Lifetime:
//   - Pure functions, no state.
//
// Links: PlistGenerator.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic macOS application metadata generation.
/// @details Inputs are validated as package metadata, XML text is escaped
///          explicitly, and optional keys are omitted when their values are empty.

#include "PlistGenerator.hpp"
#include "PkgUtils.hpp"

#include <cctype>
#include <sstream>

namespace zanna::pkg {

/// @brief Escape XML special characters in a string value.
/// @details Replaces ampersand, angle brackets, and double quote with their XML
///          entity references; all other bytes are copied unchanged.
/// @param s Plain text to embed in an XML text node.
/// @return Escaped text without surrounding XML tags.
static std::string xmlEscape(const std::string &s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':
                result += "&amp;";
                break;
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            case '"':
                result += "&quot;";
                break;
            default:
                result += c;
                break;
        }
    }
    return result;
}

/// @brief Return the LaunchServices role for a file association.
/// @details Source/text-like MIME types are advertised as editable documents,
///          while binary or unknown types default to Viewer so packaged apps do
///          not overstate their document-editing capability.
/// @param assoc File association metadata from the package manifest.
/// @return `Editor` for text/source MIME types, otherwise `Viewer`.
static std::string documentRoleFor(const FileAssoc &assoc) {
    if (assoc.mimeType.rfind("text/", 0) == 0)
        return "Editor";
    if (assoc.mimeType.find("source") != std::string::npos)
        return "Editor";
    return "Viewer";
}

/// @brief Return the parent UTI for a file association.
/// @details LaunchServices uses conformance to group document types. Text and
///          source MIME types conform to public.text so editors/search tools can
///          treat them as textual; everything else uses public.data.
/// @param assoc File association metadata from the package manifest.
/// @return Parent UTI identifier for UTTypeConformsTo.
static std::string utiConformanceFor(const FileAssoc &assoc) {
    if (assoc.mimeType.rfind("text/", 0) == 0)
        return "public.text";
    if (assoc.mimeType.find("source") != std::string::npos)
        return "public.source-code";
    return "public.data";
}

/// @brief Generate a complete macOS Info.plist for an .app bundle.
/// @details Validates scalar fields and file associations first, then emits the
///          PropertyList-1.0 XML with required `CFBundle*` keys, optional icon
///          and category entries, and both document and exported-UTI declarations
///          when file associations are present.
/// @param params Validated values to encode in the property list.
/// @return Complete UTF-8 XML document with a trailing newline.
/// @throws std::runtime_error If a scalar or association violates package
///         metadata syntax or safety requirements.
std::string generatePlist(const PlistParams &params) {
    validateSingleLineField(params.executableName, "macOS bundle executable name");
    validateMacOSBundleIdentifier(params.bundleId, "macOS bundle identifier");
    validateSingleLineField(params.bundleName, "macOS bundle name");
    validateDottedNumericVersion(params.version, "macOS bundle version");
    if (!params.minOsVersion.empty())
        validateDottedNumericVersion(params.minOsVersion, "minimum macOS version");
    validateSingleLineField(params.appCategory, "macOS application category");
    validatePackageFileAssociations(params.fileAssociations);
    std::ostringstream os;

    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\"\n"
       << "  \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       << "<plist version=\"1.0\">\n"
       << "<dict>\n";

    // Required keys
    os << "  <key>CFBundleExecutable</key>\n"
       << "  <string>" << xmlEscape(params.executableName) << "</string>\n";

    os << "  <key>CFBundleIdentifier</key>\n"
       << "  <string>" << xmlEscape(params.bundleId) << "</string>\n";

    os << "  <key>CFBundleName</key>\n"
       << "  <string>" << xmlEscape(params.bundleName) << "</string>\n";

    os << "  <key>CFBundleVersion</key>\n"
       << "  <string>" << xmlEscape(params.version) << "</string>\n";

    os << "  <key>CFBundleShortVersionString</key>\n"
       << "  <string>" << xmlEscape(params.version) << "</string>\n";

    os << "  <key>CFBundlePackageType</key>\n"
       << "  <string>APPL</string>\n";

    os << "  <key>CFBundleSignature</key>\n"
       << "  <string>" << "??" << "??" << "</string>\n";

    // Icon
    if (!params.iconFile.empty()) {
        os << "  <key>CFBundleIconFile</key>\n"
           << "  <string>" << xmlEscape(params.iconFile) << "</string>\n";
    }

    if (!params.appCategory.empty()) {
        os << "  <key>LSApplicationCategoryType</key>\n"
           << "  <string>" << xmlEscape(params.appCategory) << "</string>\n";
    }

    // Minimum OS version
    std::string minOs = params.minOsVersion.empty() ? "10.13" : params.minOsVersion;
    os << "  <key>LSMinimumSystemVersion</key>\n"
       << "  <string>" << xmlEscape(minOs) << "</string>\n";

    // Retina support
    os << "  <key>NSHighResolutionCapable</key>\n"
       << "  <true/>\n";

    // File associations
    if (!params.fileAssociations.empty()) {
        os << "  <key>CFBundleDocumentTypes</key>\n"
           << "  <array>\n";
        for (const auto &fa : params.fileAssociations) {
            os << "    <dict>\n";
            // Extension (strip leading dot)
            std::string ext = fa.extension;
            if (!ext.empty() && ext[0] == '.')
                ext = ext.substr(1);
            os << "      <key>CFBundleTypeExtensions</key>\n"
               << "      <array><string>" << xmlEscape(ext) << "</string></array>\n";
            os << "      <key>CFBundleTypeName</key>\n"
               << "      <string>" << xmlEscape(fa.description) << "</string>\n";
            os << "      <key>CFBundleTypeRole</key>\n"
               << "      <string>" << documentRoleFor(fa) << "</string>\n";
            os << "      <key>LSHandlerRank</key>\n"
               << "      <string>Alternate</string>\n";
            os << "    </dict>\n";
        }
        os << "  </array>\n";
    }

    // UTExportedTypeDeclarations for custom file types
    if (!params.fileAssociations.empty()) {
        os << "  <key>UTExportedTypeDeclarations</key>\n"
           << "  <array>\n";
        for (const auto &fa : params.fileAssociations) {
            std::string ext = fa.extension;
            if (!ext.empty() && ext[0] == '.')
                ext = ext.substr(1);
            // UTI: reverse-DNS identifier derived from bundle ID + extension
            std::string uti = params.bundleId + "." + ext;
            os << "    <dict>\n";
            os << "      <key>UTTypeIdentifier</key>\n"
               << "      <string>" << xmlEscape(uti) << "</string>\n";
            os << "      <key>UTTypeDescription</key>\n"
               << "      <string>" << xmlEscape(fa.description) << "</string>\n";
            os << "      <key>UTTypeConformsTo</key>\n"
               << "      <array><string>" << xmlEscape(utiConformanceFor(fa))
               << "</string></array>\n";
            os << "      <key>UTTypeTagSpecification</key>\n"
               << "      <dict>\n";
            os << "        <key>public.filename-extension</key>\n"
               << "        <array><string>" << xmlEscape(ext) << "</string></array>\n";
            if (!fa.mimeType.empty()) {
                os << "        <key>public.mime-type</key>\n"
                   << "        <array><string>" << xmlEscape(fa.mimeType) << "</string></array>\n";
            }
            os << "      </dict>\n";
            os << "    </dict>\n";
        }
        os << "  </array>\n";
    }

    os << "</dict>\n"
       << "</plist>\n";

    return os.str();
}

/// @brief Return the 8-byte macOS PkgInfo file content "APPL????".
/// Every .app bundle requires this file in Contents/ alongside Info.plist.
/// The first four bytes are the package type (APPL = application) and the
/// second four are the legacy creator code (all '?' = unregistered).
/// @return Exactly eight ASCII bytes with no terminator or trailing newline.
std::string generatePkgInfo() {
    return "APPL????";
}

} // namespace zanna::pkg
