//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_scene_editor.cpp
/// @file
/// @brief Implements editable scene documents, canonical JSON persistence,
///        diagnostics, typed rich sections, and Tilemap reconstruction.
//
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

/// @brief Canonical scene schema version emitted by this implementation.
constexpr int64_t kSceneVersion = 1;
/// @brief Maximum accepted scene JSON payload size.
constexpr int64_t kMaxJsonBytes = 16 * 1024 * 1024;
/// @brief Maximum width-times-height cells in one layer.
constexpr int64_t kMaxCellsPerLayer = 1024 * 1024;
/// @brief Maximum cells across every layer.
constexpr int64_t kMaxTotalCells = 4 * 1024 * 1024;
/// @brief Maximum editable layer count.
constexpr int64_t kMaxLayers = 16;
/// @brief Maximum editable object count.
constexpr int64_t kMaxObjects = 65536;
/// @brief Maximum scene-level scalar properties.
constexpr int64_t kMaxSceneProperties = 16384;
/// @brief Maximum public and reserved properties associated with one object.
constexpr int64_t kMaxObjectProperties = 256;
/// @brief Maximum UTF-8 byte length of a property key.
constexpr size_t kMaxPropertyKeyBytes = 128;
/// @brief Maximum UTF-8 byte length of a scalar string value.
constexpr size_t kMaxStringValueBytes = 64 * 1024;
/// @brief Aggregate canonical-JSON budget for preserved unknown content.
constexpr size_t kMaxPreservedBytes = 4 * 1024 * 1024;
/// @brief Maximum retained diagnostic records.
constexpr size_t kMaxDiagnostics = 256;
/// @brief Tilemap-compatible maximum layer-name byte length.
constexpr size_t kMaxTilemapLayerNameBytes = 31;
/// @brief Reserved serialized property encoding organizational parent index.
constexpr const char *kObjectParentProperty = "zanna.hierarchy.parentIndex";

/// @brief Copy a runtime string's explicit bytes into a C++ string.
/// @param s Borrowed runtime string; may be `NULL`.
/// @return Byte-preserving copy, or an empty string for null/empty/invalid
///         storage.
std::string toStd(rt_string s) {
    if (!s)
        return {};
    const char *data = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!data || len <= 0)
        return {};
    return std::string(data, static_cast<size_t>(len));
}

/// @brief Allocate a runtime string from C++ string bytes.
/// @param value Bytes to copy.
/// @return New runtime string reference.
rt_string makeString(const std::string &value) {
    return rt_string_from_bytes(value.data(), value.size());
}

/// @brief Release one runtime object reference and free it at zero.
/// @param obj Object handle; `NULL` is a no-op.
void releaseObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Release a parsed JSON value according to its runtime representation.
/// @param obj Parsed value; `NULL` is a no-op.
/// @details Bare runtime strings use string unref; boxed/container values use
///          the generic object reference path.
void releaseJsonValue(void *obj) {
    if (!obj)
        return;
    if (rt_string_is_handle(obj)) {
        rt_string_unref(static_cast<rt_string>(obj));
        return;
    }
    releaseObject(obj);
}

/// @brief Set a string-valued map field using temporary runtime strings.
/// @param map Destination runtime Map.
/// @param key NUL-terminated field name.
/// @param value Bytes copied into a temporary runtime string.
/// @details Both temporary references are released after the Map retains them.
void mapSetStrField(void *map, const char *key, const std::string &value) {
    rt_string k = rt_const_cstr(key);
    rt_string v = makeString(value);
    rt_map_set_str(map, k, v);
    rt_string_unref(k);
    rt_string_unref(v);
}

/// @brief Set an integer-valued map field using a temporary key string.
/// @param map Destination runtime Map.
/// @param key NUL-terminated field name.
/// @param value Integer value.
void mapSetIntField(void *map, const char *key, int64_t value) {
    rt_string k = rt_const_cstr(key);
    rt_map_set_int(map, k, value);
    rt_string_unref(k);
}

/// @brief Test whether a runtime object is a Seq.
/// @param obj Candidate handle.
/// @return `true` only for a non-null RT_SEQ_CLASS_ID object.
bool isSeq(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_SEQ_CLASS_ID;
}

/// @brief Test whether a runtime object is a Map.
/// @param obj Candidate handle.
/// @return `true` only for a non-null RT_MAP_CLASS_ID object.
bool isMap(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_MAP_CLASS_ID;
}

