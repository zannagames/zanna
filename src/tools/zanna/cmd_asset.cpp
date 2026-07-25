//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/zanna/cmd_asset.cpp
// Purpose: `zanna asset` subcommands — offline 3D asset conditioning. `bake`
//   loads a glTF/GLB/FBX/OBJ/STL model through the full runtime import
//   pipeline (including the compressed-codec decoders and import options),
//   optionally generates LOD chains, saves the complete SceneAsset as the
//   versioned .vscn baked form, then reloads it to report fidelity losses.
//   `validate` loads an asset and prints the AssetDiagnostics3D import report.
// Key invariants:
//   - Requires a graphics-enabled runtime build; other builds get a clear
//     diagnostic instead of unresolved behavior.
//   - Bake reports compare public SceneAsset resource counts plus every persisted material
//     texture reference, exact source span, decoded surface, native metadata, and alias.
//   - `--json` writes exactly one v1 report object to stdout and keeps stderr
//     empty on success. Human mode retains the historical `baked` line and
//     writes one stable-code warning per detected resource class reduction.
//   - Exit codes: 0 success, 1 usage error, 2 load/bake failure.
// Ownership/Lifetime:
//   - Runtime objects are released through the runtime's release-check
//     protocol before exit.
// Links: src/runtime/graphics/3d/render/rt_model3d.h,
//   src/runtime/graphics/3d/scene/rt_scene3d.h
//
//===----------------------------------------------------------------------===//
#include "cmd_asset.hpp"

#ifdef ZANNA_ENABLE_GRAPHICS
#include "rt_textureasset3d.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef ZANNA_ENABLE_GRAPHICS
extern "C" {
rt_string rt_const_cstr(const char *text);
const char *rt_string_cstr(rt_string s);
void *rt_model3d_load_with_options_ex(rt_string path, rt_string options);
int64_t rt_model3d_generate_lods(void *model, int64_t levels, double ratio);
int64_t rt_model3d_save(void *model, rt_string path);
int64_t rt_model3d_get_mesh_count(void *model);
int64_t rt_model3d_get_material_count(void *model);
void *rt_model3d_get_material(void *model, int64_t index);
int64_t rt_model3d_get_skeleton_count(void *model);
int64_t rt_model3d_get_animation_count(void *model);
int64_t rt_model3d_get_node_animation_count(void *model);
int64_t rt_model3d_get_morph_target_count(void *model);
int64_t rt_model3d_get_morph_shape_count(void *model);
int64_t rt_model3d_get_node_count(void *model);
int64_t rt_model3d_get_scene_count(void *model);
int64_t rt_model3d_get_camera_count(void *model, int64_t scene_index);
int64_t rt_model3d_get_variant_count(void *model);
void *rt_material3d_get_persisted_texture_ref(void *material, int64_t slot);
void *rt_material3d_resolve_texture_pixels(void *texture_ref);
int64_t rt_pixels_width(void *pixels);
int64_t rt_pixels_height(void *pixels);
const uint32_t *rt_pixels_raw_buffer(void *pixels);
rt_string rt_assets3d_get_import_report(void);
int rt_obj_release_check0(void *obj);
void rt_obj_free(void *obj);
}
#endif

