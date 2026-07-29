//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/project_loader.cpp
// Purpose: Universal project system — manifest parsing, convention-based
//          file discovery, language detection, and entry point resolution.
// Key invariants: A successfully resolved ProjectConfig has a non-empty
//                 entryFile that points at an existing source file and a lang
//                 consistent with the discovered sources. Manifest directives
//                 are line-oriented; scalar directives may appear at most once.
// Ownership/Lifetime: All helpers return values or Expected<>; the caller owns
//                     the resulting ProjectConfig. No global state is mutated.
// Links: src/tools/common/project_loader.hpp,
//        src/tools/common/packaging/PkgUtils.hpp, docs/internals/codemap.md#tools
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements zanna.project manifest parsing and convention-based project
///        discovery shared by the Zia and BASIC frontends.
/// @details Provides two entry points: resolveProject() classifies a CLI target
///          (file, directory, or manifest) and either discovers sources by
///          convention or delegates to parseManifest(), which interprets the
///          line-oriented manifest grammar including the package-* directive
///          family.

#include "tools/common/project_loader.hpp"
#include "tools/common/packaging/PkgUtils.hpp"

#include "common/Filesystem.hpp"
#include "frontends/basic/Lexer.hpp"
#include "frontends/basic/Token.hpp"
#include "frontends/zia/Lexer.hpp"
#include "frontends/zia/Token.hpp"
#include "support/diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace il::tools::common {

