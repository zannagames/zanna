//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements the protocol-agnostic Zia compiler and editor-service facade.
/// @details Adapts frontend analysis and the runtime-backed project index into
///          owned diagnostic, completion, navigation, hover, symbol, semantic
///          token, and compiler-dump records for LSP and MCP transports.
// Key invariants:
//   - Each analysis call creates a fresh SourceManager (no cross-call state)
//   - CompletionEngine persists for LRU cache benefits
//   - Runtime queries use default ICompilerBridge implementations
// Ownership/Lifetime:
//   - All returned data is fully owned
// Links: tools/zia-server/CompilerBridge.hpp, tools/lsp-common/TextUtils.hpp,
//        tools/lsp-common/DiagnosticUtils.hpp
//
//===----------------------------------------------------------------------===//

#include "tools/zia-server/CompilerBridge.hpp"

#include "common/Filesystem.hpp"
#include "tools/lsp-common/DiagnosticUtils.hpp"
#include "tools/lsp-common/TextUtils.hpp"

#include "frontends/zia/Compiler.hpp"
#include "frontends/zia/Lexer.hpp"
#include "frontends/zia/Sema.hpp"
#include "frontends/zia/ZiaAnalysis.hpp"
#include "frontends/zia/ZiaAstPrinter.hpp"
#include "frontends/zia/ZiaCompletion.hpp"
#include "il/io/Serializer.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include "runtime/collections/rt_map.h"
#include "runtime/collections/rt_seq.h"
#include "runtime/core/rt_string.h"
#include "runtime/graphics/common/rt_zia_completion.h"
#include "runtime/oop/rt_object.h"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zanna::server {

using namespace il::frontends::zia;
namespace fs = std::filesystem;

// --- Helpers ---

/// @brief Map a Zia semantic Symbol::Kind to the server's SymbolInfo kind string.
/// @param k Semantic symbol category.
/// @return Stable lowercase protocol-facing symbol kind.
static std::string symbolKindStr(Symbol::Kind k) {
    switch (k) {
        case Symbol::Kind::Variable:
            return "variable";
        case Symbol::Kind::Parameter:
            return "parameter";
        case Symbol::Kind::Function:
            return "function";
        case Symbol::Kind::Method:
            return "method";
        case Symbol::Kind::Field:
            return "field";
        case Symbol::Kind::Type:
            return "type";
        case Symbol::Kind::Module:
            return "module";
    }
    return "unknown";
}

