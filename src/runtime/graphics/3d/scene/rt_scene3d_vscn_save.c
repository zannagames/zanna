//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_vscn_save.c
// Purpose: Scene3D .vscn serialization (save). Walks a Scene3D / SceneNode3D
//   tree and emits a JSON document with base64-encoded binary asset payloads
//   (textures, mesh buffers). A pointer-deduplication table ensures shared
//   assets are emitted once and referenced by index.
//
// Key invariants:
//   - .vscn is a JSON document with base64-encoded binary embeds.
//   - Each unique mesh / material / texture / cubemap pointer appears once.
//   - Typed node metadata promotes output to VSCN v6 and remains tagged.
//
// Ownership/Lifetime:
//   - Operates on Scene3D / SceneNode3D objects defined in rt_scene3d.c;
//     this TU owns no GC objects of its own.
//
// Links: rt_scene3d.h, rt_scene3d_internal.h, rt_scene3d_vscn_internal.h,
//        rt_scene3d_vscn_load.c (inverse: load), rt_json.h,
//        docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic JSON/Base64 serialization of VSCN scene assets.
/// @details The serializer interns shared runtime resources by pointer, emits
///   endian-stable binary payloads, selects the minimum required VSCN version,
///   and writes complete output atomically after all validation succeeds.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_file_stdio.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_morphtarget3d.h"
#include "rt_morphtarget3d_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_scene3d_vscn_internal.h"
#include "rt_seq.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"
#include "rt_trap.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    /// Borrowed unique runtime pointers in insertion order.
    void **items;
    /// Number of populated pointers.
    int32_t count;
    /// Allocated pointer capacity.
    int32_t capacity;
} vscn_ptr_table_t;

typedef struct {
    vscn_ptr_table_t meshes;
    vscn_ptr_table_t materials;
    vscn_ptr_table_t textures;
    vscn_ptr_table_t cubemaps;
    /* v3 rig payload: skeletons referenced by collected meshes, plus optional
     * animation clips supplied by the baking caller (scene graphs own no clips). */
    vscn_ptr_table_t skeletons;
    vscn_ptr_table_t cameras;
    void *const *animations; /* borrowed rt_animation3d handles, not owned */
    int32_t animation_count;
    void *const *node_animations; /* borrowed rt_node_animation3d handles */
    int32_t node_animation_count;
    const rt_vscn_asset_scene_view *scenes;
    int32_t scene_count;
    const char *const *variant_names;
    int32_t variant_count;
    int32_t output_version;
    int8_t asset_mode;
    int8_t requires_v5;
    int8_t requires_v6;
    int8_t requires_v7;
} vscn_save_context_t;

/// @brief Base64 alphabet table used by the encoder.
static const char vscn_base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief Encode `len` raw bytes to a freshly-allocated NUL-terminated base64 string.
///
/// Output length is `((len + 2) / 3) * 4` characters. Trailing
/// `=` padding is appended for inputs whose length is not a
/// multiple of 3. Caller owns the buffer (`free`); writes the
/// length sans NUL into `*out_len` if non-NULL.
/// @param data Borrowed raw bytes.
/// @param len Number of readable bytes.
/// @param out_len Optional output receiving encoded character count.
/// @return Caller-owned NUL-terminated Base64 string, or `NULL` on overflow/failure.
static char *vscn_base64_encode(const uint8_t *data, size_t len, size_t *out_len) {
    if (!data && len > 0)
        return NULL;
    if (len > SIZE_MAX - 2)
        return NULL;
    size_t groups = (len + 2) / 3;
    if (groups > (SIZE_MAX - 1) / 4)
        return NULL;
    size_t olen = groups * 4;
    char *output = (char *)malloc(olen + 1);
    if (!output)
        return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = data[i++];
        uint32_t octet_b = (i < len) ? data[i++] : 0;
        uint32_t octet_c = (i < len) ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        output[j++] = vscn_base64_chars[(triple >> 18) & 0x3F];
        output[j++] = vscn_base64_chars[(triple >> 12) & 0x3F];
        output[j++] = vscn_base64_chars[(triple >> 6) & 0x3F];
        output[j++] = vscn_base64_chars[triple & 0x3F];
    }

    {
        size_t padding = (3 - (len % 3)) % 3;
        for (size_t p = 0; p < padding; p++)
            output[j - 1 - p] = '=';
    }

    output[j] = '\0';
    if (out_len)
        *out_len = j;
    return output;
}

/// @brief Encode IEEE-754 float values in explicit little-endian order before Base64.
/// @param values Borrowed finite/raw float array.
/// @param count Number of readable values.
/// @return Caller-owned NUL-terminated Base64 string, or `NULL`.
static char *vscn_base64_encode_f32_le(const float *values, size_t count) {
    uint8_t *bytes;
    char *encoded;
    if ((!values && count > 0) || count > SIZE_MAX / 4u)
        return NULL;
    bytes = (uint8_t *)malloc(count > 0 ? count * 4u : 1u);
    if (!bytes)
        return NULL;
    for (size_t i = 0; i < count; ++i) {
        uint32_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        bytes[i * 4u + 0u] = (uint8_t)(bits & 0xffu);
        bytes[i * 4u + 1u] = (uint8_t)((bits >> 8u) & 0xffu);
        bytes[i * 4u + 2u] = (uint8_t)((bits >> 16u) & 0xffu);
        bytes[i * 4u + 3u] = (uint8_t)((bits >> 24u) & 0xffu);
    }
    encoded = vscn_base64_encode(bytes, count * 4u, NULL);
    free(bytes);
    return encoded;
}

/// @brief Encode IEEE-754 double values in explicit little-endian order before Base64.
/// @param values Borrowed finite/raw double array.
/// @param count Number of readable values.
/// @return Caller-owned NUL-terminated Base64 string, or `NULL`.
static char *vscn_base64_encode_f64_le(const double *values, size_t count) {
    uint8_t *bytes;
    char *encoded;
    if ((!values && count > 0) || count > SIZE_MAX / 8u)
        return NULL;
    bytes = (uint8_t *)malloc(count > 0 ? count * 8u : 1u);
    if (!bytes)
        return NULL;
    for (size_t i = 0; i < count; ++i) {
        uint64_t bits;
        memcpy(&bits, &values[i], sizeof(bits));
        for (size_t byte = 0; byte < 8u; ++byte)
            bytes[i * 8u + byte] = (uint8_t)((bits >> (byte * 8u)) & UINT64_C(0xff));
    }
    encoded = vscn_base64_encode(bytes, count * 8u, NULL);
    free(bytes);
    return encoded;
}

/// @brief Reset a pointer table — free its backing array and zero counts.
/// @param table Borrowed mutable pointer table.
static void vscn_free_ptr_table(vscn_ptr_table_t *table) {
    if (!table)
        return;
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

/// @brief Return the index of `item` in the table, inserting it if not present.
///
/// Provides O(n) interning of asset pointers so the saver can
/// emit each shared mesh / material / texture exactly once and
/// reference it by index from elsewhere in the JSON.
/// @param table Borrowed mutable pointer-interning table.
/// @param item Borrowed non-NULL runtime pointer.
/// @return Existing or newly-assigned index, or -1 on alloc failure / NULL inputs.
static int vscn_ptr_table_index_or_add(vscn_ptr_table_t *table, void *item) {
    if (!table || !item)
        return -1;
    for (int32_t i = 0; i < table->count; i++) {
        if (table->items[i] == item)
            return i;
    }
    if (table->count >= table->capacity) {
        if (table->capacity > INT32_MAX / 2)
            return -1;
        int32_t new_cap = table->capacity == 0 ? 8 : table->capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(void *))
            return -1;
        void **new_items = (void **)realloc(table->items, (size_t)new_cap * sizeof(void *));
        if (!new_items)
            return -1;
        table->items = new_items;
        table->capacity = new_cap;
    }
    table->items[table->count] = item;
    table->count++;
    return table->count - 1;
}

/// @brief Find an already-collected pointer without mutating table order.
/// @param table Borrowed pointer table.
/// @param item Borrowed pointer to locate.
/// @return Zero-based index, or `-1` when absent/invalid.
static int vscn_ptr_table_index(const vscn_ptr_table_t *table, const void *item) {
    if (!table || !item)
        return -1;
    for (int32_t i = 0; i < table->count; ++i) {
        if (table->items[i] == item)
            return i;
    }
    return -1;
}

/// @brief Write `depth` levels of two-space indentation into `indent` (NUL-terminated).
/// @param indent Output character buffer.
/// @param indent_cap Buffer capacity including terminator.
/// @param depth Non-negative logical nesting depth.
static void vscn_make_indent(char *indent, size_t indent_cap, int depth) {
    size_t count;
    if (!indent || indent_cap == 0)
        return;
    count = (size_t)(depth * 2);
    if (count >= indent_cap)
        count = indent_cap - 1;
    memset(indent, ' ', count);
    indent[count] = '\0';
}

/// @brief Ensure `*buf` has at least `needed` bytes of capacity, doubling (starting at
///   4096) until it fits. @return 1 on success, 0 on alloc failure.
/// @param buf Address of caller-owned allocation.
/// @param cap In/out byte capacity.
/// @param needed Minimum required byte capacity.
static int vscn_reserve(char **buf, size_t *cap, size_t needed) {
    char *nb;
    size_t new_cap;
    if (!buf || !cap)
        return 0;
    if (*cap >= needed)
        return 1;
    new_cap = *cap == 0 ? 4096u : *cap;
    while (new_cap < needed) {
        size_t doubled;
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        doubled = new_cap * 2u;
        new_cap = doubled < needed ? needed : doubled;
    }
    nb = (char *)realloc(*buf, new_cap);
    if (!nb)
        return 0;
    *buf = nb;
    *cap = new_cap;
    return 1;
}

/// @brief Append `src_len` raw bytes to the growing `*buf` (reserving capacity first and
///   keeping the buffer NUL-terminated). @return 1 on success, 0 on overflow/alloc failure.
/// @param buf Address of caller-owned allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param src Borrowed source bytes.
/// @param src_len Number of bytes to append.
static int vscn_append_raw(char **buf, size_t *len, size_t *cap, const char *src, size_t src_len) {
    if (!buf || !len || !cap || (!src && src_len > 0))
        return 0;
    if (src_len > SIZE_MAX - *len - 1)
        return 0;
    if (!vscn_reserve(buf, cap, *len + src_len + 1))
        return 0;
    if (src_len > 0)
        memcpy(*buf + *len, src, src_len);
    *len += src_len;
    (*buf)[*len] = '\0';
    return 1;
}

/// @brief Append a formatted string to a growable byte buffer.
/// @details Formats directly into the buffer and retries after growing it if
///   the current capacity is too small. Avoids `vsnprintf(NULL, 0)` because
///   that path corrupts floating-point varargs on MSVC ARM64.
/// @param buf Address of caller-owned allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param fmt Borrowed printf-style format followed by matching arguments.
/// @return 1 on success, 0 if `vsnprintf` fails or realloc is denied.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
/// @brief Append printf-formatted text to a growable NUL-terminated buffer.
/// @param buf Address of caller-owned allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param fmt Borrowed printf-style format followed by matching arguments.
/// @return Nonzero on success, otherwise zero.
static int vscn_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    if (!buf || !len || !cap || !fmt)
        return 0;
    for (;;) {
        va_list ap;
        int written;
        size_t available;

        if (*len > SIZE_MAX - 1)
            return 0;
        if (!vscn_reserve(buf, cap, *len + 1))
            return 0;

        available = *cap - *len;
        va_start(ap, fmt);
        written = vsnprintf(*buf + *len, available, fmt, ap);
        va_end(ap);
        if (written < 0)
            return 0;
        if ((size_t)written < available) {
            *len += (size_t)written;
            return 1;
        }

        if ((size_t)written > SIZE_MAX - *len - 1)
            return 0;
        if (!vscn_reserve(buf, cap, *len + (size_t)written + 1))
            return 0;
    }
}