namespace {

#ifdef ZANNA_ENABLE_GRAPHICS

struct AssetSnapshot {
    int64_t meshes = 0;
    int64_t materials = 0;
    int64_t skeletons = 0;
    int64_t skeletalAnimations = 0;
    int64_t nodeAnimations = 0;
    int64_t morphTargets = 0;
    int64_t morphShapes = 0;
    int64_t nodes = 0;
    int64_t scenes = 0;
    int64_t cameras = 0;
    int64_t variants = 0;
};

struct FidelityLoss {
    const char *code;
    const char *resource;
    int64_t source;
    int64_t baked;
};

struct TextureDescriptor {
    void *reference = nullptr;
    void *pixels = nullptr;
    std::string referenceKind = "none";
    int64_t sourceContainerState = RT_TEXTUREASSET3D_SOURCE_CONTAINER_NONE;
    const uint8_t *sourceBytes = nullptr;
    uint64_t sourceByteCount = 0;
    std::string sourceContainer;
    int64_t width = 0;
    int64_t height = 0;
    int64_t mipCount = 0;
    std::string format = "none";
    bool compressed = false;
};

struct TextureFidelityEntry {
    int64_t material = 0;
    const char *slot = nullptr;
    std::string state;
    std::string lossCode;
    std::string sourceReferenceKind;
    std::string bakedReferenceKind;
    std::string sourceContainer;
    bool referenceKindMatch = false;
    bool sourceContainerMatch = false;
    bool dimensionsMatch = false;
    bool mipCountMatch = false;
    bool formatMatch = false;
    bool compressedMatch = false;
    bool decodedTexelsMatch = false;
    bool sharedIdentityMatch = false;
};

struct TextureFidelityReport {
    int64_t preservedSource = 0;
    int64_t preservedDecoded = 0;
    int64_t changedAfterImport = 0;
    int64_t losses = 0;
    std::vector<TextureFidelityEntry> entries;
};

std::string jsonQuote(const char *text) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.push_back('"');
    if (text) {
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
            switch (*p) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
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
                    if (*p < 0x20u) {
                        out += "\\u00";
                        out.push_back(hex[*p >> 4u]);
                        out.push_back(hex[*p & 0x0fu]);
                    } else {
                        out.push_back(static_cast<char>(*p));
                    }
                    break;
            }
        }
    }
    out.push_back('"');
    return out;
}

int64_t saturatingAddNonnegative(int64_t lhs, int64_t rhs) {
    if (rhs <= 0)
        return lhs;
    if (lhs > std::numeric_limits<int64_t>::max() - rhs)
        return std::numeric_limits<int64_t>::max();
    return lhs + rhs;
}

AssetSnapshot snapshotAsset(void *model) {
    AssetSnapshot snapshot;
    snapshot.meshes = rt_model3d_get_mesh_count(model);
    snapshot.materials = rt_model3d_get_material_count(model);
    snapshot.skeletons = rt_model3d_get_skeleton_count(model);
    snapshot.skeletalAnimations = rt_model3d_get_animation_count(model);
    snapshot.nodeAnimations = rt_model3d_get_node_animation_count(model);
    snapshot.morphTargets = rt_model3d_get_morph_target_count(model);
    snapshot.morphShapes = rt_model3d_get_morph_shape_count(model);
    snapshot.nodes = rt_model3d_get_node_count(model);
    snapshot.scenes = rt_model3d_get_scene_count(model);
    snapshot.variants = rt_model3d_get_variant_count(model);
    for (int64_t scene = 0; scene < snapshot.scenes; ++scene) {
        snapshot.cameras =
            saturatingAddNonnegative(snapshot.cameras, rt_model3d_get_camera_count(model, scene));
    }
    return snapshot;
}