namespace {

namespace basic = il::frontends::basic;
namespace zia = il::frontends::zia;

inline constexpr std::uint64_t kMaxManifestHookScriptBytes = 1024u * 1024u;
inline constexpr std::streamoff kMaxConventionScanBytes = 1024 * 1024;

/// @brief Encode a native path as a Zanna UTF-8 path string.
/// @param path Native filesystem path.
/// @return Platform-native path syntax encoded as UTF-8.
std::string pathText(const fs::path &path) {
    return zanna::filesystem::pathToUtf8(path);
}

/// @brief Encode a native path with portable separators for manifest matching.
/// @param path Native filesystem path.
/// @return UTF-8 path using forward-slash separators.
std::string genericPathText(const fs::path &path) {
    return zanna::filesystem::genericPathToUtf8(path);
}

/// @brief Make a diagnostic error with a message.
/// @param msg Human-readable error text.
/// @return Error-severity diagnostic without a source range.
il::support::Diag makeErr(const std::string &msg) {
    return il::support::Diagnostic{il::support::Severity::Error, msg, {}, {}};
}

/// @brief Make a diagnostic error with file:line context.
/// @param path Manifest path.
/// @param line One-based manifest line number.
/// @param msg Human-readable error text.
/// @return Error diagnostic whose message begins with `path:line:`.
il::support::Diag makeManifestErr(const std::string &path, int line, const std::string &msg) {
    std::string full = path + ":" + std::to_string(line) + ": " + msg;
    return il::support::Diagnostic{il::support::Severity::Error, full, {}, {}};
}

/// @brief Test whether a single path segment matches an exclude pattern.
/// @details An exact match always counts; a pattern ending in '-' (e.g. "build-")
///          matches any segment that starts with it, so build directories like
///          "build-debug" are excluded.
/// @param segment One project-relative path component.
/// @param exclude Exclude pattern to compare.
/// @return true for an exact match or the documented trailing-hyphen prefix form.
bool pathSegmentMatchesExclude(std::string_view segment, std::string_view exclude) {
    if (segment == exclude)
        return true;
    return !exclude.empty() && exclude.back() == '-' && segment.rfind(exclude, 0) == 0;
}

/// @brief Test whether a project-relative path is covered by an exclude pattern.
/// @details A slash-bearing exclude matches the path or a directory prefix of it;
///          a slash-free exclude matches if any single path segment matches it
///          (via pathSegmentMatchesExclude).
/// @param relativePath Project-relative path.
/// @param exclude Normalized exclude pattern.
/// @return true when the full path/prefix or one component matches.
bool relativePathMatchesExclude(const fs::path &relativePath, const std::string &exclude) {
    const std::string rel = genericPathText(relativePath);
    if (rel == exclude || rel.rfind(exclude + "/", 0) == 0)
        return true;

    if (exclude.find('/') != std::string::npos)
        return false;

    for (const auto &part : relativePath) {
        if (pathSegmentMatchesExclude(genericPathText(part), exclude))
            return true;
    }
    return false;
}

/// @brief Return an ASCII-lowercased copy of @p value.
/// @param value Text to normalize in place.
/// @return Lowercase copy.
std::string lowerAscii(std::string value) {
    /// @brief Fold one byte through the locale-aware ctype function safely.
    /// @param c Unsigned source byte.
    /// @return Lowercase byte converted back to plain char.
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

/// @brief Recursively collect files with a given extension under a directory.
/// @param dir Root directory to scan.
/// @param ext Extension including dot (e.g. ".zia").
/// @param excludes Directories to skip (relative to dir or absolute).
/// @param err Optional destination for traversal or canonicalization errors.
/// @return Sorted list of absolute file paths, or an empty list when @p err is set.
std::vector<std::string> collectFiles(const fs::path &dir,
                                      const std::string &ext,
                                      const std::vector<std::string> &excludes,
                                      std::string *err = nullptr) {
    std::vector<std::string> result;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return result;

    std::vector<std::string> effectiveExcludes = excludes;
    effectiveExcludes.insert(effectiveExcludes.end(),
                             {".git",
                              ".svn",
                              ".hg",
                              "build",
                              "build-",
                              "cmake-build",
                              "node_modules",
                              "vendor",
                              ".zanna-cache"});

    auto it = fs::recursive_directory_iterator(dir, ec);
    if (ec) {
        if (err)
            *err = "cannot traverse source directory " + pathText(dir) + ": " + ec.message();
        return {};
    }
    const fs::path canonicalRoot = fs::canonical(dir, ec);
    if (ec) {
        if (err)
            *err = "cannot resolve source directory " + pathText(dir) + ": " + ec.message();
        return {};
    }
    ec.clear();
    const auto end = fs::recursive_directory_iterator();
    while (!ec && it != end) {
        // Check excludes against the relative path from dir
        std::error_code entryEc;
        if (it->is_directory(entryEc) && !entryEc) {
            auto rel = fs::relative(it->path(), dir, entryEc);
            if (entryEc) {
                if (err)
                    *err = "cannot compute relative source directory path for " +
                           pathText(it->path()) + ": " + entryEc.message();
                return {};
            }
            for (const auto &ex : effectiveExcludes) {
                if (relativePathMatchesExclude(rel, ex)) {
                    it.disable_recursion_pending();
                    break;
                }
            }
            it.increment(ec);
            continue;
        }

        if (it->is_regular_file(entryEc) && !entryEc &&
            lowerAscii(pathText(it->path().extension())) == ext) {
            auto canonical = fs::canonical(it->path(), entryEc);
            if (entryEc) {
                if (err)
                    *err = "cannot resolve source file " + pathText(it->path()) + ": " +
                           entryEc.message();
                return {};
            }
            if (!zanna::pkg::isPathWithin(canonicalRoot, canonical)) {
                if (err)
                    *err = "source file escapes project root through a symlink: " +
                           pathText(it->path());
                return {};
            }
            result.push_back(pathText(canonical));
        }
        it.increment(ec);
    }
    if (ec) {
        if (err)
            *err = "cannot traverse source directory " + pathText(dir) + ": " + ec.message();
        return {};
    }

    std::sort(result.begin(), result.end());
    return result;
}

/// @brief Read a source file as text for convention-based lexical scans.
/// @details Convention discovery should not report I/O diagnostics for every
///          unreadable candidate; callers receive an empty optional and treat it
///          as "no convention signal". Files larger than the bounded scan limit
///          are skipped for the same reason. Manifest-directed paths still use
///          the stricter source loader later in the pipeline.
/// @param path Source file path to read.
/// @param skipReason Optional reason populated when the file is skipped.
/// @return File contents, or std::nullopt if the file could not be opened/read.
std::optional<std::string> readConventionScanText(const std::string &path,
                                                  std::string *skipReason = nullptr) {
    if (skipReason)
        skipReason->clear();
    std::ifstream in(zanna::filesystem::pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!in) {
        if (skipReason)
            *skipReason = "cannot open " + path;
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size < 0 || size > kMaxConventionScanBytes) {
        if (skipReason)
            *skipReason = "skipped " + path + " during convention scan (larger than 1 MiB)";
        return std::nullopt;
    }
    in.seekg(0, std::ios::beg);
    if (!in) {
        if (skipReason)
            *skipReason = "cannot seek " + path + " during convention scan";
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    if (!in.good() && !in.eof()) {
        if (skipReason)
            *skipReason = "cannot read " + path + " during convention scan";
        return std::nullopt;
    }
    return contents.str();
}

/// @brief Append convention-scan skip reasons to an entry-selection diagnostic.
/// @details Limits the appended list so a large project with many unreadable
///          files still gets a useful, bounded error message.
/// @param message Diagnostic message to extend in place.
/// @param skipped Reasons collected while scanning candidate source files.
void appendConventionSkipReasons(std::string &message, const std::vector<std::string> &skipped) {
    if (skipped.empty())
        return;
    message += "; skipped ";
    const std::size_t limit = std::min<std::size_t>(skipped.size(), 5);
    for (std::size_t i = 0; i < limit; ++i) {
        if (i != 0)
            message += "; ";
        message += skipped[i];
    }
    if (skipped.size() > limit)
        message += "; plus " + std::to_string(skipped.size() - limit) + " more";
}

/// @brief Check if a file contains a Zia entry point (func start() or func main()).
/// @details Uses the Zia lexer so comments and string literals cannot create
///          false entry-point candidates. The scan is intentionally shallow: it
///          looks for a function declaration token followed by an identifier
///          named @c start or @c main.
/// @param path Candidate Zia source path.
/// @param skipped Optional collector for bounded-scan I/O/size skip reasons.
/// @return true when a lexical `func start` or `func main` declaration is found.
bool hasZiaEntryPoint(const std::string &path, std::vector<std::string> *skipped = nullptr) {
    std::string skipReason;
    auto source = readConventionScanText(path, &skipReason);
    if (!source) {
        if (skipped && !skipReason.empty())
            skipped->push_back(std::move(skipReason));
        return false;
    }

    il::support::DiagnosticEngine diag;
    zia::Lexer lexer(std::move(*source), 1, diag);
    for (;;) {
        zia::Token tok = lexer.next();
        if (tok.kind == zia::TokenKind::Eof || tok.kind == zia::TokenKind::Error)
            return false;
        if (tok.kind != zia::TokenKind::KwFunc)
            continue;

        zia::Token name = lexer.next();
        if (name.kind == zia::TokenKind::Identifier &&
            (name.text == "start" || name.text == "main"))
            return true;
    }
}

/// @brief Signals discovered while token-scanning a BASIC source file.
/// @details The project loader uses these booleans to pick a root file by
///          convention without needing to fully parse or lower the program.
struct BasicConventionSignals {
    bool hasAddFile{false};      ///< A top-level ADDFILE statement was found.
    bool hasTopLevelCode{false}; ///< A top-level executable statement was found.
};

/// @brief Return true for procedure declaration modifiers that may precede SUB/FUNCTION.
/// @param kind BASIC token kind to classify.
/// @return True when @p kind is a declaration modifier, false otherwise.
bool isBasicProcedureModifier(basic::TokenKind kind) {
    switch (kind) {
        case basic::TokenKind::KeywordPublic:
        case basic::TokenKind::KeywordPrivate:
        case basic::TokenKind::KeywordExport:
        case basic::TokenKind::KeywordStatic:
        case basic::TokenKind::KeywordVirtual:
        case basic::TokenKind::KeywordOverride:
        case basic::TokenKind::KeywordAbstract:
        case basic::TokenKind::KeywordFinal:
            return true;
        default:
            return false;
    }
}

/// @brief Locate the first token that represents the statement leader on a BASIC line.
/// @details Skips traditional numeric line numbers and label prefixes of the
///          form @c Label: so top-level detection sees the executable statement
///          that follows.
/// @param line Tokens on a single BASIC source line.
/// @return Index of the statement leader, or line.size() when no statement remains.
std::size_t basicStatementLeaderIndex(const std::vector<basic::Token> &line) {
    std::size_t index = 0;
    if (index < line.size() && line[index].kind == basic::TokenKind::Number)
        ++index;
    if (index + 1 < line.size() && line[index].kind == basic::TokenKind::Identifier &&
        line[index + 1].kind == basic::TokenKind::Colon)
        index += 2;
    return index;
}

/// @brief Return the declaration token after any BASIC procedure modifiers.
/// @param line Tokens on a single BASIC source line.
/// @return Index of the first non-modifier token, or line.size().
std::size_t basicDeclarationLeaderIndex(const std::vector<basic::Token> &line) {
    std::size_t index = basicStatementLeaderIndex(line);
    while (index < line.size() && isBasicProcedureModifier(line[index].kind))
        ++index;
    return index;
}

/// @brief Test whether a BASIC line starts a procedure body.
/// @details DECLARE FUNCTION/SUB prototypes are excluded because they do not
///          introduce a body whose inner statements should be hidden from
///          top-level convention detection.
/// @param line Tokens on a single BASIC source line.
/// @return True when the line begins a SUB or FUNCTION body.
bool isBasicProcedureStartLine(const std::vector<basic::Token> &line) {
    if (line.empty())
        return false;
    const std::size_t leader = basicStatementLeaderIndex(line);
    if (leader < line.size() && line[leader].kind == basic::TokenKind::KeywordDeclare)
        return false;
    const std::size_t decl = basicDeclarationLeaderIndex(line);
    return decl < line.size() && (line[decl].kind == basic::TokenKind::KeywordSub ||
                                  line[decl].kind == basic::TokenKind::KeywordFunction);
}

/// @brief Test whether a BASIC line closes a procedure body.
/// @param line Tokens on a single BASIC source line.
/// @return True for @c END SUB or @c END FUNCTION.
bool isBasicProcedureEndLine(const std::vector<basic::Token> &line) {
    const std::size_t leader = basicStatementLeaderIndex(line);
    return leader + 1 < line.size() && line[leader].kind == basic::TokenKind::KeywordEnd &&
           (line[leader + 1].kind == basic::TokenKind::KeywordSub ||
            line[leader + 1].kind == basic::TokenKind::KeywordFunction);
}

/// @brief Test whether a BASIC token is an executable top-level statement leader.
/// @details Declaration-only keywords are deliberately omitted. @c CALL is kept
///          as an identifier check because the current lexer treats it as a soft
///          command name rather than a reserved keyword.
/// @param tok Statement leader token.
/// @return True when @p tok denotes executable code.
bool isBasicExecutableStatementLeader(const basic::Token &tok) {
    switch (tok.kind) {
        case basic::TokenKind::KeywordPrint:
        case basic::TokenKind::KeywordWrite:
        case basic::TokenKind::KeywordInput:
        case basic::TokenKind::KeywordIf:
        case basic::TokenKind::KeywordFor:
        case basic::TokenKind::KeywordWhile:
        case basic::TokenKind::KeywordDo:
        case basic::TokenKind::KeywordLet:
        case basic::TokenKind::KeywordDim:
        case basic::TokenKind::KeywordGoto:
        case basic::TokenKind::KeywordGosub:
        case basic::TokenKind::KeywordReturn:
        case basic::TokenKind::KeywordAddfile:
        case basic::TokenKind::KeywordOpen:
        case basic::TokenKind::KeywordClose:
        case basic::TokenKind::KeywordSleep:
        case basic::TokenKind::KeywordRandomize:
        case basic::TokenKind::KeywordRedim:
            return true;
        case basic::TokenKind::Identifier:
            return lowerAscii(tok.lexeme) == "call";
        default:
            return false;
    }
}

/// @brief Token-scan a BASIC file for convention-based root-file signals.
/// @details Uses the BASIC lexer so comments, strings, and mixed-case REM lines
///          cannot trigger false positives. Procedure nesting is tracked so
///          executable statements inside SUB/FUNCTION bodies do not make a
///          library module look like a top-level program.
/// @param path BASIC source file to inspect.
/// @param skipped Optional collector for bounded-scan I/O/size skip reasons.
/// @return Convention signals found in @p path.
BasicConventionSignals scanBasicConventionSignals(const std::string &path,
                                                  std::vector<std::string> *skipped = nullptr) {
    BasicConventionSignals signals;
    std::string skipReason;
    auto source = readConventionScanText(path, &skipReason);
    if (!source) {
        if (skipped && !skipReason.empty())
            skipped->push_back(std::move(skipReason));
        return signals;
    }

    basic::Lexer lexer(std::string_view(*source), 1);
    std::vector<basic::Token> line;
    int procedureDepth = 0;

    /// @brief Classify the accumulated BASIC line and reset its token buffer.
    /// @details Updates procedure nesting and top-level AddFile/executable signals.
    auto flushLine = [&]() {
        if (line.empty())
            return;
        if (isBasicProcedureEndLine(line)) {
            if (procedureDepth > 0)
                --procedureDepth;
            line.clear();
            return;
        }
        if (procedureDepth == 0) {
            const std::size_t leader = basicStatementLeaderIndex(line);
            if (leader < line.size()) {
                if (line[leader].kind == basic::TokenKind::KeywordAddfile)
                    signals.hasAddFile = true;
                if (isBasicExecutableStatementLeader(line[leader]))
                    signals.hasTopLevelCode = true;
            }
        }
        if (isBasicProcedureStartLine(line))
            ++procedureDepth;
        line.clear();
    };

    for (;;) {
        basic::Token tok = lexer.next();
        if (tok.kind == basic::TokenKind::EndOfLine) {
            flushLine();
            continue;
        }
        if (tok.kind == basic::TokenKind::EndOfFile) {
            flushLine();
            return signals;
        }
        if (tok.kind == basic::TokenKind::Unknown) {
            line.clear();
            continue;
        }
        line.push_back(std::move(tok));
    }
}

/// @brief Find the Zia entry file from a list of source files.
/// @param files Absolute Zia source paths in deterministic order.
/// @return `main.zia`, a unique lexical entry declaration, or a diagnostic.
il::support::Expected<std::string> findZiaEntry(const std::vector<std::string> &files) {
    // Priority 1: file named main.zia
    for (const auto &f : files) {
        if (lowerAscii(pathText(zanna::filesystem::pathFromUtf8(f).filename())) == "main.zia")
            return f;
    }

    // Priority 2: scan for func start() or func main()
    std::vector<std::string> candidates;
    std::vector<std::string> skipped;
    for (const auto &f : files) {
        if (hasZiaEntryPoint(f, &skipped))
            candidates.push_back(f);
    }

    if (candidates.size() == 1)
        return candidates[0];
    if (candidates.size() > 1) {
        std::string msg = "multiple entry points found:";
        for (const auto &c : candidates)
            msg += " " + pathText(zanna::filesystem::pathFromUtf8(c).filename());
        msg += "; specify entry in zanna.project";
        return makeErr(msg);
    }

    std::string msg = "no entry point found; expected func start() or func main() in a .zia file, "
                      "or a file named main.zia";
    appendConventionSkipReasons(msg, skipped);
    return makeErr(msg);
}

/// @brief Find the BASIC entry file from a list of source files.
/// @param files Absolute BASIC source paths in deterministic order.
/// @return Convention-selected root file, or a diagnostic when ambiguous/absent.
il::support::Expected<std::string> findBasicEntry(const std::vector<std::string> &files) {
    // Priority 1: file named main.bas
    for (const auto &f : files) {
        if (lowerAscii(pathText(zanna::filesystem::pathFromUtf8(f).filename())) == "main.bas")
            return f;
    }

    std::vector<std::string> skipped;
    std::vector<BasicConventionSignals> signals;
    signals.reserve(files.size());
    for (const auto &f : files)
        signals.push_back(scanBasicConventionSignals(f, &skipped));

    // Priority 2: look for files with AddFile directives (root files)
    std::vector<std::string> roots;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (signals[i].hasAddFile)
            roots.push_back(files[i]);
    }

    if (roots.size() == 1)
        return roots[0];
    if (roots.size() > 1) {
        std::string msg = "multiple root files found:";
        for (const auto &r : roots)
            msg += " " + pathText(zanna::filesystem::pathFromUtf8(r).filename());
        msg += "; specify entry in zanna.project";
        return makeErr(msg);
    }

    // Priority 3: look for files with top-level executable statements
    std::vector<std::string> execFiles;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (signals[i].hasTopLevelCode)
            execFiles.push_back(files[i]);
    }

    if (execFiles.size() == 1)
        return execFiles[0];
    if (execFiles.size() > 1) {
        std::string msg = "multiple BASIC files contain top-level statements:";
        for (const auto &f : execFiles)
            msg += " " + pathText(zanna::filesystem::pathFromUtf8(f).filename());
        msg += "; specify entry in zanna.project";
        return makeErr(msg);
    }

    if (files.size() == 1)
        return files[0];

    std::string msg = "no entry point found; expected a .bas file with top-level statements, "
                      "or a file named main.bas";
    appendConventionSkipReasons(msg, skipped);
    return makeErr(msg);
}

/// @brief Discover project configuration by convention (no manifest).
/// @param dir Existing project directory.
/// @param excludes Additional normalized discovery exclusions.
/// @return Resolved single-language configuration, or a discovery diagnostic.
il::support::Expected<ProjectConfig> discoverConvention(const fs::path &dir,
                                                        const std::vector<std::string> &excludes) {
    std::string collectErr;
    auto ziaFiles = collectFiles(dir, ".zia", excludes, &collectErr);
    if (!collectErr.empty())
        return makeErr(collectErr);
    auto basFiles = collectFiles(dir, ".bas", excludes, &collectErr);
    if (!collectErr.empty())
        return makeErr(collectErr);

    // Language detection
    if (ziaFiles.empty() && basFiles.empty())
        return makeErr("no source files found in " + pathText(dir));

    if (!ziaFiles.empty() && !basFiles.empty()) {
        // Mixed project detected — require a zanna.project manifest to
        // specify the entry point and 'lang mixed'.
        return makeErr("mixed .zia and .bas files in " + pathText(dir) +
                       "; create zanna.project with 'lang mixed' and 'entry <file>'");
    }

    ProjectConfig config;
    std::error_code ec;
    auto canonicalDir = fs::canonical(dir, ec);
    if (ec)
        return makeErr("cannot resolve project directory: " + pathText(dir));
    config.rootDir = pathText(canonicalDir);
    config.name = pathText(dir.filename());

    if (!ziaFiles.empty()) {
        config.lang = ProjectLang::Zia;
        config.sourceFiles = std::move(ziaFiles);

        auto entry = findZiaEntry(config.sourceFiles);
        if (!entry)
            return il::support::Expected<ProjectConfig>(entry.error());
        config.entryFile = std::move(entry.value());
    } else {
        config.lang = ProjectLang::Basic;
        config.sourceFiles = std::move(basFiles);

        auto entry = findBasicEntry(config.sourceFiles);
        if (!entry)
            return il::support::Expected<ProjectConfig>(entry.error());
        config.entryFile = std::move(entry.value());
    }

    return config;
}

/// @brief Strip an inline `#` manifest comment while respecting quoted strings.
/// @details The manifest grammar treats `#` as a comment only outside double
///          quotes. Backslash escapes inside quotes are skipped so an escaped quote
///          does not accidentally terminate the string while looking for comments.
/// @param line One already-trimmed manifest line.
/// @return The line prefix before the comment marker, with trailing whitespace removed.
std::string stripInlineManifestComment(std::string line) {
    bool inQuote = false;
    bool escaping = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escaping) {
            escaping = false;
            continue;
        }
        if (inQuote && c == '\\') {
            escaping = true;
            continue;
        }
        if (c == '"') {
            inQuote = !inQuote;
            continue;
        }
        if (!inQuote && c == '#') {
            line.resize(i);
            break;
        }
    }
    const auto end = line.find_last_not_of(" \t\r\n");
    if (end == std::string::npos)
        return {};
    line.resize(end + 1);
    return line;
}