/// @brief Lowercase ASCII bytes without locale-dependent signed-char behavior.
/// @param value String copied and transformed in place.
/// @return Lowercased copy.
std::string lowerAscii(std::string value) {
    for (char &ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

/// @brief Encode arbitrary bytes as one JSON string literal.
/// @param s Bytes to escape.
/// @return Quoted JSON with control characters and metacharacters escaped.
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

/// @brief Format a finite double for canonical scene JSON.
/// @param value Number to format.
/// @return 17-digit decimal representation, or `"null"` for NaN/infinity.
std::string jsonNumber(double value) {
    if (!std::isfinite(value))
        return "null";
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

/// @brief Scalar kinds supported by scene and object property APIs.
enum class ScalarKind {
    Null,
    Bool,
    Int,
    Float,
    String,
};

/// @brief Convert a scalar-kind enum to its public token.
/// @param kind Kind to name.
/// @return Static token `null`, `bool`, `int`, `float`, or `string`.
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

/// @brief Typed scalar value stored directly in editable scene state.
struct SceneScalar {
    ScalarKind kind{ScalarKind::Null};
    bool boolValue{false};
    int64_t intValue{0};
    double floatValue{0.0};
    std::string stringValue;
};

/// @brief Construct a null scalar.
/// @return Default Null value.
SceneScalar makeNullScalar() {
    return {};
}

/// @brief Construct a boolean scalar.
/// @param value Boolean payload.
/// @return Typed Bool value.
SceneScalar makeBoolScalar(bool value) {
    SceneScalar out;
    out.kind = ScalarKind::Bool;
    out.boolValue = value;
    return out;
}

/// @brief Construct an integer scalar.
/// @param value Integer payload.
/// @return Typed Int value.
SceneScalar makeIntScalar(int64_t value) {
    SceneScalar out;
    out.kind = ScalarKind::Int;
    out.intValue = value;
    return out;
}

/// @brief Construct a floating-point scalar.
/// @param value Floating payload.
/// @return Typed Float value.
SceneScalar makeFloatScalar(double value) {
    SceneScalar out;
    out.kind = ScalarKind::Float;
    out.floatValue = value;
    return out;
}

/// @brief Construct a string scalar.
/// @param value Bytes copied into the payload.
/// @return Typed String value.
SceneScalar makeStringScalar(const std::string &value) {
    SceneScalar out;
    out.kind = ScalarKind::String;
    out.stringValue = value;
    return out;
}

/// @brief Serialize one typed scalar as canonical JSON.
/// @param value Scalar to encode.
/// @return JSON token or literal.
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

/// @brief Convert one typed scalar to legacy string-property text.
/// @param value Scalar to convert.
/// @return Empty for Null, lowercase Boolean, decimal number, or stored string.
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

/// @brief Editable tile layer with optional asset binding.
struct Layer {
    std::string name;
    std::string asset;
    bool visible{true};
    std::vector<int64_t> tiles;
};

/// @brief Editable scene object with absolute position and typed properties.
/// @brief Default RGBA tint meaning "no tint applied".
constexpr int64_t kObjectTintNone = 0xFFFFFFFF;

struct Object {
    std::string type;
    std::string id;
    int64_t x{0};
    int64_t y{0};
    int64_t parent{-1};
    // Optional transform fields (ADR 0192); serialized only when they
    // differ from these defaults so legacy documents stay byte-stable.
    double rotation{0.0};
    double scaleX{1.0};
    double scaleY{1.0};
    bool flipX{false};
    bool flipY{false};
    int64_t tint{kObjectTintNone};
    double pivotX{0.5};
    double pivotY{0.5};
    std::map<std::string, SceneScalar> properties;
};

/// @brief Normalize an authored rotation into [0, 360) degrees.
/// @param value Candidate rotation in degrees.
/// @return Canonical finite rotation; non-finite input becomes 0.
double sanitizeObjectRotation(double value) {
    if (!std::isfinite(value))
        return 0.0;
    value = std::fmod(value, 360.0);
    if (value < 0.0)
        value += 360.0;
    return value;
}

/// @brief Clamp an authored scale factor into the supported envelope.
/// @param value Candidate scale factor.
/// @return Finite non-zero factor in [-10000, 10000]; degenerate input becomes 1.
double sanitizeObjectScale(double value) {
    if (!std::isfinite(value) || value == 0.0)
        return 1.0;
    if (value > 10000.0)
        return 10000.0;
    if (value < -10000.0)
        return -10000.0;
    return value;
}

/// @brief Clamp an authored normalized pivot component into [0, 1].
/// @param value Candidate pivot component.
/// @return Clamped finite pivot; non-finite input becomes centered 0.5.
double sanitizeObjectPivot(double value) {
    if (!std::isfinite(value))
        return 0.5;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Mask an authored tint to its canonical 32-bit RGBA range.
/// @param value Candidate tint.
/// @return Value masked to 32 bits.
int64_t sanitizeObjectTint(int64_t value) {
    return value & kObjectTintNone;
}

/// @brief Structured load/edit diagnostic retained by a scene.
struct Diagnostic {
    std::string code;
    std::string severity;
    std::string message;
    std::string path;
    int64_t line{0};
    int64_t column{0};
    std::string source;
};

/// @brief Unknown rich top-level section retained as canonical JSON.
struct PreservedSection {
    std::string key;
    std::string canonicalJson;
};

/// @brief Minimum tile ID addressable by editable behavior tables.
constexpr int64_t kMinBehaviorTileId = 1;
/// @brief Maximum tile ID addressable by editable behavior tables.
constexpr int64_t kMaxBehaviorTileId = 4095;
/// @brief Maximum typed tile-property entries across a scene.
constexpr int64_t kMaxTilePropertyEntries = 4096;
/// @brief Maximum typed properties authored on one tile.
constexpr int64_t kMaxTilePropertiesPerTile = 64;
/// @brief Maximum frames in one animation rule.
constexpr int64_t kMaxAnimFramesPerRule = 4096;
/// @brief Maximum animation frames across all rules.
constexpr int64_t kMaxTotalAnimFrames = 65536;
/// @brief Required number of variants in one autotile rule.
constexpr size_t kAutotileVariantCount = 16;

/// @brief One typed tile-animation rule plus preserved unknown members.
struct TileAnimation {
    std::vector<int64_t> frames;
    std::vector<int64_t> durations;
    std::map<std::string, std::string> unknown;
};

/// @brief One 16-variant autotile rule plus preserved unknown members.
struct AutotileRule {
    std::array<int64_t, kAutotileVariantCount> variants{};
    std::map<std::string, std::string> unknown;
};

/// @brief Typed Int/Bool properties and unknown members for one tile.
struct TilePropertySet {
    std::map<std::string, SceneScalar> values; // Int/Bool kinds only
    std::map<std::string, std::string> unknown;

    /// @brief Test whether neither typed nor unknown properties remain.
    /// @return `true` when the set serializes no content.
    bool empty() const {
        return values.empty() && unknown.empty();
    }
};

/// @brief Typed collision/property/animation/autotile document state.
/// @details Loaders accept arbitrary int64 tile IDs for lossless round-trips;
///          editing APIs enforce Tilemap's 1–4095 addressability.
struct TileBehaviorState {
    std::map<int64_t, int64_t> collision; // tile -> kind (1 solid, 2 one-way-up)
    int64_t collisionLayer{0};
    bool hasCollisionLayer{false};
    std::map<std::string, std::string> collisionUnknown;
    std::map<int64_t, TilePropertySet> tileProperties;
    std::map<std::string, std::string> tilePropertiesUnknown; // non-integer root keys
    std::map<int64_t, TileAnimation> animations;
    std::map<int64_t, AutotileRule> autotiles;

    /// @brief Test whether the collision section has any serializable content.
    /// @return `true` when collision mappings, layer, and unknowns are absent.
    bool collisionEmpty() const {
        return collision.empty() && !hasCollisionLayer && collisionUnknown.empty();
    }

    /// @brief Test whether the tile-properties section has serializable content.
    /// @return `true` when typed and unknown property maps are absent.
    bool tilePropertiesEmpty() const {
        return tileProperties.empty() && tilePropertiesUnknown.empty();
    }
};

/// @brief Scene-global scalar section (camera, lighting): int/string fields only.
struct SectionScalarState {
    std::map<std::string, SceneScalar> fields;
    std::map<std::string, std::string> unknown;

    /// @brief Test whether the section has typed or unknown fields.
    /// @return `true` when nothing would be serialized.
    bool empty() const {
        return fields.empty() && unknown.empty();
    }
};

/// @brief Complete C++ state owned by one runtime SceneDocument handle.
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

/// @brief Runtime object payload holding the separately allocated C++ state.
struct SceneHandle {
    SceneState *state{nullptr};
};

/// @brief Validate a SceneDocument handle and its C++ state pointer.
/// @param scene Candidate runtime handle.
/// @return Valid SceneHandle, or `nullptr` after raising a runtime trap.
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

/// @brief Delete the C++ state owned by a runtime SceneDocument.
/// @param obj SceneHandle supplied by the runtime finalizer system.
void sceneFinalizer(void *obj) {
    auto *h = static_cast<SceneHandle *>(obj);
    delete h->state;
    h->state = nullptr;
}

/// @brief Test whether a state is invalid or retains an error diagnostic.
/// @param s Scene state to inspect.
/// @return `true` for invalid/error-bearing state.
bool hasErrors(const SceneState &s) {
    if (!s.valid)
        return true;
    for (const auto &diag : s.diagnostics) {
        if (diag.severity == "error")
            return true;
    }
    return false;
}

/// @brief Append a bounded structured diagnostic and update error state.
/// @param s Scene receiving the diagnostic.
/// @param code Stable diagnostic code.
/// @param severity Severity token; `"error"` marks the scene invalid.
/// @param message Human-readable message and new last-error text.
/// @param path Optional document path.
/// @param line Optional one-based source line.
/// @param column Optional one-based source column.
/// @details At the 256-record cap, the final retained record becomes one
///          truncation warning and later records are dropped. Source path is
///          copied from the SceneState.
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

/// @brief Validate layer dimensions and compute their cell count safely.
/// @param width Requested cell width.
/// @param height Requested cell height.
/// @param cells Receives `width * height`, or zero on failure.
/// @return `true` for positive, nonoverflowing dimensions within the
///         one-million-cell layer budget.
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

/// @brief Test a layer index against current scene storage.
/// @param s Scene to inspect.
/// @param layer Candidate index.
/// @return `true` when in range.
bool validLayer(const SceneState &s, int64_t layer) {
    return layer >= 0 && layer < static_cast<int64_t>(s.layers.size());
}

/// @brief Test tile coordinates against scene dimensions.
/// @param s Scene to inspect.
/// @param x Candidate X.
/// @param y Candidate Y.
/// @return `true` when inside the tile grid.
bool validTile(const SceneState &s, int64_t x, int64_t y) {
    return x >= 0 && y >= 0 && x < s.width && y < s.height;
}

/// @brief Test an object index against current scene storage.
/// @param s Scene to inspect.
/// @param index Candidate index.
/// @return `true` when in range.
bool validObjectIndex(const SceneState &s, int64_t index) {
    return index >= 0 && index < static_cast<int64_t>(s.objects.size());
}

/// @brief Test a C++ property key for internal object-hierarchy reservation.
/// @param key Property key.
/// @return `true` only for the parent-index encoding key.
bool isReservedObjectProperty(const std::string &key) {
    return key == kObjectParentProperty;
}

/// @brief Test a runtime property key for internal hierarchy reservation.
/// @param key Borrowed runtime string.
/// @return Result of the C++ string overload.
bool isReservedObjectProperty(rt_string key) {
    return isReservedObjectProperty(toStd(key));
}

/// @brief Enforce the property-key byte budget.
/// @param key Candidate key bytes.
/// @return `true` when at most 128 bytes.
bool validKey(const std::string &key) {
    return key.size() <= kMaxPropertyKeyBytes;
}

/// @brief Enforce the scalar-string byte budget.
/// @param value Candidate value bytes.
/// @return `true` when at most 64 KiB.
bool validStringValue(const std::string &value) {
    return value.size() <= kMaxStringValueBytes;
}

/// @brief Validate key/string scalar limits and emit schema diagnostics.
/// @param s Scene receiving errors.
/// @param key Candidate property key.
/// @param scalar Candidate typed value.
/// @param path Parent diagnostic path.
/// @return `true` when both applicable limits pass.
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

/// @brief Construct a zero-filled tile layer matching scene dimensions.
/// @param s Scene supplying dimensions and default-name index.
/// @param name Requested name; empty generates `LayerN`.
/// @return New visible layer with no asset and all tile IDs zero.
Layer makeLayer(SceneState &s, const std::string &name) {
    size_t cells = 1;
    checkedCellCount(s.width, s.height, cells);
    Layer layer;
    layer.name = name.empty() ? "Layer" + std::to_string(s.layers.size()) : name;
    layer.tiles.assign(cells, 0);
    return layer;
}

/// @brief Move C++ scene state into a finalized runtime handle.
/// @param state State value to heap-allocate.
/// @return New SceneDocument handle, or `nullptr` if runtime allocation fails.
/// @details C++ state allocation occurs before runtime allocation so exception
///          barriers cannot strand an unfinalized handle.
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

/// @brief Create baseline scene state with one `base` layer.
/// @param width Requested width.
/// @param height Requested height.
/// @param tileWidth Requested positive tile width; invalid values become 16.
/// @param tileHeight Requested positive tile height; invalid values become 16.
/// @return Initialized state. Invalid scene dimensions produce a diagnostic
///         and fall back to an invalid 1×1 state.
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

/// @brief Construct a runtime SceneDocument from requested dimensions.
/// @param width Scene width in cells.
/// @param height Scene height in cells.
/// @param tileWidth Tile pixel width.
/// @param tileHeight Tile pixel height.
/// @return Result of makeBaseState() wrapped by handleFromState().
void *newSceneHandle(int64_t width, int64_t height, int64_t tileWidth, int64_t tileHeight) {
    return handleFromState(makeBaseState(width, height, tileWidth, tileHeight));
}

/// @brief Test whether a runtime Map contains a C-string key.
/// @param map Candidate Map.
/// @param key NUL-terminated field name.
/// @return `false` for non-Map input; otherwise Map membership.
bool mapHas(void *map, const char *key) {
    if (!isMap(map))
        return false;
    rt_string k = rt_const_cstr(key);
    bool result = rt_map_has(map, k) != 0;
    rt_string_unref(k);
    return result;
}

/// @brief Borrow one value from a runtime Map by C-string key.
/// @param map Candidate Map.
/// @param key NUL-terminated field name.
/// @return Borrowed mapped value, or `nullptr` for non-Map/missing input.
void *mapGet(void *map, const char *key) {
    if (!isMap(map))
        return nullptr;
    rt_string k = rt_const_cstr(key);
    void *value = rt_map_get(map, k);
    rt_string_unref(k);
    return value;
}

/// @brief Convert a runtime or boxed string value to C++ bytes.
/// @param value Candidate JSON scalar.
/// @return Copied string, or empty for unsupported input.
/// @details Temporary unboxed string references are released.
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

/// @brief Read a string-valued JSON map field.
/// @param map Runtime Map.
/// @param key Field name.
/// @param out Receives copied bytes on success.
/// @return `true` only when the field exists and is a string representation.
bool jsonString(void *map, const char *key, std::string &out) {
    if (!mapHas(map, key))
        return false;
    void *value = mapGet(map, key);
    if (!rt_string_is_handle(value) && (!value || rt_box_type(value) != RT_BOX_STR))
        return false;
    out = valueToString(value);
    return true;
}

/// @brief Convert a boxed JSON number to exact int64.
/// @param value Candidate boxed I64 or F64.
/// @param out Receives the integer on success.
/// @return `true` for I64 or finite integral in-range F64.
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

/// @brief Read an exact integer JSON map field.
/// @param map Runtime Map.
/// @param key Field name.
/// @param out Receives value on success.
/// @return `true` when present and accepted by jsonNumberToInt().
bool jsonInt(void *map, const char *key, int64_t &out) {
    if (!mapHas(map, key))
        return false;
    return jsonNumberToInt(mapGet(map, key), out);
}

/// @brief Read a finite numeric JSON map field as double.
/// @param map Runtime Map.
/// @param key Field name.
/// @param out Receives converted I64/F64 value.
/// @return `true` when present, numeric, and finite.
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

/// @brief Read a Boolean JSON map field with optional numeric compatibility.
/// @param map Runtime Map.
/// @param key Field name.
/// @param out Receives Boolean value.
/// @param allowNumeric Whether exact integer zero/one are accepted.
/// @return `true` when a supported representation is present.
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

/// @brief Convert one parsed JSON scalar to SceneScalar.
/// @param value Runtime JSON value; null pointer represents JSON null.
/// @param out Receives typed scalar.
/// @return `true` for null, string, Bool, integer, or finite float; false for
///         containers and unsupported boxes.
/// @details Integral F64 values in int64 range normalize to Int.
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

/// @brief Parse a runtime Map of bounded scalar properties.
/// @param s Scene receiving schema diagnostics.
/// @param map Candidate runtime Map.
/// @param out Destination ordered property map.
/// @param maxEntries Maximum accepted source entries.
/// @param path Parent diagnostic path.
/// @return `false` for non-Map input or source count overflow; otherwise
///         `true` after processing every key.
/// @details Invalid values/limits emit diagnostics and are skipped while valid
///          entries continue loading.
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

/// @brief Canonically format an arbitrary supported runtime JSON value.
/// @param value Parsed value; null pointer encodes JSON null.
/// @return Compact JSON. Map keys are sorted; unsupported boxes become null.
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

/// @brief Public wrapper for recursive runtime-value JSON formatting.
/// @param value Runtime JSON value.
/// @return Canonical compact JSON string.
std::string formatRuntimeJson(void *value) {
    return formatRuntimeJsonValue(value);
}

/// @brief Test whether a top-level key has typed rich-section support.
/// @param key Candidate key.
/// @return `true` for camera, lighting, collision, tileProperties, animations,
///         or autotiles.
bool isKnownRichSection(const std::string &key) {
    return key == "camera" || key == "lighting" || key == "collision" || key == "tileProperties" ||
           key == "animations" || key == "autotiles";
}

/// @brief Test whether a top-level key belongs to the core scene schema.
/// @param key Candidate key.
/// @return `true` when handled outside preserved-section logic.
bool isReservedTopLevelKey(const std::string &key) {
    return key == "version" || key == "name" || key == "width" || key == "height" ||
           key == "tileWidth" || key == "tileHeight" || key == "tilesetAsset" ||
           key == "properties" || key == "layers" || key == "objects" || key == "tilemap";
}

/// @brief Compute aggregate bytes retained for unknown scene content.
/// @param s Scene to inspect.
/// @return Typed-unknown bytes plus canonical preserved-section JSON bytes.
size_t preservedSectionBytes(const SceneState &s) {
    size_t total = s.typedUnknownBytes;
    for (const auto &[_, section] : s.preservedSections)
        total += section.canonicalJson.size();
    return total;
}

/// @brief Reserve budget for one retained unknown section member (ADR 0176).
/// @param s Scene whose aggregate budget is updated.
/// @param path Diagnostic path.
/// @param json Canonical JSON to be retained.
/// @return `true` when the 4 MiB aggregate budget permits the value.
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

/// @brief Preserve an unknown rich top-level section as canonical JSON.
/// @param s Scene to modify.
/// @param key Top-level section key.
/// @param value Parsed runtime value.
/// @details Values exceeding the aggregate 4 MiB budget emit an error and are
///          dropped; repeated keys replace prior preserved content.
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

/// @brief Parse a whole decimal string as int64 using exception-safe stoll.
/// @param key Candidate decimal bytes.
/// @param out Receives parsed value on success.
/// @return `true` only when every byte belongs to one in-range integer.
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

/// @brief Retain an unrecognized typed-section member as canonical JSON.
/// @param s Scene supplying the aggregate unknown-content budget.
/// @param unknown Destination ordered unknown-member map.
/// @param path Parent diagnostic path.
/// @param key Member key.
/// @param value Parsed runtime value.
/// @details The member is stored only when reserveTypedUnknown() succeeds.
void retainUnknownMember(SceneState &s,
                         std::map<std::string, std::string> &unknown,
                         const std::string &path,
                         const std::string &key,
                         void *value) {
    std::string json = formatRuntimeJson(value);
    if (reserveTypedUnknown(s, path + "/" + key, json))
        unknown[key] = std::move(json);
}

/// @brief Parse typed collision mappings and preserve unknown members.
/// @param s Scene receiving behavior state and warnings.
/// @param root Collision-section runtime Map.
/// @details Integer tile IDs from `solid`/`oneWayUp` arrays are retained
///          losslessly; `layer` must be integer. Later mappings overwrite
///          earlier values for the same tile.
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

/// @brief Parse per-tile Int/Bool property maps.
/// @param s Scene receiving typed properties, unknown JSON, and diagnostics.
/// @param root Tile-ID-keyed runtime Map.
/// @details Noninteger tile keys and non-Map values are preserved as unknown.
///          Typed entry and per-tile budgets are enforced while unsupported
///          property values remain preserved when budget permits.
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

/// @brief Parse animation-rule array into typed behavior state.
/// @param s Scene receiving animation rules and diagnostics.
/// @param root Runtime Seq of rule Maps.
/// @details Rules require integer baseTile, equal frame/duration arrays,
///          positive durations, and configured per-rule/aggregate frame
///          budgets. Unknown rule members are preserved canonically.
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

/// @brief Parse 16-variant autotile rules into typed behavior state.
/// @param s Scene receiving rules and diagnostics.
/// @param root Runtime Seq of rule Maps.
/// @details Invalid shapes are warned and skipped. Unknown rule members are
///          preserved; a later duplicate base tile replaces the earlier rule.
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

/// @brief Parse a camera/lighting field map into Int/String typed state.
/// @param s Scene receiving diagnostics and unknown-byte accounting.
/// @param section Destination typed section.
/// @param name Section name used in paths/messages.
/// @param root Runtime Map.
/// @details Unsupported types are preserved as canonical unknown members.
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
/// @param s Scene receiving parsed state.
/// @param key Known top-level key.
/// @param value Parsed runtime value.
/// @return `true` when key and container shape were consumed; `false` when the
///         caller should preserve the value verbatim.
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

/// @brief Parse or preserve all non-core top-level scene sections.
/// @param s Scene receiving typed/preserved state and diagnostics.
/// @param root Root runtime Map.
/// @details Correctly shaped known sections enter typed state. Wrong-shaped
///          Map/Seq rich sections remain preserved; unknown Maps are preserved;
///          unknown scalars/arrays are warned and dropped.
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

/// @brief Load one layer's tile array with canonical/legacy normalization.
/// @param s Scene supplying dimensions and receiving diagnostics.
/// @param layer Destination zero-initialized layer.
/// @param layerMap Source layer Map.
/// @param layerIndex Index used in diagnostic paths.
/// @param legacy Whether legacy omissions/count mismatches are tolerated.
/// @param nestedDraft Whether missing tiles are tolerated for nested drafts.
/// @return `false` for required-field, dimension, or canonical count failure;
///         otherwise `true`.
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

/// @brief Parse bounded layer definitions into scene state.
/// @param s Scene receiving layers and diagnostics.
/// @param sourceMap Root or nested tilemap Map containing `layers`.
/// @param legacy Whether legacy compatibility rules apply.
/// @param nestedDraft Whether nested-draft omissions are tolerated.
/// @details Enforces 16 layers, one million cells per layer, four million
///          aggregate cells, and 31-byte Tilemap layer names. At least one
///          fallback base layer is retained.
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

/// @brief Validate organizational parent indices and break invalid cycles.
/// @param s Scene whose loaded object hierarchy is normalized.
/// @details Out-of-range/self parents become roots with errors. Cycle members
///          are all rooted so subsequent hierarchy operations remain bounded.
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

/// @brief Parse bounded scene objects and scalar properties.
/// @param s Scene receiving objects and diagnostics.
/// @param root Root runtime Map.
/// @details Supports canonical `properties` plus legacy scalar object members.
///          The reserved parent key is extracted into organizational state and
///          removed from the public property map before hierarchy validation.
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

        // Optional transform fields (ADR 0192): claimed before the
        // property catch-all exactly like x/y; absent keys keep defaults.
        double transformValue = 0.0;
        if (jsonDouble(objMap, "rotation", transformValue))
            obj.rotation = sanitizeObjectRotation(transformValue);
        if (jsonDouble(objMap, "scaleX", transformValue))
            obj.scaleX = sanitizeObjectScale(transformValue);
        if (jsonDouble(objMap, "scaleY", transformValue))
            obj.scaleY = sanitizeObjectScale(transformValue);
        bool transformFlag = false;
        if (jsonBool(objMap, "flipX", transformFlag, true))
            obj.flipX = transformFlag;
        if (jsonBool(objMap, "flipY", transformFlag, true))
            obj.flipY = transformFlag;
        int64_t tintValue = 0;
        if (jsonInt(objMap, "tint", tintValue))
            obj.tint = sanitizeObjectTint(tintValue);
        if (jsonDouble(objMap, "pivotX", transformValue))
            obj.pivotX = sanitizeObjectPivot(transformValue);
        if (jsonDouble(objMap, "pivotY", transformValue))
            obj.pivotY = sanitizeObjectPivot(transformValue);

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
            if (key == "type" || key == "id" || key == "x" || key == "y" ||
                key == "properties" || key == "rotation" || key == "scaleX" ||
                key == "scaleY" || key == "flipX" || key == "flipY" || key == "tint" ||
                key == "pivotX" || key == "pivotY")
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

/// @brief Parse SceneDocument JSON into complete diagnostic-bearing state.
/// @param text Borrowed runtime JSON string.
/// @param sourcePath Optional source filename copied into diagnostics.
/// @return Loaded state, always containing at least one layer.
/// @details Empty, oversized, malformed, wrong-root, unsupported-version, and
///          schema-invalid inputs return an invalid diagnostic document rather
///          than publishing partial runtime state. Legacy and nested tilemap
///          layouts are normalized into version one.
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

/// @brief Emit two-space indentation.
/// @param out Destination stream.
/// @param level Number of indentation levels.
void writeIndent(std::ostringstream &out, int level) {
    for (int i = 0; i < level; ++i)
        out << "  ";
}

/// @brief Emit an ordered scalar map as pretty canonical JSON.
/// @param out Destination stream.
/// @param map Lexicographically ordered properties.
/// @param level Current indentation level.
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

/// @brief Emit a tile vector as a compact JSON integer array.
/// @param out Destination stream.
/// @param tiles Tile IDs in row-major order.
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
/// @param out Destination stream.
/// @param members Key and already-rendered JSON value pairs.
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

/// @brief Render an integer vector as compact JSON.
/// @param values Ordered integer values.
/// @return JSON array text.
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

/// @brief Emit collision state merged with preserved unknown fields.
/// @param out Destination stream.
/// @param b Tile behavior state.
/// @details Collision tile maps are emitted as ascending `solid` and
///          `oneWayUp` arrays.
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

/// @brief Emit per-tile typed and unknown property content.
/// @param out Destination stream.
/// @param b Tile behavior state.
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

/// @brief Emit animation rules in ascending base-tile order.
/// @param out Destination stream.
/// @param b Tile behavior state.
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

/// @brief Emit autotile rules in ascending base-tile order.
/// @param out Destination stream.
/// @param b Tile behavior state.
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

/// @brief Emit typed camera/lighting fields merged with preserved unknowns.
/// @param out Destination stream.
/// @param section Section state.
void writeScalarSection(std::ostringstream &out, const SectionScalarState &section) {
    std::vector<std::pair<std::string, std::string>> members;
    for (const auto &[key, value] : section.fields)
        members.emplace_back(key, scalarToJson(value));
    for (const auto &[key, json] : section.unknown)
        members.emplace_back(key, json);
    writeMergedObject(out, std::move(members));
}

/// @brief Emit one typed rich section when non-empty; return true when emitted.
/// @param out Destination scene stream.
/// @param s Scene providing section state.
/// @param key Known rich-section key.
/// @return `true` when nonempty typed content was emitted.
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

/// @brief Emit a complete version-one SceneDocument in canonical order.
/// @param out Destination stream.
/// @param s Scene state to serialize.
/// @details Core fields/layers/objects use stable formatting, object parents
///          re-enter the reserved property slot, typed rich sections take
///          precedence over preserved legacy versions, and all remaining
///          preserved sections follow in lexicographic order.
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
        // Transform fields serialize only away from their defaults so
        // untransformed documents keep their exact legacy bytes (ADR 0192).
        if (obj.rotation != 0.0)
            out << "      \"rotation\": " << jsonNumber(obj.rotation) << ",\n";
        if (obj.scaleX != 1.0)
            out << "      \"scaleX\": " << jsonNumber(obj.scaleX) << ",\n";
        if (obj.scaleY != 1.0)
            out << "      \"scaleY\": " << jsonNumber(obj.scaleY) << ",\n";
        if (obj.flipX)
            out << "      \"flipX\": true,\n";
        if (obj.flipY)
            out << "      \"flipY\": true,\n";
        if (obj.tint != kObjectTintNone)
            out << "      \"tint\": " << obj.tint << ",\n";
        if (obj.pivotX != 0.5)
            out << "      \"pivotX\": " << jsonNumber(obj.pivotX) << ",\n";
        if (obj.pivotY != 0.5)
            out << "      \"pivotY\": " << jsonNumber(obj.pivotY) << ",\n";
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

/// @brief Infer an asset descriptor kind from a property/member key.
/// @param key Context key inspected case-insensitively.
/// @param fallback Kind used for generic `asset` keys.
/// @return `tileset`, `sprite`, `audio`, `image`, fallback/`unknown`, or empty
///         when the key is not asset-like.
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

/// @brief One discovered asset reference with authoring provenance.
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

/// @brief Append a nonempty asset reference descriptor.
/// @param out Destination descriptor vector.
/// @param s Scene supplying source path.
/// @param path Referenced asset path; empty values are ignored.
/// @param kind Inferred asset kind.
/// @param owner Owner token (`scene`, `layer`, `object`, or `section`).
/// @param layer Owning layer index or `-1`.
/// @param object Owning object index or `-1`.
/// @param key Field/property key.
/// @param section Optional rich-section name.
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

/// @brief Recursively discover asset-like strings in preserved runtime JSON.
/// @param out Destination descriptors.
/// @param s Scene supplying source provenance.
/// @param value Current parsed runtime value.
/// @param section Top-level preserved section.
/// @param keyHint Nearest Map key used for kind inference.
/// @details Sequences inherit their parent's key hint; Maps replace it per
///          child. Strings whose context is not asset-like are ignored.
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

/// @brief Discover asset references across typed and preserved scene content.
/// @param s Scene to scan.
/// @return Descriptor vector in deterministic scene/layer/object/section scan
///         order.
/// @details Preserved canonical JSON is reparsed temporarily. Invalid preserved
///          content is skipped.
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

/// @brief Convert one C++ asset descriptor to a runtime Map.
/// @param d Descriptor to copy.
/// @return New Map containing path, kind, owner, indices, key, section, source.
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

/// @brief Find a typed scalar by runtime string key.
/// @param map Ordered scalar map.
/// @param key Borrowed runtime key.
/// @return Pointer into @p map, or `nullptr`.
const SceneScalar *findScalar(const std::map<std::string, SceneScalar> &map, rt_string key) {
    auto it = map.find(toStd(key));
    return it == map.end() ? nullptr : &it->second;
}

/// @brief Insert or replace a bounded scalar property.
/// @param map Destination property map.
/// @param s Scene receiving edit-rejection warnings.
/// @param key Borrowed runtime key.
/// @param value Typed value to move on success.
/// @param maxEntries Maximum distinct keys.
/// @details Oversized keys/strings and new entries beyond capacity are warning
///          no-ops; replacing an existing entry remains allowed at capacity.
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

/// @brief Compute remaining public property capacity for one object.
/// @param object Object to inspect.
/// @return 256 when root, or 255 when parent serialization consumes the
///         reserved slot.
int64_t maxPublicObjectProperties(const Object &object) {
    return kMaxObjectProperties - (object.parent >= 0 ? 1 : 0);
}

/// @brief Read the current OS process identifier.
/// @return Platform process ID as uint64.
uint64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

/// @brief Choose a process/counter-qualified temporary filename.
/// @param dir Destination directory.
/// @param target Final target filename used in the hidden stem.
/// @return Candidate path not observed to exist during up to 1024 probes;
///         after that, a fresh counter path without an existence guarantee.
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

/// @brief Atomically replace a target path with a completed temporary file.
/// @param temp Existing temporary path in the target directory.
/// @param target Final scene path.
/// @param ec Receives platform/filesystem failure.
/// @return `true` after rename/replacement succeeds.
/// @details Uses filesystem rename first and Windows write-through replacement
///          as a platform fallback.
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

/// @brief Apply a preserved collision JSON object to a Tilemap.
/// @param tilemap Destination Tilemap.
/// @param root Parsed collision Map.
/// @details Recognized integer layer and integer tile IDs are forwarded;
///          malformed/unknown values are skipped.
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

/// @brief Apply preserved per-tile numeric/Boolean properties to a Tilemap.
/// @param tilemap Destination Tilemap.
/// @param root Parsed tileProperties Map.
/// @details Noninteger tile keys, non-Map values, and unsupported property
///          types are skipped.
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

/// @brief Apply preserved animation rules to a Tilemap.
/// @param tilemap Destination Tilemap.
/// @param root Parsed animation Seq.
/// @details Supports typed per-frame durations and legacy
///          frameCount/msPerFrame encoding. Invalid rules are skipped.
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

/// @brief Apply preserved Tiled import-layout metadata to a Tilemap.
/// @param tilemap Destination Tilemap.
/// @param root Parsed tiledRuntime Map.
/// @details Orientation/render/stagger/skew/parallax fields use safe defaults;
///          per-layer metadata is applied only to existing Tilemap layers.
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

/// @brief Apply preserved 16-variant autotile rules to a Tilemap.
/// @param tilemap Destination Tilemap.
/// @param root Parsed autotile Seq.
/// @details Rules with missing/noninteger base or fewer than 16 integer
///          variants are skipped.
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
/// @param s Scene supplying typed behavior.
/// @param tilemap Destination Tilemap.
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

/// @brief Reparse and apply legacy preserved Tilemap-relevant sections.
/// @param s Scene supplying canonical preserved JSON.
/// @param tilemap Destination Tilemap.
/// @details Only collision, tileProperties, animations, autotiles, and
///          tiledRuntime are considered. Parse failures are silent no-ops.
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

/// @brief Remap an object index after moving one vector element.
/// @param index Existing index or negative root sentinel.
/// @param from Original moved-object index.
/// @param to Destination index.
/// @return Index referring to the same logical object after the move.
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

/// @brief Remap an object index after inserting before one vector position.
/// @param index Existing index.
/// @param insertion New element index.
/// @return Index incremented when at or after insertion.
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
/// @brief Begin the C ABI exception barrier.
#define SCENE_TRY try
/// @brief Convert any C++ exception to a value-returning ABI fallback.
#define SCENE_CATCH(default_ret)                                                                   \
    catch (...) {                                                                                  \
        return default_ret;                                                                        \
    }
/// @brief Convert any C++ exception to a void ABI return.
#define SCENE_CATCH_VOID                                                                           \
    catch (...) {                                                                                  \
    }

/// @brief Create a diagnostic-bearing scene with one base layer.
/// @param width Width in tile cells.
/// @param height Height in tile cells.
/// @param tile_width Tile width in pixels; nonpositive input becomes 16.
/// @param tile_height Tile height in pixels; nonpositive input becomes 16.
/// @return New SceneDocument, or `NULL` on an escaped C++ allocation failure.
/// @details Invalid scene dimensions produce a 1×1 document retaining an error
///          diagnostic rather than returning a partial state.
void *rt_game_scene_new(int64_t width, int64_t height, int64_t tile_width, int64_t tile_height) {
    SCENE_TRY {
        return newSceneHandle(width, height, tile_width, tile_height);
    }
    SCENE_CATCH(nullptr)
}

/// @brief Load JSON into a diagnostic-bearing compatibility SceneDocument.
/// @param text Borrowed scene JSON.
/// @return New SceneDocument even for parse/schema errors, or `NULL` on an
///         escaped C++ allocation failure.
/// @details Inspect diagnostics/has-errors to distinguish an invalid document;
///          use rt_game_scene_load_json_result() for Result semantics.
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

/// @brief Load a scene document from a filesystem path.
/// @details Reads at most 16 MiB of JSON and records schema or I/O failures on
///          the returned document. A missing, oversized, or malformed file
///          therefore produces an editable fallback scene with diagnostics
///          rather than a null handle.
/// @param path_s Runtime string containing the scene file path.
/// @return Owned SceneDocument handle, or `nullptr` if an exception crosses
///         the runtime allocation/translation boundary.
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

/// @brief Serialize a scene document to canonical JSON.
/// @details The serializer emits normalized dimensions, layers, objects,
///          scalar properties, typed behavior sections, and preserved rich
///          sections in deterministic key order.
/// @param scene SceneDocument handle.
/// @return Newly allocated runtime string containing the JSON text, or an
///         empty runtime string if serialization fails.
rt_string rt_game_scene_to_json(void *scene){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
std::ostringstream out;
writeCanonicalJson(out, s);
return makeString(out.str());
}
SCENE_CATCH(makeString(""))
}

/// @brief Save a scene document using a same-directory atomic replacement.
/// @details Serializes the document, writes an exclusively created temporary
///          file, flushes it, and replaces the destination only after the
///          complete payload is durable. Failed writes leave a diagnostic and
///          make a best-effort attempt to remove the temporary file.
/// @param scene SceneDocument handle whose diagnostics receive save failures.
/// @param path_s Destination filesystem path.
/// @return `1` after a successful replacement; otherwise `0`.
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

/// @brief Return the most recently recorded diagnostic message.
/// @param scene SceneDocument handle.
/// @return Newly allocated runtime string, empty when no diagnostic has been
///         recorded or when the operation fails.
rt_string rt_game_scene_last_error(void *scene) {
    SCENE_TRY {
        return makeString(requireScene(scene)->state->lastError);
    }
    SCENE_CATCH(makeString(""))
}

/// @brief Copy the document's diagnostic messages into a runtime sequence.
/// @details Messages retain insertion order but omit diagnostic metadata; use
///          @ref rt_game_scene_diagnostic_records for structured records.
/// @param scene SceneDocument handle.
/// @return Owned sequence of owned runtime strings, or an empty owned sequence
///         if the operation fails.
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

/// @brief Copy all structured diagnostic records from a scene document.
/// @details Each sequence item is a map with `code`, `severity`, `message`,
///          `path`, `line`, `column`, and `source` fields. Records retain their
///          original insertion order.
/// @param scene SceneDocument handle.
/// @return Owned sequence of owned diagnostic maps, or an empty owned sequence
///         if the operation fails.
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

/// @brief Test whether a document remains invalid or has error diagnostics.
/// @param scene SceneDocument handle.
/// @return `1` when structural validity is false or an error is present;
///         otherwise `0`.
int8_t rt_game_scene_has_errors(void *scene) {
    return hasErrors(*requireScene(scene)->state) ? 1 : 0;
}

/// @brief Acknowledge and remove all queued diagnostics.
/// @details Clearing messages does not repair or reset the structural-validity
///          state established while loading the document.
/// @param scene SceneDocument handle.
void rt_game_scene_clear_diagnostics(void *scene) {
    SceneState &s = *requireScene(scene)->state;
    // Clear only the diagnostic queue and last-error text. The `valid` flag records
    // the document's structural validity (set during load); acknowledging messages
    // must not flip an invalid/normalized scene to valid, which would make
    // HasErrors() falsely report success without repairing the data (VDOC-254).
    s.diagnostics.clear();
    s.lastError.clear();
}

/// @brief Return the scene width in tile cells.
/// @param scene SceneDocument handle.
/// @return Positive scene width.
int64_t rt_game_scene_get_width(void *scene) {
    return requireScene(scene)->state->width;
}

/// @brief Return the scene height in tile cells.
/// @param scene SceneDocument handle.
/// @return Positive scene height.
int64_t rt_game_scene_get_height(void *scene) {
    return requireScene(scene)->state->height;
}

/// @brief Return the rendered width of one tile in pixels.
/// @param scene SceneDocument handle.
/// @return Positive tile width.
int64_t rt_game_scene_get_tile_width(void *scene) {
    return requireScene(scene)->state->tileWidth;
}

/// @brief Return the rendered height of one tile in pixels.
/// @param scene SceneDocument handle.
/// @return Positive tile height.
int64_t rt_game_scene_get_tile_height(void *scene) {
    return requireScene(scene)->state->tileHeight;
}

/// @brief Append an empty tile layer to the scene.
/// @details The edit is rejected with a warning if it would exceed the layer
///          count, aggregate tile-cell budget, or Tilemap layer-name limit.
/// @param scene SceneDocument handle.
/// @param name Name assigned to the new layer.
/// @return Zero-based index of the new layer, or `-1` when rejected.
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

/// @brief Return the number of tile layers in the document.
/// @param scene SceneDocument handle.
/// @return Current layer count.
int64_t rt_game_scene_layer_count(void *scene) {
    return static_cast<int64_t>(requireScene(scene)->state->layers.size());
}

/// @brief Copy the name of a tile layer.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index.
/// @return Newly allocated layer-name string, or an empty string for an
///         invalid index or failed operation.
rt_string rt_game_scene_layer_name(void *scene, int64_t layer) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        return makeString(validLayer(s, layer) ? s.layers[static_cast<size_t>(layer)].name : "");
    }
    SCENE_CATCH(makeString(""))
}

/// @brief Rename a tile layer when the requested name is Tilemap-compatible.
/// @details Invalid layer indices are ignored. Names longer than the Tilemap
///          31-byte limit are rejected and recorded as warning diagnostics.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param name Replacement layer name.
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

/// @brief Query whether a tile layer is visible.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index.
/// @return `1` for a visible valid layer; `0` for a hidden or invalid layer.
int8_t rt_game_scene_layer_visible(void *scene, int64_t layer) {
    SceneState &s = *requireScene(scene)->state;
    return validLayer(s, layer) && s.layers[static_cast<size_t>(layer)].visible ? 1 : 0;
}

/// @brief Change a tile layer's visibility flag.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index; invalid indices are ignored.
/// @param visible Nonzero to show the layer, zero to hide it.
void rt_game_scene_set_layer_visible(void *scene, int64_t layer, int8_t visible) {
    SceneState &s = *requireScene(scene)->state;
    if (validLayer(s, layer))
        s.layers[static_cast<size_t>(layer)].visible = visible != 0;
}

/// @brief Move a layer to another position in the layer stack.
/// @details The selected layer is removed and reinserted at @p to, preserving
///          its tiles and metadata. Invalid indices and identity moves are
///          ignored.
/// @param scene SceneDocument handle.
/// @param from Current zero-based layer index.
/// @param to Destination zero-based layer index.
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

/// @brief Remove a layer while preserving the scene's required base layer.
/// @param scene SceneDocument handle.
/// @param layer Zero-based index to remove; invalid indices and removal of the
///        sole remaining layer are ignored.
void rt_game_scene_remove_layer(void *scene, int64_t layer) {
    SceneState &s = *requireScene(scene)->state;
    if (s.layers.size() <= 1 || !validLayer(s, layer))
        return;
    s.layers.erase(s.layers.begin() + layer);
}

/// @brief Read the tile identifier stored at one layer cell.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param x Horizontal tile coordinate.
/// @param y Vertical tile coordinate.
/// @return Stored tile identifier, or `0` when any index is out of range.
int64_t rt_game_scene_get_tile(void *scene, int64_t layer, int64_t x, int64_t y) {
    SceneState &s = *requireScene(scene)->state;
    if (!validLayer(s, layer) || !validTile(s, x, y))
        return 0;
    return s.layers[static_cast<size_t>(layer)].tiles[static_cast<size_t>(y * s.width + x)];
}

/// @brief Replace the tile identifier at one layer cell.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param x Horizontal tile coordinate.
/// @param y Vertical tile coordinate.
/// @param tile Tile identifier to store.
void rt_game_scene_set_tile(void *scene, int64_t layer, int64_t x, int64_t y, int64_t tile) {
    SceneState &s = *requireScene(scene)->state;
    if (!validLayer(s, layer) || !validTile(s, x, y))
        return;
    s.layers[static_cast<size_t>(layer)].tiles[static_cast<size_t>(y * s.width + x)] = tile;
}

/// @brief Fill the in-bounds portion of a rectangular tile region.
/// @details The rectangle is clipped to the scene bounds. Non-positive extents,
///          invalid layers, and overflowing endpoint calculations are ignored.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param x Left tile coordinate, which may lie outside the scene.
/// @param y Top tile coordinate, which may lie outside the scene.
/// @param w Rectangle width in cells.
/// @param h Rectangle height in cells.
/// @param tile Tile identifier written to every clipped cell.
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

/// @brief Flood-fill a four-connected region of equal tile identifiers.
/// @details The complete bounded queue and visited set are allocated before the
///          first tile mutation, keeping allocation failures atomic.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param x Horizontal seed coordinate.
/// @param y Vertical seed coordinate.
/// @param tile Replacement tile identifier.
/// @return Number of changed cells, or `0` for invalid input, an unchanged
///         replacement, or a failed operation.
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

/// @brief Associate an asset path with a tile layer.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index; invalid indices are ignored.
/// @param asset_path Asset path copied into the layer metadata.
void rt_game_scene_set_layer_asset(void *scene, int64_t layer, rt_string asset_path){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (validLayer(s, layer))
    s.layers[static_cast<size_t>(layer)].asset = toStd(asset_path);
}
SCENE_CATCH_VOID
}

