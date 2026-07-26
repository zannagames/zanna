//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_tiled_import.cpp
/// @file
/// @brief Implements dependency-aware Tiled JSON/TMX import, canonical tile
///        identity, SceneDocument conversion, and render-atlas composition.
// Purpose: Dependency-aware import of bounded/infinite and projected Tiled
//   JSON/TMX maps into SceneDocument and render-ready Tilemap runtime objects.
//
// Key invariants:
//   - Filesystem and asset-package dependency graphs use separate readers.
//   - Malformed, unsafe, or ambiguous Tiled input fails before publication.
//   - Layer GIDs are normalized to map-wide canonical tile identities.
//   - Input, decoded layer, dependency, and output sizes are explicitly bounded.
//
// Ownership/Lifetime:
//   - Runtime parse trees are released before an import returns.
//   - Result objects retain their SceneDocument/Tilemap payloads.
//   - Temporary Pixels and SceneDocument handles are released after transfer.
//
// Links: rt_scene_editor.h, ../graphics/2d/rt_graphics2d.h,
//   docs/adr/0140-tiled-map-and-scene-import.md,
//   docs/adr/0144-complete-tiled-map-import.md
//
//===----------------------------------------------------------------------===//

#include "rt_scene_editor.h"

#include "rt_asset.h"
#include "rt_box.h"
#include "rt_compress.h"
#include "rt_crc32.h"
#include "rt_graphics2d.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_tilemap.h"
#include "rt_xml.h"
#include "rt_zstd.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

/// @brief Runtime class identifier for TiledMapLoader receiver validation.
constexpr int64_t kTiledMapLoaderClassId = INT64_C(-0x620121);
/// @brief Maximum byte size accepted for any root or dependency document.
constexpr size_t kMaxDocumentBytes = 16u * 1024u * 1024u;
/// @brief Maximum number of dependency reads in one import graph.
constexpr size_t kMaxDependencyCount = 4096u;
/// @brief Maximum decoded cells accepted for a single layer.
constexpr size_t kMaxCellsPerLayer = 1024u * 1024u;
/// @brief Maximum decoded cells accumulated across all layers.
constexpr size_t kMaxTotalCells = 4u * 1024u * 1024u;
/// @brief Maximum number of published scene layers.
constexpr size_t kMaxLayers = 16u;
/// @brief Maximum number of published scene objects.
constexpr size_t kMaxObjects = 65536u;
/// @brief Maximum number of properties accepted in a property collection.
constexpr size_t kMaxProperties = 16384u;
/// @brief Maximum byte length accepted for scalar strings.
constexpr size_t kMaxStringBytes = 64u * 1024u;
/// @brief Tiled GID flag for horizontal reflection.
constexpr uint64_t kTiledHorizontalFlag = UINT64_C(0x80000000);
/// @brief Tiled GID flag for vertical reflection.
constexpr uint64_t kTiledVerticalFlag = UINT64_C(0x40000000);
/// @brief Tiled GID flag for diagonal reflection or 60-degree hex rotation.
constexpr uint64_t kTiledDiagonalOrHex60Flag = UINT64_C(0x20000000);
/// @brief Tiled GID flag for 120-degree hex rotation.
constexpr uint64_t kTiledHex120Flag = UINT64_C(0x10000000);
/// @brief Mask containing every transform bit encoded in a Tiled GID.
constexpr uint64_t kTiledTransformMask =
    kTiledHorizontalFlag | kTiledVerticalFlag | kTiledDiagonalOrHex60Flag | kTiledHex120Flag;

/// @brief Release one owned runtime-object reference.
/// @param object Owned object handle; null is ignored.
void releaseObject(void *object) {
    if (object && rt_obj_release_check0(object))
        rt_obj_free(object);
}

/// @brief Release an owned parsed runtime value of any supported handle kind.
/// @details Runtime strings use their string reference API; other parsed
///          values use generic object release semantics.
/// @param value Owned parsed value; null is ignored.
void releaseParsedValue(void *value) {
    if (!value)
        return;
    if (rt_string_is_handle(value)) {
        rt_string_unref(static_cast<rt_string>(value));
        return;
    }
    releaseObject(value);
}

/// @brief Scope guard for a runtime parse-tree value.
class OwnedObject {
  public:
    /// @brief Adopt a parsed runtime value for automatic release.
    /// @param value Owned value, optionally null.
    explicit OwnedObject(void *value = nullptr) : value_(value) {}

    /// @brief Release the adopted value unless ownership was transferred.
    ~OwnedObject() {
        releaseParsedValue(value_);
    }

    /// @brief Prevent copying an owning parse-tree guard.
    OwnedObject(const OwnedObject &) = delete;
    /// @brief Prevent copy assignment of an owning parse-tree guard.
    OwnedObject &operator=(const OwnedObject &) = delete;

    /// @brief Inspect the guarded value without transferring ownership.
    /// @return Borrowed guarded handle.
    void *get() const {
        return value_;
    }

    /// @brief Transfer the guarded value to the caller.
    /// @return Owned handle formerly held by this guard.
    void *release() {
        void *value = value_;
        value_ = nullptr;
        return value;
    }

  private:
    void *value_;
};

/// @brief Copy explicit runtime-string bytes into a C++ string.
/// @param value Borrowed runtime string; null and invalid storage are accepted.
/// @return Byte-preserving copy, or an empty string when unavailable.
std::string toStd(rt_string value) {
    if (!value)
        return {};
    const char *data = rt_string_cstr(value);
    int64_t length = rt_str_len(value);
    if (!data || length <= 0)
        return {};
    return std::string(data, static_cast<size_t>(length));
}

/// @brief Allocate a runtime string from an explicit C++ byte sequence.
/// @param value Source bytes.
/// @return Owned runtime string.
rt_string makeString(const std::string &value) {
    return rt_string_from_bytes(value.data(), value.size());
}

/// @brief Test whether a runtime handle is a Map object.
/// @param value Borrowed candidate handle.
/// @return `true` only for a non-null Map.
bool isMap(void *value) {
    return value && rt_obj_class_id(value) == RT_MAP_CLASS_ID;
}

/// @brief Test whether a runtime handle is a Seq object.
/// @param value Borrowed candidate handle.
/// @return `true` only for a non-null Seq.
bool isSeq(void *value) {
    return value && rt_obj_class_id(value) == RT_SEQ_CLASS_ID;
}

/// @brief Look up a C-string key in a runtime Map.
/// @param map Borrowed candidate Map.
/// @param key NUL-terminated key.
/// @return Borrowed mapped value, or `nullptr` for a non-Map or absent key.
void *mapGet(void *map, const char *key) {
    if (!isMap(map))
        return nullptr;
    rt_string runtimeKey = rt_const_cstr(key);
    void *value = rt_map_get(map, runtimeKey);
    rt_string_unref(runtimeKey);
    return value;
}

/// @brief Test whether a runtime Map contains a C-string key.
/// @param map Borrowed candidate Map.
/// @param key NUL-terminated key.
/// @return `true` when present.
bool mapHas(void *map, const char *key) {
    if (!isMap(map))
        return false;
    rt_string runtimeKey = rt_const_cstr(key);
    bool present = rt_map_has(map, runtimeKey) != 0;
    rt_string_unref(runtimeKey);
    return present;
}

/// @brief Decode a runtime value as a string.
/// @param value Borrowed candidate runtime string.
/// @param out Destination replaced on success.
/// @return `true` when @p value is a runtime string.
bool jsonStringValue(void *value, std::string &out) {
    if (!value || !rt_string_is_handle(value))
        return false;
    out = toStd(static_cast<rt_string>(value));
    return true;
}

/// @brief Read a string member from a runtime JSON Map.
/// @param map Borrowed Map.
/// @param key Member name.
/// @param out Destination replaced on success.
/// @return `true` when the member exists and is a string.
bool jsonString(void *map, const char *key, std::string &out) {
    return jsonStringValue(mapGet(map, key), out);
}

/// @brief Decode a finite runtime JSON number as double.
/// @param value Borrowed boxed numeric value.
/// @param out Destination replaced on success.
/// @return `true` for finite boxed integers or floats.
bool jsonNumberValue(void *value, double &out) {
    if (!value || rt_obj_class_id(value) != RT_BOX_CLASS_ID)
        return false;
    if (rt_box_type(value) == RT_BOX_F64) {
        out = rt_unbox_f64(value);
        return std::isfinite(out);
    }
    if (rt_box_type(value) == RT_BOX_I64) {
        out = static_cast<double>(rt_unbox_i64(value));
        return true;
    }
    return false;
}

/// @brief Decode a runtime JSON number as an exact signed integer.
/// @param value Borrowed boxed numeric value.
/// @param out Destination replaced on success.
/// @return `true` for boxed integers or integral finite floats in int64 range.
bool jsonIntValue(void *value, int64_t &out) {
    if (!value || rt_obj_class_id(value) != RT_BOX_CLASS_ID)
        return false;
    if (rt_box_type(value) == RT_BOX_I64) {
        out = rt_unbox_i64(value);
        return true;
    }
    if (rt_box_type(value) != RT_BOX_F64)
        return false;
    const double number = rt_unbox_f64(value);
    constexpr double kInt64Limit = 9223372036854775808.0;
    if (!std::isfinite(number) || std::trunc(number) != number || number < -kInt64Limit ||
        number >= kInt64Limit)
        return false;
    out = static_cast<int64_t>(number);
    return true;
}

/// @brief Read an exact integer member from a runtime JSON Map.
/// @param map Borrowed Map.
/// @param key Member name.
/// @param out Destination replaced on success.
/// @return `true` when the member is exactly integral and in range.
bool jsonInt(void *map, const char *key, int64_t &out) {
    return jsonIntValue(mapGet(map, key), out);
}

/// @brief Decode a runtime JSON Boolean.
/// @param value Borrowed boxed value.
/// @param out Destination replaced on success.
/// @return `true` only for a boxed Boolean.
bool jsonBoolValue(void *value, bool &out) {
    if (!value || rt_obj_class_id(value) != RT_BOX_CLASS_ID || rt_box_type(value) != RT_BOX_I1)
        return false;
    out = rt_unbox_i1(value) != 0;
    return true;
}

/// @brief Read a Boolean member from a runtime JSON Map.
/// @param map Borrowed Map.
/// @param key Member name.
/// @param out Destination replaced on success.
/// @return `true` when the member is a Boolean.
bool jsonBool(void *map, const char *key, bool &out) {
    return jsonBoolValue(mapGet(map, key), out);
}

/// @brief Encode arbitrary bytes as a quoted JSON string literal.
/// @param value Bytes to escape.
/// @return Complete quoted JSON literal.
std::string jsonEscape(const std::string &value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
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
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(ch) << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    out << '"';
    return out.str();
}

/// @brief Format a finite double with round-trip-oriented precision.
/// @param value Numeric value to format.
/// @return Locale-sensitive stream representation using 17 significant digits.
std::string jsonDouble(double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

/// @brief Round a finite double to the nearest integer with ties away from zero.
/// @param value Candidate numeric value.
/// @param out Destination replaced on success.
/// @return `true` when the rounded result fits in int64.
bool roundNearestTiesAway(double value, int64_t &out) {
    if (!std::isfinite(value))
        return false;
    double rounded = value < 0.0 ? std::ceil(value - 0.5) : std::floor(value + 0.5);
    long double wide = static_cast<long double>(rounded);
    if (wide < static_cast<long double>(INT64_MIN) || wide > static_cast<long double>(INT64_MAX))
        return false;
    out = static_cast<int64_t>(rounded);
    return true;
}

/// @brief Scalar kinds preserved across Tiled and SceneDocument properties.
enum class ScalarKind { Null, Boolean, Integer, Float, String };

/// @brief Tagged scalar property value.
struct Scalar {
    ScalarKind kind{ScalarKind::Null};
    bool boolean{false};
    int64_t integer{0};
    double floating{0.0};
    std::string string;
};

/// @brief Compare two scalar values including inactive payload fields.
/// @param left First scalar.
/// @param right Second scalar.
/// @return `true` when kind and every stored payload field are equal.
bool scalarEqual(const Scalar &left, const Scalar &right) {
    return left.kind == right.kind && left.boolean == right.boolean &&
           left.integer == right.integer && left.floating == right.floating &&
           left.string == right.string;
}

/// @brief Serialize one scalar as canonical compact JSON.
/// @param value Scalar to encode.
/// @return JSON literal for the active scalar kind.
std::string scalarJson(const Scalar &value) {
    switch (value.kind) {
        case ScalarKind::Null:
            return "null";
        case ScalarKind::Boolean:
            return value.boolean ? "true" : "false";
        case ScalarKind::Integer:
            return std::to_string(value.integer);
        case ScalarKind::Float:
            return jsonDouble(value.floating);
        case ScalarKind::String:
            return jsonEscape(value.string);
    }
    return "null";
}

/// @brief Convert a runtime JSON scalar into importer property storage.
/// @details Declared `int` and `object` values must be integral. Other numbers
///          preserve integral representation when possible and otherwise use
///          finite floating-point storage; strings obey the 64 KiB limit.
/// @param value Borrowed parsed value; null represents JSON null.
/// @param declaredType Optional Tiled property type.
/// @param out Destination scalar replaced on success.
/// @return `true` for a supported bounded scalar.
bool scalarFromJson(void *value, const std::string &declaredType, Scalar &out) {
    if (!value) {
        out = Scalar{};
        return true;
    }
    bool boolean = false;
    if (jsonBoolValue(value, boolean)) {
        out.kind = ScalarKind::Boolean;
        out.boolean = boolean;
        return true;
    }
    std::string string;
    if (jsonStringValue(value, string)) {
        out.kind = ScalarKind::String;
        out.string = std::move(string);
        return out.string.size() <= kMaxStringBytes;
    }
    double number = 0.0;
    if (!jsonNumberValue(value, number))
        return false;
    if (declaredType == "int" || declaredType == "object") {
        if (std::trunc(number) != number || number < static_cast<double>(INT64_MIN) ||
            number > static_cast<double>(INT64_MAX))
            return false;
        out.kind = ScalarKind::Integer;
        out.integer = static_cast<int64_t>(number);
    } else if (std::trunc(number) == number && number >= static_cast<double>(INT64_MIN) &&
               number <= static_cast<double>(INT64_MAX)) {
        out.kind = ScalarKind::Integer;
        out.integer = static_cast<int64_t>(number);
    } else {
        out.kind = ScalarKind::Float;
        out.floating = number;
    }
    return true;
}

/// @brief Parse a complete decimal string as signed 64-bit integer.
/// @param text Candidate text without surrounding whitespace.
/// @param out Destination replaced on success.
/// @return `true` when the entire string parses without range error.
bool parseIntegerText(const std::string &text, int64_t &out) {
    if (text.empty())
        return false;
    char *end = nullptr;
    errno = 0;
    long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return false;
    out = static_cast<int64_t>(value);
    return true;
}

/// @brief Parse a complete decimal string as a finite double.
/// @param text Candidate text without surrounding whitespace.
/// @param out Destination replaced on success.
/// @return `true` when the entire string parses without range error.
bool parseDoubleText(const std::string &text, double &out) {
    if (text.empty())
        return false;
    char *end = nullptr;
    errno = 0;
    double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || !end || *end != '\0' || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

/// @brief Lowercase ASCII letters without altering other bytes.
/// @param value String to normalize in place.
/// @return Normalized string.
std::string lowerAscii(std::string value) {
    for (char &ch : value) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value;
}

/// @brief Decode one hexadecimal digit.
/// @param ch Candidate ASCII character.
/// @return Nibble value 0..15, or `-1` for a non-hex character.
int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/// @brief Parse Tiled `#RRGGBB` or `#AARRGGBB` color text.
/// @param text Tiled color literal.
/// @param color Destination ARGB value; six-digit input receives opaque alpha.
/// @return `true` when syntax and every digit are valid.
bool parseTiledColor(const std::string &text, uint32_t &color) {
    if ((text.size() != 7u && text.size() != 9u) || text.front() != '#')
        return false;
    uint32_t value = 0u;
    for (size_t index = 1u; index < text.size(); ++index) {
        int digit = hexDigit(text[index]);
        if (digit < 0)
            return false;
        value = (value << 4u) | static_cast<uint32_t>(digit);
    }
    if (text.size() == 7u)
        value |= UINT32_C(0xFF000000);
    color = value;
    return true;
}

/// @brief Component-multiply two 8-bit-per-channel ARGB colors.
/// @param left First packed color.
/// @param right Second packed color.
/// @return Rounded per-channel product divided by 255.
uint32_t multiplyTiledColors(uint32_t left, uint32_t right) {
    uint32_t result = 0u;
    for (unsigned int shift : {24u, 16u, 8u, 0u}) {
        uint32_t a = (left >> shift) & 0xFFu;
        uint32_t b = (right >> shift) & 0xFFu;
        result |= ((a * b + 127u) / 255u) << shift;
    }
    return result;
}

/// @brief Strip an optional UTF-8 BOM, leading whitespace, and XML declaration.
/// @param text Raw TMX/TSX document bytes.
/// @return XML content beginning at the first post-declaration byte.
std::string normalizeTiledXmlInput(const std::string &text) {
    size_t position = 0;
    if (text.size() >= 3u && static_cast<unsigned char>(text[0]) == 0xEFu &&
        static_cast<unsigned char>(text[1]) == 0xBBu &&
        static_cast<unsigned char>(text[2]) == 0xBFu)
        position = 3u;
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                      text[position] == '\r' || text[position] == '\n'))
        ++position;
    if (text.compare(position, 5u, "<?xml") == 0) {
        size_t end = text.find("?>", position + 5u);
        if (end != std::string::npos)
            position = end + 2u;
    }
    return text.substr(position);
}

/// @brief Canonical animation rule emitted for one base tile.
struct TileAnimation {
    int64_t baseTile{0};
    int64_t milliseconds{0};
    std::vector<int64_t> frames;
    std::vector<int64_t> durations;
};

/// @brief Per-local-tile collision, property, and animation metadata.
struct TileMetadata {
    bool solid{false};
    std::map<std::string, Scalar> properties;
    std::vector<int64_t> animationFrames;
    std::vector<int64_t> animationDurations;
};

/// @brief Image-collection source for one local tileset tile.
struct TileImage {
    std::string path;
    int64_t width{0};
    int64_t height{0};
    uint32_t transparentColor{0};
    bool hasTransparentColor{false};
};

/// @brief Parsed external or embedded Tiled tileset and artwork layout.
struct Tileset {
    int64_t firstGid{1};
    std::string name;
    int64_t tileWidth{0};
    int64_t tileHeight{0};
    int64_t tileCount{0};
    int64_t columns{0};
    int64_t margin{0};
    int64_t spacing{0};
    int64_t imageWidth{0};
    int64_t imageHeight{0};
    int64_t tileOffsetX{0};
    int64_t tileOffsetY{0};
    int64_t canonicalFirstId{0};
    std::string image;
    uint32_t transparentColor{0};
    bool hasTransparentColor{false};
    std::map<int64_t, TileImage> tileImages;
    std::map<int64_t, TileMetadata> metadata;
};

/// @brief Normalized visible tile layer with rendering metadata.
struct Layer {
    std::string name;
    bool visible{true};
    std::vector<int64_t> tiles;
    double offsetX{0.0};
    double offsetY{0.0};
    double opacity{1.0};
    double parallaxX{1.0};
    double parallaxY{1.0};
    uint32_t tint{UINT32_C(0xFFFFFFFF)};
};

/// @brief Canonical tile identity and render transform stored in scene cells.
struct CanonicalTile {
    int64_t tileset{-1};
    int64_t localId{0};
    uint32_t transformFlags{0};
    uint32_t tint{UINT32_C(0xFFFFFFFF)};
    uint16_t opacity{UINT16_MAX};
    bool reachable{false};
};

/// @brief Ordered deduplication key for canonical transformed tile variants.
struct CanonicalTileKey {
    int64_t tileset{-1};
    int64_t localId{0};
    uint32_t transformFlags{0};
    uint32_t tint{UINT32_C(0xFFFFFFFF)};
    uint16_t opacity{UINT16_MAX};

    /// @brief Establish lexicographic ordering for canonical-variant maps.
    /// @param other Key to compare.
    /// @return `true` when this key sorts before @p other.
    bool operator<(const CanonicalTileKey &other) const {
        if (tileset != other.tileset)
            return tileset < other.tileset;
        if (localId != other.localId)
            return localId < other.localId;
        if (transformFlags != other.transformFlags)
            return transformFlags < other.transformFlags;
        if (tint != other.tint)
            return tint < other.tint;
        return opacity < other.opacity;
    }
};

/// @brief Flattened SceneDocument object produced from a Tiled object.
struct SceneObject {
    std::string type;
    std::string id;
    int64_t x{0};
    int64_t y{0};
    std::map<std::string, Scalar> properties;
};

/// @brief Complete normalized intermediate representation of one Tiled map.
struct MapDocument {
    std::string name;
    std::string orientation{"orthogonal"};
    std::string renderOrder{"right-down"};
    std::string staggerAxis;
    std::string staggerIndex;
    bool infinite{false};
    int64_t width{0};
    int64_t height{0};
    int64_t originTileX{0};
    int64_t originTileY{0};
    int64_t projectionHeight{0};
    int64_t tileWidth{0};
    int64_t tileHeight{0};
    int64_t hexSideLength{0};
    double skewX{0.0};
    double skewY{0.0};
    double parallaxOriginX{0.0};
    double parallaxOriginY{0.0};
    int64_t sourceFrameWidth{0};
    int64_t sourceFrameHeight{0};
    int64_t drawOffsetX{0};
    int64_t drawOffsetY{0};
    std::map<std::string, Scalar> properties;
    std::vector<Tileset> tilesets;
    std::vector<CanonicalTile> canonicalTiles;
    std::map<CanonicalTileKey, int64_t> canonicalVariants;
    std::vector<Layer> layers;
    std::vector<SceneObject> objects;
    std::set<int64_t> solidTiles;
    std::map<int64_t, std::map<std::string, Scalar>> tileProperties;
    std::map<int64_t, TileAnimation> animations;
    size_t totalCells{0};
};

/// @brief SceneDocument import result plus render-construction metadata.
struct ImportProduct {
    void *scene{nullptr};
    MapDocument document;
    std::string error;
};

/// @brief Reads bounded Tiled documents through filesystem or asset semantics.
class SourceReader {
  public:
    /// @brief Select the dependency namespace for this import.
    /// @param assetMode `true` for logical asset paths, `false` for filesystem
    ///        paths.
    explicit SourceReader(bool assetMode) : assetMode_(assetMode) {}

    /// @brief Resolve and read the root Tiled document.
    /// @param path Caller-supplied root path.
    /// @param resolved Canonical logical or absolute path written on success.
    /// @param text Complete bounded document bytes written on success.
    /// @param error Human-readable failure detail.
    /// @return `true` after a complete read.
    bool readRoot(const std::string &path,
                  std::string &resolved,
                  std::string &text,
                  std::string &error) {
        if (path.empty()) {
            error = "Tiled import path is empty";
            return false;
        }
        if (assetMode_) {
            resolved = normalizeAssetRoot(path, error);
            if (resolved.empty())
                return false;
        } else {
            std::error_code ec;
            resolved = std::filesystem::absolute(std::filesystem::path(path), ec)
                           .lexically_normal()
                           .string();
            if (ec) {
                error = "cannot resolve Tiled map path: " + path;
                return false;
            }
        }
        return readResolved(resolved, text, error);
    }

    /// @brief Resolve and read one dependency relative to its owner.
    /// @details Counts every request against the 4096-dependency budget and
    ///          rejects absolute/URI references in portable filesystem mode.
    /// @param owner Resolved path of the referencing document.
    /// @param reference Relative dependency text.
    /// @param resolved Resolved dependency path written on success.
    /// @param text Complete bounded dependency bytes written on success.
    /// @param error Human-readable failure detail.
    /// @return `true` after a complete read.
    bool readDependency(const std::string &owner,
                        const std::string &reference,
                        std::string &resolved,
                        std::string &text,
                        std::string &error) {
        if (++dependencyCount_ > kMaxDependencyCount) {
            error = "Tiled dependency count exceeds 4096";
            return false;
        }
        if (reference.empty()) {
            error = "Tiled dependency path is empty";
            return false;
        }
        if (assetMode_) {
            resolved = resolveAsset(owner, reference, error);
            if (resolved.empty())
                return false;
        } else {
            std::filesystem::path ref(reference);
            if (ref.is_absolute() || reference.find(':') != std::string::npos) {
                error = "absolute or URI Tiled dependency paths are not portable: " + reference;
                return false;
            }
            resolved =
                (std::filesystem::path(owner).parent_path() / ref).lexically_normal().string();
        }
        return readResolved(resolved, text, error);
    }

    /// @brief Resolve an image dependency without reading its bytes.
    /// @param owner Resolved path of the referencing document.
    /// @param reference Relative dependency text.
    /// @param resolved Resolved dependency path written on success.
    /// @param error Human-readable failure detail.
    /// @return `true` for a portable path contained by the selected root.
    bool resolveDependencyPath(const std::string &owner,
                               const std::string &reference,
                               std::string &resolved,
                               std::string &error) const {
        if (reference.empty()) {
            error = "Tiled image dependency path is empty";
            return false;
        }
        if (assetMode_) {
            resolved = resolveAsset(owner, reference, error);
            return !resolved.empty();
        }
        std::filesystem::path ref(reference);
        if (ref.is_absolute() || reference.find(':') != std::string::npos) {
            error = "absolute or URI Tiled dependency paths are not portable: " + reference;
            return false;
        }
        resolved = (std::filesystem::path(owner).parent_path() / ref).lexically_normal().string();
        return true;
    }

    /// @brief Report which dependency namespace this reader uses.
    /// @return `true` for asset mode; `false` for filesystem mode.
    bool assetMode() const {
        return assetMode_;
    }

  private:
    /// @brief Normalize and contain a caller-supplied logical asset root.
    /// @param path Asset path, optionally prefixed with `asset://`.
    /// @param error Human-readable rejection detail.
    /// @return Root-relative normalized path, or empty on rejection.
    static std::string normalizeAssetRoot(std::string path, std::string &error) {
        if (path.rfind("asset://", 0) == 0)
            path.erase(0, 8);
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos) {
            error = "unsafe asset path: " + path;
            return {};
        }
        std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
        std::string result = normalized.generic_string();
        if (result.empty() || result == "." || result == ".." || result.rfind("../", 0) == 0) {
            error = "asset path escapes the logical asset root: " + path;
            return {};
        }
        return result;
    }

    /// @brief Resolve a logical asset reference relative to an owner.
    /// @param owner Root-relative owner path.
    /// @param reference Dependency path, optionally prefixed with `asset://`.
    /// @param error Human-readable rejection detail.
    /// @return Contained normalized asset path, or empty on rejection.
    static std::string resolveAsset(const std::string &owner,
                                    std::string reference,
                                    std::string &error) {
        if (reference.rfind("asset://", 0) == 0)
            reference.erase(0, 8);
        std::replace(reference.begin(), reference.end(), '\\', '/');
        if (reference.empty() || reference.front() == '/' ||
            reference.find(':') != std::string::npos) {
            error = "unsafe asset dependency path: " + reference;
            return {};
        }
        std::filesystem::path joined =
            std::filesystem::path(owner).parent_path() / std::filesystem::path(reference);
        std::string result = joined.lexically_normal().generic_string();
        if (result.empty() || result == "." || result == ".." || result.rfind("../", 0) == 0) {
            error = "asset dependency escapes the logical asset root: " + reference;
            return {};
        }
        return result;
    }

    /// @brief Read one already-resolved path with the 16 MiB document limit.
    /// @param resolved Absolute filesystem or logical asset path.
    /// @param text Complete bytes written on success.
    /// @param error Human-readable load or size failure.
    /// @return `true` after a complete bounded read.
    bool readResolved(const std::string &resolved, std::string &text, std::string &error) const {
        if (assetMode_) {
            rt_string path = makeString(resolved);
            size_t size = 0;
            uint8_t *bytes = rt_asset_load_raw(path, &size);
            rt_string_unref(path);
            if (!bytes) {
                error = "cannot load Tiled asset: " + resolved;
                return false;
            }
            if (size > kMaxDocumentBytes) {
                std::free(bytes);
                error = "Tiled document exceeds 16 MiB: " + resolved;
                return false;
            }
            text.assign(reinterpret_cast<const char *>(bytes), size);
            std::free(bytes);
            return true;
        }

        std::ifstream input(resolved, std::ios::binary | std::ios::ate);
        if (!input) {
            error = "cannot open Tiled document: " + resolved;
            return false;
        }
        std::streamoff size = input.tellg();
        if (size < 0 || static_cast<uint64_t>(size) > kMaxDocumentBytes) {
            error = "Tiled document exceeds 16 MiB: " + resolved;
            return false;
        }
        input.seekg(0, std::ios::beg);
        text.resize(static_cast<size_t>(size));
        if (size > 0)
            input.read(text.data(), size);
        if (!input && size > 0) {
            error = "cannot read complete Tiled document: " + resolved;
            return false;
        }
        return true;
    }

    bool assetMode_{false};
    size_t dependencyCount_{0};
};