TextureDescriptor describeTexture(void *reference) {
    TextureDescriptor descriptor;
    descriptor.reference = reference;
    if (!reference)
        return descriptor;

    descriptor.mipCount = rt_textureasset3d_get_mip_count(reference);
    if (descriptor.mipCount > 0) {
        descriptor.referenceKind = "texture-asset";
        descriptor.width = rt_textureasset3d_get_width(reference);
        descriptor.height = rt_textureasset3d_get_height(reference);
        descriptor.compressed = rt_textureasset3d_get_compressed(reference) != 0;
        rt_string format = rt_textureasset3d_get_format(reference);
        const char *formatText = format ? rt_string_cstr(format) : nullptr;
        descriptor.format = formatText && *formatText ? formatText : "unknown";
        descriptor.sourceContainerState = rt_textureasset3d_get_source_container_state(reference);
        if (descriptor.sourceContainerState == RT_TEXTUREASSET3D_SOURCE_CONTAINER_VALID) {
            const char *kind = nullptr;
            if (rt_textureasset3d_get_source_container(
                    reference, &descriptor.sourceBytes, &descriptor.sourceByteCount, &kind) &&
                kind) {
                descriptor.sourceContainer = kind;
            }
        }
    }

    descriptor.pixels = rt_material3d_resolve_texture_pixels(reference);
    if (descriptor.mipCount <= 0) {
        descriptor.referenceKind = descriptor.pixels == reference ? "pixels" : "render-target";
        descriptor.mipCount = descriptor.pixels ? 1 : 0;
        descriptor.width = descriptor.pixels ? rt_pixels_width(descriptor.pixels) : 0;
        descriptor.height = descriptor.pixels ? rt_pixels_height(descriptor.pixels) : 0;
        descriptor.format = descriptor.pixels ? "rgba8" : "unresolved";
    }
    return descriptor;
}

bool decodedTexelsEqual(const TextureDescriptor &source, const TextureDescriptor &baked) {
    if (!source.pixels || !baked.pixels)
        return source.pixels == baked.pixels;
    const int64_t sourceWidth = rt_pixels_width(source.pixels);
    const int64_t sourceHeight = rt_pixels_height(source.pixels);
    const int64_t bakedWidth = rt_pixels_width(baked.pixels);
    const int64_t bakedHeight = rt_pixels_height(baked.pixels);
    if (sourceWidth < 0 || sourceHeight < 0 || sourceWidth != bakedWidth ||
        sourceHeight != bakedHeight)
        return false;
    const uint64_t width = static_cast<uint64_t>(sourceWidth);
    const uint64_t height = static_cast<uint64_t>(sourceHeight);
    if (height > 0 && width > UINT64_MAX / height)
        return false;
    const uint64_t pixelCount = width * height;
    if (pixelCount > SIZE_MAX / sizeof(uint32_t))
        return false;
    if (pixelCount == 0)
        return true;
    const uint32_t *sourcePixels = rt_pixels_raw_buffer(source.pixels);
    const uint32_t *bakedPixels = rt_pixels_raw_buffer(baked.pixels);
    return sourcePixels && bakedPixels &&
           std::memcmp(
               sourcePixels, bakedPixels, static_cast<size_t>(pixelCount) * sizeof(uint32_t)) == 0;
}

std::string textureLossCode(const TextureDescriptor &source,
                            const TextureDescriptor &baked,
                            const TextureFidelityEntry &entry) {
    if (!source.reference && baked.reference)
        return "texture-reference-added";
    if (source.reference && !baked.reference)
        return "texture-reference-lost";
    if (source.sourceContainerState == RT_TEXTUREASSET3D_SOURCE_CONTAINER_CHANGED_AFTER_IMPORT &&
        baked.sourceContainerState == RT_TEXTUREASSET3D_SOURCE_CONTAINER_VALID)
        return "texture-stale-source-preserved";
    if (source.sourceContainerState == RT_TEXTUREASSET3D_SOURCE_CONTAINER_VALID &&
        !entry.sourceContainerMatch)
        return "texture-source-container-lost";
    if (!entry.decodedTexelsMatch)
        return "texture-decoded-texels-changed";
    if (!entry.dimensionsMatch)
        return "texture-dimensions-changed";
    if (!entry.referenceKindMatch)
        return "texture-reference-kind-changed";
    if (!entry.mipCountMatch)
        return "texture-mip-count-changed";
    if (!entry.formatMatch || !entry.compressedMatch)
        return "texture-native-format-changed";
    if (!entry.sharedIdentityMatch)
        return "texture-sharing-changed";
    return "texture-fidelity-mismatch";
}