/// @brief Append token text with C-style escapes for control characters.
/// @details Token dumps are line-oriented text. Escaping tabs, newlines, carriage
///          returns, backslashes, and other control bytes keeps one token per line
///          and makes the output unambiguous for MCP clients and logs.
/// @param out Destination string receiving escaped text.
/// @param text Raw token spelling from the lexer.
static void appendEscapedTokenText(std::string &out, const std::string &text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char c : text) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20u || c == 0x7Fu) {
                    out += "\\x";
                    out.push_back(kHex[c >> 4u]);
                    out.push_back(kHex[c & 0x0Fu]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
}

/// @brief RAII wrapper for runtime strings allocated for editor-service calls.
class RuntimeString {
  public:
    /// @brief Allocate a runtime string containing @p text.
    /// @param text UTF-8 bytes copied into the runtime string.
    /// @throws std::runtime_error if the runtime allocation fails.
    explicit RuntimeString(std::string_view text)
        : value_(rt_string_from_bytes(text.data(), text.size())) {
        if (!value_)
            throw std::runtime_error("failed to allocate runtime string");
    }

    /// @brief Release the owned runtime string reference.
    ~RuntimeString() {
        rt_string_unref(value_);
    }

    RuntimeString(const RuntimeString &) = delete;
    RuntimeString &operator=(const RuntimeString &) = delete;

    /// @brief Access the borrowed runtime string handle.
    /// @return Runtime handle valid for this wrapper's lifetime.
    [[nodiscard]] rt_string get() const {
        return value_;
    }

  private:
    rt_string value_{nullptr};
};

/// @brief Copy a nullable runtime string into an owning standard string.
/// @param value Borrowed runtime string handle, possibly null.
/// @return UTF-8 contents, or an empty string for a null handle.
static std::string rtStringToStd(rt_string value) {
    const char *cstr = value ? rt_string_cstr(value) : "";
    const size_t len = value ? static_cast<size_t>(rt_str_len(value)) : 0;
    return std::string(cstr ? cstr : "", len);
}

/// @brief Read a string-valued field from a runtime map.
/// @param map Borrowed runtime map object, possibly null.
/// @param name Field name to query.
/// @return Owning field value, or an empty string when unavailable.
static std::string mapString(void *map, const char *name) {
    if (!map)
        return {};
    RuntimeString key(name);
    rt_string value = rt_map_get_str(map, key.get());
    std::string out = rtStringToStd(value);
    rt_string_unref(value);
    return out;
}

/// @brief Read an integer-valued field from a runtime map.
/// @param map Borrowed runtime map object, possibly null.
/// @param name Field name to query.
/// @param fallback Value returned when the map or field has no integer.
/// @return Stored integer or @p fallback.
static int64_t mapInt(void *map, const char *name, int64_t fallback = 0) {
    if (!map)
        return fallback;
    RuntimeString key(name);
    return rt_map_get_int_or(map, key.get(), fallback);
}

/// @brief Read a Boolean-valued field from a runtime map.
/// @param map Borrowed runtime map object, possibly null.
/// @param name Field name to query.
/// @param fallback Value returned when the map or field has no Boolean.
/// @return Stored Boolean or @p fallback.
static bool mapBool(void *map, const char *name, bool fallback = false) {
    if (!map)
        return fallback;
    RuntimeString key(name);
    return rt_map_get_bool_or(map, key.get(), fallback ? 1 : 0) != 0;
}

/// @brief Read an object-valued field from a runtime map.
/// @param map Borrowed runtime map object, possibly null.
/// @param name Field name to query.
/// @return Borrowed object pointer, or null when unavailable.
static void *mapObject(void *map, const char *name) {
    if (!map)
        return nullptr;
    RuntimeString key(name);
    return rt_map_get(map, key.get());
}

/// @brief Release and free a runtime object when its reference count reaches zero.
/// @param obj Runtime object pointer, possibly null.
static void releaseRuntimeObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Clamp a signed runtime coordinate into the source-coordinate domain.
/// @param value Runtime-provided line or column value.
/// @return Zero for nonpositive input, UINT32_MAX on overflow, or the converted value.
static uint32_t toSourceCoord(int64_t value) {
    if (value <= 0)
        return 0;
    if (value > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(value);
}

/// @brief Convert a one-based editor column to the runtime's zero-based column.
/// @param col One-based column, with nonpositive values treated as zero.
/// @return Nonnegative zero-based column.
static int zeroBasedColumnForRuntime(int col) {
    return col > 0 ? col - 1 : 0;
}

/// @brief Decode a source range from named fields in a runtime map.
/// @param map Borrowed runtime map containing location fields.
/// @param lineKey Key for the start line.
/// @param columnKey Key for the start column.
/// @param endLineKey Key for the end line.
/// @param endColumnKey Key for the end column.
/// @return Owning protocol source-range record.
static SourceRangeInfo rangeFromMap(void *map,
                                    const char *lineKey,
                                    const char *columnKey,
                                    const char *endLineKey,
                                    const char *endColumnKey) {
    SourceRangeInfo range;
    range.file = mapString(map, "file");
    range.line = toSourceCoord(mapInt(map, lineKey));
    range.column = toSourceCoord(mapInt(map, columnKey));
    range.endLine = toSourceCoord(mapInt(map, endLineKey));
    range.endColumn = toSourceCoord(mapInt(map, endColumnKey));
    return range;
}

/// @brief Decode a navigation location from a runtime map.
/// @param map Borrowed runtime map containing location metadata.
/// @param forceDefinition Whether to mark the result as a definition unconditionally.
/// @return Owning protocol location record.
static LocationInfo locationFromMap(void *map, bool forceDefinition) {
    LocationInfo location;
    location.range = rangeFromMap(map, "line", "column", "endLine", "endColumn");
    location.name = mapString(map, "name");
    location.kind = mapString(map, "kind");
    location.isDefinition = forceDefinition || mapBool(map, "isDefinition");
    return location;
}

/// @brief Decode an optional definition result from a runtime map.
/// @param map Borrowed runtime result map.
/// @return Definition location when the runtime reports `found`, otherwise nullopt.
static std::optional<LocationInfo> definitionFromRuntimeMap(void *map) {
    if (!map || !mapBool(map, "found"))
        return std::nullopt;
    return locationFromMap(map, true);
}

/// @brief Decode a runtime sequence of reference maps.
/// @param seq Borrowed runtime sequence, possibly null.
/// @return Owning reference-location records in runtime order.
static std::vector<LocationInfo> referencesFromRuntimeSeq(void *seq) {
    std::vector<LocationInfo> out;
    const int64_t count = seq ? rt_seq_len(seq) : 0;
    out.reserve(static_cast<size_t>(std::max<int64_t>(count, 0)));
    for (int64_t i = 0; i < count; ++i) {
        void *map = rt_seq_get(seq, i);
        out.push_back(locationFromMap(map, false));
    }
    return out;
}

/// @brief Decode a rename response and its text edits from a runtime map.
/// @param map Borrowed runtime rename-result map.
/// @return Owning rename result with normalized edit ranges.
static RenameResult renameFromRuntimeMap(void *map) {
    RenameResult result;
    result.success = mapBool(map, "success");
    result.reason = mapString(map, "reason");
    void *edits = mapObject(map, "edits");
    const int64_t count = edits ? rt_seq_len(edits) : 0;
    result.edits.reserve(static_cast<size_t>(std::max<int64_t>(count, 0)));
    for (int64_t i = 0; i < count; ++i) {
        void *editMap = rt_seq_get(edits, i);
        TextEditInfo edit;
        edit.range = rangeFromMap(editMap, "startLine", "startColumn", "endLine", "endColumn");
        edit.newText = mapString(editMap, "newText");
        result.edits.push_back(std::move(edit));
    }
    return result;
}

/// @brief Decode one signature parameter from a runtime map.
/// @param map Borrowed parameter record.
/// @return Protocol parameter label and documentation.
static SignatureParameterInfo parameterFromRuntimeMap(void *map) {
    SignatureParameterInfo parameter;
    std::string name = mapString(map, "name");
    std::string type = mapString(map, "type");
    parameter.label = type.empty() ? name : name + ": " + type;
    parameter.documentation = mapString(map, "documentation");
    return parameter;
}

/// @brief Decode one callable signature from a runtime map.
/// @param map Borrowed signature record.
/// @return Protocol signature with all decoded parameters.
static SignatureInfo signatureFromRuntimeMap(void *map) {
    SignatureInfo signature;
    signature.label = mapString(map, "display");
    signature.documentation = mapString(map, "documentation");
    void *params = mapObject(map, "parameters");
    const int64_t count = params ? rt_seq_len(params) : 0;
    signature.parameters.reserve(static_cast<size_t>(std::max<int64_t>(count, 0)));
    for (int64_t i = 0; i < count; ++i)
        signature.parameters.push_back(parameterFromRuntimeMap(rt_seq_get(params, i)));
    return signature;
}

/// @brief Decode signature-help state and overloads from a runtime map.
/// @param map Borrowed runtime signature-help result.
/// @return Owning signature-help response.
static SignatureHelpInfo signatureHelpFromRuntimeMap(void *map) {
    SignatureHelpInfo result;
    result.available = mapBool(map, "available");
    result.activeSignature = static_cast<int>(mapInt(map, "activeSignature"));
    result.activeParameter = static_cast<int>(mapInt(map, "activeParameter"));
    if (!result.available)
        return result;

    void *overloads = mapObject(map, "overloads");
    const int64_t count = overloads ? rt_seq_len(overloads) : 0;
    result.signatures.reserve(static_cast<size_t>(std::max<int64_t>(count, 0)));
    for (int64_t i = 0; i < count; ++i)
        result.signatures.push_back(signatureFromRuntimeMap(rt_seq_get(overloads, i)));

    if (result.signatures.empty()) {
        SignatureInfo signature;
        signature.label = mapString(map, "display");
        signature.documentation = mapString(map, "documentation");
        void *params = mapObject(map, "parameters");
        const int64_t paramCount = params ? rt_seq_len(params) : 0;
        signature.parameters.reserve(static_cast<size_t>(std::max<int64_t>(paramCount, 0)));
        for (int64_t i = 0; i < paramCount; ++i)
            signature.parameters.push_back(parameterFromRuntimeMap(rt_seq_get(params, i)));
        result.signatures.push_back(std::move(signature));
    }
    return result;
}

/// @brief Test whether ASCII text contains a query without regard to case.
/// @param text Candidate text to search.
/// @param needle Query substring; an empty query matches every string.
/// @return true when @p needle occurs in @p text under ASCII case folding.
static bool containsCaseInsensitive(std::string_view text, std::string_view needle) {
    if (needle.empty())
        return true;
    /// @brief Fold one byte to lowercase for case-insensitive comparison.
    /// @param c Byte to normalize.
    /// @return Lowercase representation converted back to `char`.
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    /// @brief Compare one candidate byte with one query byte without regard to case.
    /// @param a Candidate text byte.
    /// @param b Query text byte.
    /// @return `true` when the folded bytes are equal.
    auto it =
        std::search(text.begin(), text.end(), needle.begin(), needle.end(), [&](char a, char b) {
            return lower(a) == lower(b);
        });
    return it != text.end();
}

/// @brief Map a semantic symbol kind to an LSP semantic-token type.
/// @param kind Semantic symbol category.
/// @return Closest protocol semantic-token category.
static SemanticTokenType semanticTypeForSymbolKind(Symbol::Kind kind) {
    switch (kind) {
        case Symbol::Kind::Function:
            return SemanticTokenType::Function;
        case Symbol::Kind::Method:
            return SemanticTokenType::Method;
        case Symbol::Kind::Field:
            return SemanticTokenType::Property;
        case Symbol::Kind::Type:
            return SemanticTokenType::Type;
        case Symbol::Kind::Module:
            return SemanticTokenType::Namespace;
        case Symbol::Kind::Parameter:
        case Symbol::Kind::Variable:
            return SemanticTokenType::Variable;
    }
    return SemanticTokenType::Variable;
}

/// @brief Classify a token using semantic symbol data when available.
/// @param token Lexer token to classify.
/// @param previous Previous non-error token kind, used to recognize declarations.
/// @param semanticNames Known semantic classifications keyed by identifier spelling.
/// @return Protocol semantic-token category for @p token.
static SemanticTokenType semanticTypeForToken(
    const Token &token,
    TokenKind previous,
    const std::unordered_map<std::string, SemanticTokenType> &semanticNames) {
    if (token.isKeyword())
        return SemanticTokenType::Keyword;

    switch (token.kind) {
        case TokenKind::Identifier:
            if (previous == TokenKind::KwFunc)
                return SemanticTokenType::Function;
            if (previous == TokenKind::KwClass || previous == TokenKind::KwStruct ||
                previous == TokenKind::KwType) {
                return previous == TokenKind::KwClass ? SemanticTokenType::Class
                                                      : SemanticTokenType::Type;
            }
            if (previous == TokenKind::KwEnum)
                return SemanticTokenType::Enum;
            if (previous == TokenKind::KwInterface)
                return SemanticTokenType::Interface;
            if (auto it = semanticNames.find(token.text); it != semanticNames.end())
                return it->second;
            return SemanticTokenType::Variable;
        case TokenKind::IntegerLiteral:
        case TokenKind::NumberLiteral:
            return SemanticTokenType::Number;
        case TokenKind::StringLiteral:
        case TokenKind::StringStart:
        case TokenKind::StringMid:
        case TokenKind::StringEnd:
            return SemanticTokenType::String;
        default:
            return SemanticTokenType::Operator;
    }
}

/// @brief Build semantic token classification data from a successful Zia analysis.
/// @param analysis Frontend analysis containing semantic symbols and type names.
/// @return Identifier-to-token-category lookup map.
static std::unordered_map<std::string, SemanticTokenType> buildSemanticNameMap(
    const AnalysisResult &analysis) {
    std::unordered_map<std::string, SemanticTokenType> names;
    if (!analysis.sema)
        return names;
    for (const auto &sym : analysis.sema->getGlobalSymbols())
        names.emplace(sym.name, semanticTypeForSymbolKind(sym.kind));
    for (const auto &typeName : analysis.sema->getTypeNames())
        names.emplace(typeName, SemanticTokenType::Type);
    return names;
}

/// @brief Index declaration-token locations for Zia type-like declarations.
/// @details The semantic API exposes type names but not every declaration span.
///          This lexer pass records the identifier immediately following class,
///          struct, type, enum, and interface keywords so document symbols use
///          declaration locations instead of an arbitrary text search match.
/// @param source Complete Zia document text.
/// @param fileId Source-manager file identifier assigned to @p source.
/// @return Declaration locations keyed by declared type name.
static std::unordered_map<std::string, il::support::SourceLoc> indexZiaTypeDeclarationLocations(
    const std::string &source, uint32_t fileId) {
    std::unordered_map<std::string, il::support::SourceLoc> out;
    il::support::DiagnosticEngine diag;
    Lexer lexer(source, fileId, diag);
    TokenKind previous = TokenKind::Eof;
    while (true) {
        Token token = lexer.next();
        if (token.kind == TokenKind::Eof)
            break;
        if (token.kind == TokenKind::Identifier &&
            (previous == TokenKind::KwClass || previous == TokenKind::KwStruct ||
             previous == TokenKind::KwType || previous == TokenKind::KwEnum ||
             previous == TokenKind::KwInterface)) {
            out.emplace(token.text, token.loc);
        }
        if (token.kind != TokenKind::Error)
            previous = token.kind;
    }
    return out;
}

/// @brief Read a workspace `.zia` source file if it is small enough for symbols.
/// @param path Native filesystem path to inspect.
/// @return File contents when readable and at most one MiB; otherwise nullopt.
static std::optional<std::string> readWorkspaceSourceFile(const fs::path &path) {
    constexpr std::streamoff kMaxWorkspaceSymbolFileBytes =
        static_cast<std::streamoff>(1024ULL * 1024ULL);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return std::nullopt;
    const std::streamoff size = in.tellg();
    if (size < 0 || size > kMaxWorkspaceSymbolFileBytes)
        return std::nullopt;
    in.seekg(0, std::ios::beg);
    if (!in)
        return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size > 0)
        in.read(text.data(), size);
    if (!in)
        return std::nullopt;
    return text;
}

/// @brief Collect bounded `.zia` workspace files rooted at open document directories.
/// @details The language server does not own a formal workspace-root contract in
///          the compiler bridge, so open documents define the search roots. This
///          avoids scanning arbitrary process current directories while still
///          finding nearby project files for normal editor sessions.
/// @param openDocuments Current in-memory documents keyed by path or editor URI.
/// @return Open documents plus bounded neighboring `.zia` files, without duplicates.
static std::vector<std::pair<std::string, std::string>> collectWorkspaceZiaSources(
    const std::unordered_map<std::string, std::string> &openDocuments) {
    constexpr std::size_t kMaxWorkspaceSymbolFiles = 256;
    std::vector<std::pair<std::string, std::string>> docs;
    docs.reserve(openDocuments.size());
    std::unordered_set<std::string> seen;
    std::vector<fs::path> roots;
    std::unordered_set<std::string> rootKeys;
    for (const auto &[path, source] : openDocuments) {
        docs.emplace_back(path, source);
        seen.insert(path);
        std::error_code ec;
        const fs::path nativePath = zanna::filesystem::pathFromUtf8(path);
        fs::path root = fs::weakly_canonical(nativePath.parent_path(), ec);
        if (ec)
            root = nativePath.parent_path().lexically_normal();
        /* Synthetic editor URIs such as file:///test.zia resolve directly under
         * a filesystem root. Recursing from there is both unrelated to the open
         * document and potentially unbounded; keep the open document indexed,
         * but do not treat a volume root as an inferred workspace. */
        if (root.empty() || root == root.root_path())
            continue;
        std::string key = zanna::filesystem::pathToUtf8(root);
        if (rootKeys.insert(key).second)
            roots.push_back(std::move(root));
    }

    for (const auto &root : roots) {
        if (docs.size() >= kMaxWorkspaceSymbolFiles)
            break;
        std::error_code ec;
        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; !ec && it != end && docs.size() < kMaxWorkspaceSymbolFiles; it.increment(ec)) {
            std::error_code entryEc;
            if (!it->is_regular_file(entryEc) || entryEc)
                continue;
            if (it->path().extension() != ".zia")
                continue;
            fs::path canonical = fs::weakly_canonical(it->path(), entryEc);
            std::string path = zanna::filesystem::pathToUtf8(entryEc ? it->path() : canonical);
            if (!seen.insert(path).second)
                continue;
            auto source = readWorkspaceSourceFile(it->path());
            if (source)
                docs.emplace_back(std::move(path), std::move(*source));
        }
    }
    return docs;
}

