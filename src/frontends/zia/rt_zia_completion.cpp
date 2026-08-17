//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/rt_zia_completion.cpp
// Purpose: Bridge Zia editor services to the runtime C ABI and maintain
//          validated incremental document mirrors.
// Key invariants:
//   - Runtime handles are borrowed unless the function documents an owned result.
//   - Mirror delta batches are validated atomically before becoming visible.
//   - Shared editor state is accessed only while holding its corresponding mutex.
// Ownership/Lifetime:
//   - Process-wide editor caches own their copied strings and analysis jobs.
//   - Returned runtime objects follow the runtime reference-counting contract.
// Links: src/frontends/zia/ZiaCompletion.hpp, docs/tools/zia-server.md
//
//===----------------------------------------------------------------------===//
///
/// @file rt_zia_completion.cpp
/// @brief extern "C" bridge between the Zia CompletionEngine and the Zanna
///        runtime string API (rt_string).
///
/// Lives in zia_editor_services so it has access to ZiaCompletion.hpp without
/// placing editor entry points in the core compiler frontend archive. The
/// rt_string functions (rt_string_cstr, rt_str_len, rt_string_from_bytes) are
/// declared via rt_string.h but implemented in zanna_runtime; symbols resolve
/// at final link time when the executable links both zia_editor_services and
/// zanna_runtime.
///
//===----------------------------------------------------------------------===//

#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "frontends/common/CharUtils.hpp"
#include "frontends/zia/Lexer.hpp"
#include "frontends/zia/Token.hpp"
#include "frontends/zia/ZiaAnalysis.hpp"
#include "frontends/zia/ZiaCompletion.hpp"
#include "il/io/Serializer.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include "runtime/collections/rt_map.h"
#include "runtime/collections/rt_seq.h"
#include "runtime/core/rt_string.h"
#include "runtime/oop/rt_object.h"
#include "runtime/oop/rt_option.h"
#include "support/source_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace il::frontends::zia;

namespace {
// One singleton CompletionEngine per process. The engine maintains a single-
// entry parse cache keyed by source hash and path, so repeated calls for the same
// file content do not re-parse.
CompletionEngine s_engine;
std::mutex s_engineMutex;

constexpr int kMaxSemanticWorkerJobs = 2;
std::atomic<int> g_activeSemanticWorkerJobs{0};

/// @brief Copy a nullable runtime string into an owning standard string.
/// @param value Borrowed runtime string handle, or null for an empty string.
/// @return Byte-preserving standard string copy.
std::string toStdString(rt_string value) {
    const char *cstr = value ? rt_string_cstr(value) : "";
    size_t len = value ? (size_t)rt_str_len(value) : 0;
    return std::string(cstr ? cstr : "", len);
}

/// @brief Convert a runtime path and supply the editor-services sentinel when empty.
/// @param filePath Borrowed runtime string containing a source path.
/// @return The supplied path, or `<editor>` when the handle or text is empty.
std::string editorPathOrDefault(rt_string filePath) {
    std::string path = toStdString(filePath);
    return path.empty() ? std::string("<editor>") : path;
}

// ── Incremental document mirror (plan 08) ──────────────────────────────────
// A per-path text mirror the editor keeps in sync via edit deltas, so semantic
// queries read the mirror instead of receiving the full buffer on every call.
// The mirror text matches the editor's materialization (codeeditor_materialize_
// lines): lines joined by '\n' with no trailing newline; positions are 0-based
// byte line/column.

struct DocumentMirror {
    std::string text;
    uint64_t revision = 0;
};

std::mutex s_mirrorMutex;
std::unordered_map<std::string, DocumentMirror> s_mirrors;

constexpr size_t kMaxMirrorBytes = 64 * 1024 * 1024;
constexpr size_t kMaxDeltaJsonBytes = 16 * 1024 * 1024;
constexpr size_t kMaxDeltasPerBatch = 100000;

/// @brief Compute the clamped byte offset of a zero-based position.
/// @param s Newline-delimited document buffer.
/// @param line Zero-based target line.
/// @param col Zero-based byte column.
/// @return Offset at the requested position, clamped to the available line or buffer.
size_t mirrorOffsetOf(const std::string &s, int line, int col) {
    size_t o = 0;
    int l = 0;
    while (l < line && o < s.size()) {
        if (s[o] == '\n')
            l++;
        o++;
    }
    for (int c = 0; c < col && o < s.size() && s[o] != '\n'; c++)
        o++;
    return o;
}

/// @brief Strict variant of mirrorOffsetOf (VDOC-109): fails instead of
///        clamping when @p line is past the buffer or @p col is past the end
///        of the line, so corrupt journal coordinates surface as malformed
///        deltas and trigger the caller's full-sync fallback.
/// @param s Newline-delimited document buffer.
/// @param line Zero-based target line.
/// @param col Zero-based byte column.
/// @param out Receives the validated byte offset on success; must be non-null.
/// @return True when the position is valid and @p out was populated.
bool mirrorOffsetOfStrict(const std::string &s, int line, int col, size_t *out) {
    if (!out || line < 0 || col < 0)
        return false;
    size_t o = 0;
    int l = 0;
    while (l < line) {
        if (o >= s.size())
            return false; // line beyond buffer
        if (s[o] == '\n')
            l++;
        o++;
    }
    for (int c = 0; c < col; c++) {
        if (o >= s.size() || s[o] == '\n')
            return false; // column beyond end of line
        o++;
    }
    *out = o;
    return true;
}

/// @brief Apply the compact delta JSON emitted by
///        vg_codeeditor_take_deltas_json to @p text in order.
/// @details Schema: `[{"r":N,"sl":N,"sc":N,"el":N,"ec":N,"t":"<escaped>"},...]`
///          — each delta replaced the pre-edit byte range [(sl,sc)..(el,ec)) with
///          t. Returns false on any malformed input (caller then full-syncs).
/// @param text Mutable mirror text updated in journal order.
/// @param json Compact delta array to parse.
/// @param base_revision Revision currently represented by @p text.
/// @param end_revision Maximum accepted revision for this journal batch.
/// @return True when every delta is structurally valid and applied.
bool mirrorApplyDeltasJson(std::string &text,
                           const std::string &json,
                           uint64_t base_revision,
                           uint64_t end_revision) {
    if (json.size() > kMaxDeltaJsonBytes || text.size() > kMaxMirrorBytes)
        return false;

    std::string updatedText = text;
    size_t i = 0;
    /// @brief Advances the parser cursor across JSON whitespace.
    auto skipWs = [&]() {
        while (i < json.size() &&
               (json[i] == ' ' || json[i] == '\n' || json[i] == '\t' || json[i] == '\r'))
            i++;
    };
    skipWs();
    if (i >= json.size() || json[i] != '[')
        return false;
    i++;
    skipWs();
    if (i < json.size() && json[i] == ']') {
        i++;
        skipWs();
        return i == json.size() && base_revision == end_revision;
    }
    size_t deltaCount = 0;
    while (i < json.size()) {
        if (++deltaCount > kMaxDeltasPerBatch)
            return false;
        skipWs();
        if (i >= json.size() || json[i] != '{')
            return false;
        i++;
        int sl = 0, sc = 0, el = 0, ec = 0;
        uint64_t rev = 0;
        bool haveSl = false, haveSc = false, haveEl = false, haveEc = false, haveRev = false,
             haveText = false;
        std::string t;
        bool firstField = true;
        while (true) {
            skipWs();
            if (i >= json.size())
                return false;
            if (json[i] == '}') {
                if (firstField)
                    return false;
                i++;
                break;
            }
            if (!firstField) {
                if (json[i] != ',')
                    return false;
                i++;
                skipWs();
                if (i >= json.size() || json[i] == '}')
                    return false;
            }
            if (json[i] != '"')
                return false;
            i++;
            std::string key;
            while (i < json.size() && json[i] != '"') {
                if (json[i] == '\\' || static_cast<unsigned char>(json[i]) < 0x20)
                    return false;
                key += json[i];
                i++;
            }
            if (i >= json.size())
                return false;
            i++; // closing quote of key
            skipWs();
            if (i >= json.size() || json[i] != ':')
                return false;
            i++;
            skipWs();
            if (key == "t") {
                if (haveText)
                    return false;
                haveText = true;
                if (i >= json.size() || json[i] != '"')
                    return false;
                i++;
                while (i < json.size() && json[i] != '"') {
                    if (json[i] == '\\') {
                        i++;
                        if (i >= json.size())
                            return false;
                        char e = json[i];
                        if (e == 'n')
                            t += '\n';
                        else if (e == 't')
                            t += '\t';
                        else if (e == 'r')
                            t += '\r';
                        else if (e == '"')
                            t += '"';
                        else if (e == '\\')
                            t += '\\';
                        else if (e == '/')
                            t += '/';
                        else if (e == 'b')
                            t += '\b';
                        else if (e == 'f')
                            t += '\f';
                        else if (e == 'u') {
                            if (i + 4 >= json.size())
                                return false;
                            uint32_t cp = 0;
                            for (int k = 1; k <= 4; k++) {
                                char h = json[i + k];
                                cp <<= 4;
                                if (h >= '0' && h <= '9')
                                    cp += h - '0';
                                else if (h >= 'a' && h <= 'f')
                                    cp += h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F')
                                    cp += h - 'A' + 10;
                                else
                                    return false;
                            }
                            i += 4;
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                if (i + 6 >= json.size() || json[i + 1] != '\\' ||
                                    json[i + 2] != 'u')
                                    return false;
                                uint32_t low = 0;
                                for (int k = 3; k <= 6; ++k) {
                                    const char h = json[i + k];
                                    low <<= 4;
                                    if (h >= '0' && h <= '9')
                                        low += static_cast<uint32_t>(h - '0');
                                    else if (h >= 'a' && h <= 'f')
                                        low += static_cast<uint32_t>(h - 'a' + 10);
                                    else if (h >= 'A' && h <= 'F')
                                        low += static_cast<uint32_t>(h - 'A' + 10);
                                    else
                                        return false;
                                }
                                if (low < 0xDC00 || low > 0xDFFF)
                                    return false;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                i += 6;
                            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                                return false;
                            }
                            if (cp <= 0x7F) {
                                t.push_back(static_cast<char>(cp));
                            } else if (cp <= 0x7FF) {
                                t.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                t.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else if (cp <= 0xFFFF) {
                                t.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                t.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                t.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else {
                                t.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                                t.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                                t.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                t.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                        } else {
                            return false;
                        }
                        i++;
                    } else {
                        if (static_cast<unsigned char>(json[i]) < 0x20)
                            return false;
                        t += json[i];
                        i++;
                    }
                    if (t.size() > kMaxMirrorBytes)
                        return false;
                }
                if (i >= json.size())
                    return false;
                i++; // closing quote of value
            } else {
                if (key != "sl" && key != "sc" && key != "el" && key != "ec" && key != "r")
                    return false;
                uint64_t v = 0;
                bool any = false;
                while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
                    const uint32_t digit = static_cast<uint32_t>(json[i] - '0');
                    if (v > (std::numeric_limits<uint64_t>::max() - digit) / 10)
                        return false;
                    v = v * 10 + digit;
                    i++;
                    any = true;
                }
                if (!any)
                    return false;
                if (key == "sl") {
                    if (haveSl || v > static_cast<uint64_t>(std::numeric_limits<int>::max()))
                        return false;
                    sl = static_cast<int>(v);
                    haveSl = true;
                } else if (key == "sc") {
                    if (haveSc || v > static_cast<uint64_t>(std::numeric_limits<int>::max()))
                        return false;
                    sc = static_cast<int>(v);
                    haveSc = true;
                } else if (key == "el") {
                    if (haveEl || v > static_cast<uint64_t>(std::numeric_limits<int>::max()))
                        return false;
                    el = static_cast<int>(v);
                    haveEl = true;
                } else if (key == "ec") {
                    if (haveEc || v > static_cast<uint64_t>(std::numeric_limits<int>::max()))
                        return false;
                    ec = static_cast<int>(v);
                    haveEc = true;
                } else if (key == "r") {
                    if (haveRev)
                        return false;
                    rev = v;
                    haveRev = true;
                }
            }
            firstField = false;
        }
        if (!haveSl || !haveSc || !haveEl || !haveEc || !haveRev || !haveText)
            return false;
        // Revision enforcement (VDOC-109): each delta must advance strictly
        // past the mirror's stored revision and stay within end_revision, so
        // stale or out-of-order journal entries are rejected.
        if (rev <= base_revision || rev > end_revision)
            return false;
        base_revision = rev;
        size_t so = 0;
        size_t eo = 0;
        if (!mirrorOffsetOfStrict(updatedText, sl, sc, &so) ||
            !mirrorOffsetOfStrict(updatedText, el, ec, &eo))
            return false;
        if (so > eo)
            return false; // reversed range is corrupt, not an insertion
        if (t.size() > kMaxMirrorBytes - (updatedText.size() - (eo - so)))
            return false;
        updatedText.replace(so, eo - so, t);

        skipWs();
        if (i < json.size() && json[i] == ',') {
            i++;
            skipWs();
            if (i >= json.size() || json[i] == ']')
                return false;
            continue;
        }
        if (i < json.size() && json[i] == ']') {
            i++;
            skipWs();
            if (i != json.size() || base_revision != end_revision)
                return false;
            text = std::move(updatedText);
            return true;
        }
        return false;
    }
    return false;
}

/// @brief Copy the mirror text for a normalized path.
/// @param path Mirror lookup key.
/// @param out Receives the copied document text when present.
/// @return True when the mirror contains @p path; otherwise false.
bool mirrorTextFor(const std::string &path, std::string &out) {
    std::lock_guard<std::mutex> lock(s_mirrorMutex);
    auto it = s_mirrors.find(path);
    if (it == s_mirrors.end())
        return false;
    out = it->second.text;
    return true;
}

/// @brief Allocate a runtime string from an arbitrary byte view.
/// @param text Bytes to copy into runtime-managed storage.
/// @return Owned runtime string handle.
rt_string toRtString(std::string_view text) {
    const char *data = text.empty() ? "" : text.data();
    return rt_string_from_bytes(data, text.size());
}

/// @brief Release an owned runtime object when its reference count reaches zero.
/// @param obj Runtime object returned by a bridge helper; null is accepted.
void releaseRuntimeObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Store a runtime object in a runtime map under a temporary string key.
/// @param map Mutable runtime map.
/// @param keyName Null-terminated field name.
/// @param value Runtime object value; ownership follows `rt_map_set`.
void mapSetObject(void *map, const char *keyName, void *value) {
    rt_string key = toRtString(keyName);
    rt_map_set(map, key, value);
    rt_string_unref(key);
}

/// @brief Store a signed integer field in a runtime map.
/// @param map Mutable runtime map.
/// @param keyName Null-terminated field name.
/// @param value Integer value to store.
void mapSetInt(void *map, const char *keyName, int64_t value) {
    rt_string key = toRtString(keyName);
    rt_map_set_int(map, key, value);
    rt_string_unref(key);
}

/// @brief Store a boolean field in a runtime map.
/// @param map Mutable runtime map.
/// @param keyName Null-terminated field name.
/// @param value Boolean value to store.
void mapSetBool(void *map, const char *keyName, bool value) {
    rt_string key = toRtString(keyName);
    rt_map_set_bool(map, key, value ? 1 : 0);
    rt_string_unref(key);
}

/// @brief Store a copied runtime string field in a runtime map.
/// @param map Mutable runtime map.
/// @param keyName Null-terminated field name.
/// @param value Bytes to copy into the runtime string value.
void mapSetStr(void *map, const char *keyName, std::string_view value) {
    rt_string key = toRtString(keyName);
    rt_string text = toRtString(value);
    rt_map_set_str(map, key, text);
    rt_string_unref(text);
    rt_string_unref(key);
}

/// @brief Convert all diagnostic fixits into a runtime sequence of maps.
/// @param diagnostic Diagnostic whose fixits should be exposed.
/// @return Owned runtime sequence; callers should store/release it like other runtime objects.
///
/// @details Legacy single-fixit fields remain populated from `front()` for API compatibility.
///          This sequence provides the complete machine-applicable fix list without truncation.
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

/// @brief Map diagnostic severity to the editor-services numeric schema.
/// @param severity Compiler diagnostic severity.
/// @return Zero for errors, one for warnings, and two for notes.
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

/// @brief Map diagnostic severity to its stable editor-facing label.
/// @param severity Compiler diagnostic severity.
/// @return Static `error`, `warning`, or `note` text.
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

/// @brief Resolve the source path associated with a diagnostic location.
/// @param loc Diagnostic source location.
/// @param sm Source manager that owns file identifiers.
/// @param fallbackPath Path returned for synthetic or unknown file identifiers.
/// @return Source-manager path when available, otherwise @p fallbackPath.
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

/// @brief Resolve a path directly from a source-manager file identifier.
/// @param fileId Source-manager file identifier.
/// @param sm Source manager that owns @p fileId.
/// @param fallbackPath Path returned for zero or unknown identifiers.
/// @return Registered source path when available, otherwise @p fallbackPath.
std::string sourcePathForFile(uint32_t fileId,
                              const il::support::SourceManager &sm,
                              const std::string &fallbackPath) {
    if (fileId != 0) {
        std::string_view path = sm.getPath(fileId);
        if (!path.empty())
            return std::string(path);
    }
    return fallbackPath;
}

struct FixitRecord {
    std::string message;
    std::string replacement;
    int64_t startLine{0};
    int64_t startColumn{0};
    int64_t endLine{0};
    int64_t endColumn{0};
};

struct DiagnosticRecord {
    std::string file;
    int64_t line{0};
    int64_t column{0};
    int64_t endLine{0};
    int64_t endColumn{0};
    int64_t severity{0};
    std::string severityName;
    std::string code;
    std::string message;
    std::string stage;
    std::string help;
    bool hasFixit{false};
    std::vector<FixitRecord> fixits; ///< all fixes; keeps async schema parity (VDOC-116)
    std::string fixitMessage;
    std::string fixitReplacement;
    int64_t fixitStartLine{0};
    int64_t fixitStartColumn{0};
    int64_t fixitEndLine{0};
    int64_t fixitEndColumn{0};
};

/// @brief Convert one compiler diagnostic to the synchronous runtime-map schema.
/// @param diagnostic Diagnostic to serialize.
/// @param sm Source manager used to resolve diagnostic file identifiers.
/// @param fallbackPath Path used when the diagnostic has no registered file.
/// @return Owned runtime map containing range, severity, message, and fix-it fields.
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
    if (hasFixit) {
        const auto &fixit = diagnostic.fixits.front();
        mapSetStr(map, "fixitMessage", fixit.message);
        mapSetStr(map, "fixitReplacement", fixit.replacement);
        mapSetInt(map, "fixitStartLine", fixit.range.begin.line);
        mapSetInt(map, "fixitStartColumn", fixit.range.begin.column);
        mapSetInt(map, "fixitEndLine", fixit.range.end.line);
        mapSetInt(map, "fixitEndColumn", fixit.range.end.column);
    } else {
        mapSetStr(map, "fixitMessage", "");
        mapSetStr(map, "fixitReplacement", "");
        mapSetInt(map, "fixitStartLine", 0);
        mapSetInt(map, "fixitStartColumn", 0);
        mapSetInt(map, "fixitEndLine", 0);
        mapSetInt(map, "fixitEndColumn", 0);
    }
    return map;
}

/// @brief Convert a diagnostic engine's current diagnostics to a runtime sequence.
/// @param diagnostics Diagnostic collection to serialize.
/// @param sm Source manager used to resolve file identifiers.
/// @param fallbackPath Path used for diagnostics without a registered file.
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

/// @brief Copy a compiler diagnostic into thread-safe C++ job storage.
/// @param diagnostic Diagnostic to snapshot.
/// @param sm Source manager used to resolve file identifiers.
/// @param fallbackPath Path used when no registered file is available.
/// @return Value record preserving every fix-it and legacy first-fix fields.
DiagnosticRecord diagnosticToRecord(const il::support::Diagnostic &diagnostic,
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

    DiagnosticRecord record;
    record.file = pathForLocation(start, sm, fallbackPath);
    record.line = start.line;
    record.column = start.column;
    record.endLine = end.line;
    record.endColumn = end.column;
    record.severity = diagnosticSeverityCode(diagnostic.severity);
    record.severityName = std::string(diagnosticSeverityName(diagnostic.severity));
    record.code = diagnostic.code;
    record.message = diagnostic.message;
    record.stage = diagnostic.stage;
    record.help = diagnostic.help;
    record.hasFixit = !diagnostic.fixits.empty();
    for (const auto &fixit : diagnostic.fixits) {
        FixitRecord fr;
        fr.message = fixit.message;
        fr.replacement = fixit.replacement;
        fr.startLine = fixit.range.begin.line;
        fr.startColumn = fixit.range.begin.column;
        fr.endLine = fixit.range.end.line;
        fr.endColumn = fixit.range.end.column;
        record.fixits.push_back(std::move(fr));
    }
    if (record.hasFixit) {
        const auto &first = record.fixits.front();
        record.fixitMessage = first.message;
        record.fixitReplacement = first.replacement;
        record.fixitStartLine = first.startLine;
        record.fixitStartColumn = first.startColumn;
        record.fixitEndLine = first.endLine;
        record.fixitEndColumn = first.endColumn;
    }
    return record;
}

/// @brief Convert asynchronous diagnostic snapshots to runtime maps.
/// @param diagnostics Immutable job result records.
/// @return Owned runtime sequence matching the synchronous diagnostic schema.
void *diagnosticRecordsToSeq(const std::vector<DiagnosticRecord> &diagnostics) {
    void *seq = rt_seq_new_owned();
    for (const auto &diagnostic : diagnostics) {
        void *map = rt_map_new();
        mapSetStr(map, "file", diagnostic.file);
        mapSetInt(map, "line", diagnostic.line);
        mapSetInt(map, "column", diagnostic.column);
        mapSetInt(map, "endLine", diagnostic.endLine);
        mapSetInt(map, "endColumn", diagnostic.endColumn);
        mapSetInt(map, "severity", diagnostic.severity);
        mapSetStr(map, "severityName", diagnostic.severityName);
        mapSetStr(map, "code", diagnostic.code);
        mapSetStr(map, "message", diagnostic.message);
        mapSetStr(map, "stage", diagnostic.stage);
        mapSetStr(map, "help", diagnostic.help);
        mapSetBool(map, "hasFixit", diagnostic.hasFixit);
        // Full fixits sequence: async diagnostics match the synchronous
        // Check*/Compile* map schema (VDOC-116).
        void *fixits = rt_seq_new_owned();
        for (const auto &fr : diagnostic.fixits) {
            void *fm = rt_map_new();
            mapSetStr(fm, "message", fr.message);
            mapSetStr(fm, "replacement", fr.replacement);
            mapSetInt(fm, "startLine", fr.startLine);
            mapSetInt(fm, "startColumn", fr.startColumn);
            mapSetInt(fm, "endLine", fr.endLine);
            mapSetInt(fm, "endColumn", fr.endColumn);
            rt_seq_push(fixits, fm);
            releaseRuntimeObject(fm);
        }
        mapSetObject(map, "fixits", fixits);
        releaseRuntimeObject(fixits);
        mapSetStr(map, "fixitMessage", diagnostic.fixitMessage);
        mapSetStr(map, "fixitReplacement", diagnostic.fixitReplacement);
        mapSetInt(map, "fixitStartLine", diagnostic.fixitStartLine);
        mapSetInt(map, "fixitStartColumn", diagnostic.fixitStartColumn);
        mapSetInt(map, "fixitEndLine", diagnostic.fixitEndLine);
        mapSetInt(map, "fixitEndColumn", diagnostic.fixitEndColumn);
        rt_seq_push(seq, map);
        releaseRuntimeObject(map);
    }
    return seq;
}

constexpr int64_t kProjectIndexClassId = INT64_C(-0x460101);
constexpr int64_t kSemanticJobClassId = INT64_C(-0x460102);
constexpr int64_t kMaxProjectQueryReferences = 2000;

enum class SemanticJobKind : int64_t {
    Unknown = 0,
    CompletionItems = 1,
    SignatureInfo = 2,
    HoverInfo = 3,
    Symbols = 4,
    Diagnostics = 5,
    Tokens = 6,
    Analysis = 7,
};

struct IndexedSource {
    std::string path;
    std::shared_ptr<const std::string> source;
};

struct ProjectIndex {
    /// @brief Create an empty index rooted at a normalized project path.
    /// @param rootPath Root against which relative file paths are resolved.
    explicit ProjectIndex(std::string rootPath) : root(std::move(rootPath)) {}

