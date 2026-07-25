//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_scene_editor.cpp
// Purpose: Scene-owned editable level documents, preserved rich sections,
//   imported-layout Tilemap reconstruction, diagnostics, and durable file I/O.
//
// Key invariants:
//   - Scene edits are bounds-checked and preserved-section input is size-bounded.
//   - Flood fill allocates its complete bounded work set before mutation.
//   - Imported Tilemap state is optional and malformed fields fall back safely.
//
// Ownership/Lifetime:
//   - Scene handles own C++ SceneState through their runtime finalizer.
//   - Temporary parsed runtime values are released before each C ABI return.
//
// Links: rt_scene_editor.h, rt_tiled_import.cpp,
//   docs/adr/0144-complete-tiled-map-import.md,
//   docs/adr/0174-scene-object-authoring-metadata-and-duplication.md,
//   docs/adr/0164-backward-compatible-2d-scene-object-hierarchy.md,
//   docs/adr/0171-bounded-scene-flood-fill-and-studio-tile-tools.md
//
//===----------------------------------------------------------------------===//

#include "rt_scene_editor.h"

#include "rt_box.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_tilemap.h"
#include "rt_trap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr int64_t kSceneVersion = 1;
constexpr int64_t kMaxJsonBytes = 16 * 1024 * 1024;
constexpr int64_t kMaxCellsPerLayer = 1024 * 1024;
constexpr int64_t kMaxTotalCells = 4 * 1024 * 1024;
constexpr int64_t kMaxLayers = 16;
constexpr int64_t kMaxObjects = 65536;
constexpr int64_t kMaxSceneProperties = 16384;
constexpr int64_t kMaxObjectProperties = 256;
constexpr size_t kMaxPropertyKeyBytes = 128;
constexpr size_t kMaxStringValueBytes = 64 * 1024;
constexpr size_t kMaxPreservedBytes = 4 * 1024 * 1024;
constexpr size_t kMaxDiagnostics = 256;
constexpr size_t kMaxTilemapLayerNameBytes = 31;
constexpr const char *kObjectParentProperty = "zanna.hierarchy.parentIndex";

std::string toStd(rt_string s) {
    if (!s)
        return {};
    const char *data = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!data || len <= 0)
        return {};
    return std::string(data, static_cast<size_t>(len));
}

rt_string makeString(const std::string &value) {
    return rt_string_from_bytes(value.data(), value.size());
}

void releaseObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

void releaseJsonValue(void *obj) {
    if (!obj)
        return;
    if (rt_string_is_handle(obj)) {
        rt_string_unref(static_cast<rt_string>(obj));
        return;
    }
    releaseObject(obj);
}

void mapSetStrField(void *map, const char *key, const std::string &value) {
    rt_string k = rt_const_cstr(key);
    rt_string v = makeString(value);
    rt_map_set_str(map, k, v);
    rt_string_unref(k);
    rt_string_unref(v);
}

void mapSetIntField(void *map, const char *key, int64_t value) {
    rt_string k = rt_const_cstr(key);
    rt_map_set_int(map, k, value);
    rt_string_unref(k);
}

bool isSeq(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_SEQ_CLASS_ID;
}

bool isMap(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_MAP_CLASS_ID;
}

std::string lowerAscii(std::string value) {
    for (char &ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string jsonEscape(const std::string &s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20)
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec << std::setfill(' ');
                else
                    out << static_cast<char>(ch);
                break;
        }
    }
    out << '"';
    return out.str();
}

std::string jsonNumber(double value) {
    if (!std::isfinite(value))
        return "null";
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

enum class ScalarKind {
    Null,
    Bool,
    Int,
    Float,
    String,
};

const char *scalarKindName(ScalarKind kind) {
    switch (kind) {
        case ScalarKind::Null:
            return "null";
        case ScalarKind::Bool:
            return "bool";
        case ScalarKind::Int:
            return "int";
        case ScalarKind::Float:
            return "float";
        case ScalarKind::String:
            return "string";
    }
    return "";
}

struct SceneScalar {
    ScalarKind kind{ScalarKind::Null};
    bool boolValue{false};
    int64_t intValue{0};
    double floatValue{0.0};
    std::string stringValue;
};

SceneScalar makeNullScalar() {
    return {};
}

SceneScalar makeBoolScalar(bool value) {
    SceneScalar out;
    out.kind = ScalarKind::Bool;
    out.boolValue = value;
    return out;
}

SceneScalar makeIntScalar(int64_t value) {
    SceneScalar out;
    out.kind = ScalarKind::Int;
    out.intValue = value;
    return out;
}

SceneScalar makeFloatScalar(double value) {
    SceneScalar out;
    out.kind = ScalarKind::Float;
    out.floatValue = value;
    return out;
}

SceneScalar makeStringScalar(const std::string &value) {
    SceneScalar out;
    out.kind = ScalarKind::String;
    out.stringValue = value;
    return out;
}

std::string scalarToJson(const SceneScalar &value) {
    switch (value.kind) {
        case ScalarKind::Null:
            return "null";
        case ScalarKind::Bool:
            return value.boolValue ? "true" : "false";
        case ScalarKind::Int:
            return std::to_string(value.intValue);
        case ScalarKind::Float:
            return jsonNumber(value.floatValue);
        case ScalarKind::String:
            return jsonEscape(value.stringValue);
    }
    return "null";
}

std::string scalarToString(const SceneScalar &value) {
    switch (value.kind) {
        case ScalarKind::Null:
            return "";
        case ScalarKind::Bool:
            return value.boolValue ? "true" : "false";
        case ScalarKind::Int:
            return std::to_string(value.intValue);
        case ScalarKind::Float:
            return jsonNumber(value.floatValue);
        case ScalarKind::String:
            return value.stringValue;
    }
    return "";
}

struct Layer {
    std::string name;
    std::string asset;
    bool visible{true};
    std::vector<int64_t> tiles;
};

struct Object {
    std::string type;
    std::string id;
    int64_t x{0};
    int64_t y{0};
    int64_t parent{-1};
    std::map<std::string, SceneScalar> properties;
};

struct Diagnostic {
    std::string code;
    std::string severity;
    std::string message;
    std::string path;
    int64_t line{0};
    int64_t column{0};
    std::string source;
};

struct PreservedSection {
    std::string key;
    std::string canonicalJson;
};

// Typed tile-behavior and camera/lighting section state (ADR 0176, ADR 0177).
// Loaders accept any int64 tile id so legacy files round-trip losslessly;
// setters enforce Tilemap's 1..4095 behavior-table addressability. Unknown
// members are retained as canonical JSON and re-emitted verbatim.
constexpr int64_t kMinBehaviorTileId = 1;
constexpr int64_t kMaxBehaviorTileId = 4095;
constexpr int64_t kMaxTilePropertyEntries = 4096;
constexpr int64_t kMaxTilePropertiesPerTile = 64;
constexpr int64_t kMaxAnimFramesPerRule = 4096;
constexpr int64_t kMaxTotalAnimFrames = 65536;
constexpr size_t kAutotileVariantCount = 16;

struct TileAnimation {
    std::vector<int64_t> frames;
    std::vector<int64_t> durations;
    std::map<std::string, std::string> unknown;
};

struct AutotileRule {
    std::array<int64_t, kAutotileVariantCount> variants{};
    std::map<std::string, std::string> unknown;
};

struct TilePropertySet {
    std::map<std::string, SceneScalar> values; // Int/Bool kinds only
    std::map<std::string, std::string> unknown;

    bool empty() const {
        return values.empty() && unknown.empty();
    }
};

struct TileBehaviorState {
    std::map<int64_t, int64_t> collision; // tile -> kind (1 solid, 2 one-way-up)
    int64_t collisionLayer{0};
    bool hasCollisionLayer{false};
    std::map<std::string, std::string> collisionUnknown;
    std::map<int64_t, TilePropertySet> tileProperties;
    std::map<std::string, std::string> tilePropertiesUnknown; // non-integer root keys
    std::map<int64_t, TileAnimation> animations;
    std::map<int64_t, AutotileRule> autotiles;

    bool collisionEmpty() const {
        return collision.empty() && !hasCollisionLayer && collisionUnknown.empty();
    }

    bool tilePropertiesEmpty() const {
        return tileProperties.empty() && tilePropertiesUnknown.empty();
    }
};

/// @brief Scene-global scalar section (camera, lighting): int/string fields only.
struct SectionScalarState {
    std::map<std::string, SceneScalar> fields;
    std::map<std::string, std::string> unknown;

    bool empty() const {
        return fields.empty() && unknown.empty();
    }
};

struct SceneState {
    int64_t version{kSceneVersion};
    std::string name;
    int64_t width{1};
    int64_t height{1};
    int64_t tileWidth{16};
    int64_t tileHeight{16};
    std::string tilesetAsset;
    std::vector<Layer> layers;
    std::vector<Object> objects;
    std::map<std::string, SceneScalar> properties;
    std::map<std::string, PreservedSection> preservedSections;
    TileBehaviorState behavior;
    SectionScalarState camera;
    SectionScalarState lighting;
    size_t typedUnknownBytes{0};
    std::vector<Diagnostic> diagnostics;
    std::string lastError;
    std::string sourcePath;
    bool valid{true};
};

struct SceneHandle {
    SceneState *state{nullptr};
};

SceneHandle *requireScene(void *scene) {
    if (!scene || rt_obj_class_id(scene) != RT_GAME_SCENE_CLASS_ID) {
        rt_trap("Game.Scene: invalid handle");
        return nullptr;
    }
    auto *h = static_cast<SceneHandle *>(scene);
    if (!h->state) {
        rt_trap("Game.Scene: destroyed handle");
        return nullptr;
    }
    return h;
}

void sceneFinalizer(void *obj) {
    auto *h = static_cast<SceneHandle *>(obj);
    delete h->state;
    h->state = nullptr;
}

bool hasErrors(const SceneState &s) {
    if (!s.valid)
        return true;
    for (const auto &diag : s.diagnostics) {
        if (diag.severity == "error")
            return true;
    }
    return false;
}

void addDiagnostic(SceneState &s,
                   std::string code,
                   std::string severity,
                   std::string message,
                   std::string path = {},
                   int64_t line = 0,
                   int64_t column = 0) {
    if (severity == "error")
        s.valid = false;
    if (s.diagnostics.size() >= kMaxDiagnostics) {
        if (s.diagnostics.size() == kMaxDiagnostics) {
            Diagnostic trunc;
            trunc.code = "scene.diagnostics.truncated";
            trunc.severity = "warning";
            trunc.message = "too many scene diagnostics; later diagnostics were dropped";
            trunc.source = s.sourcePath;
            s.lastError = trunc.message;
            s.diagnostics.back() = std::move(trunc);
        }
        return;
    }
    s.lastError = message;
    Diagnostic diag;
    diag.code = std::move(code);
    diag.severity = std::move(severity);
    diag.message = std::move(message);
    diag.path = std::move(path);
    diag.line = line;
    diag.column = column;
    diag.source = s.sourcePath;
    s.diagnostics.push_back(std::move(diag));
}

bool checkedCellCount(int64_t width, int64_t height, size_t &cells) {
    cells = 0;
    if (width <= 0 || height <= 0)
        return false;
    if (width > std::numeric_limits<int64_t>::max() / height)
        return false;
    int64_t product = width * height;
    if (product <= 0 || product > kMaxCellsPerLayer)
        return false;
    cells = static_cast<size_t>(product);
    return true;
}

bool validLayer(const SceneState &s, int64_t layer) {
    return layer >= 0 && layer < static_cast<int64_t>(s.layers.size());
}

bool validTile(const SceneState &s, int64_t x, int64_t y) {
    return x >= 0 && y >= 0 && x < s.width && y < s.height;
}

bool validObjectIndex(const SceneState &s, int64_t index) {
    return index >= 0 && index < static_cast<int64_t>(s.objects.size());
}

bool isReservedObjectProperty(const std::string &key) {
    return key == kObjectParentProperty;
}

bool isReservedObjectProperty(rt_string key) {
    return isReservedObjectProperty(toStd(key));
}

bool validKey(const std::string &key) {
    return key.size() <= kMaxPropertyKeyBytes;
}

bool validStringValue(const std::string &value) {
    return value.size() <= kMaxStringValueBytes;
}

bool validateScalarLimit(SceneState &s,
                         const std::string &key,
                         const SceneScalar &scalar,
                         const std::string &path) {
    if (!validKey(key)) {
        addDiagnostic(s,
                      "scene.schema.limit_exceeded",
                      "error",
                      "property key exceeds 128 bytes",
                      path + "/" + key);
        return false;
    }
    if (scalar.kind == ScalarKind::String && !validStringValue(scalar.stringValue)) {
        addDiagnostic(s,
                      "scene.schema.limit_exceeded",
                      "error",
                      "string property value exceeds 64 KiB",
                      path + "/" + key);
        return false;
    }
    return true;
}

Layer makeLayer(SceneState &s, const std::string &name) {
    size_t cells = 1;
    checkedCellCount(s.width, s.height, cells);
    Layer layer;
    layer.name = name.empty() ? "Layer" + std::to_string(s.layers.size()) : name;
    layer.tiles.assign(cells, 0);
    return layer;
}

void *handleFromState(SceneState state) {
    // Allocate the C++ state first: if this throws (e.g. bad_alloc) no GC handle
    // has been created yet, so the exception barrier cannot strand an unfinalized
    // runtime object (VDOC-256).
    auto *newState = new SceneState(std::move(state));
    auto *h =
        static_cast<SceneHandle *>(rt_obj_new_i64(RT_GAME_SCENE_CLASS_ID, sizeof(SceneHandle)));
    if (!h) {
        // The runtime allocation trap hook returned null instead of aborting; do not
        // dereference it, and release the state we already built.
        delete newState;
        return nullptr;
    }
    h->state = newState;
    rt_obj_set_finalizer(h, sceneFinalizer);
    return h;
}

SceneState makeBaseState(int64_t width, int64_t height, int64_t tileWidth, int64_t tileHeight) {
    SceneState s;
    size_t cells = 0;
    if (!checkedCellCount(width, height, cells)) {
        addDiagnostic(s,
                      "scene.schema.invalid_dimension",
                      "error",
                      "invalid scene dimensions; using a 1x1 invalid scene");
        s.width = 1;
        s.height = 1;
    } else {
        s.width = width;
        s.height = height;
    }
    s.tileWidth = tileWidth > 0 ? tileWidth : 16;
    s.tileHeight = tileHeight > 0 ? tileHeight : 16;
    s.layers.push_back(makeLayer(s, "base"));
    return s;
}

void *newSceneHandle(int64_t width, int64_t height, int64_t tileWidth, int64_t tileHeight) {
    return handleFromState(makeBaseState(width, height, tileWidth, tileHeight));
}

bool mapHas(void *map, const char *key) {
    if (!isMap(map))
        return false;
    rt_string k = rt_const_cstr(key);
    bool result = rt_map_has(map, k) != 0;
    rt_string_unref(k);
    return result;
}

void *mapGet(void *map, const char *key) {
    if (!isMap(map))
        return nullptr;
    rt_string k = rt_const_cstr(key);
    void *value = rt_map_get(map, k);
    rt_string_unref(k);
    return value;
}

std::string valueToString(void *value) {
    if (rt_string_is_handle(value))
        return toStd(static_cast<rt_string>(value));
    if (value && rt_box_type(value) == RT_BOX_STR) {
        rt_string s = rt_unbox_str(value);
        std::string out = toStd(s);
        rt_string_unref(s);
        return out;
    }
    return {};
}

bool jsonString(void *map, const char *key, std::string &out) {
    if (!mapHas(map, key))
        return false;
    void *value = mapGet(map, key);
    if (!rt_string_is_handle(value) && (!value || rt_box_type(value) != RT_BOX_STR))
        return false;
    out = valueToString(value);
    return true;
}

bool jsonNumberToInt(void *value, int64_t &out) {
    if (!value)
        return false;
    int64_t tag = rt_box_type(value);
    if (tag == RT_BOX_I64) {
        out = rt_unbox_i64(value);
        return true;
    }
    if (tag == RT_BOX_F64) {
        double d = rt_unbox_f64(value);
        if (!std::isfinite(d) || std::floor(d) != d ||
            d < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
            d > static_cast<double>(std::numeric_limits<int64_t>::max()))
            return false;
        out = static_cast<int64_t>(d);
        return true;
    }
    return false;
}

bool jsonInt(void *map, const char *key, int64_t &out) {
    if (!mapHas(map, key))
        return false;
    return jsonNumberToInt(mapGet(map, key), out);
}

bool jsonDouble(void *map, const char *key, double &out) {
    if (!mapHas(map, key))
        return false;
    void *value = mapGet(map, key);
    if (!value)
        return false;
    int64_t tag = rt_box_type(value);
    if (tag == RT_BOX_I64)
        out = static_cast<double>(rt_unbox_i64(value));
    else if (tag == RT_BOX_F64)
        out = rt_unbox_f64(value);
    else
        return false;
    return std::isfinite(out);
}

bool jsonBool(void *map, const char *key, bool &out, bool allowNumeric) {
    if (!mapHas(map, key))
        return false;
    void *value = mapGet(map, key);
    int64_t tag = value ? rt_box_type(value) : -1;
    if (tag == RT_BOX_I1) {
        out = rt_unbox_i1(value) != 0;
        return true;
    }
    if (allowNumeric) {
        int64_t n = 0;
        if (jsonNumberToInt(value, n) && (n == 0 || n == 1)) {
            out = n != 0;
            return true;
        }
    }
    return false;
}

bool parseScalarValue(void *value, SceneScalar &out) {
    if (!value) {
        out = makeNullScalar();
        return true;
    }
    if (rt_string_is_handle(value)) {
        out = makeStringScalar(toStd(static_cast<rt_string>(value)));
        return true;
    }
    int64_t tag = rt_box_type(value);
    if (tag == RT_BOX_I1) {
        out = makeBoolScalar(rt_unbox_i1(value) != 0);
        return true;
    }
    if (tag == RT_BOX_I64) {
        out = makeIntScalar(rt_unbox_i64(value));
        return true;
    }
    if (tag == RT_BOX_F64) {
        double d = rt_unbox_f64(value);
        if (!std::isfinite(d))
            return false;
        if (std::floor(d) == d && d >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            d <= static_cast<double>(std::numeric_limits<int64_t>::max()))
            out = makeIntScalar(static_cast<int64_t>(d));
        else
            out = makeFloatScalar(d);
        return true;
    }
    if (tag == RT_BOX_STR) {
        rt_string s = rt_unbox_str(value);
        out = makeStringScalar(toStd(s));
        rt_string_unref(s);
        return true;
    }
    return false;
}

bool parseScalarMap(SceneState &s,
                    void *map,
                    std::map<std::string, SceneScalar> &out,
                    int64_t maxEntries,
                    const std::string &path) {
    if (!isMap(map))
        return false;
    int64_t count = rt_map_len(map);
    if (count > maxEntries) {
        addDiagnostic(
            s, "scene.schema.limit_exceeded", "error", "too many scalar properties", path);
        return false;
    }
    void *keys = rt_map_keys(map);
    int64_t keysLen = rt_seq_len(keys);
    for (int64_t i = 0; i < keysLen; ++i) {
        rt_string keyStr = rt_seq_get_str(keys, i);
        std::string key = toStd(keyStr);
        void *value = rt_map_get(map, keyStr);
        rt_string_unref(keyStr);
        SceneScalar scalar;
        if (!parseScalarValue(value, scalar)) {
            addDiagnostic(s,
                          "scene.schema.invalid_type",
                          "error",
                          "property values must be scalar",
                          path + "/" + key);
            continue;
        }
        if (!validateScalarLimit(s, key, scalar, path))
            continue;
        out[key] = std::move(scalar);
    }
    releaseObject(keys);
    return true;
}

std::string formatRuntimeJsonValue(void *value) {
    if (!value)
        return "null";
    if (rt_string_is_handle(value))
        return jsonEscape(toStd(static_cast<rt_string>(value)));
    if (isSeq(value)) {
        std::ostringstream out;
        out << "[";
        int64_t len = rt_seq_len(value);
        for (int64_t i = 0; i < len; ++i) {
            if (i > 0)
                out << ",";
            out << formatRuntimeJsonValue(rt_seq_get(value, i));
        }
        out << "]";
        return out.str();
    }
    if (isMap(value)) {
        std::vector<std::string> keys;
        void *keySeq = rt_map_keys(value);
        int64_t len = rt_seq_len(keySeq);
        keys.reserve(static_cast<size_t>(len));
        for (int64_t i = 0; i < len; ++i) {
            rt_string keyStr = rt_seq_get_str(keySeq, i);
            keys.push_back(toStd(keyStr));
            rt_string_unref(keyStr);
        }
        releaseObject(keySeq);
        std::sort(keys.begin(), keys.end());

        std::ostringstream out;
        out << "{";
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i > 0)
                out << ",";
            rt_string keyStr = makeString(keys[i]);
            void *child = rt_map_get(value, keyStr);
            rt_string_unref(keyStr);
            out << jsonEscape(keys[i]) << ":" << formatRuntimeJsonValue(child);
        }
        out << "}";
        return out.str();
    }

    int64_t tag = rt_box_type(value);
    if (tag == RT_BOX_I1)
        return rt_unbox_i1(value) ? "true" : "false";
    if (tag == RT_BOX_I64)
        return std::to_string(rt_unbox_i64(value));
    if (tag == RT_BOX_F64)
        return jsonNumber(rt_unbox_f64(value));
    if (tag == RT_BOX_STR) {
        rt_string str = rt_unbox_str(value);
        std::string out = jsonEscape(toStd(str));
        rt_string_unref(str);
        return out;
    }
    return "null";
}