// --- Constructor / Destructor ---

/// @brief Construct the bridge, completion cache, and runtime project index.
/// @details The runtime index is marked unusable when allocation fails, allowing
///          independent compiler features to remain available.
CompilerBridge::CompilerBridge() : completionEngine_(std::make_unique<CompletionEngine>()) {
    RuntimeString root(".");
    projectIndex_ = rt_zia_project_index_new(root.get());
    projectIndexUsable_ = projectIndex_ != nullptr;
}

/// @brief Destroy the runtime project index under the project-state lock.
CompilerBridge::~CompilerBridge() {
    std::lock_guard<std::mutex> lock(projectMutex_);
    if (projectIndex_) {
        rt_zia_project_index_destroy(projectIndex_);
        releaseRuntimeObject(projectIndex_);
        projectIndex_ = nullptr;
    }
}

// --- Analysis ---

/// @brief Parse and semantically analyze a Zia document.
/// @param source Complete in-memory document text.
/// @param path Logical path used for source locations.
/// @return Owned diagnostics emitted by parsing and semantic analysis.
std::vector<DiagnosticInfo> CompilerBridge::check(const std::string &source,
                                                  const std::string &path) {
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    if (!result) {
        DiagnosticInfo info;
        info.severity = 2;
        info.message = "internal error: Zia analysis did not produce a result";
        info.file = path;
        info.code = "V-LSP-ANALYSIS";
        return {info};
    }
    return extractDiagnostics(result->diagnostics, &sm);
}