/// @brief Append `text` to the buffer wrapped in JSON quotes with proper escapes.
///
/// Emits the standard backslash escapes (`\"`, `\\`, `\b`, `\f`,
/// `\n`, `\r`, `\t`) for ASCII control characters and `\u00XX`
/// for any other sub-0x20 byte. Bytes >= 0x20 are passed through
/// verbatim — JSON allows raw UTF-8.
/// @param buf Address of caller-owned output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param text Borrowed NUL-terminated UTF-8 string; `NULL` emits empty.
/// @return Nonzero after successful escaped append, otherwise zero.
static int vscn_append_json_string(char **buf, size_t *len, size_t *cap, const char *text) {
    if (!vscn_append_raw(buf, len, cap, "\"", 1))
        return 0;
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            char unicode_escape[7];
            switch (*p) {
                case '\"':
                    if (!vscn_append_raw(buf, len, cap, "\\\"", 2))
                        return 0;
                    break;
                case '\\':
                    if (!vscn_append_raw(buf, len, cap, "\\\\", 2))
                        return 0;
                    break;
                case '\b':
                    if (!vscn_append_raw(buf, len, cap, "\\b", 2))
                        return 0;
                    break;
                case '\f':
                    if (!vscn_append_raw(buf, len, cap, "\\f", 2))
                        return 0;
                    break;
                case '\n':
                    if (!vscn_append_raw(buf, len, cap, "\\n", 2))
                        return 0;
                    break;
                case '\r':
                    if (!vscn_append_raw(buf, len, cap, "\\r", 2))
                        return 0;
                    break;
                case '\t':
                    if (!vscn_append_raw(buf, len, cap, "\\t", 2))
                        return 0;
                    break;
                default:
                    if (*p < 0x20u) {
                        snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", (unsigned)*p);
                        if (!vscn_append_raw(buf, len, cap, unicode_escape, 6))
                            return 0;
                    } else if (!vscn_append_raw(buf, len, cap, (const char *)p, 1)) {
                        return 0;
                    }
                    break;
            }
        }
    }
    return vscn_append_raw(buf, len, cap, "\"", 1);
}

/// @brief First pass: register every texture / cubemap referenced by `material` in the dedupe
/// tables.
///
/// The save algorithm runs collection over the whole scene before
/// any serialisation so each shared asset gets a stable index
/// referenced from every material that uses it.
/// @param texture_ref Borrowed candidate Pixels or TextureAsset3D handle.
/// @return Borrowed serializable texture reference, or `NULL`.
static void *vscn_material_texture_ref(void *texture_ref) {
    const uint8_t *source = NULL;
    uint64_t source_size = 0;
    const char *source_kind = NULL;
    void *pixels;
    if (!texture_ref)
        return NULL;
    if (rt_g3d_has_class(texture_ref, RT_G3D_TEXTUREASSET3D_CLASS_ID) &&
        rt_textureasset3d_get_source_container(texture_ref, &source, &source_size, &source_kind) &&
        source && source_size > 0 && source_kind)
        return texture_ref;
    pixels = rt_material3d_resolve_texture_pixels(texture_ref);
    return rt_pixels_checked_impl_or_null(pixels) ? texture_ref : NULL;
}

/// @brief True if a cubemap has all six faces present and square at its declared face size
///   (i.e. it can be serialized into a .vscn asset).
/// @param cubemap Borrowed candidate Cubemap3D payload.
/// @return Nonzero when all six face images are complete and dimensionally consistent.
static int vscn_cubemap_is_serializable(rt_cubemap3d *cubemap) {
    if (!rt_g3d_has_class(cubemap, RT_G3D_CUBEMAP3D_CLASS_ID) || cubemap->face_size <= 0 ||
        cubemap->face_size > INT32_MAX)
        return 0;
    for (int i = 0; i < 6; i++) {
        rt_pixels_impl *face = rt_pixels_checked_impl_or_null(cubemap->faces[i]);
        if (!face || face->width != cubemap->face_size || face->height != cubemap->face_size)
            return 0;
    }
    return 1;
}

/// @brief Return a material's environment cubemap only when present and serializable, else NULL.
/// @param material Borrowed Material3D payload.
/// @return Borrowed serializable Cubemap3D payload, or `NULL`.
static rt_cubemap3d *vscn_material_env_map(rt_material3d *material) {
    rt_cubemap3d *cubemap = material ? (rt_cubemap3d *)material->env_map : NULL;
    return vscn_cubemap_is_serializable(cubemap) ? cubemap : NULL;
}

/// @brief Register a material's referenced textures and environment cubemap into the save
///   context's dedup tables; returns 0 on allocation failure, 1 on success (or nothing to do).
/// @param material Borrowed Material3D payload.
/// @param ctx Borrowed mutable save context.
/// @return Nonzero after complete dependency collection, otherwise zero.
static int vscn_collect_material_assets(rt_material3d *material, vscn_save_context_t *ctx) {
    void *texture;
    rt_cubemap3d *cubemap;
    if (!material || !ctx)
        return 1;
    texture = vscn_material_texture_ref(material->texture);
    if (texture && vscn_ptr_table_index_or_add(&ctx->textures, texture) < 0)
        return 0;
    texture = vscn_material_texture_ref(material->normal_map);
    if (texture && vscn_ptr_table_index_or_add(&ctx->textures, texture) < 0)
        return 0;
    texture = vscn_material_texture_ref(material->specular_map);
    if (texture && vscn_ptr_table_index_or_add(&ctx->textures, texture) < 0)
        return 0;
    texture = vscn_material_texture_ref(material->emissive_map);
    if (texture && vscn_ptr_table_index_or_add(&ctx->textures, texture) < 0)
        return 0;
    texture = vscn_material_texture_ref(material->metallic_roughness_map);
    if (texture && vscn_ptr_table_index_or_add(&ctx->textures, texture) < 0)
        return 0;
    texture = vscn_material_texture_ref(material->ao_map);
    if (texture && vscn_ptr_table_index_or_add(&ctx->textures, texture) < 0)
        return 0;
    texture = vscn_material_texture_ref(material->lightmap);
    if (texture && vscn_ptr_table_index_or_add(&ctx->textures, texture) < 0)
        return 0;
    cubemap = vscn_material_env_map(material);
    if (cubemap) {
        if (vscn_ptr_table_index_or_add(&ctx->cubemaps, cubemap) < 0)
            return 0;
        for (int i = 0; i < 6; i++) {
            if (cubemap->faces[i] &&
                vscn_ptr_table_index_or_add(&ctx->textures, cubemap->faces[i]) < 0)
                return 0;
        }
    }
    return 1;
}

/// @brief Register one validated material and all texture/cubemap dependencies.
/// @param material Borrowed optional Material3D payload.
/// @param ctx Borrowed mutable save context.
/// @return Nonzero after complete collection or a NULL no-op.
static int vscn_collect_material(rt_material3d *material, vscn_save_context_t *ctx) {
    if (!material)
        return 1;
    return vscn_ptr_table_index_or_add(&ctx->materials, material) >= 0 &&
           vscn_collect_material_assets(material, ctx);
}

/// @brief Register one validated mesh and its attached skeleton.
/// @param mesh Borrowed optional Mesh3D payload.
/// @param ctx Borrowed mutable save context.
/// @return Nonzero after complete collection or a NULL no-op.
static int vscn_collect_mesh(rt_mesh3d *mesh, vscn_save_context_t *ctx) {
    if (!mesh)
        return 1;
    if (vscn_ptr_table_index_or_add(&ctx->meshes, mesh) < 0)
        return 0;
    if (rt_g3d_has_class(mesh->skeleton_ref, RT_G3D_SKELETON3D_CLASS_ID) &&
        vscn_ptr_table_index_or_add(&ctx->skeletons, mesh->skeleton_ref) < 0)
        return 0;
    return 1;
}

/// @brief Collect mesh / material / texture references from a node subtree without recursion.
/// @param node Borrowed subtree root.
/// @param ctx Borrowed mutable save context.
/// @return Nonzero when every referenced asset is valid and interned.
static int vscn_collect_node_assets(rt_scene_node3d *node, vscn_save_context_t *ctx) {
    rt_scene_node3d **stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    if (!node || !ctx)
        return 1;
    for (;;) {
        size_t new_capacity = 64u;
        stack = (rt_scene_node3d **)malloc(new_capacity * sizeof(*stack));
        if (!stack)
            return 0;
        capacity = new_capacity;
        stack[count++] = node;
        break;
    }
    while (count > 0) {
        rt_scene_node3d *current = stack[--count];
        rt_mesh3d *mesh;
        rt_material3d *material;
        if (!current)
            continue;
        if (current->prefab_path) {
            /* Prefab reference nodes persist identity + reference only
             * (ADR 0187): grafted content and non-override payloads are
             * never serialized, so they collect no shared assets. */
            ctx->requires_v7 = 1;
            continue;
        }
        mesh = (rt_mesh3d *)rt_g3d_checked_or_null(current->mesh, RT_G3D_MESH3D_CLASS_ID);
        material =
            (rt_material3d *)rt_g3d_checked_or_null(current->material, RT_G3D_MATERIAL3D_CLASS_ID);
        if ((current->mesh && !mesh) || (current->material && !material) ||
            (current->light && !rt_g3d_has_class(current->light, RT_G3D_LIGHT3D_CLASS_ID)) ||
            (current->camera && !rt_g3d_has_class(current->camera, RT_G3D_CAMERA3D_CLASS_ID))) {
            free(stack);
            return 0;
        }
        if (current->camera) {
            if (vscn_ptr_table_index_or_add(&ctx->cameras, current->camera) < 0) {
                free(stack);
                return 0;
            }
            ctx->requires_v5 = 1;
        }
        if (current->light)
            ctx->requires_v5 = 1;
        if (current->metadata_count < 0 ||
            current->metadata_count > RT_SCENE_NODE3D_MAX_METADATA_ENTRIES ||
            (current->metadata_count > 0 && !current->metadata)) {
            free(stack);
            return 0;
        }
        if (current->metadata_count > 0)
            ctx->requires_v6 = 1;
        if (!vscn_collect_mesh(mesh, ctx) || !vscn_collect_material(material, ctx)) {
            free(stack);
            return 0;
        }
        if (ctx->asset_mode && current->variant_material_count > 0) {
            if (!current->variant_materials ||
                current->variant_material_count > ctx->variant_count) {
                free(stack);
                return 0;
            }
            for (int32_t i = 0; i < current->variant_material_count; ++i) {
                rt_material3d *variant = (rt_material3d *)rt_g3d_checked_or_null(
                    current->variant_materials[i], RT_G3D_MATERIAL3D_CLASS_ID);
                if (current->variant_materials[i] && !variant) {
                    free(stack);
                    return 0;
                }
                if (!vscn_collect_material(variant, ctx)) {
                    free(stack);
                    return 0;
                }
            }
        }
        for (int32_t i = 0, lod_count = scene3d_node_lod_count(current); i < lod_count; i++) {
            rt_mesh3d *lod_mesh = (rt_mesh3d *)rt_g3d_checked_or_null(current->lod_levels[i].mesh,
                                                                      RT_G3D_MESH3D_CLASS_ID);
            if (current->lod_levels[i].mesh && !lod_mesh) {
                free(stack);
                return 0;
            }
            if (!vscn_collect_mesh(lod_mesh, ctx)) {
                free(stack);
                return 0;
            }
        }
        for (int32_t i = 0, child_count = scene3d_node_child_count(current); i < child_count; i++) {
            rt_scene_node3d *child = scene_node3d_checked(current->children[i]);
            if (!child)
                continue;
            if (count >= capacity) {
                size_t new_capacity = capacity * 2u;
                rt_scene_node3d **grown;
                if (new_capacity <= capacity ||
                    new_capacity > SIZE_MAX / sizeof(rt_scene_node3d *)) {
                    free(stack);
                    return 0;
                }
                grown =
                    (rt_scene_node3d **)realloc(stack, new_capacity * sizeof(rt_scene_node3d *));
                if (!grown) {
                    free(stack);
                    return 0;
                }
                stack = grown;
                capacity = new_capacity;
            }
            stack[count++] = child;
        }
    }
    free(stack);
    return 1;
}