    std::string root;
    std::unordered_map<std::string, IndexedSource> sources;
};

struct ProjectIndexHandle {
    ProjectIndex *index{nullptr};
    std::mutex *mutex{nullptr};
};

/// @brief Return an indexed source's text or a stable empty-string fallback.
/// @param source Indexed path and shared text snapshot.
/// @return Reference to the stored text, or a process-lifetime empty string.
const std::string &indexedSourceText(const IndexedSource &source) {
    static const std::string kEmpty;
    return source.source ? *source.source : kEmpty;
}

struct SemanticJob {
    /// @brief Initialize an unfinished asynchronous semantic job.
    /// @param kind Result schema produced by the worker.
    explicit SemanticJob(SemanticJobKind kind) : kind(kind) {}

    SemanticJobKind kind{SemanticJobKind::Unknown};
    std::atomic<bool> done{false};
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::string error;

    std::vector<CompletionItem> completionItems;
    std::string signatureDisplay;
    std::string signatureSource;
    std::string signatureDocumentation;
    int signatureLine{1};
    int signatureCol{0};
    std::string hoverDisplay;
    std::string hoverDocumentation;
    std::string symbols;
    std::string tokens;
    std::vector<DiagnosticRecord> diagnostics;
};

struct SemanticJobHandle {
    std::shared_ptr<SemanticJob> *job{nullptr};
};

/// @brief Reserve one of the bounded semantic worker slots.
/// @return True when the active-job counter was incremented.
bool tryAcquireSemanticWorkerSlot() {
    int active = g_activeSemanticWorkerJobs.load(std::memory_order_acquire);
    while (active < kMaxSemanticWorkerJobs) {
        if (g_activeSemanticWorkerJobs.compare_exchange_weak(
                active, active + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    }
    return false;
}

/// @brief Return a previously acquired semantic worker slot.
void releaseSemanticWorkerSlot() {
    g_activeSemanticWorkerJobs.fetch_sub(1, std::memory_order_acq_rel);
}

/// @brief Publish successful job completion and release its worker slot.
/// @param job Shared job state updated with release ordering.
void finishSemanticJob(const std::shared_ptr<SemanticJob> &job) {
    job->done.store(true, std::memory_order_release);
    releaseSemanticWorkerSlot();
}

/// @brief Publish a startup failure on a job that did not acquire a worker slot.
/// @param job Shared job state that receives the error.
/// @param message Human-readable failure description.
void completeSemanticJobWithError(const std::shared_ptr<SemanticJob> &job, std::string message) {
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->error = std::move(message);
    }
    job->done.store(true, std::memory_order_release);
}

struct IdentifierToken {
    std::string text;
    il::support::SourceLoc loc{};
    uint32_t endColumn{0};
};

struct SymbolKey {
    bool valid{false};
    std::string semanticName;
    std::string displayName;
    std::string kind;
    std::string typeDisplay;
    std::string ownerType;
    std::string file;
    uint32_t line{0};
    uint32_t column{0};
};

struct SymbolRange {
    bool valid{false};
    std::string file;
    uint32_t line{0};
    uint32_t column{0};
    uint32_t endLine{0};
    uint32_t endColumn{0};
};

/// @brief Normalize a project root for stable path resolution.
/// @param root User-supplied root, an empty path, or the `<editor>` sentinel.
/// @return Absolute lexically normalized path where possible; `<editor>` is preserved.
std::string normalizeProjectRoot(std::string root) {
    if (root.empty())
        root = ".";
    if (root == "<editor>")
        return root;
    std::error_code ec;
    std::filesystem::path path = zanna::filesystem::pathFromUtf8(root);
    path = std::filesystem::absolute(path, ec);
    if (ec)
        path = zanna::filesystem::pathFromUtf8(root);
    return zanna::filesystem::pathToUtf8(path.lexically_normal());
}

/// @brief Resolve a project path against an index root and normalize it.
/// @param index Project index providing the root for relative paths.
/// @param path Source path to resolve.
/// @return Absolute lexically normalized path where possible, or `<editor>`.
std::string normalizeProjectPath(const ProjectIndex &index, std::string path) {
    if (path.empty())
        return "<editor>";
    if (path == "<editor>")
        return path;

    std::filesystem::path fsPath = zanna::filesystem::pathFromUtf8(path);
    if (fsPath.is_relative() && !index.root.empty() && index.root != "<editor>")
        fsPath = zanna::filesystem::pathFromUtf8(index.root) / fsPath;

    std::error_code ec;
    fsPath = std::filesystem::absolute(fsPath, ec);
    if (ec)
        fsPath = zanna::filesystem::pathFromUtf8(path);
    return zanna::filesystem::pathToUtf8(fsPath.lexically_normal());
}

/// @brief Produce a platform-stable key for comparing project paths.
/// @param path Path to absolutize, weakly canonicalize, and slash-normalize.
/// @return Canonical comparison key; Windows keys are also lowercased.
std::string projectPathLookupKey(std::string path) {
    if (path.empty() || path == "<editor>")
        return path;
    std::error_code ec;
    std::filesystem::path fsPath =
        std::filesystem::absolute(zanna::filesystem::pathFromUtf8(path), ec);
    if (ec)
        fsPath = zanna::filesystem::pathFromUtf8(path);
    std::filesystem::path canonical = std::filesystem::weakly_canonical(fsPath, ec);
    if (!ec)
        path = zanna::filesystem::pathToUtf8(canonical.lexically_normal());
    else
        path = zanna::filesystem::pathToUtf8(fsPath.lexically_normal());
    std::replace(path.begin(), path.end(), '\\', '/');
    if (zanna::platform::kHostWindows) {
        /// @brief Converts one path byte to lowercase.
        /// @param ch Unsigned byte to convert.
        /// @return Lowercase character value.
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    }
    return path;
}

/// @brief Match a path to the exact spelling stored by a project index.
/// @param index Index whose source keys are searched.
/// @param path Absolute or root-relative candidate path.
/// @return Existing indexed key when canonically equivalent, otherwise the normalized path.
std::string canonicalProjectPath(const ProjectIndex &index, const std::string &path) {
    std::string normalized = normalizeProjectPath(index, path);
    if (index.sources.find(normalized) != index.sources.end())
        return normalized;

    const std::string lookup = projectPathLookupKey(normalized);
    for (const auto &[indexedPath, _] : index.sources) {
        if (projectPathLookupKey(indexedPath) == lookup)
            return indexedPath;
    }
    return normalized;
}

/// @brief Validate and cast an opaque runtime project-index object.
/// @param handle Candidate runtime object.
/// @return Typed payload pointer, or nullptr for a null/wrong-class object.
ProjectIndexHandle *asProjectIndexHandle(void *handle) {
    if (!rt_obj_is_instance(handle, kProjectIndexClassId, sizeof(ProjectIndexHandle)))
        return nullptr;
    return static_cast<ProjectIndexHandle *>(handle);
}

/// @brief Copy a project index while holding its handle mutex.
/// @param handle Opaque project-index runtime object.
/// @return Independent index snapshot, or an empty pointer for an invalid handle.
std::unique_ptr<ProjectIndex> snapshotProjectIndex(void *handle) {
    ProjectIndexHandle *typed = asProjectIndexHandle(handle);
    if (!typed || !typed->mutex)
        return {};
    std::lock_guard<std::mutex> lock(*typed->mutex);
    if (!typed->index)
        return {};
    return std::make_unique<ProjectIndex>(*typed->index);
}

/// @brief Validate and cast an opaque runtime semantic-job object.
/// @param handle Candidate runtime object.
/// @return Typed payload pointer, or nullptr for a null/wrong-class object.
SemanticJobHandle *asSemanticJobHandle(void *handle) {
    if (!rt_obj_is_instance(handle, kSemanticJobClassId, sizeof(SemanticJobHandle)))
        return nullptr;
    return static_cast<SemanticJobHandle *>(handle);
}

/// @brief Recover shared semantic-job state from an opaque handle.
/// @param handle Runtime semantic-job object.
/// @return Shared job state, or an empty pointer for an invalid/finalized handle.
std::shared_ptr<SemanticJob> asSemanticJob(void *handle) {
    SemanticJobHandle *typed = asSemanticJobHandle(handle);
    if (!typed || !typed->job)
        return {};
    return *typed->job;
}

/// @brief Runtime finalizer for project-index native state.
/// @param obj Runtime object whose index and mutex are destroyed.
void projectIndexFinalize(void *obj) {
    ProjectIndexHandle *handle = asProjectIndexHandle(obj);
    if (!handle)
        return;
    if (handle->mutex) {
        {
            std::lock_guard<std::mutex> lock(*handle->mutex);
            delete handle->index;
            handle->index = nullptr;
        }
        delete handle->mutex;
        handle->mutex = nullptr;
    } else {
        delete handle->index;
        handle->index = nullptr;
    }
}

/// @brief Runtime finalizer for an asynchronous semantic-job handle.
/// @param obj Runtime object whose job is cancelled and shared holder destroyed.
void semanticJobFinalize(void *obj) {
    SemanticJobHandle *handle = asSemanticJobHandle(obj);
    if (!handle)
        return;
    if (handle->job) {
        if (*handle->job)
            (*handle->job)->cancelled.store(true, std::memory_order_release);
        delete handle->job;
        handle->job = nullptr;
    }
}

/// @brief Wrap shared job state in a finalizable runtime object.
/// @param job Job state retained by the returned handle.
/// @return Owned runtime object, or nullptr when allocation fails.
void *semanticJobHandleFor(std::shared_ptr<SemanticJob> job) {
    auto *handle = static_cast<SemanticJobHandle *>(
        rt_obj_new_i64(kSemanticJobClassId, sizeof(SemanticJobHandle)));
    if (!handle)
        return nullptr;
    handle->job = new std::shared_ptr<SemanticJob>(std::move(job));
    rt_obj_set_finalizer(handle, semanticJobFinalize);
    return handle;
}

/// @brief Convert a semantic symbol kind to the editor protocol label.
/// @param kind Semantic symbol category.
/// @return Stable lowercase category name.
std::string symbolKindName(Symbol::Kind kind) {
    switch (kind) {
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
    return "symbol";
}

/// @brief Strip namespace or owner qualification from a semantic name.
/// @param name Possibly dotted symbol name.
/// @return Text following the final dot, or the complete input.
std::string baseSymbolName(std::string_view name) {
    size_t pos = name.rfind('.');
    if (pos == std::string_view::npos)
        return std::string(name);
    return std::string(name.substr(pos + 1));
}

/// @brief Lex every identifier token from a source snapshot.
/// @param source Source text to tokenize.
/// @param fileId File identifier embedded in returned locations.
/// @return Identifier text and one-past-end column records in source order.
std::vector<IdentifierToken> lexIdentifierTokens(const std::string &source, uint32_t fileId) {
    il::support::DiagnosticEngine diagnostics;
    Lexer lexer(source, fileId, diagnostics);

    std::vector<IdentifierToken> result;
    while (true) {
        Token token = lexer.next();
        if (token.kind == TokenKind::Eof)
            break;
        if (token.kind != TokenKind::Identifier)
            continue;
        IdentifierToken ident;
        ident.text = token.text;
        ident.loc = token.loc;
        ident.endColumn = token.loc.column + static_cast<uint32_t>(token.text.size());
        result.push_back(std::move(ident));
    }
    return result;
}

/// @brief Find the identifier covering an editor cursor position.
/// @param source Source text to tokenize.
/// @param fileId File identifier embedded in token locations.
/// @param line One-based target line.
/// @param col Zero-based target byte column.
/// @return Covering identifier, or no value for invalid/non-identifier positions.
std::optional<IdentifierToken> tokenAtPosition(const std::string &source,
                                               uint32_t fileId,
                                               int64_t line,
                                               int64_t col) {
    if (line <= 0 || col < 0)
        return std::nullopt;
    const uint32_t targetLine = static_cast<uint32_t>(line);
    const uint32_t targetColumn = static_cast<uint32_t>(col + 1);
    for (const auto &token : lexIdentifierTokens(source, fileId)) {
        if (token.loc.line != targetLine)
            continue;
        if (targetColumn < token.loc.column || targetColumn > token.endColumn)
            continue;
        return token;
    }
    return std::nullopt;
}

/// @brief Select the best available definition location for a symbol.
/// @param symbol Semantic symbol with optional explicit and declaration locations.
/// @param fallback Position-specific snapshot location.
/// @return First valid location in fallback, symbol, declaration order.
il::support::SourceLoc symbolDefinitionLoc(const Symbol &symbol,
                                           const il::support::SourceLoc &fallback) {
    if (fallback.isValid())
        return fallback;
    if (symbol.loc.isValid())
        return symbol.loc;
    if (symbol.decl && symbol.decl->loc.isValid())
        return symbol.decl->loc;
    return {};
}

/// @brief Build the stable identity used by definition/reference queries.
/// @param symbol Resolved semantic symbol.
/// @param fallbackLoc Position-based definition location.
/// @param ownerType Enclosing nominal type, when applicable.
/// @param sm Source manager used to resolve the selected file identifier.
/// @param fallbackPath Path used when no source-manager path is available.
/// @return Valid key when a definition location exists; otherwise an invalid key.
SymbolKey keyForSymbol(const Symbol &symbol,
                       const il::support::SourceLoc &fallbackLoc,
                       std::string_view ownerType,
                       const il::support::SourceManager &sm,
                       const std::string &fallbackPath) {
    il::support::SourceLoc loc = symbolDefinitionLoc(symbol, fallbackLoc);
    if (!loc.isValid())
        return {};

    SymbolKey key;
    key.valid = true;
    key.semanticName = symbol.name;
    key.displayName = baseSymbolName(symbol.name);
    key.kind = symbolKindName(symbol.kind);
    key.typeDisplay = symbol.type ? symbol.type->toDisplayString() : "";
    key.ownerType = std::string(ownerType);
    key.file = pathForLocation(loc, sm, fallbackPath);
    key.line = loc.line;
    key.column = loc.column;
    return key;
}

/// @brief Compare two resolved symbol identities.
/// @param lhs First symbol key.
/// @param rhs Second symbol key.
/// @return True when both keys identify the same named definition.
bool sameSymbolKey(const SymbolKey &lhs, const SymbolKey &rhs) {
    return lhs.valid && rhs.valid && lhs.semanticName == rhs.semanticName && lhs.kind == rhs.kind &&
           lhs.file == rhs.file && lhs.line == rhs.line && lhs.column == rhs.column;
}

/// @brief Analyze one indexed source with bound files supplied from the index snapshot.
/// @param index Project snapshot used to satisfy cross-file source requests.
/// @param path Path assigned to the primary source.
/// @param source Primary source text.
/// @param sm Source manager populated by analysis.
/// @return Owned partial-compilation result.
std::unique_ptr<AnalysisResult> analyzeIndexedSource(ProjectIndex &index,
                                                     const std::string &path,
                                                     const std::string &source,
                                                     il::support::SourceManager &sm) {
    CompilerInput input{.source = source, .path = path};
    /// @brief Supplies an indexed source file to the compiler import resolver.
    /// @param normalizedPath Requested normalized import path.
    /// @return Indexed source text, or `std::nullopt` when absent.
    input.sourceProvider = [&index](std::string_view normalizedPath) -> std::optional<std::string> {
        std::string indexedPath = canonicalProjectPath(index, std::string(normalizedPath));
        auto it = index.sources.find(indexedPath);
        if (it == index.sources.end())
            return std::nullopt;
        return indexedSourceText(it->second);
    };
    CompilerOptions opts{};
    return parseAndAnalyze(input, opts, sm);
}

/// @brief Resolve an identifier token against analyzed global symbols.
/// @param analysis Completed semantic result.
/// @param token Identifier occurrence to resolve.
/// @param sm Source manager owning analysis locations.
/// @param fallbackPath Primary source path.
/// @return Same-file definition when present, otherwise the first matching bound export.
std::optional<SymbolKey> findGlobalSymbolKey(const AnalysisResult &analysis,
                                             const IdentifierToken &token,
                                             const il::support::SourceManager &sm,
                                             const std::string &fallbackPath) {
    std::optional<SymbolKey> importedCandidate;
    for (const auto &symbol : analysis.sema->getGlobalSymbols()) {
        if (symbol.name != token.text && baseSymbolName(symbol.name) != token.text)
            continue;
        SymbolKey key = keyForSymbol(symbol, {}, "", sm, fallbackPath);
        if (!key.valid)
            continue;
        if (symbolDefinitionLoc(symbol, {}).file_id == analysis.fileId)
            return key;
        if (!importedCandidate)
            importedCandidate = std::move(key);
    }
    return importedCandidate;
}

/// @brief Resolve the symbol referenced at an indexed source position.
/// @param index Project source snapshot.
/// @param path Primary source path.
/// @param source Primary source text.
/// @param line One-based editor line.
/// @param col Zero-based editor byte column.
/// @return Stable definition key, or no value when analysis/token lookup fails.
std::optional<SymbolKey> resolveAtPosition(ProjectIndex &index,
                                           const std::string &path,
                                           const std::string &source,
                                           int64_t line,
                                           int64_t col) {
    il::support::SourceManager sm;
    auto analysis = analyzeIndexedSource(index, path, source, sm);
    if (!analysis || !analysis->sema)
        return std::nullopt;

    auto token = tokenAtPosition(source, analysis->fileId, line, col);
    if (!token)
        return std::nullopt;

    const ScopedSymbol *scoped = analysis->sema->findSymbolAtPosition(
        token->text, analysis->fileId, token->loc.line, token->loc.column);
    const std::string sourcePath = sourcePathForFile(analysis->fileId, sm, path);
    if (scoped) {
        SymbolKey key =
            keyForSymbol(scoped->symbol, scoped->loc, scoped->ownerType, sm, sourcePath);
        if (key.valid)
            return key;
    }
    return findGlobalSymbolKey(*analysis, *token, sm, sourcePath);
}

/// @brief Refine a symbol key into an editor-visible definition range.
/// @param index Project index containing the definition source when available.
/// @param key Resolved symbol identity.
/// @return Definition range; validity follows @p key even when token refinement fails.
SymbolRange definitionRangeForKey(const ProjectIndex &index, const SymbolKey &key) {
    if (!key.valid)
        return {};

    SymbolRange range;
    range.valid = true;
    range.file = canonicalProjectPath(index, key.file);
    range.line = key.line;
    range.column = key.column;
    range.endLine = key.line;
    range.endColumn = key.column + static_cast<uint32_t>(key.displayName.size());

    auto sourceIt = index.sources.find(range.file);
    if (sourceIt == index.sources.end())
        return range;

    il::support::SourceManager sm;
    uint32_t fileId = sm.addFile(range.file);
    const std::string &definitionSource = indexedSourceText(sourceIt->second);
    sm.setSource(fileId, definitionSource);
    for (const auto &token : lexIdentifierTokens(definitionSource, fileId)) {
        if (token.loc.line != key.line)
            continue;
        if (token.text != key.displayName)
            continue;
        if (token.loc.column < key.column)
            continue;
        range.column = token.loc.column;
        range.endColumn = token.endColumn;
        return range;
    }

    for (const auto &token : lexIdentifierTokens(definitionSource, fileId)) {
        if (token.loc.line != key.line)
            continue;
        if (token.text != key.displayName)
            continue;
        range.column = token.loc.column;
        range.endColumn = token.endColumn;
        return range;
    }
    return range;
}

/// @brief Build a negative definition-query result.
/// @param reason Stable failure reason.
/// @return Owned runtime map with `found` set to false.
void *notFoundMap(std::string_view reason) {
    void *map = rt_map_new();
    mapSetBool(map, "found", false);
    mapSetStr(map, "reason", reason);
    return map;
}

/// @brief Add one-based compiler and zero-based editor range fields to a map.
/// @param map Mutable runtime map.
/// @param range Definition/reference range to serialize.
void setRangeFields(void *map, const SymbolRange &range) {
    mapSetStr(map, "file", range.file);
    mapSetInt(map, "line", range.line);
    mapSetInt(map, "column", range.column);
    mapSetInt(map, "endLine", range.endLine);
    mapSetInt(map, "endColumn", range.endColumn);
    mapSetInt(map, "editorLine", range.line > 0 ? range.line - 1 : 0);
    mapSetInt(map, "editorColumn", range.column > 0 ? range.column - 1 : 0);
    mapSetInt(map, "editorEndLine", range.endLine > 0 ? range.endLine - 1 : 0);
    mapSetInt(map, "editorEndColumn", range.endColumn > 0 ? range.endColumn - 1 : 0);
}

/// @brief Serialize a resolved symbol definition.
/// @param index Project index used to refine the source range.
/// @param key Stable symbol identity.
/// @return Owned positive definition map or a `not_found` map.
void *definitionMapForKey(const ProjectIndex &index, const SymbolKey &key) {
    SymbolRange range = definitionRangeForKey(index, key);
    if (!range.valid)
        return notFoundMap("not_found");

    void *map = rt_map_new();
    mapSetBool(map, "found", true);
    setRangeFields(map, range);
    mapSetStr(map, "name", key.displayName);
    mapSetStr(map, "semanticName", key.semanticName);
    mapSetStr(map, "kind", key.kind);
    mapSetStr(map, "type", key.typeDisplay);
    mapSetStr(map, "ownerType", key.ownerType);
    return map;
}

/// @brief Serialize one resolved identifier occurrence.
/// @param token Identifier token and source range.
/// @param path Canonical indexed source path.
/// @param definitionRange Target symbol's refined definition range.
/// @param key Target symbol identity and display metadata.
/// @return Owned runtime reference map.
void *referenceMapForToken(const IdentifierToken &token,
                           const std::string &path,
                           const SymbolRange &definitionRange,
                           const SymbolKey &key) {
    void *map = rt_map_new();
    mapSetStr(map, "file", path);
    mapSetInt(map, "line", token.loc.line);
    mapSetInt(map, "column", token.loc.column);
    mapSetInt(map, "endLine", token.loc.line);
    mapSetInt(map, "endColumn", token.endColumn);
    mapSetInt(map, "editorLine", token.loc.line > 0 ? token.loc.line - 1 : 0);
    mapSetInt(map, "editorColumn", token.loc.column > 0 ? token.loc.column - 1 : 0);
    mapSetInt(map, "editorEndLine", token.loc.line > 0 ? token.loc.line - 1 : 0);
    mapSetInt(map, "editorEndColumn", token.endColumn > 0 ? token.endColumn - 1 : 0);
    mapSetStr(map, "name", key.displayName);
    mapSetStr(map, "semanticName", key.semanticName);
    mapSetStr(map, "kind", key.kind);
    const bool isDefinition = definitionRange.valid && path == definitionRange.file &&
                              token.loc.line == definitionRange.line &&
                              token.loc.column == definitionRange.column;
    mapSetBool(map, "isDefinition", isDefinition);
    return map;
}

/// @brief Collect indexed source paths in deterministic order.
/// @param index Project index to inspect.
/// @return Lexicographically sorted copy of its source keys.
std::vector<std::string> sortedIndexPaths(const ProjectIndex &index) {
    std::vector<std::string> paths;
    paths.reserve(index.sources.size());
    for (const auto &[path, _] : index.sources)
        paths.push_back(path);
    std::sort(paths.begin(), paths.end());
    return paths;
}

/// @brief Find semantically equivalent identifier occurrences across an index.
/// @param index Mutable project snapshot used for per-file analysis.
/// @param targetKey Definition identity to match.
/// @param maxReferences Positive result cap, or non-positive for unlimited.
/// @return Owned runtime sequence of reference maps in path/token order.
void *referencesForKey(ProjectIndex &index, const SymbolKey &targetKey, int64_t maxReferences) {
    void *seq = rt_seq_new_owned();
    if (!targetKey.valid)
        return seq;

    SymbolRange definitionRange = definitionRangeForKey(index, targetKey);
    for (const std::string &path : sortedIndexPaths(index)) {
        auto sourceIt = index.sources.find(path);
        if (sourceIt == index.sources.end())
            continue;

        il::support::SourceManager sm;
        const std::string &indexedSource = indexedSourceText(sourceIt->second);
        auto analysis = analyzeIndexedSource(index, path, indexedSource, sm);
        if (!analysis || !analysis->sema)
            continue;

        for (const auto &token : lexIdentifierTokens(indexedSource, analysis->fileId)) {
            if (token.text != targetKey.displayName)
                continue;

            const ScopedSymbol *scoped = analysis->sema->findSymbolAtPosition(
                token.text, analysis->fileId, token.loc.line, token.loc.column);
            const std::string sourcePath = sourcePathForFile(analysis->fileId, sm, path);
            std::optional<SymbolKey> refKey;
            if (scoped) {
                refKey =
                    keyForSymbol(scoped->symbol, scoped->loc, scoped->ownerType, sm, sourcePath);
            } else {
                refKey = findGlobalSymbolKey(*analysis, token, sm, sourcePath);
            }
            if (!refKey || !sameSymbolKey(*refKey, targetKey))
                continue;

            void *map = referenceMapForToken(token, path, definitionRange, targetKey);
            rt_seq_push(seq, map);
            releaseRuntimeObject(map);
            if (maxReferences > 0 && rt_seq_len(seq) >= maxReferences)
                return seq;
        }
    }
    return seq;
}

/// @brief Validate a proposed rename against the Zia identifier grammar.
/// @param text Candidate identifier bytes.
/// @return True when length, start/continuation bytes, and keyword status are valid.
bool isIdentifierText(std::string_view text) {
    // Mirror the Zia lexer exactly (VDOC-114): deterministic ASCII
    // classification (il::frontends::common::char_utils) and the lexer's 1024-byte
    // identifier cap, so an accepted rename can never produce a lexer error.
    if (text.empty() || text.size() > 1024)
        return false;
    if (!il::frontends::common::char_utils::isIdentifierStart(text.front()))
        return false;
    for (char ch : text.substr(1)) {
        if (!il::frontends::common::char_utils::isIdentifierContinue(ch))
            return false;
    }
    return !Lexer::lookupKeyword(text).has_value();
}

/// @brief Check whether a proposed name resolves to another symbol at any reference.
/// @param index Project snapshot used for semantic probes.
/// @param targetKey Symbol being renamed.
/// @param references Runtime sequence returned by referencesForKey().
/// @param newName Valid candidate identifier.
/// @return True when at least one occurrence would bind to a different definition.
bool renameWouldCollide(ProjectIndex &index,
                        const SymbolKey &targetKey,
                        void *references,
                        std::string_view newName) {
    const int64_t count = rt_seq_len(references);
    for (int64_t i = 0; i < count; ++i) {
        void *refMap = rt_seq_get(references, i);
        rt_string fileKey = toRtString("file");
        rt_string lineKey = toRtString("line");
        rt_string columnKey = toRtString("column");
        rt_string fileValue = rt_map_get_str(refMap, fileKey);
        int64_t line = rt_map_get_int(refMap, lineKey);
        int64_t column = rt_map_get_int(refMap, columnKey);
        std::string path = toStdString(fileValue);
        rt_string_unref(fileValue);
        rt_string_unref(columnKey);
        rt_string_unref(lineKey);
        rt_string_unref(fileKey);

        auto sourceIt = index.sources.find(path);
        if (sourceIt == index.sources.end())
            continue;

        IdentifierToken probe;
        probe.text = std::string(newName);
        probe.loc =
            il::support::SourceLoc{0, static_cast<uint32_t>(line), static_cast<uint32_t>(column)};
        probe.endColumn = static_cast<uint32_t>(column + newName.size());

        il::support::SourceManager sm;
        auto analysis = analyzeIndexedSource(index, path, indexedSourceText(sourceIt->second), sm);
        if (!analysis || !analysis->sema)
            continue;
        probe.loc.file_id = analysis->fileId;

        const ScopedSymbol *scoped = analysis->sema->findSymbolAtPosition(
            probe.text, analysis->fileId, probe.loc.line, probe.loc.column);
        const std::string sourcePath = sourcePathForFile(analysis->fileId, sm, path);
        std::optional<SymbolKey> collisionKey;
        if (scoped)
            collisionKey =
                keyForSymbol(scoped->symbol, scoped->loc, scoped->ownerType, sm, sourcePath);
        else
            collisionKey = findGlobalSymbolKey(*analysis, probe, sm, sourcePath);

        if (collisionKey && !sameSymbolKey(*collisionKey, targetKey))
            return true;
    }
    return false;
}

/// @brief Build a failed rename result with an empty edit sequence.
/// @param reason Stable failure reason.
/// @return Owned runtime result map.
void *renameFailureMap(std::string_view reason) {
    void *map = rt_map_new();
    mapSetBool(map, "success", false);
    mapSetStr(map, "reason", reason);
    void *edits = rt_seq_new_owned();
    mapSetObject(map, "edits", edits);
    releaseRuntimeObject(edits);
    return map;
}

/// @brief Convert completion kind to its stable structured-API label.
/// @param kind Completion category.
/// @return Process-lifetime lowercase/camel-case label.
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

/// @brief Serialize one native completion item to a runtime map.
/// @param item Completion result to expose.
/// @return Owned map containing text, kind, documentation, and replacement metadata.
void *completionItemToMap(const CompletionItem &item) {
    void *map = rt_map_new();
    mapSetStr(map, "label", item.label);
    mapSetStr(map, "insertText", item.insertText);
    mapSetInt(map, "kind", static_cast<int64_t>(item.kind));
    mapSetStr(map, "kindName", completionKindName(item.kind));
    mapSetStr(map, "detail", item.detail);
    mapSetStr(map, "documentation", item.documentation);
    mapSetStr(map, "source", item.source);
    mapSetStr(map, "commitCharacters", item.commitCharacters);
    mapSetBool(map, "isSnippet", item.isSnippet);
    mapSetInt(map, "cursorOffset", item.cursorOffset);
    mapSetInt(map, "replacementStartLine", item.replacementStartLine);
    mapSetInt(map, "replacementStartColumn", item.replacementStartColumn);
    mapSetInt(map, "replacementEndLine", item.replacementEndLine);
    mapSetInt(map, "replacementEndColumn", item.replacementEndColumn);
    return map;
}

/// @brief Serialize native completion items to an owned runtime sequence.
/// @param items Completion results in final ranked order.
/// @return Owned sequence of completion maps.
void *completionItemsToSeq(const std::vector<CompletionItem> &items) {
    void *seq = rt_seq_new_owned();
    for (const auto &item : items) {
        void *map = completionItemToMap(item);
        rt_seq_push(seq, map);
        releaseRuntimeObject(map);
    }
    return seq;
}

/// @brief Copy text after removing leading and trailing ASCII whitespace.
/// @param text Input byte view.
/// @return Trimmed owning string.
std::string trimAscii(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string(text.substr(start, end - start));
}

/// @brief Test a bytewise, case-sensitive prefix.
/// @param text Candidate text.
/// @param prefix Required leading bytes.
/// @return True when @p text starts with @p prefix.
bool startsWithAscii(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/// @brief Read an ASCII identifier-like token after optional whitespace.
/// @param text Source line.
/// @param start Initial byte offset.
/// @return Consecutive alphanumeric/underscore bytes after leading whitespace.
std::string readIdentifier(std::string_view text, size_t start) {
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    size_t end = start;
    while (end < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
        ++end;
    return std::string(text.substr(start, end - start));
}

/// @brief Test whether a source line declares a supported top-level name.
/// @param line Source line, optionally beginning with `expose` or `final`.
/// @param name Identifier expected after a declaration keyword.
/// @return True for matching function, nominal type, or variable declarations.
bool declarationLineMatchesName(std::string_view line, std::string_view name) {
    std::string text = trimAscii(line);
    if (startsWithAscii(text, "expose "))
        text = trimAscii(std::string_view(text).substr(7));
    if (startsWithAscii(text, "final "))
        text = trimAscii(std::string_view(text).substr(6));

    constexpr std::string_view keywords[] = {
        "func ",
        "class ",
        "struct ",
        "interface ",
        "var ",
    };
    for (std::string_view keyword : keywords) {
        if (startsWithAscii(text, keyword))
            return readIdentifier(text, keyword.size()) == std::string(name);
    }
    return false;
}

/// @brief Collect contiguous triple-slash documentation above a line.
/// @param lines Source split into lines without newline terminators.
/// @param declLine Zero-based declaration-line index.
/// @return Documentation in source order, with markers removed.
std::string docCommentBeforeLine(const std::vector<std::string> &lines, size_t declLine) {
    if (declLine == 0 || declLine > lines.size())
        return {};

    std::vector<std::string> docs;
    size_t i = declLine;
    while (i > 0) {
        --i;
        std::string text = trimAscii(lines[i]);
        if (startsWithAscii(text, "///"))
            docs.push_back(trimAscii(std::string_view(text).substr(3)));
        else
            break;
    }

    std::string doc;
    for (auto it = docs.rbegin(); it != docs.rend(); ++it) {
        if (!doc.empty())
            doc += "\n";
        doc += *it;
    }
    return doc;
}

/// @brief Collect contiguous triple-slash documentation above a source location.
/// @param sm Source manager containing the referenced file text.
/// @param loc One-based declaration location.
/// @return Documentation in source order, with markers removed.
std::string docCommentBeforeLoc(const il::support::SourceManager &sm, il::support::SourceLoc loc) {
    if (!loc.isValid() || loc.line <= 1)
        return {};

    std::vector<std::string> docs;
    uint32_t line = loc.line - 1;
    while (line > 0) {
        std::string text = trimAscii(sm.getLine(loc.file_id, line));
        if (startsWithAscii(text, "///")) {
            docs.push_back(trimAscii(std::string_view(text).substr(3)));
        } else {
            break;
        }
        --line;
    }

    std::string doc;
    for (auto it = docs.rbegin(); it != docs.rend(); ++it) {
        if (!doc.empty())
            doc += "\n";
        doc += *it;
    }
    return doc;
}

/// @brief Obtain explicit or source-adjacent documentation for a symbol.
/// @param sm Source manager containing declaration text.
/// @param sym Semantic symbol.
/// @return Metadata documentation when present, otherwise preceding source comments.
std::string documentationForSymbolLoc(const il::support::SourceManager &sm, const Symbol &sym) {
    if (!sym.documentation.empty())
        return sym.documentation;
    il::support::SourceLoc loc = sym.loc.isValid() ? sym.loc : il::support::SourceLoc{};
    if (!loc.isValid() && sym.decl)
        loc = sym.decl->loc;
    return docCommentBeforeLoc(sm, loc);
}

/// @brief Find source documentation for the first supported declaration of a name.
/// @param source Full source buffer.
/// @param name Declaration identifier to locate.
/// @return Contiguous preceding triple-slash text, or an empty string.
std::string declarationDocumentationForName(std::string_view source, std::string_view name) {
    if (source.empty() || name.empty())
        return {};

    std::vector<std::string> lines;
    std::istringstream input{std::string(source)};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        if (declarationLineMatchesName(lines[i], name))
            return docCommentBeforeLine(lines, i);
    }
    return {};
}

/// @brief Extract the callable name preceding the first signature parenthesis.
/// @param display Signature-help display, optionally followed by documentation lines.
/// @return Trimmed callable name, or an empty string for a malformed display.
std::string signatureNameFromDisplay(std::string_view display) {
    std::string firstLine(display.substr(0, display.find('\n')));
    size_t open = firstLine.find('(');
    if (open == std::string::npos)
        return {};
    return trimAscii(std::string_view(firstLine).substr(0, open));
}

/// @brief Copy the first line of a multiline editor display.
/// @param display Display text.
/// @return Bytes preceding the first newline, or all input when single-line.
std::string firstDisplayLine(std::string_view display) {
    return std::string(display.substr(0, display.find('\n')));
}

/// @brief Extract non-parameter documentation lines following a display signature.
/// @param display Multiline signature/help text.
/// @return Trimmed documentation lines joined by newlines.
std::string documentationFromDisplayTail(std::string_view display) {
    size_t pos = display.find('\n');
    if (pos == std::string_view::npos)
        return {};
    ++pos;

    std::string doc;
    while (pos <= display.size()) {
        size_t next = display.find('\n', pos);
        std::string_view line =
            next == std::string_view::npos ? display.substr(pos) : display.substr(pos, next - pos);
        std::string trimmed = trimAscii(line);
        if (!trimmed.empty() && trimmed.rfind("parameter ", 0) != 0) {
            if (!doc.empty())
                doc += "\n";
            doc += trimmed;
        }
        if (next == std::string_view::npos)
            break;
        pos = next + 1;
    }
    return doc;
}

/// @brief Recognize a signature line within newline-delimited help output.
/// @param line Candidate display line.
/// @return True when the line contains a callable parenthesis before any colon.
bool looksLikeSignatureLine(std::string_view line) {
    std::string trimmed = trimAscii(line);
    if (trimmed.empty() || trimmed.rfind("parameter ", 0) == 0)
        return false;
    size_t open = trimmed.find('(');
    if (open == std::string::npos)
        return false;
    size_t colon = trimmed.find(':');
    return colon == std::string::npos || colon > open;
}

/// @brief Partition multiline signature help into overload display blocks.
/// @param display Newline-delimited signatures and associated documentation.
/// @return Non-empty overload blocks in source order.
std::vector<std::string> splitSignatureDisplays(std::string_view display) {
    std::vector<std::string> blocks;
    std::string current;
    size_t pos = 0;
    while (pos <= display.size()) {
        size_t next = display.find('\n', pos);
        std::string_view line =
            next == std::string_view::npos ? display.substr(pos) : display.substr(pos, next - pos);
        if (looksLikeSignatureLine(line) && !current.empty()) {
            blocks.push_back(std::move(current));
            current.clear();
        }
        if (!line.empty()) {
            if (!current.empty())
                current += "\n";
            current.append(line);
        }
        if (next == std::string_view::npos)
            break;
        pos = next + 1;
    }
    if (!current.empty())
        blocks.push_back(std::move(current));
    return blocks;
}

/// @brief Parse a display signature's parameters into structured runtime records.
/// @param signatureLine Single-line callable signature.
/// @return Owned sequence of parameter maps.
void *signatureParametersToSeq(std::string_view signatureLine);

/// @brief Serialize overload displays to structured runtime signature maps.
/// @param overloads Signature blocks in display order.
/// @return Owned sequence of overload maps.
void *signatureOverloadsToSeq(const std::vector<std::string> &overloads) {
    void *seq = rt_seq_new_owned();
    for (const auto &overload : overloads) {
        std::string firstLine = firstDisplayLine(overload);
        void *map = rt_map_new();
        mapSetStr(map, "display", firstLine);
        mapSetStr(map, "name", signatureNameFromDisplay(firstLine));
        size_t arrow = firstLine.find("->");
        std::string returnType = arrow == std::string::npos
                                     ? std::string()
                                     : trimAscii(std::string_view(firstLine).substr(arrow + 2));
        mapSetStr(map, "returnType", returnType);
        mapSetStr(map, "documentation", documentationFromDisplayTail(overload));
        void *params = signatureParametersToSeq(firstLine);
        mapSetObject(map, "parameters", params);
        releaseRuntimeObject(params);
        rt_seq_push(seq, map);
        releaseRuntimeObject(map);
    }
    return seq;
}

/// @brief Extract the symbol name from `kind name: type` hover text.
/// @param display Hover display text.
/// @return Trimmed symbol name, or an empty string for an incompatible shape.
std::string hoverNameFromDisplay(std::string_view display) {
    std::string text = firstDisplayLine(display);
    size_t firstSpace = text.find(' ');
    size_t colon = text.find(':');
    if (firstSpace == std::string::npos || colon == std::string::npos || colon <= firstSpace)
        return {};
    return trimAscii(std::string_view(text).substr(firstSpace + 1, colon - firstSpace - 1));
}

/// @brief Test the ASCII byte class used by editor hover scanning.
/// @param c Candidate byte.
/// @return True for an alphanumeric byte or underscore.
bool isIdentByte(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// @brief Read the single identifier immediately before a member-access dot.
/// @param source Full source buffer.
/// @param identStart Byte offset of the member identifier.
/// @return Immediate qualifier without the dot, or an empty string.
std::string qualifierBeforeIdentifier(std::string_view source, size_t identStart) {
    if (identStart == 0 || source[identStart - 1] != '.')
        return {};

    size_t end = identStart - 1;
    size_t start = end;
    while (start > 0 && isIdentByte(source[start - 1]))
        --start;
    if (start == end)
        return {};
    return std::string(source.substr(start, end - start));
}

/// @brief Read a dotted qualifier immediately before an identifier.
/// @param source Full source buffer.
/// @param identStart Byte offset of the trailing identifier.
/// @return Dotted qualifier without the final dot, or an empty string.
std::string dottedQualifierBeforeIdentifier(std::string_view source, size_t identStart) {
    if (identStart == 0 || source[identStart - 1] != '.')
        return {};

    size_t end = identStart - 1;
    size_t start = end;
    while (start > 0) {
        const char previous = source[start - 1];
        if (!isIdentByte(previous) && previous != '.')
            break;
        --start;
    }
    if (start == end)
        return {};
    return std::string(source.substr(start, end - start));
}

/// @brief Compare an identifier with a symbol's full or final dotted name.
/// @param sym Semantic symbol.
/// @param ident Unqualified identifier text.
/// @return True when the full name or its final component equals @p ident.
bool symbolNameMatchesIdentifier(const Symbol &sym, std::string_view ident) {
    if (sym.name == ident)
        return true;
    size_t dot = sym.name.rfind('.');
    return dot != std::string::npos && std::string_view(sym.name).substr(dot + 1) == ident;
}

/// @brief Resolve a qualified identifier as an export of a bound file module.
/// @param sema Completed semantic analyzer.
/// @param source Full source buffer.
/// @param identStart Byte offset of the exported identifier.
/// @param ident Exported identifier text.
/// @return Matching symbol copy, or no value.
std::optional<Symbol> moduleExportSymbolAt(const Sema &sema,
                                           std::string_view source,
                                           size_t identStart,
                                           std::string_view ident) {
    std::string moduleName = qualifierBeforeIdentifier(source, identStart);
    if (moduleName.empty())
        return std::nullopt;

    for (const auto &sym : sema.getModuleExports(moduleName)) {
        if (symbolNameMatchesIdentifier(sym, ident))
            return sym;
    }
    return std::nullopt;
}

/// @brief Split a dotted identifier while discarding empty components.
/// @param text Dotted source text.
/// @return Non-empty components in source order.
std::vector<std::string> splitDottedIdentifier(std::string_view text) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : text) {
        if (c == '.') {
            if (!part.empty())
                parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty())
        parts.push_back(part);
    return parts;
}

/// @brief Resolve a qualified identifier as a runtime class member.
/// @param sema Completed semantic analyzer.
/// @param source Full source buffer.
/// @param identStart Byte offset of the member identifier.
/// @param ident Member identifier text.
/// @return Matching runtime symbol copy, or no value.
std::optional<Symbol> runtimeMemberSymbolAt(const Sema &sema,
                                            std::string_view source,
                                            size_t identStart,
                                            std::string_view ident) {
    std::string qualifier = qualifierBeforeIdentifier(source, identStart);
    if (qualifier.empty())
        return std::nullopt;

    auto parts = splitDottedIdentifier(qualifier);
    if (parts.empty())
        return std::nullopt;

    std::string className = sema.resolveModuleAlias(parts[0]);
    if (!className.empty()) {
        for (size_t i = 1; i < parts.size(); ++i)
            className += "." + parts[i];
    } else {
        className = qualifier;
    }

    for (const auto &sym : sema.getRuntimeMembers(className)) {
        if (symbolNameMatchesIdentifier(sym, ident))
            return sym;
    }
    return std::nullopt;
}

/// @brief Build hover text for a qualified runtime class identifier.
/// @param sema Completed semantic analyzer used to expand module aliases.
/// @param source Full source buffer.
/// @param identStart Byte offset of the class identifier.
/// @param ident Unqualified class identifier.
/// @return Class display plus runtime catalog documentation, or an empty string.
static std::string runtimeClassHoverAt(const Sema &sema,
                                       std::string_view source,
                                       size_t identStart,
                                       std::string_view ident) {
    std::string qualifier = dottedQualifierBeforeIdentifier(source, identStart);
    if (qualifier.empty())
        return {};

    auto parts = splitDottedIdentifier(qualifier);
    if (parts.empty())
        return {};

    std::string className = sema.resolveModuleAlias(parts.front());
    if (!className.empty()) {
        for (size_t i = 1; i < parts.size(); ++i)
            className += "." + parts[i];
    } else {
        className = qualifier;
    }
    className += "." + std::string(ident);

    const auto *runtimeClass = il::runtime::findRuntimeClassByQName(className);
    if (!runtimeClass)
        return {};

    std::string hover = "class " + std::string(ident) + ": " + className;
    if (runtimeClass->summary && *runtimeClass->summary)
        hover += "\n" + std::string(runtimeClass->summary);
    if (runtimeClass->details && *runtimeClass->details)
        hover += "\n\n" + std::string(runtimeClass->details);
    return hover;
}

/// @brief Convert a semantic symbol category to its hover-display keyword.
/// @param sym Symbol whose kind is inspected.
/// @return Stable short display label.
std::string hoverKindForSymbol(const Symbol &sym) {
    switch (sym.kind) {
        case Symbol::Kind::Variable:
            return "var";
        case Symbol::Kind::Parameter:
            return "param";
        case Symbol::Kind::Function:
            return "func";
        case Symbol::Kind::Method:
            return "method";
        case Symbol::Kind::Field:
            return "field";
        case Symbol::Kind::Type:
            return "type";
        case Symbol::Kind::Module:
            return "module";
        default:
            return "symbol";
    }
}

/// @brief Format type, mutability, and documentation for a resolved symbol.
/// @param sm Source manager used for declaration-comment lookup.
/// @param sym Semantic symbol.
/// @param displayName Identifier spelling under the cursor.
/// @return Newline-delimited hover display.
std::string formatHoverForSymbol(const il::support::SourceManager &sm,
                                 const Symbol &sym,
                                 std::string_view displayName) {
    std::string typeStr = sym.type ? sym.type->toDisplayString() : "unknown";
    std::string hover = hoverKindForSymbol(sym) + " " + std::string(displayName) + ": " + typeStr;
    if (sym.isFinal)
        hover += " (final)";
    std::string doc = documentationForSymbolLoc(sm, sym);
    if (!doc.empty())
        hover += "\n" + doc;
    return hover;
}

/// @brief Count top-level commas in the call containing a cursor.
/// @param source Full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Zero-based active parameter index; zero when no call is found.
int activeParameterForSource(std::string_view source, int line, int col) {
    if (source.empty())
        return 0;
    if (line < 1)
        line = 1;
    if (col < 0)
        col = 0;

    size_t cursor = 0;
    int curLine = 1;
    while (cursor < source.size() && curLine < line) {
        if (source[cursor] == '\n')
            ++curLine;
        ++cursor;
    }
    size_t lineStart = cursor;
    while (cursor < source.size() && source[cursor] != '\n')
        ++cursor;
    size_t at = lineStart + (col < 0 ? 0 : static_cast<size_t>(col));
    if (at > cursor)
        at = cursor;

    int depth = 0;
    size_t open = std::string_view::npos;
    for (size_t i = at; i > 0; --i) {
        char c = source[i - 1];
        if (c == ')')
            ++depth;
        else if (c == '(') {
            if (depth == 0) {
                open = i - 1;
                break;
            }
            --depth;
        }
    }
    if (open == std::string_view::npos)
        return 0;

    int param = 0;
    int nestedParen = 0;
    int nestedBracket = 0;
    int nestedBrace = 0;
    for (size_t i = open + 1; i < at && i < source.size(); ++i) {
        char c = source[i];
        if (c == '(')
            ++nestedParen;
        else if (c == ')' && nestedParen > 0)
            --nestedParen;
        else if (c == '[')
            ++nestedBracket;
        else if (c == ']' && nestedBracket > 0)
            --nestedBracket;
        else if (c == '{')
            ++nestedBrace;
        else if (c == '}' && nestedBrace > 0)
            --nestedBrace;
        else if (c == ',' && nestedParen == 0 && nestedBracket == 0 && nestedBrace == 0)
            ++param;
    }
    return param;
}

/// @brief Split a parameter list on commas outside nested delimiters.
/// @param params Text between a signature's outer parentheses.
/// @return Trimmed non-empty parameter strings.
std::vector<std::string> splitTopLevelParams(std::string_view params) {
    std::vector<std::string> out;
    int nestedParen = 0;
    int nestedBracket = 0;
    int nestedBrace = 0;
    size_t start = 0;
    for (size_t i = 0; i < params.size(); ++i) {
        char c = params[i];
        if (c == '(')
            ++nestedParen;
        else if (c == ')' && nestedParen > 0)
            --nestedParen;
        else if (c == '[')
            ++nestedBracket;
        else if (c == ']' && nestedBracket > 0)
            --nestedBracket;
        else if (c == '{')
            ++nestedBrace;
        else if (c == '}' && nestedBrace > 0)
            --nestedBrace;
        else if (c == ',' && nestedParen == 0 && nestedBracket == 0 && nestedBrace == 0) {
            out.push_back(trimAscii(params.substr(start, i - start)));
            start = i + 1;
        }
    }
    std::string tail = trimAscii(params.substr(start));
    if (!tail.empty())
        out.push_back(std::move(tail));
    return out;
}

/// @brief Parse a display signature's parameters into structured runtime records.
/// @param signatureLine Single-line callable signature.
/// @return Owned sequence of maps with name, type, and documentation fields.
void *signatureParametersToSeq(std::string_view signatureLine) {
    void *seq = rt_seq_new_owned();
    size_t open = signatureLine.find('(');
    size_t close = signatureLine.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
        return seq;

    auto params = splitTopLevelParams(signatureLine.substr(open + 1, close - open - 1));
    for (const auto &paramText : params) {
        if (paramText.empty())
            continue;
        void *param = rt_map_new();
        size_t colon = paramText.find(':');
        if (colon == std::string::npos) {
            mapSetStr(param, "name", paramText);
            mapSetStr(param, "type", "");
        } else {
            mapSetStr(param, "name", trimAscii(std::string_view(paramText).substr(0, colon)));
            mapSetStr(param, "type", trimAscii(std::string_view(paramText).substr(colon + 1)));
        }
        mapSetStr(param, "documentation", "");
        rt_seq_push(seq, param);
        releaseRuntimeObject(param);
    }
    return seq;
}

/// @brief Build the structured signature-help result map.
/// @param display Raw engine signature and documentation display.
/// @param source Source used for active-parameter and documentation fallback.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @param documentation Precomputed documentation, if any.
/// @param computeDocumentation Whether to scan source declarations as a fallback.
/// @return Owned runtime signature-info map.
void *signatureInfoMap(std::string_view display,
                       std::string_view source,
                       int line,
                       int col,
                       std::string_view documentation,
                       bool computeDocumentation) {
    void *map = rt_map_new();
    bool available = !display.empty();
    mapSetBool(map, "available", available);
    std::vector<std::string> overloads =
        available ? splitSignatureDisplays(display) : std::vector<std::string>{};
    if (available && overloads.empty())
        overloads.push_back(std::string(display));
    std::string activeDisplay = overloads.empty() ? std::string() : overloads.front();
    std::string firstLine = firstDisplayLine(activeDisplay);
    mapSetStr(map, "display", firstLine);
    mapSetInt(map, "activeParameter", available ? activeParameterForSource(source, line, col) : 0);
    mapSetInt(map, "activeSignature", 0);
    mapSetInt(map, "overloadCount", static_cast<int64_t>(overloads.size()));
    std::string name = signatureNameFromDisplay(firstLine);
    mapSetStr(map, "name", name);
    size_t arrow = firstLine.find("->");
    std::string returnType = arrow == std::string::npos
                                 ? std::string()
                                 : trimAscii(std::string_view(firstLine).substr(arrow + 2));
    mapSetStr(map, "returnType", returnType);
    void *params = signatureParametersToSeq(firstLine);
    mapSetObject(map, "parameters", params);
    releaseRuntimeObject(params);
    void *overloadSeq = signatureOverloadsToSeq(overloads);
    mapSetObject(map, "overloads", overloadSeq);
    releaseRuntimeObject(overloadSeq);
    std::string doc(documentation);
    if (doc.empty())
        doc = documentationFromDisplayTail(activeDisplay);
    if (computeDocumentation && doc.empty())
        doc = declarationDocumentationForName(source, name);
    mapSetStr(map, "documentation", doc);
    mapSetStr(map, "source", "zia");
    return map;
}

/// @brief Build the structured hover result map.
/// @param display Raw `kind name: type` hover display.
/// @param source Source used for declaration-documentation fallback.
/// @param documentation Precomputed documentation, if any.
/// @param computeDocumentation Whether to scan source declarations as a fallback.
/// @return Owned runtime hover-info map.
void *hoverInfoMap(std::string_view display,
                   std::string_view source,
                   std::string_view documentation,
                   bool computeDocumentation) {
    void *map = rt_map_new();
    bool available = !display.empty();
    mapSetBool(map, "available", available);
    std::string firstLine = firstDisplayLine(display);
    mapSetStr(map, "display", firstLine);

    std::string text = firstLine;
    std::string kind;
    std::string name;
    std::string type;
    size_t firstSpace = text.find(' ');
    size_t colon = text.find(':');
    if (firstSpace != std::string::npos)
        kind = trimAscii(std::string_view(text).substr(0, firstSpace));
    if (firstSpace != std::string::npos && colon != std::string::npos && colon > firstSpace)
        name = trimAscii(std::string_view(text).substr(firstSpace + 1, colon - firstSpace - 1));
    if (colon != std::string::npos)
        type = trimAscii(std::string_view(text).substr(colon + 1));
    mapSetStr(map, "kind", kind);
    mapSetStr(map, "name", name);
    mapSetStr(map, "type", type);
    mapSetStr(map, "title", name.empty() ? text : name);
    std::string doc(documentation);
    if (doc.empty())
        doc = documentationFromDisplayTail(display);
    if (computeDocumentation && doc.empty())
        doc = declarationDocumentationForName(source, name);
    mapSetStr(map, "documentation", doc);
    mapSetStr(map, "source", "zia");
    return map;
}

/// @brief Analyze source and snapshot its diagnostics for an asynchronous job.
/// @param source Full source buffer.
/// @param path Virtual source path.
/// @return Value records safe to retain outside the compiler/source-manager lifetime.
std::vector<DiagnosticRecord> diagnosticRecordsForAnalysis(const AnalysisResult &result,
                                                           const il::support::SourceManager &sm,
                                                           const std::string &path) {
    const std::string sourcePath = sourcePathForFile(result.fileId, sm, path);
    std::vector<DiagnosticRecord> records;
    records.reserve(result.diagnostics.diagnostics().size());
    for (const auto &diagnostic : result.diagnostics.diagnostics())
        records.push_back(diagnosticToRecord(diagnostic, sm, sourcePath));
    return records;
}

/// @brief Analyze source and snapshot its diagnostics for an asynchronous job.
/// @param source Full source buffer.
/// @param path Virtual source path.
/// @return Value records safe to retain outside the compiler/source-manager lifetime.
std::vector<DiagnosticRecord> diagnosticRecordsForSource(const std::string &source,
                                                         const std::string &path) {
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    return result ? diagnosticRecordsForAnalysis(*result, sm, path)
                  : std::vector<DiagnosticRecord>{};
}

/// @brief Compute hover display for the identifier at an editor position.
/// @param source Full source buffer.
/// @param path Virtual source path.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Formatted local, bound-export, runtime-class, or runtime-member hover.
std::string hoverForSource(const std::string &source,
                           const std::string &path,
                           int64_t line,
                           int64_t col) {
    int lineIdx = (int)line - 1;
    int colIdx = (int)col;
    size_t pos = 0;
    int curLine = 0;
    while (curLine < lineIdx && pos < source.size()) {
        if (source[pos] == '\n')
            ++curLine;
        ++pos;
    }
    pos += (size_t)colIdx;
    if (pos >= source.size())
        return {};

    size_t start = pos;
    while (start > 0 && isIdentByte(source[start - 1]))
        --start;
    size_t end = pos;
    while (end < source.size() && isIdentByte(source[end]))
        ++end;
    if (start == end)
        return {};
    std::string ident = source.substr(start, end - start);

    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    if (!result || !result->sema)
        return {};

    const ScopedSymbol *scoped = result->sema->findSymbolAtPosition(
        ident, result->fileId, static_cast<uint32_t>(line), static_cast<uint32_t>(col + 1));
    if (scoped)
        return formatHoverForSymbol(sm, scoped->symbol, ident);

    if (auto exportSym = moduleExportSymbolAt(*result->sema, source, start, ident))
        return formatHoverForSymbol(sm, *exportSym, ident);

    if (std::string runtimeClassHover = runtimeClassHoverAt(*result->sema, source, start, ident);
        !runtimeClassHover.empty()) {
        return runtimeClassHover;
    }

    if (auto runtimeSym = runtimeMemberSymbolAt(*result->sema, source, start, ident))
        return formatHoverForSymbol(sm, *runtimeSym, ident);

    return {};
}

/// @brief Classify resolved identifiers for the editor's semantic-highlight overlay.
/// @param source Full source buffer.
/// @param path Virtual source path.
/// @return Tab-separated `line start end kind` rows in zero-based editor coordinates.
/// @details This performs compiler-only work suitable for a worker thread.
///          Unresolved identifiers are omitted so lexical highlighting remains visible.
std::string tokensForAnalysis(const std::string &source, const AnalysisResult &result) {
    if (!result.sema)
        return {};

    std::ostringstream out;
    for (const auto &token : lexIdentifierTokens(source, result.fileId)) {
        const ScopedSymbol *scoped = result.sema->findSymbolAtPosition(
            token.text, result.fileId, token.loc.line, token.loc.column);
        if (!scoped)
            continue;
        std::string kind = symbolKindName(scoped->symbol.kind);
        int line0 = token.loc.line > 0 ? static_cast<int>(token.loc.line) - 1 : 0;
        int start0 = token.loc.column > 0 ? static_cast<int>(token.loc.column) - 1 : 0;
        int end0 = token.endColumn > 0 ? static_cast<int>(token.endColumn) - 1 : 0;
        out << line0 << '\t' << start0 << '\t' << end0 << '\t' << kind << '\n';
    }
    return out.str();
}

/// @brief Analyze and classify resolved identifiers for the editor overlay.
/// @param source Full source buffer.
/// @param path Virtual source path.
/// @return Tab-separated semantic-token rows.
std::string tokensForSource(const std::string &source, const std::string &path) {
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};
    input.fileId = sm.addFile(path);
    auto result = parseAndAnalyze(input, opts, sm);
    return result ? tokensForAnalysis(source, *result) : std::string{};
}

/// @brief Collect document-local global symbols for the editor outline.
/// @param source Full source buffer.
/// @param path Virtual source path.
/// @return Tab-separated `name kind type line` rows.
std::string symbolsForAnalysis(const AnalysisResult &result) {
    if (!result.sema)
        return {};

    std::ostringstream out;
    auto globals = result.sema->getGlobalSymbols();
    for (const auto &sym : globals) {
        // Document symbols cover THIS file only (VDOC-110): runtime-registry
        // entries have no declaration and imported exports carry a foreign
        // file id; both would otherwise leak registry-sized payloads and
        // wrong-document outline locations into the four-field protocol.
        if (!sym.decl || sym.decl->loc.file_id != result.fileId)
            continue;
        std::string kindStr;
        switch (sym.kind) {
            case Symbol::Kind::Variable:
                kindStr = "variable";
                break;
            case Symbol::Kind::Function:
                kindStr = "function";
                break;
            case Symbol::Kind::Type:
                kindStr = "type";
                break;
            case Symbol::Kind::Module:
                kindStr = "module";
                break;
            default:
                kindStr = "symbol";
                break;
        }
        std::string typeStr = sym.type ? sym.type->toDisplayString() : "";
        int symLine = sym.decl ? (int)sym.decl->loc.line : 0;
        out << sym.name << '\t' << kindStr << '\t' << typeStr << '\t' << symLine << '\n';
    }

    // Type names are not appended separately: user-declared types already
    // appear in the filtered global symbols, and registry types are not part
    // of this document (VDOC-110).

    return out.str();
}

/// @brief Analyze and collect document-local global symbols.
/// @param source Full source buffer.
/// @param path Virtual source path.
/// @return Tab-separated document-symbol rows.
std::string symbolsForSource(const std::string &source, const std::string &path) {
    il::support::SourceManager sm;
    CompilerInput input{.source = source, .path = path};
    CompilerOptions opts{};
    input.fileId = sm.addFile(path);
    auto result = parseAndAnalyze(input, opts, sm);
    return result ? symbolsForAnalysis(*result) : std::string{};
}

/// @brief Start a detached, capacity-limited semantic worker job.
/// @tparam Worker Callable accepting `SemanticJob&`.
/// @param kind Result schema the worker will populate.
/// @param worker Native-only computation captured by the detached thread.
/// @return Owned runtime job handle, or nullptr when handle allocation fails.
template <typename Worker> void *startSemanticJob(SemanticJobKind kind, Worker worker) {
    auto job = std::make_shared<SemanticJob>(kind);
    void *handle = semanticJobHandleFor(job);
    if (!handle)
        return nullptr;

    if (!tryAcquireSemanticWorkerSlot()) {
        completeSemanticJobWithError(job, "semantic worker pool busy");
        return handle;
    }

    try {
        /// @brief Executes a semantic job and publishes completion or failure.
        std::thread([job, worker = std::move(worker)]() mutable {
            try {
                if (!job->cancelled.load(std::memory_order_acquire))
                    worker(*job);
            } catch (const std::exception &ex) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->error = ex.what();
            } catch (...) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->error = "unknown semantic job failure";
            }
            finishSemanticJob(job);
        }).detach();
    } catch (const std::exception &ex) {
        releaseSemanticWorkerSlot();
        completeSemanticJobWithError(job, ex.what());
    } catch (...) {
        releaseSemanticWorkerSlot();
        completeSemanticJobWithError(job, "failed to start semantic job");
    }
    return handle;
}
} // namespace