/// @brief Copy the asset path associated with a tile layer.
/// @param scene SceneDocument handle.
/// @param layer Zero-based layer index.
/// @return Newly allocated asset-path string, or an empty string for an
///         invalid index or failed operation.
rt_string rt_game_scene_layer_asset(void *scene, int64_t layer){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
return makeString(validLayer(s, layer) ? s.layers[static_cast<size_t>(layer)].asset : "");
}
SCENE_CATCH(makeString(""))
}

/// @brief Append an object with the supplied metadata and absolute position.
/// @param scene SceneDocument handle.
/// @param type Object type copied into the new record.
/// @param id Object identifier copied into the new record.
/// @param x Absolute horizontal scene coordinate.
/// @param y Absolute vertical scene coordinate.
/// @return Zero-based index of the new root object, or `-1` if the object
///         limit is reached or the operation fails.
int64_t rt_game_scene_add_object(void *scene, rt_string type, rt_string id, int64_t x, int64_t y){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (static_cast<int64_t>(s.objects.size()) >= kMaxObjects) {
    addDiagnostic(s, "scene.edit.rejected", "warning", "too many scene objects");
    return -1;
}
Object added;
added.type = toStd(type);
added.id = toStd(id);
added.x = x;
added.y = y;
s.objects.push_back(std::move(added));
return static_cast<int64_t>(s.objects.size()) - 1;
}
SCENE_CATCH(-1)
}