/// @brief Validate and seed every shared table from a complete SceneAsset view before tree walks.
/// @param view Borrowed complete asset inventory.
/// @param ctx Borrowed zero-initialized mutable save context.
/// @return Nonzero when every inventory entry/reference is valid and collected.
static int vscn_collect_asset_view(const rt_vscn_asset_save_view *view, vscn_save_context_t *ctx) {
    if (!view || !ctx || view->scene_count <= 0 || view->scene_count > 65536 || !view->scenes ||
        view->mesh_count < 0 || view->material_count < 0 || view->skeleton_count < 0 ||
        view->animation_count < 0 || view->node_animation_count < 0 || view->camera_count < 0 ||
        view->variant_count < 0)
        return 0;
    ctx->asset_mode = 1;
    ctx->animations = view->animations;
    ctx->animation_count = view->animation_count;
    ctx->node_animations = view->node_animations;
    ctx->node_animation_count = view->node_animation_count;
    ctx->scenes = view->scenes;
    ctx->scene_count = view->scene_count;
    ctx->variant_names = view->variant_names;
    ctx->variant_count = view->variant_count;

    for (int32_t i = 0; i < view->mesh_count; ++i) {
        rt_mesh3d *mesh = view->meshes ? (rt_mesh3d *)rt_g3d_checked_or_null(view->meshes[i],
                                                                             RT_G3D_MESH3D_CLASS_ID)
                                       : NULL;
        if (!mesh || !vscn_collect_mesh(mesh, ctx))
            return 0;
    }
    for (int32_t i = 0; i < view->material_count; ++i) {
        rt_material3d *material =
            view->materials ? (rt_material3d *)rt_g3d_checked_or_null(view->materials[i],
                                                                      RT_G3D_MATERIAL3D_CLASS_ID)
                            : NULL;
        if (!material || !vscn_collect_material(material, ctx))
            return 0;
    }
    for (int32_t i = 0; i < view->skeleton_count; ++i) {
        void *skeleton =
            view->skeletons ? rt_g3d_checked_or_null(view->skeletons[i], RT_G3D_SKELETON3D_CLASS_ID)
                            : NULL;
        if (!skeleton || vscn_ptr_table_index_or_add(&ctx->skeletons, skeleton) < 0)
            return 0;
    }
    for (int32_t i = 0; i < view->animation_count; ++i) {
        if (!view->animations ||
            !rt_g3d_has_class(view->animations[i], RT_G3D_ANIMATION3D_CLASS_ID))
            return 0;
    }
    for (int32_t i = 0; i < view->node_animation_count; ++i) {
        if (!view->node_animations ||
            !rt_g3d_has_class(view->node_animations[i], RT_G3D_NODEANIMATION3D_CLASS_ID))
            return 0;
    }
    for (int32_t i = 0; i < view->camera_count; ++i) {
        void *camera = view->cameras
                           ? rt_g3d_checked_or_null(view->cameras[i], RT_G3D_CAMERA3D_CLASS_ID)
                           : NULL;
        if (!camera || vscn_ptr_table_index_or_add(&ctx->cameras, camera) < 0)
            return 0;
    }
    if (view->variant_count > 0 && !view->variant_names)
        return 0;
    for (int32_t i = 0; i < view->variant_count; ++i) {
        if (!view->variant_names[i])
            return 0;
    }
    for (int32_t i = 0; i < view->scene_count; ++i) {
        const rt_vscn_asset_scene_view *scene = &view->scenes[i];
        if (!rt_g3d_has_class(scene->root, RT_G3D_SCENENODE3D_CLASS_ID) || !scene->name ||
            scene->camera_count < 0 || (scene->camera_count > 0 && !scene->cameras) ||
            !vscn_collect_node_assets(scene->root, ctx))
            return 0;
        for (int32_t camera_index = 0; camera_index < scene->camera_count; ++camera_index) {
            void *camera =
                rt_g3d_checked_or_null(scene->cameras[camera_index], RT_G3D_CAMERA3D_CLASS_ID);
            if (!camera || vscn_ptr_table_index_or_add(&ctx->cameras, camera) < 0)
                return 0;
        }
    }
    return 1;
}

/// @brief Emit one original texture reference as exact source bytes or canonical RGBA8.
/// @param texture_ref Borrowed validated texture reference.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param depth JSON indentation depth.
/// @param version Selected VSCN output version.
/// @return Nonzero on success, otherwise zero.
static int vscn_serialize_texture(
    void *texture_ref, char **buf, size_t *len, size_t *cap, int depth, int version) {
    char indent[64];
    const uint8_t *source = NULL;
    uint64_t source_size = 0;
    const char *source_kind = NULL;
    rt_pixels_impl *pixels;
    size_t pixel_count;
    size_t rgba_len;
    uint8_t *rgba = NULL;
    char *rgba_base64 = NULL;

    if (!texture_ref)
        return 0;
    vscn_make_indent(indent, sizeof(indent), depth);
    if (version >= 5 &&
        rt_textureasset3d_get_source_container(texture_ref, &source, &source_size, &source_kind)) {
        char *source_base64;
        int ok;
        if (!source || source_size == 0 || source_size > (uint64_t)SIZE_MAX || !source_kind)
            return 0;
        source_base64 = vscn_base64_encode(source, (size_t)source_size, NULL);
        if (!source_base64)
            return 0;
        ok = vscn_append(buf, len, cap, "%s{\"kind\": \"source\", \"container\": ", indent) &&
             vscn_append_json_string(buf, len, cap, source_kind) &&
             vscn_append(buf, len, cap, ", \"sourceBase64\": ") &&
             vscn_append_json_string(buf, len, cap, source_base64) &&
             vscn_append(buf, len, cap, "}");
        free(source_base64);
        return ok;
    }
    pixels = rt_pixels_checked_impl_or_null(rt_material3d_resolve_texture_pixels(texture_ref));
    if (!pixels)
        return 0;
    if (pixels->width < 0 || pixels->height < 0)
        return 0;
    if ((size_t)pixels->width > SIZE_MAX / (size_t)(pixels->height > 0 ? pixels->height : 1))
        return 0;

    pixel_count = (size_t)pixels->width * (size_t)pixels->height;
    if (pixel_count > SIZE_MAX / 4)
        return 0;
    if (pixel_count > 0 && !pixels->data)
        return 0;
    rgba_len = pixel_count * 4;
    rgba = (uint8_t *)malloc(rgba_len > 0 ? rgba_len : 1);
    if (!rgba)
        return 0;

    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t px = pixels->data[i];
        rgba[i * 4 + 0] = (uint8_t)((px >> 24) & 0xFF);
        rgba[i * 4 + 1] = (uint8_t)((px >> 16) & 0xFF);
        rgba[i * 4 + 2] = (uint8_t)((px >> 8) & 0xFF);
        rgba[i * 4 + 3] = (uint8_t)(px & 0xFF);
    }

    rgba_base64 = vscn_base64_encode(rgba, rgba_len, NULL);
    free(rgba);
    if (!rgba_base64)
        return 0;

    {
        int ok = vscn_append(buf,
                             len,
                             cap,
                             version >= 5 ? "%s{\"kind\": \"rgba8\", \"width\": %lld, "
                                            "\"height\": %lld, \"rgbaBase64\": "
                                          : "%s{\"width\": %lld, \"height\": %lld, "
                                            "\"rgbaBase64\": ",
                             indent,
                             (long long)pixels->width,
                             (long long)pixels->height) &&
                 vscn_append_json_string(buf, len, cap, rgba_base64) &&
                 vscn_append(buf, len, cap, "}");
        free(rgba_base64);
        return ok;
    }
}

/// @brief Emit a cubemap as six texture-index references (px/nx/py/ny/pz/nz).
/// @param cubemap Borrowed serializable Cubemap3D payload.
/// @param ctx Borrowed save context containing stable texture indices.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param depth JSON indentation depth.
/// @return Nonzero on success, otherwise zero.
static int vscn_serialize_cubemap(rt_cubemap3d *cubemap,
                                  vscn_save_context_t *ctx,
                                  char **buf,
                                  size_t *len,
                                  size_t *cap,
                                  int depth) {
    char indent[64];
    if (!cubemap || !ctx)
        return 0;
    if (!vscn_cubemap_is_serializable(cubemap))
        return 0;
    vscn_make_indent(indent, sizeof(indent), depth);
    if (!vscn_append(buf, len, cap, "%s{\"faces\": [", indent))
        return 0;
    for (int i = 0; i < 6; i++) {
        if (!cubemap->faces[i])
            return 0;
        int index = vscn_ptr_table_index_or_add(&ctx->textures, cubemap->faces[i]);
        if (index < 0)
            return 0;
        if (!vscn_append(buf, len, cap, "%s%d", i == 0 ? "" : ", ", index))
            return 0;
    }
    return vscn_append(buf, len, cap, "]}");
}