extern "C" {
rt_string rt_zia_complete_for_file(rt_string source,
                                   rt_string file_path,
                                   int64_t line,
                                   int64_t col);
void *rt_zia_completion_items_for_file(rt_string source,
                                       rt_string file_path,
                                       int64_t line,
                                       int64_t col);
rt_string rt_zia_signature_help_for_file(rt_string source,
                                         rt_string file_path,
                                         int64_t line,
                                         int64_t col);
void *rt_zia_signature_info_for_file(rt_string source,
                                     rt_string file_path,
                                     int64_t line,
                                     int64_t col);
rt_string rt_zia_check_for_file(rt_string source, rt_string file_path);
rt_string rt_zia_hover_for_file(rt_string source, rt_string file_path, int64_t line, int64_t col);
void *rt_zia_hover_info_for_file(rt_string source, rt_string file_path, int64_t line, int64_t col);
rt_string rt_zia_symbols_for_file(rt_string source, rt_string file_path);
void *rt_zia_toolchain_check_for_file(rt_string source, rt_string file_path);
void *rt_zia_toolchain_compile_for_file(rt_string source, rt_string file_path);
void *rt_zia_completion_begin_items_for_file(rt_string source,
                                             rt_string file_path,
                                             int64_t line,
                                             int64_t col);
void *rt_zia_completion_begin_signature_info_for_file(rt_string source,
                                                      rt_string file_path,
                                                      int64_t line,
                                                      int64_t col);
void *rt_zia_completion_begin_hover_info_for_file(rt_string source,
                                                  rt_string file_path,
                                                  int64_t line,
                                                  int64_t col);
void *rt_zia_completion_begin_symbols_for_file(rt_string source, rt_string file_path);
void *rt_zia_completion_begin_tokens_for_file(rt_string source, rt_string file_path);
void *rt_zia_completion_begin_analysis_for_file(rt_string source, rt_string file_path);
void *rt_zia_toolchain_begin_check_for_file(rt_string source, rt_string file_path);
int8_t rt_zia_semantic_job_is_done(void *handle);
int8_t rt_zia_semantic_job_is_error(void *handle);
rt_string rt_zia_semantic_job_error(void *handle);
void *rt_zia_semantic_job_error_option(void *handle);
int64_t rt_zia_semantic_job_kind(void *handle);
void rt_zia_semantic_job_cancel(void *handle);
void *rt_zia_semantic_job_completion_items(void *handle);
void *rt_zia_semantic_job_signature_info(void *handle);
void *rt_zia_semantic_job_hover_info(void *handle);
rt_string rt_zia_semantic_job_symbols(void *handle);
rt_string rt_zia_semantic_job_tokens(void *handle);
void *rt_zia_semantic_job_diagnostics(void *handle);
void *rt_zia_project_index_new(rt_string root);
int8_t rt_zia_project_index_is_valid(void *handle);
int8_t rt_zia_project_index_update_file(void *handle, rt_string file_path, rt_string source);
int8_t rt_zia_project_index_remove_file(void *handle, rt_string file_path);
void rt_zia_project_index_clear(void *handle);
void rt_zia_project_index_destroy(void *handle);
void *rt_zia_project_index_definition(
    void *handle, rt_string file_path, rt_string source, int64_t line, int64_t col);
void *rt_zia_project_index_references(
    void *handle, rt_string file_path, rt_string source, int64_t line, int64_t col);
void *rt_zia_project_index_rename_edits(void *handle,
                                        rt_string file_path,
                                        rt_string source,
                                        int64_t line,
                                        int64_t col,
                                        rt_string new_name);

/// @brief Runtime entry point: code completion at (@p line, @p col) in
///        @p source. Returns the serialized completion item list as an
///        rt_string (the editor/LSP bridge consumes this).
/// @param source Borrowed runtime string containing the full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned serialized completion string, empty if completion throws.
rt_string rt_zia_complete(rt_string source, int64_t line, int64_t col) {
    // Never let a frontend exception unwind across the extern "C" boundary into
    // the editor/LSP host (that is undefined behavior); degrade to no results.
    try {
        std::string sourceStr = toStdString(source);

        std::vector<CompletionItem> items;
        {
            std::lock_guard<std::mutex> lock(s_engineMutex);
            items = s_engine.complete(sourceStr, (int)line, (int)col);
        }
        std::string result = serialize(items);

        return rt_string_from_bytes(result.c_str(), result.size());
    } catch (...) {
        return rt_string_from_bytes("", 0);
    }
}

/// @brief As @ref rt_zia_complete but with an explicit @p file_path so
///        cross-file/module-aware completions resolve correctly.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned serialized completion string, empty if completion throws.
rt_string rt_zia_complete_for_file(rt_string source,
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
        std::string result = serialize(items);

        return rt_string_from_bytes(result.c_str(), result.size());
    } catch (...) {
        return rt_string_from_bytes("", 0);
    }
}