/// @brief Insert or replace one bounded scalar property.
/// @param properties Ordered target property map.
/// @param name Non-empty key of at most 128 bytes.
/// @param value Scalar moved into the map.
/// @param error Human-readable limit failure.
/// @return `true` when stored; replacement remains allowed at capacity.
bool setProperty(std::map<std::string, Scalar> &properties,
                 const std::string &name,
                 Scalar value,
                 std::string &error) {
    if (name.empty() || name.size() > 128u) {
        error = "Tiled property name is empty or exceeds 128 bytes";
        return false;
    }
    if (properties.size() >= kMaxProperties && properties.find(name) == properties.end()) {
        error = "Tiled property count exceeds 16384";
        return false;
    }
    properties[name] = std::move(value);
    return true;
}

/// @brief Parse Tiled JSON's property-array representation.
/// @details Rejects class values, converts supported scalars, resolves `file`
///          values when a reader is supplied, and enforces name/value/count
///          limits before publication.
/// @param value Borrowed property array; null means no properties.
/// @param properties Target ordered property map.
/// @param error Human-readable schema or limit failure.
/// @param reader Optional dependency resolver for file properties.
/// @param owner Resolved owner path used for relative file properties.
/// @return `true` when every entry is valid.
bool parseJsonProperties(void *value,
                         std::map<std::string, Scalar> &properties,
                         std::string &error,
                         SourceReader *reader = nullptr,
                         const std::string &owner = {}) {
    if (!value)
        return true;
    if (!isSeq(value)) {
        error = "Tiled properties must be an array";
        return false;
    }
    int64_t count = rt_seq_len(value);
    if (count < 0 || static_cast<uint64_t>(count) > kMaxProperties) {
        error = "Tiled property count exceeds 16384";
        return false;
    }
    for (int64_t index = 0; index < count; ++index) {
        void *entry = rt_seq_get(value, index);
        std::string name;
        std::string type;
        if (!isMap(entry) || !jsonString(entry, "name", name)) {
            error = "Tiled property is missing a string name";
            return false;
        }
        jsonString(entry, "type", type);
        if (type == "class") {
            error = "Tiled class properties are not representable as SceneDocument scalars";
            return false;
        }
        Scalar scalar;
        if (!scalarFromJson(mapGet(entry, "value"), type, scalar)) {
            error = "invalid Tiled property value for '" + name + "'";
            return false;
        }
        if (type == "file" && scalar.kind == ScalarKind::String && !scalar.string.empty() &&
            reader) {
            std::string resolved;
            if (!reader->resolveDependencyPath(owner, scalar.string, resolved, error))
                return false;
            scalar.string = std::move(resolved);
        }
        if (!setProperty(properties, name, std::move(scalar), error))
            return false;
    }
    return true;
}

/// @brief Copy an optional XML attribute into a C++ string.
/// @param node Borrowed XML element.
/// @param attribute Attribute name.
/// @param present Optional destination indicating whether the attribute exists.
/// @return Attribute bytes, or empty when absent.
std::string xmlString(void *node, const char *attribute, bool *present = nullptr) {
    rt_string key = rt_const_cstr(attribute);
    bool has = rt_xml_has_attr(node, key) != 0;
    if (present)
        *present = has;
    if (!has) {
        rt_string_unref(key);
        return {};
    }
    rt_string value = rt_xml_attr(node, key);
    rt_string_unref(key);
    std::string result = toStd(value);
    rt_str_release_maybe(value);
    return result;
}

/// @brief Copy an XML node's tag name.
/// @param node Borrowed XML node.
/// @return Tag-name bytes.
std::string xmlTag(void *node) {
    rt_string value = rt_xml_tag(node);
    std::string result = toStd(value);
    rt_str_release_maybe(value);
    return result;
}

/// @brief Copy the concatenated text content of an XML node.
/// @param node Borrowed XML node.
/// @return Text-content bytes.
std::string xmlText(void *node) {
    rt_string value = rt_xml_text_content(node);
    std::string result = toStd(value);
    rt_str_release_maybe(value);
    return result;
}

/// @brief Parse an optional or required XML integer attribute.
/// @param node Borrowed XML element.
/// @param attribute Attribute name.
/// @param out Destination replaced when present and valid.
/// @param required Whether absence is an error.
/// @param error Human-readable absence or syntax failure.
/// @return `true` when valid or permissibly absent.
bool xmlInt(void *node, const char *attribute, int64_t &out, bool required, std::string &error) {
    bool present = false;
    std::string text = xmlString(node, attribute, &present);
    if (!present) {
        if (required)
            error = std::string("TMX element is missing integer attribute '") + attribute + "'";
        return !required;
    }
    if (!parseIntegerText(text, out)) {
        error = std::string("invalid TMX integer attribute '") + attribute + "'";
        return false;
    }
    return true;
}

/// @brief Parse an optional or required finite XML numeric attribute.
/// @param node Borrowed XML element.
/// @param attribute Attribute name.
/// @param out Destination replaced when present and valid.
/// @param required Whether absence is an error.
/// @param error Human-readable absence or syntax failure.
/// @return `true` when valid or permissibly absent.
bool xmlDouble(void *node, const char *attribute, double &out, bool required, std::string &error) {
    bool present = false;
    std::string text = xmlString(node, attribute, &present);
    if (!present) {
        if (required)
            error = std::string("TMX element is missing numeric attribute '") + attribute + "'";
        return !required;
    }
    if (!parseDoubleText(text, out)) {
        error = std::string("invalid TMX numeric attribute '") + attribute + "'";
        return false;
    }
    return true;
}

/// @brief Find the first direct XML element child with a tag name.
/// @param node Borrowed parent node.
/// @param tag Exact tag name.
/// @return Borrowed matching child, or `nullptr` when absent.
void *firstXmlChild(void *node, const char *tag) {
    int64_t count = rt_xml_child_count(node);
    for (int64_t index = 0; index < count; ++index) {
        void *child = rt_xml_child_at(node, index);
        if (rt_xml_node_type(child) == XML_NODE_ELEMENT && xmlTag(child) == tag)
            return child;
    }
    return nullptr;
}

/// @brief Parse a TMX/TSX `<properties>` child.
/// @details Supports Boolean, integer/object, float, string/file/color values;
///          rejects class and unknown types; resolves file properties when a
///          reader is available.
/// @param parent Borrowed parent XML element.
/// @param properties Target ordered property map.
/// @param error Human-readable schema or limit failure.
/// @param reader Optional dependency resolver for file properties.
/// @param owner Resolved owner path used for relative file properties.
/// @return `true` when the section is absent or every property is valid.
bool parseXmlProperties(void *parent,
                        std::map<std::string, Scalar> &properties,
                        std::string &error,
                        SourceReader *reader = nullptr,
                        const std::string &owner = {}) {
    void *container = firstXmlChild(parent, "properties");
    if (!container)
        return true;
    int64_t count = rt_xml_child_count(container);
    for (int64_t index = 0; index < count; ++index) {
        void *property = rt_xml_child_at(container, index);
        if (rt_xml_node_type(property) != XML_NODE_ELEMENT || xmlTag(property) != "property")
            continue;
        std::string name = xmlString(property, "name");
        std::string type = xmlString(property, "type");
        if (type == "class") {
            error = "TMX class properties are not representable as SceneDocument scalars";
            return false;
        }
        bool hasValue = false;
        std::string text = xmlString(property, "value", &hasValue);
        if (!hasValue)
            text = xmlText(property);
        Scalar scalar;
        if (type == "bool") {
            std::string normalized = lowerAscii(text);
            if (normalized != "true" && normalized != "false" && normalized != "1" &&
                normalized != "0") {
                error = "invalid TMX bool property '" + name + "'";
                return false;
            }
            scalar.kind = ScalarKind::Boolean;
            scalar.boolean = normalized == "true" || normalized == "1";
        } else if (type == "int" || type == "object") {
            if (!parseIntegerText(text, scalar.integer)) {
                error = "invalid TMX integer property '" + name + "'";
                return false;
            }
            scalar.kind = ScalarKind::Integer;
        } else if (type == "float") {
            if (!parseDoubleText(text, scalar.floating)) {
                error = "invalid TMX float property '" + name + "'";
                return false;
            }
            scalar.kind = ScalarKind::Float;
        } else if (type.empty() || type == "string" || type == "file" || type == "color") {
            if (text.size() > kMaxStringBytes) {
                error = "TMX string property exceeds 64 KiB";
                return false;
            }
            scalar.kind = ScalarKind::String;
            scalar.string = std::move(text);
        } else {
            error = "unsupported TMX property type '" + type + "'";
            return false;
        }
        if (type == "file" && !scalar.string.empty() && reader) {
            std::string resolved;
            if (!reader->resolveDependencyPath(owner, scalar.string, resolved, error))
                return false;
            scalar.string = std::move(resolved);
        }
        if (!setProperty(properties, name, std::move(scalar), error))
            return false;
    }
    return true;
}

/// @brief Strictly decode whitespace-tolerant RFC 4648 Base64.
/// @details Rejects non-alphabet bytes, misplaced padding, and nonzero unused
///          bits instead of accepting ambiguous layer payloads.
/// @param text Encoded layer text.
/// @param output Decoded bytes replaced on success.
/// @param error Human-readable syntax failure.
/// @return `true` for a non-empty, correctly padded payload.
bool decodeBase64(const std::string &text, std::vector<uint8_t> &output, std::string &error) {
    static constexpr signed char decode[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -2, -1, -1, -2, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62,
        -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -3, -1, -1, -1, 0,
        1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
        23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
        39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    std::vector<unsigned char> compact;
    compact.reserve(text.size());
    for (unsigned char ch : text) {
        if (decode[ch] == -2)
            continue;
        if (decode[ch] == -1) {
            error = "invalid character in Tiled Base64 layer data";
            return false;
        }
        compact.push_back(ch);
    }
    if (compact.empty() || compact.size() % 4u != 0u) {
        error = "invalid Tiled Base64 layer length";
        return false;
    }
    output.clear();
    output.reserve((compact.size() / 4u) * 3u);
    for (size_t index = 0; index < compact.size(); index += 4u) {
        bool finalGroup = index + 4u == compact.size();
        int a = decode[compact[index]];
        int b = decode[compact[index + 1u]];
        int c = decode[compact[index + 2u]];
        int d = decode[compact[index + 3u]];
        if (a < 0 || b < 0 || (!finalGroup && (c < 0 || d < 0)) || (c == -3 && d != -3) ||
            (c == -3 && (b & 0x0F) != 0) || (d == -3 && c >= 0 && (c & 0x03) != 0)) {
            error = "invalid padding in Tiled Base64 layer data";
            return false;
        }
        uint32_t bits = static_cast<uint32_t>(a << 18) | static_cast<uint32_t>(b << 12);
        if (c >= 0)
            bits |= static_cast<uint32_t>(c << 6);
        if (d >= 0)
            bits |= static_cast<uint32_t>(d);
        output.push_back(static_cast<uint8_t>(bits >> 16));
        if (c >= 0)
            output.push_back(static_cast<uint8_t>(bits >> 8));
        if (d >= 0)
            output.push_back(static_cast<uint8_t>(bits));
    }
    return true;
}

/// @brief Decode an unaligned little-endian 32-bit integer.
/// @param data Pointer to at least four readable bytes.
/// @return Decoded unsigned value.
uint32_t readLe32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
           (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

/// @brief Inflate a complete gzip member to an exact expected size.
/// @details Validates method/reserved bits, optional header fields and checksum,
///          raw DEFLATE output size, CRC32, and trailer length.
/// @param input Complete gzip member bytes.
/// @param expectedSize Required decoded byte count.
/// @param output Decoded bytes replaced on success.
/// @param error Human-readable header, inflate, or trailer failure.
/// @return `true` only for a fully validated member.
bool gunzipExact(const std::vector<uint8_t> &input,
                 size_t expectedSize,
                 std::vector<uint8_t> &output,
                 std::string &error) {
    if (input.size() < 18u || input[0] != 0x1Fu || input[1] != 0x8Bu || input[2] != 8u ||
        (input[3] & 0xE0u) != 0u) {
        error = "invalid gzip-compressed Tiled layer data";
        return false;
    }
    uint8_t flags = input[3];
    size_t position = 10u;
    if ((flags & 0x04u) != 0u) {
        if (position + 2u > input.size()) {
            error = "truncated gzip header in Tiled layer data";
            return false;
        }
        size_t extra = static_cast<size_t>(input[position]) |
                       (static_cast<size_t>(input[position + 1u]) << 8u);
        position += 2u;
        if (extra > input.size() - position) {
            error = "truncated gzip extra field in Tiled layer data";
            return false;
        }
        position += extra;
    }
    /// @brief Skip one NUL-terminated optional gzip header field.
    /// @return `true` after consuming its terminator; `false` when truncated.
    auto skipTerminated = [&]() {
        while (position < input.size() && input[position] != 0u)
            ++position;
        if (position >= input.size())
            return false;
        ++position;
        return true;
    };
    if (((flags & 0x08u) != 0u && !skipTerminated()) ||
        ((flags & 0x10u) != 0u && !skipTerminated())) {
        error = "truncated gzip text field in Tiled layer data";
        return false;
    }
    if ((flags & 0x02u) != 0u) {
        if (position + 2u > input.size()) {
            error = "truncated gzip header checksum in Tiled layer data";
            return false;
        }
        uint16_t expected = static_cast<uint16_t>(input[position]) |
                            static_cast<uint16_t>(input[position + 1u] << 8u);
        uint16_t actual = static_cast<uint16_t>(rt_crc32_compute(input.data(), position));
        if (expected != actual) {
            error = "gzip header checksum mismatch in Tiled layer data";
            return false;
        }
        position += 2u;
    }
    if (position + 8u >= input.size()) {
        error = "truncated gzip payload in Tiled layer data";
        return false;
    }
    size_t payloadSize = input.size() - position - 8u;
    uint8_t *decoded = nullptr;
    size_t decodedSize = 0;
    if (!rt_compress_inflate_raw(
            input.data() + position, payloadSize, expectedSize, &decoded, &decodedSize) ||
        decodedSize != expectedSize) {
        std::free(decoded);
        error = "cannot inflate gzip-compressed Tiled layer data";
        return false;
    }
    uint32_t expectedCrc = readLe32(input.data() + input.size() - 8u);
    uint32_t expectedLength = readLe32(input.data() + input.size() - 4u);
    if (expectedCrc != rt_crc32_compute(decoded, decodedSize) ||
        expectedLength != static_cast<uint32_t>(decodedSize)) {
        std::free(decoded);
        error = "gzip trailer mismatch in Tiled layer data";
        return false;
    }
    output.assign(decoded, decoded + decodedSize);
    std::free(decoded);
    return true;
}

/// @brief Decode a Tiled layer byte stream to an exact expected size.
/// @details Supports uncompressed, zlib, gzip, and zstd payloads; unknown
///          compression names fail explicitly.
/// @param encoded Encoded or compressed bytes.
/// @param compression Tiled compression token.
/// @param expectedSize Required decoded byte count.
/// @param decoded Output bytes replaced on success.
/// @param error Human-readable size, codec, or decompression failure.
/// @return `true` when exactly @p expectedSize bytes are produced.
bool decompressLayer(const std::vector<uint8_t> &encoded,
                     const std::string &compression,
                     size_t expectedSize,
                     std::vector<uint8_t> &decoded,
                     std::string &error) {
    if (compression.empty()) {
        if (encoded.size() != expectedSize) {
            error = "uncompressed Tiled Base64 layer has the wrong byte length";
            return false;
        }
        decoded = encoded;
        return true;
    }
    if (compression == "zlib") {
        decoded.resize(expectedSize);
        if (!rt_compress_inflate_zlib_into(
                encoded.data(), encoded.size(), decoded.data(), expectedSize)) {
            error = "cannot inflate zlib-compressed Tiled layer data";
            return false;
        }
        return true;
    }
    if (compression == "gzip")
        return gunzipExact(encoded, expectedSize, decoded, error);
    if (compression == "zstd") {
        uint8_t *data = nullptr;
        size_t size = 0;
        if (!rt_zstd_decompress_raw(encoded.data(), encoded.size(), expectedSize, &data, &size) ||
            size != expectedSize) {
            std::free(data);
            error = "cannot decompress zstd-compressed Tiled layer data";
            return false;
        }
        decoded.assign(data, data + size);
        std::free(data);
        return true;
    }
    error = "unsupported Tiled layer compression '" + compression + "'";
    return false;
}

/// @brief Parse comma-separated unsigned 32-bit Tiled GIDs.
/// @param text CSV payload with optional ASCII whitespace.
/// @param expectedCells Required number of values.
/// @param gids Decoded values replaced on success.
/// @param error Human-readable syntax, range, or count failure.
/// @return `true` when exactly @p expectedCells valid GIDs are parsed.
bool parseCsv(const std::string &text,
              size_t expectedCells,
              std::vector<int64_t> &gids,
              std::string &error) {
    gids.clear();
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() &&
               (text[position] == ' ' || text[position] == '\t' || text[position] == '\r' ||
                text[position] == '\n' || text[position] == ','))
            ++position;
        if (position == text.size())
            break;
        size_t start = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9')
            ++position;
        if (start == position) {
            error = "invalid token in Tiled CSV layer data";
            return false;
        }
        int64_t gid = 0;
        if (!parseIntegerText(text.substr(start, position - start), gid) || gid < 0 ||
            static_cast<uint64_t>(gid) > UINT32_MAX) {
            error = "Tiled CSV GID is outside the uint32 range";
            return false;
        }
        gids.push_back(gid);
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                          text[position] == '\r' || text[position] == '\n'))
            ++position;
        if (position < text.size() && text[position] != ',') {
            error = "Tiled CSV values must be comma-separated";
            return false;
        }
    }
    if (gids.size() != expectedCells) {
        error = "Tiled layer cell count does not match its dimensions";
        return false;
    }
    return true;
}

/// @brief Decode Base64/compressed little-endian Tiled GIDs.
/// @param text Base64 payload.
/// @param compression Empty, `zlib`, `gzip`, or `zstd`.
/// @param expectedCells Required number of GIDs.
/// @param gids Decoded GIDs replaced on success.
/// @param error Human-readable decoding failure.
/// @return `true` when the exact cell payload is decoded.
bool decodeBase64Gids(const std::string &text,
                      const std::string &compression,
                      size_t expectedCells,
                      std::vector<int64_t> &gids,
                      std::string &error) {
    if (expectedCells > SIZE_MAX / 4u) {
        error = "Tiled layer byte count overflows";
        return false;
    }
    std::vector<uint8_t> encoded;
    if (!decodeBase64(text, encoded, error))
        return false;
    std::vector<uint8_t> decoded;
    if (!decompressLayer(encoded, compression, expectedCells * 4u, decoded, error))
        return false;
    gids.resize(expectedCells);
    for (size_t index = 0; index < expectedCells; ++index)
        gids[index] = static_cast<int64_t>(readLe32(decoded.data() + index * 4u));
    return true;
}

/// @brief Interpret JSON tile collision as an unambiguous full-tile solid.
/// @details Every collision object must be an axis-aligned rectangle covering
///          the complete tile at origin; other shapes or tile objects fail.
/// @param objectGroup Borrowed Tiled object-group Map, optionally null.
/// @param tileWidth Expected rectangle width.
/// @param tileHeight Expected rectangle height.
/// @param solid Set when at least one valid full-tile rectangle exists.
/// @param error Human-readable ambiguity or schema failure.
/// @return `true` when the group is absent or fully representable.
bool jsonCollisionIsSolid(
    void *objectGroup, int64_t tileWidth, int64_t tileHeight, bool &solid, std::string &error) {
    solid = false;
    if (!objectGroup)
        return true;
    void *objects = mapGet(objectGroup, "objects");
    if (!isSeq(objects)) {
        error = "Tiled tile collision objectgroup is missing objects";
        return false;
    }
    int64_t count = rt_seq_len(objects);
    for (int64_t index = 0; index < count; ++index) {
        void *object = rt_seq_get(objects, index);
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        if (!isMap(object) || !jsonNumberValue(mapGet(object, "x"), x) ||
            !jsonNumberValue(mapGet(object, "y"), y) ||
            !jsonNumberValue(mapGet(object, "width"), width) ||
            !jsonNumberValue(mapGet(object, "height"), height) || x != 0.0 || y != 0.0 ||
            width != static_cast<double>(tileWidth) || height != static_cast<double>(tileHeight) ||
            mapHas(object, "ellipse") || mapHas(object, "polygon") || mapHas(object, "polyline") ||
            mapHas(object, "point") || mapHas(object, "text") || mapHas(object, "gid")) {
            error = "Tiled tile collision is not an unambiguous full-tile rectangle";
            return false;
        }
        solid = true;
    }
    return true;
}

/// @brief Interpret TMX tile collision as an unambiguous full-tile solid.
/// @details Every `<object>` must cover the complete tile at origin and may
///          contain only a `<properties>` child.
/// @param objectGroup Borrowed `<objectgroup>`, optionally null.
/// @param tileWidth Expected rectangle width.
/// @param tileHeight Expected rectangle height.
/// @param solid Set when at least one valid full-tile rectangle exists.
/// @param error Human-readable ambiguity or schema failure.
/// @return `true` when the group is absent or fully representable.
bool xmlCollisionIsSolid(
    void *objectGroup, int64_t tileWidth, int64_t tileHeight, bool &solid, std::string &error) {
    solid = false;
    if (!objectGroup)
        return true;
    int64_t count = rt_xml_child_count(objectGroup);
    for (int64_t index = 0; index < count; ++index) {
        void *object = rt_xml_child_at(objectGroup, index);
        if (rt_xml_node_type(object) != XML_NODE_ELEMENT || xmlTag(object) != "object")
            continue;
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        if (!xmlDouble(object, "x", x, false, error) || !xmlDouble(object, "y", y, false, error) ||
            !xmlDouble(object, "width", width, true, error) ||
            !xmlDouble(object, "height", height, true, error) || x != 0.0 || y != 0.0 ||
            width != static_cast<double>(tileWidth) || height != static_cast<double>(tileHeight))
            return false;
        int64_t childCount = rt_xml_child_count(object);
        for (int64_t childIndex = 0; childIndex < childCount; ++childIndex) {
            void *shape = rt_xml_child_at(object, childIndex);
            if (rt_xml_node_type(shape) == XML_NODE_ELEMENT && xmlTag(shape) != "properties") {
                error = "TMX tile collision is not an unambiguous full-tile rectangle";
                return false;
            }
        }
        solid = true;
    }
    return true;
}

/// @brief Parses Tiled documents and dependencies into a normalized map model.
class ImportParser {
  public:
    /// @brief Select filesystem or logical-asset dependency semantics.
    /// @param assetMode `true` for asset-backed reads.
    explicit ImportParser(bool assetMode) : reader_(assetMode) {}

    /// @brief Read, parse, normalize, and finalize one Tiled root map.
    /// @param path Caller-supplied root path.
    /// @param document Destination intermediate document.
    /// @param error Human-readable failure detail.
    /// @return `true` only after complete final validation.
    bool parse(const std::string &path, MapDocument &document, std::string &error) {
        std::string owner;
        std::string text;
        if (!reader_.readRoot(path, owner, text, error))
            return false;
        document.name = std::filesystem::path(owner).stem().string();
        if (!parseMapText(owner, text, document, error))
            return false;
        return finalize(document, error);
    }

  private:
    /// @brief Enter a dependency document with depth and cycle checks.
    /// @param owner Resolved document path.
    /// @param error Human-readable depth or cycle failure.
    /// @return `true` when newly active.
    bool enter(const std::string &owner, std::string &error) {
        if (active_.size() >= 16u) {
            error = "Tiled dependency depth exceeds 16 at: " + owner;
            return false;
        }
        if (!active_.insert(owner).second) {
            error = "cyclic Tiled dependency: " + owner;
            return false;
        }
        return true;
    }

    /// @brief Remove a document from the active dependency stack.
    /// @param owner Resolved document path.
    void leave(const std::string &owner) {
        active_.erase(owner);
    }