/// @brief Emit a material as JSON: PBR parameters + texture-index references for each map.
/// @param material Borrowed validated Material3D payload.
/// @param ctx Borrowed save context containing stable resource indices.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param depth JSON indentation depth.
/// @return Nonzero on success, otherwise zero.
static int vscn_serialize_material(rt_material3d *material,
                                   vscn_save_context_t *ctx,
                                   char **buf,
                                   size_t *len,
                                   size_t *cap,
                                   int depth) {
    char indent[64];
    if (!material || !ctx)
        return 0;
    vscn_make_indent(indent, sizeof(indent), depth);

    const int texture_index =
        vscn_ptr_table_index_or_add(&ctx->textures, vscn_material_texture_ref(material->texture));
    const int normal_index = vscn_ptr_table_index_or_add(
        &ctx->textures, vscn_material_texture_ref(material->normal_map));
    const int specular_index = vscn_ptr_table_index_or_add(
        &ctx->textures, vscn_material_texture_ref(material->specular_map));
    const int emissive_index = vscn_ptr_table_index_or_add(
        &ctx->textures, vscn_material_texture_ref(material->emissive_map));
    const int metallic_roughness_index = vscn_ptr_table_index_or_add(
        &ctx->textures, vscn_material_texture_ref(material->metallic_roughness_map));
    const int ao_index =
        vscn_ptr_table_index_or_add(&ctx->textures, vscn_material_texture_ref(material->ao_map));
    const int lightmap_index =
        vscn_ptr_table_index_or_add(&ctx->textures, vscn_material_texture_ref(material->lightmap));
    const int env_index =
        vscn_ptr_table_index_or_add(&ctx->cubemaps, vscn_material_env_map(material));

    if (!vscn_append(buf,
                     len,
                     cap,
                     "%s{\"diffuse\": [%.17g, %.17g, %.17g, %.17g], ",
                     indent,
                     vscn_clamp_or(material->diffuse[0], 1.0, 0.0, 1.0),
                     vscn_clamp_or(material->diffuse[1], 1.0, 0.0, 1.0),
                     vscn_clamp_or(material->diffuse[2], 1.0, 0.0, 1.0),
                     vscn_clamp_or(material->diffuse[3], 1.0, 0.0, 1.0)) ||
        !vscn_append(
            buf,
            len,
            cap,
            "\"specular\": [%.17g, %.17g, %.17g], \"shininess\": %.17g, "
            "\"workflow\": %d, ",
            vscn_clamp_or(material->specular[0], 1.0, 0.0, VSCN_ABS_MAX),
            vscn_clamp_or(material->specular[1], 1.0, 0.0, VSCN_ABS_MAX),
            vscn_clamp_or(material->specular[2], 1.0, 0.0, VSCN_ABS_MAX),
            vscn_nonnegative_or(material->shininess, 32.0),
            vscn_material_workflow_or(material->workflow, RT_MATERIAL3D_WORKFLOW_LEGACY)) ||
        !vscn_append(buf,
                     len,
                     cap,
                     "\"emissive\": [%.17g, %.17g, %.17g], \"metallic\": %.17g, "
                     "\"roughness\": %.17g, \"ao\": %.17g, ",
                     vscn_clamp_or(material->emissive[0], 0.0, 0.0, VSCN_ABS_MAX),
                     vscn_clamp_or(material->emissive[1], 0.0, 0.0, VSCN_ABS_MAX),
                     vscn_clamp_or(material->emissive[2], 0.0, 0.0, VSCN_ABS_MAX),
                     vscn_clamp_or(material->metallic, 0.0, 0.0, 1.0),
                     vscn_clamp_or(material->roughness, 0.5, 0.0, 1.0),
                     vscn_clamp_or(material->ao, 1.0, 0.0, 1.0)) ||
        !vscn_append(buf,
                     len,
                     cap,
                     "\"emissiveIntensity\": %.17g, \"normalScale\": %.17g, "
                     "\"alpha\": %.17g, \"alphaMode\": %d, \"alphaCutoff\": %.17g, ",
                     vscn_nonnegative_or(material->emissive_intensity, 1.0),
                     vscn_nonnegative_or(material->normal_scale, 1.0),
                     vscn_clamp_or(material->alpha, 1.0, 0.0, 1.0),
                     vscn_alpha_mode_or(material->alpha_mode, RT_MATERIAL3D_ALPHA_MODE_OPAQUE),
                     vscn_clamp_or(material->alpha_cutoff, 0.5, 0.0, 1.0)) ||
        !vscn_append(buf,
                     len,
                     cap,
                     "\"doubleSided\": %s, \"reflectivity\": %.17g, \"unlit\": %s, "
                     "\"shadingModel\": %d, ",
                     material->double_sided ? "true" : "false",
                     vscn_clamp_or(material->reflectivity, 0.0, 0.0, 1.0),
                     material->unlit ? "true" : "false",
                     (material->shading_model >= 0 && material->shading_model <= 5)
                         ? material->shading_model
                         : 0) ||
        !vscn_append(buf,
                     len,
                     cap,
                     "\"texture\": %d, \"normalMap\": %d, \"specularMap\": %d, "
                     "\"emissiveMap\": %d, \"metallicRoughnessMap\": %d, \"aoMap\": %d, "
                     "\"envMap\": %d, \"lightMap\": %d, \"customParams\": [",
                     texture_index,
                     normal_index,
                     specular_index,
                     emissive_index,
                     metallic_roughness_index,
                     ao_index,
                     env_index,
                     lightmap_index)) {
        return 0;
    }

    for (int i = 0; i < 12; i++) {
        if (!vscn_append(buf,
                         len,
                         cap,
                         "%s%.17g",
                         i == 0 ? "" : ", ",
                         vscn_clamp_abs_or(material->custom_params[i], 0.0))) {
            return 0;
        }
    }
    if (!vscn_append(buf, len, cap, "], \"textureSlots\": ["))
        return 0;
    for (int i = 0; i < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; i++) {
        const double *uvm = material->texture_slot_uv_transform[i];
        if (!vscn_append(
                buf,
                len,
                cap,
                "%s{\"uvSet\": %d, \"wrapS\": %d, \"wrapT\": %d, "
                "\"filter\": %d, \"minFilter\": %d, \"magFilter\": %d, \"mipFilter\": %d, "
                "\"anisotropy\": %d, "
                "\"uvTransform\": [%.17g, %.17g, %.17g, %.17g, %.17g, %.17g]}",
                i == 0 ? "" : ", ",
                material->texture_slot_uv_set[i] > 0 ? 1 : 0,
                vscn_wrap_or(material->texture_slot_wrap_s[i], RT_MATERIAL3D_TEXTURE_WRAP_REPEAT),
                vscn_wrap_or(material->texture_slot_wrap_t[i], RT_MATERIAL3D_TEXTURE_WRAP_REPEAT),
                vscn_filter_or(material->texture_slot_filter[i],
                               RT_MATERIAL3D_TEXTURE_FILTER_LINEAR),
                vscn_filter_or(material->texture_slot_min_filter[i],
                               RT_MATERIAL3D_TEXTURE_FILTER_LINEAR),
                vscn_filter_or(material->texture_slot_mag_filter[i],
                               RT_MATERIAL3D_TEXTURE_FILTER_LINEAR),
                vscn_mip_filter_or(material->texture_slot_mip_filter[i],
                                   RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE),
                material->texture_slot_anisotropy[i] < 1
                    ? 1
                    : (material->texture_slot_anisotropy[i] > 16
                           ? 16
                           : material->texture_slot_anisotropy[i]),
                vscn_clamp_abs_or(uvm[0], 1.0),
                vscn_clamp_abs_or(uvm[1], 0.0),
                vscn_clamp_abs_or(uvm[2], 0.0),
                vscn_clamp_abs_or(uvm[3], 1.0),
                vscn_clamp_abs_or(uvm[4], 0.0),
                vscn_clamp_abs_or(uvm[5], 0.0))) {
            return 0;
        }
    }
    return vscn_append(buf, len, cap, "]}");
}

/// @brief Append a mesh's complete VSCN v4+ morph-target block, if attached.
/// @param mesh Borrowed validated Mesh3D payload.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @return Nonzero when absent or serialized successfully, otherwise zero.
static int vscn_serialize_mesh_morph_targets(rt_mesh3d *mesh,
                                             char **buf,
                                             size_t *len,
                                             size_t *cap) {
    int64_t shape_count;
    if (!mesh || !mesh->morph_targets_ref)
        return 1;
    if (!rt_g3d_has_class(mesh->morph_targets_ref, RT_G3D_MORPHTARGET3D_CLASS_ID))
        return 0;
    shape_count = rt_morphtarget3d_get_shape_count(mesh->morph_targets_ref);
    if (shape_count < 0 || shape_count > 65536 ||
        !vscn_append(buf,
                     len,
                     cap,
                     ", \"morphTargets\": {\"deltaFormat\": \"f32le-v1\", "
                     "\"vertexCount\": %u, \"shapes\": [",
                     mesh->vertex_count))
        return 0;
    for (int64_t shape = 0; shape < shape_count; ++shape) {
        rt_morphtarget3d_shape_view_internal view;
        size_t value_count;
        char *positions = NULL;
        char *normals = NULL;
        char *tangents = NULL;
        int ok;
        if (!rt_morphtarget3d_get_shape_view_internal(mesh->morph_targets_ref, shape, &view) ||
            view.vertex_count != (int32_t)mesh->vertex_count || view.vertex_count <= 0 ||
            (size_t)view.vertex_count > SIZE_MAX / 3u)
            return 0;
        value_count = (size_t)view.vertex_count * 3u;
        if (!isfinite(view.weight))
            return 0;
        for (size_t value = 0; value < value_count; ++value) {
            if (!isfinite(view.position_deltas[value]) ||
                (view.normal_deltas && !isfinite(view.normal_deltas[value])) ||
                (view.tangent_deltas && !isfinite(view.tangent_deltas[value])))
                return 0;
        }
        positions = vscn_base64_encode_f32_le(view.position_deltas, value_count);
        if (view.normal_deltas)
            normals = vscn_base64_encode_f32_le(view.normal_deltas, value_count);
        if (view.tangent_deltas)
            tangents = vscn_base64_encode_f32_le(view.tangent_deltas, value_count);
        if (!positions || (view.normal_deltas && !normals) || (view.tangent_deltas && !tangents)) {
            free(positions);
            free(normals);
            free(tangents);
            return 0;
        }
        ok = vscn_append(buf, len, cap, shape > 0 ? ",{\"name\": " : "{\"name\": ") &&
             vscn_append_json_string(buf, len, cap, view.name ? view.name : "") &&
             vscn_append(buf, len, cap, ", \"weight\": %.9g, \"positionBase64\": ", view.weight) &&
             vscn_append_json_string(buf, len, cap, positions);
        if (ok && normals)
            ok = vscn_append(buf, len, cap, ", \"normalBase64\": ") &&
                 vscn_append_json_string(buf, len, cap, normals);
        if (ok && tangents)
            ok = vscn_append(buf, len, cap, ", \"tangentBase64\": ") &&
                 vscn_append_json_string(buf, len, cap, tangents);
        if (ok)
            ok = vscn_append(buf, len, cap, "}");
        free(positions);
        free(normals);
        free(tangents);
        if (!ok)
            return 0;
    }
    return vscn_append(buf, len, cap, "]}");
}

/// @brief Emit a mesh as JSON: vertex/index buffers as base64 + the layout descriptor.
/// @param mesh Borrowed validated Mesh3D payload.
/// @param ctx Borrowed save context containing stable skeleton indices.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param depth JSON indentation depth.
/// @return Nonzero on success, otherwise zero.
static int vscn_serialize_mesh(
    rt_mesh3d *mesh, vscn_save_context_t *ctx, char **buf, size_t *len, size_t *cap, int depth) {
    char indent[64];
    size_t vertex_bytes_len;
    size_t index_bytes_len;
    char *vertex_base64 = NULL;
    char *index_base64 = NULL;

    if (!mesh)
        return 0;
    if (mesh->vertex_count > 0 && !mesh->vertices)
        return 0;
    if (mesh->index_count > 0 && !mesh->indices)
        return 0;
    if ((size_t)mesh->vertex_count > SIZE_MAX / sizeof(vgfx3d_vertex_t) ||
        (size_t)mesh->index_count > SIZE_MAX / sizeof(uint32_t))
        return 0;
    vertex_bytes_len = (size_t)mesh->vertex_count * sizeof(vgfx3d_vertex_t);
    index_bytes_len = (size_t)mesh->index_count * sizeof(uint32_t);
    vscn_make_indent(indent, sizeof(indent), depth);
    vertex_base64 = vscn_base64_encode((const uint8_t *)mesh->vertices, vertex_bytes_len, NULL);
    index_base64 = vscn_base64_encode((const uint8_t *)mesh->indices, index_bytes_len, NULL);
    if (!vertex_base64 || !index_base64) {
        free(vertex_base64);
        free(index_base64);
        return 0;
    }

    {
        int32_t skeleton_index = -1;
        if (ctx && rt_g3d_has_class(mesh->skeleton_ref, RT_G3D_SKELETON3D_CLASS_ID)) {
            for (int32_t i = 0; i < ctx->skeletons.count; i++) {
                if (ctx->skeletons.items[i] == mesh->skeleton_ref) {
                    skeleton_index = i;
                    break;
                }
            }
        }
        int ok = vscn_append(buf,
                             len,
                             cap,
                             "%s{\"vertexFormat\": \"vgfx3d_vertex_le_v2\", "
                             "\"vertexCount\": %u, "
                             "\"indexCount\": %u, "
                             "\"boneCount\": %d, "
                             "\"skeletonIndex\": %d, "
                             "\"resident\": %s, "
                             "\"verticesBase64\": ",
                             indent,
                             mesh->vertex_count,
                             mesh->index_count,
                             mesh->bone_count,
                             skeleton_index,
                             rt_mesh3d_get_resident(mesh) ? "true" : "false") &&
                 vscn_append_json_string(buf, len, cap, vertex_base64) &&
                 vscn_append(buf, len, cap, ", \"indicesBase64\": ") &&
                 vscn_append_json_string(buf, len, cap, index_base64);
        free(vertex_base64);
        free(index_base64);
        if (!ok)
            return 0;
        /* Optional v3 rig side streams. */
        if (mesh->bone_map && mesh->bone_count > 0) {
            char *map64 = vscn_base64_encode(
                (const uint8_t *)mesh->bone_map, (size_t)mesh->bone_count * sizeof(int32_t), NULL);
            if (!map64)
                return 0;
            ok = vscn_append(buf, len, cap, ", \"boneMapBase64\": ") &&
                 vscn_append_json_string(buf, len, cap, map64);
            free(map64);
            if (!ok)
                return 0;
        }
        if (mesh->extra_influences && mesh->vertex_count > 0) {
            char *extra64 =
                vscn_base64_encode((const uint8_t *)mesh->extra_influences,
                                   (size_t)mesh->vertex_count * sizeof(vgfx3d_extra_influences_t),
                                   NULL);
            if (!extra64)
                return 0;
            ok = vscn_append(buf, len, cap, ", \"extraInfluencesBase64\": ") &&
                 vscn_append_json_string(buf, len, cap, extra64);
            free(extra64);
            if (!ok)
                return 0;
        }
        if (ctx && ctx->asset_mode && !vscn_serialize_mesh_morph_targets(mesh, buf, len, cap))
            return 0;
        return vscn_append(buf, len, cap, "}");
    }
}