TextureFidelityReport compareTextureFidelity(void *sourceModel,
                                             void *bakedModel,
                                             int64_t sourceMaterialCount,
                                             int64_t bakedMaterialCount) {
    static const char *slotNames[] = {
        "baseColor", "normal", "specular", "emissive", "metallicRoughness", "ao", "lightMap"};
    constexpr int64_t textureSlotCount =
        static_cast<int64_t>(sizeof(slotNames) / sizeof(slotNames[0]));
    TextureFidelityReport report;
    std::unordered_map<void *, void *> sourceToBaked;
    std::unordered_map<void *, void *> bakedToSource;
    const int64_t materialCount =
        sourceMaterialCount > bakedMaterialCount ? sourceMaterialCount : bakedMaterialCount;
    for (int64_t materialIndex = 0; materialIndex < materialCount; ++materialIndex) {
        void *sourceMaterial = materialIndex < sourceMaterialCount
                                   ? rt_model3d_get_material(sourceModel, materialIndex)
                                   : nullptr;
        void *bakedMaterial = materialIndex < bakedMaterialCount
                                  ? rt_model3d_get_material(bakedModel, materialIndex)
                                  : nullptr;
        for (int64_t slot = 0; slot < textureSlotCount; ++slot) {
            TextureDescriptor source = describeTexture(
                sourceMaterial ? rt_material3d_get_persisted_texture_ref(sourceMaterial, slot)
                               : nullptr);
            TextureDescriptor baked = describeTexture(
                bakedMaterial ? rt_material3d_get_persisted_texture_ref(bakedMaterial, slot)
                              : nullptr);
            if (!source.reference && !baked.reference)
                continue;

            TextureFidelityEntry entry;
            entry.material = materialIndex;
            entry.slot = slotNames[slot];
            entry.sourceReferenceKind = source.referenceKind;
            entry.bakedReferenceKind = baked.referenceKind;
            entry.sourceContainer = source.sourceContainer;
            entry.referenceKindMatch = source.referenceKind == baked.referenceKind;
            entry.dimensionsMatch = source.width == baked.width && source.height == baked.height;
            entry.mipCountMatch = source.mipCount == baked.mipCount;
            entry.formatMatch = source.format == baked.format;
            entry.compressedMatch = source.compressed == baked.compressed;
            entry.decodedTexelsMatch = decodedTexelsEqual(source, baked);
            if (source.sourceContainerState == RT_TEXTUREASSET3D_SOURCE_CONTAINER_VALID) {
                const bool byteCountsMatch = source.sourceByteCount == baked.sourceByteCount;
                entry.sourceContainerMatch =
                    baked.sourceContainerState == RT_TEXTUREASSET3D_SOURCE_CONTAINER_VALID &&
                    source.sourceContainer == baked.sourceContainer && byteCountsMatch &&
                    source.sourceBytes && baked.sourceBytes &&
                    std::memcmp(source.sourceBytes,
                                baked.sourceBytes,
                                static_cast<size_t>(source.sourceByteCount)) == 0;
            } else {
                entry.sourceContainerMatch =
                    baked.sourceContainerState == RT_TEXTUREASSET3D_SOURCE_CONTAINER_NONE;
            }

            if (source.reference && baked.reference) {
                auto sourceMapping = sourceToBaked.find(source.reference);
                auto bakedMapping = bakedToSource.find(baked.reference);
                if (sourceMapping == sourceToBaked.end() && bakedMapping == bakedToSource.end()) {
                    sourceToBaked.emplace(source.reference, baked.reference);
                    bakedToSource.emplace(baked.reference, source.reference);
                    entry.sharedIdentityMatch = true;
                } else {
                    entry.sharedIdentityMatch = sourceMapping != sourceToBaked.end() &&
                                                bakedMapping != bakedToSource.end() &&
                                                sourceMapping->second == baked.reference &&
                                                bakedMapping->second == source.reference;
                }
            }

            bool preserved = false;
            if (source.sourceContainerState == RT_TEXTUREASSET3D_SOURCE_CONTAINER_VALID) {
                preserved = entry.referenceKindMatch && entry.sourceContainerMatch &&
                            entry.dimensionsMatch && entry.mipCountMatch && entry.formatMatch &&
                            entry.compressedMatch && entry.decodedTexelsMatch &&
                            entry.sharedIdentityMatch;
                if (preserved) {
                    entry.state = "preserved-source";
                    report.preservedSource++;
                }
            } else if (source.sourceContainerState ==
                       RT_TEXTUREASSET3D_SOURCE_CONTAINER_CHANGED_AFTER_IMPORT) {
                preserved = source.reference && baked.reference && entry.sourceContainerMatch &&
                            entry.dimensionsMatch && entry.decodedTexelsMatch &&
                            entry.sharedIdentityMatch;
                if (preserved) {
                    entry.state = "changed-after-import";
                    report.changedAfterImport++;
                }
            } else {
                const bool canonicalDecodedSource =
                    source.referenceKind == "pixels" || source.referenceKind == "render-target";
                preserved =
                    source.reference && baked.reference && entry.sourceContainerMatch &&
                    entry.dimensionsMatch && entry.decodedTexelsMatch &&
                    entry.sharedIdentityMatch &&
                    (canonicalDecodedSource || (entry.referenceKindMatch && entry.mipCountMatch &&
                                                entry.formatMatch && entry.compressedMatch));
                if (preserved) {
                    entry.state = "preserved-decoded";
                    report.preservedDecoded++;
                }
            }
            if (!preserved) {
                entry.state = "mismatch";
                entry.lossCode = textureLossCode(source, baked, entry);
                report.losses++;
            }
            report.entries.push_back(std::move(entry));
        }
    }
    return report;
}