/// @brief Return the number of objects in document order.
/// @param scene SceneDocument handle.
/// @return Current object count.
int64_t rt_game_scene_object_count(void *scene) {
    return static_cast<int64_t>(requireScene(scene)->state->objects.size());
}

/// @brief Remove an object and repair every stored parent index.
/// @details Direct children are reparented to the removed object's parent, and
///          references after the erased index are shifted down.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
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

/// @brief Copy an object's type string.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Newly allocated type string, or an empty string for an invalid
///         index or failed operation.
rt_string rt_game_scene_object_type(void *scene, int64_t index){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
return makeString(validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].type : "");
}
SCENE_CATCH(makeString(""))
}

/// @brief Copy an object's identifier string.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Newly allocated identifier string, or an empty string for an
///         invalid index or failed operation.
rt_string rt_game_scene_object_id(void *scene, int64_t index){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
return makeString(validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].id : "");
}
SCENE_CATCH(makeString(""))
}

/// @brief Return an object's absolute horizontal scene coordinate.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored x coordinate, or `0` for an invalid index.
int64_t rt_game_scene_object_x(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].x : 0;
}

/// @brief Return an object's absolute vertical scene coordinate.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored y coordinate, or `0` for an invalid index.
int64_t rt_game_scene_object_y(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].y : 0;
}