    /// @brief Dispatch root-map bytes to JSON or TMX parsing.
    /// @param owner Resolved root path used for dependencies.
    /// @param text Complete root bytes.
    /// @param document Destination map model.
    /// @param error Human-readable parsing failure.
    /// @return `true` when the selected parser succeeds.
    bool parseMapText(const std::string &owner,
                      const std::string &text,
                      MapDocument &document,
                      std::string &error) {
        size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            error = "Tiled map is empty: " + owner;
            return false;
        }
        if (!enter(owner, error))
            return false;
        bool ok = text[first] == '<' ? parseTmxMap(owner, text, document, error)
                                     : parseJsonMap(owner, text, document, error);
        leave(owner);
        return ok;
    }

    /// @brief Validate finite-map and tile dimensions against cell limits.
    /// @param document Map model to inspect.
    /// @param error Human-readable dimension failure.
    /// @return `true` for positive dimensions within one-layer capacity.
    bool validateMapDimensions(MapDocument &document, std::string &error) {
        if (document.width <= 0 || document.height <= 0 || document.tileWidth <= 0 ||
            document.tileHeight <= 0) {
            error = "Tiled map dimensions and tile dimensions must be positive";
            return false;
        }
        if (document.width > static_cast<int64_t>(kMaxCellsPerLayer) / document.height) {
            error = "Tiled map layer exceeds 1048576 cells";
            return false;
        }
        return true;
    }

    /// @brief Half-open aggregate tile bounds for infinite-map chunks.
    struct TileBounds {
        bool any{false};
        int64_t minimumX{0};
        int64_t minimumY{0};
        int64_t maximumX{0};
        int64_t maximumY{0};
    };

    /// @brief Expand aggregate bounds to include one half-open chunk rectangle.
    /// @param bounds Aggregate bounds to update.
    /// @param x Chunk origin tile x.
    /// @param y Chunk origin tile y.
    /// @param width Positive chunk width.
    /// @param height Positive chunk height.
    /// @param error Human-readable invalid/overflow failure.
    /// @return `true` when the rectangle is valid and incorporated.
    bool extendTileBounds(TileBounds &bounds,
                          int64_t x,
                          int64_t y,
                          int64_t width,
                          int64_t height,
                          std::string &error) {
        if (width <= 0 || height <= 0 || x > INT64_MAX - width || y > INT64_MAX - height) {
            error = "Tiled infinite chunk rectangle is invalid or overflows";
            return false;
        }
        int64_t endX = x + width;
        int64_t endY = y + height;
        if (!bounds.any) {
            bounds = TileBounds{true, x, y, endX, endY};
        } else {
            bounds.minimumX = std::min(bounds.minimumX, x);
            bounds.minimumY = std::min(bounds.minimumY, y);
            bounds.maximumX = std::max(bounds.maximumX, endX);
            bounds.maximumY = std::max(bounds.maximumY, endY);
        }
        return true;
    }

    /// @brief Recursively accumulate chunk bounds from JSON infinite layers.
    /// @param layers Borrowed layer sequence.
    /// @param inheritedX Parent-group tile x offset.
    /// @param inheritedY Parent-group tile y offset.
    /// @param bounds Aggregate bounds to update.
    /// @param error Human-readable schema or overflow failure.
    /// @return `true` when every tile/group layer is structurally valid.
    bool scanJsonInfiniteLayers(void *layers,
                                int64_t inheritedX,
                                int64_t inheritedY,
                                TileBounds &bounds,
                                std::string &error) {
        if (!isSeq(layers)) {
            error = "Tiled map must contain a layers array";
            return false;
        }
        int64_t count = rt_seq_len(layers);
        for (int64_t index = 0; index < count; ++index) {
            void *layer = rt_seq_get(layers, index);
            std::string type;
            if (!isMap(layer) || !jsonString(layer, "type", type)) {
                error = "Tiled layer is missing a string type";
                return false;
            }
            int64_t ownX = 0;
            int64_t ownY = 0;
            if ((mapHas(layer, "x") && !jsonInt(layer, "x", ownX)) ||
                (mapHas(layer, "y") && !jsonInt(layer, "y", ownY)) ||
                (ownX > 0 && inheritedX > INT64_MAX - ownX) ||
                (ownX < 0 && inheritedX < INT64_MIN - ownX) ||
                (ownY > 0 && inheritedY > INT64_MAX - ownY) ||
                (ownY < 0 && inheritedY < INT64_MIN - ownY)) {
                error = "Tiled infinite layer tile offset is invalid or overflows";
                return false;
            }
            int64_t layerX = inheritedX + ownX;
            int64_t layerY = inheritedY + ownY;
            if (type == "tilelayer") {
                void *chunks = mapGet(layer, "chunks");
                if (!isSeq(chunks)) {
                    error = "infinite Tiled tile layer must contain a chunks array";
                    return false;
                }
                int64_t chunkCount = rt_seq_len(chunks);
                for (int64_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
                    void *chunk = rt_seq_get(chunks, chunkIndex);
                    int64_t x = 0;
                    int64_t y = 0;
                    int64_t width = 0;
                    int64_t height = 0;
                    if (!isMap(chunk) || !jsonInt(chunk, "x", x) || !jsonInt(chunk, "y", y) ||
                        !jsonInt(chunk, "width", width) || !jsonInt(chunk, "height", height) ||
                        (layerX > 0 && x > INT64_MAX - layerX) ||
                        (layerX < 0 && x < INT64_MIN - layerX) ||
                        (layerY > 0 && y > INT64_MAX - layerY) ||
                        (layerY < 0 && y < INT64_MIN - layerY) ||
                        !extendTileBounds(bounds, x + layerX, y + layerY, width, height, error))
                        return false;
                }
            } else if (type == "group") {
                if (!scanJsonInfiniteLayers(mapGet(layer, "layers"), layerX, layerY, bounds, error))
                    return false;
            }
        }
        return true;
    }

    /// @brief Recursively accumulate chunk bounds from TMX infinite layers.
    /// @param parent Borrowed map or group XML node.
    /// @param inheritedX Parent-group tile x offset.
    /// @param inheritedY Parent-group tile y offset.
    /// @param bounds Aggregate bounds to update.
    /// @param error Human-readable schema or overflow failure.
    /// @return `true` when every tile/group layer is structurally valid.
    bool scanXmlInfiniteLayers(void *parent,
                               int64_t inheritedX,
                               int64_t inheritedY,
                               TileBounds &bounds,
                               std::string &error) {
        int64_t count = rt_xml_child_count(parent);
        for (int64_t index = 0; index < count; ++index) {
            void *layer = rt_xml_child_at(parent, index);
            if (rt_xml_node_type(layer) != XML_NODE_ELEMENT)
                continue;
            std::string tag = xmlTag(layer);
            if (tag != "layer" && tag != "group")
                continue;
            int64_t ownX = 0;
            int64_t ownY = 0;
            if (!xmlInt(layer, "x", ownX, false, error) ||
                !xmlInt(layer, "y", ownY, false, error) ||
                (ownX > 0 && inheritedX > INT64_MAX - ownX) ||
                (ownX < 0 && inheritedX < INT64_MIN - ownX) ||
                (ownY > 0 && inheritedY > INT64_MAX - ownY) ||
                (ownY < 0 && inheritedY < INT64_MIN - ownY)) {
                if (error.empty())
                    error = "TMX infinite layer tile offset is invalid or overflows";
                return false;
            }
            int64_t layerX = inheritedX + ownX;
            int64_t layerY = inheritedY + ownY;
            if (tag == "group") {
                if (!scanXmlInfiniteLayers(layer, layerX, layerY, bounds, error))
                    return false;
                continue;
            }
            void *data = firstXmlChild(layer, "data");
            if (!data) {
                error = "infinite TMX tile layer is missing <data>";
                return false;
            }
            int64_t childCount = rt_xml_child_count(data);
            for (int64_t childIndex = 0; childIndex < childCount; ++childIndex) {
                void *chunk = rt_xml_child_at(data, childIndex);
                if (rt_xml_node_type(chunk) != XML_NODE_ELEMENT || xmlTag(chunk) != "chunk")
                    continue;
                int64_t x = 0;
                int64_t y = 0;
                int64_t width = 0;
                int64_t height = 0;
                if (!xmlInt(chunk, "x", x, true, error) || !xmlInt(chunk, "y", y, true, error) ||
                    !xmlInt(chunk, "width", width, true, error) ||
                    !xmlInt(chunk, "height", height, true, error) ||
                    (layerX > 0 && x > INT64_MAX - layerX) ||
                    (layerX < 0 && x < INT64_MIN - layerX) ||
                    (layerY > 0 && y > INT64_MAX - layerY) ||
                    (layerY < 0 && y < INT64_MIN - layerY) ||
                    !extendTileBounds(bounds, x + layerX, y + layerY, width, height, error))
                    return false;
            }
        }
        return true;
    }

    /// @brief Convert aggregate infinite bounds into finite scene dimensions.
    /// @details An infinite map with no chunks becomes a 1-by-1 scene at
    ///          origin; nonempty bounds retain their minimum tile as the
    ///          placement origin.
    /// @param bounds Scanned half-open chunk bounds.
    /// @param document Map model to update.
    /// @param error Human-readable invalid/limit failure.
    /// @return `true` when resulting dimensions pass normal validation.
    bool applyInfiniteBounds(const TileBounds &bounds, MapDocument &document, std::string &error) {
        if (!bounds.any) {
            document.originTileX = 0;
            document.originTileY = 0;
            document.width = 1;
            document.height = 1;
            return validateMapDimensions(document, error);
        }
        if (bounds.maximumX <= bounds.minimumX || bounds.maximumY <= bounds.minimumY) {
            error = "Tiled infinite map bounds are invalid";
            return false;
        }
        document.originTileX = bounds.minimumX;
        document.originTileY = bounds.minimumY;
        document.width = bounds.maximumX - bounds.minimumX;
        document.height = bounds.maximumY - bounds.minimumY;
        return validateMapDimensions(document, error);
    }

    /// @brief Validate orientation, render order, stagger, and hex geometry.
    /// @param document Map model to inspect.
    /// @param error Human-readable unsupported-value failure.
    /// @return `true` when projection metadata is supported and consistent.
    bool validateOrientation(MapDocument &document, std::string &error) {
        if (document.orientation != "orthogonal" && document.orientation != "isometric" &&
            document.orientation != "staggered" && document.orientation != "hexagonal" &&
            document.orientation != "oblique") {
            error = "unsupported Tiled map orientation '" + document.orientation + "'";
            return false;
        }
        const bool validRenderOrder =
            document.renderOrder == "right-down" || document.renderOrder == "right-up" ||
            document.renderOrder == "left-down" || document.renderOrder == "left-up";
        if (!validRenderOrder) {
            error = "unsupported Tiled render order '" + document.renderOrder + "'";
            return false;
        }
        bool staggered = document.orientation == "staggered" || document.orientation == "hexagonal";
        if (staggered && document.staggerAxis != "x" && document.staggerAxis != "y") {
            error = "staggered and hexagonal Tiled maps require stagger axis x or y";
            return false;
        }
        if (staggered && document.staggerIndex != "odd" && document.staggerIndex != "even") {
            error = "staggered and hexagonal Tiled maps require stagger index odd or even";
            return false;
        }
        if (document.orientation == "hexagonal") {
            int64_t axisSize =
                document.staggerAxis == "x" ? document.tileWidth : document.tileHeight;
            if (document.hexSideLength < 0 || document.hexSideLength > axisSize) {
                error = "Tiled hex side length is outside its tile-axis extent";
                return false;
            }
        }
        return true;
    }

    /// @brief Establish canonical untransformed IDs for every tileset tile.
    /// @details Derives missing tile counts when safe, verifies ordered
    ///          nonoverlapping GID ranges below transform bits, enforces the
    ///          canonical count budget, and reserves canonical ID zero as empty.
    /// @param document Parsed map and tilesets to update.
    /// @param error Human-readable range or derivation failure.
    /// @return `true` when base canonical identities are complete.
    bool prepareCanonicalTiles(MapDocument &document, std::string &error) {
        document.canonicalTiles.clear();
        document.canonicalVariants.clear();
        document.canonicalTiles.push_back(CanonicalTile{});
        for (size_t index = 0; index < document.tilesets.size(); ++index) {
            Tileset &tileset = document.tilesets[index];
            if (index > 0 && document.tilesets[index - 1u].firstGid == tileset.firstGid) {
                error = "Tiled tilesets have duplicate firstgid values";
                return false;
            }
            if (tileset.tileCount <= 0 && tileset.columns > 0 && tileset.imageWidth > 0 &&
                tileset.imageHeight > 0) {
                int64_t usableWidth = tileset.imageWidth - tileset.margin * 2;
                int64_t usableHeight = tileset.imageHeight - tileset.margin * 2;
                if (usableWidth >= tileset.tileWidth && usableHeight >= tileset.tileHeight) {
                    int64_t rows =
                        (usableHeight + tileset.spacing) / (tileset.tileHeight + tileset.spacing);
                    tileset.tileCount = tileset.columns * rows;
                }
            }
            if (tileset.tileCount <= 0 && !tileset.metadata.empty()) {
                int64_t maximumId = tileset.metadata.rbegin()->first;
                if (maximumId < INT64_MAX)
                    tileset.tileCount = maximumId + 1;
            }
            if (tileset.tileCount <= 0) {
                error = "Tiled tileset tilecount cannot be derived safely";
                return false;
            }
            if (tileset.firstGid > static_cast<int64_t>(UINT32_C(0x0FFFFFFF)) ||
                tileset.tileCount - 1 >
                    static_cast<int64_t>(UINT32_C(0x0FFFFFFF)) - tileset.firstGid) {
                error = "Tiled tileset GID range overlaps transform bits";
                return false;
            }
            if (index > 0) {
                const Tileset &previous = document.tilesets[index - 1u];
                if (previous.tileCount > tileset.firstGid - previous.firstGid) {
                    error = "Tiled tileset GID ranges overlap";
                    return false;
                }
            }
            if (tileset.tileCount > static_cast<int64_t>(kMaxCellsPerLayer) ||
                document.canonicalTiles.size() >
                    kMaxCellsPerLayer - static_cast<size_t>(tileset.tileCount)) {
                error = "Tiled canonical tile count exceeds 1048576";
                return false;
            }
            tileset.canonicalFirstId = static_cast<int64_t>(document.canonicalTiles.size());
            for (int64_t localId = 0; localId < tileset.tileCount; ++localId) {
                CanonicalTile tile;
                tile.tileset = static_cast<int64_t>(index);
                tile.localId = localId;
                document.canonicalTiles.push_back(tile);
                CanonicalTileKey key;
                key.tileset = tile.tileset;
                key.localId = localId;
                document.canonicalVariants[key] =
                    static_cast<int64_t>(document.canonicalTiles.size() - 1u);
            }
        }
        return true;
    }

    /// @brief Parse a Tiled JSON root map into the intermediate document.
    /// @details Validates root metadata and dimensions, resolves external or
    ///          embedded tilesets, prepares canonical IDs, then recursively
    ///          parses layers and objects before later finalization.
    /// @param owner Resolved root path for relative dependencies.
    /// @param text Complete JSON bytes.
    /// @param document Destination map model.
    /// @param error Human-readable parse/schema/limit failure.
    /// @return `true` when all root content is representable.
    bool parseJsonMap(const std::string &owner,
                      const std::string &text,
                      MapDocument &document,
                      std::string &error) {
        rt_string source = makeString(text);
        void *rawRoot = nullptr;
        rt_string message = nullptr;
        int64_t line = 0;
        int64_t column = 0;
        bool parsed = rt_json_try_parse(source, &rawRoot, &message, &line, &column) != 0;
        rt_string_unref(source);
        OwnedObject root(rawRoot);
        if (!parsed || !isMap(root.get())) {
            error = "invalid Tiled JSON map";
            if (message) {
                error += ": " + toStd(message);
                rt_string_unref(message);
            }
            if (line > 0)
                error += " at " + std::to_string(line) + ":" + std::to_string(column);
            return false;
        }
        std::string rootType;
        if (!jsonString(root.get(), "type", rootType) || rootType != "map") {
            error = "Tiled JSON root must identify type 'map'";
            return false;
        }
        if (!jsonString(root.get(), "orientation", document.orientation)) {
            error = "Tiled map is missing a string orientation";
            return false;
        }
        if (mapHas(root.get(), "renderorder") &&
            !jsonString(root.get(), "renderorder", document.renderOrder)) {
            error = "Tiled map renderorder must be a string";
            return false;
        }
        if (mapHas(root.get(), "infinite") &&
            !jsonBool(root.get(), "infinite", document.infinite)) {
            error = "Tiled map 'infinite' must be boolean";
            return false;
        }
        int64_t sourceWidth = 0;
        int64_t sourceHeight = 0;
        if (!jsonInt(root.get(), "width", sourceWidth) ||
            !jsonInt(root.get(), "height", sourceHeight) ||
            !jsonInt(root.get(), "tilewidth", document.tileWidth) ||
            !jsonInt(root.get(), "tileheight", document.tileHeight) || document.tileWidth <= 0 ||
            document.tileHeight <= 0) {
            error = "Tiled map dimensions and tile dimensions are invalid";
            return false;
        }
        document.projectionHeight = sourceHeight;
        if (mapHas(root.get(), "staggeraxis") &&
            !jsonString(root.get(), "staggeraxis", document.staggerAxis)) {
            error = "Tiled map staggeraxis must be a string";
            return false;
        }
        if (mapHas(root.get(), "staggerindex") &&
            !jsonString(root.get(), "staggerindex", document.staggerIndex)) {
            error = "Tiled map staggerindex must be a string";
            return false;
        }
        if (mapHas(root.get(), "hexsidelength") &&
            !jsonInt(root.get(), "hexsidelength", document.hexSideLength)) {
            error = "Tiled map hexsidelength must be integral";
            return false;
        }
        if ((mapHas(root.get(), "skewx") &&
             !jsonNumberValue(mapGet(root.get(), "skewx"), document.skewX)) ||
            (mapHas(root.get(), "skewy") &&
             !jsonNumberValue(mapGet(root.get(), "skewy"), document.skewY)) ||
            (mapHas(root.get(), "parallaxoriginx") &&
             !jsonNumberValue(mapGet(root.get(), "parallaxoriginx"), document.parallaxOriginX)) ||
            (mapHas(root.get(), "parallaxoriginy") &&
             !jsonNumberValue(mapGet(root.get(), "parallaxoriginy"), document.parallaxOriginY))) {
            error = "Tiled projection and parallax-origin values must be finite numbers";
            return false;
        }
        if (!validateOrientation(document, error))
            return false;

        void *layers = mapGet(root.get(), "layers");
        if (!isSeq(layers)) {
            error = "Tiled map must contain a layers array";
            return false;
        }
        if (document.infinite) {
            if (sourceWidth < 0 || sourceHeight < 0) {
                error = "infinite Tiled map source dimensions cannot be negative";
                return false;
            }
            TileBounds bounds;
            if (!scanJsonInfiniteLayers(layers, 0, 0, bounds, error) ||
                !applyInfiniteBounds(bounds, document, error))
                return false;
        } else {
            document.width = sourceWidth;
            document.height = sourceHeight;
            if (!validateMapDimensions(document, error))
                return false;
        }
        if (!parseJsonProperties(
                mapGet(root.get(), "properties"), document.properties, error, &reader_, owner))
            return false;

        void *tilesets = mapGet(root.get(), "tilesets");
        if (!isSeq(tilesets)) {
            error = "Tiled map must contain a tilesets array";
            return false;
        }
        int64_t tilesetCount = rt_seq_len(tilesets);
        for (int64_t index = 0; index < tilesetCount; ++index) {
            void *entry = rt_seq_get(tilesets, index);
            int64_t firstGid = 0;
            if (!isMap(entry) || !jsonInt(entry, "firstgid", firstGid) || firstGid <= 0) {
                error = "Tiled tileset reference has an invalid firstgid";
                return false;
            }
            Tileset tileset;
            tileset.firstGid = firstGid;
            std::string dependency;
            if (jsonString(entry, "source", dependency)) {
                std::string resolved;
                std::string dependencyText;
                if (!reader_.readDependency(owner, dependency, resolved, dependencyText, error) ||
                    !parseTilesetText(resolved, dependencyText, tileset, error))
                    return false;
            } else if (!parseJsonTileset(owner, entry, tileset, error)) {
                return false;
            }
            document.tilesets.push_back(std::move(tileset));
        }
        if (document.tilesets.empty()) {
            error = "Tiled map has no tilesets";
            return false;
        }
        std::sort(document.tilesets.begin(),
                  document.tilesets.end(),
                  /// @brief Order parsed tilesets by their first global tile ID.
                  /// @param left First tileset candidate.
                  /// @param right Second tileset candidate.
                  /// @return `true` when @p left precedes @p right by first GID.
                  [](const Tileset &left, const Tileset &right) {
                      return left.firstGid < right.firstGid;
                  });
        if (!prepareCanonicalTiles(document, error))
            return false;

        return parseJsonLayers(owner,
                               layers,
                               "",
                               true,
                               0.0,
                               0.0,
                               1.0,
                               1.0,
                               1.0,
                               UINT32_C(0xFFFFFFFF),
                               document,
                               error);
    }

    /// @brief Dispatch an external TSJ or TSX tileset dependency.
    /// @param owner Resolved tileset path used for nested image references.
    /// @param text Complete tileset bytes.
    /// @param tileset Destination retaining the map-provided first GID.
    /// @param error Human-readable parse/schema/dependency failure.
    /// @return `true` when the dependency is valid and acyclic.
    bool parseTilesetText(const std::string &owner,
                          const std::string &text,
                          Tileset &tileset,
                          std::string &error) {
        size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            error = "Tiled tileset is empty: " + owner;
            return false;
        }
        if (!enter(owner, error))
            return false;
        bool ok = false;
        if (text[first] == '<') {
            std::string normalizedXml = normalizeTiledXmlInput(text);
            rt_string source = makeString(normalizedXml);
            OwnedObject document(rt_xml_parse(source));
            rt_string_unref(source);
            void *root = document.get() ? rt_xml_root(document.get()) : nullptr;
            if (!root || xmlTag(root) != "tileset") {
                error = "external TSX root must be <tileset>";
            } else {
                ok = parseXmlTileset(owner, root, tileset, error);
            }
        } else {
            rt_string source = makeString(text);
            void *rawRoot = nullptr;
            rt_string message = nullptr;
            bool parsed = rt_json_try_parse(source, &rawRoot, &message, nullptr, nullptr) != 0;
            rt_string_unref(source);
            OwnedObject root(rawRoot);
            if (!parsed || !isMap(root.get())) {
                error = "invalid external TSJ tileset";
                if (message) {
                    error += ": " + toStd(message);
                    rt_string_unref(message);
                }
            } else {
                ok = parseJsonTileset(owner, root.get(), tileset, error);
            }
        }
        leave(owner);
        return ok;
    }

    /// @brief Parse embedded or external JSON tileset content.
    /// @details Validates layout dimensions, resolves root/per-tile artwork,
    ///          parses scalar/collision/animation metadata, rejects duplicate
    ///          tile IDs, and requires at least one artwork source.
    /// @param owner Resolved containing document path.
    /// @param root Borrowed tileset Map.
    /// @param tileset Destination tileset model.
    /// @param error Human-readable schema or dependency failure.
    /// @return `true` when all declared content is representable.
    bool parseJsonTileset(const std::string &owner,
                          void *root,
                          Tileset &tileset,
                          std::string &error) {
        std::string rootType;
        if (mapHas(root, "type") &&
            (!jsonString(root, "type", rootType) || rootType != "tileset")) {
            error = "Tiled JSON tileset root has the wrong type";
            return false;
        }
        jsonString(root, "name", tileset.name);
        if (!jsonInt(root, "tilewidth", tileset.tileWidth) ||
            !jsonInt(root, "tileheight", tileset.tileHeight) || tileset.tileWidth <= 0 ||
            tileset.tileHeight <= 0) {
            error = "Tiled tileset has invalid tile dimensions";
            return false;
        }
        if (mapHas(root, "tilecount") &&
            (!jsonInt(root, "tilecount", tileset.tileCount) || tileset.tileCount <= 0)) {
            error = "Tiled tileset has invalid tilecount";
            return false;
        }
        if (mapHas(root, "columns") &&
            (!jsonInt(root, "columns", tileset.columns) || tileset.columns < 0)) {
            error = "Tiled tileset has invalid columns";
            return false;
        }
        if (mapHas(root, "margin") &&
            (!jsonInt(root, "margin", tileset.margin) || tileset.margin < 0)) {
            error = "Tiled tileset has invalid margin";
            return false;
        }
        if (mapHas(root, "spacing") &&
            (!jsonInt(root, "spacing", tileset.spacing) || tileset.spacing < 0)) {
            error = "Tiled tileset has invalid spacing";
            return false;
        }
        void *tileOffset = mapGet(root, "tileoffset");
        int64_t offsetX = 0;
        int64_t offsetY = 0;
        if (tileOffset &&
            (!jsonInt(tileOffset, "x", offsetX) || !jsonInt(tileOffset, "y", offsetY))) {
            error = "Tiled tileset tile offsets must be integral";
            return false;
        }
        tileset.tileOffsetX = offsetX;
        tileset.tileOffsetY = offsetY;
        std::string imageReference;
        if (jsonString(root, "image", imageReference)) {
            if (!reader_.resolveDependencyPath(owner, imageReference, tileset.image, error))
                return false;
            if ((mapHas(root, "imagewidth") && !jsonInt(root, "imagewidth", tileset.imageWidth)) ||
                (mapHas(root, "imageheight") &&
                 !jsonInt(root, "imageheight", tileset.imageHeight)) ||
                tileset.imageWidth < 0 || tileset.imageHeight < 0) {
                error = "Tiled tileset image dimensions are invalid";
                return false;
            }
            std::string transparent;
            if (mapHas(root, "transparentcolor") &&
                (!jsonString(root, "transparentcolor", transparent) ||
                 !parseTiledColor(transparent, tileset.transparentColor))) {
                error = "Tiled tileset transparentcolor is invalid";
                return false;
            }
            tileset.hasTransparentColor = !transparent.empty();
        }

        void *tiles = mapGet(root, "tiles");
        if (!tiles)
            return true;
        if (!isSeq(tiles)) {
            error = "Tiled tileset 'tiles' must be an array";
            return false;
        }
        int64_t count = rt_seq_len(tiles);
        for (int64_t index = 0; index < count; ++index) {
            void *tile = rt_seq_get(tiles, index);
            int64_t id = 0;
            if (!isMap(tile) || !jsonInt(tile, "id", id) || id < 0) {
                error = "Tiled tile metadata has an invalid id";
                return false;
            }
            if (tileset.metadata.find(id) != tileset.metadata.end()) {
                error = "Tiled tileset contains duplicate tile ids";
                return false;
            }
            std::string tileImageReference;
            if (jsonString(tile, "image", tileImageReference)) {
                TileImage image;
                if (!reader_.resolveDependencyPath(owner, tileImageReference, image.path, error) ||
                    (mapHas(tile, "imagewidth") && !jsonInt(tile, "imagewidth", image.width)) ||
                    (mapHas(tile, "imageheight") && !jsonInt(tile, "imageheight", image.height)) ||
                    image.width < 0 || image.height < 0)
                    return false;
                std::string transparent;
                if (mapHas(tile, "transparentcolor") &&
                    (!jsonString(tile, "transparentcolor", transparent) ||
                     !parseTiledColor(transparent, image.transparentColor))) {
                    error = "Tiled per-tile transparentcolor is invalid";
                    return false;
                }
                image.hasTransparentColor = !transparent.empty();
                tileset.tileImages[id] = std::move(image);
            }
            TileMetadata metadata;
            if (!parseJsonProperties(
                    mapGet(tile, "properties"), metadata.properties, error, &reader_, owner))
                return false;
            if (!jsonCollisionIsSolid(mapGet(tile, "objectgroup"),
                                      tileset.tileWidth,
                                      tileset.tileHeight,
                                      metadata.solid,
                                      error))
                return false;
            void *animation = mapGet(tile, "animation");
            if (animation) {
                if (!isSeq(animation) || rt_seq_len(animation) <= 0) {
                    error = "Tiled tile animation must contain frames";
                    return false;
                }
                int64_t frameCount = rt_seq_len(animation);
                for (int64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                    void *frame = rt_seq_get(animation, frameIndex);
                    int64_t frameTile = 0;
                    int64_t duration = 0;
                    if (!isMap(frame) || !jsonInt(frame, "tileid", frameTile) || frameTile < 0 ||
                        !jsonInt(frame, "duration", duration) || duration <= 0) {
                        error = "Tiled tile animation frame is invalid";
                        return false;
                    }
                    metadata.animationFrames.push_back(frameTile + 1);
                    metadata.animationDurations.push_back(duration);
                }
            }
            tileset.metadata[id] = std::move(metadata);
        }
        if (tileset.image.empty() && tileset.tileImages.empty()) {
            error = "Tiled tileset has neither a root image nor per-tile images";
            return false;
        }
        return true;
    }

    /// @brief Parse embedded or external TMX/TSX tileset content.
    /// @details Mirrors JSON tileset validation while reading XML attributes,
    ///          child images, properties, collision groups, and animations.
    /// @param owner Resolved containing document path.
    /// @param root Borrowed `<tileset>` element.
    /// @param tileset Destination tileset model.
    /// @param error Human-readable schema or dependency failure.
    /// @return `true` when all declared content is representable.
    bool parseXmlTileset(const std::string &owner,
                         void *root,
                         Tileset &tileset,
                         std::string &error) {
        tileset.name = xmlString(root, "name");
        if (!xmlInt(root, "tilewidth", tileset.tileWidth, true, error) ||
            !xmlInt(root, "tileheight", tileset.tileHeight, true, error) ||
            tileset.tileWidth <= 0 || tileset.tileHeight <= 0) {
            if (error.empty())
                error = "TMX tileset has invalid tile dimensions";
            return false;
        }
        if (!xmlInt(root, "tilecount", tileset.tileCount, false, error) ||
            !xmlInt(root, "columns", tileset.columns, false, error) ||
            !xmlInt(root, "margin", tileset.margin, false, error) ||
            !xmlInt(root, "spacing", tileset.spacing, false, error) || tileset.tileCount < 0 ||
            tileset.columns < 0 || tileset.margin < 0 || tileset.spacing < 0)
            return false;
        void *tileOffset = firstXmlChild(root, "tileoffset");
        if (tileOffset) {
            int64_t offsetX = 0;
            int64_t offsetY = 0;
            if (!xmlInt(tileOffset, "x", offsetX, false, error) ||
                !xmlInt(tileOffset, "y", offsetY, false, error)) {
                error = "TMX tileset tile offsets must be integral";
                return false;
            }
            tileset.tileOffsetX = offsetX;
            tileset.tileOffsetY = offsetY;
        }
        void *image = firstXmlChild(root, "image");
        std::string imageReference = image ? xmlString(image, "source") : std::string();
        if (image && !imageReference.empty()) {
            if (!reader_.resolveDependencyPath(owner, imageReference, tileset.image, error) ||
                !xmlInt(image, "width", tileset.imageWidth, false, error) ||
                !xmlInt(image, "height", tileset.imageHeight, false, error) ||
                tileset.imageWidth < 0 || tileset.imageHeight < 0)
                return false;
            std::string transparent = xmlString(image, "trans");
            if (!transparent.empty()) {
                if (transparent.front() != '#')
                    transparent.insert(transparent.begin(), '#');
                if (!parseTiledColor(transparent, tileset.transparentColor)) {
                    error = "TMX tileset transparent color is invalid";
                    return false;
                }
                tileset.hasTransparentColor = true;
            }
        }

        int64_t childCount = rt_xml_child_count(root);
        for (int64_t index = 0; index < childCount; ++index) {
            void *tile = rt_xml_child_at(root, index);
            if (rt_xml_node_type(tile) != XML_NODE_ELEMENT || xmlTag(tile) != "tile")
                continue;
            int64_t id = 0;
            if (!xmlInt(tile, "id", id, true, error) || id < 0) {
                error = "TMX tile metadata has an invalid id";
                return false;
            }
            if (tileset.metadata.find(id) != tileset.metadata.end()) {
                error = "TMX tileset contains duplicate tile ids";
                return false;
            }
            void *tileImage = firstXmlChild(tile, "image");
            if (tileImage) {
                std::string reference = xmlString(tileImage, "source");
                TileImage imageInfo;
                if (reference.empty() ||
                    !reader_.resolveDependencyPath(owner, reference, imageInfo.path, error) ||
                    !xmlInt(tileImage, "width", imageInfo.width, false, error) ||
                    !xmlInt(tileImage, "height", imageInfo.height, false, error) ||
                    imageInfo.width < 0 || imageInfo.height < 0)
                    return false;
                std::string transparent = xmlString(tileImage, "trans");
                if (!transparent.empty()) {
                    if (transparent.front() != '#')
                        transparent.insert(transparent.begin(), '#');
                    if (!parseTiledColor(transparent, imageInfo.transparentColor)) {
                        error = "TMX per-tile transparent color is invalid";
                        return false;
                    }
                    imageInfo.hasTransparentColor = true;
                }
                tileset.tileImages[id] = std::move(imageInfo);
            }
            TileMetadata metadata;
            if (!parseXmlProperties(tile, metadata.properties, error, &reader_, owner))
                return false;
            if (!xmlCollisionIsSolid(firstXmlChild(tile, "objectgroup"),
                                     tileset.tileWidth,
                                     tileset.tileHeight,
                                     metadata.solid,
                                     error))
                return false;
            void *animation = firstXmlChild(tile, "animation");
            if (animation) {
                int64_t animationChildCount = rt_xml_child_count(animation);
                for (int64_t frameIndex = 0; frameIndex < animationChildCount; ++frameIndex) {
                    void *frame = rt_xml_child_at(animation, frameIndex);
                    if (rt_xml_node_type(frame) != XML_NODE_ELEMENT || xmlTag(frame) != "frame")
                        continue;
                    int64_t tileId = 0;
                    int64_t duration = 0;
                    if (!xmlInt(frame, "tileid", tileId, true, error) || tileId < 0 ||
                        !xmlInt(frame, "duration", duration, true, error) || duration <= 0) {
                        error = "TMX tile animation frame is invalid";
                        return false;
                    }
                    metadata.animationFrames.push_back(tileId + 1);
                    metadata.animationDurations.push_back(duration);
                }
                if (metadata.animationFrames.empty()) {
                    error = "TMX tile animation must contain frames";
                    return false;
                }
            }
            tileset.metadata[id] = std::move(metadata);
        }
        if (tileset.image.empty() && tileset.tileImages.empty()) {
            error = "TMX tileset has neither a root image nor per-tile images";
            return false;
        }
        return true;
    }

    /// @brief Rewrite raw Tiled GIDs to deduplicated canonical tile IDs.
    /// @details Separates transform bits, locates the owning tileset, validates
    ///          local range, incorporates inherited tint/opacity into the
    ///          canonical key, creates bounded variants as needed, and marks
    ///          referenced variants reachable.
    /// @param gids Raw uint32-valued GIDs replaced in place.
    /// @param document Map model containing tilesets and canonical tables.
    /// @param tint Inherited combined ARGB layer tint.
    /// @param opacity Inherited combined opacity in 0..65535.
    /// @param error Human-readable range or capacity failure.
    /// @return `true` when every nonempty GID maps unambiguously.
    bool normalizeGids(std::vector<int64_t> &gids,
                       MapDocument &document,
                       uint32_t tint,
                       uint16_t opacity,
                       std::string &error) {
        for (int64_t &raw : gids) {
            if (raw < 0 || static_cast<uint64_t>(raw) > UINT32_MAX) {
                error = "Tiled GID is outside the uint32 range";
                return false;
            }
            uint64_t gid = static_cast<uint64_t>(raw);
            uint32_t flags = static_cast<uint32_t>(gid & kTiledTransformMask);
            uint64_t baseGid = gid & ~kTiledTransformMask;
            if (baseGid == 0u) {
                if (flags != 0u) {
                    error = "Tiled empty GID cannot carry transform flags";
                    return false;
                }
                continue;
            }
            if (document.orientation != "hexagonal")
                flags &= ~static_cast<uint32_t>(kTiledHex120Flag);
            int64_t selected = -1;
            for (size_t index = 0; index < document.tilesets.size(); ++index) {
                if (static_cast<uint64_t>(document.tilesets[index].firstGid) <= baseGid)
                    selected = static_cast<int64_t>(index);
                else
                    break;
            }
            if (selected < 0) {
                error = "Tiled GID does not belong to a declared tileset";
                return false;
            }
            Tileset &tileset = document.tilesets[static_cast<size_t>(selected)];
            int64_t local = static_cast<int64_t>(baseGid) - tileset.firstGid;
            if (local < 0 || local >= tileset.tileCount) {
                error = "Tiled GID exceeds its tileset tilecount";
                return false;
            }
            CanonicalTileKey key;
            key.tileset = selected;
            key.localId = local;
            key.transformFlags = flags;
            key.tint = tint;
            key.opacity = opacity;
            auto found = document.canonicalVariants.find(key);
            int64_t canonical = 0;
            if (found == document.canonicalVariants.end()) {
                if (document.canonicalTiles.size() >= kMaxCellsPerLayer) {
                    error = "Tiled canonical tile variant count exceeds 1048575";
                    return false;
                }
                CanonicalTile tile;
                tile.tileset = selected;
                tile.localId = local;
                tile.transformFlags = flags;
                tile.tint = tint;
                tile.opacity = opacity;
                document.canonicalTiles.push_back(tile);
                canonical = static_cast<int64_t>(document.canonicalTiles.size() - 1u);
                document.canonicalVariants.emplace(key, canonical);
            } else {
                canonical = found->second;
            }
            document.canonicalTiles[static_cast<size_t>(canonical)].reachable = true;
            raw = canonical;
        }
        return true;
    }

    /// @brief Validate, normalize, and append one full-size scene tile layer.
    /// @param name Authored name; empty names receive a deterministic fallback.
    /// @param visible Effective inherited visibility.
    /// @param gids Full-map raw GID cells moved into the layer.
    /// @param offsetX Effective pixel x offset.
    /// @param offsetY Effective pixel y offset.
    /// @param opacity Effective opacity in 0..1.
    /// @param parallaxX Effective horizontal parallax factor.
    /// @param parallaxY Effective vertical parallax factor.
    /// @param tint Effective ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable layer/cell/canonicalization failure.
    /// @return `true` when appended within layer and total-cell limits.
    bool addLayer(std::string name,
                  bool visible,
                  std::vector<int64_t> gids,
                  double offsetX,
                  double offsetY,
                  double opacity,
                  double parallaxX,
                  double parallaxY,
                  uint32_t tint,
                  MapDocument &document,
                  std::string &error) {
        if (document.layers.size() >= kMaxLayers) {
            error = "Tiled tile layer count exceeds Tilemap's 16-layer limit";
            return false;
        }
        if (gids.size() != static_cast<size_t>(document.width * document.height)) {
            error = "Tiled layer cell count does not match map dimensions";
            return false;
        }
        if (document.totalCells > kMaxTotalCells - gids.size()) {
            error = "Tiled map exceeds the 4194304 total-cell limit";
            return false;
        }
        Layer layer;
        layer.name =
            name.empty() ? "Layer" + std::to_string(document.layers.size()) : std::move(name);
        layer.visible = visible;
        layer.offsetX = offsetX;
        layer.offsetY = offsetY;
        layer.opacity = opacity;
        layer.parallaxX = parallaxX;
        layer.parallaxY = parallaxY;
        layer.tint = tint;
        double scaledOpacity = std::floor(opacity * static_cast<double>(UINT16_MAX) + 0.5);
        uint16_t canonicalOpacity =
            static_cast<uint16_t>(std::clamp(scaledOpacity, 0.0, static_cast<double>(UINT16_MAX)));
        if (!normalizeGids(gids, document, tint, canonicalOpacity, error))
            return false;
        layer.tiles = std::move(gids);
        document.totalCells += layer.tiles.size();
        document.layers.push_back(std::move(layer));
        return true;
    }

    /// @brief Place a finite layer rectangle into full-map cell storage.
    /// @details Nonempty source cells outside the finite map are rejected;
    ///          out-of-bounds empty padding is tolerated.
    /// @param source Source GIDs in row-major order.
    /// @param sourceWidth Source width.
    /// @param sourceHeight Source height.
    /// @param offsetX Destination tile x offset.
    /// @param offsetY Destination tile y offset.
    /// @param document Destination map geometry.
    /// @param placed Full-map row-major cells replaced on success.
    /// @param error Human-readable dimension or placement failure.
    /// @return `true` when all nonempty cells fit.
    bool placeLayerGids(const std::vector<int64_t> &source,
                        int64_t sourceWidth,
                        int64_t sourceHeight,
                        int64_t offsetX,
                        int64_t offsetY,
                        const MapDocument &document,
                        std::vector<int64_t> &placed,
                        std::string &error) {
        if (sourceWidth <= 0 || sourceHeight <= 0 ||
            sourceWidth > static_cast<int64_t>(kMaxCellsPerLayer) / sourceHeight ||
            source.size() != static_cast<size_t>(sourceWidth * sourceHeight)) {
            error = "Tiled tile layer dimensions or cell count are invalid";
            return false;
        }
        placed.assign(static_cast<size_t>(document.width * document.height), 0);
        for (int64_t y = 0; y < sourceHeight; ++y) {
            for (int64_t x = 0; x < sourceWidth; ++x) {
                int64_t gid = source[static_cast<size_t>(y * sourceWidth + x)];
                int64_t destinationX = x + offsetX;
                int64_t destinationY = y + offsetY;
                if (destinationX < 0 || destinationY < 0 || destinationX >= document.width ||
                    destinationY >= document.height) {
                    if (gid != 0) {
                        error = "offset Tiled layer places a nonempty tile outside the finite map";
                        return false;
                    }
                    continue;
                }
                placed[static_cast<size_t>(destinationY * document.width + destinationX)] = gid;
            }
        }
        return true;
    }

    /// @brief Compose JSON layer rendering attributes with inherited values.
    /// @details Multiplies opacity/parallax/tint and accepts only the normal
    ///          blend mode.
    /// @param layer Borrowed layer Map.
    /// @param inheritedOpacity Parent opacity.
    /// @param inheritedParallaxX Parent horizontal parallax.
    /// @param inheritedParallaxY Parent vertical parallax.
    /// @param inheritedTint Parent ARGB tint.
    /// @param opacity Effective opacity written on success.
    /// @param parallaxX Effective horizontal parallax written on success.
    /// @param parallaxY Effective vertical parallax written on success.
    /// @param tint Effective tint written on success.
    /// @param error Human-readable type/range/overflow failure.
    /// @return `true` when all effective values are finite and supported.
    bool composeLayerRenderingJson(void *layer,
                                   double inheritedOpacity,
                                   double inheritedParallaxX,
                                   double inheritedParallaxY,
                                   uint32_t inheritedTint,
                                   double &opacity,
                                   double &parallaxX,
                                   double &parallaxY,
                                   uint32_t &tint,
                                   std::string &error) {
        double ownOpacity = 1.0;
        double ownParallaxX = 1.0;
        double ownParallaxY = 1.0;
        if ((mapHas(layer, "opacity") && !jsonNumberValue(mapGet(layer, "opacity"), ownOpacity)) ||
            (mapHas(layer, "parallaxx") &&
             !jsonNumberValue(mapGet(layer, "parallaxx"), ownParallaxX)) ||
            (mapHas(layer, "parallaxy") &&
             !jsonNumberValue(mapGet(layer, "parallaxy"), ownParallaxY)) ||
            ownOpacity < 0.0 || ownOpacity > 1.0) {
            error = "Tiled layer opacity/parallax values are invalid";
            return false;
        }
        uint32_t ownTint = UINT32_C(0xFFFFFFFF);
        std::string tintText;
        if (mapHas(layer, "tintcolor") &&
            (!jsonString(layer, "tintcolor", tintText) || !parseTiledColor(tintText, ownTint))) {
            error = "Tiled layer tintcolor must be #RRGGBB or #AARRGGBB";
            return false;
        }
        std::string blendMode;
        if (mapHas(layer, "blendmode") &&
            (!jsonString(layer, "blendmode", blendMode) || blendMode != "normal")) {
            error = "unsupported Tiled layer blend mode";
            return false;
        }
        opacity = inheritedOpacity * ownOpacity;
        parallaxX = inheritedParallaxX * ownParallaxX;
        parallaxY = inheritedParallaxY * ownParallaxY;
        tint = multiplyTiledColors(inheritedTint, ownTint);
        if (!std::isfinite(opacity) || !std::isfinite(parallaxX) || !std::isfinite(parallaxY)) {
            error = "composed Tiled layer rendering values overflow";
            return false;
        }
        return true;
    }

    /// @brief Decode one JSON tile-layer or chunk data payload.
    /// @details Supports direct integer arrays, CSV strings, and optionally
    ///          compressed Base64 strings while enforcing the expected count.
    /// @param payload Borrowed layer/chunk Map containing `data`.
    /// @param encodingOwner Borrowed layer Map declaring encoding/compression.
    /// @param cells Required GID count.
    /// @param gids Decoded GIDs replaced on success.
    /// @param error Human-readable encoding/schema/count failure.
    /// @return `true` when exactly @p cells values are decoded.
    bool parseJsonGidPayload(void *payload,
                             void *encodingOwner,
                             size_t cells,
                             std::vector<int64_t> &gids,
                             std::string &error) {
        void *data = mapGet(payload, "data");
        if (isSeq(data)) {
            if (mapHas(encodingOwner, "encoding") || mapHas(encodingOwner, "compression")) {
                error = "array-backed Tiled layer data cannot declare encoding/compression";
                return false;
            }
            int64_t count = rt_seq_len(data);
            if (count < 0 || static_cast<size_t>(count) != cells) {
                error = "Tiled layer cell count does not match its dimensions";
                return false;
            }
            gids.resize(cells);
            for (int64_t index = 0; index < count; ++index) {
                if (!jsonIntValue(rt_seq_get(data, index), gids[static_cast<size_t>(index)])) {
                    error = "Tiled layer data contains a non-integer GID";
                    return false;
                }
            }
            return true;
        }
        std::string encoded;
        std::string encoding;
        std::string compression;
        if (!jsonStringValue(data, encoded) || !jsonString(encodingOwner, "encoding", encoding)) {
            error = "Tiled layer data must be an array, CSV string, or Base64 string";
            return false;
        }
        jsonString(encodingOwner, "compression", compression);
        if (encoding == "csv") {
            if (!compression.empty()) {
                error = "Tiled CSV layer data cannot be compressed";
                return false;
            }
            return parseCsv(encoded, cells, gids, error);
        }
        if (encoding == "base64")
            return decodeBase64Gids(encoded, compression, cells, gids, error);
        error = "unsupported Tiled layer encoding '" + encoding + "'";
        return false;
    }

    /// @brief Parse one finite or chunked JSON tile layer.
    /// @details Composes offsets, decodes cells, detects overlapping infinite
    ///          chunks, places all data into scanned map bounds, and appends a
    ///          canonicalized scene layer.
    /// @param layer Borrowed tile-layer Map.
    /// @param name Effective hierarchical layer name.
    /// @param visible Effective inherited visibility.
    /// @param inheritedX Parent pixel x offset.
    /// @param inheritedY Parent pixel y offset.
    /// @param opacity Effective opacity.
    /// @param parallaxX Effective horizontal parallax.
    /// @param parallaxY Effective vertical parallax.
    /// @param tint Effective ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable decoding or placement failure.
    /// @return `true` when the layer is appended.
    bool parseJsonTileLayer(void *layer,
                            const std::string &name,
                            bool visible,
                            double inheritedX,
                            double inheritedY,
                            double opacity,
                            double parallaxX,
                            double parallaxY,
                            uint32_t tint,
                            MapDocument &document,
                            std::string &error) {
        int64_t width = 0;
        int64_t height = 0;
        int64_t layerX = 0;
        int64_t layerY = 0;
        double offsetX = inheritedX;
        double offsetY = inheritedY;
        if (mapHas(layer, "x") && !jsonInt(layer, "x", layerX)) {
            error = "Tiled tile layer x offset must be integral";
            return false;
        }
        if (mapHas(layer, "y") && !jsonInt(layer, "y", layerY)) {
            error = "Tiled tile layer y offset must be integral";
            return false;
        }
        double ownOffset = 0.0;
        if (mapHas(layer, "offsetx")) {
            if (!jsonNumberValue(mapGet(layer, "offsetx"), ownOffset)) {
                error = "Tiled tile layer offsetx must be numeric";
                return false;
            }
            offsetX += ownOffset;
        }
        ownOffset = 0.0;
        if (mapHas(layer, "offsety")) {
            if (!jsonNumberValue(mapGet(layer, "offsety"), ownOffset)) {
                error = "Tiled tile layer offsety must be numeric";
                return false;
            }
            offsetY += ownOffset;
        }
        if (!std::isfinite(offsetX) || !std::isfinite(offsetY)) {
            error = "composed Tiled tile-layer pixel offset overflows";
            return false;
        }
        std::vector<int64_t> placed(static_cast<size_t>(document.width * document.height), 0);
        if (document.infinite) {
            void *chunks = mapGet(layer, "chunks");
            if (!isSeq(chunks)) {
                error = "infinite Tiled tile layer must contain a chunks array";
                return false;
            }
            std::vector<uint8_t> occupied(placed.size(), 0u);
            int64_t count = rt_seq_len(chunks);
            for (int64_t chunkIndex = 0; chunkIndex < count; ++chunkIndex) {
                void *chunk = rt_seq_get(chunks, chunkIndex);
                int64_t chunkX = 0;
                int64_t chunkY = 0;
                if (!isMap(chunk) || !jsonInt(chunk, "x", chunkX) || !jsonInt(chunk, "y", chunkY) ||
                    !jsonInt(chunk, "width", width) || !jsonInt(chunk, "height", height) ||
                    width <= 0 || height <= 0 ||
                    width > static_cast<int64_t>(kMaxCellsPerLayer) / height) {
                    error = "Tiled infinite chunk dimensions are invalid";
                    return false;
                }
                size_t cells = static_cast<size_t>(width * height);
                std::vector<int64_t> gids;
                if (!parseJsonGidPayload(chunk, layer, cells, gids, error))
                    return false;
                if ((layerX > 0 && chunkX > INT64_MAX - layerX) ||
                    (layerX < 0 && chunkX < INT64_MIN - layerX) ||
                    (layerY > 0 && chunkY > INT64_MAX - layerY) ||
                    (layerY < 0 && chunkY < INT64_MIN - layerY)) {
                    error = "Tiled infinite chunk placement overflows";
                    return false;
                }
                int64_t destinationX = chunkX + layerX - document.originTileX;
                int64_t destinationY = chunkY + layerY - document.originTileY;
                if (destinationX < 0 || destinationY < 0 || destinationX > document.width - width ||
                    destinationY > document.height - height) {
                    error = "Tiled infinite chunk lies outside scanned map bounds";
                    return false;
                }
                for (int64_t y = 0; y < height; ++y) {
                    for (int64_t x = 0; x < width; ++x) {
                        size_t destination = static_cast<size_t>(
                            (destinationY + y) * document.width + destinationX + x);
                        if (occupied[destination] != 0u) {
                            error = "Tiled infinite chunks overlap within a layer";
                            return false;
                        }
                        occupied[destination] = 1u;
                        placed[destination] = gids[static_cast<size_t>(y * width + x)];
                    }
                }
            }
        } else {
            if (!jsonInt(layer, "width", width) || !jsonInt(layer, "height", height) ||
                width <= 0 || height <= 0 ||
                width > static_cast<int64_t>(kMaxCellsPerLayer) / height) {
                error = "finite Tiled tile layer dimensions are invalid";
                return false;
            }
            std::vector<int64_t> gids;
            if (!parseJsonGidPayload(
                    layer, layer, static_cast<size_t>(width * height), gids, error) ||
                !placeLayerGids(gids, width, height, layerX, layerY, document, placed, error))
                return false;
        }
        return addLayer(name,
                        visible,
                        std::move(placed),
                        offsetX,
                        offsetY,
                        opacity,
                        parallaxX,
                        parallaxY,
                        tint,
                        document,
                        error);
    }

    /// @brief Insert a generated property only when its reserved key is free.
    /// @param properties Target property map.
    /// @param name Reserved import key.
    /// @param value Generated scalar value.
    /// @param error Human-readable collision or capacity failure.
    /// @return `true` when stored.
    bool addReservedProperty(std::map<std::string, Scalar> &properties,
                             const std::string &name,
                             Scalar value,
                             std::string &error) {
        if (properties.find(name) != properties.end()) {
            error = "Tiled property collides with reserved import key '" + name + "'";
            return false;
        }
        return setProperty(properties, name, std::move(value), error);
    }

    /// @brief Preserve JSON layer properties as namespaced scene properties.
    /// @param owner Resolved containing document for file properties.
    /// @param layer Borrowed layer Map.
    /// @param layerName Effective hierarchical name.
    /// @param document Scene property target.
    /// @param error Human-readable parse, collision, or limit failure.
    /// @return `true` when every property is preserved.
    bool preserveLayerPropertiesJson(const std::string &owner,
                                     void *layer,
                                     const std::string &layerName,
                                     MapDocument &document,
                                     std::string &error) {
        std::map<std::string, Scalar> properties;
        if (!parseJsonProperties(mapGet(layer, "properties"), properties, error, &reader_, owner))
            return false;
        for (auto &[name, value] : properties) {
            std::string key = "tiled.layer." + layerName + "." + name;
            if (document.properties.find(key) != document.properties.end()) {
                error = "Tiled layer property collides with scene property '" + key + "'";
                return false;
            }
            if (!setProperty(document.properties, key, std::move(value), error))
                return false;
        }
        return true;
    }

    /// @brief Preserve TMX layer properties as namespaced scene properties.
    /// @param owner Resolved containing document for file properties.
    /// @param layer Borrowed layer element.
    /// @param layerName Effective hierarchical name.
    /// @param document Scene property target.
    /// @param error Human-readable parse, collision, or limit failure.
    /// @return `true` when every property is preserved.
    bool preserveLayerPropertiesXml(const std::string &owner,
                                    void *layer,
                                    const std::string &layerName,
                                    MapDocument &document,
                                    std::string &error) {
        std::map<std::string, Scalar> properties;
        if (!parseXmlProperties(layer, properties, error, &reader_, owner))
            return false;
        for (auto &[name, value] : properties) {
            std::string key = "tiled.layer." + layerName + "." + name;
            if (document.properties.find(key) != document.properties.end()) {
                error = "TMX layer property collides with scene property '" + key + "'";
                return false;
            }
            if (!setProperty(document.properties, key, std::move(value), error))
                return false;
        }
        return true;
    }

    /// @brief Construct an integer scalar.
    /// @param value Integer payload.
    /// @return Tagged scalar.
    Scalar integerScalar(int64_t value) const {
        Scalar scalar;
        scalar.kind = ScalarKind::Integer;
        scalar.integer = value;
        return scalar;
    }

    /// @brief Construct a floating-point scalar.
    /// @param value Finite numeric payload.
    /// @return Tagged scalar.
    Scalar floatScalar(double value) const {
        Scalar scalar;
        scalar.kind = ScalarKind::Float;
        scalar.floating = value;
        return scalar;
    }

    /// @brief Construct a Boolean scalar.
    /// @param value Boolean payload.
    /// @return Tagged scalar.
    Scalar boolScalar(bool value) const {
        Scalar scalar;
        scalar.kind = ScalarKind::Boolean;
        scalar.boolean = value;
        return scalar;
    }

    /// @brief Construct a string scalar by moving its bytes.
    /// @param value String payload.
    /// @return Tagged scalar.
    Scalar stringScalar(std::string value) const {
        Scalar scalar;
        scalar.kind = ScalarKind::String;
        scalar.string = std::move(value);
        return scalar;
    }

    /// @brief Choose and reserve a unique SceneDocument object ID.
    /// @details Prefers a unique authored name, otherwise falls back to the
    ///          numeric Tiled object ID.
    /// @param authoredName Authored object name.
    /// @param numericId Parsed Tiled numeric ID.
    /// @param result Chosen ID written on success.
    /// @param error Human-readable uniqueness failure.
    /// @return `true` when a nonempty unique ID is reserved.
    bool chooseObjectId(const std::string &authoredName,
                        int64_t numericId,
                        std::string &result,
                        std::string &error) {
        result = authoredName;
        if (result.empty() || objectIds_.find(result) != objectIds_.end())
            result = std::to_string(numericId);
        if (result.empty() || !objectIds_.insert(result).second) {
            error = "Tiled object names and numeric IDs do not provide a unique SceneDocument ID";
            return false;
        }
        return true;
    }

    /// @brief Preserve a runtime JSON value as bounded canonical JSON text.
    /// @param value Borrowed parsed value.
    /// @param properties Target object property map.
    /// @param name Reserved property key.
    /// @param error Human-readable formatting, size, or collision failure.
    /// @return `true` when stored as a string scalar.
    bool canonicalJsonProperty(void *value,
                               std::map<std::string, Scalar> &properties,
                               const std::string &name,
                               std::string &error) {
        rt_string formatted = rt_json_format(value);
        if (!formatted) {
            error = "cannot preserve Tiled object shape data";
            return false;
        }
        std::string text = toStd(formatted);
        rt_string_unref(formatted);
        if (text.size() > kMaxStringBytes) {
            error = "Tiled object shape data exceeds 64 KiB";
            return false;
        }
        return addReservedProperty(properties, name, stringScalar(std::move(text)), error);
    }

    /// @brief Merge and parse a JSON object-template instance.
    /// @details Reads an acyclic JSON template, clones its base object, appends
    ///          template then instance properties, overlays instance members,
    ///          records the resolved template path, and parses the merged object.
    /// @param owner Resolved instance document path.
    /// @param instance Borrowed instance object Map.
    /// @param templateReference Relative template path.
    /// @param layerName Effective containing layer name.
    /// @param layerVisible Effective containing visibility.
    /// @param offsetX Effective containing x offset.
    /// @param offsetY Effective containing y offset.
    /// @param document Map model to update.
    /// @param error Human-readable dependency, merge, or object failure.
    /// @return `true` when the merged object is appended.
    bool parseJsonTemplateObject(const std::string &owner,
                                 void *instance,
                                 const std::string &templateReference,
                                 const std::string &layerName,
                                 bool layerVisible,
                                 double offsetX,
                                 double offsetY,
                                 MapDocument &document,
                                 std::string &error) {
        std::string resolved;
        std::string text;
        if (!reader_.readDependency(owner, templateReference, resolved, text, error))
            return false;
        size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || text[first] == '<') {
            error = "a JSON Tiled object requires a JSON object template";
            return false;
        }
        if (!enter(resolved, error))
            return false;
        rt_string source = makeString(text);
        void *rawRoot = nullptr;
        rt_string message = nullptr;
        bool parsed = rt_json_try_parse(source, &rawRoot, &message, nullptr, nullptr) != 0;
        rt_string_unref(source);
        OwnedObject root(rawRoot);
        if (!parsed || !isMap(root.get())) {
            error = "invalid Tiled JSON object template: " + resolved;
            if (message) {
                error += ": " + toStd(message);
                rt_string_unref(message);
            }
            leave(resolved);
            return false;
        }
        std::string type;
        void *templateObject = mapGet(root.get(), "object");
        if (!jsonString(root.get(), "type", type) || type != "template" || !isMap(templateObject)) {
            error = "Tiled JSON template root must identify type 'template' and contain object";
            leave(resolved);
            return false;
        }
        OwnedObject merged(rt_map_clone(templateObject));
        if (!merged.get()) {
            error = "cannot allocate merged Tiled object template";
            leave(resolved);
            return false;
        }
        OwnedObject mergedProperties(rt_seq_new_owned());
        /// @brief Append a parsed property sequence to the merged template properties.
        /// @param properties Borrowed candidate property Seq; non-Seqs are ignored.
        auto appendProperties = [&](void *properties) {
            if (!isSeq(properties))
                return;
            int64_t count = rt_seq_len(properties);
            for (int64_t index = 0; index < count; ++index)
                rt_seq_push(mergedProperties.get(), rt_seq_get(properties, index));
        };
        appendProperties(mapGet(templateObject, "properties"));
        appendProperties(mapGet(instance, "properties"));
        rt_string propertiesKey = rt_const_cstr("properties");
        rt_map_set(merged.get(), propertiesKey, mergedProperties.get());
        rt_string_unref(propertiesKey);

        OwnedObject keys(rt_map_keys(instance));
        int64_t keyCount = rt_seq_len(keys.get());
        for (int64_t index = 0; index < keyCount; ++index) {
            rt_string key = rt_seq_get_str(keys.get(), index);
            std::string name = toStd(key);
            if (name != "template" && name != "properties")
                rt_map_set(merged.get(), key, rt_map_get(instance, key));
            rt_string_unref(key);
        }
        rt_string markerKey = rt_const_cstr("__zannaTemplatePath");
        rt_string markerValue = makeString(resolved);
        rt_map_set_str(merged.get(), markerKey, markerValue);
        rt_string_unref(markerKey);
        rt_string_unref(markerValue);
        bool ok = parseJsonObject(
            resolved, merged.get(), layerName, layerVisible, offsetX, offsetY, document, error);
        leave(resolved);
        return ok;
    }

    /// @brief Convert one JSON Tiled object into a SceneDocument object.
    /// @details Resolves templates, composes/rounds coordinates, chooses a
    ///          unique ID, preserves authored and generated metadata, normalizes
    ///          tile-object GIDs, and serializes supported shape payloads.
    /// @param owner Resolved containing document path.
    /// @param object Borrowed object Map.
    /// @param layerName Effective containing layer name.
    /// @param layerVisible Effective containing visibility.
    /// @param offsetX Effective containing x offset.
    /// @param offsetY Effective containing y offset.
    /// @param document Map model to update.
    /// @param error Human-readable object/template/property failure.
    /// @return `true` when exactly one object is appended.
    bool parseJsonObject(const std::string &owner,
                         void *object,
                         const std::string &layerName,
                         bool layerVisible,
                         double offsetX,
                         double offsetY,
                         MapDocument &document,
                         std::string &error) {
        if (!isMap(object)) {
            error = "Tiled object layer contains a non-object entry";
            return false;
        }
        std::string templateReference;
        if (jsonString(object, "template", templateReference))
            return parseJsonTemplateObject(owner,
                                           object,
                                           templateReference,
                                           layerName,
                                           layerVisible,
                                           offsetX,
                                           offsetY,
                                           document,
                                           error);
        if (document.objects.size() >= kMaxObjects) {
            error = "Tiled object count exceeds 65536";
            return false;
        }
        double x = 0.0;
        double y = 0.0;
        if ((mapHas(object, "x") && !jsonNumberValue(mapGet(object, "x"), x)) ||
            (mapHas(object, "y") && !jsonNumberValue(mapGet(object, "y"), y))) {
            error = "Tiled object coordinates must be numeric";
            return false;
        }
        x += offsetX;
        y += offsetY;
        SceneObject result;
        if (!roundNearestTiesAway(x, result.x) || !roundNearestTiesAway(y, result.y)) {
            error = "Tiled object coordinates are outside the SceneDocument integer range";
            return false;
        }
        if (!jsonString(object, "class", result.type) || result.type.empty())
            jsonString(object, "type", result.type);
        if (result.type.empty())
            result.type = "TiledObject";
        std::string authoredName;
        jsonString(object, "name", authoredName);
        int64_t numericId = 0;
        if (mapHas(object, "id") && (!jsonInt(object, "id", numericId) || numericId < 0)) {
            error = "Tiled object id must be a nonnegative integer";
            return false;
        }
        if (!chooseObjectId(authoredName, numericId, result.id, error))
            return false;
        if (!parseJsonProperties(
                mapGet(object, "properties"), result.properties, error, &reader_, owner) ||
            !addReservedProperty(
                result.properties, "tiled.layer", stringScalar(layerName), error) ||
            !addReservedProperty(
                result.properties, "tiled.numericId", integerScalar(numericId), error) ||
            !addReservedProperty(
                result.properties, "tiled.layerVisible", boolScalar(layerVisible), error))
            return false;
        if (!addReservedProperty(result.properties, "tiled.sourceX", floatScalar(x), error) ||
            !addReservedProperty(result.properties, "tiled.sourceY", floatScalar(y), error))
            return false;
        std::string templatePath;
        if (jsonString(object, "__zannaTemplatePath", templatePath) &&
            !addReservedProperty(
                result.properties, "tiled.template", stringScalar(templatePath), error))
            return false;

        double width = 0.0;
        double height = 0.0;
        double rotation = 0.0;
        if ((mapHas(object, "width") && !jsonNumberValue(mapGet(object, "width"), width)) ||
            (mapHas(object, "height") && !jsonNumberValue(mapGet(object, "height"), height)) ||
            (mapHas(object, "rotation") &&
             !jsonNumberValue(mapGet(object, "rotation"), rotation))) {
            error = "Tiled object geometry must be numeric";
            return false;
        }
        if (!addReservedProperty(result.properties, "tiled.width", floatScalar(width), error) ||
            !addReservedProperty(result.properties, "tiled.height", floatScalar(height), error) ||
            !addReservedProperty(result.properties, "tiled.rotation", floatScalar(rotation), error))
            return false;
        bool objectVisible = true;
        if (mapHas(object, "visible") && !jsonBool(object, "visible", objectVisible)) {
            error = "Tiled object visibility must be boolean";
            return false;
        }
        if (!addReservedProperty(
                result.properties, "tiled.visible", boolScalar(objectVisible), error))
            return false;

        int64_t gid = 0;
        if (mapHas(object, "gid")) {
            if (!jsonInt(object, "gid", gid) || gid < 0 ||
                static_cast<uint64_t>(gid) > UINT32_MAX) {
                error = "Tiled tile object GID is invalid";
                return false;
            }
            int64_t rawGid = gid;
            std::vector<int64_t> normalized{gid};
            if (!normalizeGids(normalized, document, UINT32_C(0xFFFFFFFF), UINT16_MAX, error))
                return false;
            uint32_t flags =
                static_cast<uint32_t>(rawGid) & static_cast<uint32_t>(kTiledTransformMask);
            if (document.orientation != "hexagonal")
                flags &= ~static_cast<uint32_t>(kTiledHex120Flag);
            if (!addReservedProperty(
                    result.properties, "tiled.gid", integerScalar(normalized.front()), error) ||
                !addReservedProperty(
                    result.properties, "tiled.rawGid", integerScalar(rawGid), error) ||
                !addReservedProperty(result.properties,
                                     "tiled.gidFlags",
                                     integerScalar(static_cast<int64_t>(flags)),
                                     error))
                return false;
        }
        const char *shapeKeys[] = {"ellipse", "point", "polygon", "polyline", "text"};
        for (const char *key : shapeKeys) {
            if (mapHas(object, key) &&
                !canonicalJsonProperty(
                    mapGet(object, key), result.properties, std::string("tiled.") + key, error))
                return false;
        }
        document.objects.push_back(std::move(result));
        (void)owner;
        return true;
    }

    /// @brief Parse a JSON object layer and attach effective render metadata.
    /// @param owner Resolved containing document path.
    /// @param layer Borrowed object-layer Map.
    /// @param name Effective hierarchical layer name.
    /// @param visible Effective visibility.
    /// @param inheritedX Parent pixel x offset.
    /// @param inheritedY Parent pixel y offset.
    /// @param opacity Effective opacity.
    /// @param parallaxX Effective horizontal parallax.
    /// @param parallaxY Effective vertical parallax.
    /// @param tint Effective ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable schema/object/property failure.
    /// @return `true` when every source object produces one result.
    bool parseJsonObjectLayer(const std::string &owner,
                              void *layer,
                              const std::string &name,
                              bool visible,
                              double inheritedX,
                              double inheritedY,
                              double opacity,
                              double parallaxX,
                              double parallaxY,
                              uint32_t tint,
                              MapDocument &document,
                              std::string &error) {
        double offsetX = inheritedX;
        double offsetY = inheritedY;
        double value = 0.0;
        if (mapHas(layer, "offsetx")) {
            if (!jsonNumberValue(mapGet(layer, "offsetx"), value)) {
                error = "Tiled object layer offsetx must be numeric";
                return false;
            }
            offsetX += value;
        }
        value = 0.0;
        if (mapHas(layer, "offsety")) {
            if (!jsonNumberValue(mapGet(layer, "offsety"), value)) {
                error = "Tiled object layer offsety must be numeric";
                return false;
            }
            offsetY += value;
        }
        void *objects = mapGet(layer, "objects");
        if (!isSeq(objects)) {
            error = "Tiled object layer is missing an objects array";
            return false;
        }
        int64_t count = rt_seq_len(objects);
        for (int64_t index = 0; index < count; ++index) {
            size_t before = document.objects.size();
            if (!parseJsonObject(owner,
                                 rt_seq_get(objects, index),
                                 name,
                                 visible,
                                 offsetX,
                                 offsetY,
                                 document,
                                 error))
                return false;
            if (document.objects.size() != before + 1u) {
                error = "Tiled object import did not produce exactly one object";
                return false;
            }
            auto &properties = document.objects.back().properties;
            if (!addReservedProperty(properties, "tiled.opacity", floatScalar(opacity), error) ||
                !addReservedProperty(
                    properties, "tiled.parallaxX", floatScalar(parallaxX), error) ||
                !addReservedProperty(
                    properties, "tiled.parallaxY", floatScalar(parallaxY), error) ||
                !addReservedProperty(
                    properties, "tiled.tint", integerScalar(static_cast<int64_t>(tint)), error))
                return false;
        }
        return true;
    }

    /// @brief Convert a JSON image layer into a metadata SceneDocument object.
    /// @details Resolves artwork, composes/rounds offsets, preserves layer
    ///          properties and repeat/render attributes, and appends an object
    ///          of type `tiled.image-layer`.
    /// @param owner Resolved containing document path.
    /// @param layer Borrowed image-layer Map.
    /// @param name Effective hierarchical layer name and object ID.
    /// @param visible Effective visibility.
    /// @param inheritedX Parent pixel x offset.
    /// @param inheritedY Parent pixel y offset.
    /// @param opacity Effective opacity.
    /// @param parallaxX Effective horizontal parallax.
    /// @param parallaxY Effective vertical parallax.
    /// @param tint Effective ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable dependency or metadata failure.
    /// @return `true` when the metadata object is appended.
    bool parseJsonImageLayer(const std::string &owner,
                             void *layer,
                             const std::string &name,
                             bool visible,
                             double inheritedX,
                             double inheritedY,
                             double opacity,
                             double parallaxX,
                             double parallaxY,
                             uint32_t tint,
                             MapDocument &document,
                             std::string &error) {
        std::string reference;
        if (!jsonString(layer, "image", reference)) {
            error = "Tiled image layer is missing an image";
            return false;
        }
        std::string resolved;
        if (!reader_.resolveDependencyPath(owner, reference, resolved, error))
            return false;
        double x = inheritedX;
        double y = inheritedY;
        double value = 0.0;
        if (mapHas(layer, "offsetx")) {
            if (!jsonNumberValue(mapGet(layer, "offsetx"), value)) {
                error = "Tiled image layer offsetx must be numeric";
                return false;
            }
            x += value;
        }
        value = 0.0;
        if (mapHas(layer, "offsety")) {
            if (!jsonNumberValue(mapGet(layer, "offsety"), value)) {
                error = "Tiled image layer offsety must be numeric";
                return false;
            }
            y += value;
        }
        SceneObject object;
        object.type = "tiled.image-layer";
        object.id = name;
        if (!roundNearestTiesAway(x, object.x) || !roundNearestTiesAway(y, object.y)) {
            error = "Tiled image-layer coordinates are outside the SceneDocument integer range";
            return false;
        }
        if (!parseJsonProperties(
                mapGet(layer, "properties"), object.properties, error, &reader_, owner) ||
            !addReservedProperty(object.properties, "tiled.image", stringScalar(resolved), error) ||
            !addReservedProperty(object.properties, "tiled.visible", boolScalar(visible), error) ||
            !addReservedProperty(object.properties, "tiled.sourceX", floatScalar(x), error) ||
            !addReservedProperty(object.properties, "tiled.sourceY", floatScalar(y), error) ||
            !addReservedProperty(object.properties, "tiled.opacity", floatScalar(opacity), error) ||
            !addReservedProperty(
                object.properties, "tiled.parallaxX", floatScalar(parallaxX), error) ||
            !addReservedProperty(
                object.properties, "tiled.parallaxY", floatScalar(parallaxY), error) ||
            !addReservedProperty(
                object.properties, "tiled.tint", integerScalar(static_cast<int64_t>(tint)), error))
            return false;
        bool repeatX = false;
        bool repeatY = false;
        if ((mapHas(layer, "repeatx") && !jsonBool(layer, "repeatx", repeatX)) ||
            (mapHas(layer, "repeaty") && !jsonBool(layer, "repeaty", repeatY))) {
            error = "Tiled image-layer repeat flags must be boolean";
            return false;
        }
        if (!addReservedProperty(object.properties, "tiled.repeatX", boolScalar(repeatX), error) ||
            !addReservedProperty(object.properties, "tiled.repeatY", boolScalar(repeatY), error))
            return false;
        document.objects.push_back(std::move(object));
        return true;
    }

    /// @brief Recursively parse and flatten a JSON layer hierarchy.
    /// @details Builds slash-separated names, composes visibility/rendering and
    ///          group offsets, preserves layer properties, and dispatches tile,
    ///          object, image, and group layers.
    /// @param owner Resolved containing document path.
    /// @param layers Borrowed layer sequence.
    /// @param prefix Parent hierarchical name.
    /// @param inheritedVisible Parent visibility.
    /// @param inheritedX Parent pixel x offset.
    /// @param inheritedY Parent pixel y offset.
    /// @param inheritedOpacity Parent opacity.
    /// @param inheritedParallaxX Parent horizontal parallax.
    /// @param inheritedParallaxY Parent vertical parallax.
    /// @param inheritedTint Parent ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable unsupported-layer or child failure.
    /// @return `true` when every descendant is representable.
    bool parseJsonLayers(const std::string &owner,
                         void *layers,
                         const std::string &prefix,
                         bool inheritedVisible,
                         double inheritedX,
                         double inheritedY,
                         double inheritedOpacity,
                         double inheritedParallaxX,
                         double inheritedParallaxY,
                         uint32_t inheritedTint,
                         MapDocument &document,
                         std::string &error) {
        int64_t count = rt_seq_len(layers);
        for (int64_t index = 0; index < count; ++index) {
            void *layer = rt_seq_get(layers, index);
            std::string type;
            std::string ownName;
            if (!isMap(layer) || !jsonString(layer, "type", type)) {
                error = "Tiled layer is missing a string type";
                return false;
            }
            jsonString(layer, "name", ownName);
            std::string name = prefix.empty() ? ownName : prefix + "/" + ownName;
            bool ownVisible = true;
            if (mapHas(layer, "visible") && !jsonBool(layer, "visible", ownVisible)) {
                error = "Tiled layer visibility must be boolean";
                return false;
            }
            bool visible = inheritedVisible && ownVisible;
            double opacity = 1.0;
            double parallaxX = 1.0;
            double parallaxY = 1.0;
            uint32_t tint = UINT32_C(0xFFFFFFFF);
            if (!composeLayerRenderingJson(layer,
                                           inheritedOpacity,
                                           inheritedParallaxX,
                                           inheritedParallaxY,
                                           inheritedTint,
                                           opacity,
                                           parallaxX,
                                           parallaxY,
                                           tint,
                                           error))
                return false;
            if (!preserveLayerPropertiesJson(owner, layer, name, document, error))
                return false;
            if (type == "tilelayer") {
                if (!parseJsonTileLayer(layer,
                                        name,
                                        visible,
                                        inheritedX,
                                        inheritedY,
                                        opacity,
                                        parallaxX,
                                        parallaxY,
                                        tint,
                                        document,
                                        error))
                    return false;
            } else if (type == "objectgroup") {
                if (!parseJsonObjectLayer(owner,
                                          layer,
                                          name,
                                          visible,
                                          inheritedX,
                                          inheritedY,
                                          opacity,
                                          parallaxX,
                                          parallaxY,
                                          tint,
                                          document,
                                          error))
                    return false;
            } else if (type == "imagelayer") {
                if (!parseJsonImageLayer(owner,
                                         layer,
                                         name,
                                         visible,
                                         inheritedX,
                                         inheritedY,
                                         opacity,
                                         parallaxX,
                                         parallaxY,
                                         tint,
                                         document,
                                         error))
                    return false;
            } else if (type == "group") {
                double groupX = inheritedX;
                double groupY = inheritedY;
                double offset = 0.0;
                if (mapHas(layer, "offsetx")) {
                    if (!jsonNumberValue(mapGet(layer, "offsetx"), offset)) {
                        error = "Tiled group offsetx must be numeric";
                        return false;
                    }
                    groupX += offset;
                }
                offset = 0.0;
                if (mapHas(layer, "offsety")) {
                    if (!jsonNumberValue(mapGet(layer, "offsety"), offset)) {
                        error = "Tiled group offsety must be numeric";
                        return false;
                    }
                    groupY += offset;
                }
                void *children = mapGet(layer, "layers");
                if (!isSeq(children) || !parseJsonLayers(owner,
                                                         children,
                                                         name,
                                                         visible,
                                                         groupX,
                                                         groupY,
                                                         opacity,
                                                         parallaxX,
                                                         parallaxY,
                                                         tint,
                                                         document,
                                                         error))
                    return false;
            } else {
                error = "unsupported Tiled layer type '" + type + "'";
                return false;
            }
        }
        return true;
    }

    /// @brief Compose TMX layer rendering attributes with inherited values.
    /// @details Multiplies opacity/parallax/tint and accepts only the normal
    ///          blend mode.
    /// @param layer Borrowed layer element.
    /// @param inheritedOpacity Parent opacity.
    /// @param inheritedParallaxX Parent horizontal parallax.
    /// @param inheritedParallaxY Parent vertical parallax.
    /// @param inheritedTint Parent ARGB tint.
    /// @param opacity Effective opacity written on success.
    /// @param parallaxX Effective horizontal parallax written on success.
    /// @param parallaxY Effective vertical parallax written on success.
    /// @param tint Effective tint written on success.
    /// @param error Human-readable type/range/overflow failure.
    /// @return `true` when all effective values are finite and supported.
    bool composeLayerRenderingXml(void *layer,
                                  double inheritedOpacity,
                                  double inheritedParallaxX,
                                  double inheritedParallaxY,
                                  uint32_t inheritedTint,
                                  double &opacity,
                                  double &parallaxX,
                                  double &parallaxY,
                                  uint32_t &tint,
                                  std::string &error) {
        double ownOpacity = 1.0;
        double ownParallaxX = 1.0;
        double ownParallaxY = 1.0;
        if (!xmlDouble(layer, "opacity", ownOpacity, false, error) ||
            !xmlDouble(layer, "parallaxx", ownParallaxX, false, error) ||
            !xmlDouble(layer, "parallaxy", ownParallaxY, false, error) || ownOpacity < 0.0 ||
            ownOpacity > 1.0) {
            if (error.empty())
                error = "TMX layer opacity/parallax values are invalid";
            return false;
        }
        uint32_t ownTint = UINT32_C(0xFFFFFFFF);
        std::string tintText = xmlString(layer, "tintcolor");
        if (!tintText.empty() && !parseTiledColor(tintText, ownTint)) {
            error = "TMX layer tintcolor must be #RRGGBB or #AARRGGBB";
            return false;
        }
        std::string blendMode = xmlString(layer, "blendmode");
        if (!blendMode.empty() && blendMode != "normal") {
            error = "unsupported TMX layer blend mode '" + blendMode + "'";
            return false;
        }
        opacity = inheritedOpacity * ownOpacity;
        parallaxX = inheritedParallaxX * ownParallaxX;
        parallaxY = inheritedParallaxY * ownParallaxY;
        tint = multiplyTiledColors(inheritedTint, ownTint);
        if (!std::isfinite(opacity) || !std::isfinite(parallaxX) || !std::isfinite(parallaxY)) {
            error = "composed TMX layer rendering values overflow";
            return false;
        }
        return true;
    }

    /// @brief Decode one TMX tile-layer or chunk data payload.
    /// @details Supports child `<tile>` elements, CSV text, and optionally
    ///          compressed Base64 text with an exact expected cell count.
    /// @param payload Borrowed `<data>` or `<chunk>` element.
    /// @param encodingOwner Borrowed `<data>` element declaring codecs.
    /// @param cells Required GID count.
    /// @param gids Decoded GIDs appended/replaced for the selected encoding.
    /// @param error Human-readable encoding/schema/count failure.
    /// @return `true` when exactly @p cells values are decoded.
    bool parseXmlGidPayload(void *payload,
                            void *encodingOwner,
                            size_t cells,
                            std::vector<int64_t> &gids,
                            std::string &error) {
        std::string encoding = xmlString(encodingOwner, "encoding");
        std::string compression = xmlString(encodingOwner, "compression");
        if (encoding.empty()) {
            if (!compression.empty()) {
                error = "unencoded TMX layer data cannot be compressed";
                return false;
            }
            int64_t childCount = rt_xml_child_count(payload);
            for (int64_t index = 0; index < childCount; ++index) {
                void *tile = rt_xml_child_at(payload, index);
                if (rt_xml_node_type(tile) != XML_NODE_ELEMENT || xmlTag(tile) != "tile")
                    continue;
                int64_t gid = 0;
                if (!xmlInt(tile, "gid", gid, true, error))
                    return false;
                gids.push_back(gid);
            }
            if (gids.size() != cells) {
                error = "TMX layer cell count does not match its dimensions";
                return false;
            }
            return true;
        }
        if (encoding == "csv") {
            if (!compression.empty()) {
                error = "TMX CSV layer data cannot be compressed";
                return false;
            }
            return parseCsv(xmlText(payload), cells, gids, error);
        }
        if (encoding == "base64")
            return decodeBase64Gids(xmlText(payload), compression, cells, gids, error);
        error = "unsupported TMX layer encoding '" + encoding + "'";
        return false;
    }

    /// @brief Parse one finite or chunked TMX tile layer.
    /// @details Mirrors JSON tile-layer placement, including composed offsets,
    ///          overlap detection, scanned infinite bounds, and canonicalization.
    /// @param layer Borrowed `<layer>` element.
    /// @param name Effective hierarchical layer name.
    /// @param visible Effective inherited visibility.
    /// @param inheritedX Parent pixel x offset.
    /// @param inheritedY Parent pixel y offset.
    /// @param opacity Effective opacity.
    /// @param parallaxX Effective horizontal parallax.
    /// @param parallaxY Effective vertical parallax.
    /// @param tint Effective ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable decoding or placement failure.
    /// @return `true` when the layer is appended.
    bool parseXmlTileLayer(void *layer,
                           const std::string &name,
                           bool visible,
                           double inheritedX,
                           double inheritedY,
                           double opacity,
                           double parallaxX,
                           double parallaxY,
                           uint32_t tint,
                           MapDocument &document,
                           std::string &error) {
        int64_t width = 0;
        int64_t height = 0;
        int64_t layerX = 0;
        int64_t layerY = 0;
        double offsetX = inheritedX;
        double offsetY = inheritedY;
        double ownOffset = 0.0;
        if (!xmlInt(layer, "x", layerX, false, error) ||
            !xmlInt(layer, "y", layerY, false, error) ||
            !xmlDouble(layer, "offsetx", ownOffset, false, error)) {
            return false;
        }
        offsetX += ownOffset;
        ownOffset = 0.0;
        if (!xmlDouble(layer, "offsety", ownOffset, false, error))
            return false;
        offsetY += ownOffset;
        if (!std::isfinite(offsetX) || !std::isfinite(offsetY)) {
            error = "composed TMX tile-layer pixel offset overflows";
            return false;
        }
        void *data = firstXmlChild(layer, "data");
        if (!data) {
            error = "TMX tile layer is missing <data>";
            return false;
        }
        std::vector<int64_t> placed(static_cast<size_t>(document.width * document.height), 0);
        if (document.infinite) {
            std::vector<uint8_t> occupied(placed.size(), 0u);
            bool sawChunk = false;
            int64_t childCount = rt_xml_child_count(data);
            for (int64_t chunkIndex = 0; chunkIndex < childCount; ++chunkIndex) {
                void *chunk = rt_xml_child_at(data, chunkIndex);
                if (rt_xml_node_type(chunk) != XML_NODE_ELEMENT || xmlTag(chunk) != "chunk")
                    continue;
                sawChunk = true;
                int64_t chunkX = 0;
                int64_t chunkY = 0;
                if (!xmlInt(chunk, "x", chunkX, true, error) ||
                    !xmlInt(chunk, "y", chunkY, true, error) ||
                    !xmlInt(chunk, "width", width, true, error) ||
                    !xmlInt(chunk, "height", height, true, error) || width <= 0 || height <= 0 ||
                    width > static_cast<int64_t>(kMaxCellsPerLayer) / height) {
                    if (error.empty())
                        error = "TMX infinite chunk dimensions are invalid";
                    return false;
                }
                std::vector<int64_t> gids;
                if (!parseXmlGidPayload(
                        chunk, data, static_cast<size_t>(width * height), gids, error))
                    return false;
                if ((layerX > 0 && chunkX > INT64_MAX - layerX) ||
                    (layerX < 0 && chunkX < INT64_MIN - layerX) ||
                    (layerY > 0 && chunkY > INT64_MAX - layerY) ||
                    (layerY < 0 && chunkY < INT64_MIN - layerY)) {
                    error = "TMX infinite chunk placement overflows";
                    return false;
                }
                int64_t destinationX = chunkX + layerX - document.originTileX;
                int64_t destinationY = chunkY + layerY - document.originTileY;
                if (destinationX < 0 || destinationY < 0 || destinationX > document.width - width ||
                    destinationY > document.height - height) {
                    error = "TMX infinite chunk lies outside scanned map bounds";
                    return false;
                }
                for (int64_t y = 0; y < height; ++y) {
                    for (int64_t x = 0; x < width; ++x) {
                        size_t destination = static_cast<size_t>(
                            (destinationY + y) * document.width + destinationX + x);
                        if (occupied[destination] != 0u) {
                            error = "TMX infinite chunks overlap within a layer";
                            return false;
                        }
                        occupied[destination] = 1u;
                        placed[destination] = gids[static_cast<size_t>(y * width + x)];
                    }
                }
            }
            if (!sawChunk && rt_xml_child_count(data) > 0) {
                error = "infinite TMX tile layer data contains no chunks";
                return false;
            }
        } else {
            if (!xmlInt(layer, "width", width, true, error) ||
                !xmlInt(layer, "height", height, true, error) || width <= 0 || height <= 0 ||
                width > static_cast<int64_t>(kMaxCellsPerLayer) / height) {
                if (error.empty())
                    error = "finite TMX tile layer dimensions are invalid";
                return false;
            }
            std::vector<int64_t> gids;
            if (!parseXmlGidPayload(data, data, static_cast<size_t>(width * height), gids, error) ||
                !placeLayerGids(gids, width, height, layerX, layerY, document, placed, error))
                return false;
        }
        return addLayer(name,
                        visible,
                        std::move(placed),
                        offsetX,
                        offsetY,
                        opacity,
                        parallaxX,
                        parallaxY,
                        tint,
                        document,
                        error);
    }

    /// @brief Preserve an XML object-shape node as bounded formatted text.
    /// @param shape Borrowed shape element.
    /// @param properties Target object properties.
    /// @param name Reserved destination key.
    /// @param error Human-readable formatting, size, or collision failure.
    /// @return `true` when stored as a string scalar.
    bool preserveXmlShape(void *shape,
                          std::map<std::string, Scalar> &properties,
                          const std::string &name,
                          std::string &error) {
        rt_string formatted = rt_xml_format(shape);
        if (!formatted) {
            error = "cannot preserve TMX object shape data";
            return false;
        }
        std::string text = toStd(formatted);
        rt_string_unref(formatted);
        if (text.size() > kMaxStringBytes) {
            error = "TMX object shape data exceeds 64 KiB";
            return false;
        }
        return addReservedProperty(properties, name, stringScalar(std::move(text)), error);
    }

    /// @brief Copy all XML attributes between elements.
    /// @param from Borrowed source element.
    /// @param to Borrowed destination element.
    /// @param skipTemplate Whether to omit the `template` attribute.
    void copyXmlAttributes(void *from, void *to, bool skipTemplate) {
        OwnedObject names(rt_xml_attr_names(from));
        int64_t count = names.get() ? rt_seq_len(names.get()) : 0;
        for (int64_t index = 0; index < count; ++index) {
            rt_string name = rt_seq_get_str(names.get(), index);
            std::string key = toStd(name);
            if (!skipTemplate || key != "template") {
                rt_string value = rt_xml_attr(from, name);
                rt_xml_set_attr(to, name, value);
                rt_str_release_maybe(value);
            }
            rt_string_unref(name);
        }
    }

    /// @brief Deep-clone a supported XML node subtree.
    /// @param node Borrowed element, text, comment, or CDATA node.
    /// @return Owned cloned node, or `nullptr` on unsupported type/allocation
    ///         failure.
    void *cloneXmlNode(void *node) {
        int64_t type = rt_xml_node_type(node);
        if (type == XML_NODE_ELEMENT) {
            rt_string tag = rt_xml_tag(node);
            void *clone = rt_xml_element(tag);
            rt_str_release_maybe(tag);
            if (!clone)
                return nullptr;
            copyXmlAttributes(node, clone, false);
            int64_t count = rt_xml_child_count(node);
            for (int64_t index = 0; index < count; ++index) {
                void *childClone = cloneXmlNode(rt_xml_child_at(node, index));
                if (!childClone) {
                    releaseObject(clone);
                    return nullptr;
                }
                rt_xml_append(clone, childClone);
                releaseObject(childClone);
            }
            return clone;
        }
        rt_string content = rt_xml_content(node);
        void *clone = nullptr;
        if (type == XML_NODE_TEXT)
            clone = rt_xml_text(content);
        else if (type == XML_NODE_COMMENT)
            clone = rt_xml_comment(content);
        else if (type == XML_NODE_CDATA)
            clone = rt_xml_cdata(content);
        rt_str_release_maybe(content);
        return clone;
    }

    /// @brief Clone property children from an object into a merged container.
    /// @param object Borrowed template or instance object element.
    /// @param mergedProperties Borrowed destination `<properties>` element.
    void appendXmlProperties(void *object, void *mergedProperties) {
        void *properties = firstXmlChild(object, "properties");
        if (!properties)
            return;
        int64_t count = rt_xml_child_count(properties);
        for (int64_t index = 0; index < count; ++index) {
            void *property = rt_xml_child_at(properties, index);
            if (rt_xml_node_type(property) == XML_NODE_ELEMENT && xmlTag(property) == "property") {
                void *clone = cloneXmlNode(property);
                if (clone) {
                    rt_xml_append(mergedProperties, clone);
                    releaseObject(clone);
                }
            }
        }
    }

    /// @brief Merge and parse a TMX object-template instance.
    /// @details Reads an acyclic XML template, overlays instance attributes,
    ///          concatenates template/instance properties and shape children,
    ///          records the resolved template path, and parses the merged object.
    /// @param owner Resolved instance document path.
    /// @param instance Borrowed instance `<object>`.
    /// @param templateReference Relative template path.
    /// @param layerName Effective containing layer name.
    /// @param layerVisible Effective containing visibility.
    /// @param offsetX Effective containing x offset.
    /// @param offsetY Effective containing y offset.
    /// @param document Map model to update.
    /// @param error Human-readable dependency, merge, or object failure.
    /// @return `true` when the merged object is appended.
    bool parseXmlTemplateObject(const std::string &owner,
                                void *instance,
                                const std::string &templateReference,
                                const std::string &layerName,
                                bool layerVisible,
                                double offsetX,
                                double offsetY,
                                MapDocument &document,
                                std::string &error) {
        std::string resolved;
        std::string text;
        if (!reader_.readDependency(owner, templateReference, resolved, text, error))
            return false;
        size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || text[first] != '<') {
            error = "a TMX object requires an XML object template";
            return false;
        }
        if (!enter(resolved, error))
            return false;
        std::string normalized = normalizeTiledXmlInput(text);
        rt_string source = makeString(normalized);
        OwnedObject templateDocument(rt_xml_parse(source));
        rt_string_unref(source);
        void *root = templateDocument.get() ? rt_xml_root(templateDocument.get()) : nullptr;
        void *templateObject = root ? firstXmlChild(root, "object") : nullptr;
        if (!root || xmlTag(root) != "template" || !templateObject) {
            error = "TMX template root must be <template> containing <object>";
            leave(resolved);
            return false;
        }
        rt_string objectTag = rt_const_cstr("object");
        OwnedObject merged(rt_xml_element(objectTag));
        rt_string_unref(objectTag);
        if (!merged.get()) {
            error = "cannot allocate merged TMX object template";
            leave(resolved);
            return false;
        }
        copyXmlAttributes(templateObject, merged.get(), false);
        copyXmlAttributes(instance, merged.get(), true);

        rt_string propertiesTag = rt_const_cstr("properties");
        OwnedObject mergedProperties(rt_xml_element(propertiesTag));
        rt_string_unref(propertiesTag);
        appendXmlProperties(templateObject, mergedProperties.get());
        appendXmlProperties(instance, mergedProperties.get());
        if (rt_xml_child_count(mergedProperties.get()) > 0)
            rt_xml_append(merged.get(), mergedProperties.get());

        /// @brief Clone non-property shape children into the merged template object.
        /// @param object Borrowed XML object whose shape children should be appended.
        auto appendShapes = [&](void *object) {
            int64_t count = rt_xml_child_count(object);
            for (int64_t index = 0; index < count; ++index) {
                void *child = rt_xml_child_at(object, index);
                if (rt_xml_node_type(child) == XML_NODE_ELEMENT && xmlTag(child) != "properties") {
                    void *clone = cloneXmlNode(child);
                    if (clone) {
                        rt_xml_append(merged.get(), clone);
                        releaseObject(clone);
                    }
                }
            }
        };
        appendShapes(templateObject);
        appendShapes(instance);
        rt_string markerKey = rt_const_cstr("__zannaTemplatePath");
        rt_string markerValue = makeString(resolved);
        rt_xml_set_attr(merged.get(), markerKey, markerValue);
        rt_string_unref(markerKey);
        rt_string_unref(markerValue);
        bool ok = parseXmlObject(
            resolved, merged.get(), layerName, layerVisible, offsetX, offsetY, document, error);
        leave(resolved);
        return ok;
    }

    /// @brief Convert one TMX `<object>` into a SceneDocument object.
    /// @details Mirrors JSON object conversion while resolving XML templates,
    ///          properties, geometry, visibility, GIDs, and formatted shape
    ///          child elements.
    /// @param owner Resolved containing document path.
    /// @param object Borrowed `<object>` element.
    /// @param layerName Effective containing layer name.
    /// @param layerVisible Effective containing visibility.
    /// @param offsetX Effective containing x offset.
    /// @param offsetY Effective containing y offset.
    /// @param document Map model to update.
    /// @param error Human-readable object/template/property failure.
    /// @return `true` when exactly one object is appended.
    bool parseXmlObject(const std::string &owner,
                        void *object,
                        const std::string &layerName,
                        bool layerVisible,
                        double offsetX,
                        double offsetY,
                        MapDocument &document,
                        std::string &error) {
        std::string templateReference = xmlString(object, "template");
        if (!templateReference.empty())
            return parseXmlTemplateObject(owner,
                                          object,
                                          templateReference,
                                          layerName,
                                          layerVisible,
                                          offsetX,
                                          offsetY,
                                          document,
                                          error);
        if (document.objects.size() >= kMaxObjects) {
            error = "TMX object count exceeds 65536";
            return false;
        }
        double x = 0.0;
        double y = 0.0;
        if (!xmlDouble(object, "x", x, false, error) || !xmlDouble(object, "y", y, false, error))
            return false;
        x += offsetX;
        y += offsetY;
        SceneObject result;
        if (!roundNearestTiesAway(x, result.x) || !roundNearestTiesAway(y, result.y)) {
            error = "TMX object coordinates are outside the SceneDocument integer range";
            return false;
        }
        result.type = xmlString(object, "class");
        if (result.type.empty())
            result.type = xmlString(object, "type");
        if (result.type.empty())
            result.type = "TiledObject";
        std::string authoredName = xmlString(object, "name");
        int64_t numericId = 0;
        if (!xmlInt(object, "id", numericId, false, error) || numericId < 0) {
            error = "TMX object id must be a nonnegative integer";
            return false;
        }
        if (!chooseObjectId(authoredName, numericId, result.id, error))
            return false;
        if (!parseXmlProperties(object, result.properties, error, &reader_, owner) ||
            !addReservedProperty(
                result.properties, "tiled.layer", stringScalar(layerName), error) ||
            !addReservedProperty(
                result.properties, "tiled.numericId", integerScalar(numericId), error) ||
            !addReservedProperty(
                result.properties, "tiled.layerVisible", boolScalar(layerVisible), error))
            return false;
        if (!addReservedProperty(result.properties, "tiled.sourceX", floatScalar(x), error) ||
            !addReservedProperty(result.properties, "tiled.sourceY", floatScalar(y), error))
            return false;
        std::string templatePath = xmlString(object, "__zannaTemplatePath");
        if (!templatePath.empty() &&
            !addReservedProperty(
                result.properties, "tiled.template", stringScalar(templatePath), error))
            return false;
        double width = 0.0;
        double height = 0.0;
        double rotation = 0.0;
        if (!xmlDouble(object, "width", width, false, error) ||
            !xmlDouble(object, "height", height, false, error) ||
            !xmlDouble(object, "rotation", rotation, false, error) ||
            !addReservedProperty(result.properties, "tiled.width", floatScalar(width), error) ||
            !addReservedProperty(result.properties, "tiled.height", floatScalar(height), error) ||
            !addReservedProperty(result.properties, "tiled.rotation", floatScalar(rotation), error))
            return false;
        int64_t visible = 1;
        if (!xmlInt(object, "visible", visible, false, error) ||
            !addReservedProperty(
                result.properties, "tiled.visible", boolScalar(visible != 0), error))
            return false;
        int64_t gid = 0;
        bool hasGid = false;
        std::string gidText = xmlString(object, "gid", &hasGid);
        if (hasGid) {
            if (!parseIntegerText(gidText, gid) || gid < 0 ||
                static_cast<uint64_t>(gid) > UINT32_MAX) {
                error = "TMX tile object GID is invalid";
                return false;
            }
            int64_t rawGid = gid;
            std::vector<int64_t> normalized{gid};
            if (!normalizeGids(normalized, document, UINT32_C(0xFFFFFFFF), UINT16_MAX, error))
                return false;
            uint32_t flags =
                static_cast<uint32_t>(rawGid) & static_cast<uint32_t>(kTiledTransformMask);
            if (document.orientation != "hexagonal")
                flags &= ~static_cast<uint32_t>(kTiledHex120Flag);
            if (!addReservedProperty(
                    result.properties, "tiled.gid", integerScalar(normalized.front()), error) ||
                !addReservedProperty(
                    result.properties, "tiled.rawGid", integerScalar(rawGid), error) ||
                !addReservedProperty(result.properties,
                                     "tiled.gidFlags",
                                     integerScalar(static_cast<int64_t>(flags)),
                                     error))
                return false;
        }
        int64_t childCount = rt_xml_child_count(object);
        for (int64_t index = 0; index < childCount; ++index) {
            void *shape = rt_xml_child_at(object, index);
            if (rt_xml_node_type(shape) != XML_NODE_ELEMENT)
                continue;
            std::string tag = xmlTag(shape);
            if (tag == "properties")
                continue;
            if (tag != "ellipse" && tag != "point" && tag != "polygon" && tag != "polyline" &&
                tag != "text") {
                error = "unsupported TMX object child <" + tag + ">";
                return false;
            }
            if (!preserveXmlShape(shape, result.properties, "tiled." + tag + "Xml", error))
                return false;
        }
        document.objects.push_back(std::move(result));
        return true;
    }

    /// @brief Parse a TMX object layer and attach effective render metadata.
    /// @param owner Resolved containing document path.
    /// @param layer Borrowed `<objectgroup>` element.
    /// @param name Effective hierarchical layer name.
    /// @param visible Effective visibility.
    /// @param inheritedX Parent pixel x offset.
    /// @param inheritedY Parent pixel y offset.
    /// @param opacity Effective opacity.
    /// @param parallaxX Effective horizontal parallax.
    /// @param parallaxY Effective vertical parallax.
    /// @param tint Effective ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable schema/object/property failure.
    /// @return `true` when every source object produces one result.
    bool parseXmlObjectLayer(const std::string &owner,
                             void *layer,
                             const std::string &name,
                             bool visible,
                             double inheritedX,
                             double inheritedY,
                             double opacity,
                             double parallaxX,
                             double parallaxY,
                             uint32_t tint,
                             MapDocument &document,
                             std::string &error) {
        double offsetX = inheritedX;
        double offsetY = inheritedY;
        double value = 0.0;
        if (!xmlDouble(layer, "offsetx", value, false, error))
            return false;
        offsetX += value;
        value = 0.0;
        if (!xmlDouble(layer, "offsety", value, false, error))
            return false;
        offsetY += value;
        int64_t count = rt_xml_child_count(layer);
        for (int64_t index = 0; index < count; ++index) {
            void *object = rt_xml_child_at(layer, index);
            if (rt_xml_node_type(object) != XML_NODE_ELEMENT || xmlTag(object) != "object")
                continue;
            size_t before = document.objects.size();
            if (!parseXmlObject(owner, object, name, visible, offsetX, offsetY, document, error))
                return false;
            if (document.objects.size() != before + 1u) {
                error = "TMX object import did not produce exactly one object";
                return false;
            }
            auto &properties = document.objects.back().properties;
            if (!addReservedProperty(properties, "tiled.opacity", floatScalar(opacity), error) ||
                !addReservedProperty(
                    properties, "tiled.parallaxX", floatScalar(parallaxX), error) ||
                !addReservedProperty(
                    properties, "tiled.parallaxY", floatScalar(parallaxY), error) ||
                !addReservedProperty(
                    properties, "tiled.tint", integerScalar(static_cast<int64_t>(tint)), error))
                return false;
        }
        return true;
    }

    /// @brief Convert a TMX image layer into a metadata SceneDocument object.
    /// @param owner Resolved containing document path.
    /// @param layer Borrowed `<imagelayer>` element.
    /// @param name Effective hierarchical layer name and object ID.
    /// @param visible Effective visibility.
    /// @param inheritedX Parent pixel x offset.
    /// @param inheritedY Parent pixel y offset.
    /// @param opacity Effective opacity.
    /// @param parallaxX Effective horizontal parallax.
    /// @param parallaxY Effective vertical parallax.
    /// @param tint Effective ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable dependency or metadata failure.
    /// @return `true` when the metadata object is appended.
    bool parseXmlImageLayer(const std::string &owner,
                            void *layer,
                            const std::string &name,
                            bool visible,
                            double inheritedX,
                            double inheritedY,
                            double opacity,
                            double parallaxX,
                            double parallaxY,
                            uint32_t tint,
                            MapDocument &document,
                            std::string &error) {
        void *image = firstXmlChild(layer, "image");
        std::string reference = image ? xmlString(image, "source") : std::string();
        if (!image || reference.empty()) {
            error = "TMX image layer is missing an image";
            return false;
        }
        std::string resolved;
        if (!reader_.resolveDependencyPath(owner, reference, resolved, error))
            return false;
        double x = inheritedX;
        double y = inheritedY;
        double offset = 0.0;
        if (!xmlDouble(layer, "offsetx", offset, false, error))
            return false;
        x += offset;
        offset = 0.0;
        if (!xmlDouble(layer, "offsety", offset, false, error))
            return false;
        y += offset;
        SceneObject object;
        object.type = "tiled.image-layer";
        object.id = name;
        if (!roundNearestTiesAway(x, object.x) || !roundNearestTiesAway(y, object.y)) {
            error = "TMX image-layer coordinates are outside the SceneDocument integer range";
            return false;
        }
        int64_t repeatX = 0;
        int64_t repeatY = 0;
        if (!xmlInt(layer, "repeatx", repeatX, false, error) ||
            !xmlInt(layer, "repeaty", repeatY, false, error) ||
            !parseXmlProperties(layer, object.properties, error, &reader_, owner) ||
            !addReservedProperty(object.properties, "tiled.image", stringScalar(resolved), error) ||
            !addReservedProperty(object.properties, "tiled.visible", boolScalar(visible), error) ||
            !addReservedProperty(object.properties, "tiled.sourceX", floatScalar(x), error) ||
            !addReservedProperty(object.properties, "tiled.sourceY", floatScalar(y), error) ||
            !addReservedProperty(object.properties, "tiled.opacity", floatScalar(opacity), error) ||
            !addReservedProperty(
                object.properties, "tiled.parallaxX", floatScalar(parallaxX), error) ||
            !addReservedProperty(
                object.properties, "tiled.parallaxY", floatScalar(parallaxY), error) ||
            !addReservedProperty(object.properties,
                                 "tiled.tint",
                                 integerScalar(static_cast<int64_t>(tint)),
                                 error) ||
            !addReservedProperty(
                object.properties, "tiled.repeatX", boolScalar(repeatX != 0), error) ||
            !addReservedProperty(
                object.properties, "tiled.repeatY", boolScalar(repeatY != 0), error))
            return false;
        document.objects.push_back(std::move(object));
        return true;
    }

    /// @brief Recursively parse and flatten a TMX layer hierarchy.
    /// @details Builds slash-separated names, composes visibility/rendering and
    ///          group offsets, preserves layer properties, and dispatches tile,
    ///          object, image, and group elements.
    /// @param owner Resolved containing document path.
    /// @param parent Borrowed map or group element.
    /// @param prefix Parent hierarchical name.
    /// @param inheritedVisible Parent visibility.
    /// @param inheritedX Parent pixel x offset.
    /// @param inheritedY Parent pixel y offset.
    /// @param inheritedOpacity Parent opacity.
    /// @param inheritedParallaxX Parent horizontal parallax.
    /// @param inheritedParallaxY Parent vertical parallax.
    /// @param inheritedTint Parent ARGB tint.
    /// @param document Map model to update.
    /// @param error Human-readable child parsing failure.
    /// @return `true` when every descendant is representable.
    bool parseXmlLayers(const std::string &owner,
                        void *parent,
                        const std::string &prefix,
                        bool inheritedVisible,
                        double inheritedX,
                        double inheritedY,
                        double inheritedOpacity,
                        double inheritedParallaxX,
                        double inheritedParallaxY,
                        uint32_t inheritedTint,
                        MapDocument &document,
                        std::string &error) {
        int64_t count = rt_xml_child_count(parent);
        for (int64_t index = 0; index < count; ++index) {
            void *layer = rt_xml_child_at(parent, index);
            if (rt_xml_node_type(layer) != XML_NODE_ELEMENT)
                continue;
            std::string tag = xmlTag(layer);
            if (tag != "layer" && tag != "objectgroup" && tag != "imagelayer" && tag != "group")
                continue;
            std::string ownName = xmlString(layer, "name");
            std::string name = prefix.empty() ? ownName : prefix + "/" + ownName;
            int64_t ownVisible = 1;
            if (!xmlInt(layer, "visible", ownVisible, false, error))
                return false;
            bool visible = inheritedVisible && ownVisible != 0;
            double opacity = 1.0;
            double parallaxX = 1.0;
            double parallaxY = 1.0;
            uint32_t tint = UINT32_C(0xFFFFFFFF);
            if (!composeLayerRenderingXml(layer,
                                          inheritedOpacity,
                                          inheritedParallaxX,
                                          inheritedParallaxY,
                                          inheritedTint,
                                          opacity,
                                          parallaxX,
                                          parallaxY,
                                          tint,
                                          error))
                return false;
            if (!preserveLayerPropertiesXml(owner, layer, name, document, error))
                return false;
            if (tag == "layer") {
                if (!parseXmlTileLayer(layer,
                                       name,
                                       visible,
                                       inheritedX,
                                       inheritedY,
                                       opacity,
                                       parallaxX,
                                       parallaxY,
                                       tint,
                                       document,
                                       error))
                    return false;
            } else if (tag == "objectgroup") {
                if (!parseXmlObjectLayer(owner,
                                         layer,
                                         name,
                                         visible,
                                         inheritedX,
                                         inheritedY,
                                         opacity,
                                         parallaxX,
                                         parallaxY,
                                         tint,
                                         document,
                                         error))
                    return false;
            } else if (tag == "imagelayer") {
                if (!parseXmlImageLayer(owner,
                                        layer,
                                        name,
                                        visible,
                                        inheritedX,
                                        inheritedY,
                                        opacity,
                                        parallaxX,
                                        parallaxY,
                                        tint,
                                        document,
                                        error))
                    return false;
            } else {
                double groupX = inheritedX;
                double groupY = inheritedY;
                double offset = 0.0;
                if (!xmlDouble(layer, "offsetx", offset, false, error))
                    return false;
                groupX += offset;
                offset = 0.0;
                if (!xmlDouble(layer, "offsety", offset, false, error))
                    return false;
                groupY += offset;
                if (!parseXmlLayers(owner,
                                    layer,
                                    name,
                                    visible,
                                    groupX,
                                    groupY,
                                    opacity,
                                    parallaxX,
                                    parallaxY,
                                    tint,
                                    document,
                                    error))
                    return false;
            }
        }
        return true;
    }

    /// @brief Parse a TMX root map into the intermediate document.
    /// @details Normalizes XML prologue bytes, validates map geometry and
    ///          projection metadata, resolves TSX/embedded tilesets, prepares
    ///          canonical IDs, and recursively parses supported layers.
    /// @param owner Resolved root path for relative dependencies.
    /// @param text Complete TMX bytes.
    /// @param document Destination map model.
    /// @param error Human-readable parse/schema/limit failure.
    /// @return `true` when all root content is representable.
    bool parseTmxMap(const std::string &owner,
                     const std::string &text,
                     MapDocument &document,
                     std::string &error) {
        std::string normalizedXml = normalizeTiledXmlInput(text);
        rt_string source = makeString(normalizedXml);
        OwnedObject xmlDocument(rt_xml_parse(source));
        rt_string_unref(source);
        void *root = xmlDocument.get() ? rt_xml_root(xmlDocument.get()) : nullptr;
        if (!root || xmlTag(root) != "map") {
            rt_string xmlError = rt_xml_error();
            error = "invalid TMX map";
            std::string detail = toStd(xmlError);
            rt_str_release_maybe(xmlError);
            if (!detail.empty())
                error += ": " + detail;
            return false;
        }
        document.orientation = xmlString(root, "orientation");
        if (document.orientation.empty()) {
            error = "TMX map is missing orientation";
            return false;
        }
        std::string renderOrder = xmlString(root, "renderorder");
        if (!renderOrder.empty())
            document.renderOrder = std::move(renderOrder);
        int64_t infinite = 0;
        if (!xmlInt(root, "infinite", infinite, false, error) || (infinite != 0 && infinite != 1)) {
            if (error.empty())
                error = "TMX map infinite attribute must be 0 or 1";
            return false;
        }
        document.infinite = infinite != 0;
        int64_t sourceWidth = 0;
        int64_t sourceHeight = 0;
        if (!xmlInt(root, "width", sourceWidth, true, error) ||
            !xmlInt(root, "height", sourceHeight, true, error) ||
            !xmlInt(root, "tilewidth", document.tileWidth, true, error) ||
            !xmlInt(root, "tileheight", document.tileHeight, true, error) ||
            document.tileWidth <= 0 || document.tileHeight <= 0)
            return false;
        document.projectionHeight = sourceHeight;
        document.staggerAxis = xmlString(root, "staggeraxis");
        document.staggerIndex = xmlString(root, "staggerindex");
        if (!xmlInt(root, "hexsidelength", document.hexSideLength, false, error) ||
            !xmlDouble(root, "skewx", document.skewX, false, error) ||
            !xmlDouble(root, "skewy", document.skewY, false, error) ||
            !xmlDouble(root, "parallaxoriginx", document.parallaxOriginX, false, error) ||
            !xmlDouble(root, "parallaxoriginy", document.parallaxOriginY, false, error) ||
            !validateOrientation(document, error))
            return false;
        if (document.infinite) {
            if (sourceWidth < 0 || sourceHeight < 0) {
                error = "infinite TMX map source dimensions cannot be negative";
                return false;
            }
            TileBounds bounds;
            if (!scanXmlInfiniteLayers(root, 0, 0, bounds, error) ||
                !applyInfiniteBounds(bounds, document, error))
                return false;
        } else {
            document.width = sourceWidth;
            document.height = sourceHeight;
            if (!validateMapDimensions(document, error))
                return false;
        }
        if (!parseXmlProperties(root, document.properties, error, &reader_, owner))
            return false;

        int64_t count = rt_xml_child_count(root);
        for (int64_t index = 0; index < count; ++index) {
            void *entry = rt_xml_child_at(root, index);
            if (rt_xml_node_type(entry) != XML_NODE_ELEMENT || xmlTag(entry) != "tileset")
                continue;
            int64_t firstGid = 0;
            if (!xmlInt(entry, "firstgid", firstGid, true, error) || firstGid <= 0) {
                error = "TMX tileset reference has an invalid firstgid";
                return false;
            }
            Tileset tileset;
            tileset.firstGid = firstGid;
            std::string dependency = xmlString(entry, "source");
            if (!dependency.empty()) {
                std::string resolved;
                std::string dependencyText;
                if (!reader_.readDependency(owner, dependency, resolved, dependencyText, error) ||
                    !parseTilesetText(resolved, dependencyText, tileset, error))
                    return false;
            } else if (!parseXmlTileset(owner, entry, tileset, error)) {
                return false;
            }
            document.tilesets.push_back(std::move(tileset));
        }
        if (document.tilesets.empty()) {
            error = "TMX map has no tilesets";
            return false;
        }
        std::sort(document.tilesets.begin(),
                  document.tilesets.end(),
                  /// @brief Order parsed tilesets by their first global tile ID.
                  /// @param left First tileset candidate.
                  /// @param right Second tileset candidate.
                  /// @return `true` when @p left precedes @p right by first GID.
                  [](const Tileset &left, const Tileset &right) {
                      return left.firstGid < right.firstGid;
                  });
        if (!prepareCanonicalTiles(document, error))
            return false;
        return parseXmlLayers(
            owner, root, "", true, 0.0, 0.0, 1.0, 1.0, 1.0, UINT32_C(0xFFFFFFFF), document, error);
    }

    /// @brief Merge local tile metadata into one canonical tile variant.
    /// @details Accumulates solid IDs and scalar properties while rejecting
    ///          conflicting values, and installs a validated animation rule.
    /// @param document Map model to update.
    /// @param canonicalTile Canonical tile ID receiving metadata.
    /// @param metadata Parsed local-tile metadata.
    /// @param error Human-readable conflict or animation failure.
    /// @return `true` when metadata is absent or merges consistently.
    bool mergeMetadata(MapDocument &document,
                       int64_t canonicalTile,
                       const TileMetadata &metadata,
                       std::string &error) {
        if (metadata.solid)
            document.solidTiles.insert(canonicalTile);
        if (!metadata.properties.empty()) {
            auto &target = document.tileProperties[canonicalTile];
            for (const auto &[name, value] : metadata.properties) {
                auto found = target.find(name);
                if (found != target.end() && !scalarEqual(found->second, value)) {
                    error = "different Tiled tilesets assign conflicting tile property '" + name +
                            "' to local tile " + std::to_string(canonicalTile);
                    return false;
                }
                target[name] = value;
            }
        }
        if (!metadata.animationFrames.empty()) {
            if (metadata.animationDurations.size() != metadata.animationFrames.size()) {
                error = "Tiled animation frame/duration count mismatch";
                return false;
            }
            TileAnimation animation;
            animation.baseTile = canonicalTile;
            animation.frames = metadata.animationFrames;
            animation.durations = metadata.animationDurations;
            animation.milliseconds = metadata.animationDurations.front();
            auto found = document.animations.find(canonicalTile);
            if (found != document.animations.end() &&
                (found->second.milliseconds != animation.milliseconds ||
                 found->second.frames != animation.frames ||
                 found->second.durations != animation.durations)) {
                error = "different Tiled tilesets assign conflicting animations to local tile " +
                        std::to_string(canonicalTile);
                return false;
            }
            document.animations[canonicalTile] = std::move(animation);
        }
        return true;
    }

    /// @brief Derive a common atlas frame and draw offset from declared artwork.
    /// @details Accounts for per-tile dimensions, tileset offsets, diagonal
    ///          transforms, and hex rotations across all canonical variants.
    /// @param document Map model receiving source frame size and draw offset.
    /// @param error Human-readable unsafe-dimension or overflow failure.
    /// @return `true` when bounded artwork extents are representable.
    bool computeDeclaredArtworkLayout(MapDocument &document, std::string &error) {
        long double minimumX = 0.0L;
        long double minimumY = 0.0L;
        long double maximumX = 0.0L;
        long double maximumY = 0.0L;
        bool haveBounds = false;
        for (size_t index = 1u; index < document.canonicalTiles.size(); ++index) {
            const CanonicalTile &tile = document.canonicalTiles[index];
            const Tileset &tileset = document.tilesets[static_cast<size_t>(tile.tileset)];
            int64_t sourceWidth = tileset.tileWidth;
            int64_t sourceHeight = tileset.tileHeight;
            auto image = tileset.tileImages.find(tile.localId);
            if (image != tileset.tileImages.end()) {
                if (image->second.width > 0)
                    sourceWidth = image->second.width;
                if (image->second.height > 0)
                    sourceHeight = image->second.height;
            } else if (tileset.image.empty()) {
                continue;
            }
            if (sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > INT32_MAX ||
                sourceHeight > INT32_MAX) {
                error = "Tiled declared tile artwork dimensions are unsafe";
                return false;
            }
            int64_t transformedWidth = sourceWidth;
            int64_t transformedHeight = sourceHeight;
            if (document.orientation != "hexagonal" &&
                (tile.transformFlags & kTiledDiagonalOrHex60Flag) != 0u) {
                transformedWidth = sourceHeight;
                transformedHeight = sourceWidth;
            } else if (document.orientation == "hexagonal") {
                int degrees = 0;
                if ((tile.transformFlags & kTiledDiagonalOrHex60Flag) != 0u)
                    degrees += 60;
                if ((tile.transformFlags & kTiledHex120Flag) != 0u)
                    degrees += 120;
                if (degrees != 0 && degrees != 180) {
                    double radians = static_cast<double>(degrees) * 3.14159265358979323846 / 180.0;
                    transformedWidth =
                        std::max<int64_t>(1,
                                          static_cast<int64_t>(std::ceil(
                                              std::fabs(std::cos(radians)) * sourceWidth +
                                              std::fabs(std::sin(radians)) * sourceHeight)));
                    transformedHeight =
                        std::max<int64_t>(1,
                                          static_cast<int64_t>(std::ceil(
                                              std::fabs(std::sin(radians)) * sourceWidth +
                                              std::fabs(std::cos(radians)) * sourceHeight)));
                }
            }
            long double relativeX = static_cast<long double>(tileset.tileOffsetX) +
                                    static_cast<long double>(sourceWidth - transformedWidth) / 2.0L;
            long double relativeY =
                static_cast<long double>(document.tileHeight) -
                static_cast<long double>(sourceHeight) +
                static_cast<long double>(tileset.tileOffsetY) +
                static_cast<long double>(sourceHeight - transformedHeight) / 2.0L;
            long double endX = relativeX + transformedWidth;
            long double endY = relativeY + transformedHeight;
            if (!haveBounds) {
                minimumX = relativeX;
                minimumY = relativeY;
                maximumX = endX;
                maximumY = endY;
                haveBounds = true;
            } else {
                minimumX = std::min(minimumX, relativeX);
                minimumY = std::min(minimumY, relativeY);
                maximumX = std::max(maximumX, endX);
                maximumY = std::max(maximumY, endY);
            }
        }
        if (!haveBounds) {
            minimumX = 0.0L;
            minimumY = 0.0L;
            maximumX = static_cast<long double>(document.tileWidth);
            maximumY = static_cast<long double>(document.tileHeight);
        }
        long double frameWidth = maximumX - minimumX;
        long double frameHeight = maximumY - minimumY;
        if (minimumX < static_cast<long double>(INT64_MIN) ||
            minimumX > static_cast<long double>(INT64_MAX) ||
            minimumY < static_cast<long double>(INT64_MIN) ||
            minimumY > static_cast<long double>(INT64_MAX) || frameWidth <= 0.0L ||
            frameHeight <= 0.0L || frameWidth > static_cast<long double>(INT64_MAX) ||
            frameHeight > static_cast<long double>(INT64_MAX)) {
            error = "Tiled declared artwork bounds overflow";
            return false;
        }
        document.drawOffsetX = static_cast<int64_t>(minimumX);
        document.drawOffsetY = static_cast<int64_t>(minimumY);
        document.sourceFrameWidth = static_cast<int64_t>(frameWidth);
        document.sourceFrameHeight = static_cast<int64_t>(frameHeight);
        return true;
    }

    /// @brief Complete cross-tileset metadata and artwork normalization.
    /// @details Creates a base layer when none exists, validates metadata and
    ///          animation local IDs, materializes transformed animation-frame
    ///          variants, propagates reachability, merges metadata, and derives
    ///          the declared atlas frame.
    /// @param document Parsed map model to finalize.
    /// @param error Human-readable validation or capacity failure.
    /// @return `true` when the model is ready for scene serialization/rendering.
    bool finalize(MapDocument &document, std::string &error) {
        if (document.layers.empty()) {
            std::vector<int64_t> empty(static_cast<size_t>(document.width * document.height), 0);
            if (!addLayer("base",
                          true,
                          std::move(empty),
                          0.0,
                          0.0,
                          1.0,
                          1.0,
                          1.0,
                          UINT32_C(0xFFFFFFFF),
                          document,
                          error))
                return false;
        }
        for (const Tileset &tileset : document.tilesets) {
            for (const auto &[zeroBasedTile, metadata] : tileset.metadata) {
                if (zeroBasedTile < 0 || zeroBasedTile >= tileset.tileCount) {
                    error = "Tiled tile metadata id exceeds tileset tilecount";
                    return false;
                }
                for (int64_t frame : metadata.animationFrames) {
                    if (frame <= 0 || frame > tileset.tileCount) {
                        error = "Tiled animation frame exceeds tileset tilecount";
                        return false;
                    }
                }
            }
        }
        for (size_t canonicalIndex = 1u; canonicalIndex < document.canonicalTiles.size();
             ++canonicalIndex) {
            const CanonicalTile source = document.canonicalTiles[canonicalIndex];
            if (source.tileset < 0 ||
                static_cast<size_t>(source.tileset) >= document.tilesets.size()) {
                error = "internal Tiled canonical tileset selection failed";
                return false;
            }
            const Tileset &tileset = document.tilesets[static_cast<size_t>(source.tileset)];
            auto metadataIt = tileset.metadata.find(source.localId);
            if (metadataIt == tileset.metadata.end())
                continue;
            TileMetadata metadata = metadataIt->second;
            for (int64_t &frame : metadata.animationFrames) {
                CanonicalTileKey key;
                key.tileset = source.tileset;
                key.localId = frame - 1;
                key.transformFlags = source.transformFlags;
                key.tint = source.tint;
                key.opacity = source.opacity;
                auto found = document.canonicalVariants.find(key);
                if (found == document.canonicalVariants.end()) {
                    if (document.canonicalTiles.size() >= kMaxCellsPerLayer) {
                        error = "Tiled canonical animation variant count exceeds 1048575";
                        return false;
                    }
                    CanonicalTile variant;
                    variant.tileset = key.tileset;
                    variant.localId = key.localId;
                    variant.transformFlags = key.transformFlags;
                    variant.tint = key.tint;
                    variant.opacity = key.opacity;
                    variant.reachable = source.reachable;
                    document.canonicalTiles.push_back(variant);
                    frame = static_cast<int64_t>(document.canonicalTiles.size() - 1u);
                    document.canonicalVariants.emplace(key, frame);
                } else {
                    frame = found->second;
                    if (source.reachable)
                        document.canonicalTiles[static_cast<size_t>(frame)].reachable = true;
                }
            }
            if (!mergeMetadata(document, static_cast<int64_t>(canonicalIndex), metadata, error))
                return false;
        }
        return computeDeclaredArtworkLayout(document, error);
    }

    SourceReader reader_;
    std::set<std::string> active_;
    std::set<std::string> objectIds_;
};