void appendLoss(std::vector<FidelityLoss> &losses,
                const char *code,
                const char *resource,
                int64_t source,
                int64_t baked) {
    if (source > baked)
        losses.push_back(FidelityLoss{code, resource, source, baked});
}

std::vector<FidelityLoss> compareSnapshots(const AssetSnapshot &source,
                                           const AssetSnapshot &baked) {
    std::vector<FidelityLoss> losses;
    appendLoss(losses, "mesh-count-reduced", "meshes", source.meshes, baked.meshes);
    appendLoss(losses, "material-count-reduced", "materials", source.materials, baked.materials);
    appendLoss(losses, "skeleton-count-reduced", "skeletons", source.skeletons, baked.skeletons);
    appendLoss(losses,
               "skeletal-animation-count-reduced",
               "skeletalAnimations",
               source.skeletalAnimations,
               baked.skeletalAnimations);
    appendLoss(losses,
               "node-animation-count-reduced",
               "nodeAnimations",
               source.nodeAnimations,
               baked.nodeAnimations);
    appendLoss(losses,
               "morph-target-count-reduced",
               "morphTargets",
               source.morphTargets,
               baked.morphTargets);
    appendLoss(
        losses, "morph-shape-count-reduced", "morphShapes", source.morphShapes, baked.morphShapes);
    appendLoss(losses, "node-count-reduced", "nodes", source.nodes, baked.nodes);
    appendLoss(losses, "scene-count-reduced", "scenes", source.scenes, baked.scenes);
    appendLoss(losses, "camera-count-reduced", "cameras", source.cameras, baked.cameras);
    appendLoss(losses, "variant-count-reduced", "variants", source.variants, baked.variants);
    return losses;
}