std::string formatRuntimeJson(void *value) {
    return formatRuntimeJsonValue(value);
}

bool isKnownRichSection(const std::string &key) {
    return key == "camera" || key == "lighting" || key == "collision" || key == "tileProperties" ||
           key == "animations" || key == "autotiles";
}

bool isReservedTopLevelKey(const std::string &key) {
    return key == "version" || key == "name" || key == "width" || key == "height" ||
           key == "tileWidth" || key == "tileHeight" || key == "tilesetAsset" ||
           key == "properties" || key == "layers" || key == "objects" || key == "tilemap";
}

size_t preservedSectionBytes(const SceneState &s) {
    size_t total = s.typedUnknownBytes;
    for (const auto &[_, section] : s.preservedSections)
        total += section.canonicalJson.size();
    return total;
}

/// @brief Reserve budget for one retained unknown section member (ADR 0176).
bool reserveTypedUnknown(SceneState &s, const std::string &path, const std::string &json) {
    if (preservedSectionBytes(s) + json.size() > kMaxPreservedBytes) {
        addDiagnostic(s,
                      "scene.schema.limit_exceeded",
                      "error",
                      "preserved scene sections exceed 4 MiB",
                      path);
        return false;
    }
    s.typedUnknownBytes += json.size();
    return true;
}

void preserveSection(SceneState &s, const std::string &key, void *value) {
    std::string json = formatRuntimeJson(value);
    size_t total = json.size() + s.typedUnknownBytes;
    for (const auto &[_, section] : s.preservedSections)
        total += section.canonicalJson.size();
    if (total > kMaxPreservedBytes) {
        addDiagnostic(s,
                      "scene.schema.limit_exceeded",
                      "error",
                      "preserved scene sections exceed 4 MiB",
                      "/" + key);
        return;
    }
    s.preservedSections[key] = PreservedSection{key, std::move(json)};
}

bool strictIntegerKey(const std::string &key, int64_t &out) {
    if (key.empty())
        return false;
    try {
        size_t pos = 0;
        out = std::stoll(key, &pos);
        return pos == key.size();
    } catch (...) {
        return false;
    }
}

void retainUnknownMember(SceneState &s,
                         std::map<std::string, std::string> &unknown,
                         const std::string &path,
                         const std::string &key,
                         void *value) {
    std::string json = formatRuntimeJson(value);
    if (reserveTypedUnknown(s, path + "/" + key, json))
        unknown[key] = std::move(json);
}

void parseCollisionSection(SceneState &s, void *root) {
    void *keys = rt_map_keys(root);
    int64_t count = rt_seq_len(keys);
    for (int64_t i = 0; i < count; ++i) {
        rt_string keyStr = rt_seq_get_str(keys, i);
        std::string key = toStd(keyStr);
        void *value = rt_map_get(root, keyStr);
        rt_string_unref(keyStr);
        if (key == "layer") {
            int64_t layer = 0;
            if (jsonNumberToInt(value, layer)) {
                s.behavior.collisionLayer = layer;
                s.behavior.hasCollisionLayer = true;
            } else {
                addDiagnostic(s,
                              "scene.schema.invalid_value",
                              "warning",
                              "collision layer must be an integer",
                              "/collision/layer");
            }
            continue;
        }
        if (key == "solid" || key == "oneWayUp") {
            if (!isSeq(value)) {
                addDiagnostic(s,
                              "scene.schema.invalid_value",
                              "warning",
                              "collision tile list must be an array",
                              "/collision/" + key);
                continue;
            }
            int64_t kind = key == "solid" ? RT_TILE_COLLISION_SOLID : RT_TILE_COLLISION_ONE_WAY_UP;
            int64_t len = rt_seq_len(value);
            for (int64_t ti = 0; ti < len; ++ti) {
                int64_t tile = 0;
                if (jsonNumberToInt(rt_seq_get(value, ti), tile))
                    s.behavior.collision[tile] = kind;
                else
                    addDiagnostic(s,
                                  "scene.schema.invalid_value",
                                  "warning",
                                  "collision tile id must be an integer",
                                  "/collision/" + key);
            }
            continue;
        }
        retainUnknownMember(s, s.behavior.collisionUnknown, "/collision", key, value);
    }
    releaseObject(keys);
}

void parseTilePropertiesSection(SceneState &s, void *root) {
    void *tileKeys = rt_map_keys(root);
    int64_t count = rt_seq_len(tileKeys);
    int64_t totalEntries = 0;
    for (const auto &[_, props] : s.behavior.tileProperties)
        totalEntries += static_cast<int64_t>(props.values.size());
    for (int64_t i = 0; i < count; ++i) {
        rt_string tileKey = rt_seq_get_str(tileKeys, i);
        std::string key = toStd(tileKey);
        void *props = rt_map_get(root, tileKey);
        rt_string_unref(tileKey);
        int64_t tileId = 0;
        if (!strictIntegerKey(key, tileId) || !isMap(props)) {
            retainUnknownMember(s, s.behavior.tilePropertiesUnknown, "/tileProperties", key, props);
            continue;
        }
        TilePropertySet &target = s.behavior.tileProperties[tileId];
        void *propKeys = rt_map_keys(props);
        int64_t propCount = rt_seq_len(propKeys);
        for (int64_t pi = 0; pi < propCount; ++pi) {
            rt_string propKeyStr = rt_seq_get_str(propKeys, pi);
            std::string propKey = toStd(propKeyStr);
            void *raw = rt_map_get(props, propKeyStr);
            rt_string_unref(propKeyStr);
            int64_t intValue = 0;
            SceneScalar scalar;
            if (raw && rt_box_type(raw) == RT_BOX_I1)
                scalar = makeBoolScalar(rt_unbox_i1(raw) != 0);
            else if (jsonNumberToInt(raw, intValue))
                scalar = makeIntScalar(intValue);
            else {
                retainUnknownMember(s, target.unknown, "/tileProperties/" + key, propKey, raw);
                continue;
            }
            if (!validKey(propKey)) {
                addDiagnostic(s,
                              "scene.schema.limit_exceeded",
                              "warning",
                              "tile property key exceeds 128 bytes",
                              "/tileProperties/" + key);
                continue;
            }
            if (target.values.size() >= static_cast<size_t>(kMaxTilePropertiesPerTile) ||
                totalEntries >= kMaxTilePropertyEntries) {
                addDiagnostic(s,
                              "scene.schema.limit_exceeded",
                              "warning",
                              "too many tile properties; later entries were dropped",
                              "/tileProperties/" + key);
                continue;
            }
            target.values[propKey] = std::move(scalar);
            ++totalEntries;
        }
        releaseObject(propKeys);
        if (target.empty())
            s.behavior.tileProperties.erase(tileId);
    }
    releaseObject(tileKeys);
}

void parseAnimationsSection(SceneState &s, void *root) {
    int64_t count = rt_seq_len(root);
    int64_t totalFrames = 0;
    for (const auto &[_, anim] : s.behavior.animations)
        totalFrames += static_cast<int64_t>(anim.frames.size());
    for (int64_t i = 0; i < count; ++i) {
        void *record = rt_seq_get(root, i);
        if (!isMap(record)) {
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "tile animation record must be an object",
                          "/animations");
            continue;
        }
        int64_t base = 0;
        if (!jsonInt(record, "baseTile", base)) {
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "tile animation record is missing an integer baseTile",
                          "/animations");
            continue;
        }
        std::string path = "/animations/" + std::to_string(base);
        void *frameList = mapGet(record, "frames");
        int64_t frameCount = isSeq(frameList) ? rt_seq_len(frameList) : 0;
        if (frameCount <= 0 || frameCount > kMaxAnimFramesPerRule) {
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "tile animation frames must be a 1..4096 entry array",
                          path);
            continue;
        }
        TileAnimation animation;
        animation.frames.resize(static_cast<size_t>(frameCount));
        bool validFrames = true;
        for (int64_t fi = 0; fi < frameCount; ++fi)
            validFrames = validFrames && jsonNumberToInt(rt_seq_get(frameList, fi),
                                                         animation.frames[static_cast<size_t>(fi)]);
        if (!validFrames) {
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "tile animation frames must be integers",
                          path);
            continue;
        }
        void *durationList = mapGet(record, "durations");
        bool haveDurations = false;
        if (isSeq(durationList) && rt_seq_len(durationList) == frameCount) {
            animation.durations.resize(static_cast<size_t>(frameCount));
            haveDurations = true;
            for (int64_t fi = 0; fi < frameCount; ++fi) {
                haveDurations = haveDurations &&
                                jsonNumberToInt(rt_seq_get(durationList, fi),
                                                animation.durations[static_cast<size_t>(fi)]) &&
                                animation.durations[static_cast<size_t>(fi)] > 0;
            }
        }
        if (!haveDurations) {
            int64_t declaredFrames = 0;
            int64_t ms = 0;
            if (!jsonInt(record, "frameCount", declaredFrames) || declaredFrames != frameCount ||
                !jsonInt(record, "msPerFrame", ms) || ms <= 0) {
                addDiagnostic(s,
                              "scene.schema.invalid_value",
                              "warning",
                              "tile animation needs per-frame durations or a "
                              "matching frameCount with positive msPerFrame",
                              path);
                continue;
            }
            animation.durations.assign(static_cast<size_t>(frameCount), ms);
        }
        if (totalFrames + frameCount > kMaxTotalAnimFrames) {
            addDiagnostic(s,
                          "scene.schema.limit_exceeded",
                          "warning",
                          "too many tile animation frames; later rules were dropped",
                          path);
            continue;
        }
        void *memberKeys = rt_map_keys(record);
        int64_t memberCount = rt_seq_len(memberKeys);
        for (int64_t mi = 0; mi < memberCount; ++mi) {
            rt_string memberStr = rt_seq_get_str(memberKeys, mi);
            std::string member = toStd(memberStr);
            void *value = rt_map_get(record, memberStr);
            rt_string_unref(memberStr);
            // frameCount/msPerFrame are a redundant legacy encoding of the
            // canonical per-frame durations and are intentionally not retained.
            if (member == "baseTile" || member == "frames" || member == "durations" ||
                member == "frameCount" || member == "msPerFrame")
                continue;
            retainUnknownMember(s, animation.unknown, path, member, value);
        }
        releaseObject(memberKeys);
        if (s.behavior.animations.count(base))
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "duplicate tile animation baseTile; the later rule wins",
                          path);
        else
            totalFrames += frameCount;
        s.behavior.animations[base] = std::move(animation);
    }
}