/// @brief Return an object's organizational parent index.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Parent object index, or `-1` for a root or invalid object.
int64_t rt_game_scene_object_parent(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].parent : -1;
}

/// @brief Assign an object's organizational parent without creating a cycle.
/// @details The requested parent may be `-1` for a root. The edit validates
///          both indices, reserves the hierarchy serialization slot, and walks
///          the ancestor chain before changing the object.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index to reparent.
/// @param parent New parent index, or `-1` to detach the object.
/// @return `1` when the assignment succeeds; `0` when invalid, cyclic, or over
///         the object's property budget.
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

/// @brief Replace an object's type and identifier atomically.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
/// @param type Replacement type string.
/// @param id Replacement identifier string.
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

/// @brief Set an object's absolute scene position.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
/// @param x Replacement horizontal coordinate.
/// @param y Replacement vertical coordinate.
void rt_game_scene_set_object_position(void *scene, int64_t index, int64_t x, int64_t y) {
    SceneState &s = *requireScene(scene)->state;
    if (validObjectIndex(s, index)) {
        s.objects[static_cast<size_t>(index)].x = x;
        s.objects[static_cast<size_t>(index)].y = y;
    }
}

/// @brief Return an object's rotation in canonical [0, 360) degrees.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored rotation, or `0` for an invalid index.
double rt_game_scene_object_rotation(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].rotation : 0.0;
}

/// @brief Set an object's rotation about its pivot.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
/// @param degrees Rotation normalized into [0, 360); non-finite input becomes 0.
void rt_game_scene_set_object_rotation(void *scene, int64_t index, double degrees) {
    SceneState &s = *requireScene(scene)->state;
    if (validObjectIndex(s, index))
        s.objects[static_cast<size_t>(index)].rotation = sanitizeObjectRotation(degrees);
}

/// @brief Return an object's horizontal scale factor.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored factor, or `1` for an invalid index.
double rt_game_scene_object_scale_x(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].scaleX : 1.0;
}

/// @brief Return an object's vertical scale factor.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored factor, or `1` for an invalid index.
double rt_game_scene_object_scale_y(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].scaleY : 1.0;
}

/// @brief Set an object's scale factors about its pivot.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
/// @param scale_x Horizontal factor clamped to [-10000, 10000]; 0 becomes 1.
/// @param scale_y Vertical factor with the same envelope.
void rt_game_scene_set_object_scale(void *scene, int64_t index, double scale_x, double scale_y) {
    SceneState &s = *requireScene(scene)->state;
    if (validObjectIndex(s, index)) {
        s.objects[static_cast<size_t>(index)].scaleX = sanitizeObjectScale(scale_x);
        s.objects[static_cast<size_t>(index)].scaleY = sanitizeObjectScale(scale_y);
    }
}

/// @brief Return whether an object mirrors horizontally about its pivot.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored flag, or `0` for an invalid index.
int8_t rt_game_scene_object_flip_x(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) && s.objects[static_cast<size_t>(index)].flipX ? 1 : 0;
}