void appendSnapshotJson(std::string &out, const AssetSnapshot &snapshot) {
    out += "{\"meshes\":" + std::to_string(snapshot.meshes);
    out += ",\"materials\":" + std::to_string(snapshot.materials);
    out += ",\"skeletons\":" + std::to_string(snapshot.skeletons);
    out += ",\"skeletalAnimations\":" + std::to_string(snapshot.skeletalAnimations);
    out += ",\"nodeAnimations\":" + std::to_string(snapshot.nodeAnimations);
    out += ",\"morphTargets\":" + std::to_string(snapshot.morphTargets);
    out += ",\"morphShapes\":" + std::to_string(snapshot.morphShapes);
    out += ",\"nodes\":" + std::to_string(snapshot.nodes);
    out += ",\"scenes\":" + std::to_string(snapshot.scenes);
    out += ",\"cameras\":" + std::to_string(snapshot.cameras);
    out += ",\"variants\":" + std::to_string(snapshot.variants) + "}";
}

void appendTextureFidelityJson(std::string &out, const TextureFidelityReport &textureReport) {
    out += "{\"summary\":{\"preserved-source\":" + std::to_string(textureReport.preservedSource);
    out += ",\"preserved-decoded\":" + std::to_string(textureReport.preservedDecoded);
    out += ",\"changed-after-import\":" + std::to_string(textureReport.changedAfterImport);
    out += ",\"losses\":" + std::to_string(textureReport.losses) + "},\"entries\":[";
    for (size_t i = 0; i < textureReport.entries.size(); ++i) {
        const TextureFidelityEntry &entry = textureReport.entries[i];
        if (i > 0)
            out.push_back(',');
        out += "{\"material\":" + std::to_string(entry.material);
        out += ",\"slot\":" + jsonQuote(entry.slot);
        out += ",\"state\":" + jsonQuote(entry.state.c_str());
        out += ",\"lossCode\":";
        out += entry.lossCode.empty() ? "null" : jsonQuote(entry.lossCode.c_str());
        out += ",\"sourceReferenceKind\":" + jsonQuote(entry.sourceReferenceKind.c_str());
        out += ",\"bakedReferenceKind\":" + jsonQuote(entry.bakedReferenceKind.c_str());
        out += ",\"sourceContainer\":";
        out += entry.sourceContainer.empty() ? "null" : jsonQuote(entry.sourceContainer.c_str());
        out += ",\"referenceKindMatch\":";
        out += entry.referenceKindMatch ? "true" : "false";
        out += ",\"sourceContainerMatch\":";
        out += entry.sourceContainerMatch ? "true" : "false";
        out += ",\"dimensionsMatch\":";
        out += entry.dimensionsMatch ? "true" : "false";
        out += ",\"mipCountMatch\":";
        out += entry.mipCountMatch ? "true" : "false";
        out += ",\"formatMatch\":";
        out += entry.formatMatch ? "true" : "false";
        out += ",\"compressedMatch\":";
        out += entry.compressedMatch ? "true" : "false";
        out += ",\"decodedTexelsMatch\":";
        out += entry.decodedTexelsMatch ? "true" : "false";
        out += ",\"sharedIdentityMatch\":";
        out += entry.sharedIdentityMatch ? "true" : "false";
        out.push_back('}');
    }
    out += "]}";
}

std::string bakeReportJson(const char *input,
                           const char *output,
                           const AssetSnapshot &source,
                           const AssetSnapshot &baked,
                           const std::vector<FidelityLoss> &losses,
                           const TextureFidelityReport &textureReport,
                           const std::string &importReport) {
    std::string report = "{\"schema\":\"zanna.asset-bake-report/v1\",\"status\":\"ok\"";
    report += ",\"input\":" + jsonQuote(input);
    report += ",\"output\":" + jsonQuote(output);
    report += losses.empty() && textureReport.losses == 0 ? ",\"lossy\":false,\"source\":"
                                                          : ",\"lossy\":true,\"source\":";
    appendSnapshotJson(report, source);
    report += ",\"baked\":";
    appendSnapshotJson(report, baked);
    report += ",\"losses\":[";
    for (size_t i = 0; i < losses.size(); ++i) {
        const FidelityLoss &loss = losses[i];
        if (i > 0)
            report.push_back(',');
        report += "{\"code\":" + jsonQuote(loss.code);
        report += ",\"resource\":" + jsonQuote(loss.resource);
        report += ",\"source\":" + std::to_string(loss.source);
        report += ",\"baked\":" + std::to_string(loss.baked);
        report += ",\"dropped\":" + std::to_string(loss.source - loss.baked) + "}";
    }
    report += "]";
    report += ",\"textureFidelity\":";
    appendTextureFidelityJson(report, textureReport);
    report += ",\"importReport\":";
    report += importReport.empty() ? "{}" : importReport;
    report += "}";
    return report;
}