void parseAutotilesSection(SceneState &s, void *root) {
    int64_t count = rt_seq_len(root);
    for (int64_t i = 0; i < count; ++i) {
        void *rule = rt_seq_get(root, i);
        if (!isMap(rule)) {
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "autotile rule must be an object",
                          "/autotiles");
            continue;
        }
        int64_t base = 0;
        if (!jsonInt(rule, "baseTile", base)) {
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "autotile rule is missing an integer baseTile",
                          "/autotiles");
            continue;
        }
        std::string path = "/autotiles/" + std::to_string(base);
        void *variants = mapGet(rule, "variants");
        if (!isSeq(variants) ||
            rt_seq_len(variants) < static_cast<int64_t>(kAutotileVariantCount)) {
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "autotile variants must be an array of at least 16 tile ids",
                          path);
            continue;
        }
        AutotileRule parsed;
        bool ok = true;
        for (size_t vi = 0; vi < kAutotileVariantCount; ++vi)
            ok = ok && jsonNumberToInt(rt_seq_get(variants, static_cast<int64_t>(vi)),
                                       parsed.variants[vi]);
        if (!ok) {
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "autotile variants must be integers",
                          path);
            continue;
        }
        if (rt_seq_len(variants) > static_cast<int64_t>(kAutotileVariantCount))
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "autotile variants beyond the first 16 were dropped",
                          path);
        void *memberKeys = rt_map_keys(rule);
        int64_t memberCount = rt_seq_len(memberKeys);
        for (int64_t mi = 0; mi < memberCount; ++mi) {
            rt_string memberStr = rt_seq_get_str(memberKeys, mi);
            std::string member = toStd(memberStr);
            void *value = rt_map_get(rule, memberStr);
            rt_string_unref(memberStr);
            if (member == "baseTile" || member == "variants")
                continue;
            retainUnknownMember(s, parsed.unknown, path, member, value);
        }
        releaseObject(memberKeys);
        if (s.behavior.autotiles.count(base))
            addDiagnostic(s,
                          "scene.schema.invalid_value",
                          "warning",
                          "duplicate autotile baseTile; the later rule wins",
                          path);
        s.behavior.autotiles[base] = std::move(parsed);
    }
}

void parseScalarSection(SceneState &s,
                        SectionScalarState &section,
                        const std::string &name,
                        void *root) {
    void *keys = rt_map_keys(root);
    int64_t count = rt_seq_len(keys);
    for (int64_t i = 0; i < count; ++i) {
        rt_string keyStr = rt_seq_get_str(keys, i);
        std::string key = toStd(keyStr);
        void *value = rt_map_get(root, keyStr);
        rt_string_unref(keyStr);
        if (!validKey(key)) {
            addDiagnostic(s,
                          "scene.schema.limit_exceeded",
                          "warning",
                          name + " field key exceeds 128 bytes",
                          "/" + name);
            continue;
        }
        int64_t intValue = 0;
        if (value && rt_string_is_handle(value)) {
            std::string text = toStd(static_cast<rt_string>(value));
            if (validStringValue(text)) {
                section.fields[key] = makeStringScalar(std::move(text));
                continue;
            }
        } else if (value && rt_box_type(value) == RT_BOX_STR) {
            rt_string str = rt_unbox_str(value);
            std::string text = toStd(str);
            rt_string_unref(str);
            if (validStringValue(text)) {
                section.fields[key] = makeStringScalar(std::move(text));
                continue;
            }
        } else if (jsonNumberToInt(value, intValue)) {
            section.fields[key] = makeIntScalar(intValue);
            continue;
        }
        retainUnknownMember(s, section.unknown, "/" + name, key, value);
    }
    releaseObject(keys);
}

/// @brief Route one known rich section into typed state (ADR 0176, ADR 0177).
/// @return True when the section was consumed; false to preserve it verbatim.
bool parseTypedSection(SceneState &s, const std::string &key, void *value) {
    if (key == "collision" && isMap(value)) {
        parseCollisionSection(s, value);
        return true;
    }
    if (key == "tileProperties" && isMap(value)) {
        parseTilePropertiesSection(s, value);
        return true;
    }
    if (key == "animations" && isSeq(value)) {
        parseAnimationsSection(s, value);
        return true;
    }
    if (key == "autotiles" && isSeq(value)) {
        parseAutotilesSection(s, value);
        return true;
    }
    if (key == "camera" && isMap(value)) {
        parseScalarSection(s, s.camera, "camera", value);
        return true;
    }
    if (key == "lighting" && isMap(value)) {
        parseScalarSection(s, s.lighting, "lighting", value);
        return true;
    }
    return false;
}

void parsePreservedSections(SceneState &s, void *root) {
    if (!isMap(root))
        return;
    void *keys = rt_map_keys(root);
    int64_t count = rt_seq_len(keys);
    for (int64_t i = 0; i < count; ++i) {
        rt_string keyStr = rt_seq_get_str(keys, i);
        std::string key = toStd(keyStr);
        void *value = rt_map_get(root, keyStr);
        rt_string_unref(keyStr);

        if (isReservedTopLevelKey(key))
            continue;
        if (isKnownRichSection(key)) {
            if (parseTypedSection(s, key, value))
                continue;
            // Wrong-shaped known sections stay preserved verbatim, exactly as
            // before typed parsing existed; BuildTilemap treats them as no-ops.
            if (isMap(value) || isSeq(value))
                preserveSection(s, key, value);
            else
                addDiagnostic(s,
                              "scene.schema.unknown_field_dropped",
                              "warning",
                              "rich scene section must be an object or array",
                              "/" + key);
            continue;
        }
        if (isMap(value))
            preserveSection(s, key, value);
        else
            addDiagnostic(s,
                          "scene.schema.unknown_field_dropped",
                          "warning",
                          "unknown top-level scalar or array was dropped",
                          "/" + key);
    }
    releaseObject(keys);
}

bool loadTiles(SceneState &s,
               Layer &layer,
               void *layerMap,
               int64_t layerIndex,
               bool legacy,
               bool nestedDraft) {
    const char *field = mapHas(layerMap, "tiles") ? "tiles" : "data";
    void *tiles = mapGet(layerMap, field);
    std::string path = "/layers/" + std::to_string(layerIndex) + "/" + field;
    if (!isSeq(tiles)) {
        if (!legacy && !nestedDraft)
            addDiagnostic(
                s, "scene.schema.missing_field", "error", "layer tiles are required", path);
        return legacy || nestedDraft;
    }
    size_t cells = 0;
    if (!checkedCellCount(s.width, s.height, cells)) {
        addDiagnostic(s,
                      "scene.schema.invalid_dimension",
                      "error",
                      "invalid dimensions for tile storage",
                      path);
        return false;
    }
    int64_t len = rt_seq_len(tiles);
    if (len != static_cast<int64_t>(cells)) {
        if (legacy || std::string(field) == "data") {
            addDiagnostic(s,
                          "scene.schema.legacy_tile_count_mismatch",
                          "warning",
                          "legacy layer tile count was normalized",
                          path);
        } else {
            addDiagnostic(s,
                          "scene.schema.tile_count_mismatch",
                          "error",
                          "layer tile count differs from width * height",
                          path);
            return false;
        }
    }
    int64_t n = std::min<int64_t>(len, static_cast<int64_t>(cells));
    for (int64_t i = 0; i < n; ++i) {
        int64_t tile = 0;
        if (jsonNumberToInt(rt_seq_get(tiles, i), tile))
            layer.tiles[static_cast<size_t>(i)] = tile;
        else
            addDiagnostic(s,
                          "scene.schema.invalid_type",
                          "error",
                          "tile entries must be integers",
                          path + "/" + std::to_string(i));
    }
    return true;
}

void parseLayers(SceneState &s, void *sourceMap, bool legacy, bool nestedDraft) {
    void *layers = mapGet(sourceMap, "layers");
    if (!isSeq(layers)) {
        if (!legacy)
            addDiagnostic(
                s, "scene.schema.missing_field", "error", "layers are required", "/layers");
        if (s.layers.empty())
            s.layers.push_back(makeLayer(s, "base"));
        return;
    }

    int64_t count = rt_seq_len(layers);
    if (count <= 0) {
        addDiagnostic(
            s, "scene.schema.missing_field", "error", "at least one layer is required", "/layers");
        s.layers.push_back(makeLayer(s, "base"));
        return;
    }
    if (count > kMaxLayers) {
        addDiagnostic(
            s, "scene.schema.limit_exceeded", "error", "too many scene layers", "/layers");
        count = kMaxLayers;
    }

    s.layers.clear();
    size_t cells = 0;
    checkedCellCount(s.width, s.height, cells);
    if (count * static_cast<int64_t>(cells) > kMaxTotalCells) {
        addDiagnostic(s,
                      "scene.schema.limit_exceeded",
                      "error",
                      "total tile cell budget exceeded",
                      "/layers");
        s.layers.push_back(makeLayer(s, "base"));
        return;
    }

    for (int64_t li = 0; li < count; ++li) {
        void *layerMap = rt_seq_get(layers, li);
        if (!isMap(layerMap)) {
            addDiagnostic(s,
                          "scene.schema.invalid_type",
                          "error",
                          "layer must be an object",
                          "/layers/" + std::to_string(li));
            continue;
        }
        std::string name;
        jsonString(layerMap, "name", name);
        if (name.empty())
            name = li == 0 ? "base" : "Layer" + std::to_string(li);
        if (name.size() > kMaxTilemapLayerNameBytes)
            addDiagnostic(s,
                          "scene.schema.limit_exceeded",
                          "error",
                          "layer name exceeds Tilemap's 31-byte limit",
                          "/layers/" + std::to_string(li) + "/name");

        Layer layer = makeLayer(s, name);
        jsonString(layerMap, "asset", layer.asset);
        bool visible = true;
        if (jsonBool(layerMap, "visible", visible, legacy))
            layer.visible = visible;

        loadTiles(s, layer, layerMap, li, legacy, nestedDraft);
        s.layers.push_back(std::move(layer));
    }

    if (s.layers.empty())
        s.layers.push_back(makeLayer(s, "base"));
}

void validateObjectParents(SceneState &s) {
    const int64_t count = static_cast<int64_t>(s.objects.size());
    for (int64_t index = 0; index < count; ++index) {
        Object &object = s.objects[static_cast<size_t>(index)];
        if (object.parent == -1)
            continue;
        if (object.parent < -1 || object.parent >= count) {
            addDiagnostic(s,
                          "scene.schema.invalid_object_parent",
                          "error",
                          "object parent index is outside the object array",
                          "/objects/" + std::to_string(index) + "/properties/" +
                              kObjectParentProperty);
            object.parent = -1;
            continue;
        }
        if (object.parent == index) {
            addDiagnostic(s,
                          "scene.schema.invalid_object_parent",
                          "error",
                          "object cannot be its own parent",
                          "/objects/" + std::to_string(index) + "/properties/" +
                              kObjectParentProperty);
            object.parent = -1;
        }
    }

    std::vector<uint8_t> state(static_cast<size_t>(count), 0);
    for (int64_t start = 0; start < count; ++start) {
        if (state[static_cast<size_t>(start)] != 0)
            continue;

        std::vector<int64_t> chain;
        int64_t current = start;
        while (current >= 0 && state[static_cast<size_t>(current)] == 0) {
            state[static_cast<size_t>(current)] = 1;
            chain.push_back(current);
            current = s.objects[static_cast<size_t>(current)].parent;
        }

        if (current >= 0 && state[static_cast<size_t>(current)] == 1) {
            auto cycleStart = std::find(chain.begin(), chain.end(), current);
            addDiagnostic(s,
                          "scene.schema.object_parent_cycle",
                          "error",
                          "object hierarchy contains a parent cycle",
                          "/objects/" + std::to_string(current) + "/properties/" +
                              kObjectParentProperty);
            if (cycleStart == chain.end()) {
                s.objects[static_cast<size_t>(current)].parent = -1;
            } else {
                for (auto it = cycleStart; it != chain.end(); ++it)
                    s.objects[static_cast<size_t>(*it)].parent = -1;
            }
        }

        for (int64_t index : chain)
            state[static_cast<size_t>(index)] = 2;
    }
}

void parseObjects(SceneState &s, void *root) {
    void *objects = mapGet(root, "objects");
    if (!isSeq(objects))
        return;
    int64_t count = rt_seq_len(objects);
    if (count > kMaxObjects) {
        addDiagnostic(
            s, "scene.schema.limit_exceeded", "error", "too many scene objects", "/objects");
        count = kMaxObjects;
    }
    for (int64_t i = 0; i < count; ++i) {
        void *objMap = rt_seq_get(objects, i);
        if (!isMap(objMap)) {
            addDiagnostic(s,
                          "scene.schema.invalid_type",
                          "error",
                          "object must be a map",
                          "/objects/" + std::to_string(i));
            continue;
        }
        Object obj;
        jsonString(objMap, "type", obj.type);
        jsonString(objMap, "id", obj.id);
        jsonInt(objMap, "x", obj.x);
        jsonInt(objMap, "y", obj.y);

        void *props = mapGet(objMap, "properties");
        if (isMap(props))
            parseScalarMap(s,
                           props,
                           obj.properties,
                           kMaxObjectProperties,
                           "/objects/" + std::to_string(i) + "/properties");

        void *keys = rt_map_keys(objMap);
        int64_t keysLen = rt_seq_len(keys);
        for (int64_t ki = 0; ki < keysLen; ++ki) {
            rt_string keyStr = rt_seq_get_str(keys, ki);
            std::string key = toStd(keyStr);
            void *value = rt_map_get(objMap, keyStr);
            rt_string_unref(keyStr);
            if (key == "type" || key == "id" || key == "x" || key == "y" || key == "properties")
                continue;
            if (obj.properties.size() >= static_cast<size_t>(kMaxObjectProperties)) {
                addDiagnostic(s,
                              "scene.schema.limit_exceeded",
                              "error",
                              "too many object properties",
                              "/objects/" + std::to_string(i) + "/" + key);
                continue;
            }
            SceneScalar scalar;
            std::string path = "/objects/" + std::to_string(i);
            if (!parseScalarValue(value, scalar)) {
                addDiagnostic(s,
                              "scene.schema.invalid_type",
                              "error",
                              "property values must be scalar",
                              path + "/" + key);
                continue;
            }
            if (!validateScalarLimit(s, key, scalar, path))
                continue;
            obj.properties[key] = std::move(scalar);
        }
        releaseObject(keys);

        auto parent = obj.properties.find(kObjectParentProperty);
        if (parent != obj.properties.end()) {
            if (parent->second.kind == ScalarKind::Int) {
                obj.parent = parent->second.intValue;
            } else {
                addDiagnostic(s,
                              "scene.schema.invalid_type",
                              "error",
                              "object parent index must be an integer",
                              "/objects/" + std::to_string(i) + "/properties/" +
                                  kObjectParentProperty);
            }
            obj.properties.erase(parent);
        }
        s.objects.push_back(std::move(obj));
    }
    validateObjectParents(s);
}