/// @brief Parse an on/off boolean value.
/// @param val Candidate Boolean spelling.
/// @param manifestPath Manifest path used in diagnostics.
/// @param line One-based line number.
/// @param directive Directive name used in diagnostics.
/// @return Parsed Boolean or a contextual diagnostic.
il::support::Expected<bool> parseBool(const std::string &val,
                                      const std::string &manifestPath,
                                      int line,
                                      const std::string &directive) {
    if (val == "on" || val == "true" || val == "yes" || val == "1")
        return true;
    if (val == "off" || val == "false" || val == "no" || val == "0")
        return false;
    return makeManifestErr(manifestPath,
                           line,
                           "invalid value '" + val + "' for " + directive +
                               "; expected on/off, true/false, yes/no, or 1/0");
}

/// @brief Map a build profile name to its default optimization level.
/// @details Translates the manifest `profile` directive into an `O0`/`O1`/`O2`
///          string: debug→O0, balanced→O1, release→O2. The optimization level
///          is only applied when the manifest does not also set `optimize`
///          explicitly.
/// @param profile Profile name from the manifest ("debug", "balanced", "release").
/// @param manifestPath Manifest path, used for error context.
/// @param line 1-based line number, used for error context.
/// @return The mapped optimization level string, or a diagnostic for an
///         unrecognised profile.
il::support::Expected<std::string> optimizeForBuildProfile(const std::string &profile,
                                                           const std::string &manifestPath,
                                                           int line) {
    if (profile == "debug")
        return std::string("O0");
    if (profile == "balanced")
        return std::string("O1");
    if (profile == "release")
        return std::string("O2");
    return makeManifestErr(manifestPath,
                           line,
                           "invalid build profile '" + profile +
                               "'; expected debug, balanced, or release");
}