void printBakeJsonError(const char *stage,
                        const char *input,
                        const char *output,
                        const std::string &importReport) {
    std::string report = "{\"schema\":\"zanna.asset-bake-report/v1\",\"status\":\"error\"";
    report += ",\"stage\":" + jsonQuote(stage);
    report += ",\"input\":" + jsonQuote(input);
    report += ",\"output\":" + jsonQuote(output);
    report += ",\"importReport\":";
    report += importReport.empty() ? "{}" : importReport;
    report += "}";
    std::printf("%s\n", report.c_str());
}

std::string currentImportReport() {
    rt_string report = rt_assets3d_get_import_report();
    const char *text = report ? rt_string_cstr(report) : nullptr;
    return text && *text ? text : "{}";
}

#endif

void printAssetUsage(std::FILE *out) {
    std::fprintf(out,
                 "usage: zanna asset <bake|validate> ...\n"
                 "  zanna asset bake <input> <output.scene3d> [--force-tangents]\n"
                 "                   [--eight-influences] [--compress-anims] [--lods N]\n"
                 "                   [--json]\n"
                 "      Load a model through the full import pipeline (glTF/GLB/FBX/\n"
                 "      OBJ/STL, including meshopt/Draco/BasisU decode), optionally\n"
                 "      generate LOD chains, save the baked .scene3d scene, and report\n"
                 "      source-versus-baked fidelity. --json emits the v1 report.\n"
                 "  zanna asset validate <input>\n"
                 "      Load a model and print the import diagnostics report (JSON).\n");
}

} // namespace