SceneState loadStateFromJson(rt_string text, const std::string &sourcePath) {
    SceneState s;
    s.sourcePath = sourcePath;
    if (!text || rt_str_len(text) == 0) {
        addDiagnostic(s, "scene.load.empty", "error", "scene JSON is empty", {}, 1, 1);
        s.layers.push_back(makeLayer(s, "base"));
        return s;
    }
    if (rt_str_len(text) > kMaxJsonBytes) {
        addDiagnostic(s, "scene.schema.limit_exceeded", "error", "scene JSON exceeds 16 MiB");
        s.layers.push_back(makeLayer(s, "base"));
        return s;
    }

    void *root = nullptr;
    rt_string parseMessage = nullptr;
    int64_t line = 0;
    int64_t column = 0;
    if (!rt_json_try_parse(text, &root, &parseMessage, &line, &column)) {
        addDiagnostic(s,
                      "scene.parse.malformed_json",
                      "error",
                      parseMessage ? toStd(parseMessage) : "malformed JSON",
                      {},
                      line,
                      column);
        rt_str_release_maybe(parseMessage);
        s.layers.push_back(makeLayer(s, "base"));
        return s;
    }
    rt_str_release_maybe(parseMessage);
    if (!isMap(root)) {
        addDiagnostic(
            s, "scene.schema.root_not_object", "error", "scene JSON root must be an object");
        releaseJsonValue(root);
        s.layers.push_back(makeLayer(s, "base"));
        return s;
    }

    bool legacy = !mapHas(root, "version");
    int64_t version = 1;
    if (!legacy && (!jsonInt(root, "version", version) || version != 1)) {
        addDiagnostic(s,
                      "scene.schema.unsupported_version",
                      "error",
                      "unsupported scene version",
                      "/version");
    }
    s.version = kSceneVersion;
    jsonString(root, "name", s.name);
    jsonString(root, "tilesetAsset", s.tilesetAsset);

    void *sceneMap = root;
    bool nestedDraft = false;
    void *tilemap = mapGet(root, "tilemap");
    if (isMap(tilemap)) {
        sceneMap = tilemap;
        nestedDraft = true;
        legacy = true;
        if (s.tilesetAsset.empty())
            jsonString(tilemap, "tilesetAsset", s.tilesetAsset);
    }

    int64_t width = 0;
    int64_t height = 0;
    int64_t tw = 0;
    int64_t th = 0;
    bool haveWidth = jsonInt(sceneMap, "width", width);
    bool haveHeight = jsonInt(sceneMap, "height", height);
    bool haveTileWidth = jsonInt(sceneMap, "tileWidth", tw);
    bool haveTileHeight = jsonInt(sceneMap, "tileHeight", th);

    if (!legacy && (!haveWidth || !haveHeight || !haveTileWidth || !haveTileHeight))
        addDiagnostic(s,
                      "scene.schema.missing_field",
                      "error",
                      "canonical scenes require width, height, tileWidth, and tileHeight");
    if (!haveWidth)
        width = 1;
    if (!haveHeight)
        height = 1;
    if (!haveTileWidth)
        tw = 16;
    if (!haveTileHeight)
        th = 16;

    size_t cells = 0;
    if (!checkedCellCount(width, height, cells)) {
        addDiagnostic(s,
                      "scene.schema.invalid_dimension",
                      "error",
                      "scene dimensions are invalid or exceed limits");
        width = 1;
        height = 1;
    }
    if (tw <= 0 || th <= 0) {
        addDiagnostic(
            s, "scene.schema.invalid_dimension", "error", "tile dimensions must be positive");
        tw = tw <= 0 ? 16 : tw;
        th = th <= 0 ? 16 : th;
    }
    s.width = width;
    s.height = height;
    s.tileWidth = tw;
    s.tileHeight = th;

    void *props = mapGet(root, "properties");
    if (isMap(props))
        parseScalarMap(s, props, s.properties, kMaxSceneProperties, "/properties");
    else if (!legacy && mapHas(root, "properties"))
        addDiagnostic(s,
                      "scene.schema.invalid_type",
                      "error",
                      "scene properties must be an object",
                      "/properties");

    parseLayers(s, sceneMap, legacy, nestedDraft);
    parseObjects(s, root);
    parsePreservedSections(s, root);

    releaseJsonValue(root);
    return s;
}

void writeIndent(std::ostringstream &out, int level) {
    for (int i = 0; i < level; ++i)
        out << "  ";
}

void writeScalarMap(std::ostringstream &out,
                    const std::map<std::string, SceneScalar> &map,
                    int level) {
    out << "{";
    if (!map.empty())
        out << "\n";
    size_t index = 0;
    for (const auto &[key, value] : map) {
        writeIndent(out, level + 1);
        out << jsonEscape(key) << ": " << scalarToJson(value);
        if (++index < map.size())
            out << ",";
        out << "\n";
    }
    if (!map.empty())
        writeIndent(out, level);
    out << "}";
}

void writeTiles(std::ostringstream &out, const std::vector<int64_t> &tiles) {
    out << "[";
    for (size_t i = 0; i < tiles.size(); ++i) {
        if (i)
            out << ", ";
        out << tiles[i];
    }
    out << "]";
}

/// @brief Emit one compact JSON object from pre-rendered members, sorted by key.
void writeMergedObject(std::ostringstream &out,
                       std::vector<std::pair<std::string, std::string>> members) {
    std::sort(members.begin(), members.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    out << "{";
    for (size_t i = 0; i < members.size(); ++i) {
        if (i)
            out << ",";
        out << jsonEscape(members[i].first) << ":" << members[i].second;
    }
    out << "}";
}

std::string intListJson(const std::vector<int64_t> &values) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            out << ",";
        out << values[i];
    }
    out << "]";
    return out.str();
}

void writeCollisionSection(std::ostringstream &out, const TileBehaviorState &b) {
    std::vector<std::pair<std::string, std::string>> members;
    if (b.hasCollisionLayer)
        members.emplace_back("layer", std::to_string(b.collisionLayer));
    std::vector<int64_t> solid;
    std::vector<int64_t> oneWayUp;
    for (const auto &[tile, kind] : b.collision) {
        if (kind == RT_TILE_COLLISION_ONE_WAY_UP)
            oneWayUp.push_back(tile);
        else
            solid.push_back(tile);
    }
    if (!solid.empty())
        members.emplace_back("solid", intListJson(solid));
    if (!oneWayUp.empty())
        members.emplace_back("oneWayUp", intListJson(oneWayUp));
    for (const auto &[key, json] : b.collisionUnknown)
        members.emplace_back(key, json);
    writeMergedObject(out, std::move(members));
}

void writeTilePropertiesSection(std::ostringstream &out, const TileBehaviorState &b) {
    out << "{";
    bool first = true;
    for (const auto &[tile, props] : b.tileProperties) {
        if (!first)
            out << ",";
        first = false;
        out << jsonEscape(std::to_string(tile)) << ":";
        std::vector<std::pair<std::string, std::string>> members;
        for (const auto &[key, value] : props.values)
            members.emplace_back(key, scalarToJson(value));
        for (const auto &[key, json] : props.unknown)
            members.emplace_back(key, json);
        writeMergedObject(out, std::move(members));
    }
    for (const auto &[key, json] : b.tilePropertiesUnknown) {
        if (!first)
            out << ",";
        first = false;
        out << jsonEscape(key) << ":" << json;
    }
    out << "}";
}

void writeAnimationsSection(std::ostringstream &out, const TileBehaviorState &b) {
    out << "[";
    bool first = true;
    for (const auto &[base, animation] : b.animations) {
        if (!first)
            out << ",";
        first = false;
        std::vector<std::pair<std::string, std::string>> members;
        members.emplace_back("baseTile", std::to_string(base));
        members.emplace_back("frames", intListJson(animation.frames));
        members.emplace_back("durations", intListJson(animation.durations));
        for (const auto &[key, json] : animation.unknown)
            members.emplace_back(key, json);
        writeMergedObject(out, std::move(members));
    }
    out << "]";
}

void writeAutotilesSection(std::ostringstream &out, const TileBehaviorState &b) {
    out << "[";
    bool first = true;
    for (const auto &[base, rule] : b.autotiles) {
        if (!first)
            out << ",";
        first = false;
        std::vector<std::pair<std::string, std::string>> members;
        members.emplace_back("baseTile", std::to_string(base));
        members.emplace_back("variants", intListJson({rule.variants.begin(), rule.variants.end()}));
        for (const auto &[key, json] : rule.unknown)
            members.emplace_back(key, json);
        writeMergedObject(out, std::move(members));
    }
    out << "]";
}

void writeScalarSection(std::ostringstream &out, const SectionScalarState &section) {
    std::vector<std::pair<std::string, std::string>> members;
    for (const auto &[key, value] : section.fields)
        members.emplace_back(key, scalarToJson(value));
    for (const auto &[key, json] : section.unknown)
        members.emplace_back(key, json);
    writeMergedObject(out, std::move(members));
}

/// @brief Emit one typed rich section when non-empty; return true when emitted.
bool writeTypedSection(std::ostringstream &out, const SceneState &s, const std::string &key) {
    if (key == "collision" && !s.behavior.collisionEmpty()) {
        out << ",\n  \"collision\": ";
        writeCollisionSection(out, s.behavior);
        return true;
    }
    if (key == "tileProperties" && !s.behavior.tilePropertiesEmpty()) {
        out << ",\n  \"tileProperties\": ";
        writeTilePropertiesSection(out, s.behavior);
        return true;
    }
    if (key == "animations" && !s.behavior.animations.empty()) {
        out << ",\n  \"animations\": ";
        writeAnimationsSection(out, s.behavior);
        return true;
    }
    if (key == "autotiles" && !s.behavior.autotiles.empty()) {
        out << ",\n  \"autotiles\": ";
        writeAutotilesSection(out, s.behavior);
        return true;
    }
    if (key == "camera" && !s.camera.empty()) {
        out << ",\n  \"camera\": ";
        writeScalarSection(out, s.camera);
        return true;
    }
    if (key == "lighting" && !s.lighting.empty()) {
        out << ",\n  \"lighting\": ";
        writeScalarSection(out, s.lighting);
        return true;
    }
    return false;
}

void writeCanonicalJson(std::ostringstream &out, const SceneState &s) {
    out << "{\n";
    out << "  \"version\": " << kSceneVersion << ",\n";
    out << "  \"name\": " << jsonEscape(s.name) << ",\n";
    out << "  \"width\": " << s.width << ",\n";
    out << "  \"height\": " << s.height << ",\n";
    out << "  \"tileWidth\": " << s.tileWidth << ",\n";
    out << "  \"tileHeight\": " << s.tileHeight << ",\n";
    out << "  \"tilesetAsset\": " << jsonEscape(s.tilesetAsset) << ",\n";
    out << "  \"properties\": ";
    writeScalarMap(out, s.properties, 1);
    out << ",\n";
    out << "  \"layers\": [\n";
    for (size_t li = 0; li < s.layers.size(); ++li) {
        const Layer &layer = s.layers[li];
        out << "    {\n";
        out << "      \"name\": " << jsonEscape(layer.name) << ",\n";
        out << "      \"visible\": " << (layer.visible ? "true" : "false") << ",\n";
        out << "      \"asset\": " << jsonEscape(layer.asset) << ",\n";
        out << "      \"tiles\": ";
        writeTiles(out, layer.tiles);
        out << "\n";
        out << "    }";
        if (li + 1 < s.layers.size())
            out << ",";
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"objects\": [\n";
    for (size_t oi = 0; oi < s.objects.size(); ++oi) {
        const Object &obj = s.objects[oi];
        out << "    {\n";
        out << "      \"type\": " << jsonEscape(obj.type) << ",\n";
        out << "      \"id\": " << jsonEscape(obj.id) << ",\n";
        out << "      \"x\": " << obj.x << ",\n";
        out << "      \"y\": " << obj.y << ",\n";
        out << "      \"properties\": ";
        std::map<std::string, SceneScalar> properties = obj.properties;
        if (obj.parent >= 0)
            properties[kObjectParentProperty] = makeIntScalar(obj.parent);
        writeScalarMap(out, properties, 3);
        out << "\n";
        out << "    }";
        if (oi + 1 < s.objects.size())
            out << ",";
        out << "\n";
    }
    out << "  ]";

    static const char *knownSections[] = {
        "camera", "lighting", "collision", "tileProperties", "animations", "autotiles"};
    std::set<std::string> emitted;
    for (const char *key : knownSections) {
        if (writeTypedSection(out, s, key)) {
            emitted.insert(key);
            continue;
        }
        auto it = s.preservedSections.find(key);
        if (it == s.preservedSections.end())
            continue;
        out << ",\n  " << jsonEscape(it->first) << ": " << it->second.canonicalJson;
        emitted.insert(it->first);
    }
    for (const auto &[key, section] : s.preservedSections) {
        if (emitted.count(key))
            continue;
        out << ",\n  " << jsonEscape(key) << ": " << section.canonicalJson;
    }
    out << "\n}\n";
}

std::string assetKindForKey(const std::string &key, const std::string &fallback) {
    std::string lower = lowerAscii(key);
    if (lower.find("tileset") != std::string::npos)
        return "tileset";
    if (lower.find("sprite") != std::string::npos)
        return "sprite";
    if (lower.find("audio") != std::string::npos || lower.find("sound") != std::string::npos ||
        lower.find("music") != std::string::npos)
        return "audio";
    if (lower.find("image") != std::string::npos || lower.find("texture") != std::string::npos ||
        lower.find("background") != std::string::npos ||
        lower.find("parallax") != std::string::npos)
        return "image";
    if (lower.find("asset") != std::string::npos)
        return fallback.empty() ? "unknown" : fallback;
    return "";
}

struct AssetDescriptor {
    std::string path;
    std::string kind;
    std::string owner;
    int64_t layer{-1};
    int64_t object{-1};
    std::string key;
    std::string section;
    std::string source;
};

void addAsset(std::vector<AssetDescriptor> &out,
              const SceneState &s,
              std::string path,
              std::string kind,
              std::string owner,
              int64_t layer,
              int64_t object,
              std::string key,
              std::string section = {}) {
    if (path.empty())
        return;
    out.push_back(AssetDescriptor{std::move(path),
                                  std::move(kind),
                                  std::move(owner),
                                  layer,
                                  object,
                                  std::move(key),
                                  std::move(section),
                                  s.sourcePath});
}

void collectAssetsFromJsonValue(std::vector<AssetDescriptor> &out,
                                const SceneState &s,
                                void *value,
                                const std::string &section,
                                const std::string &keyHint) {
    if (!value)
        return;
    if (rt_string_is_handle(value)) {
        std::string kind = assetKindForKey(keyHint, "unknown");
        if (!kind.empty())
            addAsset(out,
                     s,
                     toStd(static_cast<rt_string>(value)),
                     kind,
                     "section",
                     -1,
                     -1,
                     keyHint,
                     section);
        return;
    }
    if (isSeq(value)) {
        int64_t len = rt_seq_len(value);
        for (int64_t i = 0; i < len; ++i)
            collectAssetsFromJsonValue(out, s, rt_seq_get(value, i), section, keyHint);
        return;
    }
    if (!isMap(value))
        return;
    void *keys = rt_map_keys(value);
    int64_t len = rt_seq_len(keys);
    for (int64_t i = 0; i < len; ++i) {
        rt_string keyStr = rt_seq_get_str(keys, i);
        std::string key = toStd(keyStr);
        void *child = rt_map_get(value, keyStr);
        rt_string_unref(keyStr);
        collectAssetsFromJsonValue(out, s, child, section, key);
    }
    releaseObject(keys);
}

std::vector<AssetDescriptor> collectAssetDescriptors(const SceneState &s) {
    std::vector<AssetDescriptor> out;
    addAsset(out, s, s.tilesetAsset, "tileset", "scene", -1, -1, "tilesetAsset");
    for (const auto &[key, value] : s.properties) {
        if (value.kind != ScalarKind::String)
            continue;
        std::string kind = assetKindForKey(key, "unknown");
        if (!kind.empty())
            addAsset(out, s, value.stringValue, kind, "scene", -1, -1, key);
    }
    for (size_t i = 0; i < s.layers.size(); ++i)
        addAsset(
            out, s, s.layers[i].asset, "tileset", "layer", static_cast<int64_t>(i), -1, "asset");
    for (size_t i = 0; i < s.objects.size(); ++i) {
        for (const auto &[key, value] : s.objects[i].properties) {
            if (value.kind != ScalarKind::String)
                continue;
            std::string kind = assetKindForKey(key, "unknown");
            if (!kind.empty())
                addAsset(
                    out, s, value.stringValue, kind, "object", -1, static_cast<int64_t>(i), key);
        }
    }
    for (const auto &[key, section] : s.preservedSections) {
        rt_string text = makeString(section.canonicalJson);
        void *root = nullptr;
        if (rt_json_try_parse(text, &root, nullptr, nullptr, nullptr) && root) {
            collectAssetsFromJsonValue(out, s, root, key, "");
            releaseJsonValue(root);
        }
        rt_string_unref(text);
    }
    auto scanScalarSection = [&](const SectionScalarState &section, const char *name) {
        for (const auto &[key, value] : section.fields) {
            if (value.kind != ScalarKind::String)
                continue;
            std::string kind = assetKindForKey(key, "unknown");
            if (!kind.empty())
                addAsset(out, s, value.stringValue, kind, "section", -1, -1, key, name);
        }
    };
    scanScalarSection(s.camera, "camera");
    scanScalarSection(s.lighting, "lighting");
    return out;
}

void *descriptorMap(const AssetDescriptor &d) {
    void *map = rt_map_new();
    mapSetStrField(map, "path", d.path);
    mapSetStrField(map, "kind", d.kind);
    mapSetStrField(map, "owner", d.owner);
    mapSetIntField(map, "layer", d.layer);
    mapSetIntField(map, "object", d.object);
    mapSetStrField(map, "key", d.key);
    mapSetStrField(map, "section", d.section);
    mapSetStrField(map, "source", d.source);
    return map;
}

const SceneScalar *findScalar(const std::map<std::string, SceneScalar> &map, rt_string key) {
    auto it = map.find(toStd(key));
    return it == map.end() ? nullptr : &it->second;
}

void setScalar(std::map<std::string, SceneScalar> &map,
               SceneState &s,
               rt_string key,
               SceneScalar value,
               int64_t maxEntries) {
    std::string k = toStd(key);
    if (!validKey(k)) {
        addDiagnostic(s, "scene.edit.rejected", "warning", "property key exceeds 128 bytes");
        return;
    }
    if (value.kind == ScalarKind::String && !validStringValue(value.stringValue)) {
        addDiagnostic(s, "scene.edit.rejected", "warning", "string property value exceeds 64 KiB");
        return;
    }
    if (map.find(k) == map.end() && map.size() >= static_cast<size_t>(maxEntries)) {
        addDiagnostic(s, "scene.edit.rejected", "warning", "too many properties");
        return;
    }
    map[k] = std::move(value);
}

int64_t maxPublicObjectProperties(const Object &object) {
    return kMaxObjectProperties - (object.parent >= 0 ? 1 : 0);
}

uint64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

std::filesystem::path makeSceneTempPath(const std::filesystem::path &dir,
                                        const std::filesystem::path &target) {
    static std::atomic<uint64_t> counter{0};
    std::string stem =
        "." + target.filename().string() + ".tmp." + std::to_string(currentProcessId()) + ".";
    for (int attempt = 0; attempt < 1024; ++attempt) {
        uint64_t id = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        std::filesystem::path candidate = dir / (stem + std::to_string(id));
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec))
            return candidate;
    }
    uint64_t id = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return dir / (stem + std::to_string(id));
}