/// @brief Compile a Zia document through IL lowering and verification.
/// @param source Complete in-memory document text.
/// @param path Logical path used for source locations.
/// @return Success flag and owned compiler diagnostics.
CompileResult CompilerBridge::compile(const std::string &source, const std::string &path) {
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};

    auto result = il::frontends::zia::compile(input, opts, sm);
    return {result.succeeded(), extractDiagnostics(result.diagnostics, &sm)};
}

/// @brief Add or replace an open document in the workspace project index.
/// @param path Document path or editor URI used as the index key.
/// @param source Complete current document text.
void CompilerBridge::updateDocument(const std::string &path, const std::string &source) {
    std::lock_guard<std::mutex> lock(projectMutex_);
    openDocuments_[path] = source;
    workspaceSymbolCacheDirty_ = true;
    if (!projectIndex_)
        return;
    RuntimeString pathValue(path);
    RuntimeString sourceValue(source);
    if (rt_zia_project_index_update_file(projectIndex_, pathValue.get(), sourceValue.get()) == 0)
        projectIndexUsable_ = false;
    else
        projectIndexUsable_ = true;
}

/// @brief Remove an open document from workspace state and the runtime index.
/// @param path Document path or editor URI to remove.
void CompilerBridge::removeDocument(const std::string &path) {
    std::lock_guard<std::mutex> lock(projectMutex_);
    const bool wasOpen = openDocuments_.erase(path) != 0;
    if (wasOpen)
        workspaceSymbolCacheDirty_ = true;
    if (!projectIndex_)
        return;
    RuntimeString pathValue(path);
    if (wasOpen && rt_zia_project_index_remove_file(projectIndex_, pathValue.get()) == 0)
        projectIndexUsable_ = false;
    else if (wasOpen)
        projectIndexUsable_ = true;
}

/// @brief Report whether a runtime project index exists for definition lookup.
/// @return true when navigation requests can be submitted to an index object.
bool CompilerBridge::supportsDefinition() const {
    std::lock_guard<std::mutex> lock(projectMutex_);
    return projectIndex_ != nullptr;
}

/// @brief Report whether reference lookup is exposed.
/// @return The same availability state as definition lookup.
bool CompilerBridge::supportsReferences() const {
    return supportsDefinition();
}

/// @brief Report whether semantic rename is exposed.
/// @return The same availability state as definition lookup.
bool CompilerBridge::supportsRename() const {
    return supportsDefinition();
}

/// @brief Report whether signature help is implemented.
/// @return Always true for this bridge.
bool CompilerBridge::supportsSignatureHelp() const {
    return true;
}

/// @brief Report whether workspace symbol search is implemented.
/// @return Always true for this bridge.
bool CompilerBridge::supportsWorkspaceSymbols() const {
    return true;
}

/// @brief Report whether semantic token classification is implemented.
/// @return Always true for this bridge.
bool CompilerBridge::supportsSemanticTokens() const {
    return true;
}

// --- Hover helpers ---

/// @brief Resolved hover information for markdown formatting.
struct HoverResult {
    std::string name;
    std::string
        kind; ///< "variable","parameter","function","method","field","type","module","runtime-class"
    std::string type;          ///< developer-facing semantic type string
    std::string signature;     ///< Full signature for functions/methods
    std::string ownerName;     ///< Parent type name for members
    std::string documentation; ///< Authored documentation for runtime classes.
    bool isFinal{false};
    bool isExtern{false};
};