/// @brief Runtime entry point: structured completion items at (@p line, @p col)
///        in @p source. Returns a Seq<Map> with stable item fields.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned runtime sequence, or nullptr if completion throws.
void *rt_zia_completion_items(rt_string source, int64_t line, int64_t col) {
    try {
        std::string sourceStr = toStdString(source);
        std::vector<CompletionItem> items;
        {
            std::lock_guard<std::mutex> lock(s_engineMutex);
            items = s_engine.complete(sourceStr, (int)line, (int)col);
        }
        return completionItemsToSeq(items);
    } catch (...) {
        return nullptr;
    }
}

/// @brief As @ref rt_zia_completion_items but with an explicit @p file_path so
///        relative binds and project-aware source locations can be resolved.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned runtime sequence, or nullptr if completion throws.
void *rt_zia_completion_items_for_file(rt_string source,
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
        return nullptr;
    }
}

/// @brief Runtime entry point: signature help at (@p line, @p col) with no
///        file context (delegates to @ref rt_zia_signature_help_for_file).
/// @param source Borrowed runtime string containing the full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned signature display string, empty when no call resolves.
rt_string rt_zia_signature_help(rt_string source, int64_t line, int64_t col) {
    return rt_zia_signature_help_for_file(source, nullptr, line, col);
}