/// @brief Write scene JSON to a newly created temporary file.
/// @details Uses exclusive-create semantics so a stale or attacker-created
///          temp path is never truncated. The caller is responsible for
///          removing @p temp on failure and for atomically replacing the target
///          on success.
/// @param temp Temporary path in the destination directory.
/// @param json Runtime string containing the serialized scene JSON.
/// @return True when the complete JSON payload was written, flushed, and the
///         file handle closed successfully.
bool writeSceneJsonTempExclusive(const std::filesystem::path &temp, rt_string json) {
    const char *data = rt_string_cstr(json);
    int64_t len64 = rt_str_len(json);
    if (!data || len64 < 0)
        return false;
    size_t len = static_cast<size_t>(len64);
#if RT_PLATFORM_WINDOWS
    HANDLE handle = CreateFileW(temp.wstring().c_str(),
                                GENERIC_WRITE,
                                0,
                                NULL,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    size_t pos = 0;
    while (pos < len) {
        DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(len - pos, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(handle, data + pos, chunk, &written, NULL) || written != chunk) {
            CloseHandle(handle);
            return false;
        }
        pos += written;
    }
    bool ok = FlushFileBuffers(handle) != 0;
    ok = CloseHandle(handle) != 0 && ok;
    return ok;
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(temp.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return false;
    size_t pos = 0;
    while (pos < len) {
        ssize_t written = write(fd, data + pos, len - pos);
        if (written <= 0) {
            close(fd);
            return false;
        }
        pos += static_cast<size_t>(written);
    }
    bool ok = fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    return ok;
#endif
}

bool replaceFileWithTemp(const std::filesystem::path &temp,
                         const std::filesystem::path &target,
                         std::error_code &ec) {
    ec.clear();
    std::filesystem::rename(temp, target, ec);
    if (!ec)
        return true;

#ifdef _WIN32
    ec.clear();
    if (MoveFileExW(temp.wstring().c_str(),
                    target.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#endif
    return false;
}

void applyCollisionSection(void *tilemap, void *root) {
    if (!isMap(root))
        return;
    int64_t collisionLayer = 0;
    if (jsonInt(root, "layer", collisionLayer))
        rt_tilemap_set_collision_layer(tilemap, collisionLayer);
    auto applyList = [&](const char *name, int64_t collision) {
        void *list = mapGet(root, name);
        if (!isSeq(list))
            return;
        int64_t len = rt_seq_len(list);
        for (int64_t i = 0; i < len; ++i) {
            int64_t tile = 0;
            if (jsonNumberToInt(rt_seq_get(list, i), tile))
                rt_tilemap_set_collision(tilemap, tile, collision);
        }
    };
    applyList("solid", RT_TILE_COLLISION_SOLID);
    applyList("oneWayUp", RT_TILE_COLLISION_ONE_WAY_UP);
}

void applyTilePropertiesSection(void *tilemap, void *root) {
    if (!isMap(root))
        return;
    void *tileKeys = rt_map_keys(root);
    int64_t tileCount = rt_seq_len(tileKeys);
    for (int64_t i = 0; i < tileCount; ++i) {
        rt_string tileKey = rt_seq_get_str(tileKeys, i);
        int64_t tileId = 0;
        try {
            tileId = std::stoll(toStd(tileKey));
        } catch (...) {
            rt_string_unref(tileKey);
            continue;
        }
        void *props = rt_map_get(root, tileKey);
        rt_string_unref(tileKey);
        if (!isMap(props))
            continue;
        void *propKeys = rt_map_keys(props);
        int64_t propCount = rt_seq_len(propKeys);
        for (int64_t pi = 0; pi < propCount; ++pi) {
            rt_string propKey = rt_seq_get_str(propKeys, pi);
            int64_t value = 0;
            if (jsonNumberToInt(rt_map_get(props, propKey), value))
                rt_tilemap_set_tile_property(tilemap, tileId, propKey, value);
            else {
                void *raw = rt_map_get(props, propKey);
                if (raw && rt_box_type(raw) == RT_BOX_I1)
                    rt_tilemap_set_tile_property(
                        tilemap, tileId, propKey, rt_unbox_i1(raw) ? 1 : 0);
            }
            rt_string_unref(propKey);
        }
        releaseObject(propKeys);
    }
    releaseObject(tileKeys);
}

void applyAnimationsSection(void *tilemap, void *root) {
    if (!isSeq(root))
        return;
    int64_t count = rt_seq_len(root);
    for (int64_t i = 0; i < count; ++i) {
        void *anim = rt_seq_get(root, i);
        if (!isMap(anim))
            continue;
        int64_t base = 0;
        if (!jsonInt(anim, "baseTile", base))
            continue;
        void *frameList = mapGet(anim, "frames");
        if (!isSeq(frameList))
            continue;
        int64_t frameCount = rt_seq_len(frameList);
        if (frameCount <= 0 || frameCount > 4096)
            continue;
        std::vector<int64_t> frameTiles(static_cast<size_t>(frameCount));
        bool validFrames = true;
        for (int64_t fi = 0; fi < frameCount; ++fi) {
            validFrames = validFrames && jsonNumberToInt(rt_seq_get(frameList, fi),
                                                         frameTiles[static_cast<size_t>(fi)]);
        }
        if (!validFrames)
            continue;
        void *durationList = mapGet(anim, "durations");
        if (isSeq(durationList) && rt_seq_len(durationList) == frameCount) {
            std::vector<int64_t> durations(static_cast<size_t>(frameCount));
            bool validDurations = true;
            for (int64_t fi = 0; fi < frameCount; ++fi) {
                validDurations = validDurations &&
                                 jsonNumberToInt(rt_seq_get(durationList, fi),
                                                 durations[static_cast<size_t>(fi)]) &&
                                 durations[static_cast<size_t>(fi)] > 0;
            }
            if (validDurations)
                rt_tilemap_set_import_tile_anim(
                    tilemap, base, frameCount, frameTiles.data(), durations.data());
            continue;
        }
        int64_t declaredFrames = 0;
        int64_t ms = 0;
        if (!jsonInt(anim, "frameCount", declaredFrames) || declaredFrames != frameCount ||
            !jsonInt(anim, "msPerFrame", ms))
            continue;
        rt_tilemap_set_tile_anim(tilemap, base, frameCount, ms);
        for (int64_t fi = 0; fi < frameCount; ++fi)
            rt_tilemap_set_tile_anim_frame(tilemap, base, fi, frameTiles[static_cast<size_t>(fi)]);
    }
}

void applyTiledRuntimeSection(void *tilemap, void *root) {
    if (!isMap(root))
        return;
    std::string orientationName;
    jsonString(root, "orientation", orientationName);
    int64_t orientation = RT_TILEMAP_IMPORT_ORTHOGONAL;
    if (orientationName == "isometric")
        orientation = RT_TILEMAP_IMPORT_ISOMETRIC;
    else if (orientationName == "staggered")
        orientation = RT_TILEMAP_IMPORT_STAGGERED;
    else if (orientationName == "hexagonal")
        orientation = RT_TILEMAP_IMPORT_HEXAGONAL;
    else if (orientationName == "oblique")
        orientation = RT_TILEMAP_IMPORT_OBLIQUE;

    int64_t originX = 0;
    int64_t originY = 0;
    int64_t projectionHeight = rt_tilemap_get_height(tilemap);
    int64_t sourceWidth = rt_tilemap_get_tile_width(tilemap);
    int64_t sourceHeight = rt_tilemap_get_tile_height(tilemap);
    int64_t drawOffsetX = 0;
    int64_t drawOffsetY = 0;
    int64_t hexSideLength = 0;
    jsonInt(root, "originTileX", originX);
    jsonInt(root, "originTileY", originY);
    jsonInt(root, "projectionHeight", projectionHeight);
    jsonInt(root, "sourceFrameWidth", sourceWidth);
    jsonInt(root, "sourceFrameHeight", sourceHeight);
    jsonInt(root, "drawOffsetX", drawOffsetX);
    jsonInt(root, "drawOffsetY", drawOffsetY);
    jsonInt(root, "hexSideLength", hexSideLength);

    std::string renderOrderName;
    jsonString(root, "renderOrder", renderOrderName);
    int64_t renderOrder = RT_TILEMAP_IMPORT_RIGHT_DOWN;
    if (renderOrderName == "right-up")
        renderOrder = RT_TILEMAP_IMPORT_RIGHT_UP;
    else if (renderOrderName == "left-down")
        renderOrder = RT_TILEMAP_IMPORT_LEFT_DOWN;
    else if (renderOrderName == "left-up")
        renderOrder = RT_TILEMAP_IMPORT_LEFT_UP;
    std::string staggerAxisName;
    jsonString(root, "staggerAxis", staggerAxisName);
    int64_t staggerAxis = staggerAxisName == "x" ? 0 : 1;
    std::string staggerIndexName;
    jsonString(root, "staggerIndex", staggerIndexName);
    int8_t staggerEven = staggerIndexName == "even" ? 1 : 0;
    double skewX = 0.0;
    double skewY = 0.0;
    double parallaxOriginX = 0.0;
    double parallaxOriginY = 0.0;
    jsonDouble(root, "skewX", skewX);
    jsonDouble(root, "skewY", skewY);
    jsonDouble(root, "parallaxOriginX", parallaxOriginX);
    jsonDouble(root, "parallaxOriginY", parallaxOriginY);
    rt_tilemap_configure_import_layout(tilemap,
                                       orientation,
                                       originX,
                                       originY,
                                       sourceWidth,
                                       sourceHeight,
                                       drawOffsetX,
                                       drawOffsetY,
                                       renderOrder,
                                       staggerAxis,
                                       staggerEven,
                                       hexSideLength,
                                       skewX,
                                       skewY,
                                       parallaxOriginX,
                                       parallaxOriginY,
                                       projectionHeight);

    void *layers = mapGet(root, "layers");
    if (!isSeq(layers))
        return;
    int64_t layerCount = std::min(rt_seq_len(layers), rt_tilemap_get_layer_count(tilemap));
    for (int64_t index = 0; index < layerCount; ++index) {
        void *layer = rt_seq_get(layers, index);
        if (!isMap(layer))
            continue;
        double offsetX = 0.0;
        double offsetY = 0.0;
        double parallaxX = 1.0;
        double parallaxY = 1.0;
        jsonDouble(layer, "offsetX", offsetX);
        jsonDouble(layer, "offsetY", offsetY);
        jsonDouble(layer, "parallaxX", parallaxX);
        jsonDouble(layer, "parallaxY", parallaxY);
        rt_tilemap_configure_import_layer(tilemap, index, offsetX, offsetY, parallaxX, parallaxY);
    }
}

void applyAutotilesSection(void *tilemap, void *root) {
    if (!isSeq(root))
        return;
    int64_t count = rt_seq_len(root);
    for (int64_t i = 0; i < count; ++i) {
        void *rule = rt_seq_get(root, i);
        if (!isMap(rule))
            continue;
        int64_t base = 0;
        if (!jsonInt(rule, "baseTile", base))
            continue;
        void *variants = mapGet(rule, "variants");
        if (!isSeq(variants) || rt_seq_len(variants) < 16)
            continue;
        int64_t v[16] = {};
        bool ok = true;
        for (int64_t vi = 0; vi < 16; ++vi)
            ok = ok && jsonNumberToInt(rt_seq_get(variants, vi), v[vi]);
        if (!ok)
            continue;
        rt_tilemap_set_autotile_lo(tilemap, base, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        rt_tilemap_set_autotile_hi(
            tilemap, base, v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
    }
}

/// @brief Apply typed tile-behavior state to a Tilemap copy (ADR 0176).
/// @details Values pass through unchanged; Tilemap validates addressability
///          exactly as it did for the preserved-JSON path.
void applyTypedBehavior(const SceneState &s, void *tilemap) {
    const TileBehaviorState &b = s.behavior;
    if (b.hasCollisionLayer)
        rt_tilemap_set_collision_layer(tilemap, b.collisionLayer);
    for (const auto &[tile, kind] : b.collision)
        rt_tilemap_set_collision(tilemap, tile, kind);
    for (const auto &[tile, props] : b.tileProperties) {
        for (const auto &[key, value] : props.values) {
            rt_string keyStr = makeString(key);
            rt_tilemap_set_tile_property(tilemap,
                                         tile,
                                         keyStr,
                                         value.kind == ScalarKind::Bool ? (value.boolValue ? 1 : 0)
                                                                        : value.intValue);
            rt_string_unref(keyStr);
        }
    }
    for (const auto &[base, animation] : b.animations)
        rt_tilemap_set_import_tile_anim(tilemap,
                                        base,
                                        static_cast<int64_t>(animation.frames.size()),
                                        animation.frames.data(),
                                        animation.durations.data());
    for (const auto &[base, rule] : b.autotiles) {
        const auto &v = rule.variants;
        rt_tilemap_set_autotile_lo(tilemap, base, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        rt_tilemap_set_autotile_hi(
            tilemap, base, v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
    }
}

void applyPreservedTilemapSections(const SceneState &s, void *tilemap) {
    for (const auto &[key, section] : s.preservedSections) {
        if (key != "collision" && key != "tileProperties" && key != "animations" &&
            key != "autotiles" && key != "tiledRuntime")
            continue;
        rt_string text = makeString(section.canonicalJson);
        void *root = nullptr;
        if (rt_json_try_parse(text, &root, nullptr, nullptr, nullptr) && root) {
            if (key == "collision")
                applyCollisionSection(tilemap, root);
            else if (key == "tileProperties")
                applyTilePropertiesSection(tilemap, root);
            else if (key == "animations")
                applyAnimationsSection(tilemap, root);
            else if (key == "autotiles")
                applyAutotilesSection(tilemap, root);
            else if (key == "tiledRuntime")
                applyTiledRuntimeSection(tilemap, root);
            releaseJsonValue(root);
        }
        rt_string_unref(text);
    }
}

int64_t remapMovedObjectIndex(int64_t index, int64_t from, int64_t to) {
    if (index < 0)
        return index;
    if (index == from)
        return to;
    if (from < to && index > from && index <= to)
        return index - 1;
    if (from > to && index >= to && index < from)
        return index + 1;
    return index;
}

int64_t remapInsertedObjectIndex(int64_t index, int64_t insertion) {
    return index >= insertion ? index + 1 : index;
}

} // namespace

extern "C" {

// --- Exception barrier -------------------------------------------------------
// Every entry point below runs C++ STL that can throw (heap allocation,
// std::filesystem, vector/string growth). An exception unwinding across this
// extern "C" boundary into C / VM frames is undefined behavior, so each function
// is wrapped to return a safe default instead. rt_trap() terminates the process
// (or returns via the test hook) and never throws a C++ exception, so these
// guards never mask an intentional runtime trap.
#define SCENE_TRY try
#define SCENE_CATCH(default_ret)                                                                   \
    catch (...) {                                                                                  \
        return default_ret;                                                                        \
    }
#define SCENE_CATCH_VOID                                                                           \
    catch (...) {                                                                                  \
    }

void *rt_game_scene_new(int64_t width, int64_t height, int64_t tile_width, int64_t tile_height) {
    SCENE_TRY {
        return newSceneHandle(width, height, tile_width, tile_height);
    }
    SCENE_CATCH(nullptr)
}

void *rt_game_scene_load_json(rt_string text) {
    SCENE_TRY {
        return handleFromState(loadStateFromJson(text, ""));
    }
    SCENE_CATCH(nullptr)
}

} // extern "C"

namespace {

/// @brief Return the first error-severity diagnostic message, or empty.
/// @details The `Err` message must describe the actual fatal problem. `lastError`
///          holds the newest diagnostic of *any* severity, so a non-fatal warning
///          emitted after a schema error would otherwise be chosen; selecting the
///          first error record surfaces the root cause instead (VDOC-255).
static std::string firstErrorMessage(const SceneState &s) {
    for (const auto &diag : s.diagnostics) {
        if (diag.severity == "error")
            return diag.message;
    }
    return {};
}

/// @brief Convert a loaded SceneDocument into Result.Ok or Result.ErrStr.
/// @details SceneDocument legacy load APIs return diagnostic documents on
///          malformed user input. Result APIs treat retained error diagnostics
///          as `Err(message)` while preserving warning-only documents as Ok.
/// @param scene SceneDocument handle returned by a load API.
/// @param fallback Fallback error message when the document has no error diagnostic.
/// @return Owned `Zanna.Result` carrying a SceneDocument or an error string.
static void *scene_load_to_result(void *scene, const char *fallback) {
    if (!scene)
        return rt_result_err_str(rt_const_cstr(fallback ? fallback : "SceneDocument load failed"));

    if (rt_game_scene_has_errors(scene)) {
        // Choose the first error-severity diagnostic rather than lastError (the
        // newest of any severity), so a trailing warning cannot mask the real
        // fatal error (VDOC-255).
        std::string errMsg = firstErrorMessage(*requireScene(scene)->state);
        void *result = NULL;
        if (!errMsg.empty()) {
            rt_string err = makeString(errMsg);
            result = rt_result_err_str(err);
            rt_str_release_maybe(err);
        } else {
            result =
                rt_result_err_str(rt_const_cstr(fallback ? fallback : "SceneDocument load failed"));
        }
        releaseObject(scene);
        return result;
    }

    void *result = rt_result_ok(scene);
    releaseObject(scene);
    return result;
}

} // namespace

extern "C" {

/// @brief `SceneDocument.LoadJsonResult(text)` — load scene JSON as a Result.
/// @param text Scene JSON text.
/// @return Owned `Zanna.Result` carrying a SceneDocument or an error string.
void *rt_game_scene_load_json_result(rt_string text) {
    void *scene = rt_game_scene_load_json(text);
    return scene_load_to_result(scene, "SceneDocument.LoadJson failed");
}

void *rt_game_scene_load_file(rt_string path_s) {
    SCENE_TRY {
        std::string path = toStd(path_s);
        SceneState missing;
        missing.sourcePath = path;
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            addDiagnostic(missing, "scene.load.file_missing", "error", "cannot open scene file");
            missing.layers.push_back(makeLayer(missing, "base"));
            return handleFromState(std::move(missing));
        }
        std::streamoff size = in.tellg();
        if (size > kMaxJsonBytes) {
            addDiagnostic(
                missing, "scene.schema.limit_exceeded", "error", "scene JSON exceeds 16 MiB");
            missing.layers.push_back(makeLayer(missing, "base"));
            return handleFromState(std::move(missing));
        }
        in.seekg(0, std::ios::beg);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        rt_string text = makeString(buffer.str());
        SceneState state = loadStateFromJson(text, path);
        rt_string_unref(text);
        return handleFromState(std::move(state));
    }
    SCENE_CATCH(nullptr)
}

/// @brief `SceneDocument.LoadResult(path)` — load a scene file as a Result.
/// @param path Scene file path.
/// @return Owned `Zanna.Result` carrying a SceneDocument or an error string.
void *rt_game_scene_load_file_result(rt_string path) {
    void *scene = rt_game_scene_load_file(path);
    return scene_load_to_result(scene, "SceneDocument.Load failed");
}

rt_string rt_game_scene_to_json(void *scene){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
std::ostringstream out;
writeCanonicalJson(out, s);
return makeString(out.str());
}
SCENE_CATCH(makeString(""))
}

int8_t rt_game_scene_save_file(void *scene, rt_string path_s){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
std::string pathText = toStd(path_s);
if (pathText.empty()) {
    addDiagnostic(s, "scene.save.write_failed", "error", "scene save path is empty");
    return 0;
}

std::filesystem::path target(pathText);
std::filesystem::path dir = target.parent_path();
if (dir.empty())
    dir = ".";
std::filesystem::path temp = makeSceneTempPath(dir, target);

rt_string json = rt_game_scene_to_json(scene);
if (!writeSceneJsonTempExclusive(temp, json)) {
    rt_string_unref(json);
    std::error_code removeEc;
    std::filesystem::remove(temp, removeEc);
    addDiagnostic(s, "scene.save.write_failed", "error", "cannot write temporary scene file");
    return 0;
}
rt_string_unref(json);

std::error_code ec;
if (!replaceFileWithTemp(temp, target, ec)) {
    std::error_code removeEc;
    std::filesystem::remove(temp, removeEc);
    addDiagnostic(s, "scene.save.write_failed", "error", "cannot replace scene file");
    return 0;
}
return 1;
}
SCENE_CATCH(0)
}

rt_string rt_game_scene_last_error(void *scene) {
    SCENE_TRY {
        return makeString(requireScene(scene)->state->lastError);
    }
    SCENE_CATCH(makeString(""))
}

void *rt_game_scene_diagnostics(void *scene) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        void *seq = rt_seq_new_owned();
        for (const auto &diag : s.diagnostics) {
            rt_string item = makeString(diag.message);
            rt_seq_push(seq, item);
            rt_string_unref(item);
        }
        return seq;
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void *rt_game_scene_diagnostic_records(void *scene){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
void *seq = rt_seq_new_owned();
for (const auto &diag : s.diagnostics) {
    void *map = rt_map_new();
    mapSetStrField(map, "code", diag.code);
    mapSetStrField(map, "severity", diag.severity);
    mapSetStrField(map, "message", diag.message);
    mapSetStrField(map, "path", diag.path);
    mapSetIntField(map, "line", diag.line);
    mapSetIntField(map, "column", diag.column);
    mapSetStrField(map, "source", diag.source);
    rt_seq_push(seq, map);
    releaseObject(map);
}
return seq;
}
SCENE_CATCH(rt_seq_new_owned())
}

int8_t rt_game_scene_has_errors(void *scene) {
    return hasErrors(*requireScene(scene)->state) ? 1 : 0;
}

void rt_game_scene_clear_diagnostics(void *scene) {
    SceneState &s = *requireScene(scene)->state;
    // Clear only the diagnostic queue and last-error text. The `valid` flag records
    // the document's structural validity (set during load); acknowledging messages
    // must not flip an invalid/normalized scene to valid, which would make
    // HasErrors() falsely report success without repairing the data (VDOC-254).
    s.diagnostics.clear();
    s.lastError.clear();
}

int64_t rt_game_scene_get_width(void *scene) {
    return requireScene(scene)->state->width;
}

int64_t rt_game_scene_get_height(void *scene) {
    return requireScene(scene)->state->height;
}

int64_t rt_game_scene_get_tile_width(void *scene) {
    return requireScene(scene)->state->tileWidth;
}

int64_t rt_game_scene_get_tile_height(void *scene) {
    return requireScene(scene)->state->tileHeight;
}

int64_t rt_game_scene_add_layer(void *scene, rt_string name){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (static_cast<int64_t>(s.layers.size()) >= kMaxLayers) {
    addDiagnostic(s, "scene.edit.rejected", "warning", "too many scene layers");
    return -1;
}
size_t cells = 0;
if (!checkedCellCount(s.width, s.height, cells) ||
    (static_cast<int64_t>(s.layers.size()) + 1) * static_cast<int64_t>(cells) > kMaxTotalCells) {
    addDiagnostic(s, "scene.edit.rejected", "warning", "total tile cell budget exceeded");
    return -1;
}
std::string layerName = toStd(name);
if (layerName.size() > kMaxTilemapLayerNameBytes) {
    addDiagnostic(
        s, "scene.edit.rejected", "warning", "layer name exceeds Tilemap's 31-byte limit");
    return -1;
}
s.layers.push_back(makeLayer(s, layerName));
return static_cast<int64_t>(s.layers.size()) - 1;
}
SCENE_CATCH(-1)
}

int64_t rt_game_scene_layer_count(void *scene) {
    return static_cast<int64_t>(requireScene(scene)->state->layers.size());
}

rt_string rt_game_scene_layer_name(void *scene, int64_t layer) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        return makeString(validLayer(s, layer) ? s.layers[static_cast<size_t>(layer)].name : "");
    }
    SCENE_CATCH(makeString(""))
}

void rt_game_scene_set_layer_name(void *scene, int64_t layer, rt_string name){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (!validLayer(s, layer))
    return;
std::string layerName = toStd(name);
if (layerName.size() > kMaxTilemapLayerNameBytes) {
    addDiagnostic(
        s, "scene.edit.rejected", "warning", "layer name exceeds Tilemap's 31-byte limit");
    return;
}
s.layers[static_cast<size_t>(layer)].name = std::move(layerName);
}
SCENE_CATCH_VOID
}