int cmdAsset(int argc, char **argv) {
    if (argc < 1) {
        printAssetUsage(stderr);
        return 1;
    }
#ifndef ZANNA_ENABLE_GRAPHICS
    (void)argv;
    std::fprintf(stderr, "zanna asset: requires a graphics-enabled runtime build\n");
    return 2;
#else
    const std::string sub = argv[0];
    if (sub == "validate") {
        if (argc < 2) {
            printAssetUsage(stderr);
            return 1;
        }
        void *model = rt_model3d_load_with_options_ex(rt_const_cstr(argv[1]), rt_const_cstr(""));
        rt_string report = rt_assets3d_get_import_report();
        const char *text = report ? rt_string_cstr(report) : nullptr;
        std::printf("%s\n", text && *text ? text : "{}");
        if (!model) {
            std::fprintf(stderr, "zanna asset validate: failed to load '%s'\n", argv[1]);
            return 2;
        }
        if (rt_obj_release_check0(model))
            rt_obj_free(model);
        return 0;
    }
    if (sub == "bake") {
        if (argc < 3) {
            printAssetUsage(stderr);
            return 1;
        }
        const char *input = argv[1];
        const char *output = argv[2];
        std::string options;
        long lods = 0;
        bool json = false;
        for (int i = 3; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg == "--force-tangents") {
                options += options.empty() ? "forceTangents" : ",forceTangents";
            } else if (arg == "--eight-influences") {
                options += options.empty() ? "eightInfluences" : ",eightInfluences";
            } else if (arg == "--compress-anims") {
                options += options.empty() ? "compressAnimations" : ",compressAnimations";
            } else if (arg == "--lods" && i + 1 < argc) {
                lods = std::strtol(argv[++i], nullptr, 10);
                if (lods < 0 || lods > 8) {
                    std::fprintf(stderr, "zanna asset bake: --lods expects 0..8\n");
                    return 1;
                }
            } else if (arg == "--json") {
                json = true;
            } else {
                std::fprintf(stderr, "zanna asset bake: unknown option '%s'\n", arg.c_str());
                printAssetUsage(stderr);
                return 1;
            }
        }
        void *model =
            rt_model3d_load_with_options_ex(rt_const_cstr(input), rt_const_cstr(options.c_str()));
        const std::string importReport = currentImportReport();
        if (!model) {
            if (json)
                printBakeJsonError("load", input, output, importReport);
            else
                std::fprintf(stderr, "zanna asset bake: failed to load '%s'\n", input);
            return 2;
        }
        if (lods > 0)
            (void)rt_model3d_generate_lods(model, (int64_t)lods, 0.5);
        const AssetSnapshot sourceSnapshot = snapshotAsset(model);
        int64_t saved = rt_model3d_save(model, rt_const_cstr(output));
        if (!saved) {
            if (rt_obj_release_check0(model))
                rt_obj_free(model);
            if (json)
                printBakeJsonError("save", input, output, importReport);
            else
                std::fprintf(stderr, "zanna asset bake: failed to save '%s'\n", output);
            return 2;
        }
        void *bakedModel =
            rt_model3d_load_with_options_ex(rt_const_cstr(output), rt_const_cstr(""));
        if (!bakedModel) {
            if (rt_obj_release_check0(model))
                rt_obj_free(model);
            if (json)
                printBakeJsonError("verify", input, output, importReport);
            else
                std::fprintf(
                    stderr, "zanna asset bake: saved output failed verification '%s'\n", output);
            return 2;
        }
        const AssetSnapshot bakedSnapshot = snapshotAsset(bakedModel);
        const TextureFidelityReport textureReport = compareTextureFidelity(
            model, bakedModel, sourceSnapshot.materials, bakedSnapshot.materials);
        if (rt_obj_release_check0(model))
            rt_obj_free(model);
        if (rt_obj_release_check0(bakedModel))
            rt_obj_free(bakedModel);
        const std::vector<FidelityLoss> losses = compareSnapshots(sourceSnapshot, bakedSnapshot);
        if (json) {
            const std::string report = bakeReportJson(
                input, output, sourceSnapshot, bakedSnapshot, losses, textureReport, importReport);
            std::printf("%s\n", report.c_str());
        } else {
            for (const FidelityLoss &loss : losses) {
                std::fprintf(stderr,
                             "zanna asset bake: warning: dropped %lld %s "
                             "(source=%lld, baked=%lld) [%s]\n",
                             static_cast<long long>(loss.source - loss.baked),
                             loss.resource,
                             static_cast<long long>(loss.source),
                             static_cast<long long>(loss.baked),
                             loss.code);
            }
            for (const TextureFidelityEntry &entry : textureReport.entries) {
                if (entry.lossCode.empty())
                    continue;
                std::fprintf(stderr,
                             "zanna asset bake: warning: texture material=%lld slot=%s "
                             "failed fidelity verification [%s]\n",
                             static_cast<long long>(entry.material),
                             entry.slot,
                             entry.lossCode.c_str());
            }
            if (!textureReport.entries.empty()) {
                std::printf("texture fidelity: preserved-source=%lld preserved-decoded=%lld "
                            "changed-after-import=%lld losses=%lld\n",
                            static_cast<long long>(textureReport.preservedSource),
                            static_cast<long long>(textureReport.preservedDecoded),
                            static_cast<long long>(textureReport.changedAfterImport),
                            static_cast<long long>(textureReport.losses));
            }
            std::printf("baked %s -> %s\n", input, output);
        }
        return 0;
    }
    printAssetUsage(stderr);
    return 1;
#endif
}

int cmdAssetHelp(std::FILE *out) {
    printAssetUsage(out);
    return 0;
}