/// @brief Runtime entry point: return call signature text for the invocation
///        active at (@p line, @p col), or an empty string if none resolves.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned signature display string.
rt_string rt_zia_signature_help_for_file(rt_string source,
                                         rt_string file_path,
                                         int64_t line,
                                         int64_t col) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);

    std::string result;
    {
        std::lock_guard<std::mutex> lock(s_engineMutex);
        result = s_engine.signatureHelp(sourceStr, (int)line, (int)col, pathStr);
    }
    return rt_string_from_bytes(result.c_str(), result.size());
}

/// @brief Runtime entry point: structured signature help at (@p line, @p col)
///        with no file context.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned structured signature-info map.
void *rt_zia_signature_info(rt_string source, int64_t line, int64_t col) {
    return rt_zia_signature_info_for_file(source, nullptr, line, col);
}

/// @brief Runtime entry point: structured signature help map for the active
///        invocation at (@p line, @p col).
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned structured signature-info map, with `available` false if unresolved.
void *rt_zia_signature_info_for_file(rt_string source,
                                     rt_string file_path,
                                     int64_t line,
                                     int64_t col) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);
    std::string result;
    {
        std::lock_guard<std::mutex> lock(s_engineMutex);
        result = s_engine.signatureHelp(sourceStr, (int)line, (int)col, pathStr);
    }
    return signatureInfoMap(result, sourceStr, (int)line, (int)col, "", true);
}