int8_t rt_game_scene_layer_visible(void *scene, int64_t layer) {
    SceneState &s = *requireScene(scene)->state;
    return validLayer(s, layer) && s.layers[static_cast<size_t>(layer)].visible ? 1 : 0;
}

void rt_game_scene_set_layer_visible(void *scene, int64_t layer, int8_t visible) {
    SceneState &s = *requireScene(scene)->state;
    if (validLayer(s, layer))
        s.layers[static_cast<size_t>(layer)].visible = visible != 0;
}

void rt_game_scene_move_layer(void *scene, int64_t from, int64_t to) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!validLayer(s, from) || !validLayer(s, to) || from == to)
            return;
        Layer layer = std::move(s.layers[static_cast<size_t>(from)]);
        s.layers.erase(s.layers.begin() + from);
        s.layers.insert(s.layers.begin() + to, std::move(layer));
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_remove_layer(void *scene, int64_t layer) {
    SceneState &s = *requireScene(scene)->state;
    if (s.layers.size() <= 1 || !validLayer(s, layer))
        return;
    s.layers.erase(s.layers.begin() + layer);
}

int64_t rt_game_scene_get_tile(void *scene, int64_t layer, int64_t x, int64_t y) {
    SceneState &s = *requireScene(scene)->state;
    if (!validLayer(s, layer) || !validTile(s, x, y))
        return 0;
    return s.layers[static_cast<size_t>(layer)].tiles[static_cast<size_t>(y * s.width + x)];
}

void rt_game_scene_set_tile(void *scene, int64_t layer, int64_t x, int64_t y, int64_t tile) {
    SceneState &s = *requireScene(scene)->state;
    if (!validLayer(s, layer) || !validTile(s, x, y))
        return;
    s.layers[static_cast<size_t>(layer)].tiles[static_cast<size_t>(y * s.width + x)] = tile;
}

void rt_game_scene_fill_tiles(
    void *scene, int64_t layer, int64_t x, int64_t y, int64_t w, int64_t h, int64_t tile) {
    SceneState &s = *requireScene(scene)->state;
    if (!validLayer(s, layer) || w <= 0 || h <= 0)
        return;
    if (x > std::numeric_limits<int64_t>::max() - w || y > std::numeric_limits<int64_t>::max() - h)
        return;
    int64_t xEnd = std::min<int64_t>(s.width, x + w);
    int64_t yEnd = std::min<int64_t>(s.height, y + h);
    int64_t xStart = std::max<int64_t>(0, x);
    int64_t yStart = std::max<int64_t>(0, y);
    for (int64_t yy = yStart; yy < yEnd; ++yy)
        for (int64_t xx = xStart; xx < xEnd; ++xx)
            s.layers[static_cast<size_t>(layer)].tiles[static_cast<size_t>(yy * s.width + xx)] =
                tile;
}

int64_t rt_game_scene_flood_fill_tiles(
    void *scene, int64_t layer, int64_t x, int64_t y, int64_t tile) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!validLayer(s, layer) || !validTile(s, x, y))
            return 0;

        std::vector<int64_t> &tiles = s.layers[static_cast<size_t>(layer)].tiles;
        const size_t rowWidth = static_cast<size_t>(s.width);
        const size_t start = static_cast<size_t>(y) * rowWidth + static_cast<size_t>(x);
        const int64_t sourceTile = tiles[start];
        if (sourceTile == tile)
            return 0;

        // Allocate the complete bounded work set before the first write. The
        // queue cannot grow after this point, so allocation failure is atomic.
        std::vector<size_t> queue(tiles.size());
        std::vector<uint8_t> queued(tiles.size(), 0);
        size_t head = 0;
        size_t tail = 0;
        queue[tail++] = start;
        queued[start] = 1;

        auto enqueue = [&](size_t index) {
            if (!queued[index] && tiles[index] == sourceTile) {
                queued[index] = 1;
                queue[tail++] = index;
            }
        };

        int64_t changed = 0;
        while (head < tail) {
            const size_t current = queue[head++];
            tiles[current] = tile;
            ++changed;

            const size_t column = current % rowWidth;
            if (column > 0)
                enqueue(current - 1);
            if (column + 1 < rowWidth)
                enqueue(current + 1);
            if (current >= rowWidth)
                enqueue(current - rowWidth);
            if (current + rowWidth < tiles.size())
                enqueue(current + rowWidth);
        }
        return changed;
    }
    SCENE_CATCH(0)
}

void rt_game_scene_set_layer_asset(void *scene, int64_t layer, rt_string asset_path){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (validLayer(s, layer))
    s.layers[static_cast<size_t>(layer)].asset = toStd(asset_path);
}
SCENE_CATCH_VOID
}

rt_string rt_game_scene_layer_asset(void *scene, int64_t layer){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
return makeString(validLayer(s, layer) ? s.layers[static_cast<size_t>(layer)].asset : "");
}
SCENE_CATCH(makeString(""))
}

int64_t rt_game_scene_add_object(void *scene, rt_string type, rt_string id, int64_t x, int64_t y){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (static_cast<int64_t>(s.objects.size()) >= kMaxObjects) {
    addDiagnostic(s, "scene.edit.rejected", "warning", "too many scene objects");
    return -1;
}
s.objects.push_back(Object{toStd(type), toStd(id), x, y, -1, {}});
return static_cast<int64_t>(s.objects.size()) - 1;
}
SCENE_CATCH(-1)
}

int64_t rt_game_scene_object_count(void *scene) {
    return static_cast<int64_t>(requireScene(scene)->state->objects.size());
}

void rt_game_scene_remove_object(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    if (!validObjectIndex(s, index))
        return;

    const int64_t removedParent = s.objects[static_cast<size_t>(index)].parent;
    for (Object &object : s.objects) {
        if (object.parent == index)
            object.parent = removedParent;
        if (object.parent > index)
            --object.parent;
    }
    s.objects.erase(s.objects.begin() + index);
}

rt_string rt_game_scene_object_type(void *scene, int64_t index){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
return makeString(validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].type : "");
}
SCENE_CATCH(makeString(""))
}

rt_string rt_game_scene_object_id(void *scene, int64_t index){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
return makeString(validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].id : "");
}
SCENE_CATCH(makeString(""))
}

int64_t rt_game_scene_object_x(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].x : 0;
}

int64_t rt_game_scene_object_y(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].y : 0;
}

int64_t rt_game_scene_object_parent(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].parent : -1;
}

int8_t rt_game_scene_try_set_object_parent(void *scene, int64_t index, int64_t parent) {
    SceneState &s = *requireScene(scene)->state;
    if (!validObjectIndex(s, index) || (parent != -1 && !validObjectIndex(s, parent)) ||
        parent == index)
        return 0;

    Object &object = s.objects[static_cast<size_t>(index)];
    if (parent >= 0 && object.properties.size() >= static_cast<size_t>(kMaxObjectProperties)) {
        addDiagnostic(
            s, "scene.edit.rejected", "warning", "object hierarchy requires one property slot");
        return 0;
    }

    int64_t ancestor = parent;
    for (size_t depth = 0; ancestor >= 0 && depth < s.objects.size(); ++depth) {
        if (ancestor == index)
            return 0;
        if (!validObjectIndex(s, ancestor))
            return 0;
        ancestor = s.objects[static_cast<size_t>(ancestor)].parent;
    }
    if (ancestor >= 0)
        return 0;

    object.parent = parent;
    return 1;
}