/// @brief Return whether an object mirrors vertically about its pivot.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored flag, or `0` for an invalid index.
int8_t rt_game_scene_object_flip_y(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) && s.objects[static_cast<size_t>(index)].flipY ? 1 : 0;
}

/// @brief Set an object's mirroring flags.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
/// @param flip_x Nonzero mirrors horizontally about the pivot.
/// @param flip_y Nonzero mirrors vertically about the pivot.
void rt_game_scene_set_object_flip(void *scene, int64_t index, int8_t flip_x, int8_t flip_y) {
    SceneState &s = *requireScene(scene)->state;
    if (validObjectIndex(s, index)) {
        s.objects[static_cast<size_t>(index)].flipX = flip_x != 0;
        s.objects[static_cast<size_t>(index)].flipY = flip_y != 0;
    }
}

/// @brief Return an object's RGBA multiply tint.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored 32-bit tint, or opaque white for an invalid index.
int64_t rt_game_scene_object_tint(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].tint
                                      : kObjectTintNone;
}

/// @brief Set an object's RGBA multiply tint.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
/// @param rgba Tint masked to 32 bits; opaque white means "no tint".
void rt_game_scene_set_object_tint(void *scene, int64_t index, int64_t rgba) {
    SceneState &s = *requireScene(scene)->state;
    if (validObjectIndex(s, index))
        s.objects[static_cast<size_t>(index)].tint = sanitizeObjectTint(rgba);
}

/// @brief Return an object's normalized horizontal pivot component.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored pivot in [0, 1], or centered `0.5` for an invalid index.
double rt_game_scene_object_pivot_x(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].pivotX : 0.5;
}

/// @brief Return an object's normalized vertical pivot component.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored pivot in [0, 1], or centered `0.5` for an invalid index.
double rt_game_scene_object_pivot_y(void *scene, int64_t index) {
    SceneState &s = *requireScene(scene)->state;
    return validObjectIndex(s, index) ? s.objects[static_cast<size_t>(index)].pivotY : 0.5;
}

/// @brief Set an object's normalized pivot within its sprite footprint.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
/// @param pivot_x Horizontal component clamped to [0, 1].
/// @param pivot_y Vertical component clamped to [0, 1].
void rt_game_scene_set_object_pivot(void *scene, int64_t index, double pivot_x, double pivot_y) {
    SceneState &s = *requireScene(scene)->state;
    if (validObjectIndex(s, index)) {
        s.objects[static_cast<size_t>(index)].pivotX = sanitizeObjectPivot(pivot_x);
        s.objects[static_cast<size_t>(index)].pivotY = sanitizeObjectPivot(pivot_y);
    }
}

/// @brief Duplicate an object immediately after its source.
/// @details Copies type, position, parent, and scalar properties, replaces the
///          identifier, then remaps all parent indices to account for the
///          insertion.
/// @param scene SceneDocument handle.
/// @param index Zero-based source object index.
/// @param id Identifier assigned to the duplicate.
/// @return Index of the inserted duplicate, or `-1` for invalid input, an
///         exhausted object budget, or a failed operation.
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

/// @brief Store a legacy string-valued object property.
/// @details This compatibility entry point delegates to the typed string
///          setter and therefore observes key, value, capacity, and reserved
///          hierarchy-key validation.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key.
/// @param value String value to copy.
void rt_game_scene_set_object_property(void *scene, int64_t index, rt_string key, rt_string value) {
    rt_game_scene_object_set_str(scene, index, key, value);
}

/// @brief Read any object scalar through the legacy string interface.
/// @details Existing null, integer, string, floating-point, and Boolean values
///          are converted to their canonical textual representation.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key.
/// @return Newly allocated textual value, or an empty string when absent,
///         invalid, or unsuccessful.
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

/// @brief Remove an object property through the legacy string interface.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key; the reserved hierarchy key cannot be removed.
void rt_game_scene_delete_object_property(void *scene, int64_t index, rt_string key) {
    rt_game_scene_object_remove(scene, index, key);
}

/// @brief Read an integer-valued object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key.
/// @param def Fallback returned for a missing, mismatched, or invalid property.
/// @return Stored integer or @p def.
int64_t rt_game_scene_object_get_int(void *scene, int64_t index, rt_string key, int64_t def){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (!validObjectIndex(s, index))
    return def;
const SceneScalar *scalar = findScalar(s.objects[static_cast<size_t>(index)].properties, key);
return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
}
SCENE_CATCH(def)
}

/// @brief Read a string-valued object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key.
/// @param def Fallback copied for a missing, mismatched, or invalid property.
/// @return Newly allocated stored or fallback string.
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

/// @brief Read a numeric object property as a floating-point value.
/// @details Stored integers are widened exactly when possible; other scalar
///          kinds use the caller-provided fallback.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key.
/// @param def Fallback for absence, type mismatch, invalid input, or failure.
/// @return Stored float, widened integer, or @p def.
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

/// @brief Read a Boolean-valued object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key.
/// @param def Fallback for absence, type mismatch, invalid input, or failure.
/// @return `1` or `0` for a stored Boolean, otherwise @p def unchanged.
int8_t rt_game_scene_object_get_bool(void *scene, int64_t index, rt_string key, int8_t def){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (!validObjectIndex(s, index))
    return def;
const SceneScalar *scalar = findScalar(s.objects[static_cast<size_t>(index)].properties, key);
return scalar && scalar->kind == ScalarKind::Bool ? (scalar->boolValue ? 1 : 0) : def;
}
SCENE_CATCH(def)
}

/// @brief Test whether an object contains a scalar property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key.
/// @return `1` when present on a valid object; otherwise `0`.
int8_t rt_game_scene_object_has(void *scene, int64_t index, rt_string key){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
return validObjectIndex(s, index) &&
               findScalar(s.objects[static_cast<size_t>(index)].properties, key)
           ? 1
           : 0;
}
SCENE_CATCH(0)
}

/// @brief Report the stored scalar kind of an object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key.
/// @return Newly allocated kind name (`null`, `int`, `string`, `float`, or
///         `bool`), or an empty string when absent or invalid.
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

/// @brief Enumerate an object's public property keys.
/// @details Keys follow the lexical order of the document's ordered property
///          map. The reserved hierarchy encoding is not stored in this map.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @return Owned sequence of owned key strings, empty for an invalid object or
///         failed operation.
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

/// @brief Store an explicit null object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key; invalid, reserved, or over-budget edits are
///        ignored and may record a warning.
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

/// @brief Store an integer object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key subject to object-property limits.
/// @param value Integer value to store.
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

/// @brief Store a string object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key subject to object-property limits.
/// @param value String value subject to the 64 KiB scalar-value limit.
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

/// @brief Store a finite floating-point object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key subject to object-property limits.
/// @param value Finite value to store; NaN and infinities are ignored.
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

/// @brief Store a Boolean object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Property key subject to object-property limits.
/// @param value Zero for false, nonzero for true.
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

/// @brief Remove a public object property.
/// @param scene SceneDocument handle.
/// @param index Zero-based object index; invalid indices are ignored.
/// @param key Property key; the reserved hierarchy key is left untouched.
void rt_game_scene_object_remove(void *scene, int64_t index, rt_string key){
    SCENE_TRY{SceneState &s = *requireScene(scene) -> state;
if (validObjectIndex(s, index) && !isReservedObjectProperty(key))
    s.objects[static_cast<size_t>(index)].properties.erase(toStd(key));
}
SCENE_CATCH_VOID
}

/// @brief Count objects whose type exactly matches a target string.
/// @param scene SceneDocument handle.
/// @param type Case-sensitive object type to match.
/// @return Number of matching objects, or `0` if the operation fails.
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

/// @brief Find the nth object of a given type in document order.
/// @param scene SceneDocument handle.
/// @param type Case-sensitive object type to match.
/// @param n Zero-based match ordinal.
/// @return Object index, or `-1` when the ordinal is negative, out of range,
///         or the operation fails.
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

/// @brief Find the first object with an exact identifier match.
/// @param scene SceneDocument handle.
/// @param id Case-sensitive identifier to search for.
/// @return Zero-based object index, or `-1` when absent or unsuccessful.
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

/// @brief Move an object to another document-order index.
/// @details Every parent reference is remapped before the selected record is
///          reinserted, preserving the hierarchy represented by object
///          indices.
/// @param scene SceneDocument handle.
/// @param from Current zero-based object index.
/// @param to Destination zero-based object index.
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

/// @brief Store a legacy string-valued scene property.
/// @details Delegates to the typed string setter and therefore observes all
///          property-key, string-size, and property-count limits.
/// @param scene SceneDocument handle.
/// @param key Property key.
/// @param value String value to copy.
void rt_game_scene_set_property(void *scene, rt_string key, rt_string value) {
    rt_game_scene_set_str(scene, key, value);
}

/// @brief Read any scene scalar through the legacy string interface.
/// @details Existing scalar kinds are converted to canonical text.
/// @param scene SceneDocument handle.
/// @param key Property key.
/// @return Newly allocated textual value, or an empty string when absent or
///         unsuccessful.
rt_string rt_game_scene_get_property(void *scene, rt_string key) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        const SceneScalar *scalar = findScalar(s.properties, key);
        return makeString(scalar ? scalarToString(*scalar) : "");
    }
    SCENE_CATCH(makeString(""))
}

/// @brief Remove a scene property through the legacy string interface.
/// @param scene SceneDocument handle.
/// @param key Property key to erase.
void rt_game_scene_delete_property(void *scene, rt_string key) {
    rt_game_scene_remove(scene, key);
}

/// @brief Read an integer-valued scene property.
/// @param scene SceneDocument handle.
/// @param key Property key.
/// @param def Fallback for absence, type mismatch, or failure.
/// @return Stored integer or @p def.
int64_t rt_game_scene_get_int(void *scene, rt_string key, int64_t def){
    SCENE_TRY{const SceneScalar *scalar = findScalar(requireScene(scene) -> state->properties, key);
return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
}
SCENE_CATCH(def)
}

/// @brief Read a string-valued scene property.
/// @param scene SceneDocument handle.
/// @param key Property key.
/// @param def Fallback copied when the property is absent or has another type.
/// @return Newly allocated stored or fallback string.
rt_string rt_game_scene_get_str(void *scene, rt_string key, rt_string def) {
    SCENE_TRY {
        const SceneScalar *scalar = findScalar(requireScene(scene)->state->properties, key);
        return makeString(scalar && scalar->kind == ScalarKind::String ? scalar->stringValue
                                                                       : toStd(def));
    }
    SCENE_CATCH(makeString(""))
}

/// @brief Read a numeric scene property as a floating-point value.
/// @details Floating-point values are returned directly and integer values are
///          widened; all other kinds use the supplied fallback.
/// @param scene SceneDocument handle.
/// @param key Property key.
/// @param def Fallback for absence, type mismatch, or failure.
/// @return Stored float, widened integer, or @p def.
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

/// @brief Read a Boolean-valued scene property.
/// @param scene SceneDocument handle.
/// @param key Property key.
/// @param def Fallback for absence, type mismatch, or failure.
/// @return `1` or `0` for a stored Boolean, otherwise @p def unchanged.
int8_t rt_game_scene_get_bool(void *scene, rt_string key, int8_t def){
    SCENE_TRY{const SceneScalar *scalar = findScalar(requireScene(scene) -> state->properties, key);
return scalar && scalar->kind == ScalarKind::Bool ? (scalar->boolValue ? 1 : 0) : def;
}
SCENE_CATCH(def)
}