/// @brief Emit one node's sorted tagged-metadata object body ("{...}").
/// @details Integer values use canonical decimal strings to avoid JSON-number precision loss.
/// @param node Borrowed node owning the sorted metadata table.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param indent Borrowed indentation prefix.
/// @return Nonzero on success, otherwise zero.
static int vscn_serialize_metadata_object(
    const rt_scene_node3d *node, char **buf, size_t *len, size_t *cap, const char *indent) {
    if (!node->metadata || node->metadata_count > RT_SCENE_NODE3D_MAX_METADATA_ENTRIES ||
        !vscn_append(buf, len, cap, "{\n"))
        return 0;
    for (int32_t index = 0; index < node->metadata_count; ++index) {
        const rt_scene3d_metadata_entry *entry = &node->metadata[index];
        const char *kind;
        if (!entry->key || entry->key_length <= 0 ||
            entry->key_length > RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES ||
            strlen(entry->key) != (size_t)entry->key_length ||
            (index > 0 && strcmp(node->metadata[index - 1].key, entry->key) >= 0))
            return 0;
        switch (entry->kind) {
            case RT_SCENE3D_METADATA_NULL:
                kind = "null";
                break;
            case RT_SCENE3D_METADATA_BOOL:
                kind = "bool";
                break;
            case RT_SCENE3D_METADATA_INT:
                kind = "int";
                break;
            case RT_SCENE3D_METADATA_FLOAT:
                kind = "float";
                break;
            case RT_SCENE3D_METADATA_STRING:
                kind = "string";
                break;
            default:
                return 0;
        }
        if (!vscn_append(buf, len, cap, "%s    ", indent) ||
            !vscn_append_json_string(buf, len, cap, entry->key) ||
            !vscn_append(buf, len, cap, ": {\"kind\": \"%s\"", kind))
            return 0;
        switch (entry->kind) {
            case RT_SCENE3D_METADATA_NULL:
                break;
            case RT_SCENE3D_METADATA_BOOL:
                if (!vscn_append(buf,
                                 len,
                                 cap,
                                 ", \"value\": %s",
                                 entry->value.bool_value ? "true" : "false"))
                    return 0;
                break;
            case RT_SCENE3D_METADATA_INT:
                if (!vscn_append(
                        buf, len, cap, ", \"value\": \"%" PRId64 "\"", entry->value.int_value))
                    return 0;
                break;
            case RT_SCENE3D_METADATA_FLOAT:
                if (!isfinite(entry->value.float_value) ||
                    !vscn_append(buf, len, cap, ", \"value\": %.17g", entry->value.float_value))
                    return 0;
                break;
            case RT_SCENE3D_METADATA_STRING:
                if (!entry->value.string_value.data || entry->value.string_value.length < 0 ||
                    entry->value.string_value.length > RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES ||
                    strlen(entry->value.string_value.data) !=
                        (size_t)entry->value.string_value.length ||
                    !vscn_append(buf, len, cap, ", \"value\": ") ||
                    !vscn_append_json_string(buf, len, cap, entry->value.string_value.data))
                    return 0;
                break;
        }
        if (!vscn_append(buf, len, cap, "}%s\n", index + 1 < node->metadata_count ? "," : ""))
            return 0;
    }
    return vscn_append(buf, len, cap, "%s  }", indent);
}

/// @brief Emit one node's optional tagged gameplay-metadata member.
/// @param node Borrowed SceneNode3D payload.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param indent Borrowed indentation prefix.
/// @return Nonzero on success or absent metadata, otherwise zero.
static int vscn_serialize_node_metadata(
    const rt_scene_node3d *node, char **buf, size_t *len, size_t *cap, const char *indent) {
    if (!node || node->metadata_count <= 0)
        return 1;
    if (!vscn_append(buf, len, cap, ",\n%s  \"metadata\": ", indent))
        return 0;
    return vscn_serialize_metadata_object(node, buf, len, cap, indent);
}

/// @brief Emit document-level root metadata ("  \"metadata\": {...},\n").
/// @details Root metadata carries scene-scoped conventions (bake.*, env.*;
///          ADR 0188) and shares the v6 tagged format and loader path.
/// @param root Borrowed scene root.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @return Nonzero on success or absent metadata, otherwise zero.
static int vscn_save_emit_root_metadata(
    const rt_scene_node3d *root, char **buf, size_t *len, size_t *cap) {
    if (!root || root->metadata_count <= 0)
        return 1;
    if (!vscn_append(buf, len, cap, "  \"metadata\": "))
        return 0;
    if (!vscn_serialize_metadata_object(root, buf, len, cap, ""))
        return 0;
    return vscn_append(buf, len, cap, ",\n");
}

/// @brief Emit the fields of one scene node, leaving its JSON object open for children.
/// @param node Borrowed validated SceneNode3D payload.
/// @param ctx Borrowed save context containing stable resource indices.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param depth JSON indentation depth.
/// @return Nonzero on success, otherwise zero.
static int vscn_serialize_node_fields(rt_scene_node3d *node,
                                      vscn_save_context_t *ctx,
                                      char **buf,
                                      size_t *len,
                                      size_t *cap,
                                      int depth) {
    if (!node)
        return 1;
    char indent[64];
    vscn_make_indent(indent, sizeof(indent), depth);

    if (node->prefab_path) {
        /* ADR 0187: identity + reference + metadata only. */
        const char *prefab_name = node->name ? rt_string_cstr(node->name) : "node";
        const char *prefab_ref = rt_string_cstr(node->prefab_path);
        double prefab_rotation[4] = {
            node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]};
        if (!prefab_name)
            prefab_name = "node";
        if (!prefab_ref)
            return 0;
        vscn_normalize_quat(prefab_rotation);
        if (!vscn_append(buf, len, cap, "%s{\n", indent) ||
            !vscn_append(buf, len, cap, "%s  \"name\": ", indent) ||
            !vscn_append_json_string(buf, len, cap, prefab_name) ||
            !vscn_append(buf,
                         len,
                         cap,
                         ",\n%s  \"position\": [%.17g, %.17g, %.17g],\n",
                         indent,
                         vscn_clamp_abs_or(node->position[0], 0.0),
                         vscn_clamp_abs_or(node->position[1], 0.0),
                         vscn_clamp_abs_or(node->position[2], 0.0)) ||
            !vscn_append(buf,
                         len,
                         cap,
                         "%s  \"rotation\": [%.17g, %.17g, %.17g, %.17g],\n",
                         indent,
                         prefab_rotation[0],
                         prefab_rotation[1],
                         prefab_rotation[2],
                         prefab_rotation[3]) ||
            !vscn_append(buf,
                         len,
                         cap,
                         "%s  \"scale\": [%.17g, %.17g, %.17g],\n",
                         indent,
                         vscn_clamp_abs_or(node->scale_xyz[0], 1.0),
                         vscn_clamp_abs_or(node->scale_xyz[1], 1.0),
                         vscn_clamp_abs_or(node->scale_xyz[2], 1.0)) ||
            !vscn_append(buf,
                         len,
                         cap,
                         "%s  \"visible\": %s,\n",
                         indent,
                         node->visible ? "true" : "false") ||
            !vscn_append(buf, len, cap, "%s  \"prefab\": ", indent) ||
            !vscn_append_json_string(buf, len, cap, prefab_ref))
            return 0;
        return vscn_serialize_node_metadata(node, buf, len, cap, indent);
    }

    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(node->mesh, RT_G3D_MESH3D_CLASS_ID);
    rt_material3d *material =
        (rt_material3d *)rt_g3d_checked_or_null(node->material, RT_G3D_MATERIAL3D_CLASS_ID);
    const char *name = node->name ? rt_string_cstr(node->name) : "node";
    if (!name)
        name = "node";
    if ((node->mesh && !mesh) || (node->material && !material) ||
        (node->light && !rt_g3d_has_class(node->light, RT_G3D_LIGHT3D_CLASS_ID)) ||
        (ctx->asset_mode && node->import_index < -1))
        return 0;
    double rotation[4] = {
        node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]};
    vscn_normalize_quat(rotation);
    int mesh_index = mesh ? vscn_ptr_table_index_or_add(&ctx->meshes, mesh) : -1;
    int material_index = material ? vscn_ptr_table_index_or_add(&ctx->materials, material) : -1;
    if ((mesh && mesh_index < 0) || (material && material_index < 0))
        return 0;

    if (!vscn_append(buf, len, cap, "%s{\n", indent) ||
        !vscn_append(buf, len, cap, "%s  \"name\": ", indent) ||
        !vscn_append_json_string(buf, len, cap, name) ||
        !vscn_append(buf,
                     len,
                     cap,
                     ",\n%s  \"position\": [%.17g, %.17g, %.17g],\n",
                     indent,
                     vscn_clamp_abs_or(node->position[0], 0.0),
                     vscn_clamp_abs_or(node->position[1], 0.0),
                     vscn_clamp_abs_or(node->position[2], 0.0)) ||
        !vscn_append(buf,
                     len,
                     cap,
                     "%s  \"rotation\": [%.17g, %.17g, %.17g, %.17g],\n",
                     indent,
                     rotation[0],
                     rotation[1],
                     rotation[2],
                     rotation[3]) ||
        !vscn_append(buf,
                     len,
                     cap,
                     "%s  \"scale\": [%.17g, %.17g, %.17g],\n",
                     indent,
                     vscn_clamp_abs_or(node->scale_xyz[0], 1.0),
                     vscn_clamp_abs_or(node->scale_xyz[1], 1.0),
                     vscn_clamp_abs_or(node->scale_xyz[2], 1.0)) ||
        !vscn_append(
            buf, len, cap, "%s  \"visible\": %s,\n", indent, node->visible ? "true" : "false") ||
        /* Authoring flags round-trip so bakes and binding sync behave the same
         * after reload. Impostor ASSETS are intentionally transient — they are
         * generated proxies (pixels/meshes) rebuilt by the impostor bake. */
        !vscn_append(
            buf, len, cap, "%s  \"isStatic\": %s,\n", indent, node->is_static ? "true" : "false") ||
        !vscn_append(buf, len, cap, "%s  \"syncMode\": %d,\n", indent, (int)node->sync_mode) ||
        !vscn_append(buf, len, cap, "%s  \"hasMesh\": %s,\n", indent, mesh ? "true" : "false") ||
        !vscn_append(
            buf, len, cap, "%s  \"hasMaterial\": %s,\n", indent, material ? "true" : "false") ||
        !vscn_append(buf,
                     len,
                     cap,
                     "%s  \"mesh\": %d,\n%s  \"material\": %d",
                     indent,
                     mesh_index,
                     indent,
                     material_index)) {
        return 0;
    }

    if (ctx->asset_mode &&
        !vscn_append(buf, len, cap, ",\n%s  \"importIndex\": %d", indent, node->import_index))
        return 0;

    if (ctx->asset_mode && node->variant_material_count > 0) {
        if (!node->variant_materials || node->variant_material_count > ctx->variant_count ||
            !vscn_append(buf, len, cap, ",\n%s  \"variantMaterials\": [", indent))
            return 0;
        for (int32_t i = 0; i < ctx->variant_count; ++i) {
            int variant_index = -1;
            void *variant_material =
                i < node->variant_material_count ? node->variant_materials[i] : NULL;
            if (variant_material) {
                if (!rt_g3d_has_class(variant_material, RT_G3D_MATERIAL3D_CLASS_ID))
                    return 0;
                variant_index = vscn_ptr_table_index_or_add(&ctx->materials, variant_material);
                if (variant_index < 0)
                    return 0;
            }
            if (!vscn_append(buf, len, cap, i > 0 ? ", %d" : "%d", variant_index))
                return 0;
        }
        if (!vscn_append(buf, len, cap, "]"))
            return 0;
    }

    if (ctx->output_version >= 5 && node->camera) {
        int camera_index = vscn_ptr_table_index_or_add(&ctx->cameras, node->camera);
        if (camera_index < 0 ||
            !vscn_append(buf, len, cap, ",\n%s  \"camera\": %d", indent, camera_index))
            return 0;
    }

    if (node->light) {
        rt_light3d *light = (rt_light3d *)node->light;
        double light_dir[3] = {vscn_clamp_abs_or(light->direction[0], 0.0),
                               vscn_clamp_abs_or(light->direction[1], 0.0),
                               vscn_clamp_abs_or(light->direction[2], -1.0)};
        vscn_normalize_vec3(light_dir, 0.0, 0.0, -1.0);
        if (!vscn_append(buf,
                         len,
                         cap,
                         ",\n%s  \"light\": {\"type\": %d, "
                         "\"direction\": [%.17g, %.17g, %.17g], "
                         "\"position\": [%.17g, %.17g, %.17g], "
                         "\"color\": [%.17g, %.17g, %.17g], "
                         "\"intensity\": %.17g, \"attenuation\": %.17g, "
                         "\"innerCos\": %.17g, \"outerCos\": %.17g, "
                         "\"castsShadows\": %s",
                         indent,
                         (light->type >= 0 && light->type <= (ctx->output_version >= 5 ? 6 : 3))
                             ? light->type
                             : 1,
                         light_dir[0],
                         light_dir[1],
                         light_dir[2],
                         vscn_clamp_abs_or(light->position[0], 0.0),
                         vscn_clamp_abs_or(light->position[1], 0.0),
                         vscn_clamp_abs_or(light->position[2], 0.0),
                         vscn_clamp_or(light->color[0], 1.0, 0.0, VSCN_ABS_MAX),
                         vscn_clamp_or(light->color[1], 1.0, 0.0, VSCN_ABS_MAX),
                         vscn_clamp_or(light->color[2], 1.0, 0.0, VSCN_ABS_MAX),
                         vscn_nonnegative_or(light->intensity, 1.0),
                         vscn_nonnegative_or(light->attenuation, 0.0),
                         vscn_clamp_or(light->inner_cos, 1.0, 0.0, 1.0),
                         vscn_clamp_or(light->outer_cos, 0.7071067811865476, 0.0, 1.0),
                         light->casts_shadows ? "true" : "false")) {
            return 0;
        }
        if (ctx->output_version >= 5) {
            if (!vscn_append(buf,
                             len,
                             cap,
                             ", \"enabled\": %s, \"basisU\": [%.17g, %.17g, %.17g], "
                             "\"basisV\": [%.17g, %.17g, %.17g], \"width\": %.17g, "
                             "\"height\": %.17g, \"radius\": %.17g, \"range\": %.17g, "
                             "\"decayType\": %d}",
                             light->enabled ? "true" : "false",
                             vscn_clamp_abs_or(light->basis_u[0], 1.0),
                             vscn_clamp_abs_or(light->basis_u[1], 0.0),
                             vscn_clamp_abs_or(light->basis_u[2], 0.0),
                             vscn_clamp_abs_or(light->basis_v[0], 0.0),
                             vscn_clamp_abs_or(light->basis_v[1], 1.0),
                             vscn_clamp_abs_or(light->basis_v[2], 0.0),
                             vscn_nonnegative_or(light->width, 1.0),
                             vscn_nonnegative_or(light->height, 1.0),
                             vscn_nonnegative_or(light->radius, 1.0),
                             vscn_nonnegative_or(light->range, 0.0),
                             light->decay_type >= 0 && light->decay_type <= 3 ? light->decay_type
                                                                              : 2))
                return 0;
        } else if (!vscn_append(buf, len, cap, "}")) {
            return 0;
        }
    }

    int32_t lod_count = scene3d_node_lod_count(node);
    if (lod_count > 0) {
        if (!vscn_append(buf, len, cap, ",\n%s  \"lod\": [\n", indent))
            return 0;
        for (int32_t i = 0; i < lod_count; i++) {
            void *lod_mesh = rt_g3d_has_class(node->lod_levels[i].mesh, RT_G3D_MESH3D_CLASS_ID)
                                 ? node->lod_levels[i].mesh
                                 : NULL;
            int lod_mesh_index =
                lod_mesh ? vscn_ptr_table_index_or_add(&ctx->meshes, lod_mesh) : -1;
            if (lod_mesh && lod_mesh_index < 0)
                return 0;
            if (!vscn_append(buf,
                             len,
                             cap,
                             "%s    {\"distance\": %.17g, \"hasMesh\": %s, \"mesh\": %d}%s\n",
                             indent,
                             vscn_nonnegative_or(node->lod_levels[i].distance, 0.0),
                             lod_mesh ? "true" : "false",
                             lod_mesh_index,
                             i + 1 < lod_count ? "," : "")) {
                return 0;
            }
        }
        if (!vscn_append(buf, len, cap, "%s  ]", indent))
            return 0;
    }

    if (node->auto_lod_enabled) {
        if (!vscn_append(buf,
                         len,
                         cap,
                         ",\n%s  \"autoLOD\": {\"enabled\": true, \"screenErrorPx\": %.17g}",
                         indent,
                         vscn_nonnegative_or(node->auto_lod_screen_error_px, 8.0))) {
            return 0;
        }
    }

    return vscn_serialize_node_metadata(node, buf, len, cap, indent);
}