/// @brief Serialize an ordered scalar map as compact JSON.
/// @param out Destination stream.
/// @param properties Lexically ordered property map.
void writeScalarMap(std::ostringstream &out, const std::map<std::string, Scalar> &properties) {
    out << '{';
    bool first = true;
    for (const auto &[name, value] : properties) {
        if (!first)
            out << ',';
        first = false;
        out << jsonEscape(name) << ':' << scalarJson(value);
    }
    out << '}';
}

/// @brief Serialize the normalized map model as SceneDocument JSON.
/// @details Emits editable layers/objects and typed collision/property/
///          animation sections plus a preserved `tiledRuntime` section carrying
///          projection, artwork, canonical-variant, and dependency metadata.
/// @param document Finalized Tiled map model.
/// @return Complete compact SceneDocument JSON.
std::string toSceneJson(const MapDocument &document) {
    std::ostringstream out;
    out << '{';
    out << "\"version\":1";
    out << ",\"name\":" << jsonEscape(document.name);
    out << ",\"width\":" << document.width;
    out << ",\"height\":" << document.height;
    out << ",\"tileWidth\":" << document.tileWidth;
    out << ",\"tileHeight\":" << document.tileHeight;
    std::string defaultAsset;
    if (document.tilesets.size() == 1u)
        defaultAsset = document.tilesets.front().image;
    out << ",\"tilesetAsset\":" << jsonEscape(defaultAsset);
    out << ",\"properties\":";
    writeScalarMap(out, document.properties);
    out << ",\"layers\":[";
    for (size_t layerIndex = 0; layerIndex < document.layers.size(); ++layerIndex) {
        if (layerIndex > 0)
            out << ',';
        const Layer &layer = document.layers[layerIndex];
        out << '{';
        out << "\"name\":" << jsonEscape(layer.name);
        out << ",\"visible\":" << (layer.visible ? "true" : "false");
        out << ",\"asset\":" << jsonEscape(defaultAsset);
        out << ",\"tiles\":[";
        for (size_t tileIndex = 0; tileIndex < layer.tiles.size(); ++tileIndex) {
            if (tileIndex > 0)
                out << ',';
            out << layer.tiles[tileIndex];
        }
        out << "]}";
    }
    out << ']';
    out << ",\"objects\":[";
    for (size_t objectIndex = 0; objectIndex < document.objects.size(); ++objectIndex) {
        if (objectIndex > 0)
            out << ',';
        const SceneObject &object = document.objects[objectIndex];
        out << '{';
        out << "\"type\":" << jsonEscape(object.type);
        out << ",\"id\":" << jsonEscape(object.id);
        out << ",\"x\":" << object.x;
        out << ",\"y\":" << object.y;
        out << ",\"properties\":";
        writeScalarMap(out, object.properties);
        out << '}';
    }
    out << ']';
    out << ",\"collision\":{\"layer\":0,\"solid\":[";
    bool first = true;
    for (int64_t tile : document.solidTiles) {
        if (!first)
            out << ',';
        first = false;
        out << tile;
    }
    out << "]}";
    out << ",\"tileProperties\":{";
    first = true;
    for (const auto &[tile, properties] : document.tileProperties) {
        if (!first)
            out << ',';
        first = false;
        out << jsonEscape(std::to_string(tile)) << ':';
        writeScalarMap(out, properties);
    }
    out << '}';
    out << ",\"animations\":[";
    first = true;
    for (const auto &[tile, animation] : document.animations) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"baseTile\":" << tile;
        out << ",\"frameCount\":" << animation.frames.size();
        out << ",\"msPerFrame\":" << animation.milliseconds;
        out << ",\"frames\":[";
        for (size_t frame = 0; frame < animation.frames.size(); ++frame) {
            if (frame > 0)
                out << ',';
            out << animation.frames[frame];
        }
        out << "],\"durations\":[";
        for (size_t frame = 0; frame < animation.durations.size(); ++frame) {
            if (frame > 0)
                out << ',';
            out << animation.durations[frame];
        }
        out << "]}";
    }
    out << ']';
    out << ",\"tiledRuntime\":{";
    out << "\"orientation\":" << jsonEscape(document.orientation);
    out << ",\"renderOrder\":" << jsonEscape(document.renderOrder);
    out << ",\"infinite\":" << (document.infinite ? "true" : "false");
    out << ",\"originTileX\":" << document.originTileX;
    out << ",\"originTileY\":" << document.originTileY;
    out << ",\"projectionHeight\":" << document.projectionHeight;
    out << ",\"staggerAxis\":" << jsonEscape(document.staggerAxis);
    out << ",\"staggerIndex\":" << jsonEscape(document.staggerIndex);
    out << ",\"hexSideLength\":" << document.hexSideLength;
    out << ",\"skewX\":" << jsonDouble(document.skewX);
    out << ",\"skewY\":" << jsonDouble(document.skewY);
    out << ",\"parallaxOriginX\":" << jsonDouble(document.parallaxOriginX);
    out << ",\"parallaxOriginY\":" << jsonDouble(document.parallaxOriginY);
    out << ",\"sourceFrameWidth\":" << document.sourceFrameWidth;
    out << ",\"sourceFrameHeight\":" << document.sourceFrameHeight;
    out << ",\"drawOffsetX\":" << document.drawOffsetX;
    out << ",\"drawOffsetY\":" << document.drawOffsetY;
    out << ",\"layers\":[";
    for (size_t layerIndex = 0; layerIndex < document.layers.size(); ++layerIndex) {
        if (layerIndex > 0)
            out << ',';
        const Layer &layer = document.layers[layerIndex];
        out << '{';
        out << "\"offsetX\":" << jsonDouble(layer.offsetX);
        out << ",\"offsetY\":" << jsonDouble(layer.offsetY);
        out << ",\"opacity\":" << jsonDouble(layer.opacity);
        out << ",\"parallaxX\":" << jsonDouble(layer.parallaxX);
        out << ",\"parallaxY\":" << jsonDouble(layer.parallaxY);
        out << ",\"tint\":" << layer.tint;
        out << '}';
    }
    out << ']';
    out << ",\"tilesets\":[";
    for (size_t index = 0; index < document.tilesets.size(); ++index) {
        if (index > 0)
            out << ',';
        const Tileset &tileset = document.tilesets[index];
        out << '{';
        out << "\"canonicalFirstId\":" << tileset.canonicalFirstId;
        out << ",\"tileCount\":" << tileset.tileCount;
        out << ",\"firstGid\":" << tileset.firstGid;
        out << ",\"tileOffsetX\":" << tileset.tileOffsetX;
        out << ",\"tileOffsetY\":" << tileset.tileOffsetY;
        out << ",\"image\":" << jsonEscape(tileset.image);
        out << '}';
    }
    out << ']';
    out << ",\"canonicalVariants\":[";
    bool firstVariant = true;
    for (size_t index = 1u; index < document.canonicalTiles.size(); ++index) {
        const CanonicalTile &tile = document.canonicalTiles[index];
        if (tile.transformFlags == 0u && tile.tint == UINT32_C(0xFFFFFFFF) &&
            tile.opacity == UINT16_MAX)
            continue;
        if (!firstVariant)
            out << ',';
        firstVariant = false;
        out << '{';
        out << "\"id\":" << index;
        out << ",\"tileset\":" << tile.tileset;
        out << ",\"localId\":" << tile.localId;
        out << ",\"flags\":" << tile.transformFlags;
        out << ",\"tint\":" << tile.tint;
        out << ",\"opacity\":" << tile.opacity;
        out << '}';
    }
    out << ']';
    out << ",\"dependencies\":[";
    bool firstDependency = true;
    std::set<std::string> dependencies;
    for (const Tileset &tileset : document.tilesets) {
        if (!tileset.image.empty())
            dependencies.insert(tileset.image);
        for (const auto &[_, image] : tileset.tileImages)
            dependencies.insert(image.path);
    }
    for (const std::string &dependency : dependencies) {
        if (!firstDependency)
            out << ',';
        firstDependency = false;
        out << jsonEscape(dependency);
    }
    out << "]}}";
    return out.str();
}