/// @brief Return combined authored documentation for a runtime class.
/// @param qualifiedName Fully qualified runtime class name.
/// @return Summary and details separated by a blank line, or an empty string.
static std::string runtimeClassDocumentation(std::string_view qualifiedName) {
    const auto *runtimeClass = il::runtime::findRuntimeClassByQName(qualifiedName);
    if (!runtimeClass)
        return {};
    std::string documentation = runtimeClass->summary ? runtimeClass->summary : "";
    if (runtimeClass->details && *runtimeClass->details) {
        if (!documentation.empty())
            documentation += "\n\n";
        documentation += runtimeClass->details;
    }
    return documentation;
}

/// @brief Populate hover metadata for a catalogued runtime class.
/// @param result Output hover record overwritten on a successful lookup.
/// @param qualifiedName Fully qualified runtime class name.
/// @return true when the runtime class exists and @p result was populated.
static bool populateRuntimeClassHover(HoverResult &result, std::string_view qualifiedName) {
    const auto *runtimeClass = il::runtime::findRuntimeClassByQName(qualifiedName);
    if (!runtimeClass)
        return false;
    result.name = std::string(qualifiedName);
    result.kind = "runtime-class";
    result.signature = std::to_string(runtimeClass->properties.size()) + " properties, " +
                       std::to_string(runtimeClass->methods.size()) + " methods";
    result.documentation = runtimeClassDocumentation(qualifiedName);
    return true;
}

/// @brief Build a human-readable function signature from AST param names + semantic types.
/// @param params Declared parameters supplying source-level names.
/// @param funcType Semantic function type supplying parameter and return types.
/// @return Display signature in `(name: type) -> return` form.
static std::string buildSignatureFromDecl(const std::vector<Param> &params,
                                          const TypeRef &funcType) {
    auto paramTys = funcType ? funcType->paramTypes() : std::vector<TypeRef>{};
    auto retTy = funcType ? funcType->returnType() : TypeRef{};
    std::string sig = "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0)
            sig += ", ";
        sig += params[i].name + ": ";
        if (i < paramTys.size() && paramTys[i])
            sig += paramTys[i]->toDisplayString();
        else
            sig += "?";
    }
    sig += ")";
    if (retTy && retTy->kind != TypeKindSem::Unit && retTy->kind != TypeKindSem::Void)
        sig += " -> " + retTy->toDisplayString();
    return sig;
}

/// @brief Build a function signature from just the ZannaType (no param names).
/// @param funcType Semantic function type to render.
/// @return Display signature, or an empty string for a non-function type.
static std::string buildSignatureFromType(const TypeRef &funcType) {
    if (!funcType || funcType->kind != TypeKindSem::Function)
        return "";
    auto paramTys = funcType->paramTypes();
    auto retTy = funcType->returnType();
    std::string sig = "(";
    for (size_t i = 0; i < paramTys.size(); ++i) {
        if (i > 0)
            sig += ", ";
        sig += paramTys[i] ? paramTys[i]->toDisplayString() : "?";
    }
    sig += ")";
    if (retTy && retTy->kind != TypeKindSem::Unit && retTy->kind != TypeKindSem::Void)
        sig += " -> " + retTy->toDisplayString();
    return sig;
}