/// @brief Runtime entry point: drop the completion engine's cached analysis
///        (call after the project/files change materially).
void rt_zia_completion_clear_cache(void) {
    std::lock_guard<std::mutex> lock(s_engineMutex);
    s_engine.clearCache();
}

// =========================================================================
// Check — run semantic analysis and return serialized diagnostics.
// Format: severity\tline\tcol\tcode\tmessage\n  (one per diagnostic)
// severity: 0=error, 1=warning, 2=note
// =========================================================================
/// @brief Runtime entry point: check @p source with no file context
///        (delegates to @ref rt_zia_check_for_file).
/// @param source Borrowed runtime string containing the full source buffer.
/// @return Owned tab-separated diagnostic string.
rt_string rt_zia_check(rt_string source) {
    return rt_zia_check_for_file(source, nullptr);
}

/// @brief Runtime entry point: run lex/parse/sema on @p source (named
///        @p file_path) and return serialized diagnostics, one per line as
///        `severity\tline\tcol\tcode\tmessage` (0=error,1=warning,2=note).
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @return Owned serialized diagnostic string, empty when no result is produced.
rt_string rt_zia_check_for_file(rt_string source, rt_string file_path) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);

    il::support::SourceManager sm;
    CompilerInput input{.source = sourceStr, .path = pathStr};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    if (!result)
        return rt_string_from_bytes("", 0);

    std::ostringstream out;
    for (const auto &d : result->diagnostics.diagnostics()) {
        int sev = 0;
        if (d.severity == il::support::Severity::Warning)
            sev = 1;
        else if (d.severity == il::support::Severity::Note)
            sev = 2;
        out << sev << '\t' << d.loc.line << '\t' << d.loc.column << '\t' << d.code << '\t'
            << d.message << '\n';
    }
    std::string s = out.str();
    return rt_string_from_bytes(s.c_str(), s.size());
}

// =========================================================================
// Incremental document mirror sync (plan 08)
// =========================================================================

/// @brief Replace the mirror for @p path with @p text at @p revision.
/// @param path Borrowed non-empty document key.
/// @param text Borrowed full document text.
/// @param revision Revision represented by @p text.
void rt_zia_doc_sync_full(rt_string path, rt_string text, int64_t revision) {
    std::string p = toStdString(path);
    if (p.empty() || revision < 0)
        return;
    std::string body = toStdString(text);
    if (body.size() > kMaxMirrorBytes)
        return;
    std::lock_guard<std::mutex> lock(s_mirrorMutex);
    DocumentMirror &m = s_mirrors[p];
    m.text = std::move(body);
    m.revision = static_cast<uint64_t>(revision);
}

/// @brief Apply @p deltas_json to the mirror for @p path, advancing to
///        @p end_revision.
/// @param path Borrowed non-empty document key.
/// @param deltas_json Borrowed compact journal array.
/// @param end_revision Revision that the complete batch should produce.
/// @return `1` on success; `0` for no baseline, stale revisions, or malformed deltas.
int8_t rt_zia_doc_sync_delta(rt_string path, rt_string deltas_json, int64_t end_revision) {
    std::string p = toStdString(path);
    if (p.empty())
        return 0;
    std::string json = toStdString(deltas_json);
    std::lock_guard<std::mutex> lock(s_mirrorMutex);
    auto it = s_mirrors.find(p);
    if (it == s_mirrors.end())
        return 0;
    // A delta batch must move the mirror forward (VDOC-109); stale or
    // backwards end revisions force the caller's full-sync path.
    if (end_revision < 0 || static_cast<uint64_t>(end_revision) <= it->second.revision)
        return 0;
    if (!mirrorApplyDeltasJson(
            it->second.text, json, it->second.revision, static_cast<uint64_t>(end_revision)))
        return 0;
    it->second.revision = static_cast<uint64_t>(end_revision);
    return 1;
}

/// @brief Whether the full editor-service bridge is linked (VDOC-113).
/// @details Lets callers distinguish "analysis ran and found nothing" from
///          "the weak stub returned an empty compatibility payload".
/// @return Always `1` for this strong editor-services implementation.
int8_t rt_zia_service_available(void) {
    return 1;
}

/// @brief Whether a mirror exists for @p path (VDOC-113), independent of the
///        mirrored text being empty.
/// @param path Borrowed document key.
/// @return `1` when the mirror table contains the key; otherwise `0`.
int8_t rt_zia_doc_has(rt_string path) {
    std::string p = toStdString(path);
    if (p.empty())
        return 0;
    std::lock_guard<std::mutex> lock(s_mirrorMutex);
    return s_mirrors.find(p) != s_mirrors.end() ? 1 : 0;
}

/// @brief Drop the mirror for @p path (document closed).
/// @param path Borrowed document key; absent keys are ignored.
void rt_zia_doc_close(rt_string path) {
    std::string p = toStdString(path);
    std::lock_guard<std::mutex> lock(s_mirrorMutex);
    s_mirrors.erase(p);
}

/// @brief Return the current mirror text for @p path, or "" when absent.
/// @param path Borrowed document key.
/// @return Owned copy of the mirror text, or an empty runtime string.
rt_string rt_zia_doc_text(rt_string path) {
    std::string out;
    if (!mirrorTextFor(toStdString(path), out))
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(out.c_str(), out.size());
}

/// @brief Diagnostics for @p file_path sourced from its mirror (no text param),
///        serialized like rt_zia_check_for_file. Returns "" when no mirror exists
///        (the caller should full-sync and retry).
/// @param file_path Borrowed mirror key and source path.
/// @return Owned tab-separated diagnostic string.
rt_string rt_zia_check_for_file_mirror(rt_string file_path) {
    std::string sourceStr;
    if (!mirrorTextFor(toStdString(file_path), sourceStr))
        return rt_string_from_bytes("", 0);
    std::string pathStr = editorPathOrDefault(file_path);

    il::support::SourceManager sm;
    CompilerInput input{.source = sourceStr, .path = pathStr};
    CompilerOptions opts{};
    auto result = parseAndAnalyze(input, opts, sm);
    if (!result)
        return rt_string_from_bytes("", 0);

    std::ostringstream out;
    for (const auto &d : result->diagnostics.diagnostics()) {
        int sev = 0;
        if (d.severity == il::support::Severity::Warning)
            sev = 1;
        else if (d.severity == il::support::Severity::Note)
            sev = 2;
        out << sev << '\t' << d.loc.line << '\t' << d.loc.column << '\t' << d.code << '\t'
            << d.message << '\n';
    }
    std::string s = out.str();
    return rt_string_from_bytes(s.c_str(), s.size());
}