/// @brief Import and validate a Tiled map as a SceneDocument product.
/// @details Parses the source into an intermediate model, serializes it through
///          the SceneDocument schema, and rejects any diagnostics produced by
///          that second validation layer.
/// @param path Filesystem or logical asset root path.
/// @param assetMode Select logical asset resolution when true.
/// @return Product containing an owned scene on success or error text on
///         failure, plus the normalized model for render construction.
ImportProduct importTiled(const std::string &path, bool assetMode) {
    ImportProduct product;
    ImportParser parser(assetMode);
    if (!parser.parse(path, product.document, product.error))
        return product;
    std::string json = toSceneJson(product.document);
    rt_string source = makeString(json);
    product.scene = rt_game_scene_load_json(source);
    rt_string_unref(source);
    if (!product.scene) {
        product.error = "failed to allocate imported SceneDocument";
        return product;
    }
    if (rt_game_scene_has_errors(product.scene)) {
        rt_string message = rt_game_scene_last_error(product.scene);
        product.error = "imported Tiled map is not a valid SceneDocument";
        std::string detail = toStd(message);
        rt_str_release_maybe(message);
        if (!detail.empty())
            product.error += ": " + detail;
        releaseObject(product.scene);
        product.scene = nullptr;
    }
    return product;
}

/// @brief Create an owned error Result for a Tiled import failure.
/// @param error Error text; empty input receives a generic fallback.
/// @return Owned `Result.ErrStr`.
void *makeErrorResult(const std::string &error) {
    rt_string message = makeString(error.empty() ? "Tiled import failed" : error);
    void *result = rt_result_err_str(message);
    rt_string_unref(message);
    return result;
}