/// @brief Resolve a hover target using Sema APIs.
/// @param ar Parsed and analyzed document, including the AST.
/// @param sema Semantic model used for symbol and member lookup.
/// @param ctx Identifier and dotted-prefix context at the cursor.
/// @param line One-based cursor line.
/// @param col One-based cursor column.
/// @return Resolved hover record; an empty name indicates no target.
static HoverResult resolveHoverTarget(
    const AnalysisResult &ar, const Sema &sema, const HoverContext &ctx, int line, int col) {
    HoverResult result;

    if (!ctx.dotPrefix.empty()) {
        // ── Dotted expression: resolve prefix, then find member ──
        std::vector<std::string> parts;
        std::string token;
        for (char c : ctx.dotPrefix) {
            if (c == '.') {
                if (!token.empty())
                    parts.push_back(token);
                token.clear();
            } else {
                token += c;
            }
        }
        if (!token.empty())
            parts.push_back(token);

        if (parts.empty())
            return result;

        // A literal qualified class name (for example, a type annotation such
        // as `Zanna.GUI.App`) does not require a module binding to resolve.
        if (populateRuntimeClassHover(result, ctx.dotPrefix + "." + ctx.identifier))
            return result;

        // Look up first part in globals.
        TypeRef current;
        auto globals = sema.getGlobalSymbols();
        for (const auto &sym : globals) {
            if (sym.name == parts[0]) {
                current = sym.type;
                break;
            }
        }

        // Try position-based lookup (locals, params, class fields).
        if (!current) {
            auto *scoped = sema.findSymbolAtPosition(
                parts[0], ar.fileId, static_cast<uint32_t>(line), static_cast<uint32_t>(col));
            if (scoped)
                current = scoped->symbol.type;
        }

        // Try module alias expansion.
        if (!current) {
            std::string ns = sema.resolveModuleAlias(parts[0]);
            if (!ns.empty()) {
                std::string fullQname = ns;
                for (size_t i = 1; i < parts.size(); ++i)
                    fullQname += "." + parts[i];

                std::string classQname = fullQname + "." + ctx.identifier;
                if (populateRuntimeClassHover(result, classQname))
                    return result;

                auto members = sema.getRuntimeMembers(fullQname);
                if (!members.empty())
                    current = types::runtimeClass(fullQname);
            }
        }

        // Handle Module type.
        if (current && current->kind == TypeKindSem::Module && !current->name.empty()) {
            std::string fullQname = current->name;
            for (size_t i = 1; i < parts.size(); ++i)
                fullQname += "." + parts[i];

            std::string classQname = fullQname + "." + ctx.identifier;
            if (populateRuntimeClassHover(result, classQname))
                return result;

            auto members = sema.getRuntimeMembers(fullQname);
            if (!members.empty())
                current = types::runtimeClass(fullQname);
        }

        // Walk remaining prefix parts via getMembersOf.
        if (current) {
            for (size_t i = 1; i < parts.size(); ++i) {
                auto members = sema.getMembersOf(current);
                bool found = false;
                for (const auto &mem : members) {
                    if (mem.name == parts[i]) {
                        if (mem.type && mem.type->kind == TypeKindSem::Function)
                            current = mem.type->returnType();
                        else
                            current = mem.type;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return result;
            }
        }

        if (!current)
            return result;

        // Search for the identifier in members of the resolved type.
        std::string ownerName;
        if (current->kind == TypeKindSem::Ptr && !current->name.empty())
            ownerName = current->name;
        else if (!current->name.empty())
            ownerName = current->name;

        auto members = sema.getMembersOf(current);
        for (const auto &mem : members) {
            if (mem.name == ctx.identifier) {
                result.name = mem.name;
                result.kind = symbolKindStr(mem.kind);
                result.type = mem.type ? mem.type->toDisplayString() : "";
                result.ownerName = ownerName;
                result.isFinal = mem.isFinal;
                result.isExtern = mem.isExtern;
                if (mem.type && mem.type->kind == TypeKindSem::Function) {
                    if (mem.decl && mem.decl->kind == DeclKind::Method) {
                        auto *md = static_cast<MethodDecl *>(mem.decl);
                        result.signature = buildSignatureFromDecl(md->params, mem.type);
                    } else {
                        result.signature = buildSignatureFromType(mem.type);
                    }
                }
                return result;
            }
        }

        return result;
    }

    // ── No dot prefix: search position-based, then globals, types, module aliases ──

    // 0. Position-based lookup.
    {
        auto *scoped = sema.findSymbolAtPosition(
            ctx.identifier, ar.fileId, static_cast<uint32_t>(line), static_cast<uint32_t>(col));
        if (scoped) {
            result.name = scoped->symbol.name;
            result.kind = symbolKindStr(scoped->symbol.kind);
            result.type = scoped->symbol.type ? scoped->symbol.type->toDisplayString() : "";
            result.isFinal = scoped->symbol.isFinal;
            result.isExtern = scoped->symbol.isExtern;
            result.ownerName = scoped->ownerType;
            if (scoped->symbol.type && scoped->symbol.type->kind == TypeKindSem::Function) {
                if (scoped->symbol.decl && scoped->symbol.decl->kind == DeclKind::Method) {
                    auto *md = static_cast<MethodDecl *>(scoped->symbol.decl);
                    result.signature = buildSignatureFromDecl(md->params, scoped->symbol.type);
                } else if (scoped->symbol.decl && scoped->symbol.decl->kind == DeclKind::Function) {
                    auto *fd = static_cast<FunctionDecl *>(scoped->symbol.decl);
                    result.signature = buildSignatureFromDecl(fd->params, scoped->symbol.type);
                } else {
                    result.signature = buildSignatureFromType(scoped->symbol.type);
                }
            }
            return result;
        }
    }

    // 1. Global symbols.
    auto globals = sema.getGlobalSymbols();
    for (const auto &sym : globals) {
        if (sym.name == ctx.identifier) {
            result.name = sym.name;
            result.kind = symbolKindStr(sym.kind);
            result.type = sym.type ? sym.type->toDisplayString() : "";
            result.isFinal = sym.isFinal;
            result.isExtern = sym.isExtern;
            if (sym.type && sym.type->kind == TypeKindSem::Function) {
                if (sym.decl && sym.decl->kind == DeclKind::Function) {
                    auto *fd = static_cast<FunctionDecl *>(sym.decl);
                    result.signature = buildSignatureFromDecl(fd->params, sym.type);
                } else {
                    result.signature = buildSignatureFromType(sym.type);
                }
            }
            return result;
        }
    }

    // 2. User-defined type names.
    auto typeNames = sema.getTypeNames();
    for (const auto &tn : typeNames) {
        if (tn == ctx.identifier) {
            result.name = tn;
            result.kind = "type";
            result.type = tn;
            return result;
        }
    }

    // 3. Module aliases.
    std::string ns = sema.resolveModuleAlias(ctx.identifier);
    if (!ns.empty()) {
        result.name = ctx.identifier;
        result.kind = "module";
        result.type = ns;
        result.documentation = runtimeClassDocumentation(ns);
        return result;
    }

    return result;
}

/// @brief Escape text before inserting it into generated Markdown code spans or fences.
/// @details Compiler-provided identifiers and type strings should be well-formed, but hover text is
///          still user-visible markdown. Escaping backticks and replacing control characters keeps
///          a malformed or adversarial symbol spelling from closing a fence or corrupting the LSP
///          hover payload.
/// @param text Identifier, type, or signature text to sanitize.
/// @return Markdown-safe code text without raw control characters or backticks.
static std::string escapeMarkdownCodeText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '`') {
            out += "\\`";
        } else if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// @brief Format hover info as rich markdown.
/// @param info Resolved semantic hover record.
/// @return Zia code fence, contextual annotations, and authored documentation.
static std::string formatHoverMarkdown(const HoverResult &info) {
    std::string md;

    if (info.kind == "function") {
        md += "```zia\nfunc " + escapeMarkdownCodeText(info.name);
        if (!info.signature.empty())
            md += escapeMarkdownCodeText(info.signature);
        md += "\n```";
    } else if (info.kind == "method") {
        md += "```zia\nmethod " + escapeMarkdownCodeText(info.name);
        if (!info.signature.empty())
            md += escapeMarkdownCodeText(info.signature);
        md += "\n```";
        if (!info.ownerName.empty())
            md += "\n\n*Member of `" + escapeMarkdownCodeText(info.ownerName) + "`*";
    } else if (info.kind == "variable") {
        md += "```zia\n";
        if (info.isFinal)
            md += "final ";
        else
            md += "var ";
        md += escapeMarkdownCodeText(info.name);
        if (!info.type.empty())
            md += ": " + escapeMarkdownCodeText(info.type);
        md += "\n```";
    } else if (info.kind == "parameter") {
        md += "```zia\n" + escapeMarkdownCodeText(info.name);
        if (!info.type.empty())
            md += ": " + escapeMarkdownCodeText(info.type);
        md += "\n```\n\n*Parameter*";
    } else if (info.kind == "field") {
        md += "```zia\nfield " + escapeMarkdownCodeText(info.name);
        if (!info.type.empty())
            md += ": " + escapeMarkdownCodeText(info.type);
        md += "\n```";
        if (!info.ownerName.empty())
            md += "\n\n*Member of `" + escapeMarkdownCodeText(info.ownerName) + "`*";
    } else if (info.kind == "type") {
        md += "```zia\nclass " + escapeMarkdownCodeText(info.name) + "\n```";
    } else if (info.kind == "module") {
        md += "```zia\nbind " + escapeMarkdownCodeText(info.name) + " = " +
              escapeMarkdownCodeText(info.type) + "\n```\n\n*Module namespace*";
    } else if (info.kind == "runtime-class") {
        md += "```zia\nclass " + escapeMarkdownCodeText(info.name) + "\n```";
        if (!info.signature.empty())
            md += "\n\n*Runtime class — " + escapeMarkdownCodeText(info.signature) + "*";
    } else {
        md += "```zia\n" + escapeMarkdownCodeText(info.name);
        if (!info.type.empty())
            md += ": " + escapeMarkdownCodeText(info.type);
        md += "\n```";
    }

    if (!info.documentation.empty())
        md += "\n\n" + info.documentation;

    return md;
}

// --- IDE Features ---

/// @brief Find the definition of the symbol at a document position.
/// @param source Complete current document text.
/// @param line One-based cursor line.
/// @param col One-based cursor column.
/// @param path Document path or editor URI.
/// @return Definition location, or nullopt when the index is unavailable or no target exists.
std::optional<LocationInfo> CompilerBridge::definition(const std::string &source,
                                                       int line,
                                                       int col,
                                                       const std::string &path) {
    std::lock_guard<std::mutex> lock(projectMutex_);
    if (!projectIndex_ || !projectIndexUsable_)
        return std::nullopt;
    openDocuments_[path] = source;
    RuntimeString pathValue(path);
    RuntimeString sourceValue(source);
    if (rt_zia_project_index_update_file(projectIndex_, pathValue.get(), sourceValue.get()) == 0) {
        projectIndexUsable_ = false;
        return std::nullopt;
    }
    void *map = rt_zia_project_index_definition(
        projectIndex_, pathValue.get(), sourceValue.get(), line, zeroBasedColumnForRuntime(col));
    auto result = definitionFromRuntimeMap(map);
    releaseRuntimeObject(map);
    return result;
}

/// @brief Find semantic references to the symbol at a document position.
/// @param source Complete current document text.
/// @param line One-based cursor line.
/// @param col One-based cursor column.
/// @param path Document path or editor URI.
/// @return Reference locations, or an empty vector when unavailable or unmatched.
std::vector<LocationInfo> CompilerBridge::references(const std::string &source,
                                                     int line,
                                                     int col,
                                                     const std::string &path) {
    std::lock_guard<std::mutex> lock(projectMutex_);
    if (!projectIndex_ || !projectIndexUsable_)
        return {};
    openDocuments_[path] = source;
    RuntimeString pathValue(path);
    RuntimeString sourceValue(source);
    if (rt_zia_project_index_update_file(projectIndex_, pathValue.get(), sourceValue.get()) == 0) {
        projectIndexUsable_ = false;
        return {};
    }
    void *seq = rt_zia_project_index_references(
        projectIndex_, pathValue.get(), sourceValue.get(), line, zeroBasedColumnForRuntime(col));
    auto result = referencesFromRuntimeSeq(seq);
    releaseRuntimeObject(seq);
    return result;
}

/// @brief Compute workspace text edits for renaming a symbol.
/// @param source Complete current document text.
/// @param line One-based cursor line.
/// @param col One-based cursor column.
/// @param path Document path or editor URI.
/// @param newName Replacement identifier spelling.
/// @return Rename status, failure reason, and all semantic edits.
RenameResult CompilerBridge::rename(const std::string &source,
                                    int line,
                                    int col,
                                    const std::string &path,
                                    const std::string &newName) {
    std::lock_guard<std::mutex> lock(projectMutex_);
    if (!projectIndex_ || !projectIndexUsable_) {
        RenameResult invalid;
        invalid.reason = "invalid_index";
        return invalid;
    }
    openDocuments_[path] = source;
    RuntimeString pathValue(path);
    RuntimeString sourceValue(source);
    RuntimeString newNameValue(newName);
    if (rt_zia_project_index_update_file(projectIndex_, pathValue.get(), sourceValue.get()) == 0) {
        projectIndexUsable_ = false;
        RenameResult invalid;
        invalid.reason = "invalid_index";
        return invalid;
    }
    void *map = rt_zia_project_index_rename_edits(projectIndex_,
                                                  pathValue.get(),
                                                  sourceValue.get(),
                                                  line,
                                                  zeroBasedColumnForRuntime(col),
                                                  newNameValue.get());
    RenameResult result = renameFromRuntimeMap(map);
    releaseRuntimeObject(map);
    return result;
}

/// @brief Resolve callable overload and active-parameter information at a cursor.
/// @param source Complete current document text.
/// @param line One-based cursor line.
/// @param col One-based cursor column.
/// @param path Document path or editor URI.
/// @return Signature-help response decoded from the runtime editor service.
SignatureHelpInfo CompilerBridge::signatureHelp(const std::string &source,
                                                int line,
                                                int col,
                                                const std::string &path) {
    RuntimeString sourceValue(source);
    RuntimeString pathValue(path);
    void *map = rt_zia_signature_info_for_file(
        sourceValue.get(), pathValue.get(), line, zeroBasedColumnForRuntime(col));
    SignatureHelpInfo result = signatureHelpFromRuntimeMap(map);
    releaseRuntimeObject(map);
    return result;
}

/// @brief Search cached document symbols across the inferred workspace.
/// @details Rebuilds the cache from open documents and bounded neighboring Zia
///          files after document changes, then applies a case-insensitive filter.
/// @param query Symbol-name substring; an empty query returns all cached symbols.
/// @return Deterministically ordered matching symbol records.
std::vector<SymbolInfo> CompilerBridge::workspaceSymbols(const std::string &query) {
    std::vector<SymbolInfo> cachedSymbols;
    std::unordered_map<std::string, std::string> openDocs;
    bool rebuild = false;
    {
        std::lock_guard<std::mutex> lock(projectMutex_);
        if (workspaceSymbolCacheDirty_) {
            openDocs = openDocuments_;
            rebuild = true;
        } else {
            cachedSymbols = workspaceSymbolCache_;
        }
    }

    if (rebuild) {
        auto docs = collectWorkspaceZiaSources(openDocs);
        std::vector<SymbolInfo> rebuilt;
        for (const auto &[path, source] : docs) {
            auto docSymbols = symbols(source, path);
            for (auto &symbol : docSymbols) {
                if (symbol.file.empty())
                    symbol.file = path;
                rebuilt.push_back(std::move(symbol));
            }
        }
        /// @brief Order workspace symbols deterministically by name, file, line, and column.
        /// @param lhs Left-hand symbol.
        /// @param rhs Right-hand symbol.
        /// @return `true` when `lhs` precedes `rhs`.
        std::sort(rebuilt.begin(), rebuilt.end(), [](const SymbolInfo &lhs, const SymbolInfo &rhs) {
            if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
            if (lhs.file != rhs.file)
                return lhs.file < rhs.file;
            if (lhs.line != rhs.line)
                return lhs.line < rhs.line;
            return lhs.column < rhs.column;
        });
        {
            std::lock_guard<std::mutex> lock(projectMutex_);
            workspaceSymbolCache_ = std::move(rebuilt);
            workspaceSymbolCacheDirty_ = false;
            cachedSymbols = workspaceSymbolCache_;
        }
    }

    std::vector<SymbolInfo> out;
    out.reserve(cachedSymbols.size());
    for (const auto &symbol : cachedSymbols) {
        if (containsCaseInsensitive(symbol.name, query))
            out.push_back(symbol);
    }
    return out;
}

/// @brief Classify lexical tokens for semantic highlighting.
/// @param source Complete current document text.
/// @param path Document path or editor URI used for analysis locations.
/// @return Source-ordered semantic token spans.
std::vector<SemanticTokenInfo> CompilerBridge::semanticTokens(const std::string &source,
                                                              const std::string &path) {
    il::support::DiagnosticEngine diag;
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};
    auto analysis = parseAndAnalyze(input, opts, sm);
    const auto semanticNames = analysis ? buildSemanticNameMap(*analysis)
                                        : std::unordered_map<std::string, SemanticTokenType>{};

    uint32_t fileId = sm.addFile(path);
    Lexer lexer(source, fileId, diag);

    std::vector<SemanticTokenInfo> out;
    TokenKind previous = TokenKind::Eof;
    while (true) {
        Token token = lexer.next();
        if (token.kind == TokenKind::Eof)
            break;
        if (token.kind == TokenKind::Error || token.text.empty())
            continue;

        SemanticTokenInfo info;
        info.line = token.loc.line;
        info.column = token.loc.column;
        info.length = static_cast<uint32_t>(
            std::min<size_t>(token.text.size(), std::numeric_limits<uint32_t>::max()));
        info.type = semanticTypeForToken(token, previous, semanticNames);
        out.push_back(info);
        previous = token.kind;
    }
    return out;
}