/// @brief Split a manifest directive value into whitespace-separated tokens.
/// @details Implements a small lexer with double-quote grouping and backslash
///          escapes inside quotes (`\"`, `\\`, `\n`, `\t`; any other escaped
///          character is taken literally). Unquoted runs of spaces or tabs
///          separate tokens. An unterminated quote is reported as an error.
/// @param value Raw value text following the directive name.
/// @param manifestPath Manifest path, used for error context.
/// @param line 1-based line number, used for error context.
/// @param directive Directive name, used for error context.
/// @return The token list, or a diagnostic when a quote is left open.
il::support::Expected<std::vector<std::string>> tokenizeManifestValue(
    const std::string &value,
    const std::string &manifestPath,
    int line,
    const std::string &directive) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuote = false;
    bool escaping = false;

    for (char c : value) {
        if (escaping) {
            switch (c) {
                case '"':
                case '\\':
                    cur.push_back(c);
                    break;
                case 'n':
                    cur.push_back('\n');
                    break;
                case 't':
                    cur.push_back('\t');
                    break;
                default:
                    cur.push_back(c);
                    break;
            }
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
        if (!inQuote && (c == ' ' || c == '\t')) {
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
        return makeManifestErr(manifestPath, line, "unterminated quote in " + directive);
    if (!cur.empty())
        tokens.push_back(std::move(cur));
    return tokens;
}

/// @brief Tokenize a directive value and require exactly @p count tokens.
/// @param value Raw directive value.
/// @param manifestPath Manifest path used in diagnostics.
/// @param line One-based line number.
/// @param directive Directive name used in diagnostics.
/// @param count Exact required token count.
/// @return The tokens on success, or a manifest diagnostic on a tokenize failure
///         or arity mismatch.
il::support::Expected<std::vector<std::string>> requireManifestTokenCount(
    const std::string &value,
    const std::string &manifestPath,
    int line,
    const std::string &directive,
    std::size_t count) {
    auto tokens = tokenizeManifestValue(value, manifestPath, line, directive);
    if (!tokens)
        return il::support::Expected<std::vector<std::string>>(tokens.error());
    if (tokens.value().size() != count) {
        return makeManifestErr(manifestPath,
                               line,
                               directive + " requires exactly " + std::to_string(count) + " value" +
                                   (count == 1 ? "" : "s"));
    }
    return tokens;
}

/// @brief Parse a core manifest directive as exactly one scalar token.
/// @details Core directives use the same tokenizer as package directives so
///          values containing whitespace must be quoted consistently.
/// @param value Raw directive value after inline comment stripping.
/// @param manifestPath Manifest path, used for error context.
/// @param line 1-based line number, used for error context.
/// @param directive Directive name, used for error context.
/// @return The single scalar token, or a manifest diagnostic.
il::support::Expected<std::string> parseCoreScalar(const std::string &value,
                                                   const std::string &manifestPath,
                                                   int line,
                                                   const std::string &directive) {
    auto tokens = requireManifestTokenCount(value, manifestPath, line, directive, 1);
    if (!tokens)
        return il::support::Expected<std::string>(tokens.error());
    return tokens.value()[0];
}

/// @brief Resolve a project-relative manifest path to an absolute, in-root path.
/// @details Sanitizes @p raw, resolves it against @p manifestDir, and rejects any
///          result that escapes the project root. An empty path maps to the
///          project root when @p allowProjectRoot, otherwise is an error.
/// @param manifestDir Canonical project root and resolution base.
/// @param raw Caller-supplied project-relative path.
/// @param manifestPath Manifest path used in diagnostics.
/// @param line One-based line number.
/// @param directive Directive name used in diagnostics.
/// @param allowProjectRoot Whether an empty/`.` path may resolve to the root.
/// @return The resolved path, or a manifest diagnostic on rejection.
il::support::Expected<fs::path> resolveManifestRelativePath(const fs::path &manifestDir,
                                                            const std::string &raw,
                                                            const std::string &manifestPath,
                                                            int line,
                                                            const std::string &directive,
                                                            bool allowProjectRoot) {
    try {
        std::string clean = zanna::pkg::sanitizePackageRelativePath(raw, directive.c_str());
        if (clean.empty()) {
            if (allowProjectRoot)
                return manifestDir;
            return makeManifestErr(manifestPath, line, directive + " path must not be empty");
        }

        std::error_code ec;
        fs::path candidate =
            (manifestDir / zanna::filesystem::pathFromUtf8(clean)).lexically_normal();
        fs::path resolved = fs::weakly_canonical(candidate, ec);
        if (ec)
            return makeManifestErr(
                manifestPath, line, "cannot resolve " + directive + " path: '" + raw + "'");
        if (!zanna::pkg::isPathWithin(manifestDir, resolved)) {
            return makeManifestErr(
                manifestPath, line, directive + " path escapes the project root: '" + raw + "'");
        }
        return resolved;
    } catch (const std::exception &ex) {
        return makeManifestErr(manifestPath, line, ex.what());
    }
}

/// @brief Parse a directive value as a single project-relative path string.
/// @details Requires exactly one token (requireManifestTokenCount) and resolves it
///          via resolveManifestRelativePath; returns the resolved path as a string.
/// @param manifestDir Canonical project root and resolution base.
/// @param value Raw directive value.
/// @param manifestPath Manifest path used in diagnostics.
/// @param line One-based line number.
/// @param directive Directive name used in diagnostics.
/// @param allowProjectRoot Whether the project root is an acceptable value.
/// @return The resolved path string, or a manifest diagnostic on failure.
il::support::Expected<std::string> parseManifestRelativeToken(const fs::path &manifestDir,
                                                              const std::string &value,
                                                              const std::string &manifestPath,
                                                              int line,
                                                              const std::string &directive,
                                                              bool allowProjectRoot) {
    auto tokens = requireManifestTokenCount(value, manifestPath, line, directive, 1);
    if (!tokens)
        return il::support::Expected<std::string>(tokens.error());
    auto path = resolveManifestRelativePath(
        manifestDir, tokens.value()[0], manifestPath, line, directive, allowProjectRoot);
    if (!path)
        return il::support::Expected<std::string>(path.error());
    return pathText(path.value());
}

/// @brief Read the contents of a project-relative script hook file.
/// @details Used by the post-install / pre-uninstall directives. The value must
///          tokenize to exactly one project-relative path, which is resolved
///          against @p manifestDir (rejecting path escapes via
///          zanna::pkg::resolvePackageSourcePath), required to be a regular
///          file, and slurped into a string. Any failure produces a manifest
///          diagnostic with file:line context.
/// @param manifestDir Directory the manifest lives in; the resolution root.
/// @param value Raw directive value naming the script path.
/// @param manifestPath Manifest path, used for error context.
/// @param line 1-based line number, used for error context.
/// @param directive Directive name, used for error context.
/// @return The script file contents, or a diagnostic on any failure.
il::support::Expected<std::string> readManifestScriptHook(const fs::path &manifestDir,
                                                          const std::string &value,
                                                          const std::string &manifestPath,
                                                          int line,
                                                          const std::string &directive) {
    auto tokens = tokenizeManifestValue(value, manifestPath, line, directive);
    if (!tokens)
        return il::support::Expected<std::string>(tokens.error());
    if (tokens.value().size() != 1)
        return makeManifestErr(
            manifestPath, line, directive + " requires exactly one project-relative script path");

    fs::path scriptPath;
    try {
        scriptPath =
            zanna::pkg::resolvePackageSourcePath(manifestDir, tokens.value()[0], directive.c_str());
    } catch (const std::exception &ex) {
        return makeManifestErr(manifestPath, line, ex.what());
    }
    if (!fs::is_regular_file(scriptPath))
        return makeManifestErr(
            manifestPath, line, directive + " script is not a regular file: " + tokens.value()[0]);

    std::ifstream in(scriptPath, std::ios::binary | std::ios::ate);
    if (!in)
        return makeManifestErr(
            manifestPath, line, "cannot read " + directive + " script: " + pathText(scriptPath));
    const std::streamoff scriptSize = in.tellg();
    if (scriptSize < 0)
        return makeManifestErr(manifestPath,
                               line,
                               "cannot determine size of " + directive +
                                   " script: " + pathText(scriptPath));
    if (static_cast<std::uint64_t>(scriptSize) > kMaxManifestHookScriptBytes)
        return makeManifestErr(
            manifestPath, line, directive + " script exceeds 1 MiB limit: " + pathText(scriptPath));
    in.seekg(0);
    if (!in)
        return makeManifestErr(
            manifestPath, line, "cannot seek " + directive + " script: " + pathText(scriptPath));
    std::ostringstream contents;
    contents << in.rdbuf();
    if (!in.good() && !in.eof())
        return makeManifestErr(manifestPath,
                               line,
                               "failed while reading " + directive +
                                   " script: " + pathText(scriptPath));
    return contents.str();
}

/// @brief Record a scalar package directive and reject duplicates.
/// @details Inserts @p name into @p seen; if it was already present the
///          directive appeared more than once, which is an error. Shared by
///          parsePackageDirective() so every "package-*" scalar enforces
///          at-most-once semantics uniformly.
/// @param seen Set of scalar directive names already parsed this manifest.
/// @param name Directive name being recorded.
/// @param manifestPath Manifest path, used for error context.
/// @param line 1-based line number, used for error context.
/// @return True on first occurrence; a diagnostic on a duplicate.
il::support::Expected<bool> markPackageScalar(std::set<std::string> &seen,
                                              const std::string &name,
                                              const std::string &manifestPath,
                                              int line) {
    if (!seen.insert(name).second)
        return makeManifestErr(manifestPath, line, "duplicate directive '" + name + "'");
    return true;
}

/// @brief Parse a scalar package directive value, enforcing single occurrence.
/// @details Combines markPackageScalar() (duplicate rejection) with
///          tokenizeManifestValue() and then requires exactly one resulting
///          token, so values containing spaces must be quoted. Shared by the
///          many string-valued "package-*"/"macos-*"/"windows-*" directives.
/// @param seen Set of scalar directive names already parsed this manifest.
/// @param name Directive name being parsed.
/// @param value Raw value text following the directive name.
/// @param manifestPath Manifest path, used for error context.
/// @param line 1-based line number, used for error context.
/// @return The single scalar value, or a diagnostic on duplicate/arity errors.
il::support::Expected<std::string> parsePackageScalar(std::set<std::string> &seen,
                                                      const std::string &name,
                                                      const std::string &value,
                                                      const std::string &manifestPath,
                                                      int line) {
    auto ok = markPackageScalar(seen, name, manifestPath, line);
    if (!ok)
        return il::support::Expected<std::string>(ok.error());
    auto tokens = tokenizeManifestValue(value, manifestPath, line, name);
    if (!tokens)
        return il::support::Expected<std::string>(tokens.error());
    if (tokens.value().size() != 1)
        return makeManifestErr(manifestPath,
                               line,
                               name + " requires exactly one scalar value; quote values that "
                                      "contain spaces");
    return tokens.value()[0];
}

/// @brief Parse a scalar package directive as a safe project-relative path.
/// @details This wraps parsePackageScalar() so duplicate and quoting rules stay
///          identical to other scalar package fields, then normalizes and
///          validates the returned value with sanitizePackageRelativePath().
///          Existence is not checked here because packaging builders validate
///          optional metadata paths when the target format actually consumes them.
/// @param seen Set of already-seen scalar package directives.
/// @param name Directive name to parse.
/// @param value Raw directive value.
/// @param manifestPath Manifest path, used for diagnostics.
/// @param line 1-based line number, used for diagnostics.
/// @return A normalized relative path, or a diagnostic on duplicate, arity, or
///         path-safety failure.
il::support::Expected<std::string> parsePackageRelativePathScalar(std::set<std::string> &seen,
                                                                  const std::string &name,
                                                                  const std::string &value,
                                                                  const std::string &manifestPath,
                                                                  int line) {
    auto scalar = parsePackageScalar(seen, name, value, manifestPath, line);
    if (!scalar)
        return scalar;
    try {
        return zanna::pkg::sanitizePackageRelativePath(scalar.value(), name.c_str());
    } catch (const std::exception &ex) {
        return makeManifestErr(manifestPath, line, ex.what());
    }
}

/// @brief Parse and append comma-separated package dependency terms.
/// @details Manifest dependency directives accept one raw line rather than
///          tokenizing on spaces because Debian and RPM dependency expressions
///          commonly contain spaces around version operators and alternatives.
///          This helper trims each comma-delimited term, runs the caller-provided
///          validator, rejects duplicates across repeated directives, and appends
///          valid terms to @p out in declaration order.
/// @param out Dependency list to extend.
/// @param value Raw directive value after the directive name.
/// @param manifestPath Manifest path, used for error context.
/// @param lineNum 1-based manifest line number.
/// @param directive Directive name, used in diagnostics.
/// @param validator Function that validates one trimmed dependency string.
/// @return True on success; a manifest diagnostic on an empty, duplicate, or
///         validator-rejected dependency.
il::support::Expected<bool> appendCommaSeparatedPackageDependencies(
    std::vector<std::string> &out,
    const std::string &value,
    const std::string &manifestPath,
    int lineNum,
    const std::string &directive,
    const std::function<void(const std::string &)> &validator) {
    std::string depToken;
    std::set<std::string> seenDeps(out.begin(), out.end());
    /// @brief Validate and append one trimmed dependency token.
    /// @param raw Raw comma-delimited token.
    /// @return Success with `true`, or a manifest diagnostic on invalid or duplicate input.
    auto appendDependency = [&](const std::string &raw) -> il::support::Expected<bool> {
        size_t ds = raw.find_first_not_of(" \t");
        size_t de = raw.find_last_not_of(" \t");
        if (ds == std::string::npos)
            return makeManifestErr(
                manifestPath, lineNum, directive + " contains an empty dependency");
        std::string dep = raw.substr(ds, de - ds + 1);
        try {
            validator(dep);
        } catch (const std::exception &ex) {
            return makeManifestErr(manifestPath, lineNum, ex.what());
        }
        if (!seenDeps.insert(dep).second)
            return makeManifestErr(
                manifestPath, lineNum, "duplicate package dependency '" + dep + "'");
        out.push_back(std::move(dep));
        return true;
    };
    for (char c : value) {
        if (c == ',') {
            auto added = appendDependency(depToken);
            if (!added)
                return il::support::Expected<bool>(added.error());
            depToken.clear();
        } else {
            depToken.push_back(c);
        }
    }
    return appendDependency(depToken);
}

/// @brief Apply one packaging-related manifest directive to @p config.
/// @details Dispatches the "package-*", "macos-*", "windows-*", asset embedding
///          (asset/embed/pack/pack-compressed), file association, shortcut,
///          dependency, and install-hook directives. Scalar directives are
///          routed through parsePackageScalar() for duplicate/arity checking;
///          boolean directives through parseBool(); multi-token directives
///          through tokenizeManifestValue(). Unrecognised directives are not an
///          error here — they return false so the caller can decide whether the
///          directive is unknown.
/// @param config Project configuration mutated in place with parsed values.
/// @param packageScalarDirectives Set tracking already-seen scalar directives.
/// @param manifestDir Directory containing the manifest (for path resolution).
/// @param manifestPath Manifest path, used for error context.
/// @param directive Directive name to handle.
/// @param value Raw value text following the directive name.
/// @param lineNum 1-based line number, used for error context.
/// @return True when @p directive was a recognised package directive and applied;
///         false when it is not a package directive; a diagnostic on parse error.
il::support::Expected<bool> parsePackageDirective(ProjectConfig &config,
                                                  std::set<std::string> &packageScalarDirectives,
                                                  const fs::path &manifestDir,
                                                  const std::string &manifestPath,
                                                  const std::string &directive,
                                                  const std::string &value,
                                                  int lineNum) {
    if (directive == "package-name") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.displayName = scalar.value();
    } else if (directive == "package-author") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.author = scalar.value();
    } else if (directive == "package-description") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.description = scalar.value();
    } else if (directive == "package-homepage") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.homepage = scalar.value();
    } else if (directive == "package-license") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.license = scalar.value();
    } else if (directive == "package-license-file") {
        auto scalar = parsePackageRelativePathScalar(
            packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.licenseFilePath = scalar.value();
    } else if (directive == "package-readme") {
        auto scalar = parsePackageRelativePathScalar(
            packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.readmeFilePath = scalar.value();
    } else if (directive == "package-welcome") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.welcomeText = scalar.value();
    } else if (directive == "package-identifier") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.identifier = scalar.value();
    } else if (directive == "package-icon") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.iconPath = scalar.value();
    } else if (directive == "macos-sign-mode") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.macosSignMode = scalar.value();
    } else if (directive == "macos-sign-identity") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.macosSignIdentity = scalar.value();
    } else if (directive == "macos-entitlements") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.macosEntitlements = scalar.value();
    } else if (directive == "macos-notary-profile") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.macosNotaryProfile = scalar.value();
    } else if (directive == "macos-hardened-runtime") {
        auto ok = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!ok)
            return il::support::Expected<bool>(ok.error());
        auto b = parseBool(value, manifestPath, lineNum, directive);
        if (!b)
            return il::support::Expected<bool>(b.error());
        config.packageConfig.macosHardenedRuntime = b.value();
    } else if (directive == "macos-staple") {
        auto ok = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!ok)
            return il::support::Expected<bool>(ok.error());
        auto b = parseBool(value, manifestPath, lineNum, directive);
        if (!b)
            return il::support::Expected<bool>(b.error());
        config.packageConfig.macosStaple = b.value();
    } else if (directive == "macos-dmg-background") {
        auto scalar = parsePackageRelativePathScalar(
            packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.macosDmgBackground = scalar.value();
    } else if (directive == "macos-dmg-icon") {
        auto scalar = parsePackageRelativePathScalar(
            packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.macosDmgIcon = scalar.value();
    } else if (directive == "windows-install-scope") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        if (scalar.value() != "machine" && scalar.value() != "user") {
            return makeManifestErr(manifestPath,
                                   lineNum,
                                   "invalid windows-install-scope '" + scalar.value() +
                                       "'; expected 'machine' or 'user'");
        }
        config.packageConfig.windowsInstallScope = scalar.value();
    } else if (directive == "windows-install-dir") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.windowsInstallDir = scalar.value();
    } else if (directive == "windows-publisher") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.windowsPublisher = scalar.value();
    } else if (directive == "windows-wizard-summary") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.windowsWizardSummary = scalar.value();
    } else if (directive == "windows-dll") {
        auto tokens = requireManifestTokenCount(value, manifestPath, lineNum, directive, 1);
        if (!tokens)
            return il::support::Expected<bool>(tokens.error());
        try {
            std::string dllPath =
                zanna::pkg::sanitizePackageRelativePath(tokens.value()[0], "windows-dll path");
            if (dllPath.empty())
                return makeManifestErr(manifestPath, lineNum, "windows-dll path must not be empty");
            if (std::find(config.packageConfig.windowsDlls.begin(),
                          config.packageConfig.windowsDlls.end(),
                          dllPath) != config.packageConfig.windowsDlls.end()) {
                return makeManifestErr(
                    manifestPath, lineNum, "duplicate windows-dll path '" + dllPath + "'");
            }
            config.packageConfig.windowsDlls.push_back(std::move(dllPath));
        } catch (const std::exception &ex) {
            return makeManifestErr(manifestPath, lineNum, ex.what());
        }
    } else if (directive == "windows-sign") {
        auto ok = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!ok)
            return il::support::Expected<bool>(ok.error());
        auto b = parseBool(value, manifestPath, lineNum, directive);
        if (!b)
            return il::support::Expected<bool>(b.error());
        config.packageConfig.windowsSign = b.value();
        config.packageConfig.windowsSignSet = true;
    } else if (directive == "windows-sign-pfx") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.windowsSignPfx = scalar.value();
    } else if (directive == "windows-sign-thumbprint") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.windowsSignThumbprint = scalar.value();
    } else if (directive == "windows-timestamp-url") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.windowsTimestampUrl = scalar.value();
    } else if (directive == "windows-signtool") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.windowsSigntoolPath = scalar.value();
    } else if (directive == "windows-sign-no-verify") {
        auto ok = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!ok)
            return il::support::Expected<bool>(ok.error());
        auto b = parseBool(value, manifestPath, lineNum, directive);
        if (!b)
            return il::support::Expected<bool>(b.error());
        config.packageConfig.windowsSignNoVerify = b.value();
    } else if (directive == "asset") {
        // Format: asset <source-path> <target-relative-dir>
        auto tokens = tokenizeManifestValue(value, manifestPath, lineNum, directive);
        if (!tokens)
            return il::support::Expected<bool>(tokens.error());
        if (tokens.value().size() != 2)
            return makeManifestErr(
                manifestPath, lineNum, "asset requires <source> <target>; got '" + value + "'");
        try {
            std::string source =
                zanna::pkg::sanitizePackageRelativePath(tokens.value()[0], "asset source path");
            if (source.empty())
                return makeManifestErr(
                    manifestPath, lineNum, "asset source path must not be empty");
            std::string target =
                zanna::pkg::sanitizePackageRelativePath(tokens.value()[1], "asset target path");
            config.packageConfig.assets.push_back({std::move(source), std::move(target)});
        } catch (const std::exception &ex) {
            return makeManifestErr(manifestPath, lineNum, ex.what());
        }
    } else if (directive == "embed") {
        // Format: embed <source-path>
        auto tokens = requireManifestTokenCount(value, manifestPath, lineNum, directive, 1);
        if (!tokens)
            return il::support::Expected<bool>(tokens.error());
        try {
            std::string embedSrc =
                zanna::pkg::sanitizePackageRelativePath(tokens.value()[0], "embed");
            if (embedSrc.empty())
                return makeManifestErr(manifestPath, lineNum, "embed path must not be empty");
            config.embedAssets.push_back({std::move(embedSrc)});
        } catch (const std::exception &ex) {
            return makeManifestErr(manifestPath, lineNum, ex.what());
        }

    } else if (directive == "pack" || directive == "pack-compressed") {
        // Format: pack <name> <source-path>
        auto tokens = requireManifestTokenCount(value, manifestPath, lineNum, directive, 2);
        if (!tokens)
            return il::support::Expected<bool>(tokens.error());
        std::string packName = tokens.value()[0];
        std::string packSrc;
        try {
            packSrc = zanna::pkg::sanitizePackageRelativePath(tokens.value()[1], directive.c_str());
            if (packSrc.empty())
                return makeManifestErr(
                    manifestPath, lineNum, std::string(directive) + " path must not be empty");
        } catch (const std::exception &ex) {
            return makeManifestErr(manifestPath, lineNum, ex.what());
        }
        bool compressed = (directive == "pack-compressed");

        // Find existing group with same name, or create new one.
        auto it =
            std::find_if(config.packGroups.begin(),
                         config.packGroups.end(),
                         /// @brief Match an existing pack group by manifest name.
                         /// @param g Candidate pack group.
                         /// @return `true` when @p g has @c packName.
                         [&](const ProjectConfig::PackGroup &g) { return g.name == packName; });
        if (it == config.packGroups.end()) {
            config.packGroups.push_back({packName, {packSrc}, compressed});
        } else {
            it->sources.push_back(packSrc);
            if (compressed)
                it->compressed = true;
        }

    } else if (directive == "file-assoc") {
        // Format: file-assoc <extension> <description> <mime-type> [windows-open-args]
        auto tokens = tokenizeManifestValue(value, manifestPath, lineNum, directive);
        if (!tokens)
            return il::support::Expected<bool>(tokens.error());
        if (tokens.value().size() != 3 && tokens.value().size() != 4)
            return makeManifestErr(manifestPath,
                                   lineNum,
                                   "file-assoc requires <ext> <description> <mime> "
                                   "[windows-open-args]; got '" +
                                       value + "'");
        config.packageConfig.fileAssociations.push_back(
            {tokens.value()[0],
             tokens.value()[1],
             tokens.value()[2],
             tokens.value().size() == 4 ? tokens.value()[3] : std::string{}});
    } else if (directive == "shortcut-desktop") {
        auto seen = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!seen)
            return il::support::Expected<bool>(seen.error());
        auto b = parseBool(value, manifestPath, lineNum, "shortcut-desktop");
        if (!b)
            return il::support::Expected<bool>(b.error());
        config.packageConfig.shortcutDesktop = b.value();
    } else if (directive == "shortcut-menu") {
        auto seen = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!seen)
            return il::support::Expected<bool>(seen.error());
        auto b = parseBool(value, manifestPath, lineNum, "shortcut-menu");
        if (!b)
            return il::support::Expected<bool>(b.error());
        config.packageConfig.shortcutMenu = b.value();
    } else if (directive == "allow-home-desktop-shortcuts") {
        auto seen = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!seen)
            return il::support::Expected<bool>(seen.error());
        auto b = parseBool(value, manifestPath, lineNum, "allow-home-desktop-shortcuts");
        if (!b)
            return il::support::Expected<bool>(b.error());
        config.packageConfig.allowHomeDesktopShortcuts = b.value();
    } else if (directive == "min-os-windows") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.minOsWindows = scalar.value();
    } else if (directive == "min-os-macos") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.minOsMacos = scalar.value();
    } else if (directive == "target-arch") {
        auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        const std::string arch = scalar.value();
        if (arch != "x64" && arch != "arm64")
            return makeManifestErr(manifestPath,
                                   lineNum,
                                   "invalid target-arch '" + arch + "'; expected 'x64' or 'arm64'");
        if (std::find(config.packageConfig.targetArchitectures.begin(),
                      config.packageConfig.targetArchitectures.end(),
                      arch) != config.packageConfig.targetArchitectures.end()) {
            return makeManifestErr(manifestPath, lineNum, "duplicate target-arch '" + arch + "'");
        }
        config.packageConfig.targetArchitectures.push_back(arch);
    } else if (directive == "package-category") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.category = scalar.value();
    } else if (directive == "package-depends") {
        auto added = appendCommaSeparatedPackageDependencies(config.packageConfig.depends,
                                                             value,
                                                             manifestPath,
                                                             lineNum,
                                                             directive,
                                                             zanna::pkg::validateDebDependency);
        if (!added)
            return il::support::Expected<bool>(added.error());
    } else if (directive == "package-rpm-depends") {
        auto added = appendCommaSeparatedPackageDependencies(
            config.packageConfig.rpmDepends,
            value,
            manifestPath,
            lineNum,
            directive,
            /// @brief Validate one RPM dependency expression.
            /// @param dep Trimmed dependency expression.
            [](const std::string &dep) { zanna::pkg::validateRpmDependency(dep); });
        if (!added)
            return il::support::Expected<bool>(added.error());
    } else if (directive == "linux-startup-wm-class") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.linuxStartupWmClass = scalar.value();
    } else if (directive == "linux-keywords") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.linuxKeywords = scalar.value();
    } else if (directive == "linux-appstream-id") {
        auto scalar =
            parsePackageScalar(packageScalarDirectives, directive, value, manifestPath, lineNum);
        if (!scalar)
            return il::support::Expected<bool>(scalar.error());
        config.packageConfig.appstreamId = scalar.value();
    } else if (directive == "post-install") {
        auto seen = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!seen)
            return il::support::Expected<bool>(seen.error());
        auto script = readManifestScriptHook(manifestDir, value, manifestPath, lineNum, directive);
        if (!script)
            return il::support::Expected<bool>(script.error());
        config.packageConfig.postInstallScript = script.value();
    } else if (directive == "pre-uninstall") {
        auto seen = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!seen)
            return il::support::Expected<bool>(seen.error());
        auto script = readManifestScriptHook(manifestDir, value, manifestPath, lineNum, directive);
        if (!script)
            return il::support::Expected<bool>(script.error());
        config.packageConfig.preUninstallScript = script.value();
    } else if (directive == "allow-install-hooks") {
        auto seen = markPackageScalar(packageScalarDirectives, directive, manifestPath, lineNum);
        if (!seen)
            return il::support::Expected<bool>(seen.error());
        auto b = parseBool(value, manifestPath, lineNum, "allow-install-hooks");
        if (!b)
            return il::support::Expected<bool>(b.error());
        config.packageConfig.allowInstallHooks = b.value();
    } else {
        return false;
    }
    return true;
}

} // anonymous namespace