/// @brief Wrap an owned runtime object in a success Result.
/// @param value Owned payload reference transferred into the Result.
/// @return Owned `Result.Ok`.
void *makeOkResult(void *value) {
    void *result = rt_result_ok(value);
    releaseObject(value);
    return result;
}

/// @brief Decoded source rectangle for one canonical tile.
struct TiledSourceTile {
    void *pixels{nullptr};
    int64_t sourceX{0};
    int64_t sourceY{0};
    int64_t width{0};
    int64_t height{0};
    uint32_t transparentColor{0};
    bool hasTransparentColor{false};
};

/// @brief Root-image grid metadata for one tileset.
struct TiledAtlasInfo {
    void *rootPixels{nullptr};
    int64_t columns{0};
    int64_t rows{0};
};

/// @brief Pixel atlas and layout values produced for a render Tilemap.
struct ComposedTiledAtlas {
    void *pixels{nullptr};
    int64_t frameWidth{0};
    int64_t frameHeight{0};
    int64_t drawOffsetX{0};
    int64_t drawOffsetY{0};
    int64_t columns{0};
    int64_t tileCount{0};
};

/// @brief Release every owned Pixels value in an image cache.
/// @param cache Cache to release and clear.
void releasePixelCache(std::map<std::string, void *> &cache) {
    for (const auto &[_, pixels] : cache)
        releaseObject(pixels);
    cache.clear();
}