/// @brief Test whether a scene scalar property exists.
/// @param scene SceneDocument handle.
/// @param key Property key.
/// @return `1` when present; otherwise `0`.
int8_t rt_game_scene_has(void *scene, rt_string key){
    SCENE_TRY{return findScalar(requireScene(scene) -> state->properties, key) ? 1 : 0;
}
SCENE_CATCH(0)
}

/// @brief Report the stored scalar kind of a scene property.
/// @param scene SceneDocument handle.
/// @param key Property key.
/// @return Newly allocated kind name (`null`, `int`, `string`, `float`, or
///         `bool`), or an empty string when absent or unsuccessful.
rt_string rt_game_scene_property_kind(void *scene, rt_string key) {
    SCENE_TRY {
        const SceneScalar *scalar = findScalar(requireScene(scene)->state->properties, key);
        return makeString(scalar ? scalarKindName(scalar->kind) : "");
    }
    SCENE_CATCH(makeString(""))
}

/// @brief Enumerate all scene property keys in lexical order.
/// @param scene SceneDocument handle.
/// @return Owned sequence of owned key strings, or an empty owned sequence if
///         the operation fails.
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

/// @brief Store an explicit null scene property.
/// @param scene SceneDocument handle.
/// @param key Property key subject to key and property-count limits.
void rt_game_scene_set_null(void *scene, rt_string key) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setScalar(s.properties, s, key, makeNullScalar(), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

/// @brief Store an integer scene property.
/// @param scene SceneDocument handle.
/// @param key Property key subject to key and property-count limits.
/// @param value Integer value to store.
void rt_game_scene_set_int(void *scene, rt_string key, int64_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setScalar(s.properties, s, key, makeIntScalar(value), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

/// @brief Store a string scene property.
/// @param scene SceneDocument handle.
/// @param key Property key subject to key and property-count limits.
/// @param value String value subject to the 64 KiB scalar-value limit.
void rt_game_scene_set_str(void *scene, rt_string key, rt_string value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setScalar(s.properties, s, key, makeStringScalar(toStd(value)), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

/// @brief Store a finite floating-point scene property.
/// @param scene SceneDocument handle.
/// @param key Property key subject to key and property-count limits.
/// @param value Finite value to store; NaN and infinities are ignored.
void rt_game_scene_set_float(void *scene, rt_string key, double value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        if (std::isfinite(value))
            setScalar(s.properties, s, key, makeFloatScalar(value), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

/// @brief Store a Boolean scene property.
/// @param scene SceneDocument handle.
/// @param key Property key subject to key and property-count limits.
/// @param value Zero for false, nonzero for true.
void rt_game_scene_set_bool(void *scene, rt_string key, int8_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setScalar(s.properties, s, key, makeBoolScalar(value != 0), kMaxSceneProperties);
    }
    SCENE_CATCH_VOID
}

/// @brief Remove a scene scalar property if present.
/// @param scene SceneDocument handle.
/// @param key Property key to erase.
void rt_game_scene_remove(void *scene, rt_string key) {
    SCENE_TRY {
        requireScene(scene)->state->properties.erase(toStd(key));
    }
    SCENE_CATCH_VOID
}

/// @brief Discover structured asset references reachable from the document.
/// @details The scan covers layer assets plus recognized asset-shaped values
///          in preserved JSON. Each returned map describes a discovered path
///          and its source location; descriptors retain deterministic order.
/// @param scene SceneDocument handle.
/// @return Owned sequence of owned descriptor maps, or an empty owned sequence
///         if discovery fails.
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

/// @brief Enumerate unique asset paths referenced by the document.
/// @details Paths are deduplicated and returned in lexical order.
/// @param scene SceneDocument handle.
/// @return Owned sequence of owned path strings, or an empty owned sequence if
///         discovery fails.
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

/// @brief Materialize the editable scene as a runtime Tilemap.
/// @details Copies layer ordering, names, visibility, and tile cells, then
///          applies typed collision/animation/autotile/property state and any
///          compatible preserved Tilemap sections.
/// @param scene SceneDocument handle.
/// @return Owned Tilemap handle, or `nullptr` if construction fails.
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
/// @param s Scene that receives a rejection diagnostic.
/// @param tile Candidate nonzero Tilemap tile identifier.
/// @return `true` when @p tile is within the editable 1..4095 range.
bool editableBehaviorTileId(SceneState &s, int64_t tile) {
    if (tile >= kMinBehaviorTileId && tile <= kMaxBehaviorTileId)
        return true;
    addDiagnostic(s, "scene.edit.rejected", "warning", "tile id must be between 1 and 4095");
    return false;
}

/// @brief Replace a same-key malformed preserved section when typed authoring begins.
/// @details Typed APIs own the canonical representation of a section. If load
///          preservation retained an invalid same-named JSON value, the first
///          typed edit discards it and records the normalization.
/// @param s Scene containing preserved and typed sections.
/// @param key Top-level section name being claimed.
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

/// @brief Append a boxed integer to an owned runtime sequence.
/// @param seq Borrowed runtime sequence.
/// @param value Integer to box and append.
void pushIntValue(void *seq, int64_t value) {
    void *boxed = rt_box_i64(value);
    rt_seq_push(seq, boxed);
    releaseObject(boxed);
}

/// @brief Convert the ordered keys of an integer map to a runtime sequence.
/// @param map Map whose ascending keys are copied.
/// @return Owned sequence of boxed integers.
void *ascendingKeySeq(const std::map<int64_t, int64_t> &map) {
    void *seq = rt_seq_new_owned();
    for (const auto &[key, _] : map)
        pushIntValue(seq, key);
    return seq;
}

/// @brief Convert an integer vector to an order-preserving runtime sequence.
/// @param values Values to box in vector order.
/// @return Owned sequence of boxed integers.
void *intVectorSeq(const std::vector<int64_t> &values) {
    void *seq = rt_seq_new_owned();
    for (int64_t value : values)
        pushIntValue(seq, value);
    return seq;
}

/// @brief Validate and store one typed property for a behavior tile.
/// @details Enforces the editable tile range, key limit, per-tile capacity,
///          and global tile-property budget. Empty speculative entries are
///          removed when capacity validation rejects the edit.
/// @param scene SceneDocument handle.
/// @param tile Tile identifier that owns the property.
/// @param key Property key.
/// @param value Scalar value to move into the behavior table.
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
/// @param seq Borrowed candidate sequence handle.
/// @param out Destination replaced with decoded integers on success.
/// @return `true` when every item is an integral runtime number.
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

/// @brief Record a typed-section validation warning and reject the edit.
/// @param s Scene receiving the diagnostic.
/// @param message Human-readable constraint failure.
/// @return Always `false`, allowing direct use in validation expressions.
bool rejectSectionEdit(SceneState &s, const char *message) {
    addDiagnostic(s, "scene.edit.rejected", "warning", message);
    return false;
}

/// @brief Test whether a scalar is an integer in a closed interval.
/// @param value Scalar to inspect.
/// @param min Inclusive lower bound.
/// @param max Inclusive upper bound.
/// @return `true` only for an integer scalar within both bounds.
bool intFieldInRange(const SceneScalar &value, int64_t min, int64_t max) {
    return value.kind == ScalarKind::Int && value.intValue >= min && value.intValue <= max;
}

/// @brief Validate the known camera-section fields of ADR 0177; unknown keys pass.
/// @param s Scene receiving any rejection diagnostic.
/// @param key Camera field name.
/// @param value Candidate typed value.
/// @return `true` when the known-field contract is satisfied.
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
/// @param s Scene receiving any rejection diagnostic.
/// @param key Lighting field name.
/// @param value Candidate typed value.
/// @return `true` when the known-field contract is satisfied.
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

/// @brief Validate and store a scalar in the camera or lighting section.
/// @details Applies shared key/string limits, section-specific known-field
///          validation, and preserved-section claiming before replacement.
/// @param s Scene containing the target section.
/// @param section Camera or lighting field map to update.
/// @param key Field name.
/// @param value Scalar value to move into the section.
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

/// @brief Find a typed field in a camera or lighting section.
/// @param section Section to inspect.
/// @param key Runtime field name.
/// @return Pointer to the stored scalar, or `nullptr` when absent.
const SceneScalar *findSectionScalar(const SectionScalarState &section, rt_string key) {
    auto it = section.fields.find(toStd(key));
    return it == section.fields.end() ? nullptr : &it->second;
}

/// @brief Enumerate typed camera or lighting field names.
/// @param section Section whose ordered field map is inspected.
/// @return Owned sequence of owned strings in lexical key order.
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

/// @brief Read the collision behavior assigned to a tile identifier.
/// @param scene SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @return One of the `RT_TILE_COLLISION_*` values, defaulting to
///         `RT_TILE_COLLISION_NONE` when no rule exists or lookup fails.
int64_t rt_game_scene_tile_collision(void *scene, int64_t tile) {
    SCENE_TRY {
        const auto &collision = requireScene(scene)->state->behavior.collision;
        auto it = collision.find(tile);
        return it == collision.end() ? RT_TILE_COLLISION_NONE : it->second;
    }
    SCENE_CATCH(RT_TILE_COLLISION_NONE)
}

/// @brief Assign or clear a tile collision behavior.
/// @details Tile IDs must be in the editable 1..4095 range. A `NONE` kind
///          erases the sparse entry; solid and one-way-up kinds are stored.
///          Other kind values are rejected with a warning diagnostic.
/// @param scene SceneDocument handle.
/// @param tile Tile identifier to edit.
/// @param kind One of the `RT_TILE_COLLISION_*` constants.
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

/// @brief Enumerate tile IDs with non-default collision behavior.
/// @param scene SceneDocument handle.
/// @return Owned sequence of boxed tile IDs in ascending order, or an empty
///         owned sequence if the operation fails.
void *rt_game_scene_collision_tiles(void *scene){
    SCENE_TRY{return ascendingKeySeq(requireScene(scene) -> state->behavior.collision);
}
SCENE_CATCH(rt_seq_new_owned())
}

/// @brief Return the configured Tilemap collision-layer index.
/// @param scene SceneDocument handle.
/// @return Stored layer index, or `0` if access fails.
int64_t rt_game_scene_collision_layer(void *scene) {
    SCENE_TRY {
        return requireScene(scene)->state->behavior.collisionLayer;
    }
    SCENE_CATCH(0)
}

/// @brief Configure the Tilemap layer used for collision queries.
/// @details The serialized Tilemap ABI accepts layer slots 0..15; this setter
///          validates that fixed range independently of the current number of
///          scene layers.
/// @param scene SceneDocument handle.
/// @param layer Collision layer slot in the inclusive range 0..15.
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

/// @brief Report the stored kind of a typed tile property.
/// @param scene SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @param key Property key.
/// @return Newly allocated kind name, or an empty string when the tile/key is
///         absent or lookup fails.
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

/// @brief Read an integer tile property.
/// @param scene SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @param key Property key.
/// @param def Fallback for absence, type mismatch, or failure.
/// @return Stored integer or @p def.
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

/// @brief Read a Boolean tile property.
/// @param scene SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @param key Property key.
/// @param def Fallback for absence, type mismatch, or failure.
/// @return `1` or `0` for a stored Boolean, otherwise @p def unchanged.
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

/// @brief Store an integer property for a behavior tile.
/// @param scene SceneDocument handle.
/// @param tile Editable tile identifier in the range 1..4095.
/// @param key Property key subject to tile-property limits.
/// @param value Integer value to store.
void rt_game_scene_set_tile_property_int(void *scene, int64_t tile, rt_string key, int64_t value) {
    SCENE_TRY {
        setTilePropertyScalar(scene, tile, key, makeIntScalar(value));
    }
    SCENE_CATCH_VOID
}

/// @brief Store a Boolean property for a behavior tile.
/// @param scene SceneDocument handle.
/// @param tile Editable tile identifier in the range 1..4095.
/// @param key Property key subject to tile-property limits.
/// @param value Zero for false, nonzero for true.
void rt_game_scene_set_tile_property_bool(void *scene, int64_t tile, rt_string key, int8_t value) {
    SCENE_TRY {
        setTilePropertyScalar(scene, tile, key, makeBoolScalar(value != 0));
    }
    SCENE_CATCH_VOID
}

/// @brief Remove one typed property from a behavior tile.
/// @details The sparse per-tile container is also erased when its final
///          property is removed.
/// @param scene SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @param key Property key to erase.
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

/// @brief Enumerate the property keys defined for one tile.
/// @param scene SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @return Owned sequence of owned strings in lexical key order, empty when
///         the tile has no properties or the operation fails.
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

/// @brief Enumerate tile IDs that currently have typed properties.
/// @param scene SceneDocument handle.
/// @return Owned sequence of boxed tile IDs in ascending order, or an empty
///         owned sequence if the operation fails.
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

/// @brief Define or replace a tile animation rule.
/// @details Both inputs must be integer sequences of equal length containing
///          1..4096 entries. Every duration must be positive and the complete
///          document may contain at most the configured aggregate frame
///          budget. Validation completes before the existing rule is replaced.
/// @param scene SceneDocument handle.
/// @param base_tile Editable base tile identifier in the range 1..4095.
/// @param frames Borrowed runtime sequence of frame tile identifiers.
/// @param durations_ms Borrowed runtime sequence of positive frame durations
///        in milliseconds.
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

/// @brief Copy the frame tile IDs of an animation rule.
/// @param scene SceneDocument handle.
/// @param base_tile Base tile identifier to inspect.
/// @return Owned sequence of boxed integers in playback order, empty when the
///         rule is absent or the operation fails.
void *rt_game_scene_tile_anim_frames(void *scene, int64_t base_tile) {
    SCENE_TRY {
        const auto &animations = requireScene(scene)->state->behavior.animations;
        auto it = animations.find(base_tile);
        return it == animations.end() ? rt_seq_new_owned() : intVectorSeq(it->second.frames);
    }
    SCENE_CATCH(rt_seq_new_owned())
}

/// @brief Copy the frame durations of an animation rule.
/// @param scene SceneDocument handle.
/// @param base_tile Base tile identifier to inspect.
/// @return Owned sequence of boxed millisecond durations in playback order,
///         empty when the rule is absent or the operation fails.
void *rt_game_scene_tile_anim_durations(void *scene, int64_t base_tile) {
    SCENE_TRY {
        const auto &animations = requireScene(scene)->state->behavior.animations;
        auto it = animations.find(base_tile);
        return it == animations.end() ? rt_seq_new_owned() : intVectorSeq(it->second.durations);
    }
    SCENE_CATCH(rt_seq_new_owned())
}

/// @brief Remove a tile animation rule if present.
/// @param scene SceneDocument handle.
/// @param base_tile Base tile identifier whose rule is erased.
void rt_game_scene_remove_tile_anim(void *scene, int64_t base_tile) {
    SCENE_TRY {
        requireScene(scene)->state->behavior.animations.erase(base_tile);
    }
    SCENE_CATCH_VOID
}

/// @brief Enumerate base tiles that define animation rules.
/// @param scene SceneDocument handle.
/// @return Owned sequence of boxed base tile IDs in ascending order, or an
///         empty owned sequence if the operation fails.
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

/// @brief Define or replace a 4-neighbor autotile lookup rule.
/// @details The variants input must be a runtime sequence containing exactly
///          16 integral tile identifiers, one for every neighbor mask.
/// @param scene SceneDocument handle.
/// @param base_tile Editable base tile identifier in the range 1..4095.
/// @param variants Borrowed 16-element runtime integer sequence.
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

/// @brief Copy the 16 mask-indexed variants of an autotile rule.
/// @param scene SceneDocument handle.
/// @param base_tile Base tile identifier to inspect.
/// @return Owned sequence of boxed variant tile IDs in mask order, empty when
///         absent or unsuccessful.
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

/// @brief Remove an autotile rule if present.
/// @param scene SceneDocument handle.
/// @param base_tile Base tile identifier whose rule is erased.
void rt_game_scene_remove_autotile_rule(void *scene, int64_t base_tile) {
    SCENE_TRY {
        requireScene(scene)->state->behavior.autotiles.erase(base_tile);
    }
    SCENE_CATCH_VOID
}

/// @brief Enumerate base tiles that define autotile rules.
/// @param scene SceneDocument handle.
/// @return Owned sequence of boxed base tile IDs in ascending order, or an
///         empty owned sequence if the operation fails.
void *rt_game_scene_autotile_bases(void *scene){
    SCENE_TRY{const auto &autotiles = requireScene(scene) -> state->behavior.autotiles;
void *seq = rt_seq_new_owned();
for (const auto &[base, _] : autotiles)
    pushIntValue(seq, base);
return seq;
}
SCENE_CATCH(rt_seq_new_owned())
}

/// @brief Report the scalar kind of a typed camera field.
/// @param scene SceneDocument handle.
/// @param key Camera field name.
/// @return Newly allocated kind name, or an empty string when absent or
///         unsuccessful.
rt_string rt_game_scene_camera_field_kind(void *scene, rt_string key){SCENE_TRY{
    const SceneScalar *scalar = findSectionScalar(requireScene(scene) -> state->camera, key);
return makeString(scalar ? scalarKindName(scalar->kind) : "");
}
SCENE_CATCH(makeString(""))
}

/// @brief Read an integer camera field.
/// @param scene SceneDocument handle.
/// @param key Camera field name.
/// @param def Fallback for absence, type mismatch, or failure.
/// @return Stored integer or @p def.
int64_t rt_game_scene_camera_get_int(void *scene, rt_string key, int64_t def){SCENE_TRY{
    const SceneScalar *scalar = findSectionScalar(requireScene(scene) -> state->camera, key);
return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
}
SCENE_CATCH(def)
}

/// @brief Read a string camera field.
/// @param scene SceneDocument handle.
/// @param key Camera field name.
/// @param def Fallback copied for absence or type mismatch.
/// @return Newly allocated stored or fallback string.
rt_string rt_game_scene_camera_get_str(void *scene, rt_string key, rt_string def) {
    SCENE_TRY {
        const SceneScalar *scalar = findSectionScalar(requireScene(scene)->state->camera, key);
        return makeString(scalar && scalar->kind == ScalarKind::String ? scalar->stringValue
                                                                       : toStd(def));
    }
    SCENE_CATCH(makeString(""))
}

/// @brief Store a validated integer camera field.
/// @details Known camera keys enforce their ADR 0177 ranges; unknown keys are
///          retained as extension fields after shared key validation.
/// @param scene SceneDocument handle.
/// @param key Camera field name.
/// @param value Integer value to store.
void rt_game_scene_camera_set_int(void *scene, rt_string key, int64_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setSectionScalar(s, s.camera, key, makeIntScalar(value));
    }
    SCENE_CATCH_VOID
}

/// @brief Store a validated string camera field.
/// @details The known `mode` field accepts only `follow` or `fixed`; unknown
///          keys are retained as extension fields subject to shared limits.
/// @param scene SceneDocument handle.
/// @param key Camera field name.
/// @param value String value to copy.
void rt_game_scene_camera_set_str(void *scene, rt_string key, rt_string value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setSectionScalar(s, s.camera, key, makeStringScalar(toStd(value)));
    }
    SCENE_CATCH_VOID
}

/// @brief Remove a typed camera field if present.
/// @param scene SceneDocument handle.
/// @param key Camera field name to erase.
void rt_game_scene_camera_remove(void *scene, rt_string key) {
    SCENE_TRY {
        requireScene(scene)->state->camera.fields.erase(toStd(key));
    }
    SCENE_CATCH_VOID
}

/// @brief Enumerate typed camera field names.
/// @param scene SceneDocument handle.
/// @return Owned sequence of owned strings in lexical key order, or an empty
///         owned sequence if the operation fails.
void *rt_game_scene_camera_keys(void *scene){
    SCENE_TRY{return sectionKeySeq(requireScene(scene) -> state->camera);
}
SCENE_CATCH(rt_seq_new_owned())
}

/// @brief Report the scalar kind of a typed lighting field.
/// @param scene SceneDocument handle.
/// @param key Lighting field name.
/// @return Newly allocated kind name, or an empty string when absent or
///         unsuccessful.
rt_string rt_game_scene_lighting_field_kind(void *scene, rt_string key){SCENE_TRY{
    const SceneScalar *scalar = findSectionScalar(requireScene(scene) -> state->lighting, key);
return makeString(scalar ? scalarKindName(scalar->kind) : "");
}
SCENE_CATCH(makeString(""))
}

/// @brief Read an integer lighting field.
/// @param scene SceneDocument handle.
/// @param key Lighting field name.
/// @param def Fallback for absence, type mismatch, or failure.
/// @return Stored integer or @p def.
int64_t rt_game_scene_lighting_get_int(void *scene, rt_string key, int64_t def) {
    SCENE_TRY {
        const SceneScalar *scalar = findSectionScalar(requireScene(scene)->state->lighting, key);
        return scalar && scalar->kind == ScalarKind::Int ? scalar->intValue : def;
    }
    SCENE_CATCH(def)
}

/// @brief Store a validated integer lighting field.
/// @details Known darkness, color, and player-light-radius keys enforce their
///          ADR 0177 ranges; unknown keys remain available for extensions.
/// @param scene SceneDocument handle.
/// @param key Lighting field name.
/// @param value Integer value to store.
void rt_game_scene_lighting_set_int(void *scene, rt_string key, int64_t value) {
    SCENE_TRY {
        SceneState &s = *requireScene(scene)->state;
        setSectionScalar(s, s.lighting, key, makeIntScalar(value));
    }
    SCENE_CATCH_VOID
}

/// @brief Remove a typed lighting field if present.
/// @param scene SceneDocument handle.
/// @param key Lighting field name to erase.
void rt_game_scene_lighting_remove(void *scene, rt_string key) {
    SCENE_TRY {
        requireScene(scene)->state->lighting.fields.erase(toStd(key));
    }
    SCENE_CATCH_VOID
}

/// @brief Enumerate typed lighting field names.
/// @param scene SceneDocument handle.
/// @return Owned sequence of owned strings in lexical key order, or an empty
///         owned sequence if the operation fails.
void *rt_game_scene_lighting_keys(void *scene) {
    SCENE_TRY {
        return sectionKeySeq(requireScene(scene)->state->lighting);
    }
    SCENE_CATCH(rt_seq_new_owned())
}

} // extern "C"
