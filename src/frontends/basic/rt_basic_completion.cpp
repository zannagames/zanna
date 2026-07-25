//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/rt_basic_completion.cpp
// Purpose: Exposes BASIC diagnostics, completion, symbols, and hover results
//          through the runtime C ABI used by editor tooling.
// Key invariants:
//   - Returned maps/sequences use the same schema as the Zia bridge.
//   - Runtime objects inserted into owned collections release local references.
//   - The shared completion engine is serialized by s_engineMutex.
// Ownership/Lifetime:
//   - C ABI results transfer runtime ownership to the caller.
//   - Source and file-path handles are borrowed for each call.
// Links: docs/adr/0014-basic-language-service-runtime-bridge.md,
//        src/frontends/zia/rt_zia_completion.cpp
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief extern "C" bridge exposing the Zanna BASIC IDE engines (diagnostics,
///        completion, …) to the runtime as `Zanna.Basic.*`, mirroring the Zia
///        bridge (src/frontends/zia/rt_zia_completion.cpp).
///
/// Lives in fe_basic so it can call parseAndAnalyzeBasic / BasicCompletionEngine
/// without putting editor entry points in the runtime. The rt_string / rt_map /
/// rt_seq symbols are declared here but implemented in zanna_runtime; they
/// resolve at final link when the binary links both fe_basic and zanna_runtime.
/// Weak stubs in src/runtime/core/rt_basic_completion_stub.c cover binaries that
/// omit fe_basic. Result shapes are identical to the Zia bridge so the IDE
/// controllers consume both. See docs/adr/0014-basic-language-service-runtime-bridge.md.
///
//===----------------------------------------------------------------------===//

#include "frontends/basic/BasicAnalysis.hpp"
#include "frontends/basic/BasicCompletion.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/ast/DeclNodes.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include "runtime/collections/rt_map.h"
#include "runtime/collections/rt_seq.h"
#include "runtime/core/rt_string.h"
#include "runtime/oop/rt_object.h"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"

#include <cctype>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace il::frontends::basic;

namespace {

// One singleton completion engine per process; it keeps a one-entry parse cache
// keyed by source hash, so consecutive keystrokes on one file do not re-parse.
BasicCompletionEngine s_engine;
std::mutex s_engineMutex;

// ── Runtime object/string builders (mirror the Zia bridge helpers) ───────────

/// @brief Copies a nullable runtime string into a C++ string.
/// @param value Borrowed runtime string handle; may be null.
/// @return Byte-preserving owned string, empty for a null handle.
std::string toStdString(rt_string value) {
    const char *cstr = value ? rt_string_cstr(value) : "";
    size_t len = value ? (size_t)rt_str_len(value) : 0;
    return std::string(cstr ? cstr : "", len);
}

/// @brief Resolves the editor path used for source-manager diagnostics.
/// @param filePath Borrowed runtime path string; may be null or empty.
/// @return Supplied path, or `<editor>` when no path is available.
std::string editorPathOrDefault(rt_string filePath) {
    std::string path = toStdString(filePath);
    return path.empty() ? std::string("<editor>") : path;
}

/// @brief Allocates a runtime string from an arbitrary byte view.
/// @param text Bytes copied into runtime-managed storage.
/// @return Owned runtime string handle.
rt_string toRtString(std::string_view text) {
    const char *data = text.empty() ? "" : text.data();
    return rt_string_from_bytes(data, text.size());
}

/// @brief Releases a nullable runtime object and frees its final reference.
/// @param obj Borrowed local reference to release; may be null.
void releaseRuntimeObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Stores an object value in a runtime map.
/// @param map Borrowed destination map.
/// @param keyName Null-terminated key copied into a temporary runtime string.
/// @param value Runtime object value stored by the map.
void mapSetObject(void *map, const char *keyName, void *value) {
    rt_string key = toRtString(keyName);
    rt_map_set(map, key, value);
    rt_string_unref(key);
}

/// @brief Stores a signed integer value in a runtime map.
/// @param map Borrowed destination map.
/// @param keyName Null-terminated key.
/// @param value Integer payload.
void mapSetInt(void *map, const char *keyName, int64_t value) {
    rt_string key = toRtString(keyName);
    rt_map_set_int(map, key, value);
    rt_string_unref(key);
}

/// @brief Stores a boolean value in a runtime map.
/// @param map Borrowed destination map.
/// @param keyName Null-terminated key.
/// @param value Boolean payload normalized to the runtime's integer form.
void mapSetBool(void *map, const char *keyName, bool value) {
    rt_string key = toRtString(keyName);
    rt_map_set_bool(map, key, value ? 1 : 0);
    rt_string_unref(key);
}

/// @brief Stores a copied string value in a runtime map.
/// @param map Borrowed destination map.
/// @param keyName Null-terminated key.
/// @param value Text copied into a temporary runtime string.
void mapSetStr(void *map, const char *keyName, std::string_view value) {
    rt_string key = toRtString(keyName);
    rt_string text = toRtString(value);
    rt_map_set_str(map, key, text);
    rt_string_unref(text);
    rt_string_unref(key);
}

// ── Diagnostics → Seq<Map> (frontend-agnostic: il::support types) ────────────

/// @brief Maps diagnostic severity to the editor bridge's numeric code.
/// @param severity Frontend diagnostic severity.
/// @return Zero for error, one for warning, or two for note.
int diagnosticSeverityCode(il::support::Severity severity) {
    switch (severity) {
        case il::support::Severity::Warning:
            return 1;
        case il::support::Severity::Note:
            return 2;
        case il::support::Severity::Error:
        default:
            return 0;
    }
}

/// @brief Maps diagnostic severity to its stable editor-facing spelling.
/// @param severity Frontend diagnostic severity.
/// @return Process-lifetime lowercase severity name.
std::string_view diagnosticSeverityName(il::support::Severity severity) {
    switch (severity) {
        case il::support::Severity::Warning:
            return "warning";
        case il::support::Severity::Note:
            return "note";
        case il::support::Severity::Error:
        default:
            return "error";
    }
}

/// @brief Resolves a source location to a display path.
/// @param loc Location whose file identifier is queried.
/// @param sm Source manager owning registered file paths.
/// @param fallbackPath Path returned for unknown or pathless file identifiers.
/// @return Owned resolved or fallback path.
std::string pathForLocation(const il::support::SourceLoc &loc,
                            const il::support::SourceManager &sm,
                            const std::string &fallbackPath) {
    if (loc.file_id != 0) {
        std::string_view path = sm.getPath(loc.file_id);
        if (!path.empty())
            return std::string(path);
    }
    return fallbackPath;
}

/// @brief Converts diagnostic fix-its to an owned runtime sequence of maps.
/// @param diagnostic Diagnostic whose ordered fix-its are serialized.
/// @return Owned runtime sequence; each element contains message, replacement,
///         and source range fields.
void *fixitsToSeq(const il::support::Diagnostic &diagnostic) {
    void *seq = rt_seq_new_owned();
    for (const auto &fixit : diagnostic.fixits) {
        void *map = rt_map_new();
        mapSetStr(map, "message", fixit.message);
        mapSetStr(map, "replacement", fixit.replacement);
        mapSetInt(map, "startLine", fixit.range.begin.line);
        mapSetInt(map, "startColumn", fixit.range.begin.column);
        mapSetInt(map, "endLine", fixit.range.end.line);
        mapSetInt(map, "endColumn", fixit.range.end.column);
        rt_seq_push(seq, map);
        releaseRuntimeObject(map);
    }
    return seq;
}

/// @brief Converts one frontend diagnostic to the editor map schema.
/// @param diagnostic Diagnostic to serialize.
/// @param sm Source manager used to resolve file identifiers.
/// @param fallbackPath Path used when the diagnostic location has none.
/// @return Owned runtime map including range, severity, message, and fix-its.
void *diagnosticToMap(const il::support::Diagnostic &diagnostic,
                      const il::support::SourceManager &sm,
                      const std::string &fallbackPath) {
    il::support::SourceLoc start = diagnostic.loc;
    il::support::SourceLoc end = diagnostic.loc;
    if (diagnostic.range.isValid()) {
        if (!start.isValid())
            start = diagnostic.range.begin;
        end = diagnostic.range.end;
    }
    if (!end.isValid())
        end = start;

    void *map = rt_map_new();
    mapSetStr(map, "file", pathForLocation(start, sm, fallbackPath));
    mapSetInt(map, "line", start.line);
    mapSetInt(map, "column", start.column);
    mapSetInt(map, "endLine", end.line);
    mapSetInt(map, "endColumn", end.column);
    mapSetInt(map, "severity", diagnosticSeverityCode(diagnostic.severity));
    mapSetStr(map, "severityName", diagnosticSeverityName(diagnostic.severity));
    mapSetStr(map, "code", diagnostic.code);
    mapSetStr(map, "message", diagnostic.message);
    mapSetStr(map, "stage", diagnostic.stage);
    mapSetStr(map, "help", diagnostic.help);
    const bool hasFixit = !diagnostic.fixits.empty();
    mapSetBool(map, "hasFixit", hasFixit);
    void *fixits = fixitsToSeq(diagnostic);
    mapSetObject(map, "fixits", fixits);
    releaseRuntimeObject(fixits);
    return map;
}

/// @brief Converts a diagnostic engine's records to an owned runtime sequence.
/// @param diagnostics Diagnostic collection in emission order.
/// @param sm Source manager used to resolve locations.
/// @param fallbackPath Path used for diagnostics without registered files.
/// @return Owned runtime sequence of diagnostic maps.
void *diagnosticsToSeq(const il::support::DiagnosticEngine &diagnostics,
                       const il::support::SourceManager &sm,
                       const std::string &fallbackPath) {
    void *seq = rt_seq_new_owned();
    for (const auto &diagnostic : diagnostics.diagnostics()) {
        void *map = diagnosticToMap(diagnostic, sm, fallbackPath);
        rt_seq_push(seq, map);
        releaseRuntimeObject(map);
    }
    return seq;
}

// ── Completion items → Seq<Map> ──────────────────────────────────────────────

/// @brief Converts a completion kind to its editor-facing name.
/// @param kind Completion category produced by the BASIC engine.
/// @return Process-lifetime camel-case or lowercase kind name.
std::string_view completionKindName(CompletionKind kind) {
    switch (kind) {
        case CompletionKind::Keyword:
            return "keyword";
        case CompletionKind::Snippet:
            return "snippet";
        case CompletionKind::Variable:
            return "variable";
        case CompletionKind::Parameter:
            return "parameter";
        case CompletionKind::Field:
            return "field";
        case CompletionKind::Method:
            return "method";
        case CompletionKind::Function:
            return "function";
        case CompletionKind::Entity:
            return "entity";
        case CompletionKind::Value:
            return "value";
        case CompletionKind::Interface:
            return "interface";
        case CompletionKind::Module:
            return "module";
        case CompletionKind::RuntimeClass:
            return "runtimeClass";
        case CompletionKind::Property:
            return "property";
    }
    return "item";
}

/// @brief Serializes one completion item to the shared runtime map schema.
/// @param item Completion candidate to serialize.
/// @return Owned runtime map containing label, insertion, kind, detail, and
///         documentation fields.
void *completionItemToMap(const CompletionItem &item) {
    void *map = rt_map_new();
    mapSetStr(map, "label", item.label);
    mapSetStr(map, "insertText", item.insertText);
    mapSetInt(map, "kind", static_cast<int64_t>(item.kind));
    mapSetStr(map, "kindName", completionKindName(item.kind));
    mapSetStr(map, "detail", item.detail);
    mapSetStr(map, "documentation", item.documentation);
    mapSetStr(map, "source", "basic");
    mapSetStr(map, "commitCharacters", "");
    // Omit replacement*/cursorOffset: the IDE defaults them to the cursor
    // position (Map.GetIntOr) so the typed prefix is replaced correctly.
    return map;
}

/// @brief Serializes completion candidates to an owned runtime sequence.
/// @param items Candidates in engine ranking order.
/// @return Owned runtime sequence of completion maps.
void *completionItemsToSeq(const std::vector<CompletionItem> &items) {
    void *seq = rt_seq_new_owned();
    for (const auto &item : items) {
        void *map = completionItemToMap(item);
        rt_seq_push(seq, map);
        releaseRuntimeObject(map);
    }
    return seq;
}

// ── Document symbols → tab-delimited string (name\tkind\ttype\tline) ──────────

/// @brief Appends one non-empty document symbol record.
/// @param out Destination tab-delimited text stream.
/// @param name Symbol display name; empty names are skipped.
/// @param kind Null-terminated editor symbol kind.
/// @param line One-based source line.
void emitSymbol(std::ostringstream &out, const std::string &name, const char *kind, uint32_t line) {
    if (!name.empty())
        out << name << '\t' << kind << "\t\t" << line << '\n';
}

/// @brief Builds the BASIC bridge's tab-delimited document-symbol payload.
/// @param prog Parsed program whose procedures, classes, and types are listed.
/// @return Owned text with one symbol record per line.
std::string basicSymbolsString(const Program &prog) {
    std::ostringstream out;
    for (const auto &p : prog.procs) {
        if (!p)
            continue;
        switch (p->stmtKind()) {
            case Stmt::Kind::FunctionDecl: {
                const auto &fn = static_cast<const FunctionDecl &>(*p);
                emitSymbol(out, fn.name, "function", fn.loc.line);
                break;
            }
            case Stmt::Kind::SubDecl: {
                const auto &sub = static_cast<const SubDecl &>(*p);
                emitSymbol(out, sub.name, "function", sub.loc.line);
                break;
            }
            default:
                break;
        }
    }
    for (const auto &s : prog.main) {
        if (!s)
            continue;
        switch (s->stmtKind()) {
            case Stmt::Kind::ClassDecl: {
                const auto &c = static_cast<const ClassDecl &>(*s);
                emitSymbol(out, c.name, "type", c.loc.line);
                break;
            }
            case Stmt::Kind::TypeDecl: {
                const auto &t = static_cast<const TypeDecl &>(*s);
                emitSymbol(out, t.name, "type", t.loc.line);
                break;
            }
            default:
                break;
        }
    }
    return out.str();
}

// ── Hover: identifier at cursor → type lookup ────────────────────────────────

/// @brief Formats a semantic analyzer type using BASIC source terminology.
/// @param t Semantic type to display.
/// @return BASIC type name, or an empty string for unknown/unsupported types.
std::string semaTypeDisplay(SemanticAnalyzer::Type t) {
    switch (t) {
        case SemanticAnalyzer::Type::Int:
            return "INTEGER";
        case SemanticAnalyzer::Type::Float:
            return "DOUBLE";
        case SemanticAnalyzer::Type::String:
            return "STRING";
        case SemanticAnalyzer::Type::Bool:
            return "BOOLEAN";
        case SemanticAnalyzer::Type::ArrayInt:
            return "INTEGER()";
        case SemanticAnalyzer::Type::ArrayString:
            return "STRING()";
        case SemanticAnalyzer::Type::ArrayObject:
            return "object()";
        case SemanticAnalyzer::Type::Object:
            return "object";
        default:
            return "";
    }
}

/// @brief Display name for an AST declared type (DIM/param/return).
/// @param t AST type to display.
/// @return BASIC source type name.
std::string astTypeDisplay(Type t) {
    switch (t) {
        case Type::I64:
            return "INTEGER";
        case Type::F64:
            return "DOUBLE";
        case Type::Str:
            return "STRING";
        case Type::Bool:
            return "BOOLEAN";
    }
    return "";
}

/// @brief Formats the declared scalar, array, or object type of a DIM node.
/// @param d Declaration whose explicit class name, array flag, and type are read.
/// @return BASIC type display string.
std::string dimTypeDisplay(const DimStmt &d) {
    if (!d.explicitClassQname.empty()) {
        std::string q;
        for (const auto &seg : d.explicitClassQname) {
            if (!q.empty())
                q += ".";
            q += seg;
        }
        return d.isArray ? q + "()" : q;
    }
    std::string base = astTypeDisplay(d.type);
    return (d.isArray && !base.empty()) ? base + "()" : base;
}

/// @brief Finds a DIM declaration in one statement list.
/// @param stmts Statements searched in order.
/// @param canon Canonical identifier to match.
/// @return Declaration type display, or an empty string when absent.
std::string dimTypeInStmts(const std::vector<StmtPtr> &stmts, const std::string &canon) {
    for (const auto &s : stmts) {
        if (s && s->stmtKind() == Stmt::Kind::Dim) {
            const auto &d = static_cast<const DimStmt &>(*s);
            if (CanonicalizeIdent(d.name) == canon)
                return dimTypeDisplay(d);
        }
    }
    return "";
}

/// @brief Resolve a hover type for @p ident from the AST when the analyzer's
///        inferred-type table does not track it (e.g. INTEGER, BASIC's default).
///        Covers top-level + proc-local DIMs, parameters, and procedure names.
/// @param prog Parsed program containing declarations to search.
/// @param ident Source identifier at the hover position.
/// @return Display type or procedure signature label; empty when unresolved.
std::string lookupDeclType(const Program &prog, const std::string &ident) {
    std::string canon = CanonicalizeIdent(ident);
    if (canon.empty())
        return "";
    std::string t = dimTypeInStmts(prog.main, canon);
    if (!t.empty())
        return t;
    for (const auto &p : prog.procs) {
        if (!p)
            continue;
        if (p->stmtKind() == Stmt::Kind::FunctionDecl) {
            const auto &fn = static_cast<const FunctionDecl &>(*p);
            if (CanonicalizeIdent(fn.name) == canon)
                return "FUNCTION -> " + astTypeDisplay(fn.ret);
            for (const auto &prm : fn.params)
                if (CanonicalizeIdent(prm.name) == canon)
                    return astTypeDisplay(prm.type);
            t = dimTypeInStmts(fn.body, canon);
            if (!t.empty())
                return t;
        } else if (p->stmtKind() == Stmt::Kind::SubDecl) {
            const auto &sd = static_cast<const SubDecl &>(*p);
            if (CanonicalizeIdent(sd.name) == canon)
                return "SUB";
            for (const auto &prm : sd.params)
                if (CanonicalizeIdent(prm.name) == canon)
                    return astTypeDisplay(prm.type);
            t = dimTypeInStmts(sd.body, canon);
            if (!t.empty())
                return t;
        }
    }
    return "";
}

/// @brief Tests whether a byte can participate in a BASIC identifier.
/// @param c Candidate byte interpreted as unsigned for character classification.
/// @return True for alphanumerics, underscore, or BASIC type suffixes.
bool isBasicIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '%' ||
           c == '!' || c == '#';
}

/// @brief Extract the identifier spanning (1-based @p line, 0-based @p col).
/// @param source Complete editor buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column, which may sit just after the word.
/// @return Identifier under or immediately before the cursor; empty if none.
std::string identifierAt(const std::string &source, int line, int col) {
    if (line < 1 || col < 0)
        return "";
    size_t pos = 0, cur = 1;
    while (pos < source.size() && cur < static_cast<size_t>(line)) {
        if (source[pos] == '\n')
            ++cur;
        ++pos;
    }
    if (cur != static_cast<size_t>(line))
        return "";
    size_t lineEnd = source.find('\n', pos);
    if (lineEnd == std::string::npos)
        lineEnd = source.size();
    std::string ln = source.substr(pos, lineEnd - pos);
    size_t at = static_cast<size_t>(col);
    // Cursor sitting just after a word (common) — step back onto it.
    if ((at >= ln.size() || !isBasicIdentChar(ln[at])) && at > 0 && isBasicIdentChar(ln[at - 1]))
        --at;
    if (at >= ln.size() || !isBasicIdentChar(ln[at]))
        return "";
    size_t b = at, e = at;
    while (b > 0 && isBasicIdentChar(ln[b - 1]))
        --b;
    while (e + 1 < ln.size() && isBasicIdentChar(ln[e + 1]))
        ++e;
    return ln.substr(b, e - b + 1);
}

/// @brief Retrieves summary and detail documentation for a runtime class.
/// @param qname Canonical runtime class qualified name.
/// @return Combined documentation, or an empty string when the class is absent.
std::string runtimeClassDocumentation(std::string_view qname) {
    const auto *runtimeClass = il::runtime::findRuntimeClassByQName(qname);
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

/// @brief Builds the shared hover-result runtime map.
/// @param name Identifier display name.
/// @param typeStr Resolved type display.
/// @param documentation Optional documentation text.
/// @return Owned runtime map whose available flag is true only when both name
///         and type are non-empty.
void *basicHoverMap(const std::string &name,
                    const std::string &typeStr,
                    const std::string &documentation = {}) {
    void *map = rt_map_new();
    const bool available = !name.empty() && !typeStr.empty();
    mapSetBool(map, "available", available);
    if (available) {
        mapSetStr(map, "title", name);
        mapSetStr(map, "type", typeStr);
        mapSetStr(map, "display", name + " : " + typeStr);
        mapSetStr(map, "source", "basic");
        mapSetStr(map, "documentation", documentation);
    }
    return map;
}

} // namespace

extern "C" {

/// @brief Parses and semantically checks a BASIC editor buffer.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed runtime string used for diagnostic paths.
/// @return Owned runtime sequence of diagnostic maps; empty on bridge failure.
void *rt_basic_toolchain_check_for_file(rt_string source, rt_string file_path) {
    try {
        std::string sourceStr = toStdString(source);
        std::string pathStr = editorPathOrDefault(file_path);
        il::support::SourceManager sm;
        BasicCompilerInput input{.source = sourceStr, .path = pathStr};
        auto result = parseAndAnalyzeBasic(input, sm);
        if (!result)
            return rt_seq_new_owned();
        std::string_view resolved = sm.getPath(result->fileId);
        return diagnosticsToSeq(
            result->diagnostics, sm, resolved.empty() ? pathStr : std::string(resolved));
    } catch (...) {
        return rt_seq_new_owned();
    }
}

/// @brief Computes BASIC completion candidates at an editor position.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed runtime string used as the virtual file path.
/// @param line Editor line passed through to BasicCompletionEngine.
/// @param col Editor column passed through to BasicCompletionEngine.
/// @return Owned runtime sequence of completion maps; empty on failure.
void *rt_basic_completion_items_for_file(rt_string source,
                                         rt_string file_path,
                                         int64_t line,
                                         int64_t col) {
    try {
        std::string sourceStr = toStdString(source);
        std::string pathStr = editorPathOrDefault(file_path);
        std::vector<CompletionItem> items;
        {
            std::lock_guard<std::mutex> lock(s_engineMutex);
            items = s_engine.complete(sourceStr, (int)line, (int)col, pathStr);
        }
        return completionItemsToSeq(items);
    } catch (...) {
        return rt_seq_new_owned();
    }
}

/// @brief Extracts document symbols from a BASIC editor buffer.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed runtime string used as the virtual file path.
/// @return Owned runtime string containing tab-delimited symbol records; empty
///         when parsing or bridge execution fails.
rt_string rt_basic_completion_symbols_for_file(rt_string source, rt_string file_path) {
    try {
        std::string sourceStr = toStdString(source);
        std::string pathStr = editorPathOrDefault(file_path);
        il::support::SourceManager sm;
        BasicCompilerInput input{.source = sourceStr, .path = pathStr};
        auto result = parseAndAnalyzeBasic(input, sm);
        if (!result || !result->ast)
            return rt_string_from_bytes("", 0);
        std::string text = basicSymbolsString(*result->ast);
        return rt_string_from_bytes(text.data(), text.size());
    } catch (...) {
        return rt_string_from_bytes("", 0);
    }
}

/// @brief Resolves hover information for the BASIC identifier at a cursor.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed runtime string used as the virtual file path.
/// @param line One-based cursor line.
/// @param col Zero-based cursor column.
/// @return Owned runtime hover map; unavailable on unresolved input or failure.
void *rt_basic_completion_hover_info_for_file(rt_string source,
                                              rt_string file_path,
                                              int64_t line,
                                              int64_t col) {
    try {
        std::string sourceStr = toStdString(source);
        std::string ident = identifierAt(sourceStr, (int)line, (int)col);
        if (ident.empty())
            return basicHoverMap("", "");
        std::string pathStr = editorPathOrDefault(file_path);
        il::support::SourceManager sm;
        BasicCompilerInput input{.source = sourceStr, .path = pathStr};
        auto result = parseAndAnalyzeBasic(input, sm);
        if (!result || !result->sema)
            return basicHoverMap("", "");
        std::string disp;
        auto t = result->sema->lookupVarType(ident);
        if (t.has_value()) {
            disp = semaTypeDisplay(*t);
            if (*t == SemanticAnalyzer::Type::Object) {
                auto cls = result->sema->lookupObjectClassQName(ident);
                if (cls.has_value() && !cls->empty())
                    disp = *cls;
            }
        }
        // The analyzer only tracks inferred non-default types; fall back to the
        // AST declaration so INTEGER (BASIC's default) and explicit DIM/param/
        // proc types still resolve.
        if (disp.empty())
            disp = lookupDeclType(*result->ast, ident);
        return basicHoverMap(ident, disp, runtimeClassDocumentation(disp));
    } catch (...) {
        return basicHoverMap("", "");
    }
}

} // extern "C"