/// @brief Load and cache one bounded Tiled artwork image.
/// @details Uses asset or filesystem Pixels loading, verifies dimensions, and
///          charges decoded RGBA bytes against the 256 MiB source-image budget.
/// @param path Resolved image path.
/// @param assetMode Select asset loading when true.
/// @param cache Owned cache updated on a first successful load.
/// @param sourceBytes Aggregate decoded-byte counter updated on success.
/// @param error Human-readable load, type, dimension, or budget failure.
/// @return Borrowed cached Pixels handle, or `nullptr` on failure.
void *loadTiledPixels(const std::string &path,
                      bool assetMode,
                      std::map<std::string, void *> &cache,
                      uint64_t &sourceBytes,
                      std::string &error) {
    auto found = cache.find(path);
    if (found != cache.end())
        return found->second;
    rt_string runtimePath = makeString(path);
    void *pixels = assetMode ? rt_asset_load(runtimePath) : rt_pixels_load(runtimePath);
    rt_string_unref(runtimePath);
    if (!pixels || rt_obj_class_id(pixels) != RT_PIXELS_CLASS_ID) {
        releaseObject(pixels);
        error = "cannot load Tiled source image as Pixels: " + path;
        return nullptr;
    }
    int64_t width = rt_pixels_width(pixels);
    int64_t height = rt_pixels_height(pixels);
    if (width <= 0 || height <= 0 || width > INT64_MAX / height) {
        releaseObject(pixels);
        error = "Tiled source image has invalid dimensions: " + path;
        return nullptr;
    }
    uint64_t count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (count > (UINT64_C(256) * 1024u * 1024u - sourceBytes) / 4u) {
        releaseObject(pixels);
        error = "Tiled decoded source images exceed 256 MiB";
        return nullptr;
    }
    sourceBytes += count * 4u;
    cache[path] = pixels;
    return pixels;
}

/// @brief Compute transformed artwork bounds for a canonical tile.
/// @param tile Canonical transform flags.
/// @param sourceWidth Source image width.
/// @param sourceHeight Source image height.
/// @param hexagonal Whether flags encode hex rotations instead of diagonal
///        reflection.
/// @param width Transformed width written on return.
/// @param height Transformed height written on return.
void transformedTileDimensions(const CanonicalTile &tile,
                               int64_t sourceWidth,
                               int64_t sourceHeight,
                               bool hexagonal,
                               int64_t &width,
                               int64_t &height) {
    width = sourceWidth;
    height = sourceHeight;
    if (!hexagonal && (tile.transformFlags & kTiledDiagonalOrHex60Flag) != 0u) {
        width = sourceHeight;
        height = sourceWidth;
        return;
    }
    if (hexagonal) {
        int degrees = 0;
        if ((tile.transformFlags & kTiledDiagonalOrHex60Flag) != 0u)
            degrees += 60;
        if ((tile.transformFlags & kTiledHex120Flag) != 0u)
            degrees += 120;
        if (degrees != 0 && degrees != 180) {
            double radians = static_cast<double>(degrees) * 3.14159265358979323846 / 180.0;
            width = std::max<int64_t>(
                1,
                static_cast<int64_t>(std::ceil(std::fabs(std::cos(radians)) * sourceWidth +
                                               std::fabs(std::sin(radians)) * sourceHeight)));
            height = std::max<int64_t>(
                1,
                static_cast<int64_t>(std::ceil(std::fabs(std::sin(radians)) * sourceWidth +
                                               std::fabs(std::cos(radians)) * sourceHeight)));
        }
    }
}