/// @brief Async structured diagnostics for @p file_path sourced from its mirror
///        text (no source argument crosses the ABI on each check).
/// @details Reuses the shared semantic-job worker + diagnosticRecordsForSource,
///          so the result is the same Seq<Map> shape the editor already consumes
///          via SemanticJob.Diagnostics. Returns null when no mirror exists for
///          @p file_path — the caller should full-sync and retry.
/// @param file_path Borrowed mirror key and source path.
/// @return Owned asynchronous job handle, or nullptr when the mirror is absent.
void *rt_zia_doc_begin_check_for_file(rt_string file_path) {
    std::string sourceStr;
    if (!mirrorTextFor(toStdString(file_path), sourceStr))
        return nullptr;
    std::string pathStr = editorPathOrDefault(file_path);
    return startSemanticJob(
        SemanticJobKind::Diagnostics,
        /// @brief Computes mirrored-document diagnostics into a semantic job.
        /// @param job Job result storage.
        [sourceStr = std::move(sourceStr), pathStr = std::move(pathStr)](SemanticJob &job) {
            auto diagnostics = diagnosticRecordsForSource(sourceStr, pathStr);
            std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.cancelled.load(std::memory_order_acquire))
                job.diagnostics = std::move(diagnostics);
        });
}

// =========================================================================
// Structured Toolchain - in-process diagnostics and compile results.
// =========================================================================
/// @brief Runtime entry point: return structured diagnostics for @p source.
/// @details Each diagnostic is a Zanna.Collections.Map with file, line,
///          column, endLine, endColumn, severity, severityName, code, message,
///          stage, and help fields.
/// @param source Borrowed runtime string containing the full source buffer.
/// @return Owned sequence of structured diagnostics.
void *rt_zia_toolchain_check(rt_string source) {
    return rt_zia_toolchain_check_for_file(source, nullptr);
}

/// @brief Runtime entry point: path-aware structured semantic diagnostics.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @return Owned sequence of structured diagnostics.
void *rt_zia_toolchain_check_for_file(rt_string source, rt_string file_path) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);

    il::support::SourceManager sm;
    CompilerInput input{.source = sourceStr, .path = pathStr};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    const std::string sourcePath =
        result ? sourcePathForFile(result->fileId, sm, pathStr) : pathStr;
    return result ? diagnosticsToSeq(result->diagnostics, sm, sourcePath) : rt_seq_new_owned();
}

/// @brief Runtime entry point: compile @p source to IL and return a structured
///        result map.
/// @param source Borrowed runtime string containing the full source buffer.
/// @return Owned compile-result map.
void *rt_zia_toolchain_compile(rt_string source) {
    return rt_zia_toolchain_compile_for_file(source, nullptr);
}

/// @brief Runtime entry point: path-aware compile result for IDE tooling.
/// @details Returns a Zanna.Collections.Map with success, diagnostics,
///          sourcePath, outputPath, and il fields. `diagnostics` is always a
///          Seq of diagnostic maps, and invalid code returns the diagnostics
///          without requiring string parsing.
/// @param source Borrowed runtime string containing the full source buffer.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @return Owned compile-result map with serialized IL only on success.
void *rt_zia_toolchain_compile_for_file(rt_string source, rt_string file_path) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);

    il::support::SourceManager sm;
    CompilerInput input{.source = sourceStr, .path = pathStr};
    CompilerOptions opts{};
    CompilerResult result = compile(input, opts, sm);

    const std::string sourcePath = sourcePathForFile(result.fileId, sm, pathStr);
    void *diagnostics = diagnosticsToSeq(result.diagnostics, sm, sourcePath);

    void *map = rt_map_new();
    mapSetBool(map, "success", result.succeeded());
    mapSetStr(map, "sourcePath", sourcePath);
    mapSetStr(map, "outputPath", "");
    mapSetObject(map, "diagnostics", diagnostics);
    releaseRuntimeObject(diagnostics);

    std::string ilText;
    if (result.succeeded())
        ilText = il::io::Serializer::toString(result.module);
    mapSetStr(map, "il", ilText);

    return map;
}

// =========================================================================
// Async semantic jobs — worker threads compute into native C++ records; the
// UI thread later materializes Zanna runtime maps/sequences by polling results.
// =========================================================================
/// @brief Begin asynchronous path-aware completion-item computation.
/// @param source Borrowed runtime source text, copied before the worker starts.
/// @param file_path Borrowed source path, copied before the worker starts.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned semantic-job handle, or nullptr when handle allocation fails.
void *rt_zia_completion_begin_items_for_file(rt_string source,
                                             rt_string file_path,
                                             int64_t line,
                                             int64_t col) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);
    return startSemanticJob(
        SemanticJobKind::CompletionItems,
        /// @brief Computes completion items into a semantic job.
        /// @param job Job result storage.
        [sourceStr = std::move(sourceStr), pathStr = std::move(pathStr), line, col](
            SemanticJob &job) {
            CompletionEngine engine;
            auto items = engine.complete(sourceStr, (int)line, (int)col, pathStr);
            std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.cancelled.load(std::memory_order_acquire))
                job.completionItems = std::move(items);
        });
}

/// @brief Begin asynchronous path-aware signature-help computation.
/// @param source Borrowed runtime source text, copied before the worker starts.
/// @param file_path Borrowed source path, copied before the worker starts.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned semantic-job handle, or nullptr when handle allocation fails.
void *rt_zia_completion_begin_signature_info_for_file(rt_string source,
                                                      rt_string file_path,
                                                      int64_t line,
                                                      int64_t col) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);
    return startSemanticJob(
        SemanticJobKind::SignatureInfo,
        /// @brief Computes signature help into a semantic job.
        /// @param job Job result storage.
        [sourceStr = std::move(sourceStr), pathStr = std::move(pathStr), line, col](
            SemanticJob &job) {
            CompletionEngine engine;
            std::string display = engine.signatureHelp(sourceStr, (int)line, (int)col, pathStr);
            std::string documentation =
                declarationDocumentationForName(sourceStr, signatureNameFromDisplay(display));
            std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.cancelled.load(std::memory_order_acquire)) {
                job.signatureDisplay = std::move(display);
                job.signatureSource = sourceStr;
                job.signatureDocumentation = std::move(documentation);
                job.signatureLine = (int)line;
                job.signatureCol = (int)col;
            }
        });
}

/// @brief Begin asynchronous path-aware hover computation.
/// @param source Borrowed runtime source text, copied before the worker starts.
/// @param file_path Borrowed source path, copied before the worker starts.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned semantic-job handle, or nullptr when handle allocation fails.
void *rt_zia_completion_begin_hover_info_for_file(rt_string source,
                                                  rt_string file_path,
                                                  int64_t line,
                                                  int64_t col) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);
    return startSemanticJob(
        SemanticJobKind::HoverInfo,
        /// @brief Computes hover information into a semantic job.
        /// @param job Job result storage.
        [sourceStr = std::move(sourceStr), pathStr = std::move(pathStr), line, col](
            SemanticJob &job) {
            std::string display = hoverForSource(sourceStr, pathStr, line, col);
            std::string documentation =
                declarationDocumentationForName(sourceStr, hoverNameFromDisplay(display));
            std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.cancelled.load(std::memory_order_acquire)) {
                job.hoverDisplay = std::move(display);
                job.hoverDocumentation = std::move(documentation);
            }
        });
}

/// @brief Begin asynchronous document-symbol collection.
/// @param source Borrowed runtime source text, copied before the worker starts.
/// @param file_path Borrowed source path, copied before the worker starts.
/// @return Owned semantic-job handle, or nullptr when handle allocation fails.
void *rt_zia_completion_begin_symbols_for_file(rt_string source, rt_string file_path) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);
    return startSemanticJob(
        SemanticJobKind::Symbols,
        /// @brief Computes document symbols into a semantic job.
        /// @param job Job result storage.
        [sourceStr = std::move(sourceStr), pathStr = std::move(pathStr)](SemanticJob &job) {
            std::string symbols = symbolsForSource(sourceStr, pathStr);
            std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.cancelled.load(std::memory_order_acquire))
                job.symbols = std::move(symbols);
        });
}

/// @brief Begin asynchronous semantic-token classification.
/// @param source Borrowed runtime source text, copied before the worker starts.
/// @param file_path Borrowed source path, copied before the worker starts.
/// @return Owned semantic-job handle, or nullptr when handle allocation fails.
void *rt_zia_completion_begin_tokens_for_file(rt_string source, rt_string file_path) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);
    return startSemanticJob(
        SemanticJobKind::Tokens,
        /// @brief Computes semantic tokens into a semantic job.
        /// @param job Job result storage.
        [sourceStr = std::move(sourceStr), pathStr = std::move(pathStr)](SemanticJob &job) {
            std::string tokens = tokensForSource(sourceStr, pathStr);
            std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.cancelled.load(std::memory_order_acquire))
                job.tokens = std::move(tokens);
        });
}

/// @brief Begin one asynchronous analysis that publishes symbols, tokens, and diagnostics.
/// @details The source is parsed and semantically analyzed exactly once. Consumers may read all
///          three result channels from the completed Analysis job, avoiding independent compiler
///          passes for the same editor revision.
/// @param source Borrowed runtime source text, copied before the worker starts.
/// @param file_path Borrowed source path, copied before the worker starts.
/// @return Owned semantic-job handle, or nullptr when handle allocation fails.
void *rt_zia_completion_begin_analysis_for_file(rt_string source, rt_string file_path) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);
    return startSemanticJob(
        SemanticJobKind::Analysis,
        /// @brief Computes the shared revision analysis into one semantic job.
        /// @param job Job result storage.
        [sourceStr = std::move(sourceStr), pathStr = std::move(pathStr)](SemanticJob &job) {
            il::support::SourceManager sm;
            CompilerInput input{.source = sourceStr, .path = pathStr};
            CompilerOptions opts{};
            input.fileId = sm.addFile(pathStr);
            auto result = parseAndAnalyze(input, opts, sm);
            if (!result)
                return;

            std::string symbols = symbolsForAnalysis(*result);
            std::string tokens = tokensForAnalysis(sourceStr, *result);
            auto diagnostics = diagnosticRecordsForAnalysis(*result, sm, pathStr);
            std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.cancelled.load(std::memory_order_acquire)) {
                job.symbols = std::move(symbols);
                job.tokens = std::move(tokens);
                job.diagnostics = std::move(diagnostics);
            }
        });
}

/// @brief Begin asynchronous structured diagnostic analysis.
/// @param source Borrowed runtime source text, copied before the worker starts.
/// @param file_path Borrowed source path, copied before the worker starts.
/// @return Owned semantic-job handle, or nullptr when handle allocation fails.
void *rt_zia_toolchain_begin_check_for_file(rt_string source, rt_string file_path) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);
    return startSemanticJob(
        SemanticJobKind::Diagnostics,
        /// @brief Computes structured diagnostics into a semantic job.
        /// @param job Job result storage.
        [sourceStr = std::move(sourceStr), pathStr = std::move(pathStr)](SemanticJob &job) {
            auto diagnostics = diagnosticRecordsForSource(sourceStr, pathStr);
            std::lock_guard<std::mutex> lock(job.mutex);
            if (!job.cancelled.load(std::memory_order_acquire))
                job.diagnostics = std::move(diagnostics);
        });
}

/// @brief Query whether an asynchronous semantic job has published completion.
/// @param handle Opaque semantic-job handle.
/// @return `1` for a completed valid job; otherwise `0`.
int8_t rt_zia_semantic_job_is_done(void *handle) {
    auto job = asSemanticJob(handle);
    return job && job->done.load(std::memory_order_acquire) ? 1 : 0;
}

/// @brief Query whether a completed semantic job contains an error.
/// @param handle Opaque semantic-job handle.
/// @return `1` only for a completed valid job with non-empty error text.
int8_t rt_zia_semantic_job_is_error(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job || !job->done.load(std::memory_order_acquire))
        return 0;
    std::lock_guard<std::mutex> lock(job->mutex);
    return job->error.empty() ? 0 : 1;
}

/// @brief Copy the current semantic-job error text.
/// @param handle Opaque semantic-job handle.
/// @return Owned error string, empty for invalid handles or no error.
rt_string rt_zia_semantic_job_error(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job)
        return rt_str_empty();
    std::lock_guard<std::mutex> lock(job->mutex);
    return toRtString(job->error);
}

/// @brief Return the semantic job error text as a Zanna.Option string.
/// @details Completed jobs with a non-empty error payload return
///          `SomeStr(message)`. Jobs without an error, jobs that have not
///          produced an error yet, and invalid/null handles return `None`.
///          This keeps routine absence of an error out of the legacy empty
///          string side channel while preserving `rt_zia_semantic_job_error`
///          for compatibility.
/// @param handle Opaque `SemanticJobHandle` object returned by a begin-job API.
/// @return Owned `Zanna.Option` containing the error string when present.
void *rt_zia_semantic_job_error_option(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job)
        return rt_option_none();
    std::lock_guard<std::mutex> lock(job->mutex);
    if (job->error.empty())
        return rt_option_none();
    rt_string message = toRtString(job->error);
    void *option = rt_option_some_str(message);
    rt_str_release_maybe(message);
    return option;
}

/// @brief Identify the result schema produced by a semantic job.
/// @param handle Opaque semantic-job handle.
/// @return Numeric SemanticJobKind value, or zero for an invalid handle.
int64_t rt_zia_semantic_job_kind(void *handle) {
    auto job = asSemanticJob(handle);
    return job ? static_cast<int64_t>(job->kind) : 0;
}

/// @brief Request cancellation of an asynchronous semantic job.
/// @param handle Opaque semantic-job handle; invalid handles are ignored.
/// @details Cancellation suppresses result publication but does not forcibly stop
///          compiler work already in progress.
void rt_zia_semantic_job_cancel(void *handle) {
    auto job = asSemanticJob(handle);
    if (job)
        job->cancelled.store(true, std::memory_order_release);
}

/// @brief Materialize completion items from a finished matching job.
/// @param handle Opaque semantic-job handle.
/// @return Owned completion sequence, empty until ready or on kind/error mismatch.
void *rt_zia_semantic_job_completion_items(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job || !job->done.load(std::memory_order_acquire) ||
        job->kind != SemanticJobKind::CompletionItems)
        return rt_seq_new_owned();
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->error.empty())
        return rt_seq_new_owned();
    return completionItemsToSeq(job->completionItems);
}

/// @brief Materialize structured signature help from a finished matching job.
/// @param handle Opaque semantic-job handle.
/// @return Owned signature map; unavailable until ready or on kind/error mismatch.
void *rt_zia_semantic_job_signature_info(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job || !job->done.load(std::memory_order_acquire) ||
        job->kind != SemanticJobKind::SignatureInfo)
        return signatureInfoMap("", "", 1, 0, "", false);
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->error.empty())
        return signatureInfoMap("", "", 1, 0, "", false);
    return signatureInfoMap(job->signatureDisplay,
                            job->signatureSource,
                            job->signatureLine,
                            job->signatureCol,
                            job->signatureDocumentation,
                            false);
}

/// @brief Materialize structured hover data from a finished matching job.
/// @param handle Opaque semantic-job handle.
/// @return Owned hover map; unavailable until ready or on kind/error mismatch.
void *rt_zia_semantic_job_hover_info(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job || !job->done.load(std::memory_order_acquire) ||
        job->kind != SemanticJobKind::HoverInfo)
        return hoverInfoMap("", "", "", false);
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->error.empty())
        return hoverInfoMap("", "", "", false);
    return hoverInfoMap(job->hoverDisplay, "", job->hoverDocumentation, false);
}

/// @brief Copy document-symbol rows from a finished matching job.
/// @param handle Opaque semantic-job handle.
/// @return Owned serialized symbol string, empty until ready or on mismatch/error.
rt_string rt_zia_semantic_job_symbols(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job || !job->done.load(std::memory_order_acquire) ||
        (job->kind != SemanticJobKind::Symbols && job->kind != SemanticJobKind::Analysis))
        return rt_str_empty();
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->error.empty())
        return rt_str_empty();
    return toRtString(job->symbols);
}

/// @brief Copy semantic-token rows from a finished matching job.
/// @param handle Opaque semantic-job handle.
/// @return Owned serialized token string, empty until ready or on mismatch/error.
rt_string rt_zia_semantic_job_tokens(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job || !job->done.load(std::memory_order_acquire) ||
        (job->kind != SemanticJobKind::Tokens && job->kind != SemanticJobKind::Analysis))
        return rt_str_empty();
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->error.empty())
        return rt_str_empty();
    return toRtString(job->tokens);
}