/// @brief Compute completion candidates at a document position.
/// @param source Complete current document text.
/// @param line One-based cursor line.
/// @param col One-based cursor column.
/// @param path Document path or editor URI.
/// @return Owning protocol completion records in engine order.
std::vector<CompletionInfo> CompilerBridge::completions(const std::string &source,
                                                        int line,
                                                        int col,
                                                        const std::string &path) {
    std::lock_guard<std::mutex> lock(completionMutex_);
    auto items = completionEngine_->complete(source, line, col, path);
    std::vector<CompletionInfo> result;
    result.reserve(items.size());
    for (const auto &item : items) {
        result.push_back({item.label,
                          item.insertText,
                          static_cast<int>(item.kind),
                          item.detail,
                          item.sortPriority,
                          item.documentation});
    }
    return result;
}

/// @brief Produce rich Markdown hover information at a document position.
/// @param source Complete current document text.
/// @param line One-based cursor line.
/// @param col One-based cursor column.
/// @param path Document path or editor URI.
/// @return Markdown hover payload, or an empty string when no symbol resolves.
std::string CompilerBridge::hover(const std::string &source,
                                  int line,
                                  int col,
                                  const std::string &path) {
    auto ctx = extractIdentifierAtCursor(source, line, col);
    if (!ctx.valid)
        return "";

    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    if (!result || !result->sema)
        return "";

    auto hoverResult = resolveHoverTarget(*result, *result->sema, ctx, line, col);
    if (hoverResult.name.empty())
        return "";

    return formatHoverMarkdown(hoverResult);
}