/// @brief Iteratively emit a scene-node subtree as nested JSON objects.
///
/// @details The explicit frame stack makes the format-depth limit independent of the C call stack.
/// A root node occupies level one; save and load therefore accept exactly the same maximum
/// nesting.
/// @param node Borrowed subtree root.
/// @param ctx Borrowed save context containing stable resource indices.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param depth Initial JSON indentation depth.
/// @return Nonzero after iterative subtree serialization, otherwise zero.
static int vscn_serialize_node(rt_scene_node3d *node,
                               vscn_save_context_t *ctx,
                               char **buf,
                               size_t *len,
                               size_t *cap,
                               int depth) {
    typedef struct vscn_serialize_node_frame {
        rt_scene_node3d *node;
        int32_t next_child;
        int32_t child_count;
        int32_t emitted_children;
        int depth;
        int children_open;
    } vscn_serialize_node_frame_t;

    vscn_serialize_node_frame_t *frames;
    size_t frame_count = 0;
    int ok = 1;

    if (!node)
        return 1;
    frames = (vscn_serialize_node_frame_t *)calloc(VSCN_MAX_NODE_DEPTH, sizeof(*frames));
    if (!frames)
        return 0;
    frames[frame_count++] = (vscn_serialize_node_frame_t){
        node, 0, node->prefab_path ? 0 : scene3d_node_child_count(node), 0, depth, 0};
    if (!vscn_serialize_node_fields(node, ctx, buf, len, cap, depth))
        ok = 0;

    while (ok && frame_count > 0) {
        vscn_serialize_node_frame_t *frame = &frames[frame_count - 1];
        char indent[64];
        rt_scene_node3d *child = NULL;

        vscn_make_indent(indent, sizeof(indent), frame->depth);
        if (!frame->children_open) {
            if (frame->child_count <= 0) {
                ok = vscn_append(buf, len, cap, "\n%s}", indent);
                frame_count--;
                continue;
            }
            ok = vscn_append(buf, len, cap, ",\n%s  \"children\": [\n", indent);
            frame->children_open = 1;
            if (!ok)
                break;
        }

        while (frame->next_child < frame->child_count && !child) {
            child = scene_node3d_checked(frame->node->children[frame->next_child++]);
        }
        if (!child) {
            ok = vscn_append(buf, len, cap, "\n%s  ]\n%s}", indent, indent);
            frame_count--;
            continue;
        }
        if (frame->emitted_children > 0 && !vscn_append(buf, len, cap, ",\n")) {
            ok = 0;
            break;
        }
        frame->emitted_children++;
        if (frame_count >= VSCN_MAX_NODE_DEPTH) {
            ok = 0;
            break;
        }
        frames[frame_count++] = (vscn_serialize_node_frame_t){
            child,
            0,
            child->prefab_path ? 0 : scene3d_node_child_count(child),
            0,
            frame->depth + 2,
            0};
        if (!vscn_serialize_node_fields(child, ctx, buf, len, cap, frame->depth + 2))
            ok = 0;
    }

    free(frames);
    return ok;
}

/// @brief Public API: save a Scene3D to a `.vscn` file at `path`.
///
/// Two-pass algorithm: (1) walk the scene to populate the mesh /
/// material / texture / cubemap dedupe tables, (2) emit the JSON
/// document with shared assets first, then nodes referencing them
/// by index. Output is pretty-printed for diff-friendliness.
/// @return 1 on success, 0 on any failure (open, alloc, write).
/// @brief Free the four shared-asset pointer tables collected for a save (every failure exit).
/// @param ctx Borrowed mutable save context whose native pointer arrays are released.
static void vscn_save_free_ctx(vscn_save_context_t *ctx) {
    vscn_free_ptr_table(&ctx->meshes);
    vscn_free_ptr_table(&ctx->materials);
    vscn_free_ptr_table(&ctx->textures);
    vscn_free_ptr_table(&ctx->cubemaps);
    vscn_free_ptr_table(&ctx->skeletons);
    vscn_free_ptr_table(&ctx->cameras);
}

/// @brief True when the texture table contains at least one still-valid exact source container.
/// @param ctx Borrowed populated save context.
/// @return Nonzero when any collected TextureAsset3D preserves encoded source bytes.
static int vscn_save_has_source_texture(const vscn_save_context_t *ctx) {
    if (!ctx)
        return 0;
    for (int32_t i = 0; i < ctx->textures.count; ++i) {
        const uint8_t *data = NULL;
        uint64_t size = 0;
        const char *kind = NULL;
        if (rt_textureasset3d_get_source_container(ctx->textures.items[i], &data, &size, &kind) &&
            data && size > 0 && kind)
            return 1;
    }
    return 0;
}