/// @brief Materialize diagnostics from a finished matching job.
/// @param handle Opaque semantic-job handle.
/// @return Owned diagnostic sequence, empty until ready or on mismatch/error.
void *rt_zia_semantic_job_diagnostics(void *handle) {
    auto job = asSemanticJob(handle);
    if (!job || !job->done.load(std::memory_order_acquire) ||
        (job->kind != SemanticJobKind::Diagnostics && job->kind != SemanticJobKind::Analysis))
        return rt_seq_new_owned();
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->error.empty())
        return rt_seq_new_owned();
    return diagnosticRecordsToSeq(job->diagnostics);
}

// =========================================================================
// ProjectIndex — project-wide semantic definition/reference/rename queries.
// =========================================================================
/// @brief Create an empty project index rooted at a caller-supplied path.
/// @param root Borrowed project root; empty text resolves from the current directory.
/// @return Owned finalizable project-index object, or nullptr on allocation failure.
void *rt_zia_project_index_new(rt_string root) {
    std::string rootPath = normalizeProjectRoot(toStdString(root));
    auto *handle = static_cast<ProjectIndexHandle *>(
        rt_obj_new_i64(kProjectIndexClassId, sizeof(ProjectIndexHandle)));
    if (!handle)
        return nullptr;
    handle->mutex = new std::mutex();
    handle->index = new ProjectIndex(std::move(rootPath));
    rt_obj_set_finalizer(handle, projectIndexFinalize);
    return handle;
}

/// @brief Test whether an opaque handle still owns a live project index.
/// @param handle Candidate project-index object.
/// @return `1` for a live valid index; otherwise `0`.
int8_t rt_zia_project_index_is_valid(void *handle) {
    ProjectIndexHandle *typed = asProjectIndexHandle(handle);
    if (!typed || !typed->mutex)
        return 0;
    std::lock_guard<std::mutex> lock(*typed->mutex);
    return typed->index ? 1 : 0;
}

/// @brief Insert or replace a source snapshot in a project index.
/// @param handle Opaque project-index object.
/// @param file_path Borrowed absolute or root-relative source path.
/// @param source Borrowed full source text copied into shared native storage.
/// @return `1` on success; `0` for an invalid or destroyed index.
int8_t rt_zia_project_index_update_file(void *handle, rt_string file_path, rt_string source) {
    ProjectIndexHandle *typed = asProjectIndexHandle(handle);
    if (!typed || !typed->mutex)
        return 0;
    std::lock_guard<std::mutex> lock(*typed->mutex);
    ProjectIndex *index = typed->index;
    if (!index)
        return 0;
    std::string normalizedPath = normalizeProjectPath(*index, editorPathOrDefault(file_path));
    index->sources[normalizedPath] =
        IndexedSource{normalizedPath, std::make_shared<const std::string>(toStdString(source))};
    return 1;
}

/// @brief Remove a source snapshot from a project index.
/// @param handle Opaque project-index object.
/// @param file_path Borrowed absolute or root-relative source path.
/// @return `1` when an entry was removed; otherwise `0`.
int8_t rt_zia_project_index_remove_file(void *handle, rt_string file_path) {
    ProjectIndexHandle *typed = asProjectIndexHandle(handle);
    if (!typed || !typed->mutex)
        return 0;
    std::lock_guard<std::mutex> lock(*typed->mutex);
    ProjectIndex *index = typed->index;
    if (!index)
        return 0;
    std::string normalizedPath = normalizeProjectPath(*index, editorPathOrDefault(file_path));
    return index->sources.erase(normalizedPath) != 0 ? 1 : 0;
}

/// @brief Remove every indexed source while preserving the index root.
/// @param handle Opaque project-index object; invalid handles are ignored.
void rt_zia_project_index_clear(void *handle) {
    ProjectIndexHandle *typed = asProjectIndexHandle(handle);
    if (!typed || !typed->mutex)
        return;
    std::lock_guard<std::mutex> lock(*typed->mutex);
    ProjectIndex *index = typed->index;
    if (!index)
        return;
    index->sources.clear();
}

/// @brief Destroy native index contents before the runtime wrapper is released.
/// @param handle Opaque project-index object; repeated calls are harmless.
void rt_zia_project_index_destroy(void *handle) {
    ProjectIndexHandle *typed = asProjectIndexHandle(handle);
    if (!typed || !typed->mutex)
        return;
    std::lock_guard<std::mutex> lock(*typed->mutex);
    delete typed->index;
    typed->index = nullptr;
}

/// @brief Resolve the definition of the symbol at a source position.
/// @param handle Opaque project-index object.
/// @param file_path Borrowed primary source path.
/// @param source Borrowed current primary-source snapshot, overriding its indexed copy.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned definition map with a `found` field and either range or reason data.
void *rt_zia_project_index_definition(
    void *handle, rt_string file_path, rt_string source, int64_t line, int64_t col) {
    std::unique_ptr<ProjectIndex> index = snapshotProjectIndex(handle);
    if (!index)
        return notFoundMap("invalid_index");

    std::string path = normalizeProjectPath(*index, editorPathOrDefault(file_path));
    std::string sourceStr = toStdString(source);
    index->sources[path] = IndexedSource{path, std::make_shared<const std::string>(sourceStr)};

    auto key = resolveAtPosition(*index, path, sourceStr, line, col);
    if (!key)
        return notFoundMap("not_found");
    return definitionMapForKey(*index, *key);
}

/// @brief Find project-wide references to the symbol at a source position.
/// @param handle Opaque project-index object.
/// @param file_path Borrowed primary source path.
/// @param source Borrowed current primary-source snapshot, overriding its indexed copy.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned sequence of reference maps, empty when unresolved or invalid.
void *rt_zia_project_index_references(
    void *handle, rt_string file_path, rt_string source, int64_t line, int64_t col) {
    std::unique_ptr<ProjectIndex> index = snapshotProjectIndex(handle);
    if (!index)
        return rt_seq_new_owned();

    std::string path = normalizeProjectPath(*index, editorPathOrDefault(file_path));
    std::string sourceStr = toStdString(source);
    index->sources[path] = IndexedSource{path, std::make_shared<const std::string>(sourceStr)};

    auto key = resolveAtPosition(*index, path, sourceStr, line, col);
    if (!key)
        return rt_seq_new_owned();
    return referencesForKey(*index, *key, kMaxProjectQueryReferences + 1);
}

/// @brief Compute validated project-wide edits for renaming a resolved symbol.
/// @param handle Opaque project-index object.
/// @param file_path Borrowed primary source path.
/// @param source Borrowed current primary-source snapshot.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @param new_name Borrowed proposed identifier.
/// @return Owned result map containing success/reason and reference/edit sequences.
void *rt_zia_project_index_rename_edits(void *handle,
                                        rt_string file_path,
                                        rt_string source,
                                        int64_t line,
                                        int64_t col,
                                        rt_string new_name) {
    std::unique_ptr<ProjectIndex> index = snapshotProjectIndex(handle);
    if (!index)
        return renameFailureMap("invalid_index");

    const std::string newName = toStdString(new_name);
    if (!isIdentifierText(newName))
        return renameFailureMap("invalid_name");

    std::string path = normalizeProjectPath(*index, editorPathOrDefault(file_path));
    std::string sourceStr = toStdString(source);
    index->sources[path] = IndexedSource{path, std::make_shared<const std::string>(sourceStr)};

    auto key = resolveAtPosition(*index, path, sourceStr, line, col);
    if (!key)
        return renameFailureMap("not_found");

    void *references = referencesForKey(*index, *key, kMaxProjectQueryReferences + 1);
    if (rt_seq_len(references) > kMaxProjectQueryReferences) {
        releaseRuntimeObject(references);
        return renameFailureMap("reference_limit");
    }
    if (renameWouldCollide(*index, *key, references, newName)) {
        releaseRuntimeObject(references);
        return renameFailureMap("collision");
    }

    void *edits = rt_seq_new_owned();
    const int64_t count = rt_seq_len(references);
    for (int64_t i = 0; i < count; ++i) {
        void *refMap = rt_seq_get(references, i);
        rt_string fileKey = toRtString("file");
        rt_string lineKey = toRtString("line");
        rt_string columnKey = toRtString("column");
        rt_string endLineKey = toRtString("endLine");
        rt_string endColumnKey = toRtString("endColumn");

        rt_string fileValue = rt_map_get_str(refMap, fileKey);
        const int64_t startLine = rt_map_get_int(refMap, lineKey);
        const int64_t startColumn = rt_map_get_int(refMap, columnKey);
        const int64_t endLine = rt_map_get_int(refMap, endLineKey);
        const int64_t endColumn = rt_map_get_int(refMap, endColumnKey);

        void *edit = rt_map_new();
        mapSetStr(edit, "file", toStdString(fileValue));
        mapSetInt(edit, "startLine", startLine);
        mapSetInt(edit, "startColumn", startColumn);
        mapSetInt(edit, "endLine", endLine);
        mapSetInt(edit, "endColumn", endColumn);
        mapSetInt(edit, "editorStartLine", startLine > 0 ? startLine - 1 : 0);
        mapSetInt(edit, "editorStartColumn", startColumn > 0 ? startColumn - 1 : 0);
        mapSetInt(edit, "editorEndLine", endLine > 0 ? endLine - 1 : 0);
        mapSetInt(edit, "editorEndColumn", endColumn > 0 ? endColumn - 1 : 0);
        mapSetStr(edit, "expectedText", key->displayName);
        mapSetStr(edit, "newText", newName);
        rt_seq_push(edits, edit);
        releaseRuntimeObject(edit);

        rt_string_unref(fileValue);
        rt_string_unref(endColumnKey);
        rt_string_unref(endLineKey);
        rt_string_unref(columnKey);
        rt_string_unref(lineKey);
        rt_string_unref(fileKey);
    }

    void *result = rt_map_new();
    mapSetBool(result, "success", true);
    mapSetStr(result, "reason", "");
    mapSetStr(result, "name", key->displayName);
    mapSetStr(result, "newName", newName);
    mapSetObject(result, "references", references);
    mapSetObject(result, "edits", edits);
    releaseRuntimeObject(edits);
    releaseRuntimeObject(references);
    return result;
}

// =========================================================================
// Hover — return type/signature info for the identifier at (line, col).
// Returns empty string if nothing found; otherwise returns a human-readable
// string with the symbol kind and type.
// =========================================================================
/// @brief Runtime entry point: hover info at (@p line, @p col) with no file
///        context (delegates to @ref rt_zia_hover_for_file).
/// @param source Borrowed runtime source text.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned hover display string, empty when no symbol resolves.
rt_string rt_zia_hover(rt_string source, int64_t line, int64_t col) {
    return rt_zia_hover_for_file(source, nullptr, line, col);
}

/// @brief Runtime entry point: return a human-readable type/signature string
///        for the symbol at (@p line, @p col) in @p source / @p file_path,
///        or the empty string if nothing resolves there.
/// @param source Borrowed runtime source text.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned hover display string.
rt_string rt_zia_hover_for_file(rt_string source, rt_string file_path, int64_t line, int64_t col) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);

    // Extract identifier at cursor (simple backward scan)
    int lineIdx = (int)line - 1; // 0-based
    int colIdx = (int)col;       // 0-based
    size_t pos = 0;
    int curLine = 0;
    while (curLine < lineIdx && pos < sourceStr.size()) {
        if (sourceStr[pos] == '\n')
            ++curLine;
        ++pos;
    }
    pos += (size_t)colIdx;
    if (pos >= sourceStr.size())
        return rt_string_from_bytes("", 0);

    // Find word boundaries
    size_t start = pos;
    while (start > 0 && isIdentByte(sourceStr[start - 1]))
        --start;
    size_t end = pos;
    while (end < sourceStr.size() && isIdentByte(sourceStr[end]))
        ++end;
    if (start == end)
        return rt_string_from_bytes("", 0);
    std::string ident = sourceStr.substr(start, end - start);

    // Parse and analyze
    il::support::SourceManager sm;
    CompilerInput input{.source = sourceStr, .path = pathStr};
    CompilerOptions opts{};

    auto result = parseAndAnalyze(input, opts, sm);
    if (!result || !result->sema)
        return rt_string_from_bytes("", 0);

    // Resolve the identifier using the tooling-safe position query so hover
    // respects lexical scope instead of reaching into sema internals.
    const ScopedSymbol *scoped = result->sema->findSymbolAtPosition(
        ident, result->fileId, static_cast<uint32_t>(line), static_cast<uint32_t>(col + 1));
    std::string hover;
    if (scoped)
        hover = formatHoverForSymbol(sm, scoped->symbol, ident);
    else if (auto exportSym = moduleExportSymbolAt(*result->sema, sourceStr, start, ident))
        hover = formatHoverForSymbol(sm, *exportSym, ident);
    else if (std::string runtimeClassHover =
                 runtimeClassHoverAt(*result->sema, sourceStr, start, ident);
             !runtimeClassHover.empty())
        hover = std::move(runtimeClassHover);
    else if (auto runtimeSym = runtimeMemberSymbolAt(*result->sema, sourceStr, start, ident))
        hover = formatHoverForSymbol(sm, *runtimeSym, ident);
    else
        return rt_string_from_bytes("", 0);

    return rt_string_from_bytes(hover.c_str(), hover.size());
}

/// @brief Runtime entry point: structured hover info with no file context.
/// @param source Borrowed runtime source text.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned structured hover map.
void *rt_zia_hover_info(rt_string source, int64_t line, int64_t col) {
    return rt_zia_hover_info_for_file(source, nullptr, line, col);
}

/// @brief Runtime entry point: structured hover info for the identifier at
///        (@p line, @p col).
/// @param source Borrowed runtime source text.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @param line One-based cursor line.
/// @param col Zero-based cursor byte column.
/// @return Owned structured hover map, with `available` false when unresolved.
void *rt_zia_hover_info_for_file(rt_string source, rt_string file_path, int64_t line, int64_t col) {
    std::string sourceStr = toStdString(source);
    rt_string hover = rt_zia_hover_for_file(source, file_path, line, col);
    std::string hoverText = toStdString(hover);
    rt_string_unref(hover);
    return hoverInfoMap(hoverText, sourceStr, "", true);
}

// =========================================================================
// Symbols — return all top-level symbols in the source.
// Format: name\tkind\ttype\tline\n  (one per symbol)
// =========================================================================
/// @brief Runtime entry point: list symbols in @p source with no file context
///        (delegates to @ref rt_zia_symbols_for_file).
/// @param source Borrowed runtime source text.
/// @return Owned tab-separated document-symbol string.
rt_string rt_zia_symbols(rt_string source) {
    return rt_zia_symbols_for_file(source, nullptr);
}

/// @brief Runtime entry point: return all top-level symbols of @p source /
///        @p file_path, serialized one per line as `name\tkind\ttype\tline`.
/// @param source Borrowed runtime source text.
/// @param file_path Borrowed source path; null or empty selects `<editor>`.
/// @return Owned tab-separated document-symbol string.
rt_string rt_zia_symbols_for_file(rt_string source, rt_string file_path) {
    std::string sourceStr = toStdString(source);
    std::string pathStr = editorPathOrDefault(file_path);

    il::support::SourceManager sm;
    CompilerInput input{.source = sourceStr, .path = pathStr};
    CompilerOptions opts{};
    uint32_t fileId = sm.addFile(pathStr);
    input.fileId = fileId;

    auto result = parseAndAnalyze(input, opts, sm);
    if (!result || !result->sema)
        return rt_string_from_bytes("", 0);

    std::ostringstream out;
    auto globals = result->sema->getGlobalSymbols();
    for (const auto &sym : globals) {
        // Document symbols cover THIS file only (VDOC-110): runtime-registry
        // entries have no declaration and bound exports carry a foreign
        // file id; both would otherwise leak registry-sized payloads and
        // wrong-document outline locations into the four-field protocol.
        if (!sym.decl || sym.decl->loc.file_id != fileId)
            continue;
        std::string kindStr;
        switch (sym.kind) {
            case Symbol::Kind::Variable:
                kindStr = "variable";
                break;
            case Symbol::Kind::Function:
                kindStr = "function";
                break;
            case Symbol::Kind::Type:
                kindStr = "type";
                break;
            case Symbol::Kind::Module:
                kindStr = "module";
                break;
            default:
                kindStr = "symbol";
                break;
        }
        std::string typeStr = sym.type ? sym.type->toDisplayString() : "";
        int symLine = sym.decl ? (int)sym.decl->loc.line : 0;
        out << sym.name << '\t' << kindStr << '\t' << typeStr << '\t' << symLine << '\n';
    }

    // Type names are not appended separately (VDOC-110): user-declared types
    // already appear in the filtered global symbols above.

    std::string s = out.str();
    return rt_string_from_bytes(s.c_str(), s.size());
}

} // extern "C"