void rt_game_scene_set_object_metadata(void *scene, int64_t index, rt_string type, rt_string id) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!validObjectIndex(s, index))
            return;

        std::string next_type = toStd(type);
        std::string next_id = toStd(id);
        Object &object = s.objects[static_cast<size_t>(index)];
        object.type = std::move(next_type);
        object.id = std::move(next_id);
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_set_object_position(void *scene, int64_t index, int64_t x, int64_t y) {
    SceneState &s = *requireScene(scene)->state;
    if (validObjectIndex(s, index)) {
        s.objects[static_cast<size_t>(index)].x = x;
        s.objects[static_cast<size_t>(index)].y = y;
    }
}

int64_t rt_game_scene_duplicate_object(void *scene, int64_t index, rt_string id) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!validObjectIndex(s, index))
            return -1;
        if (static_cast<int64_t>(s.objects.size()) >= kMaxObjects) {
            addDiagnostic(s, "scene.edit.rejected", "warning", "too many scene objects");
            return -1;
        }

        std::string next_id = toStd(id);
        Object duplicate = s.objects[static_cast<size_t>(index)];
        duplicate.id = std::move(next_id);
        int64_t duplicate_index = index + 1;
        for (Object &object : s.objects)
            object.parent = remapInsertedObjectIndex(object.parent, duplicate_index);
        duplicate.parent = remapInsertedObjectIndex(duplicate.parent, duplicate_index);
        s.objects.insert(s.objects.begin() + duplicate_index, std::move(duplicate));
        return duplicate_index;
    }
    SCENE_CATCH(-1)
}

void rt_game_scene_set_object_property(void *scene, int64_t index, rt_string key, rt_string value) {
    rt_game_scene_object_set_str(scene, index, key, value);
}

rt_string rt_game_scene_get_object_property(void *scene, int64_t index, rt_string key) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!validObjectIndex(s, index))
            return makeString("");
        const SceneScalar *scalar =
            findScalar(s.objects[static_cast<size_t>(index)].properties, key);
        return makeString(scalar ? scalarToString(*scalar) : "");
    }
    SCENE_CATCH(makeString(""))
}

void rt_game_scene_delete_object_property(void *scene, int64_t index, rt_string key) {
    rt_game_scene_object_remove(scene, index, key);
}

int64_t rt_game_scene_object_get_int(void *scene, int64_t index, rt_string key, int64_t def){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (!validObjectIndex(s, index))
    return def;
const SceneScalar *scalar = findScalar(s.objects[static_cast<size_t>(index)].properties, key);
return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
}
SCENE_CATCH(def)
}

rt_string rt_game_scene_object_get_str(void *scene, int64_t index, rt_string key, rt_string def) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!validObjectIndex(s, index))
            return makeString(toStd(def));
        const SceneScalar *scalar =
            findScalar(s.objects[static_cast<size_t>(index)].properties, key);
        return makeString(scalar && scalar->kind == ScalarKind::String ? scalar->stringValue
                                                                       : toStd(def));
    }
    SCENE_CATCH(makeString(""))
}

double rt_game_scene_object_get_float(void *scene, int64_t index, rt_string key, double def){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (!validObjectIndex(s, index))
    return def;
const SceneScalar *scalar = findScalar(s.objects[static_cast<size_t>(index)].properties, key);
if (!scalar)
    return def;
if (scalar->kind == ScalarKind::Float)
    return scalar->floatValue;
if (scalar->kind == ScalarKind::Int)
    return static_cast<double>(scalar->intValue);
return def;
}
SCENE_CATCH(def)
}

int8_t rt_game_scene_object_get_bool(void *scene, int64_t index, rt_string key, int8_t def){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (!validObjectIndex(s, index))
    return def;
const SceneScalar *scalar = findScalar(s.objects[static_cast<size_t>(index)].properties, key);
return scalar && scalar->kind == ScalarKind::Bool ? (scalar->boolValue ? 1 : 0) : def;
}
SCENE_CATCH(def)
}

int8_t rt_game_scene_object_has(void *scene, int64_t index, rt_string key){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
return validObjectIndex(s, index) &&
               findScalar(s.objects[static_cast<size_t>(index)].properties, key)
           ? 1
           : 0;
}
SCENE_CATCH(0)
}

rt_string rt_game_scene_object_property_kind(void *scene, int64_t index, rt_string key) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!validObjectIndex(s, index))
            return makeString("");
        const SceneScalar *scalar =
            findScalar(s.objects[static_cast<size_t>(index)].properties, key);
        return makeString(scalar ? scalarKindName(scalar->kind) : "");
    }
    SCENE_CATCH(makeString(""))
}

void *rt_game_scene_object_keys(void *scene, int64_t index) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        void *seq = rt_seq_new_owned();
        if (!validObjectIndex(s, index))
            return seq;
        for (const auto &[key, _] : s.objects[static_cast<size_t>(index)].properties) {
            rt_string item = makeString(key);
            rt_seq_push(seq, item);
            rt_string_unref(item);
        }
        return seq;
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void rt_game_scene_object_set_null(void *scene, int64_t index, rt_string key) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (validObjectIndex(s, index) && !isReservedObjectProperty(key)) {
            Object &object = s.objects[static_cast<size_t>(index)];
            setScalar(
                object.properties, s, key, makeNullScalar(), maxPublicObjectProperties(object));
        }
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_object_set_int(void *scene, int64_t index, rt_string key, int64_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (validObjectIndex(s, index) && !isReservedObjectProperty(key)) {
            Object &object = s.objects[static_cast<size_t>(index)];
            setScalar(
                object.properties, s, key, makeIntScalar(value), maxPublicObjectProperties(object));
        }
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_object_set_str(void *scene, int64_t index, rt_string key, rt_string value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (validObjectIndex(s, index) && !isReservedObjectProperty(key)) {
            Object &object = s.objects[static_cast<size_t>(index)];
            setScalar(object.properties,
                      s,
                      key,
                      makeStringScalar(toStd(value)),
                      maxPublicObjectProperties(object));
        }
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_object_set_float(void *scene, int64_t index, rt_string key, double value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (validObjectIndex(s, index) && !isReservedObjectProperty(key) && std::isfinite(value)) {
            Object &object = s.objects[static_cast<size_t>(index)];
            setScalar(object.properties,
                      s,
                      key,
                      makeFloatScalar(value),
                      maxPublicObjectProperties(object));
        }
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_object_set_bool(void *scene, int64_t index, rt_string key, int8_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (validObjectIndex(s, index) && !isReservedObjectProperty(key)) {
            Object &object = s.objects[static_cast<size_t>(index)];
            setScalar(object.properties,
                      s,
                      key,
                      makeBoolScalar(value != 0),
                      maxPublicObjectProperties(object));
        }
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_object_remove(void *scene, int64_t index, rt_string key){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (validObjectIndex(s, index) && !isReservedObjectProperty(key))
    s.objects[static_cast<size_t>(index)].properties.erase(toStd(key));
}
SCENE_CATCH_VOID
}

int64_t rt_game_scene_count_of_type(void *scene, rt_string type){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
std::string target = toStd(type);
int64_t count = 0;
for (const auto &obj : s.objects) {
    if (obj.type == target)
        ++count;
}
return count;
}
SCENE_CATCH(0)
}

int64_t rt_game_scene_object_of_type(void *scene, rt_string type, int64_t n){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
std::string target = toStd(type);
int64_t seen = 0;
for (size_t i = 0; i < s.objects.size(); ++i) {
    if (s.objects[i].type != target)
        continue;
    if (seen == n)
        return static_cast<int64_t>(i);
    ++seen;
}
return -1;
}
SCENE_CATCH(-1)
}

int64_t rt_game_scene_find_object(void *scene, rt_string id) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        std::string target = toStd(id);
        for (size_t i = 0; i < s.objects.size(); ++i) {
            if (s.objects[i].id == target)
                return static_cast<int64_t>(i);
        }
        return -1;
    }
    SCENE_CATCH(-1)
}

/// @brief Find an object by id and return its index as an Option.
/// @details Sentinel-free companion to @ref rt_game_scene_find_object. A match
///          returns `SomeI64(index)`, while absence returns None.
/// @param scene SceneDocument handle.
/// @param id Object id to search for.
/// @return Opaque Zanna.Option containing the object index, or None.
void *rt_game_scene_find_object_option(void *scene, rt_string id) {
    int64_t index = rt_game_scene_find_object(scene, id);
    return index >= 0 ? rt_option_some_i64(index) : rt_option_none();
}

void rt_game_scene_move_object(void *scene, int64_t from, int64_t to) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!validObjectIndex(s, from) || !validObjectIndex(s, to) || from == to)
            return;
        for (Object &object : s.objects)
            object.parent = remapMovedObjectIndex(object.parent, from, to);
        Object obj = std::move(s.objects[static_cast<size_t>(from)]);
        s.objects.erase(s.objects.begin() + from);
        s.objects.insert(s.objects.begin() + to, std::move(obj));
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_set_property(void *scene, rt_string key, rt_string value) {
    rt_game_scene_set_str(scene, key, value);
}

rt_string rt_game_scene_get_property(void *scene, rt_string key) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        const SceneScalar *scalar = findScalar(s.properties, key);
        return makeString(scalar ? scalarToString(*scalar) : "");
    }
    SCENE_CATCH(makeString(""))
}

void rt_game_scene_delete_property(void *scene, rt_string key) {
    rt_game_scene_remove(scene, key);
}

int64_t rt_game_scene_get_int(void *scene, rt_string key, int64_t def){
    SCENE_TRY{const SceneScalar *scalar = findScalar(requireScene(scene) -> state->properties, key);
return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
}
SCENE_CATCH(def)
}

rt_string rt_game_scene_get_str(void *scene, rt_string key, rt_string def) {
    SCENE_TRY {
        const SceneScalar *scalar = findScalar(requireScene(scene)->state->properties, key);
        return makeString(scalar && scalar->kind == ScalarKind::String ? scalar->stringValue
                                                                       : toStd(def));
    }
    SCENE_CATCH(makeString(""))
}

double rt_game_scene_get_float(void *scene, rt_string key, double def){
    SCENE_TRY{const SceneScalar *scalar = findScalar(requireScene(scene) -> state->properties, key);
if (!scalar)
    return def;
if (scalar->kind == ScalarKind::Float)
    return scalar->floatValue;
if (scalar->kind == ScalarKind::Int)
    return static_cast<double>(scalar->intValue);
return def;
}
SCENE_CATCH(def)
}

int8_t rt_game_scene_get_bool(void *scene, rt_string key, int8_t def){
    SCENE_TRY{const SceneScalar *scalar = findScalar(requireScene(scene) -> state->properties, key);
return scalar && scalar->kind == ScalarKind::Bool ? (scalar->boolValue ? 1 : 0) : def;
}
SCENE_CATCH(def)
}

int8_t rt_game_scene_has(void *scene, rt_string key){
    SCENE_TRY{return findScalar(requireScene(scene) -> state->properties, key) ? 1 : 0;
}
SCENE_CATCH(0)
}

rt_string rt_game_scene_property_kind(void *scene, rt_string key) {
    SCENE_TRY {
        const SceneScalar *scalar = findScalar(requireScene(scene)->state->properties, key);
        return makeString(scalar ? scalarKindName(scalar->kind) : "");
    }
    SCENE_CATCH(makeString(""))
}