/// @brief Parse a zanna.project manifest file into a ProjectConfig.
/// @details Reads the manifest line by line (stripping a leading UTF-8 BOM,
///          blank lines, and '#' comments), splitting each into a directive and
///          value. Core directives (project/version/lang/entry/sources/exclude/
///          profile/optimize/*-checks) are handled inline with duplicate
///          detection; everything else is offered to parsePackageDirective().
///          After directives are consumed, source files are collected from the
///          declared (or default) directories, the language is auto-detected
///          when not declared, the file list is sorted and de-duplicated, and
///          the entry point is resolved or validated.
/// @param manifestPath Absolute or caller-resolved manifest file path.
/// @return Fully owning project configuration, or a contextual diagnostic.
il::support::Expected<ProjectConfig> parseManifest(const std::string &manifestPath) {
    std::ifstream file(zanna::filesystem::pathFromUtf8(manifestPath));
    if (!file.is_open())
        return makeErr("cannot open manifest: " + manifestPath);

    fs::path manifestDir = zanna::filesystem::pathFromUtf8(manifestPath).parent_path();
    std::error_code ec;
    if (manifestDir.empty()) {
        manifestDir = fs::current_path(ec);
        if (ec)
            return makeErr("cannot resolve current directory: " + ec.message());
    }
    manifestDir = fs::canonical(manifestDir, ec);
    if (ec)
        return makeErr("cannot resolve manifest directory: " + ec.message());

    ProjectConfig config;
    config.rootDir = pathText(manifestDir);
    config.name = pathText(manifestDir.filename());

    std::vector<std::string> sourceDirs;
    std::vector<std::string> excludes;
    bool hasProject = false;
    bool hasVersion = false;
    bool hasLang = false;
    bool hasEntry = false;
    bool hasProfile = false;
    bool hasRunProfile = false;
    bool hasOptimize = false;
    bool hasBoundsChecks = false;
    bool hasOverflowChecks = false;
    bool hasNullChecks = false;
    std::set<std::string> packageScalarDirectives;


    std::string line;
    int lineNum = 0;
    while (std::getline(file, line)) {
        ++lineNum;

        if (lineNum == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0)
            line.erase(0, 3);

        // Strip leading/trailing whitespace
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos)
            continue; // blank line
        line = line.substr(start);
        auto end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos)
            line.resize(end + 1);
        line = stripInlineManifestComment(std::move(line));

        // Skip comments
        if (line.empty() || line[0] == '#')
            continue;

        // Parse directive and value
        auto spacePos = line.find_first_of(" \t");
        if (spacePos == std::string::npos)
            return makeManifestErr(
                manifestPath, lineNum, "directive missing value: '" + line + "'");

        std::string directive = line.substr(0, spacePos);
        auto valueStart = line.find_first_not_of(" \t", spacePos);
        if (valueStart == std::string::npos)
            return makeManifestErr(
                manifestPath, lineNum, "directive missing value: '" + directive + "'");
        std::string value = line.substr(valueStart);

        if (directive == "project") {
            if (hasProject)
                return makeManifestErr(manifestPath, lineNum, "duplicate directive 'project'");
            hasProject = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            config.name = scalar.value();
        } else if (directive == "version") {
            if (hasVersion)
                return makeManifestErr(manifestPath, lineNum, "duplicate directive 'version'");
            hasVersion = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            config.version = scalar.value();
        } else if (directive == "lang") {
            if (hasLang)
                return makeManifestErr(manifestPath, lineNum, "duplicate directive 'lang'");
            hasLang = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            const std::string lang = scalar.value();
            if (lang == "zia")
                config.lang = ProjectLang::Zia;
            else if (lang == "basic")
                config.lang = ProjectLang::Basic;
            else if (lang == "mixed")
                config.lang = ProjectLang::Mixed;
            else
                return makeManifestErr(manifestPath,
                                       lineNum,
                                       "invalid language '" + lang +
                                           "'; expected 'zia', 'basic', or 'mixed'");
        } else if (directive == "entry") {
            if (hasEntry)
                return makeManifestErr(manifestPath, lineNum, "duplicate directive 'entry'");
            hasEntry = true;
            auto entry = parseManifestRelativeToken(
                manifestDir, value, manifestPath, lineNum, directive, false);
            if (!entry)
                return il::support::Expected<ProjectConfig>(entry.error());
            config.entryFile = entry.value();
        } else if (directive == "sources") {
            auto srcDir = parseManifestRelativeToken(
                manifestDir, value, manifestPath, lineNum, directive, true);
            if (!srcDir)
                return il::support::Expected<ProjectConfig>(srcDir.error());
            sourceDirs.push_back(srcDir.value());
        } else if (directive == "exclude") {
            auto tokens = requireManifestTokenCount(value, manifestPath, lineNum, directive, 1);
            if (!tokens)
                return il::support::Expected<ProjectConfig>(tokens.error());
            try {
                std::string clean =
                    zanna::pkg::sanitizePackageRelativePath(tokens.value()[0], "exclude");
                if (clean.empty())
                    return makeManifestErr(manifestPath, lineNum, "exclude path must not be empty");
                excludes.push_back(std::move(clean));
            } catch (const std::exception &ex) {
                return makeManifestErr(manifestPath, lineNum, ex.what());
            }
        } else if (directive == "run-profile") {
            if (hasRunProfile)
                return makeManifestErr(manifestPath, lineNum, "duplicate directive 'run-profile'");
            hasRunProfile = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            if (scalar.value() != "vm" && scalar.value() != "native")
                return makeManifestErr(manifestPath,
                                       lineNum,
                                       "run-profile must be 'vm' or 'native'");
            config.runProfile = scalar.value();
        } else if (directive == "profile" || directive == "build-profile") {
            if (hasProfile)
                return makeManifestErr(manifestPath, lineNum, "duplicate directive 'profile'");
            hasProfile = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            auto mapped = optimizeForBuildProfile(scalar.value(), manifestPath, lineNum);
            if (!mapped)
                return il::support::Expected<ProjectConfig>(mapped.error());
            config.buildProfile = scalar.value();
            config.buildProfileExplicit = true;
            if (!hasOptimize)
                config.optimizeLevel = mapped.value();
        } else if (directive == "optimize") {
            if (hasOptimize)
                return makeManifestErr(manifestPath, lineNum, "duplicate directive 'optimize'");
            hasOptimize = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            if (scalar.value() != "O0" && scalar.value() != "O1" && scalar.value() != "O2")
                return makeManifestErr(manifestPath,
                                       lineNum,
                                       "invalid optimize level '" + scalar.value() +
                                           "'; expected O0, O1, or O2");
            config.optimizeLevel = scalar.value();
            config.optimizeLevelExplicit = true;
        } else if (directive == "bounds-checks") {
            if (hasBoundsChecks)
                return makeManifestErr(
                    manifestPath, lineNum, "duplicate directive 'bounds-checks'");
            hasBoundsChecks = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            auto b = parseBool(scalar.value(), manifestPath, lineNum, "bounds-checks");
            if (!b)
                return il::support::Expected<ProjectConfig>(b.error());
            config.boundsChecks = b.value();
        } else if (directive == "overflow-checks") {
            if (hasOverflowChecks)
                return makeManifestErr(
                    manifestPath, lineNum, "duplicate directive 'overflow-checks'");
            hasOverflowChecks = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            auto b = parseBool(scalar.value(), manifestPath, lineNum, "overflow-checks");
            if (!b)
                return il::support::Expected<ProjectConfig>(b.error());
            config.overflowChecks = b.value();
        } else if (directive == "null-checks") {
            if (hasNullChecks)
                return makeManifestErr(manifestPath, lineNum, "duplicate directive 'null-checks'");
            hasNullChecks = true;
            auto scalar = parseCoreScalar(value, manifestPath, lineNum, directive);
            if (!scalar)
                return il::support::Expected<ProjectConfig>(scalar.error());
            auto b = parseBool(scalar.value(), manifestPath, lineNum, "null-checks");
            if (!b)
                return il::support::Expected<ProjectConfig>(b.error());
            config.nullChecks = b.value();
        } else {
            auto handled = parsePackageDirective(config,
                                                 packageScalarDirectives,
                                                 manifestDir,
                                                 manifestPath,
                                                 directive,
                                                 value,
                                                 lineNum);
            if (!handled)
                return il::support::Expected<ProjectConfig>(handled.error());
            if (!handled.value())
                return makeManifestErr(
                    manifestPath, lineNum, "unknown directive '" + directive + "'");
        }
    }
    if (file.bad())
        return makeErr("error reading manifest: " + manifestPath);

    // Collect source files from declared directories (or project root by default)
    if (sourceDirs.empty())
        sourceDirs.push_back(pathText(manifestDir));

    std::string ext = (config.lang == ProjectLang::Zia) ? ".zia" : ".bas";

    // If no lang specified, auto-detect before collecting
    if (!hasLang) {
        // Scan all source dirs for both extensions
        std::vector<std::string> allZia, allBas;
        for (const auto &sd : sourceDirs) {
            fs::path srcDir = zanna::filesystem::pathFromUtf8(sd);
            if (srcDir.is_relative())
                srcDir = manifestDir / srcDir;
            std::string collectErr;
            auto zia = collectFiles(srcDir, ".zia", excludes, &collectErr);
            if (!collectErr.empty())
                return makeErr(collectErr);
            auto bas = collectFiles(srcDir, ".bas", excludes, &collectErr);
            if (!collectErr.empty())
                return makeErr(collectErr);
            allZia.insert(allZia.end(), zia.begin(), zia.end());
            allBas.insert(allBas.end(), bas.begin(), bas.end());
        }

        if (!allZia.empty() && allBas.empty()) {
            config.lang = ProjectLang::Zia;
            ext = ".zia";
        } else if (!allBas.empty() && allZia.empty()) {
            config.lang = ProjectLang::Basic;
            ext = ".bas";
        } else if (!allZia.empty() && !allBas.empty()) {
            // Auto-detect mixed: requires 'entry' directive.
            config.lang = ProjectLang::Mixed;
            ext = ".zia"; // Will collect both below.
        } else {
            return makeErr("no source files found in project directories");
        }
    }

    if (config.lang == ProjectLang::Mixed) {
        // Mixed projects: collect both .zia and .bas files.
        for (const auto &sd : sourceDirs) {
            fs::path srcDir = zanna::filesystem::pathFromUtf8(sd);
            if (srcDir.is_relative())
                srcDir = manifestDir / srcDir;
            std::error_code srcEc;
            if (!fs::is_directory(srcDir, srcEc))
                return makeErr("sources directory not found: " + pathText(srcDir));

            std::string collectErr;
            auto zia = collectFiles(srcDir, ".zia", excludes, &collectErr);
            if (!collectErr.empty())
                return makeErr(collectErr);
            auto bas = collectFiles(srcDir, ".bas", excludes, &collectErr);
            if (!collectErr.empty())
                return makeErr(collectErr);
            config.ziaFiles.insert(config.ziaFiles.end(), zia.begin(), zia.end());
            config.basicFiles.insert(config.basicFiles.end(), bas.begin(), bas.end());
            config.sourceFiles.insert(config.sourceFiles.end(), zia.begin(), zia.end());
            config.sourceFiles.insert(config.sourceFiles.end(), bas.begin(), bas.end());
        }
    } else {
        for (const auto &sd : sourceDirs) {
            fs::path srcDir = zanna::filesystem::pathFromUtf8(sd);
            if (srcDir.is_relative())
                srcDir = manifestDir / srcDir;
            std::error_code srcEc;
            if (!fs::is_directory(srcDir, srcEc))
                return makeErr("sources directory not found: " + pathText(srcDir));
            std::string collectErr;
            auto files = collectFiles(srcDir, ext, excludes, &collectErr);
            if (!collectErr.empty())
                return makeErr(collectErr);
            config.sourceFiles.insert(config.sourceFiles.end(), files.begin(), files.end());
        }
    }

    // Deduplicate
    std::sort(config.sourceFiles.begin(), config.sourceFiles.end());
    config.sourceFiles.erase(std::unique(config.sourceFiles.begin(), config.sourceFiles.end()),
                             config.sourceFiles.end());
    std::sort(config.ziaFiles.begin(), config.ziaFiles.end());
    config.ziaFiles.erase(std::unique(config.ziaFiles.begin(), config.ziaFiles.end()),
                          config.ziaFiles.end());
    std::sort(config.basicFiles.begin(), config.basicFiles.end());
    config.basicFiles.erase(std::unique(config.basicFiles.begin(), config.basicFiles.end()),
                            config.basicFiles.end());

    if (config.sourceFiles.empty())
        return makeErr("no source files found in project directories");

    // Entry point resolution
    if (!hasEntry) {
        if (config.lang == ProjectLang::Mixed)
            return makeErr("mixed-language projects require an 'entry' directive in zanna.project");

        il::support::Expected<std::string> entry = (config.lang == ProjectLang::Zia)
                                                       ? findZiaEntry(config.sourceFiles)
                                                       : findBasicEntry(config.sourceFiles);
        if (!entry)
            return il::support::Expected<ProjectConfig>(entry.error());
        config.entryFile = std::move(entry.value());
    } else {
        // Verify entry file exists
        std::error_code entryEc;
        if (!fs::is_regular_file(zanna::filesystem::pathFromUtf8(config.entryFile), entryEc))
            return makeErr("entry file not found: " + config.entryFile);
        if (std::find(config.sourceFiles.begin(), config.sourceFiles.end(), config.entryFile) ==
            config.sourceFiles.end()) {
            return makeErr("entry file is not included by project sources: " + config.entryFile);
        }
    }

    return config;
}