/// @brief Emit the `"textures": [ ... ],` array. @return 1 on success, 0 on append failure.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
static int vscn_save_emit_textures(char **buf, size_t *len, size_t *cap, vscn_save_context_t *ctx) {
    if (!vscn_append(buf, len, cap, "  \"textures\": [\n"))
        return 0;
    for (int32_t i = 0; i < ctx->textures.count; i++) {
        if (!vscn_serialize_texture(
                ctx->textures.items[i], buf, len, cap, 2, ctx->output_version) ||
            (i < ctx->textures.count - 1 && !vscn_append(buf, len, cap, ",")) ||
            !vscn_append(buf, len, cap, "\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ],\n");
}

/// @brief Emit the `"cubemaps": [ ... ],` array. @return 1 on success, 0 on append failure.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
static int vscn_save_emit_cubemaps(char **buf, size_t *len, size_t *cap, vscn_save_context_t *ctx) {
    if (!vscn_append(buf, len, cap, "  \"cubemaps\": [\n"))
        return 0;
    for (int32_t i = 0; i < ctx->cubemaps.count; i++) {
        if (!vscn_serialize_cubemap(
                (rt_cubemap3d *)ctx->cubemaps.items[i], ctx, buf, len, cap, 2) ||
            (i < ctx->cubemaps.count - 1 && !vscn_append(buf, len, cap, ",")) ||
            !vscn_append(buf, len, cap, "\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ],\n");
}

/// @brief Emit the `"materials": [ ... ],` array. @return 1 on success, 0 on append failure.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
static int vscn_save_emit_materials(char **buf,
                                    size_t *len,
                                    size_t *cap,
                                    vscn_save_context_t *ctx) {
    if (!vscn_append(buf, len, cap, "  \"materials\": [\n"))
        return 0;
    for (int32_t i = 0; i < ctx->materials.count; i++) {
        if (!vscn_serialize_material(
                (rt_material3d *)ctx->materials.items[i], ctx, buf, len, cap, 2) ||
            (i < ctx->materials.count - 1 && !vscn_append(buf, len, cap, ",")) ||
            !vscn_append(buf, len, cap, "\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ],\n");
}

/// @brief Emit the v3 `"skeletons": [ ... ],` array (structured JSON per bone so
///   the file survives internal struct changes). Empty table emits nothing.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
/// @return Nonzero on success, otherwise zero.
static int vscn_save_emit_skeletons(char **buf,
                                    size_t *len,
                                    size_t *cap,
                                    vscn_save_context_t *ctx) {
    if (ctx->skeletons.count <= 0)
        return 1;
    if (!vscn_append(buf, len, cap, "  \"skeletons\": [\n"))
        return 0;
    for (int32_t i = 0; i < ctx->skeletons.count; i++) {
        rt_skeleton3d *skel = (rt_skeleton3d *)ctx->skeletons.items[i];
        int32_t bone_count = skeleton3d_safe_bone_count(skel);
        if (!vscn_append(buf, len, cap, "    {\"bones\": [\n"))
            return 0;
        for (int32_t b = 0; b < bone_count; b++) {
            const vgfx3d_bone_t *bone = &skel->bones[b];
            if (!vscn_append(buf, len, cap, "      {\"name\": ") ||
                !vscn_append_json_string(
                    buf, len, cap, bone->name ? rt_string_cstr(bone->name) : "") ||
                !vscn_append(
                    buf, len, cap, ", \"parent\": %d, \"bindLocal\": [", bone->parent_index))
                return 0;
            for (int k = 0; k < 16; k++) {
                if (!vscn_append(
                        buf, len, cap, k ? ",%.9g" : "%.9g", (double)bone->bind_pose_local[k]))
                    return 0;
            }
            if (!vscn_append(buf, len, cap, "], \"inverseBind\": ["))
                return 0;
            for (int k = 0; k < 16; k++) {
                if (!vscn_append(
                        buf, len, cap, k ? ",%.9g" : "%.9g", (double)bone->inverse_bind[k]))
                    return 0;
            }
            if (!vscn_append(buf, len, cap, b < bone_count - 1 ? "]},\n" : "]}\n"))
                return 0;
        }
        if (!vscn_append(buf, len, cap, i < ctx->skeletons.count - 1 ? "    ]},\n" : "    ]}\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ],\n");
}

/// @brief Emit the v3 `"animations": [ ... ],` array. Channels serialize their
///   keyframe arrays as raw little-endian structs tagged with a format name so
///   the loader can reject layout drift.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
/// @return Nonzero on success, otherwise zero.
static int vscn_save_emit_animations(char **buf,
                                     size_t *len,
                                     size_t *cap,
                                     vscn_save_context_t *ctx) {
    if (!ctx->animations || ctx->animation_count <= 0)
        return 1;
    if (!vscn_append(buf, len, cap, "  \"animations\": [\n"))
        return 0;
    for (int32_t i = 0; i < ctx->animation_count; i++) {
        rt_animation3d *anim = (rt_animation3d *)ctx->animations[i];
        int32_t channel_count;
        if (!rt_g3d_has_class(anim, RT_G3D_ANIMATION3D_CLASS_ID))
            continue;
        channel_count = animation3d_safe_channel_count(anim);
        if (!vscn_append(buf, len, cap, "    {\"name\": ") ||
            !vscn_append_json_string(buf, len, cap, anim->name) ||
            !vscn_append(buf,
                         len,
                         cap,
                         ", \"duration\": %.9g, \"looping\": %s, "
                         "\"keyframeFormat\": \"vgfx3d_keyframe_le_v2\", "
                         "\"channels\": [\n",
                         (double)anim->duration,
                         anim->looping ? "true" : "false"))
            return 0;
        for (int32_t c = 0; c < channel_count; c++) {
            const vgfx3d_anim_channel_t *ch = &anim->channels[c];
            int32_t key_count = animation3d_safe_keyframe_count(ch);
            char *keys64 = vscn_base64_encode((const uint8_t *)ch->keyframes,
                                              (size_t)key_count * sizeof(vgfx3d_keyframe_t),
                                              NULL);
            int ok;
            if (!keys64)
                return 0;
            ok = vscn_append(buf,
                             len,
                             cap,
                             "      {\"bone\": %d, \"keyCount\": %d, "
                             "\"keyframesBase64\": ",
                             ch->bone_index,
                             key_count) &&
                 vscn_append_json_string(buf, len, cap, keys64) &&
                 vscn_append(buf, len, cap, c < channel_count - 1 ? "},\n" : "}\n");
            free(keys64);
            if (!ok)
                return 0;
        }
        if (!vscn_append(buf, len, cap, i < ctx->animation_count - 1 ? "    ]},\n" : "    ]}\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ],\n");
}

/// @brief Emit complete VSCN v5 node/object/morph/camera animation clips.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
/// @return Nonzero on success, otherwise zero.
static int vscn_save_emit_node_animations(char **buf,
                                          size_t *len,
                                          size_t *cap,
                                          vscn_save_context_t *ctx) {
    if (!vscn_append(buf, len, cap, "  \"nodeAnimations\": [\n"))
        return 0;
    for (int32_t i = 0; i < ctx->node_animation_count; ++i) {
        rt_node_animation3d *animation = (rt_node_animation3d *)rt_g3d_checked_or_null(
            ctx->node_animations[i], RT_G3D_NODEANIMATION3D_CLASS_ID);
        int32_t channel_count;
        const char *name;
        if (!animation || !isfinite(animation->duration) || animation->duration <= 0.0)
            return 0;
        name = animation->name && rt_string_is_handle(animation->name)
                   ? rt_string_cstr(animation->name)
                   : NULL;
        channel_count = scene3d_node_animation_channel_count(animation);
        if (!name || !vscn_append(buf, len, cap, "    {\"name\": ") ||
            !vscn_append_json_string(buf, len, cap, name) ||
            !vscn_append(buf,
                         len,
                         cap,
                         ", \"duration\": %.17g, \"looping\": %s, "
                         "\"sampleFormat\": \"f64le-f32le-v1\", \"channels\": [\n",
                         animation->duration,
                         animation->looping ? "true" : "false"))
            return 0;
        for (int32_t channel_index = 0; channel_index < channel_count; ++channel_index) {
            const rt_node_anim_channel3d *channel = &animation->channels[channel_index];
            const char *target;
            size_t value_count;
            char *times = NULL;
            char *values = NULL;
            char *in_tangents = NULL;
            char *out_tangents = NULL;
            int ok;
            if (!channel->target_name || !rt_string_is_handle(channel->target_name) ||
                channel->key_count <= 0 || channel->value_width <= 0 || !channel->times ||
                !channel->values || channel->path < RT_NODE_ANIM_PATH_TRANSLATION ||
                channel->path > RT_NODE_ANIM_PATH_LAST ||
                channel->interpolation < RT_NODE_ANIM_INTERP_LINEAR ||
                channel->interpolation > RT_NODE_ANIM_INTERP_CUBICSPLINE ||
                (size_t)channel->key_count > SIZE_MAX / (size_t)channel->value_width)
                return 0;
            target = rt_string_cstr(channel->target_name);
            value_count = (size_t)channel->key_count * (size_t)channel->value_width;
            for (int32_t key = 0; key < channel->key_count; ++key) {
                if (!isfinite(channel->times[key]))
                    return 0;
            }
            for (size_t value = 0; value < value_count; ++value) {
                if (!isfinite(channel->values[value]) ||
                    (channel->interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE &&
                     (!channel->in_tangents || !channel->out_tangents ||
                      !isfinite(channel->in_tangents[value]) ||
                      !isfinite(channel->out_tangents[value]))))
                    return 0;
            }
            times = vscn_base64_encode_f64_le(channel->times, (size_t)channel->key_count);
            values = vscn_base64_encode_f32_le(channel->values, value_count);
            if (channel->interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE) {
                in_tangents = vscn_base64_encode_f32_le(channel->in_tangents, value_count);
                out_tangents = vscn_base64_encode_f32_le(channel->out_tangents, value_count);
            }
            if (!target || target[0] == '\0' || !times || !values ||
                (channel->interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE &&
                 (!in_tangents || !out_tangents))) {
                free(times);
                free(values);
                free(in_tangents);
                free(out_tangents);
                return 0;
            }
            ok = vscn_append(buf, len, cap, "      {\"target\": ") &&
                 vscn_append_json_string(buf, len, cap, target) &&
                 vscn_append(buf,
                             len,
                             cap,
                             ", \"targetNode\": %d, \"path\": %d, "
                             "\"interpolation\": %d, \"keyCount\": %d, "
                             "\"valueWidth\": %d, \"timesBase64\": ",
                             channel->target_node_index,
                             channel->path,
                             channel->interpolation,
                             channel->key_count,
                             channel->value_width) &&
                 vscn_append_json_string(buf, len, cap, times) &&
                 vscn_append(buf, len, cap, ", \"valuesBase64\": ") &&
                 vscn_append_json_string(buf, len, cap, values);
            if (ok && in_tangents)
                ok = vscn_append(buf, len, cap, ", \"inTangentsBase64\": ") &&
                     vscn_append_json_string(buf, len, cap, in_tangents) &&
                     vscn_append(buf, len, cap, ", \"outTangentsBase64\": ") &&
                     vscn_append_json_string(buf, len, cap, out_tangents);
            if (ok)
                ok = vscn_append(buf, len, cap, channel_index + 1 < channel_count ? "},\n" : "}\n");
            free(times);
            free(values);
            free(in_tangents);
            free(out_tangents);
            if (!ok)
                return 0;
        }
        if (!vscn_append(
                buf, len, cap, i + 1 < ctx->node_animation_count ? "    ]},\n" : "    ]}\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ],\n");
}

/// @brief Emit the deduplicated VSCN v5 Camera3D table.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
/// @return Nonzero on success, otherwise zero.
static int vscn_save_emit_cameras(char **buf, size_t *len, size_t *cap, vscn_save_context_t *ctx) {
    if (!vscn_append(buf, len, cap, "  \"cameras\": [\n"))
        return 0;
    for (int32_t i = 0; i < ctx->cameras.count; ++i) {
        rt_camera3d *camera =
            (rt_camera3d *)rt_g3d_checked_or_null(ctx->cameras.items[i], RT_G3D_CAMERA3D_CLASS_ID);
        if (!camera || !isfinite(camera->fov) || !isfinite(camera->aspect) ||
            !isfinite(camera->near_plane) || !isfinite(camera->far_plane) ||
            !isfinite(camera->ortho_size) || camera->aspect <= 0.0 || camera->near_plane <= 0.0 ||
            camera->far_plane <= camera->near_plane || camera->ortho_size <= 0.0 ||
            (!camera->is_ortho && (camera->fov <= 0.0 || camera->fov >= 180.0)) ||
            !vscn_append(buf,
                         len,
                         cap,
                         "    {\"isOrtho\": %s, \"fov\": %.17g, \"aspect\": %.17g, "
                         "\"near\": %.17g, \"far\": %.17g, \"orthoSize\": %.17g, "
                         "\"eye\": [",
                         camera->is_ortho ? "true" : "false",
                         camera->fov,
                         camera->aspect,
                         camera->near_plane,
                         camera->far_plane,
                         camera->ortho_size))
            return 0;
        for (int lane = 0; lane < 3; ++lane) {
            if (!isfinite(camera->eye[lane]) ||
                !vscn_append(buf, len, cap, lane ? ", %.17g" : "%.17g", camera->eye[lane]))
                return 0;
        }
        if (!vscn_append(buf, len, cap, "], \"view\": ["))
            return 0;
        for (int lane = 0; lane < 16; ++lane) {
            if (!isfinite(camera->view[lane]) ||
                !vscn_append(buf, len, cap, lane ? ", %.17g" : "%.17g", camera->view[lane]))
                return 0;
        }
        if (!vscn_append(buf, len, cap, i + 1 < ctx->cameras.count ? "]},\n" : "]}\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ],\n");
}

/// @brief Emit ordered VSCN v5 material-variant display names.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
/// @return Nonzero on success, otherwise zero.
static int vscn_save_emit_variant_names(char **buf,
                                        size_t *len,
                                        size_t *cap,
                                        vscn_save_context_t *ctx) {
    if (!vscn_append(buf, len, cap, "  \"variantNames\": ["))
        return 0;
    for (int32_t i = 0; i < ctx->variant_count; ++i) {
        if (!ctx->variant_names || !ctx->variant_names[i] ||
            (i > 0 && !vscn_append(buf, len, cap, ", ")) ||
            !vscn_append_json_string(buf, len, cap, ctx->variant_names[i]))
            return 0;
    }
    return vscn_append(buf, len, cap, "],\n");
}

/// @brief Emit the `"meshes": [ ... ],` array. @return 1 on success, 0 on append failure.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
static int vscn_save_emit_meshes(char **buf, size_t *len, size_t *cap, vscn_save_context_t *ctx) {
    if (!vscn_append(buf, len, cap, "  \"meshes\": [\n"))
        return 0;
    for (int32_t i = 0; i < ctx->meshes.count; i++) {
        if (!vscn_serialize_mesh((rt_mesh3d *)ctx->meshes.items[i], ctx, buf, len, cap, 2) ||
            (i < ctx->meshes.count - 1 && !vscn_append(buf, len, cap, ",")) ||
            !vscn_append(buf, len, cap, "\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ],\n");
}

/// @brief Emit the closing `"nodes": [ ... ]` array (root's children; root is implicit).
/// @return 1 on success, 0 on append failure.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
/// @param root Borrowed implicit scene root whose children are emitted.
static int vscn_save_emit_nodes(
    char **buf, size_t *len, size_t *cap, vscn_save_context_t *ctx, rt_scene_node3d *root) {
    int32_t child_count = scene3d_node_child_count(root);
    int32_t emitted = 0;
    if (!vscn_append(buf, len, cap, "  \"nodes\": [\n"))
        return 0;
    for (int32_t i = 0; i < child_count; i++) {
        if (!root->children[i])
            continue;
        rt_scene_node3d *child = scene_node3d_checked(root->children[i]);
        if (!child)
            continue;
        if (emitted > 0 && !vscn_append(buf, len, cap, ","))
            return 0;
        if (!vscn_serialize_node(child, ctx, buf, len, cap, 2) || !vscn_append(buf, len, cap, "\n"))
            return 0;
        emitted++;
    }
    return vscn_append(buf, len, cap, "  ]\n");
}

/// @brief Emit one synthetic scene root's children as a nested node array.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated save context.
/// @param root Borrowed synthetic scene root.
/// @return Nonzero on success, otherwise zero.
static int vscn_save_emit_scene_node_array(
    char **buf, size_t *len, size_t *cap, vscn_save_context_t *ctx, rt_scene_node3d *root) {
    int32_t child_count = scene3d_node_child_count(root);
    int32_t emitted = 0;
    if (!root || !vscn_append(buf, len, cap, "[\n"))
        return 0;
    for (int32_t i = 0; i < child_count; ++i) {
        rt_scene_node3d *child = scene_node3d_checked(root->children[i]);
        if (!child)
            continue;
        if (emitted > 0 && !vscn_append(buf, len, cap, ",\n"))
            return 0;
        if (!vscn_serialize_node(child, ctx, buf, len, cap, 3))
            return 0;
        emitted++;
    }
    return vscn_append(buf, len, cap, emitted > 0 ? "\n    ]" : "    ]");
}

/// @brief Emit ordered VSCN v5 immutable scenes and their camera memberships.
/// @param buf Address of output allocation.
/// @param len In/out populated byte length.
/// @param cap In/out byte capacity.
/// @param ctx Borrowed populated asset-mode save context.
/// @return Nonzero on success, otherwise zero.
static int vscn_save_emit_scenes(char **buf, size_t *len, size_t *cap, vscn_save_context_t *ctx) {
    if (!ctx->scenes || ctx->scene_count <= 0 || !vscn_append(buf, len, cap, "  \"scenes\": [\n"))
        return 0;
    for (int32_t scene_index = 0; scene_index < ctx->scene_count; ++scene_index) {
        const rt_vscn_asset_scene_view *scene = &ctx->scenes[scene_index];
        if (!scene->root || !scene->name || !vscn_append(buf, len, cap, "    {\"name\": ") ||
            !vscn_append_json_string(buf, len, cap, scene->name) ||
            !vscn_append(buf, len, cap, ", \"cameras\": ["))
            return 0;
        for (int32_t camera_index = 0; camera_index < scene->camera_count; ++camera_index) {
            int table_index = vscn_ptr_table_index(&ctx->cameras, scene->cameras[camera_index]);
            if (table_index < 0 ||
                !vscn_append(buf, len, cap, camera_index > 0 ? ", %d" : "%d", table_index))
                return 0;
        }
        if (!vscn_append(buf, len, cap, "], \"nodes\": ") ||
            !vscn_save_emit_scene_node_array(buf, len, cap, ctx, scene->root) ||
            !vscn_append(buf, len, cap, scene_index + 1 < ctx->scene_count ? "},\n" : "}\n"))
            return 0;
    }
    return vscn_append(buf, len, cap, "  ]\n");
}

/// @brief Atomically publish a completed VSCN text buffer at @p filepath.
/// @param filepath Borrowed UTF-8 destination path.
/// @param buf Borrowed complete serialized bytes.
/// @param len Number of bytes to write.
/// @return `1` after atomic replacement, otherwise `0`.
static int64_t vscn_write_atomic(const char *filepath, const char *buf, size_t len) {
    FILE *file;
    char *tmp_path = NULL;
    size_t written = 0;
    int64_t result;
    if (!filepath || !buf || len > VSCN_MAX_FILE_BYTES)
        return 0;
    file = rt_file_stdio_open_temp_for_replace_utf8(filepath, &tmp_path);
    if (!file)
        return 0;
    while (written < len) {
        size_t chunk = fwrite(buf + written, 1, len - written, file);
        if (chunk == 0) {
            fclose(file);
            (void)rt_file_stdio_unlink_utf8(tmp_path);
            free(tmp_path);
            return 0;
        }
        written += chunk;
    }
    if (fflush(file) != 0 || fclose(file) != 0) {
        (void)rt_file_stdio_unlink_utf8(tmp_path);
        free(tmp_path);
        return 0;
    }
    result = rt_file_stdio_replace_utf8(tmp_path, filepath) ? 1 : 0;
    if (!result)
        (void)rt_file_stdio_unlink_utf8(tmp_path);
    free(tmp_path);
    return result;
}

/// @brief Serialize a complete imported SceneAsset as VSCN v5.
/// @param view Borrowed complete imported asset inventory.
/// @param path Borrowed runtime-string destination path.
/// @return `1` on successful atomic save, otherwise `0`.
int64_t rt_vscn_save_asset_view(const rt_vscn_asset_save_view *view, rt_string path) {
    vscn_save_context_t ctx = {0};
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    const char *filepath;
    int64_t result;
    ctx.output_version = 5;
    if (!view || !path || !rt_string_is_handle(path))
        return 0;
    filepath = rt_string_cstr(path);
    if (!filepath || filepath[0] == '\0' || !vscn_collect_asset_view(view, &ctx)) {
        vscn_save_free_ctx(&ctx);
        return 0;
    }
    ctx.output_version = ctx.requires_v7 ? 7 : (ctx.requires_v6 ? 6 : 5);
    if (!vscn_append(&buf, &len, &cap, "{\n") ||
        !vscn_append(&buf, &len, &cap, "  \"format\": \"vscn\",\n") ||
        !vscn_append(&buf, &len, &cap, "  \"version\": %d,\n", ctx.output_version) ||
        !vscn_save_emit_textures(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_cubemaps(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_materials(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_skeletons(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_animations(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_node_animations(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_cameras(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_variant_names(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_meshes(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_scenes(&buf, &len, &cap, &ctx) || !vscn_append(&buf, &len, &cap, "}\n")) {
        vscn_save_free_ctx(&ctx);
        free(buf);
        return 0;
    }
    result = vscn_write_atomic(filepath, buf, len);
    vscn_save_free_ctx(&ctx);
    free(buf);
    return result;
}

/// @brief Build the complete canonical VSCN text for @p scene in one buffer.
/// @details Shared by the file and text save endpoints (ADR 0190) so version
///          selection and byte output are identical on both paths. On success
///          the caller owns the malloc'd @p *out_buf and must free it.
/// @param scene Borrowed validated Scene3D payload with a root.
/// @param out_buf Output receiving the caller-owned serialized allocation.
/// @param out_len Output receiving exact byte length.
/// @return 1 on success, 0 on collection or append failure.
static int scene3d_build_vscn_text(rt_scene3d *scene, char **out_buf, size_t *out_len) {
    vscn_save_context_t ctx = {0};
    char *buf = NULL;
    size_t len = 0, cap = 0;

    for (int32_t i = 0, child_count = scene3d_node_child_count(scene->root); i < child_count; i++) {
        if (!scene->root->children[i])
            continue;
        rt_scene_node3d *child = scene_node3d_checked(scene->root->children[i]);
        if (!child)
            continue;
        if (!vscn_collect_node_assets(child, &ctx)) {
            vscn_save_free_ctx(&ctx);
            return 0;
        }
    }

    ctx.animations = scene->baked_animations;
    ctx.animation_count = scene->baked_animation_count;
    if (scene->root->metadata_count > 0)
        ctx.requires_v6 = 1;

    /* Rig data (skeletons/animations/side streams) promotes the file to v3;
     * plain scenes keep emitting v2 so existing consumers see no change. */
    int has_rig = ctx.skeletons.count > 0 || ctx.animation_count > 0;
    int version;
    for (int32_t i = 0; !has_rig && i < ctx.meshes.count; i++) {
        const rt_mesh3d *m = (const rt_mesh3d *)ctx.meshes.items[i];
        if (m->bone_map || m->extra_influences)
            has_rig = 1;
    }
    version =
        ctx.requires_v7
            ? 7
            : (ctx.requires_v6
                   ? 6
                   : ((ctx.requires_v5 || vscn_save_has_source_texture(&ctx)) ? 5
                                                                              : (has_rig ? 3 : 2)));
    ctx.output_version = version;
    if (!vscn_append(&buf, &len, &cap, "{\n") ||
        !vscn_append(&buf, &len, &cap, "  \"format\": \"vscn\",\n") ||
        !vscn_append(&buf, &len, &cap, "  \"version\": %d,\n", version) ||
        !vscn_save_emit_root_metadata(scene->root, &buf, &len, &cap) ||
        !vscn_save_emit_textures(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_cubemaps(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_materials(&buf, &len, &cap, &ctx) ||
        (has_rig && !vscn_save_emit_skeletons(&buf, &len, &cap, &ctx)) ||
        (has_rig && !vscn_save_emit_animations(&buf, &len, &cap, &ctx)) ||
        (version >= 5 && !vscn_save_emit_cameras(&buf, &len, &cap, &ctx)) ||
        !vscn_save_emit_meshes(&buf, &len, &cap, &ctx) ||
        !vscn_save_emit_nodes(&buf, &len, &cap, &ctx, scene->root) ||
        !vscn_append(&buf, &len, &cap, "}\n")) {
        vscn_save_free_ctx(&ctx);
        free(buf);
        return 0;
    }

    vscn_save_free_ctx(&ctx);
    *out_buf = buf;
    *out_len = len;
    return 1;
}

/// @brief Serialize the scene to a .vscn file. @return 1 on success, 0 on failure.
/// @param scene_obj Borrowed Scene3D handle.
/// @param path Borrowed runtime-string destination path.
int64_t rt_scene3d_save(void *scene_obj, rt_string path) {
    if (!scene_obj || !path)
        return 0;
    rt_scene3d *scene = (rt_scene3d *)rt_g3d_checked_or_null(scene_obj, RT_G3D_SCENE3D_CLASS_ID);
    if (!scene)
        return 0;
    if (!scene->root)
        return 0;

    const char *filepath = rt_string_cstr(path);
    if (!filepath)
        return 0;

    char *buf = NULL;
    size_t len = 0;
    if (!scene3d_build_vscn_text(scene, &buf, &len))
        return 0;
    int64_t result = vscn_write_atomic(filepath, buf, len);
    free(buf);
    return result;
}

/// @brief Serialize the scene to canonical VSCN text entirely in memory (ADR 0190).
/// @details Byte-identical to what rt_scene3d_save would write, including the
///          version-selection rules and the 64 MB VSCN budget. Returns the
///          shared empty string on any failure, matching the 2D
///          SceneDocument.ToJson convention; no filesystem access occurs.
/// @param scene_obj Borrowed Scene3D handle.
/// @return New canonical runtime string, or the shared empty string on failure.
rt_string rt_scene3d_save_text(void *scene_obj) {
    if (!scene_obj)
        return rt_str_empty();
    rt_scene3d *scene = (rt_scene3d *)rt_g3d_checked_or_null(scene_obj, RT_G3D_SCENE3D_CLASS_ID);
    if (!scene || !scene->root)
        return rt_str_empty();

    char *buf = NULL;
    size_t len = 0;
    if (!scene3d_build_vscn_text(scene, &buf, &len))
        return rt_str_empty();
    if (len > VSCN_MAX_FILE_BYTES) {
        free(buf);
        return rt_str_empty();
    }
    rt_string text = rt_string_from_bytes(buf, len);
    free(buf);
    return text ? text : rt_str_empty();
}

#endif // ZANNA_ENABLE_GRAPHICS