void *rt_game_scene_keys(void *scene) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        void *seq = rt_seq_new_owned();
        for (const auto &[key, _] : s.properties) {
            rt_string item = makeString(key);
            rt_seq_push(seq, item);
            rt_string_unref(item);
        }
        return seq;
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void rt_game_scene_set_null(void *scene, rt_string key) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setScalar(s.properties, s, key, makeNullScalar(), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_set_int(void *scene, rt_string key, int64_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setScalar(s.properties, s, key, makeIntScalar(value), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_set_str(void *scene, rt_string key, rt_string value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setScalar(s.properties, s, key, makeStringScalar(toStd(value)), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_set_float(void *scene, rt_string key, double value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (std::isfinite(value))
            setScalar(s.properties, s, key, makeFloatScalar(value), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_set_bool(void *scene, rt_string key, int8_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setScalar(s.properties, s, key, makeBoolScalar(value != 0), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_remove(void *scene, rt_string key) {
    SCENE_TRY {
        requireScene(scene)->state->properties.erase(toStd(key));
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_asset_descriptors(void *scene) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        void *seq = rt_seq_new_owned();
        for (const auto &desc : collectAssetDescriptors(s)) {
            void *map = descriptorMap(desc);
            rt_seq_push(seq, map);
            releaseObject(map);
        }
        return seq;
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void *rt_game_scene_asset_paths(void *scene) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        std::set<std::string> unique;
        for (const auto &desc : collectAssetDescriptors(s))
            unique.insert(desc.path);
        void *seq = rt_seq_new_owned();
        for (const auto &path : unique) {
            rt_string item = makeString(path);
            rt_seq_push(seq, item);
            rt_string_unref(item);
        }
        return seq;
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void *rt_game_scene_build_tilemap(void *scene) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        void *tilemap = rt_tilemap_new(s.width, s.height, s.tileWidth, s.tileHeight);
        for (size_t li = 0; li < s.layers.size(); ++li) {
            int64_t targetLayer = static_cast<int64_t>(li);
            if (li > 0) {
                rt_string name = makeString(s.layers[li].name);
                targetLayer = rt_tilemap_add_layer(tilemap, name);
                rt_string_unref(name);
                if (targetLayer < 0)
                    continue;
            }
            rt_tilemap_set_layer_visible(tilemap, targetLayer, s.layers[li].visible ? 1 : 0);
            for (int64_t y = 0; y < s.height; ++y) {
                for (int64_t x = 0; x < s.width; ++x) {
                    size_t idx = static_cast<size_t>(y * s.width + x);
                    if (idx < s.layers[li].tiles.size())
                        rt_tilemap_set_tile_layer(
                            tilemap, targetLayer, x, y, s.layers[li].tiles[idx]);
                }
            }
        }
        applyTypedBehavior(s, tilemap);
        applyPreservedTilemapSections(s, tilemap);
        return tilemap;
    }
    SCENE_CATCH(nullptr)
}

} // extern "C"

namespace {

/// @brief Validate a setter-facing behavior tile ID (ADR 0176).
bool editableBehaviorTileId(SceneState &s, int64_t tile) {
    if (tile >= kMinBehaviorTileId && tile <= kMaxBehaviorTileId)
        return true;
    addDiagnostic(s, "scene.edit.rejected", "warning", "tile id must be between 1 and 4095");
    return false;
}

/// @brief Replace a same-key malformed preserved section when typed authoring begins.
void claimTypedSection(SceneState &s, const char *key) {
    auto it = s.preservedSections.find(key);
    if (it == s.preservedSections.end())
        return;
    addDiagnostic(s,
                  "scene.edit.rejected",
                  "warning",
                  std::string(key) + " section had an invalid shape and was replaced");
    s.preservedSections.erase(it);
}

void pushIntValue(void *seq, int64_t value) {
    void *boxed = rt_box_i64(value);
    rt_seq_push(seq, boxed);
    releaseObject(boxed);
}

void *ascendingKeySeq(const std::map<int64_t, int64_t> &map) {
    void *seq = rt_seq_new_owned();
    for (const auto &[key, _] : map)
        pushIntValue(seq, key);
    return seq;
}

void *intVectorSeq(const std::vector<int64_t> &values) {
    void *seq = rt_seq_new_owned();
    for (int64_t value : values)
        pushIntValue(seq, value);
    return seq;
}

void setTilePropertyScalar(void *scene, int64_t tile, rt_string key, SceneScalar value) {
    SceneState &s = *requireScene(scene)->state;
    if (!editableBehaviorTileId(s, tile))
        return;
    std::string k = toStd(key);
    if (!validKey(k)) {
        addDiagnostic(s, "scene.edit.rejected", "warning", "property key exceeds 128 bytes");
        return;
    }
    TilePropertySet &target = s.behavior.tileProperties[tile];
    if (target.values.find(k) == target.values.end()) {
        int64_t totalEntries = 0;
        for (const auto &[_, props] : s.behavior.tileProperties)
            totalEntries += static_cast<int64_t>(props.values.size());
        if (target.values.size() >= static_cast<size_t>(kMaxTilePropertiesPerTile) ||
            totalEntries >= kMaxTilePropertyEntries) {
            addDiagnostic(s, "scene.edit.rejected", "warning", "too many tile properties");
            if (target.empty())
                s.behavior.tileProperties.erase(tile);
            return;
        }
    }
    claimTypedSection(s, "tileProperties");
    target.values[k] = std::move(value);
}

/// @brief Read one Zia Seq of integers; false on non-seq or non-integer entries.
bool readIntSeq(void *seq, std::vector<int64_t> &out) {
    if (!isSeq(seq))
        return false;
    int64_t len = rt_seq_len(seq);
    out.clear();
    out.reserve(static_cast<size_t>(len));
    for (int64_t i = 0; i < len; ++i) {
        int64_t value = 0;
        if (!jsonNumberToInt(rt_seq_get(seq, i), value))
            return false;
        out.push_back(value);
    }
    return true;
}

bool rejectSectionEdit(SceneState &s, const char *message) {
    addDiagnostic(s, "scene.edit.rejected", "warning", message);
    return false;
}

bool intFieldInRange(const SceneScalar &value, int64_t min, int64_t max) {
    return value.kind == ScalarKind::Int && value.intValue >= min && value.intValue <= max;
}

/// @brief Validate the known camera-section fields of ADR 0177; unknown keys pass.
bool validCameraField(SceneState &s, const std::string &key, const SceneScalar &value) {
    if (key == "mode") {
        if (value.kind != ScalarKind::String ||
            (value.stringValue != "follow" && value.stringValue != "fixed"))
            return rejectSectionEdit(s, "camera mode must be \"follow\" or \"fixed\"");
        return true;
    }
    if (key == "minX" || key == "minY" || key == "maxX" || key == "maxY") {
        if (value.kind != ScalarKind::Int)
            return rejectSectionEdit(s, "camera bounds must be integers");
        return true;
    }
    if (key == "deadzoneWidth" || key == "deadzoneHeight") {
        if (!intFieldInRange(value, 0, std::numeric_limits<int64_t>::max()))
            return rejectSectionEdit(s, "camera deadzone must be a non-negative integer");
        return true;
    }
    if (key == "followLerpPct") {
        if (!intFieldInRange(value, 1, 100))
            return rejectSectionEdit(s, "camera followLerpPct must be between 1 and 100");
        return true;
    }
    if (key == "zoomPct") {
        if (!intFieldInRange(value, 10, 1000))
            return rejectSectionEdit(s, "camera zoomPct must be between 10 and 1000");
        return true;
    }
    return true;
}

/// @brief Validate the known lighting-section fields of ADR 0177; unknown keys pass.
bool validLightingField(SceneState &s, const std::string &key, const SceneScalar &value) {
    if (key == "darkness") {
        if (!intFieldInRange(value, 0, 255))
            return rejectSectionEdit(s, "lighting darkness must be between 0 and 255");
        return true;
    }
    if (key == "tint" || key == "playerLightColor") {
        if (!intFieldInRange(value, 0, 4294967295))
            return rejectSectionEdit(s, "lighting colors must be 32-bit ARGB integers");
        return true;
    }
    if (key == "playerLightRadius") {
        if (!intFieldInRange(value, 0, 4096))
            return rejectSectionEdit(s, "lighting playerLightRadius must be between 0 and 4096");
        return true;
    }
    return true;
}

void setSectionScalar(SceneState &s,
                      SectionScalarState &section,
                      rt_string key,
                      SceneScalar value) {
    std::string k = toStd(key);
    if (!validKey(k)) {
        addDiagnostic(s, "scene.edit.rejected", "warning", "property key exceeds 128 bytes");
        return;
    }
    if (value.kind == ScalarKind::String && !validStringValue(value.stringValue)) {
        addDiagnostic(s, "scene.edit.rejected", "warning", "string property value exceeds 64 KiB");
        return;
    }
    if (&section == &s.camera && !validCameraField(s, k, value))
        return;
    if (&section == &s.lighting && !validLightingField(s, k, value))
        return;
    claimTypedSection(s, &section == &s.camera ? "camera" : "lighting");
    section.fields[k] = std::move(value);
}

const SceneScalar *findSectionScalar(const SectionScalarState &section, rt_string key) {
    auto it = section.fields.find(toStd(key));
    return it == section.fields.end() ? nullptr : &it->second;
}

void *sectionKeySeq(const SectionScalarState &section) {
    void *seq = rt_seq_new_owned();
    for (const auto &[key, _] : section.fields) {
        rt_string item = makeString(key);
        rt_seq_push(seq, item);
        rt_string_unref(item);
    }
    return seq;
}

} // namespace

extern "C" {

int64_t rt_game_scene_tile_collision(void *scene, int64_t tile) {
    SCENE_TRY {
        const auto &collision = requireScene(scene)->state->behavior.collision;
        auto it = collision.find(tile);
        return it == collision.end() ? RT_TILE_COLLISION_NONE : it->second;
    }
    SCENE_CATCH(RT_TILE_COLLISION_NONE)
}

void rt_game_scene_set_tile_collision(void *scene, int64_t tile, int64_t kind) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!editableBehaviorTileId(s, tile))
            return;
        if (kind < RT_TILE_COLLISION_NONE || kind > RT_TILE_COLLISION_ONE_WAY_UP) {
            addDiagnostic(s, "scene.edit.rejected", "warning", "collision kind must be 0, 1, or 2");
            return;
        }
        claimTypedSection(s, "collision");
        if (kind == RT_TILE_COLLISION_NONE)
            s.behavior.collision.erase(tile);
        else
            s.behavior.collision[tile] = kind;
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_collision_tiles(void *scene){
    SCENE_TRY{return ascendingKeySeq(requireScene(scene) -> state->behavior.collision);
}
SCENE_CATCH(rt_seq_new_owned())
}

int64_t rt_game_scene_collision_layer(void *scene) {
    SCENE_TRY {
        return requireScene(scene)->state->behavior.collisionLayer;
    }
    SCENE_CATCH(0)
}

void rt_game_scene_set_collision_layer(void *scene, int64_t layer){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (layer < 0 || layer >= kMaxLayers) {
    addDiagnostic(s, "scene.edit.rejected", "warning", "collision layer must be between 0 and 15");
    return;
}
claimTypedSection(s, "collision");
s.behavior.collisionLayer = layer;
s.behavior.hasCollisionLayer = true;
}
SCENE_CATCH_VOID
}

rt_string rt_game_scene_tile_property_kind(void *scene, int64_t tile, rt_string key){
    SCENE_TRY{const auto &tiles = requireScene(scene) -> state->behavior.tileProperties;
auto it = tiles.find(tile);
if (it == tiles.end())
    return makeString("");
const SceneScalar *scalar = findScalar(it->second.values, key);
return makeString(scalar ? scalarKindName(scalar->kind) : "");
}
SCENE_CATCH(makeString(""))
}

int64_t rt_game_scene_tile_property_int(void *scene, int64_t tile, rt_string key, int64_t def){
    SCENE_TRY{const auto &tiles = requireScene(scene) -> state->behavior.tileProperties;
auto it = tiles.find(tile);
if (it == tiles.end())
    return def;
const SceneScalar *scalar = findScalar(it->second.values, key);
return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
}
SCENE_CATCH(def)
}

int8_t rt_game_scene_tile_property_bool(void *scene, int64_t tile, rt_string key, int8_t def) {
    SCENE_TRY {
        const auto &tiles = requireScene(scene)->state->behavior.tileProperties;
        auto it = tiles.find(tile);
        if (it == tiles.end())
            return def;
        const SceneScalar *scalar = findScalar(it->second.values, key);
        return scalar && scalar->kind == ScalarKind::Bool ? (scalar->boolValue ? 1 : 0) : def;
    }
    SCENE_CATCH(def)
}

void rt_game_scene_set_tile_property_int(void *scene, int64_t tile, rt_string key, int64_t value) {
    SCENE_TRY {
        setTilePropertyScalar(scene, tile, key, makeIntScalar(value));
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_set_tile_property_bool(void *scene, int64_t tile, rt_string key, int8_t value) {
    SCENE_TRY {
        setTilePropertyScalar(scene, tile, key, makeBoolScalar(value != 0));
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_remove_tile_property(void *scene, int64_t tile, rt_string key) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        auto it = s.behavior.tileProperties.find(tile);
        if (it == s.behavior.tileProperties.end())
            return;
        it->second.values.erase(toStd(key));
        if (it->second.empty())
            s.behavior.tileProperties.erase(it);
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_tile_property_keys(void *scene, int64_t tile) {
    SCENE_TRY {
        const auto &tiles = requireScene(scene)->state->behavior.tileProperties;
        void *seq = rt_seq_new_owned();
        auto it = tiles.find(tile);
        if (it != tiles.end()) {
            for (const auto &[key, _] : it->second.values) {
                rt_string item = makeString(key);
                rt_seq_push(seq, item);
                rt_string_unref(item);
            }
        }
        return seq;
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void *rt_game_scene_tile_property_tiles(void *scene) {
    SCENE_TRY {
        const auto &tiles = requireScene(scene)->state->behavior.tileProperties;
        void *seq = rt_seq_new_owned();
        for (const auto &[tile, props] : tiles) {
            if (!props.values.empty())
                pushIntValue(seq, tile);
        }
        return seq;
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void rt_game_scene_set_tile_anim(void *scene, int64_t base_tile, void *frames, void *durations_ms) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!editableBehaviorTileId(s, base_tile))
            return;
        std::vector<int64_t> frameValues;
        std::vector<int64_t> durationValues;
        if (!readIntSeq(frames, frameValues) || !readIntSeq(durations_ms, durationValues)) {
            addDiagnostic(s,
                          "scene.edit.rejected",
                          "warning",
                          "tile animation frames and durations must be integer sequences");
            return;
        }
        if (frameValues.empty() ||
            frameValues.size() > static_cast<size_t>(kMaxAnimFramesPerRule) ||
            frameValues.size() != durationValues.size()) {
            addDiagnostic(s,
                          "scene.edit.rejected",
                          "warning",
                          "tile animation needs 1..4096 frames with matching durations");
            return;
        }
        for (int64_t duration : durationValues) {
            if (duration <= 0) {
                addDiagnostic(s,
                              "scene.edit.rejected",
                              "warning",
                              "tile animation durations must be positive");
                return;
            }
        }
        int64_t totalFrames = 0;
        for (const auto &[base, animation] : s.behavior.animations) {
            if (base != base_tile)
                totalFrames += static_cast<int64_t>(animation.frames.size());
        }
        if (totalFrames + static_cast<int64_t>(frameValues.size()) > kMaxTotalAnimFrames) {
            addDiagnostic(s, "scene.edit.rejected", "warning", "too many tile animation frames");
            return;
        }
        claimTypedSection(s, "animations");
        TileAnimation &animation = s.behavior.animations[base_tile];
        animation.frames = std::move(frameValues);
        animation.durations = std::move(durationValues);
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_tile_anim_frames(void *scene, int64_t base_tile) {
    SCENE_TRY {
        const auto &animations = requireScene(scene)->state->behavior.animations;
        auto it = animations.find(base_tile);
        return it == animations.end() ? rt_seq_new_owned() : intVectorSeq(it->second.frames);
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void *rt_game_scene_tile_anim_durations(void *scene, int64_t base_tile) {
    SCENE_TRY {
        const auto &animations = requireScene(scene)->state->behavior.animations;
        auto it = animations.find(base_tile);
        return it == animations.end() ? rt_seq_new_owned() : intVectorSeq(it->second.durations);
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void rt_game_scene_remove_tile_anim(void *scene, int64_t base_tile) {
    SCENE_TRY {
        requireScene(scene)->state->behavior.animations.erase(base_tile);
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_tile_anim_bases(void *scene) {
    SCENE_TRY {
        const auto &animations = requireScene(scene)->state->behavior.animations;
        void *seq = rt_seq_new_owned();
        for (const auto &[base, _] : animations)
            pushIntValue(seq, base);
        return seq;
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void rt_game_scene_set_autotile_rule(void *scene, int64_t base_tile, void *variants) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (!editableBehaviorTileId(s, base_tile))
            return;
        std::vector<int64_t> values;
        if (!readIntSeq(variants, values) || values.size() != kAutotileVariantCount) {
            addDiagnostic(s,
                          "scene.edit.rejected",
                          "warning",
                          "autotile rule needs exactly 16 integer variant tile ids");
            return;
        }
        claimTypedSection(s, "autotiles");
        AutotileRule &rule = s.behavior.autotiles[base_tile];
        std::copy(values.begin(), values.end(), rule.variants.begin());
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_autotile_variants(void *scene, int64_t base_tile) {
    SCENE_TRY {
        const auto &autotiles = requireScene(scene)->state->behavior.autotiles;
        auto it = autotiles.find(base_tile);
        if (it == autotiles.end())
            return rt_seq_new_owned();
        return intVectorSeq({it->second.variants.begin(), it->second.variants.end()});
    }
    SCENE_CATCH(rt_seq_new_owned())
}

void rt_game_scene_remove_autotile_rule(void *scene, int64_t base_tile) {
    SCENE_TRY {
        requireScene(scene)->state->behavior.autotiles.erase(base_tile);
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_autotile_bases(void *scene){
    SCENE_TRY{const auto &autotiles = requireScene(scene) -> state->behavior.autotiles;
void *seq = rt_seq_new_owned();
for (const auto &[base, _] : autotiles)
    pushIntValue(seq, base);
return seq;
}
SCENE_CATCH(rt_seq_new_owned())
}

rt_string rt_game_scene_camera_field_kind(void *scene, rt_string key){SCENE_TRY{
    const SceneScalar *scalar = findSectionScalar(requireScene(scene) -> state->camera, key);
return makeString(scalar ? scalarKindName(scalar->kind) : "");
}
SCENE_CATCH(makeString(""))
}

int64_t rt_game_scene_camera_get_int(void *scene, rt_string key, int64_t def){SCENE_TRY{
    const SceneScalar *scalar = findSectionScalar(requireScene(scene) -> state->camera, key);
return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
}
SCENE_CATCH(def)
}

rt_string rt_game_scene_camera_get_str(void *scene, rt_string key, rt_string def) {
    SCENE_TRY {
        const SceneScalar *scalar = findSectionScalar(requireScene(scene)->state->camera, key);
        return makeString(scalar && scalar->kind == ScalarKind::String ? scalar->stringValue
                                                                       : toStd(def));
    }
    SCENE_CATCH(makeString(""))
}

void rt_game_scene_camera_set_int(void *scene, rt_string key, int64_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setSectionScalar(s, s.camera, key, makeIntScalar(value));
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_camera_set_str(void *scene, rt_string key, rt_string value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setSectionScalar(s, s.camera, key, makeStringScalar(toStd(value)));
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_camera_remove(void *scene, rt_string key) {
    SCENE_TRY {
        requireScene(scene)->state->camera.fields.erase(toStd(key));
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_camera_keys(void *scene){
    SCENE_TRY{return sectionKeySeq(requireScene(scene) -> state->camera);
}
SCENE_CATCH(rt_seq_new_owned())
}

rt_string rt_game_scene_lighting_field_kind(void *scene, rt_string key){SCENE_TRY{
    const SceneScalar *scalar = findSectionScalar(requireScene(scene) -> state->lighting, key);
return makeString(scalar ? scalarKindName(scalar->kind) : "");
}
SCENE_CATCH(makeString(""))
}

int64_t rt_game_scene_lighting_get_int(void *scene, rt_string key, int64_t def) {
    SCENE_TRY {
        const SceneScalar *scalar = findSectionScalar(requireScene(scene)->state->lighting, key);
        return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
    }
    SCENE_CATCH(def)
}

void rt_game_scene_lighting_set_int(void *scene, rt_string key, int64_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setSectionScalar(s, s.lighting, key, makeIntScalar(value));
    }
    SCENE_CATCH_VOID
}

void rt_game_scene_lighting_remove(void *scene, rt_string key) {
    SCENE_TRY {
        requireScene(scene)->state->lighting.fields.erase(toStd(key));
    }
    SCENE_CATCH_VOID
}

void *rt_game_scene_lighting_keys(void *scene) {
    SCENE_TRY {
        return sectionKeySeq(requireScene(scene)->state->lighting);
    }
    SCENE_CATCH(rt_seq_new_owned())
}

} // extern "C"