/// @brief Resolve a CLI target path into a ProjectConfig.
/// @details Normalises @p target to an absolute path and classifies it: a single
///          .zia or .bas file becomes a one-file project; a zanna.project (or
///          *.project) file is parsed via parseManifest(); a directory either
///          parses a contained zanna.project or falls back to convention-based
///          discovery (discoverConvention()). Anything else yields a diagnostic.
/// @param target CLI file, manifest, or directory path.
/// @return Resolved project configuration, or a diagnostic describing failure.
il::support::Expected<ProjectConfig> resolveProject(const std::string &target) {
    // Determine what the target is
    fs::path targetPath = zanna::filesystem::pathFromUtf8(target);
    std::error_code ec;

    // Handle relative paths
    if (targetPath.is_relative()) {
        auto cwd = fs::current_path(ec);
        if (ec)
            return makeErr("cannot resolve current directory: " + ec.message());
        targetPath = cwd / targetPath;
    }

    if (!fs::exists(targetPath, ec)) {
        if (ec)
            return makeErr("cannot inspect target '" + target + "': " + ec.message());
        return makeErr("target not found: " + target);
    }
    ec.clear();

    // Case 1: Single .zia file
    const std::string targetExt = lowerAscii(pathText(targetPath.extension()));
    const bool targetIsRegularFile = fs::is_regular_file(targetPath, ec);
    if (ec)
        return makeErr("cannot inspect target '" + target + "': " + ec.message());
    ec.clear();

    if (targetIsRegularFile && targetExt == ".zia") {
        ProjectConfig config;
        auto canonical = fs::canonical(targetPath, ec);
        if (ec)
            return makeErr("cannot resolve target: " + target);
        config.lang = ProjectLang::Zia;
        config.entryFile = pathText(canonical);
        config.sourceFiles = {config.entryFile};
        config.rootDir = pathText(canonical.parent_path());
        config.name = pathText(canonical.stem());
        return config;
    }

    // Case 2: Single .bas file
    if (targetIsRegularFile && targetExt == ".bas") {
        ProjectConfig config;
        auto canonical = fs::canonical(targetPath, ec);
        if (ec)
            return makeErr("cannot resolve target: " + target);
        config.lang = ProjectLang::Basic;
        config.entryFile = pathText(canonical);
        config.sourceFiles = {config.entryFile};
        config.rootDir = pathText(canonical.parent_path());
        config.name = pathText(canonical.stem());
        return config;
    }

    // Case 3: Explicit manifest file
    if (targetIsRegularFile) {
        auto filename = pathText(targetPath.filename());
        if (filename == "zanna.project" || targetPath.extension() == ".project") {
            auto canonical = fs::canonical(targetPath, ec);
            if (ec)
                return makeErr("cannot resolve manifest: " + target);
            return parseManifest(pathText(canonical));
        }
        return makeErr(target + " is not a .zia, .bas, or zanna.project file");
    }

    // Case 4: Directory
    const bool targetIsDirectory = fs::is_directory(targetPath, ec);
    if (ec)
        return makeErr("cannot inspect target '" + target + "': " + ec.message());
    ec.clear();
    if (targetIsDirectory) {
        auto canonical = fs::canonical(targetPath, ec);
        if (ec)
            return makeErr("cannot resolve project directory: " + target);

        // Check for zanna.project in directory
        auto manifestPath = canonical / "zanna.project";
        if (fs::exists(manifestPath, ec))
            return parseManifest(pathText(manifestPath));
        if (ec)
            return makeErr("cannot inspect project manifest '" + pathText(manifestPath) +
                           "': " + ec.message());

        // Convention discovery
        return discoverConvention(canonical, {});
    }

    return makeErr(target + " is not a source file, directory, or project manifest");
}

} // namespace il::tools::common