/// @brief Position transformed tile artwork relative to its map cell.
/// @param tileset Owning tileset offsets.
/// @param mapTileHeight Map cell height.
/// @param sourceWidth Source artwork width.
/// @param sourceHeight Source artwork height.
/// @param transformedWidth Transformed artwork width.
/// @param transformedHeight Transformed artwork height.
/// @param relativeX Relative x position written on success.
/// @param relativeY Relative y position written on success.
/// @param error Human-readable overflow failure.
/// @return `true` when placement and far edges fit in int64.
bool tileArtworkPosition(const Tileset &tileset,
                         int64_t mapTileHeight,
                         int64_t sourceWidth,
                         int64_t sourceHeight,
                         int64_t transformedWidth,
                         int64_t transformedHeight,
                         int64_t &relativeX,
                         int64_t &relativeY,
                         std::string &error) {
    long double x =
        static_cast<long double>(tileset.tileOffsetX) + (sourceWidth - transformedWidth) / 2;
    long double y = static_cast<long double>(mapTileHeight) - sourceHeight + tileset.tileOffsetY +
                    (sourceHeight - transformedHeight) / 2;
    if (x < static_cast<long double>(INT64_MIN) || x > static_cast<long double>(INT64_MAX) ||
        y < static_cast<long double>(INT64_MIN) || y > static_cast<long double>(INT64_MAX) ||
        x + transformedWidth > static_cast<long double>(INT64_MAX) ||
        y + transformedHeight > static_cast<long double>(INT64_MAX)) {
        error = "Tiled tile artwork placement overflows";
        return false;
    }
    relativeX = static_cast<int64_t>(x);
    relativeY = static_cast<int64_t>(y);
    return true;
}

/// @brief Apply color-key transparency, ARGB tint, and layer opacity.
/// @param pixel Source pixel in runtime 0xRRGGBBAA order.
/// @param tile Canonical tint and 16-bit opacity.
/// @param source Optional transparent-color metadata.
/// @return Transformed 0xRRGGBBAA pixel, or zero for a color-key match.
uint32_t applyCanonicalColor(uint32_t pixel,
                             const CanonicalTile &tile,
                             const TiledSourceTile &source) {
    if (source.hasTransparentColor &&
        ((pixel >> 8u) & UINT32_C(0x00FFFFFF)) == (source.transparentColor & UINT32_C(0x00FFFFFF)))
        return 0u;
    uint32_t tintA = (tile.tint >> 24u) & 0xFFu;
    uint32_t tintR = (tile.tint >> 16u) & 0xFFu;
    uint32_t tintG = (tile.tint >> 8u) & 0xFFu;
    uint32_t tintB = tile.tint & 0xFFu;
    uint32_t red = (((pixel >> 24u) & 0xFFu) * tintR + 127u) / 255u;
    uint32_t green = (((pixel >> 16u) & 0xFFu) * tintG + 127u) / 255u;
    uint32_t blue = (((pixel >> 8u) & 0xFFu) * tintB + 127u) / 255u;
    uint32_t alpha = ((pixel & 0xFFu) * tintA + 127u) / 255u;
    alpha = (alpha * static_cast<uint32_t>(tile.opacity) + UINT32_C(32767)) / UINT32_C(65535);
    return (red << 24u) | (green << 16u) | (blue << 8u) | alpha;
}

/// @brief Rasterize one transformed canonical tile into the composed atlas.
/// @details Orthogonal variants use exact reflections/diagonal mapping;
///          hexagonal variants use inverse nearest-neighbor rotation plus
///          optional reflections.
/// @param atlas Borrowed destination Pixels.
/// @param destinationX Atlas destination x.
/// @param destinationY Atlas destination y.
/// @param source Decoded source rectangle.
/// @param tile Canonical transform/color metadata.
/// @param hexagonal Select hex rotation semantics.
/// @param transformedWidth Destination artwork width.
/// @param transformedHeight Destination artwork height.
void writeCanonicalTile(void *atlas,
                        int64_t destinationX,
                        int64_t destinationY,
                        const TiledSourceTile &source,
                        const CanonicalTile &tile,
                        bool hexagonal,
                        int64_t transformedWidth,
                        int64_t transformedHeight) {
    bool horizontal = (tile.transformFlags & kTiledHorizontalFlag) != 0u;
    bool vertical = (tile.transformFlags & kTiledVerticalFlag) != 0u;
    if (!hexagonal) {
        bool diagonal = (tile.transformFlags & kTiledDiagonalOrHex60Flag) != 0u;
        for (int64_t sourceY = 0; sourceY < source.height; ++sourceY) {
            for (int64_t sourceX = 0; sourceX < source.width; ++sourceX) {
                int64_t x = sourceX;
                int64_t y = sourceY;
                if (diagonal) {
                    x = source.height - 1 - sourceY;
                    y = source.width - 1 - sourceX;
                }
                if (horizontal)
                    x = transformedWidth - 1 - x;
                if (vertical)
                    y = transformedHeight - 1 - y;
                uint32_t pixel = static_cast<uint32_t>(rt_pixels_get(
                    source.pixels, source.sourceX + sourceX, source.sourceY + sourceY));
                rt_pixels_set(atlas,
                              destinationX + x,
                              destinationY + y,
                              static_cast<int64_t>(applyCanonicalColor(pixel, tile, source)));
            }
        }
        return;
    }

    int degrees = 0;
    if ((tile.transformFlags & kTiledDiagonalOrHex60Flag) != 0u)
        degrees += 60;
    if ((tile.transformFlags & kTiledHex120Flag) != 0u)
        degrees += 120;
    double radians = static_cast<double>(degrees) * 3.14159265358979323846 / 180.0;
    double cosine = std::cos(radians);
    double sine = std::sin(radians);
    double sourceCenterX = (static_cast<double>(source.width) - 1.0) * 0.5;
    double sourceCenterY = (static_cast<double>(source.height) - 1.0) * 0.5;
    double targetCenterX = (static_cast<double>(transformedWidth) - 1.0) * 0.5;
    double targetCenterY = (static_cast<double>(transformedHeight) - 1.0) * 0.5;
    for (int64_t targetY = 0; targetY < transformedHeight; ++targetY) {
        for (int64_t targetX = 0; targetX < transformedWidth; ++targetX) {
            int64_t flippedX = horizontal ? transformedWidth - 1 - targetX : targetX;
            int64_t flippedY = vertical ? transformedHeight - 1 - targetY : targetY;
            double centeredX = static_cast<double>(flippedX) - targetCenterX;
            double centeredY = static_cast<double>(flippedY) - targetCenterY;
            double sourceX = cosine * centeredX + sine * centeredY + sourceCenterX;
            double sourceY = -sine * centeredX + cosine * centeredY + sourceCenterY;
            int64_t nearestX = 0;
            int64_t nearestY = 0;
            if (!roundNearestTiesAway(sourceX, nearestX) ||
                !roundNearestTiesAway(sourceY, nearestY) || nearestX < 0 || nearestY < 0 ||
                nearestX >= source.width || nearestY >= source.height)
                continue;
            uint32_t pixel = static_cast<uint32_t>(
                rt_pixels_get(source.pixels, source.sourceX + nearestX, source.sourceY + nearestY));
            rt_pixels_set(atlas,
                          destinationX + targetX,
                          destinationY + targetY,
                          static_cast<int64_t>(applyCanonicalColor(pixel, tile, source)));
        }
    }
}

/// @brief Decode reachable artwork and compose a canonical runtime atlas.
/// @details Loads only reachable root/per-tile images, validates declared
///          dimensions, derives common frame extents, allocates a bounded
///          near-square atlas, and rasterizes every available canonical variant.
/// @param document Finalized map model.
/// @param assetMode Select asset-backed image loading when true.
/// @param result Atlas Pixels and layout values written on success.
/// @param error Human-readable image, layout, allocation, or budget failure.
/// @return `true` when the atlas is complete.
bool composeTiledAtlas(const MapDocument &document,
                       bool assetMode,
                       ComposedTiledAtlas &result,
                       std::string &error) {
    std::map<std::string, void *> cache;
    uint64_t sourceBytes = 0u;
    std::vector<TiledAtlasInfo> atlasInfo(document.tilesets.size());
    std::vector<bool> rootImageNeeded(document.tilesets.size(), false);
    std::vector<std::set<int64_t>> tileImagesNeeded(document.tilesets.size());
    for (size_t canonicalIndex = 1u; canonicalIndex < document.canonicalTiles.size();
         ++canonicalIndex) {
        const CanonicalTile &tile = document.canonicalTiles[canonicalIndex];
        if (!tile.reachable || tile.tileset < 0 ||
            static_cast<size_t>(tile.tileset) >= document.tilesets.size())
            continue;
        const Tileset &tileset = document.tilesets[static_cast<size_t>(tile.tileset)];
        if (tileset.tileImages.find(tile.localId) != tileset.tileImages.end())
            tileImagesNeeded[static_cast<size_t>(tile.tileset)].insert(tile.localId);
        else
            rootImageNeeded[static_cast<size_t>(tile.tileset)] = true;
    }
    for (size_t index = 0; index < document.tilesets.size(); ++index) {
        const Tileset &tileset = document.tilesets[index];
        TiledAtlasInfo &info = atlasInfo[index];
        if (!tileset.image.empty() && rootImageNeeded[index]) {
            info.rootPixels = loadTiledPixels(tileset.image, assetMode, cache, sourceBytes, error);
            if (!info.rootPixels) {
                releasePixelCache(cache);
                return false;
            }
            int64_t width = rt_pixels_width(info.rootPixels);
            int64_t height = rt_pixels_height(info.rootPixels);
            if ((tileset.imageWidth > 0 && tileset.imageWidth != width) ||
                (tileset.imageHeight > 0 && tileset.imageHeight != height) ||
                width < tileset.margin * 2 + tileset.tileWidth ||
                height < tileset.margin * 2 + tileset.tileHeight) {
                error =
                    "Tiled root image dimensions do not match tileset metadata: " + tileset.image;
                releasePixelCache(cache);
                return false;
            }
            int64_t derivedColumns = (width - tileset.margin * 2 + tileset.spacing) /
                                     (tileset.tileWidth + tileset.spacing);
            int64_t derivedRows = (height - tileset.margin * 2 + tileset.spacing) /
                                  (tileset.tileHeight + tileset.spacing);
            info.columns = tileset.columns > 0 ? tileset.columns : derivedColumns;
            info.rows = derivedRows;
            if (info.columns <= 0 || info.columns > derivedColumns || info.rows <= 0) {
                error = "Tiled tileset columns do not fit its root image";
                releasePixelCache(cache);
                return false;
            }
        }
        for (const auto &[localId, image] : tileset.tileImages) {
            if (tileImagesNeeded[index].find(localId) == tileImagesNeeded[index].end())
                continue;
            void *pixels = loadTiledPixels(image.path, assetMode, cache, sourceBytes, error);
            if (!pixels || (image.width > 0 && image.width != rt_pixels_width(pixels)) ||
                (image.height > 0 && image.height != rt_pixels_height(pixels))) {
                if (error.empty())
                    error = "Tiled per-tile image dimensions do not match metadata: " + image.path;
                releasePixelCache(cache);
                return false;
            }
        }
    }

    std::vector<TiledSourceTile> sources(document.canonicalTiles.size());
    int64_t minimumX = 0;
    int64_t minimumY = 0;
    int64_t maximumX = 0;
    int64_t maximumY = 0;
    bool haveBounds = false;
    for (size_t canonicalIndex = 1u; canonicalIndex < document.canonicalTiles.size();
         ++canonicalIndex) {
        const CanonicalTile &tile = document.canonicalTiles[canonicalIndex];
        const Tileset &tileset = document.tilesets[static_cast<size_t>(tile.tileset)];
        TiledSourceTile &source = sources[canonicalIndex];
        auto image = tileset.tileImages.find(tile.localId);
        if (image != tileset.tileImages.end()) {
            auto cached = cache.find(image->second.path);
            if (cached == cache.end()) {
                if (tile.reachable) {
                    error = "reachable Tiled image-collection tile has no decoded image";
                    releasePixelCache(cache);
                    return false;
                }
                continue;
            }
            source.pixels = cached->second;
            source.width = rt_pixels_width(source.pixels);
            source.height = rt_pixels_height(source.pixels);
            source.transparentColor = image->second.transparentColor;
            source.hasTransparentColor = image->second.hasTransparentColor;
        } else if (atlasInfo[static_cast<size_t>(tile.tileset)].rootPixels &&
                   tile.localId < atlasInfo[static_cast<size_t>(tile.tileset)].columns *
                                      atlasInfo[static_cast<size_t>(tile.tileset)].rows) {
            const TiledAtlasInfo &info = atlasInfo[static_cast<size_t>(tile.tileset)];
            source.pixels = info.rootPixels;
            source.width = tileset.tileWidth;
            source.height = tileset.tileHeight;
            source.sourceX = tileset.margin +
                             (tile.localId % info.columns) * (tileset.tileWidth + tileset.spacing);
            source.sourceY = tileset.margin +
                             (tile.localId / info.columns) * (tileset.tileHeight + tileset.spacing);
            source.transparentColor = tileset.transparentColor;
            source.hasTransparentColor = tileset.hasTransparentColor;
        } else if (tile.reachable) {
            error = "reachable Tiled image-collection tile has no image";
            releasePixelCache(cache);
            return false;
        } else {
            continue;
        }
        int64_t transformedWidth = 0;
        int64_t transformedHeight = 0;
        transformedTileDimensions(tile,
                                  source.width,
                                  source.height,
                                  document.orientation == "hexagonal",
                                  transformedWidth,
                                  transformedHeight);
        int64_t relativeX = 0;
        int64_t relativeY = 0;
        if (!tileArtworkPosition(tileset,
                                 document.tileHeight,
                                 source.width,
                                 source.height,
                                 transformedWidth,
                                 transformedHeight,
                                 relativeX,
                                 relativeY,
                                 error)) {
            releasePixelCache(cache);
            return false;
        }
        if (!haveBounds) {
            minimumX = relativeX;
            minimumY = relativeY;
            maximumX = relativeX + transformedWidth;
            maximumY = relativeY + transformedHeight;
            haveBounds = true;
        } else {
            minimumX = std::min(minimumX, relativeX);
            minimumY = std::min(minimumY, relativeY);
            maximumX = std::max(maximumX, relativeX + transformedWidth);
            maximumY = std::max(maximumY, relativeY + transformedHeight);
        }
    }
    if (!haveBounds) {
        minimumX = 0;
        minimumY = 0;
        maximumX = document.tileWidth;
        maximumY = document.tileHeight;
    }
    long double frameWidth =
        static_cast<long double>(maximumX) - static_cast<long double>(minimumX);
    long double frameHeight =
        static_cast<long double>(maximumY) - static_cast<long double>(minimumY);
    if (frameWidth <= 0.0L || frameHeight <= 0.0L ||
        frameWidth > static_cast<long double>(INT64_MAX) ||
        frameHeight > static_cast<long double>(INT64_MAX)) {
        error = "composed Tiled source-frame bounds overflow";
        releasePixelCache(cache);
        return false;
    }
    result.frameWidth = static_cast<int64_t>(frameWidth);
    result.frameHeight = static_cast<int64_t>(frameHeight);
    result.drawOffsetX = minimumX;
    result.drawOffsetY = minimumY;
    result.tileCount = static_cast<int64_t>(document.canonicalTiles.size() - 1u);
    result.columns = static_cast<int64_t>(
        std::ceil(std::sqrt(static_cast<double>(std::max<int64_t>(result.tileCount, 1)))));
    int64_t rows = (result.tileCount + result.columns - 1) / result.columns;
    if (result.frameWidth <= 0 || result.frameHeight <= 0 ||
        result.columns > INT64_MAX / result.frameWidth || rows > INT64_MAX / result.frameHeight) {
        error = "composed Tiled atlas dimensions overflow";
        releasePixelCache(cache);
        return false;
    }
    int64_t atlasWidth = result.columns * result.frameWidth;
    int64_t atlasHeight = rows * result.frameHeight;
    if (atlasWidth <= 0 || atlasHeight <= 0 || atlasWidth > INT64_MAX / atlasHeight ||
        static_cast<uint64_t>(atlasWidth) * static_cast<uint64_t>(atlasHeight) >
            UINT64_C(256) * 1024u * 1024u / 4u) {
        error = "composed Tiled atlas exceeds 256 MiB";
        releasePixelCache(cache);
        return false;
    }
    result.pixels = rt_pixels_new(atlasWidth, atlasHeight);
    if (!result.pixels) {
        error = "cannot allocate composed Tiled atlas Pixels";
        releasePixelCache(cache);
        return false;
    }
    for (size_t canonicalIndex = 1u; canonicalIndex < document.canonicalTiles.size();
         ++canonicalIndex) {
        const TiledSourceTile &source = sources[canonicalIndex];
        if (!source.pixels)
            continue;
        const CanonicalTile &tile = document.canonicalTiles[canonicalIndex];
        const Tileset &tileset = document.tilesets[static_cast<size_t>(tile.tileset)];
        int64_t transformedWidth = 0;
        int64_t transformedHeight = 0;
        transformedTileDimensions(tile,
                                  source.width,
                                  source.height,
                                  document.orientation == "hexagonal",
                                  transformedWidth,
                                  transformedHeight);
        int64_t relativeX = 0;
        int64_t relativeY = 0;
        if (!tileArtworkPosition(tileset,
                                 document.tileHeight,
                                 source.width,
                                 source.height,
                                 transformedWidth,
                                 transformedHeight,
                                 relativeX,
                                 relativeY,
                                 error)) {
            releaseObject(result.pixels);
            result.pixels = nullptr;
            releasePixelCache(cache);
            return false;
        }
        int64_t frame = static_cast<int64_t>(canonicalIndex - 1u);
        int64_t destinationX = (frame % result.columns) * result.frameWidth + relativeX - minimumX;
        int64_t destinationY = (frame / result.columns) * result.frameHeight + relativeY - minimumY;
        writeCanonicalTile(result.pixels,
                           destinationX,
                           destinationY,
                           source,
                           tile,
                           document.orientation == "hexagonal",
                           transformedWidth,
                           transformedHeight);
    }
    releasePixelCache(cache);
    return true;
}

/// @brief Build and configure a render-ready Tilemap from an import product.
/// @details Creates the editable Tilemap, composes its atlas, maps projection
///          and render-order enums, configures import layout/layers, transfers
///          the tileset, and applies the canonical tile count.
/// @param product Successful import product with owned SceneDocument.
/// @param assetMode Select asset-backed image loading.
/// @param error Human-readable construction/configuration failure.
/// @return Owned Tilemap handle, or `nullptr` on failure.
void *buildRenderTilemap(ImportProduct &product, bool assetMode, std::string &error) {
    if (!product.scene) {
        error =
            product.error.empty() ? "Tiled import did not produce a SceneDocument" : product.error;
        return nullptr;
    }
    void *tilemap = rt_game_scene_build_tilemap(product.scene);
    if (!tilemap) {
        error = "cannot build Tilemap from imported SceneDocument";
        return nullptr;
    }
    ComposedTiledAtlas atlas;
    if (!composeTiledAtlas(product.document, assetMode, atlas, error)) {
        releaseObject(tilemap);
        return nullptr;
    }
    int64_t orientation = RT_TILEMAP_IMPORT_ORTHOGONAL;
    if (product.document.orientation == "isometric")
        orientation = RT_TILEMAP_IMPORT_ISOMETRIC;
    else if (product.document.orientation == "staggered")
        orientation = RT_TILEMAP_IMPORT_STAGGERED;
    else if (product.document.orientation == "hexagonal")
        orientation = RT_TILEMAP_IMPORT_HEXAGONAL;
    else if (product.document.orientation == "oblique")
        orientation = RT_TILEMAP_IMPORT_OBLIQUE;
    int64_t renderOrder = RT_TILEMAP_IMPORT_RIGHT_DOWN;
    if (product.document.renderOrder == "right-up")
        renderOrder = RT_TILEMAP_IMPORT_RIGHT_UP;
    else if (product.document.renderOrder == "left-down")
        renderOrder = RT_TILEMAP_IMPORT_LEFT_DOWN;
    else if (product.document.renderOrder == "left-up")
        renderOrder = RT_TILEMAP_IMPORT_LEFT_UP;
    if (!rt_tilemap_configure_import_layout(tilemap,
                                            orientation,
                                            product.document.originTileX,
                                            product.document.originTileY,
                                            atlas.frameWidth,
                                            atlas.frameHeight,
                                            atlas.drawOffsetX,
                                            atlas.drawOffsetY,
                                            renderOrder,
                                            product.document.staggerAxis == "x" ? 0 : 1,
                                            product.document.staggerIndex == "even" ? 1 : 0,
                                            product.document.hexSideLength,
                                            product.document.skewX,
                                            product.document.skewY,
                                            product.document.parallaxOriginX,
                                            product.document.parallaxOriginY,
                                            product.document.projectionHeight)) {
        releaseObject(atlas.pixels);
        releaseObject(tilemap);
        error = "cannot configure composed Tiled atlas layout";
        return nullptr;
    }
    rt_tilemap_set_tileset(tilemap, atlas.pixels);
    if (!rt_tilemap_set_import_tile_count(tilemap, atlas.tileCount)) {
        releaseObject(atlas.pixels);
        releaseObject(tilemap);
        error = "cannot apply composed Tiled atlas tile count";
        return nullptr;
    }
    for (size_t layerIndex = 0; layerIndex < product.document.layers.size(); ++layerIndex) {
        const Layer &layer = product.document.layers[layerIndex];
        rt_tilemap_configure_import_layer(tilemap,
                                          static_cast<int64_t>(layerIndex),
                                          layer.offsetX,
                                          layer.offsetY,
                                          layer.parallaxX,
                                          layer.parallaxY);
    }
    releaseObject(atlas.pixels);
    return tilemap;
}

/// @brief Import a path directly as a nullable SceneDocument.
/// @param path Borrowed runtime path string.
/// @param assetMode Select asset dependency semantics.
/// @return Owned SceneDocument, or `nullptr` on import failure.
void *sceneImportRaw(rt_string path, bool assetMode) {
    ImportProduct product = importTiled(toStd(path), assetMode);
    return product.scene;
}

/// @brief Import a path as a Result-wrapped SceneDocument.
/// @param path Borrowed runtime path string.
/// @param assetMode Select asset dependency semantics.
/// @return Owned Result containing a SceneDocument or error string.
void *sceneImportResult(rt_string path, bool assetMode) {
    ImportProduct product = importTiled(toStd(path), assetMode);
    if (!product.scene)
        return makeErrorResult(product.error);
    return makeOkResult(product.scene);
}

/// @brief Validate a loader receiver and import a render-ready Tilemap.
/// @param loader Borrowed TiledMapLoader handle.
/// @param path Borrowed runtime path string.
/// @param assetMode Select asset dependency/image semantics.
/// @return Owned Tilemap, or `nullptr` for an invalid receiver/import/build
///         failure.
void *loaderImportRaw(void *loader, rt_string path, bool assetMode) {
    if (!loader || rt_obj_class_id(loader) != kTiledMapLoaderClassId)
        return nullptr;
    ImportProduct product = importTiled(toStd(path), assetMode);
    if (!product.scene)
        return nullptr;
    std::string error;
    void *tilemap = buildRenderTilemap(product, assetMode, error);
    releaseObject(product.scene);
    product.scene = nullptr;
    return tilemap;
}

/// @brief Validate a loader receiver and import a Result-wrapped Tilemap.
/// @param loader Borrowed TiledMapLoader handle.
/// @param path Borrowed runtime path string.
/// @param assetMode Select asset dependency/image semantics.
/// @return Owned Result containing a Tilemap or error string.
void *loaderImportResult(void *loader, rt_string path, bool assetMode) {
    if (!loader || rt_obj_class_id(loader) != kTiledMapLoaderClassId)
        return makeErrorResult("TiledMapLoader.Load requires a TiledMapLoader receiver");
    ImportProduct product = importTiled(toStd(path), assetMode);
    if (!product.scene)
        return makeErrorResult(product.error);
    std::string error;
    void *tilemap = buildRenderTilemap(product, assetMode, error);
    releaseObject(product.scene);
    product.scene = nullptr;
    if (!tilemap)
        return makeErrorResult(error);
    return makeOkResult(tilemap);
}

} // namespace

extern "C" {

/// @brief Import a filesystem Tiled JSON/TMX map as a SceneDocument.
/// @param path Borrowed filesystem root path.
/// @return Owned SceneDocument, or `nullptr` on any import failure.
void *rt_game_scene_import_tiled(rt_string path) {
    try {
        return sceneImportRaw(path, false);
    } catch (...) {
        return nullptr;
    }
}

/// @brief Import a filesystem Tiled map with Result semantics.
/// @param path Borrowed filesystem root path.
/// @return Owned Result containing a SceneDocument or error string.
void *rt_game_scene_import_tiled_result(rt_string path) {
    try {
        return sceneImportResult(path, false);
    } catch (...) {
        return makeErrorResult("Tiled filesystem import failed unexpectedly");
    }
}

/// @brief Import an asset-backed Tiled JSON/TMX map as a SceneDocument.
/// @param path Borrowed logical asset root path.
/// @return Owned SceneDocument, or `nullptr` on any import failure.
void *rt_game_scene_import_tiled_asset(rt_string path) {
    try {
        return sceneImportRaw(path, true);
    } catch (...) {
        return nullptr;
    }
}

/// @brief Import an asset-backed Tiled map with Result semantics.
/// @param path Borrowed logical asset root path.
/// @return Owned Result containing a SceneDocument or error string.
void *rt_game_scene_import_tiled_asset_result(rt_string path) {
    try {
        return sceneImportResult(path, true);
    } catch (...) {
        return makeErrorResult("Tiled asset import failed unexpectedly");
    }
}

/// @brief Load a filesystem Tiled map as a render-ready Tilemap.
/// @param loader Borrowed TiledMapLoader receiver.
/// @param path Borrowed filesystem root path.
/// @return Owned Tilemap, or `nullptr` on receiver/import/render failure.
void *rt_tiledmaploader_load(void *loader, rt_string path) {
    try {
        return loaderImportRaw(loader, path, false);
    } catch (...) {
        return nullptr;
    }
}

/// @brief Load a filesystem Tiled map as a Result-wrapped Tilemap.
/// @param loader Borrowed TiledMapLoader receiver.
/// @param path Borrowed filesystem root path.
/// @return Owned Result containing a Tilemap or error string.
void *rt_tiledmaploader_load_result(void *loader, rt_string path) {
    try {
        return loaderImportResult(loader, path, false);
    } catch (...) {
        return makeErrorResult("TiledMapLoader filesystem load failed unexpectedly");
    }
}

/// @brief Load an asset-backed Tiled map as a render-ready Tilemap.
/// @param loader Borrowed TiledMapLoader receiver.
/// @param path Borrowed logical asset root path.
/// @return Owned Tilemap, or `nullptr` on receiver/import/render failure.
void *rt_tiledmaploader_load_asset(void *loader, rt_string path) {
    try {
        return loaderImportRaw(loader, path, true);
    } catch (...) {
        return nullptr;
    }
}

/// @brief Load an asset-backed Tiled map as a Result-wrapped Tilemap.
/// @param loader Borrowed TiledMapLoader receiver.
/// @param path Borrowed logical asset root path.
/// @return Owned Result containing a Tilemap or error string.
void *rt_tiledmaploader_load_asset_result(void *loader, rt_string path) {
    try {
        return loaderImportResult(loader, path, true);
    } catch (...) {
        return makeErrorResult("TiledMapLoader asset load failed unexpectedly");
    }
}

} // extern "C"