/// @brief Enumerate global and type declarations owned by one document.
/// @param source Complete current document text.
/// @param path Document path or editor URI.
/// @return Document-owned symbols with declaration coordinates.
std::vector<SymbolInfo> CompilerBridge::symbols(const std::string &source,
                                                const std::string &path) {
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    if (!result || !result->sema)
        return {};

    std::vector<SymbolInfo> out;
    const uint32_t mainFileId = result->fileId;
    const auto typeLocations = indexZiaTypeDeclarationLocations(source, mainFileId);

    auto globals = result->sema->getGlobalSymbols();
    for (const auto &sym : globals) {
        if (!sym.decl || sym.decl->loc.file_id != mainFileId)
            continue;
        out.push_back({sym.name,
                       symbolKindStr(sym.kind),
                       sym.type ? sym.type->toDisplayString() : "unknown",
                       sym.isFinal,
                       sym.isExtern,
                       sym.decl->loc.line,
                       sym.decl->loc.column,
                       path});
    }

    auto types = result->sema->getTypeNames();
    for (const auto &tn : types) {
        auto it = typeLocations.find(tn);
        if (it == typeLocations.end() || it->second.file_id != mainFileId)
            continue;
        out.push_back({tn, "type", tn, false, false, it->second.line, it->second.column, path});
    }

    return out;
}

// --- Dump ---

/// @brief Compile a document and serialize its generated IL.
/// @param source Complete Zia source text.
/// @param path Logical source path used for diagnostics.
/// @param optimized Whether to request the O1 frontend optimization pipeline.
/// @return Serialized IL, or a human-readable compilation-failure summary.
std::string CompilerBridge::dumpIL(const std::string &source,
                                   const std::string &path,
                                   bool optimized) {
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};
    if (optimized)
        opts.optLevel = OptLevel::O1;

    auto result = il::frontends::zia::compile(input, opts, sm);
    if (!result.succeeded()) {
        std::string err = "Compilation failed:\n";
        for (const auto &d : result.diagnostics.diagnostics())
            err += "  " + d.message + "\n";
        return err;
    }

    return il::io::Serializer::toString(result.module);
}

/// @brief Parse and semantically analyze a document, then dump its AST.
/// @param source Complete Zia source text.
/// @param path Logical source path used for locations.
/// @return Printer-formatted AST or a no-AST sentinel string.
std::string CompilerBridge::dumpAst(const std::string &source, const std::string &path) {
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    if (!result || !result->ast)
        return "(no AST produced)";

    ZiaAstPrinter printer;
    return printer.dump(*result->ast);
}

/// @brief Lex a document into a line-oriented escaped token dump.
/// @param source Complete Zia source text.
/// @param path Logical source path used for token locations.
/// @return One line per non-EOF token with location, kind, and optional spelling.
std::string CompilerBridge::dumpTokens(const std::string &source, const std::string &path) {
    il::support::DiagnosticEngine diag;
    il::support::SourceManager sm;
    uint32_t fileId = sm.addFile(path);
    Lexer lexer(source, fileId, diag);

    std::string out;
    while (true) {
        Token tok = lexer.next();
        if (tok.kind == TokenKind::Eof)
            break;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u:%u", tok.loc.line, tok.loc.column);
        out += buf;
        out += '\t';
        out += tokenKindToString(tok.kind);
        if (!tok.text.empty()) {
            out += '\t';
            appendEscapedTokenText(out, tok.text);
        }
        out += '\n';
    }
    return out;
}

} // namespace zanna::server
