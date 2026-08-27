//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_vscn_load.c
// Purpose: Scene3D .vscn deserialization (load). Parses the JSON scene
//   document, decodes base64 asset payloads, and rebuilds the Scene3D /
//   SceneNode3D tree. Rolls back partial state on any error so a half-loaded
//   scene never reaches the caller.
//
// Key invariants:
//   - Loader validates structure and clamps numeric values before use.
//   - On any failure, all partially-loaded resources are released.
//   - VSCN v6 node metadata is strictly tagged and bounded before publication.
//
// Ownership/Lifetime:
//   - Produces Scene3D / SceneNode3D objects defined in rt_scene3d.c;
//     this TU owns no GC objects of its own once load completes.
//
// Links: rt_scene3d.h, rt_scene3d_internal.h, rt_scene3d_vscn_internal.h,
//        rt_scene3d_vscn_material_parse.inc (material parsers),
//        rt_scene3d_vscn_save.c (inverse: save), rt_json.h,
//        docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md,
//        docs/adr/0295-portable-vscn-binary-wire-layouts.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements strict, transactional loading of JSON-based VSCN scene assets.
/// @details The loader bounds untrusted counts and payload sizes, validates exact
///   numeric and tagged metadata representations, decodes endian-stable buffers,
///   rebuilds retained resources and node trees, resolves nested prefab references,
///   and releases every partial object on failure.

#include "rt_alloc_size.h"
#include "rt_platform_feature.h"

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_asset_error.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_file_stdio.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_morphtarget3d_internal.h"
#include "rt_object.h"
#include "rt_path.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_result.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_scene3d_vscn_internal.h"
#include "rt_seq.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"
#include "rt_trap.h"
#include "rt_untrusted_count.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define vscn_fseek(fp, off, whence) rt_file_stdio_seek64((fp), (off), (whence))
#define vscn_ftell(fp) rt_file_stdio_tell64((fp))

/// @brief Decode an encoded texture payload using a filename-selected built-in decoder.
/// @param name Borrowed synthetic filename providing the container extension.
/// @param data Borrowed encoded payload bytes.
/// @param size Number of readable bytes.
/// @return New owned decoded asset handle, or `NULL` on failure.
extern void *rt_asset_decode_typed(const char *name, const uint8_t *data, size_t size);

/// @brief Count the total number of nodes in the subtree rooted at `node` (inclusive).
/// @details Iterative so adversarially deep loaded hierarchies cannot overflow the C stack.
/// @param node Borrowed subtree root.
/// @return Non-negative node count, `INT32_MAX` on overflow, or `-1` on allocation failure.
static int32_t scene3d_count_subtree(const rt_scene_node3d *node) {
    if (!node)
        return 0;

    const rt_scene_node3d **stack = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int32_t total = 0;

    for (;;) {
        if (count >= capacity) {
            size_t new_capacity = capacity > 0 ? capacity * 2u : 64u;
            const rt_scene_node3d **grown;
            if (new_capacity <= capacity || new_capacity > SIZE_MAX / sizeof(rt_scene_node3d *)) {
                free(stack);
                return INT32_MAX;
            }
            grown = (const rt_scene_node3d **)realloc((void *)stack,
                                                      new_capacity * sizeof(rt_scene_node3d *));
            if (!grown) {
                free(stack);
                return -1;
            }
            stack = grown;
            capacity = new_capacity;
        }
        stack[count++] = node;
        break;
    }

    while (count > 0) {
        const rt_scene_node3d *current = stack[--count];
        if (total == INT32_MAX) {
            free(stack);
            return INT32_MAX;
        }
        total++;
        for (int32_t i = 0, child_count = scene3d_node_child_count(current); i < child_count; i++) {
            if (count >= capacity) {
                size_t new_capacity = capacity > 0 ? capacity * 2u : 64u;
                const rt_scene_node3d **grown;
                if (new_capacity <= capacity ||
                    new_capacity > SIZE_MAX / sizeof(rt_scene_node3d *)) {
                    free(stack);
                    return INT32_MAX;
                }
                grown = (const rt_scene_node3d **)realloc((void *)stack,
                                                          new_capacity * sizeof(rt_scene_node3d *));
                if (!grown) {
                    free(stack);
                    return -1;
                }
                stack = grown;
                capacity = new_capacity;
            }
            {
                const rt_scene_node3d *child = (const rt_scene_node3d *)rt_g3d_checked_or_null(
                    current->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
                if (child)
                    stack[count++] = child;
            }
        }
    }
    free(stack);
    return total;
}

/// @brief Decode a single base64 character to its 0-63 value.
/// Returns -2 for `=` (padding sentinel) and -1 for any other invalid byte.
/// @param c Candidate ASCII Base64 byte.
/// @return Sextet value, `-2` for padding, or `-1` for invalid input.
static int vscn_base64_digit_value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    if (c == '=')
        return -2;
    return -1;
}

/// @brief Record invalid base64 in @p field at byte @p offset.
/// @param field Borrowed diagnostic field name, or `NULL` for `"data"`.
/// @param offset Zero-based invalid input byte offset.
static void vscn_set_base64_error(const char *field, size_t offset) {
    rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                        "Scene3D.Load: invalid base64 in %s at byte offset %zu",
                        field ? field : "data",
                        offset);
}

/// @brief Decode a base64 string of `len` characters into raw bytes.
///
/// Strict: rejects inputs whose length isn't a multiple of 4 and
/// any non-alphabet bytes. Honours `=` padding to compute the
/// exact output length. Returns a freshly-allocated buffer
/// (caller `free`s) or NULL on error.
/// @param data Borrowed encoded bytes.
/// @param len Exact encoded byte count.
/// @param out_len Optional output receiving decoded byte count.
/// @param error_offset Optional output receiving the invalid encoded offset or `SIZE_MAX`.
/// @return Caller-owned decoded allocation, including a one-byte allocation for empty input,
///   or `NULL` for malformed input/allocation failure.
static uint8_t *vscn_base64_decode_ex(const char *data,
                                      size_t len,
                                      size_t *out_len,
                                      size_t *error_offset) {
    if (out_len)
        *out_len = 0;
    if (error_offset)
        *error_offset = SIZE_MAX;
    if (!data)
        return NULL;
    if (len == 0) {
        uint8_t *empty = (uint8_t *)malloc(1);
        if (out_len)
            *out_len = 0;
        return empty;
    }
    if (len % 4 != 0) {
        if (error_offset)
            *error_offset = len;
        return NULL;
    }

    size_t padding = 0;
    if (data[len - 1] == '=')
        padding++;
    if (data[len - 2] == '=')
        padding++;
    for (size_t k = 0; k + padding < len; k++) {
        if (data[k] == '=') {
            if (error_offset)
                *error_offset = k;
            return NULL;
        }
    }
    if ((len / 4) > SIZE_MAX / 3)
        return NULL;

    size_t olen = (len / 4) * 3;
    olen -= padding;

    uint8_t *output = (uint8_t *)calloc(olen > 0 ? olen : 1u, 1u);
    if (!output)
        return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        size_t group = i;
        int a = vscn_base64_digit_value(data[i++]);
        int b = vscn_base64_digit_value(data[i++]);
        int c = vscn_base64_digit_value(data[i++]);
        int d = vscn_base64_digit_value(data[i++]);
        int is_last_group = (i == len);

        if (a < 0 || b < 0 || c == -1 || d == -1) {
            if (error_offset) {
                if (a < 0)
                    *error_offset = group;
                else if (b < 0)
                    *error_offset = group + 1u;
                else if (c == -1)
                    *error_offset = group + 2u;
                else
                    *error_offset = group + 3u;
            }
            free(output);
            return NULL;
        }
        if ((c == -2 || d == -2) && !is_last_group) {
            if (error_offset)
                *error_offset = c == -2 ? group + 2u : group + 3u;
            free(output);
            return NULL;
        }
        if (c == -2 && d != -2) {
            if (error_offset)
                *error_offset = group + 3u;
            free(output);
            return NULL;
        }
        if (d == -2 && c != -2 && (c & 0x03) != 0) {
            if (error_offset)
                *error_offset = group + 2u;
            free(output);
            return NULL;
        }
        if (c == -2 && (b & 0x0F) != 0) {
            if (error_offset)
                *error_offset = group + 1u;
            free(output);
            return NULL;
        }

        if (c == -2)
            c = 0;
        if (d == -2)
            d = 0;

        {
            uint32_t triple =
                ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
            if (j < olen)
                output[j++] = (uint8_t)((triple >> 16) & 0xFF);
            if (j < olen)
                output[j++] = (uint8_t)((triple >> 8) & 0xFF);
            if (j < olen)
                output[j++] = (uint8_t)(triple & 0xFF);
        }
    }

    if (j != olen) {
        if (error_offset)
            *error_offset = len;
        free(output);
        return NULL;
    }
    if (out_len)
        *out_len = olen;
    return output;
}

/// @brief Decode an exact f32 little-endian Base64 payload into host floats.
/// @param data Borrowed Base64 text.
/// @param len Encoded byte count.
/// @param expected_count Exact number of finite floats required.
/// @param field Borrowed diagnostic field name.
/// @return Caller-owned host-float array, or `NULL` on malformed data, size mismatch, or failure.
static float *vscn_base64_decode_f32_le(const char *data,
                                        size_t len,
                                        size_t expected_count,
                                        const char *field) {
    size_t raw_len = 0;
    size_t error_offset = SIZE_MAX;
    uint8_t *raw;
    float *values;
    if (!data || expected_count > SIZE_MAX / sizeof(float))
        return NULL;
    raw = vscn_base64_decode_ex(data, len, &raw_len, &error_offset);
    if (!raw) {
        if (error_offset != SIZE_MAX)
            vscn_set_base64_error(field, error_offset);
        return NULL;
    }
    if (raw_len != expected_count * sizeof(float)) {
        free(raw);
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                            "Scene3D.Load: %s decoded size does not match its count",
                            field ? field : "f32 payload");
        return NULL;
    }
    /* malloc-backed Base64 output is sufficiently aligned for every scalar type and has exactly
     * the final payload size. Canonicalize little-endian lanes in place to avoid a second equally
     * large allocation and copy at peak scene-load memory. */
    values = (float *)(void *)raw;
    for (size_t i = 0; i < expected_count; ++i) {
        size_t offset = i * sizeof(float);
        uint32_t bits;
        if (offset > raw_len || raw_len - offset < sizeof(float)) {
            free(raw);
            rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                "Scene3D.Load: %s decoded f32 payload ended early",
                                field ? field : "f32 payload");
            return NULL;
        }
        bits = ((uint32_t)raw[offset + 0u]) | ((uint32_t)raw[offset + 1u] << 8u) |
               ((uint32_t)raw[offset + 2u] << 16u) | ((uint32_t)raw[offset + 3u] << 24u);
        memcpy(&values[i], &bits, sizeof(bits));
        if (!isfinite(values[i])) {
            free(raw);
            rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                "Scene3D.Load: %s contains a non-finite value",
                                field ? field : "f32 payload");
            return NULL;
        }
    }
    return values;
}

/// @brief Decode an exact f64 little-endian Base64 payload into host doubles.
/// @param data Borrowed Base64 text.
/// @param len Encoded byte count.
/// @param expected_count Exact number of finite doubles required.
/// @param field Borrowed diagnostic field name.
/// @return Caller-owned host-double array, or `NULL` on malformed data, size mismatch, or failure.
static double *vscn_base64_decode_f64_le(const char *data,
                                         size_t len,
                                         size_t expected_count,
                                         const char *field) {
    size_t raw_len = 0;
    size_t error_offset = SIZE_MAX;
    uint8_t *raw;
    double *values;
    if (!data || expected_count > SIZE_MAX / sizeof(double))
        return NULL;
    raw = vscn_base64_decode_ex(data, len, &raw_len, &error_offset);
    if (!raw) {
        if (error_offset != SIZE_MAX)
            vscn_set_base64_error(field, error_offset);
        return NULL;
    }
    if (raw_len != expected_count * sizeof(double)) {
        free(raw);
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                            "Scene3D.Load: %s decoded size does not match its count",
                            field ? field : "f64 payload");
        return NULL;
    }
    values = (double *)(void *)raw;
    for (size_t i = 0; i < expected_count; ++i) {
        size_t offset = i * sizeof(double);
        uint64_t bits = 0;
        if (offset > raw_len || raw_len - offset < sizeof(double)) {
            free(raw);
            rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                "Scene3D.Load: %s decoded f64 payload ended early",
                                field ? field : "f64 payload");
            return NULL;
        }
        for (size_t byte = 0; byte < 8u; ++byte)
            bits |= (uint64_t)raw[offset + byte] << (byte * 8u);
        memcpy(&values[i], &bits, sizeof(bits));
        if (!isfinite(values[i])) {
            free(raw);
            rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                                "Scene3D.Load: %s contains a non-finite value",
                                field ? field : "f64 payload");
            return NULL;
        }
    }
    return values;
}

/// @brief Decode base64 RGBA bytes directly into a Pixels object's packed RGBA32 storage.
/// @details VSCN textures are serialized as byte-order RGBA. Pixels stores each texel as
///          `0xRRGGBBAA`, so this routine decodes each base64 byte stream group and packs
///          four emitted bytes into one `uint32_t` without allocating an intermediate RGBA
///          byte buffer. It uses the same strict alphabet, padding, and pad-bit validation
///          rules as `vscn_base64_decode_ex`.
/// @param data Base64 text; NULL is invalid.
/// @param len Byte length of @p data.
/// @param pixels Destination Pixels object with at least @p expected_pixels texels.
/// @param expected_pixels Number of RGBA texels expected from the decoded stream.
/// @param error_offset Optional byte offset for invalid base64; SIZE_MAX means size mismatch/OOM.
/// @return 1 on successful exact-size decode, 0 on malformed base64 or decoded-size mismatch.
static int vscn_base64_decode_rgba_pixels_ex(const char *data,
                                             size_t len,
                                             rt_pixels_impl *pixels,
                                             size_t expected_pixels,
                                             size_t *error_offset) {
    if (error_offset)
        *error_offset = SIZE_MAX;
    if (!data || !pixels || !pixels->data)
        return 0;
    if (expected_pixels > SIZE_MAX / 4u)
        return 0;
    if (len == 0)
        return expected_pixels == 0;
    if (len % 4 != 0) {
        if (error_offset)
            *error_offset = len;
        return 0;
    }

    size_t padding = 0;
    if (data[len - 1] == '=')
        padding++;
    if (data[len - 2] == '=')
        padding++;
    for (size_t k = 0; k + padding < len; k++) {
        if (data[k] == '=') {
            if (error_offset)
                *error_offset = k;
            return 0;
        }
    }
    if ((len / 4) > SIZE_MAX / 3u)
        return 0;
    size_t decoded_len = (len / 4u) * 3u - padding;
    if (decoded_len != expected_pixels * 4u)
        return 0;

    size_t byte_index = 0;
    size_t pixel_index = 0;
    uint32_t packed = 0;
    for (size_t i = 0; i < len;) {
        size_t group = i;
        int a = vscn_base64_digit_value(data[i++]);
        int b = vscn_base64_digit_value(data[i++]);
        int c = vscn_base64_digit_value(data[i++]);
        int d = vscn_base64_digit_value(data[i++]);
        int is_last_group = (i == len);
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            if (error_offset) {
                if (a < 0)
                    *error_offset = group;
                else if (b < 0)
                    *error_offset = group + 1u;
                else if (c == -1)
                    *error_offset = group + 2u;
                else
                    *error_offset = group + 3u;
            }
            return 0;
        }
        if ((c == -2 || d == -2) && !is_last_group) {
            if (error_offset)
                *error_offset = c == -2 ? group + 2u : group + 3u;
            return 0;
        }
        if (c == -2 && d != -2) {
            if (error_offset)
                *error_offset = group + 3u;
            return 0;
        }
        if (d == -2 && c != -2 && (c & 0x03) != 0) {
            if (error_offset)
                *error_offset = group + 2u;
            return 0;
        }
        if (c == -2 && (b & 0x0F) != 0) {
            if (error_offset)
                *error_offset = group + 1u;
            return 0;
        }

        int emitted = c == -2 ? 1 : (d == -2 ? 2 : 3);
        if (c == -2)
            c = 0;
        if (d == -2)
            d = 0;
        uint32_t triple =
            ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        uint8_t bytes[3] = {
            (uint8_t)((triple >> 16) & 0xFF),
            (uint8_t)((triple >> 8) & 0xFF),
            (uint8_t)(triple & 0xFF),
        };
        for (int out_i = 0; out_i < emitted; out_i++) {
            switch (byte_index & 3u) {
                case 0:
                    packed = (uint32_t)bytes[out_i] << 24;
                    break;
                case 1:
                    packed |= (uint32_t)bytes[out_i] << 16;
                    break;
                case 2:
                    packed |= (uint32_t)bytes[out_i] << 8;
                    break;
                default:
                    packed |= (uint32_t)bytes[out_i];
                    pixels->data[pixel_index++] = packed;
                    packed = 0;
                    break;
            }
            byte_index++;
        }
    }
    return byte_index == expected_pixels * 4u && pixel_index == expected_pixels;
}

/// @brief Decode a little-endian uint32 from serialized VSCN index bytes.
/// @details VSCN mesh payloads are tagged as little-endian; validating indices directly from
///          source bytes lets the loader reject corrupt buffers before allocating a destination
///          index array.
/// @param data Borrowed pointer to at least four serialized bytes.
/// @return Host-order unsigned 32-bit value.
#define VSCN_VERTEX_WIRE_V3_BYTES 92u
#define VSCN_EXTRA_INFLUENCES_WIRE_V1_BYTES 24u
#define VSCN_KEYFRAME_WIRE_V3_BYTES 132u

static uint16_t vscn_read_u16_le(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0]) | ((uint16_t)data[1] << 8u));
}

static uint32_t vscn_read_u32_le(const uint8_t *data) {
    return ((uint32_t)data[0]) | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t vscn_read_u64_le(const uint8_t *data) {
    uint64_t value = 0;
    for (size_t byte = 0; byte < 8u; ++byte)
        value |= (uint64_t)data[byte] << (byte * 8u);
    return value;
}

static float vscn_read_f32_le(const uint8_t *data) {
    const uint32_t bits = vscn_read_u32_le(data);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double vscn_read_f64_le(const uint8_t *data) {
    const uint64_t bits = vscn_read_u64_le(data);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void vscn_read_f32_lanes(const uint8_t **cursor, float *values, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        values[index] = vscn_read_f32_le(*cursor);
        *cursor += sizeof(float);
    }
}

static void vscn_decode_vertices_le_v3(vgfx3d_vertex_t *vertices,
                                       const uint8_t *wire,
                                       uint32_t count) {
    for (uint32_t index = 0; index < count; ++index) {
        vgfx3d_vertex_t *vertex = &vertices[index];
        const uint8_t *cursor = wire + (size_t)index * VSCN_VERTEX_WIRE_V3_BYTES;
        memset(vertex, 0, sizeof(*vertex));
        vscn_read_f32_lanes(&cursor, vertex->pos, 3u);
        vscn_read_f32_lanes(&cursor, vertex->normal, 3u);
        vscn_read_f32_lanes(&cursor, vertex->uv, 2u);
        vscn_read_f32_lanes(&cursor, vertex->uv1, 2u);
        vscn_read_f32_lanes(&cursor, vertex->color, 4u);
        vscn_read_f32_lanes(&cursor, vertex->tangent, 4u);
        memcpy(vertex->bone_indices, cursor, sizeof(vertex->bone_indices));
        cursor += sizeof(vertex->bone_indices);
        vscn_read_f32_lanes(&cursor, vertex->bone_weights, 4u);
    }
}

/// @brief Validate that every serialized VSCN index is inside the loaded vertex range.
/// @param indices_raw Borrowed packed little-endian index bytes.
/// @param index_count Number of readable uint32 indices.
/// @param vertex_count Exclusive valid vertex bound.
/// @return 1 if all @p index_count little-endian uint32 indices are `< vertex_count`, else 0.
static int vscn_indices_are_in_range(const uint8_t *indices_raw,
                                     uint32_t index_count,
                                     uint32_t vertex_count) {
    if (!indices_raw && index_count > 0)
        return 0;
    for (uint32_t i = 0; i < index_count; i++) {
        if (vscn_read_u32_le(indices_raw + (size_t)i * sizeof(uint32_t)) >= vertex_count)
            return 0;
    }
    return 1;
}

/// @brief Copy little-endian VSCN index bytes into the host uint32 index array.
/// @details On little-endian hosts this is equivalent to a memcpy, but spelling the conversion
///          here keeps VSCN index payloads portable if the runtime is built for a big-endian
///          target.
/// @param dst Output array receiving host-order indices.
/// @param src Borrowed packed little-endian index bytes.
/// @param index_count Number of uint32 indices to copy.
static void vscn_copy_indices_le(uint32_t *dst, const uint8_t *src, uint32_t index_count) {
    if (!dst || !src)
        return;
    for (uint32_t i = 0; i < index_count; i++)
        dst[i] = vscn_read_u32_le(src + (size_t)i * sizeof(uint32_t));
}

/// @brief Drop GC references to all `count` loaded objects, then free the array itself.
///
/// Used by the loader to roll back partially-loaded resources
/// when a later stage of `rt_scene3d_load` fails.
/// @param items Owned native array of retained runtime handles.
/// @param count Number of slots to release before freeing the array.
static void vscn_release_loaded_refs(void **items, int count) {
    if (!items)
        return;
    for (int i = 0; i < count; i++) {
        void *tmp = items[i];
        scene3d_release_ref(&tmp);
    }
    free(items);
}

/// @brief Determine whether a parsed JSON value is a map payload.
/// @param obj Borrowed candidate runtime value.
/// @return Nonzero for a non-string Map payload.
static int vjson_is_map(void *obj) {
    return obj && !rt_string_is_handle(obj) && rt_heap_is_payload(obj) &&
           rt_obj_class_id(obj) == RT_MAP_CLASS_ID;
}

/// @brief True if @p obj is a parsed-JSON array (a seq payload), not a string/map.
/// @param obj Borrowed candidate runtime value.
/// @return Nonzero for a non-string Seq payload.
static int vjson_is_seq(void *obj) {
    return obj && !rt_string_is_handle(obj) && rt_heap_is_payload(obj) &&
           rt_obj_class_id(obj) == RT_SEQ_CLASS_ID;
}

/// @brief Look up @p key in a parsed-JSON object; returns the value or NULL if @p obj is
///   not a map or the key is absent.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed NUL-terminated member name.
/// @return Borrowed member value, or `NULL`.
static void *vjson_get(void *obj, const char *key) {
    rt_string runtime_key;
    void *value;
    if (!obj || !key)
        return NULL;
    if (!vjson_is_map(obj))
        return NULL;
    runtime_key = rt_const_cstr(key);
    value = rt_map_get(obj, runtime_key);
    rt_string_unref(runtime_key);
    return value;
}

/// @brief Distinguish an absent JSON member from a present explicit null.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed NUL-terminated member name.
/// @return Nonzero when the map contains @p key.
static int vjson_has(void *obj, const char *key) {
    rt_string runtime_key;
    int present;
    if (!obj || !key || !vjson_is_map(obj))
        return 0;
    runtime_key = rt_const_cstr(key);
    present = rt_map_has(obj, runtime_key) ? 1 : 0;
    rt_string_unref(runtime_key);
    return present;
}

/// @brief Length of a JSON array, or 0 for NULL.
/// @param seq Borrowed parsed JSON sequence.
/// @return Non-negative sequence length, or zero for non-sequences.
static int64_t vjson_len(void *seq) {
    return vjson_is_seq(seq) ? rt_seq_len(seq) : 0;
}

/// @brief Safely coerce a JSON double to int64 without invoking undefined conversion behavior.
/// @param value Candidate finite double.
/// @param out Output receiving the converted integer.
/// @return Nonzero when @p value is finite and within the int64 conversion domain.
static int vjson_double_to_i64_checked(double value, int64_t *out) {
    if (!out || !isfinite(value))
        return 0;
    if (value < (-9223372036854775807.0 - 1.0) || value >= 9223372036854775808.0)
        return 0;
    *out = (int64_t)value;
    return 1;
}

/// @brief Coerce a boxed JSON value to int64. Falls back to `def` for non-numeric or null.
/// @param value Borrowed boxed JSON scalar.
/// @param def Fallback for absent, invalid, or out-of-range values.
/// @return Integer, safely converted double, Boolean as integer, or @p def.
static int64_t vjson_value_i64(void *value, int64_t def) {
    if (!value)
        return def;
    switch (rt_box_type(value)) {
        case 0:
            return rt_unbox_i64(value);
        case 1: {
            int64_t coerced;
            return vjson_double_to_i64_checked(rt_unbox_f64(value), &coerced) ? coerced : def;
        }
        case 2:
            return rt_unbox_i1(value);
        default:
            return def;
    }
}

/// @brief Read a JSON numeric value as an exact int64; rejects bools, non-finite doubles, and
///   fractional double values so index/count fields cannot be silently truncated.
/// @param value Borrowed boxed JSON numeric scalar.
/// @param out Output receiving the exact integer.
/// @return Nonzero when the value is an integer or exactly integral in-range double.
static int vjson_value_i64_exact(void *value, int64_t *out) {
    double number;
    if (!value || !out)
        return 0;
    switch (rt_box_type(value)) {
        case 0:
            *out = rt_unbox_i64(value);
            return 1;
        case 1:
            number = rt_unbox_f64(value);
            if (!isfinite(number) || floor(number) != number)
                return 0;
            return vjson_double_to_i64_checked(number, out);
        default:
            return 0;
    }
}

/// @brief Read an object integer property exactly, defaulting only when the key is absent.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed member name.
/// @param def Default used only for an absent/null member.
/// @param out Output receiving the exact integer or default.
/// @return Nonzero unless a present value has the wrong/inexact representation.
static int vjson_i64_exact(void *obj, const char *key, int64_t def, int64_t *out) {
    void *value;
    if (!out)
        return 0;
    *out = def;
    value = vjson_get(obj, key);
    if (!value)
        return 1;
    return vjson_value_i64_exact(value, out);
}

/// @brief Read an object finite numeric property exactly, defaulting only when absent.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed member name.
/// @param def Default used only for an absent/null member.
/// @param out Output receiving the finite numeric value or default.
/// @return Nonzero unless a present value is non-numeric or non-finite.
static int vjson_f64_exact(void *obj, const char *key, double def, double *out) {
    void *value;
    double number;
    if (!out)
        return 0;
    *out = def;
    value = vjson_get(obj, key);
    if (!value)
        return 1;
    if (rt_box_type(value) == 0)
        number = (double)rt_unbox_i64(value);
    else if (rt_box_type(value) == 1)
        number = rt_unbox_f64(value);
    else
        return 0;
    if (!isfinite(number))
        return 0;
    *out = number;
    return 1;
}

/// @brief Read an array integer element exactly, defaulting only when the index is absent.
/// @param arr Borrowed parsed JSON sequence.
/// @param index Zero-based element index.
/// @param def Default used for an absent/out-of-range element.
/// @param out Output receiving the exact integer or default.
/// @return Nonzero unless a present value has the wrong/inexact representation.
static int vjson_arr_i64_exact(void *arr, int64_t index, int64_t def, int64_t *out) {
    if (!out)
        return 0;
    *out = def;
    if (!arr || index < 0 || index >= vjson_len(arr))
        return 1;
    return vjson_value_i64_exact(rt_seq_get(arr, index), out);
}

/// @brief Coerce a boxed JSON value to double. `def` for non-numeric or null.
/// @param value Borrowed boxed JSON scalar.
/// @param def Fallback for absent, invalid, or non-finite values.
/// @return Numeric/Boolean value as a finite double, or @p def.
static double vjson_value_f64(void *value, double def) {
    if (!value)
        return def;
    switch (rt_box_type(value)) {
        case 0:
            return (double)rt_unbox_i64(value);
        case 1: {
            double number = rt_unbox_f64(value);
            return isfinite(number) ? number : def;
        }
        case 2:
            return (double)rt_unbox_i1(value);
        default:
            return def;
    }
}

/// @brief Read `obj[key]` as int64, defaulting to `def` if absent / wrong type.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed member name.
/// @param def Fallback value.
/// @return Coerced integer member value or @p def.
static int64_t vjson_i64(void *obj, const char *key, int64_t def) {
    void *value = vjson_get(obj, key);
    return vjson_value_i64(value, def);
}

/// @brief Read `obj[key]` as double, defaulting to `def` if absent / wrong type.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed member name.
/// @param def Fallback value.
/// @return Coerced finite numeric member value or @p def.
static double vjson_f64(void *obj, const char *key, double def) {
    void *value = vjson_get(obj, key);
    return vjson_value_f64(value, def);
}

/// @brief Read `obj[key]` as boolean (0/1), defaulting to `def`.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed member name.
/// @param def Fallback truth value.
/// @return Canonical member truth value, or @p def when absent.
static int8_t vjson_bool(void *obj, const char *key, int8_t def) {
    void *value = vjson_get(obj, key);
    return value ? (vjson_value_i64(value, def) ? 1 : 0) : def;
}

/// @brief Read `obj[key]` as a Zanna rt_string. NULL if missing or non-string.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed member name.
/// @return Borrowed runtime-string member, or `NULL`.
static rt_string vjson_string_value(void *obj, const char *key) {
    void *value = vjson_get(obj, key);
    return rt_string_is_handle(value) ? (rt_string)value : NULL;
}

/// @brief Read `obj[key]` as a borrowed C string and exact byte length.
/// @details Unlike `strlen(rt_string_cstr(...))`, this uses the runtime string length so large
///          base64 payloads are not scanned only to discover their byte count. The returned
///          pointer remains borrowed from the rt_string and is valid for the parsed JSON lifetime.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed member name.
/// @param out_len Optional output receiving exact runtime-string byte length.
/// @return Borrowed NUL-terminated string contents, or `NULL`.
static const char *vjson_cstr_len(void *obj, const char *key, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    rt_string value = vjson_string_value(obj, key);
    if (!value)
        return NULL;
    int64_t raw_len = rt_str_len(value);
    if (raw_len < 0 || (uint64_t)raw_len > (uint64_t)SIZE_MAX)
        return NULL;
    if (out_len)
        *out_len = (size_t)raw_len;
    return rt_string_cstr(value);
}

/// @brief Read `obj[key]` as a borrowed C string. NULL if missing or non-string.
/// The pointer remains valid only as long as the underlying rt_string lives.
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed member name.
/// @return Borrowed NUL-terminated string contents, or `NULL`.
static const char *vjson_cstr(void *obj, const char *key) {
    return vjson_cstr_len(obj, key, NULL);
}

/// @brief Array-form `arr[index]` as double with default. Useful for vec3/vec4 unpacking.
/// @param arr Borrowed parsed JSON sequence.
/// @param index Zero-based element index.
/// @param def Fallback for an absent, invalid, or non-numeric element.
/// @return Coerced finite numeric element or @p def.
static double vjson_arr_f64(void *arr, int64_t index, double def) {
    if (!arr || index < 0 || index >= vjson_len(arr))
        return def;
    return vjson_value_f64(rt_seq_get(arr, index), def);
}

/// @brief Read one required array element as a finite numeric double without bool coercion.
/// @param arr Borrowed parsed JSON sequence.
/// @param index Required zero-based element index.
/// @param out Output receiving the finite numeric value.
/// @return Nonzero only for an in-range integer or finite floating-point element.
static int vjson_arr_f64_exact(void *arr, int64_t index, double *out) {
    void *value;
    double number;
    if (!arr || !out || index < 0 || index >= vjson_len(arr))
        return 0;
    value = rt_seq_get(arr, index);
    if (!value)
        return 0;
    if (rt_box_type(value) == 0)
        number = (double)rt_unbox_i64(value);
    else if (rt_box_type(value) == 1)
        number = rt_unbox_f64(value);
    else
        return 0;
    if (!isfinite(number))
        return 0;
    *out = number;
    return 1;
}

/// @brief Read an optional integer index reference (e.g. a mesh/material index) from
///   @p key into @p out_index. Missing key → -1 and success; a value < -1 → failure (0).
/// @param obj Borrowed parsed JSON map.
/// @param key Borrowed reference-member name.
/// @param out_index Output receiving `-1` or the non-negative referenced index.
/// @return Nonzero for an absent or valid exact index, otherwise zero.
static int vscn_read_index_ref(void *obj, const char *key, int64_t *out_index) {
    void *value;
    int64_t index;
    if (!out_index)
        return 0;
    *out_index = -1;
    value = vjson_get(obj, key);
    if (!value)
        return 1;
    if (!vjson_value_i64_exact(value, &index))
        return 0;
    if (index < -1)
        return 0;
    *out_index = index;
    return 1;
}

/// @brief Reverse of `vscn_serialize_texture` — rebuild a source asset or RGBA Pixels object.
/// @param texture_obj Borrowed parsed JSON texture object.
/// @param version Validated VSCN document version controlling entry representation.
/// @return New owned TextureAsset3D/Pixels handle, or `NULL` on malformed data/failure.
static void *vscn_parse_texture(void *texture_obj, int64_t version) {
    int64_t width;
    int64_t height;
    const char *entry_kind;
    const char *rgba_b64;
    size_t rgba_b64_len = 0;
    size_t rgba_error = SIZE_MAX;
    rt_pixels_impl *pixels = NULL;

    if (!vjson_is_map(texture_obj))
        return NULL;
    entry_kind = vjson_cstr(texture_obj, "kind");
    if (version >= 5) {
        if (!entry_kind)
            return NULL;
        if (strcmp(entry_kind, "source") == 0) {
            const char *container = vjson_cstr(texture_obj, "container");
            const char *source_b64;
            const char *decode_name = NULL;
            size_t source_b64_len = 0;
            size_t source_len = 0;
            size_t source_error = SIZE_MAX;
            uint8_t *source;
            void *decoded = NULL;
            void *texture = NULL;
            if (!container)
                return NULL;
            if (strcmp(container, "ktx2") == 0)
                decode_name = "texture.ktx2";
            else if (strcmp(container, "png") == 0)
                decode_name = "texture.png";
            else if (strcmp(container, "jpeg") == 0)
                decode_name = "texture.jpg";
            else if (strcmp(container, "gif") == 0)
                decode_name = "texture.gif";
            else if (strcmp(container, "bmp") == 0)
                decode_name = "texture.bmp";
            else
                return NULL;
            source_b64 = vjson_cstr_len(texture_obj, "sourceBase64", &source_b64_len);
            if (!source_b64)
                return NULL;
            source = vscn_base64_decode_ex(source_b64, source_b64_len, &source_len, &source_error);
            if (!source) {
                if (source_error != SIZE_MAX)
                    vscn_set_base64_error("texture.sourceBase64", source_error);
                return NULL;
            }
            if (source_len == 0 || source_len > VSCN_MAX_FILE_BYTES) {
                free(source);
                rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                                   "Scene3D.Load: source texture payload is empty or too large");
                return NULL;
            }
            if (strcmp(container, "ktx2") == 0) {
                texture = rt_textureasset3d_load_ktx2_memory_strict(source, (uint64_t)source_len);
            } else {
                decoded = rt_asset_decode_typed(decode_name, source, source_len);
                if (decoded && rt_pixels_checked_impl_or_null(decoded))
                    texture = rt_textureasset3d_wrap_encoded_pixels(
                        decoded, source, (uint64_t)source_len, container);
                scene3d_release_ref(&decoded);
            }
            free(source);
            if (!texture)
                rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                            "Scene3D.Load: source texture is invalid");
            return texture;
        }
        if (strcmp(entry_kind, "rgba8") != 0)
            return NULL;
    }
    if (!vjson_i64_exact(texture_obj, "width", 0, &width) ||
        !vjson_i64_exact(texture_obj, "height", 0, &height))
        return NULL;
    rgba_b64 = vjson_cstr_len(texture_obj, "rgbaBase64", &rgba_b64_len);
    if (width <= 0 || height <= 0)
        return NULL;
    if ((uint64_t)width > SIZE_MAX || (uint64_t)height > SIZE_MAX)
        return NULL;
    if ((size_t)width > SIZE_MAX / (size_t)height)
        return NULL;
    if ((size_t)width * (size_t)height > SIZE_MAX / 4)
        return NULL;
    if (!rgba_b64)
        rgba_b64 = "";

    pixels = (rt_pixels_impl *)rt_pixels_new(width, height);
    if (!pixels)
        return NULL;

    if (!vscn_base64_decode_rgba_pixels_ex(
            rgba_b64, rgba_b64_len, pixels, (size_t)width * (size_t)height, &rgba_error)) {
        if (rgba_error != SIZE_MAX)
            vscn_set_base64_error("texture.rgbaBase64", rgba_error);
        else
            rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                               "Scene3D.Load: texture payload size does not match dimensions");
        if (rt_obj_release_check0(pixels))
            rt_obj_free(pixels);
        return NULL;
    }

    pixels_touch(pixels);
    return pixels;
}

/// @brief Reverse of `vscn_serialize_cubemap` — assemble a cubemap from texture-index references.
/// @param cubemap_obj Borrowed parsed JSON cubemap object.
/// @param textures Borrowed table of loaded texture handles.
/// @param tex_count Number of readable texture slots.
/// @return New owned Cubemap3D handle, or `NULL` for invalid references/failure.
static rt_cubemap3d *vscn_parse_cubemap(void *cubemap_obj, void **textures, int tex_count) {
    void *faces_arr;
    void *faces[6];

    if (!vjson_is_map(cubemap_obj))
        return NULL;
    faces_arr = vjson_get(cubemap_obj, "faces");
    if (!faces_arr || vjson_len(faces_arr) < 6)
        return NULL;

    for (int i = 0; i < 6; i++) {
        int64_t index;
        if (!vjson_arr_i64_exact(faces_arr, i, -1, &index))
            return NULL;
        if (index < 0 || index >= tex_count || !textures[index])
            return NULL;
        faces[i] = rt_material3d_resolve_texture_pixels(textures[index]);
        if (!rt_pixels_checked_impl_or_null(faces[i]))
            return NULL;
    }

    return (rt_cubemap3d *)rt_cubemap3d_new(
        faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
}

// Material-parse helpers (color/scalar/texture-slot parsing + texture/cubemap
// ref binding) live in a textual fragment, included here — after the vjson_*
// accessors and numeric sanitizers they depend on, and before the material
// parser that calls them.
#include "rt_scene3d_vscn_material_parse.inc"

/// @brief Reverse of `vscn_serialize_material` — restore PBR parameters and bind texture refs.
/// @param material_obj Borrowed parsed JSON material object.
/// @param textures Borrowed table of loaded texture handles.
/// @param tex_count Number of readable texture slots.
/// @param cubemaps Borrowed table of loaded Cubemap3D payloads.
/// @param cubemap_count Number of readable cubemap slots.
/// @return New owned Material3D payload, or `NULL` on malformed data/failure.
static rt_material3d *vscn_parse_material(void *material_obj,
                                          void **textures,
                                          int tex_count,
                                          rt_cubemap3d **cubemaps,
                                          int cubemap_count) {
    rt_material3d *material;

    if (!vjson_is_map(material_obj))
        return NULL;
    material = (rt_material3d *)rt_material3d_new();
    if (!material)
        return NULL;

    vscn_parse_material_color4(material_obj, "diffuse", material->diffuse, 1.0);
    vscn_parse_material_color3(material_obj, "specular", material->specular, VSCN_ABS_MAX);
    vscn_parse_material_color3(material_obj, "emissive", material->emissive, VSCN_ABS_MAX);
    vscn_parse_material_scalars(material_obj, material);
    vscn_parse_material_custom_params(material_obj, material);
    if (!vscn_parse_material_texture_slots(material_obj, material) ||
        !vscn_bind_material_refs(
            material_obj, material, textures, tex_count, cubemaps, cubemap_count)) {
        scene3d_release_ref((void **)&material);
        return NULL;
    }

    return material;
}

/// @brief Validate raw vertex payloads loaded from VSCN before they reach bounds, skinning, or
///   backend upload code.
/// @param vertices Borrowed decoded vertex array.
/// @param vertex_count Number of readable vertices.
/// @param bone_count Number of valid skeleton palette entries.
/// @return Nonzero when all numeric lanes, weights, and referenced bones are valid.
static int vscn_vertex_payload_is_valid(const vgfx3d_vertex_t *vertices,
                                        uint32_t vertex_count,
                                        int32_t bone_count) {
    if (!vertices && vertex_count > 0)
        return 0;
    for (uint32_t vi = 0; vi < vertex_count; vi++) {
        const vgfx3d_vertex_t *v = &vertices[vi];
        float weight_sum = 0.0f;
        for (int i = 0; i < 3; i++) {
            if (!isfinite((double)v->pos[i]) || !isfinite((double)v->normal[i]))
                return 0;
        }
        for (int i = 0; i < 2; i++) {
            if (!isfinite((double)v->uv[i]) || !isfinite((double)v->uv1[i]))
                return 0;
        }
        for (int i = 0; i < 4; i++) {
            if (!isfinite((double)v->color[i]) || !isfinite((double)v->tangent[i]) ||
                !isfinite((double)v->bone_weights[i]) || v->bone_weights[i] < 0.0f ||
                v->bone_weights[i] > 1.0f)
                return 0;
            if (v->bone_weights[i] > 0.000001f &&
                (bone_count <= 0 || (int32_t)v->bone_indices[i] >= bone_count))
                return 0;
            weight_sum += v->bone_weights[i];
        }
        if (weight_sum > 1.0001f)
            return 0;
    }
    return 1;
}

static int vscn_parse_mesh_rig_streams(rt_mesh3d *mesh, void *mesh_obj) {
    void *map_value = vjson_get(mesh_obj, "boneMapBase64");
    void *extra_value = vjson_get(mesh_obj, "extraInfluencesBase64");
    void *map_format_value = vjson_get(mesh_obj, "boneMapFormat");
    void *extra_format_value = vjson_get(mesh_obj, "extraInfluencesFormat");
    const char *map64 = vjson_cstr(mesh_obj, "boneMapBase64");
    const char *extra64 = vjson_cstr(mesh_obj, "extraInfluencesBase64");
    const char *map_format = vjson_cstr(mesh_obj, "boneMapFormat");
    const char *extra_format = vjson_cstr(mesh_obj, "extraInfluencesFormat");
    uint8_t *map_raw = NULL;
    uint8_t *extra_raw = NULL;
    int32_t *bone_map = NULL;
    vgfx3d_extra_influences_t *extra = NULL;

    if ((map_value && !map64) || (extra_value && !extra64) || (map_format_value && !map_format) ||
        (extra_format_value && !extra_format) || (map_format && !map64) ||
        (extra_format && !extra64) || (map_format && strcmp(map_format, "i32le-v1") != 0) ||
        (extra_format && strcmp(extra_format, "vgfx3d_extra_influences_le_v1") != 0))
        goto corrupt;

    if (map64) {
        size_t raw_len = 0;
        size_t error_offset = SIZE_MAX;
        if (mesh->bone_count <= 0)
            goto corrupt;
        map_raw = vscn_base64_decode_ex(map64, strlen(map64), &raw_len, &error_offset);
        if (!map_raw) {
            if (error_offset != SIZE_MAX)
                vscn_set_base64_error("mesh.boneMapBase64", error_offset);
            goto corrupt;
        }
        if (raw_len != (size_t)mesh->bone_count * sizeof(int32_t))
            goto corrupt;
        bone_map = (int32_t *)malloc((size_t)mesh->bone_count * sizeof(int32_t));
        if (!bone_map)
            goto fail;
        for (int32_t index = 0; index < mesh->bone_count; ++index) {
            uint32_t value = vscn_read_u32_le(map_raw + (size_t)index * sizeof(int32_t));
            if (value >= VGFX3D_MAX_SKELETON_BONES)
                goto corrupt;
            bone_map[index] = (int32_t)value;
        }
    }

    if (extra64) {
        size_t raw_len = 0;
        size_t error_offset = SIZE_MAX;
        if (mesh->vertex_count == 0 || !mesh->vertices)
            goto corrupt;
        extra_raw = vscn_base64_decode_ex(extra64, strlen(extra64), &raw_len, &error_offset);
        if (!extra_raw) {
            if (error_offset != SIZE_MAX)
                vscn_set_base64_error("mesh.extraInfluencesBase64", error_offset);
            goto corrupt;
        }
        if (raw_len != (size_t)mesh->vertex_count * VSCN_EXTRA_INFLUENCES_WIRE_V1_BYTES)
            goto corrupt;
        extra = (vgfx3d_extra_influences_t *)calloc(mesh->vertex_count,
                                                    sizeof(vgfx3d_extra_influences_t));
        if (!extra)
            goto fail;
        for (uint32_t vertex = 0; vertex < mesh->vertex_count; ++vertex) {
            const uint8_t *cursor =
                extra_raw + (size_t)vertex * VSCN_EXTRA_INFLUENCES_WIRE_V1_BYTES;
            float weight_sum = 0.0f;
            for (size_t lane = 0; lane < 4u; ++lane)
                weight_sum += mesh->vertices[vertex].bone_weights[lane];
            for (size_t lane = 0; lane < 4u; ++lane) {
                extra[vertex].indices[lane] = vscn_read_u16_le(cursor);
                cursor += sizeof(uint16_t);
            }
            for (size_t lane = 0; lane < 4u; ++lane) {
                const float weight = vscn_read_f32_le(cursor);
                cursor += sizeof(float);
                if (!isfinite(weight) || weight < 0.0f || weight > 1.0f ||
                    (weight > 0.000001f &&
                     (mesh->bone_count <= 0 || extra[vertex].indices[lane] >= mesh->bone_count)))
                    goto corrupt;
                extra[vertex].weights[lane] = weight;
                weight_sum += weight;
            }
            if (!isfinite(weight_sum) || weight_sum > 1.0001f)
                goto corrupt;
        }
    }

    free(map_raw);
    free(extra_raw);
    mesh->bone_map = bone_map;
    mesh->extra_influences = extra;
    return 1;

corrupt:
    rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                "Scene3D.Load: mesh rig side stream is corrupt");
fail:
    free(map_raw);
    free(extra_raw);
    free(bone_map);
    free(extra);
    return 0;
}

/// @brief Parse and attach a complete VSCN v4+ morph-target block to @p mesh.
/// @param mesh Borrowed destination Mesh3D payload.
/// @param mesh_obj Borrowed parsed JSON mesh object.
/// @return Nonzero when the optional block is absent or successfully attached.
static int vscn_parse_mesh_morph_targets(rt_mesh3d *mesh, void *mesh_obj) {
    void *morph_obj = vjson_get(mesh_obj, "morphTargets");
    void *shapes;
    const char *format;
    int64_t vertex_count;
    int64_t shape_count;
    void *morph = NULL;
    if (!morph_obj)
        return 1;
    if (!mesh || !vjson_is_map(morph_obj))
        return 0;
    format = vjson_cstr(morph_obj, "deltaFormat");
    shapes = vjson_get(morph_obj, "shapes");
    if (!format || strcmp(format, "f32le-v1") != 0 || !vjson_is_seq(shapes) ||
        !vjson_i64_exact(morph_obj, "vertexCount", -1, &vertex_count) || vertex_count <= 0 ||
        vertex_count != (int64_t)mesh->vertex_count)
        return 0;
    shape_count = vjson_len(shapes);
    if (shape_count < 0 || shape_count > 65536 || (size_t)vertex_count > SIZE_MAX / 3u)
        return 0;
    morph = rt_morphtarget3d_new(vertex_count);
    if (!morph)
        return 0;
    for (int64_t shape_index = 0; shape_index < shape_count; ++shape_index) {
        void *shape_obj = rt_seq_get(shapes, shape_index);
        rt_morphtarget3d_shape_view_internal view = {0};
        const char *position_text;
        const char *normal_text = NULL;
        const char *tangent_text = NULL;
        size_t position_len = 0;
        size_t normal_len = 0;
        size_t tangent_len = 0;
        size_t value_count = (size_t)vertex_count * 3u;
        float *positions = NULL;
        float *normals = NULL;
        float *tangents = NULL;
        int ok = 0;
        if (!vjson_is_map(shape_obj))
            goto shape_cleanup;
        view.name = vjson_cstr(shape_obj, "name");
        if (!view.name || strlen(view.name) > 63 ||
            !vjson_f64_exact(shape_obj, "weight", 0.0, &view.weight))
            goto shape_cleanup;
        position_text = vjson_cstr_len(shape_obj, "positionBase64", &position_len);
        if (!position_text)
            goto shape_cleanup;
        positions = vscn_base64_decode_f32_le(
            position_text, position_len, value_count, "mesh.morphTargets.positionBase64");
        if (!positions)
            goto shape_cleanup;
        if (vjson_get(shape_obj, "normalBase64")) {
            normal_text = vjson_cstr_len(shape_obj, "normalBase64", &normal_len);
            if (!normal_text)
                goto shape_cleanup;
            normals = vscn_base64_decode_f32_le(
                normal_text, normal_len, value_count, "mesh.morphTargets.normalBase64");
            if (!normals)
                goto shape_cleanup;
        }
        if (vjson_get(shape_obj, "tangentBase64")) {
            tangent_text = vjson_cstr_len(shape_obj, "tangentBase64", &tangent_len);
            if (!tangent_text)
                goto shape_cleanup;
            tangents = vscn_base64_decode_f32_le(
                tangent_text, tangent_len, value_count, "mesh.morphTargets.tangentBase64");
            if (!tangents)
                goto shape_cleanup;
        }
        view.position_deltas = positions;
        view.normal_deltas = normals;
        view.tangent_deltas = tangents;
        view.vertex_count = (int32_t)vertex_count;
        ok = rt_morphtarget3d_append_shape_internal(morph, &view) ? 1 : 0;
    shape_cleanup:
        free(positions);
        free(normals);
        free(tangents);
        if (!ok)
            goto fail;
    }
    rt_mesh3d_set_morph_targets(mesh, morph);
    scene3d_release_ref(&morph);
    return mesh->morph_targets_ref ? 1 : 0;
fail:
    scene3d_release_ref(&morph);
    return 0;
}

/// @brief Reverse of `vscn_serialize_mesh` — decode base64 buffers and rebuild the mesh.
/// @param mesh_obj Borrowed parsed JSON mesh object.
/// @return New owned Mesh3D payload, or `NULL` on malformed data/failure.
static rt_mesh3d *vscn_parse_mesh(void *mesh_obj) {
    rt_mesh3d *mesh;
    const char *vertex_format;
    const char *index_format;
    const char *vertices_b64;
    const char *indices_b64;
    size_t vertices_b64_len = 0;
    size_t indices_b64_len = 0;
    int64_t vertex_count_i64;
    int64_t index_count_i64;
    int64_t bone_count_i64 = 0;
    uint32_t vertex_count;
    uint32_t index_count;
    size_t vertices_len = 0;
    size_t indices_len = 0;
    size_t vertices_error = SIZE_MAX;
    size_t indices_error = SIZE_MAX;
    uint8_t *vertices_raw = NULL;
    uint8_t *indices_raw = NULL;
    int vertices_are_legacy84 = 0;
    int vertices_are_portable_v3 = 0;

    if (!vjson_is_map(mesh_obj))
        return NULL;

    vertex_format = vjson_cstr(mesh_obj, "vertexFormat");
    index_format = vjson_cstr(mesh_obj, "indexFormat");
    if ((vjson_get(mesh_obj, "vertexFormat") && !vertex_format) ||
        (vjson_get(mesh_obj, "indexFormat") && !index_format)) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh binary format tag has the wrong type");
        return NULL;
    }
    if (vertex_format && strcmp(vertex_format, "vgfx3d_vertex_le_v1") != 0 &&
        strcmp(vertex_format, "vgfx3d_vertex_le_v2") != 0 &&
        strcmp(vertex_format, "vgfx3d_vertex_le_v3") != 0) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh vertex format is unsupported");
        return NULL;
    }
    if (index_format && strcmp(index_format, "u32le-v1") != 0) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh index format is unsupported");
        return NULL;
    }

    if (!vjson_i64_exact(mesh_obj, "vertexCount", 0, &vertex_count_i64) ||
        !vjson_i64_exact(mesh_obj, "indexCount", 0, &index_count_i64))
        return NULL;
    if (vertex_count_i64 < 0 || index_count_i64 < 0 || vertex_count_i64 > UINT32_MAX ||
        index_count_i64 > UINT32_MAX) {
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE, "Scene3D.Load: mesh count exceeds range");
        return NULL;
    }
    if (!vjson_i64_exact(mesh_obj, "boneCount", 0, &bone_count_i64) || bone_count_i64 < 0 ||
        bone_count_i64 > VGFX3D_MAX_BONES) {
        rt_asset_error_set(bone_count_i64 > VGFX3D_MAX_BONES ? RT_ASSET_ERROR_TOO_LARGE
                                                             : RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh bone count is invalid");
        return NULL;
    }
    vertex_count = (uint32_t)vertex_count_i64;
    index_count = (uint32_t)index_count_i64;
    if (index_count % 3u != 0) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh index count is not a triangle list");
        return NULL;
    }
    if (!rt_alloc_count_ok(vertex_count, sizeof(vgfx3d_vertex_t)) ||
        !rt_alloc_count_ok(vertex_count, 84u) ||
        !rt_alloc_count_ok(index_count, sizeof(uint32_t))) {
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE, "Scene3D.Load: mesh payload is too large");
        return NULL;
    }
    vertices_b64 = vjson_cstr_len(mesh_obj, "verticesBase64", &vertices_b64_len);
    indices_b64 = vjson_cstr_len(mesh_obj, "indicesBase64", &indices_b64_len);
    if (!vertices_b64)
        vertices_b64 = "";
    if (!indices_b64)
        indices_b64 = "";

    vertices_raw =
        vscn_base64_decode_ex(vertices_b64, vertices_b64_len, &vertices_len, &vertices_error);
    indices_raw = vscn_base64_decode_ex(indices_b64, indices_b64_len, &indices_len, &indices_error);
    if (!vertices_raw || !indices_raw) {
        if (!vertices_raw && vertices_error != SIZE_MAX)
            vscn_set_base64_error("mesh.verticesBase64", vertices_error);
        else if (!indices_raw && indices_error != SIZE_MAX)
            vscn_set_base64_error("mesh.indicesBase64", indices_error);
        free(vertices_raw);
        free(indices_raw);
        return NULL;
    }
    size_t native_vertex_bytes = (size_t)vertex_count * sizeof(vgfx3d_vertex_t);
    size_t legacy_vertex_bytes = (size_t)vertex_count * 84u;
    if (vertex_format && strcmp(vertex_format, "vgfx3d_vertex_le_v3") == 0) {
        if (vertices_len != (size_t)vertex_count * VSCN_VERTEX_WIRE_V3_BYTES) {
            rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                               "Scene3D.Load: v3 mesh payload size does not match counts");
            free(vertices_raw);
            free(indices_raw);
            return NULL;
        }
        vertices_are_portable_v3 = 1;
    } else if (vertex_format && strcmp(vertex_format, "vgfx3d_vertex_le_v2") == 0) {
        if (vertices_len != native_vertex_bytes) {
            rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                               "Scene3D.Load: v2 mesh payload size does not match counts");
            free(vertices_raw);
            free(indices_raw);
            return NULL;
        }
    } else if (vertex_format && strcmp(vertex_format, "vgfx3d_vertex_le_v1") == 0) {
        if (vertices_len != legacy_vertex_bytes) {
            rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                               "Scene3D.Load: v1 mesh payload size does not match counts");
            free(vertices_raw);
            free(indices_raw);
            return NULL;
        }
        vertices_are_legacy84 = 1;
    } else if (vertices_len == native_vertex_bytes) {
        vertices_are_legacy84 = 0;
    } else if (vertices_len == legacy_vertex_bytes) {
        vertices_are_legacy84 = 1;
    } else {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh payload size does not match counts");
        free(vertices_raw);
        free(indices_raw);
        return NULL;
    }
    if (!rt_untrusted_count_ok(
            vertex_count_i64,
            vertices_are_legacy84
                ? 84u
                : (vertices_are_portable_v3 ? VSCN_VERTEX_WIRE_V3_BYTES : sizeof(vgfx3d_vertex_t)),
            vertices_len) ||
        !rt_untrusted_count_ok(index_count_i64, sizeof(uint32_t), indices_len)) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh payload count exceeds source bytes");
        free(vertices_raw);
        free(indices_raw);
        return NULL;
    }
    if (indices_len != (size_t)index_count * sizeof(uint32_t)) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh payload size does not match counts");
        free(vertices_raw);
        free(indices_raw);
        return NULL;
    }
    if (!vscn_indices_are_in_range(indices_raw, index_count, vertex_count)) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: mesh index references missing vertex");
        free(vertices_raw);
        free(indices_raw);
        return NULL;
    }

    mesh = (rt_mesh3d *)rt_mesh3d_new_empty_storage();
    if (!mesh) {
        free(vertices_raw);
        free(indices_raw);
        return NULL;
    }

    if (vertex_count > 0) {
        vgfx3d_vertex_t *vertices =
            (vgfx3d_vertex_t *)malloc((size_t)vertex_count * sizeof(vgfx3d_vertex_t));
        if (!vertices) {
            free(vertices_raw);
            free(indices_raw);
            scene3d_release_ref((void **)&mesh);
            return NULL;
        }
        if (vertices_are_portable_v3) {
            vscn_decode_vertices_le_v3(vertices, vertices_raw, vertex_count);
        } else if (!vertices_are_legacy84) {
            memcpy(vertices, vertices_raw, (size_t)vertex_count * sizeof(vgfx3d_vertex_t));
        } else {
            typedef struct {
                float pos[3];
                float normal[3];
                float uv[2];
                float color[4];
                float tangent[4];
                uint8_t bone_indices[4];
                float bone_weights[4];
            } vgfx3d_vertex_legacy84_t;

            const vgfx3d_vertex_legacy84_t *legacy = (const vgfx3d_vertex_legacy84_t *)vertices_raw;
            for (uint32_t vi = 0; vi < vertex_count; vi++) {
                memset(&vertices[vi], 0, sizeof(vertices[vi]));
                memcpy(vertices[vi].pos, legacy[vi].pos, sizeof(vertices[vi].pos));
                memcpy(vertices[vi].normal, legacy[vi].normal, sizeof(vertices[vi].normal));
                memcpy(vertices[vi].uv, legacy[vi].uv, sizeof(vertices[vi].uv));
                memcpy(vertices[vi].uv1, legacy[vi].uv, sizeof(vertices[vi].uv1));
                memcpy(vertices[vi].color, legacy[vi].color, sizeof(vertices[vi].color));
                memcpy(vertices[vi].tangent, legacy[vi].tangent, sizeof(vertices[vi].tangent));
                memcpy(vertices[vi].bone_indices,
                       legacy[vi].bone_indices,
                       sizeof(vertices[vi].bone_indices));
                memcpy(vertices[vi].bone_weights,
                       legacy[vi].bone_weights,
                       sizeof(vertices[vi].bone_weights));
            }
        }
        if (!vscn_vertex_payload_is_valid(vertices, vertex_count, (int32_t)bone_count_i64)) {
            free(vertices);
            free(vertices_raw);
            free(indices_raw);
            scene3d_release_ref((void **)&mesh);
            return NULL;
        }
        mesh->vertices = vertices;
        mesh->positions64 = NULL;
        mesh->vertex_count = vertex_count;
        mesh->vertex_capacity = vertex_count;
    } else {
        mesh->vertex_count = 0;
    }

    if (index_count > 0) {
        uint32_t *indices = (uint32_t *)malloc((size_t)index_count * sizeof(uint32_t));
        if (!indices) {
            free(vertices_raw);
            free(indices_raw);
            scene3d_release_ref((void **)&mesh);
            return NULL;
        }
        vscn_copy_indices_le(indices, indices_raw, index_count);
        mesh->indices = indices;
        mesh->index_count = index_count;
        mesh->index_capacity = index_count;
    } else {
        mesh->index_count = 0;
    }

    mesh->bone_count = bone_count_i64 > 0 ? (int32_t)bone_count_i64 : 0;
    mesh->bone_palette = NULL;
    mesh->prev_bone_palette = NULL;
    mesh->morph_deltas = NULL;
    mesh->morph_normal_deltas = NULL;
    mesh->morph_weights = NULL;
    mesh->prev_morph_weights = NULL;
    mesh->morph_shape_count = 0;
    mesh->morph_targets_ref = NULL;
    mesh->geometry_revision = 1;
    mesh->bounds_dirty = 1;
    rt_mesh3d_set_resident(mesh, vjson_bool(mesh_obj, "resident", 1));
    rt_mesh3d_refresh_bounds(mesh);

    if (!vscn_parse_mesh_rig_streams(mesh, mesh_obj)) {
        free(vertices_raw);
        free(indices_raw);
        scene3d_release_ref((void **)&mesh);
        return NULL;
    }

    free(vertices_raw);
    free(indices_raw);
    if (!vscn_parse_mesh_morph_targets(mesh, mesh_obj)) {
        scene3d_release_ref((void **)&mesh);
        return NULL;
    }
    return mesh;
}

/// @brief Parse one v3 skeleton object ({"bones": [{name,parent,bindLocal[16],
///   inverseBind[16]}...]}) into a retained Skeleton3D.
/// @details Bone construction goes through the runtime insertion path so cached bind TRS and
///          allocation ownership stay synchronized. Serialized parent indices and inverse-bind
///          matrices are then restored exactly after validation.
/// @param skel_obj Borrowed parsed JSON skeleton object.
/// @return New owned Skeleton3D handle, or `NULL` on malformed data/failure.
static void *vscn_parse_skeleton(void *skel_obj) {
    void *bones_arr;
    int64_t bone_count;
    rt_skeleton3d *skel;
    if (!vjson_is_map(skel_obj))
        return NULL;
    bones_arr = vjson_get(skel_obj, "bones");
    bone_count = vjson_len(bones_arr);
    if (bone_count <= 0 || bone_count > VGFX3D_MAX_SKELETON_BONES)
        return NULL;
    skel = (rt_skeleton3d *)rt_skeleton3d_new();
    if (!skel)
        return NULL;
    for (int64_t b = 0; b < bone_count; b++) {
        void *bone_obj = rt_seq_get(bones_arr, b);
        vgfx3d_bone_t *bone;
        rt_string parsed_name;
        void *bind_arr;
        void *inv_arr;
        void *bind_matrix = NULL;
        rt_string runtime_name = NULL;
        int64_t parent;
        double bind[16];
        float inverse[16];
        if (!vjson_is_map(bone_obj) || !vjson_i64_exact(bone_obj, "parent", -1, &parent) ||
            parent < -1 || parent >= bone_count)
            goto fail;
        parsed_name = vjson_string_value(bone_obj, "name");
        if (vjson_get(bone_obj, "name") && !parsed_name)
            goto fail;
        bind_arr = vjson_get(bone_obj, "bindLocal");
        inv_arr = vjson_get(bone_obj, "inverseBind");
        if (vjson_len(bind_arr) != 16 || vjson_len(inv_arr) != 16)
            goto fail;
        for (int k = 0; k < 16; k++) {
            double inverse_value;
            if (!vjson_arr_f64_exact(bind_arr, k, &bind[k]) ||
                !vjson_arr_f64_exact(inv_arr, k, &inverse_value) || bind[k] < -(double)FLT_MAX ||
                bind[k] > (double)FLT_MAX || inverse_value < -(double)FLT_MAX ||
                inverse_value > (double)FLT_MAX)
                goto fail;
            inverse[k] = (float)inverse_value;
        }
        runtime_name = parsed_name ? rt_string_ref(parsed_name) : rt_const_cstr("");
        bind_matrix = rt_mat4_new(bind[0],
                                  bind[1],
                                  bind[2],
                                  bind[3],
                                  bind[4],
                                  bind[5],
                                  bind[6],
                                  bind[7],
                                  bind[8],
                                  bind[9],
                                  bind[10],
                                  bind[11],
                                  bind[12],
                                  bind[13],
                                  bind[14],
                                  bind[15]);
        if (!runtime_name || !bind_matrix ||
            rt_skeleton3d_add_bone(skel, runtime_name, -1, bind_matrix) != b) {
            rt_string_unref(runtime_name);
            scene3d_release_ref(&bind_matrix);
            goto fail;
        }
        rt_string_unref(runtime_name);
        scene3d_release_ref(&bind_matrix);
        bone = &skel->bones[b];
        bone->parent_index = (int32_t)parent;
        memcpy(bone->inverse_bind, inverse, sizeof(inverse));
    }
    return skel;
fail:
    scene3d_release_ref((void **)&skel);
    return NULL;
}

/// @brief Validate and canonicalize decoded skeletal keyframes before publication.
/// @details Times must be finite, non-negative, and strictly increasing. Presence/cubic masks are
///          bounded to their defined bits, cubic components require complete values, every stored
///          lane/tangent must be finite, and present rotations are normalized with scaled
///          arithmetic so large finite quaternions cannot overflow their norm.
/// @param keys Mutable decoded keyframe array.
/// @param key_count Positive number of readable entries.
/// @return Nonzero when the complete payload is safe for binary search and interpolation.
static int vscn_skeletal_keyframes_valid(vgfx3d_keyframe_t *keys, int32_t key_count) {
    double previous_time = -1.0;
    if (!keys || key_count <= 0)
        return 0;
    for (int32_t index = 0; index < key_count; index++) {
        vgfx3d_keyframe_t *key = &keys[index];
        if (!isfinite(key->time) || key->time < 0.0 || key->time > (double)FLT_MAX ||
            (index > 0 && !(key->time > previous_time)) || (key->position_mask & ~0x07u) != 0 ||
            (key->rotation_mask != 0u && key->rotation_mask != 0x0Fu) ||
            (key->scale_mask & ~0x07u) != 0 || (key->cubic_mask & ~0x07u) != 0 ||
            ((key->cubic_mask & 1u) && key->position_mask != 0x07u) ||
            ((key->cubic_mask & 2u) && key->rotation_mask != 0x0Fu) ||
            ((key->cubic_mask & 4u) && key->scale_mask != 0x07u))
            return 0;
        previous_time = key->time;
        for (int lane = 0; lane < 3; lane++) {
            if (!isfinite(key->position[lane]) || !isfinite(key->scale_xyz[lane]) ||
                !isfinite(key->pos_in_tangent[lane]) || !isfinite(key->pos_out_tangent[lane]) ||
                !isfinite(key->scale_in_tangent[lane]) || !isfinite(key->scale_out_tangent[lane]))
                return 0;
        }
        for (int lane = 0; lane < 4; lane++) {
            if (!isfinite(key->rotation[lane]) || !isfinite(key->rot_in_tangent[lane]) ||
                !isfinite(key->rot_out_tangent[lane]))
                return 0;
        }
        if (key->rotation_mask == 0x0Fu) {
            double max_abs =
                fmax(fmax(fabs((double)key->rotation[0]), fabs((double)key->rotation[1])),
                     fmax(fabs((double)key->rotation[2]), fabs((double)key->rotation[3])));
            if (!isfinite(max_abs) || max_abs <= 1e-8)
                return 0;
            double length = hypot(
                hypot((double)key->rotation[0] / max_abs, (double)key->rotation[1] / max_abs),
                hypot((double)key->rotation[2] / max_abs, (double)key->rotation[3] / max_abs));
            if (!isfinite(length) || length <= 1e-8)
                return 0;
            for (int lane = 0; lane < 4; lane++)
                key->rotation[lane] = (float)(((double)key->rotation[lane] / max_abs) / length);
        }
    }
    return 1;
}

static vgfx3d_keyframe_t *vscn_decode_keyframes_le_v3(const uint8_t *wire, int32_t key_count) {
    vgfx3d_keyframe_t *keys;
    if (!wire || key_count <= 0 || (size_t)key_count > SIZE_MAX / sizeof(*keys))
        return NULL;
    keys = (vgfx3d_keyframe_t *)calloc((size_t)key_count, sizeof(*keys));
    if (!keys)
        return NULL;
    for (int32_t index = 0; index < key_count; ++index) {
        vgfx3d_keyframe_t *key = &keys[index];
        const uint8_t *cursor = wire + (size_t)index * VSCN_KEYFRAME_WIRE_V3_BYTES;
        key->time = vscn_read_f64_le(cursor);
        cursor += sizeof(double);
        vscn_read_f32_lanes(&cursor, key->position, 3u);
        vscn_read_f32_lanes(&cursor, key->rotation, 4u);
        vscn_read_f32_lanes(&cursor, key->scale_xyz, 3u);
        key->position_mask = *cursor++;
        key->rotation_mask = *cursor++;
        key->scale_mask = *cursor++;
        key->cubic_mask = *cursor++;
        vscn_read_f32_lanes(&cursor, key->pos_in_tangent, 3u);
        vscn_read_f32_lanes(&cursor, key->pos_out_tangent, 3u);
        vscn_read_f32_lanes(&cursor, key->rot_in_tangent, 4u);
        vscn_read_f32_lanes(&cursor, key->rot_out_tangent, 4u);
        vscn_read_f32_lanes(&cursor, key->scale_in_tangent, 3u);
        vscn_read_f32_lanes(&cursor, key->scale_out_tangent, 3u);
    }
    return keys;
}

/// @brief Parse one v3 animation clip ({name,duration,looping,keyframeFormat,
///   channels:[{bone,keyCount,keyframesBase64}...]}) into a retained Animation3D.
/// @param anim_obj Borrowed parsed JSON skeletal-animation object.
/// @return New owned Animation3D handle, or `NULL` on malformed data/failure.
static void *vscn_parse_animation(void *anim_obj) {
    const char *name;
    const char *format;
    void *channels_arr;
    int64_t channel_count;
    double duration;
    rt_animation3d *anim;
    uint8_t seen_bones[(VGFX3D_MAX_SKELETON_BONES + 7) / 8] = {0};
    int portable_v3;
    if (!vjson_is_map(anim_obj))
        return NULL;
    format = vjson_cstr(anim_obj, "keyframeFormat");
    if (!format || (strcmp(format, "vgfx3d_keyframe_le_v2") != 0 &&
                    strcmp(format, "vgfx3d_keyframe_le_v3") != 0))
        return NULL; /* layout drift: refuse rather than misread */
    portable_v3 = strcmp(format, "vgfx3d_keyframe_le_v3") == 0;
    name = vjson_cstr(anim_obj, "name");
    channels_arr = vjson_get(anim_obj, "channels");
    channel_count = vjson_len(channels_arr);
    if (channel_count < 0 || channel_count > RT_ANIMATION3D_MAX_CHANNELS ||
        !vjson_f64_exact(anim_obj, "duration", 1.0, &duration) || duration <= 0.0 ||
        duration > (double)FLT_MAX)
        return NULL;
    {
        rt_string runtime_name = rt_const_cstr(name ? name : "baked");
        anim = (rt_animation3d *)rt_animation3d_new(runtime_name, duration);
        rt_string_unref(runtime_name);
    }
    if (!anim)
        return NULL;
    rt_animation3d_set_looping(anim, vjson_bool(anim_obj, "looping", 1));
    if (channel_count > 0) {
        anim->channels =
            (vgfx3d_anim_channel_t *)calloc((size_t)channel_count, sizeof(vgfx3d_anim_channel_t));
        if (!anim->channels)
            goto fail;
        anim->owned_channels = anim->channels;
        anim->owned_channel_capacity = (int32_t)channel_count;
        anim->channel_capacity = (int32_t)channel_count;
    }
    for (int64_t c = 0; c < channel_count; c++) {
        void *ch_obj = rt_seq_get(channels_arr, c);
        vgfx3d_anim_channel_t *ch = &anim->channels[c];
        const char *keys64;
        int64_t bone_index;
        int64_t key_count;
        size_t keys_len = 0;
        size_t raw_len = 0;
        size_t raw_err = SIZE_MAX;
        uint8_t *raw;
        vgfx3d_keyframe_t *keys;
        if (!vjson_is_map(ch_obj) || !vjson_i64_exact(ch_obj, "bone", 0, &bone_index) ||
            bone_index < 0 || bone_index >= VGFX3D_MAX_SKELETON_BONES ||
            !vjson_i64_exact(ch_obj, "keyCount", 0, &key_count) || key_count <= 0 ||
            key_count > RT_ANIMATION3D_MAX_KEYFRAMES_PER_CHANNEL)
            goto fail;
        if ((seen_bones[(size_t)bone_index >> 3u] & (uint8_t)(1u << ((uint32_t)bone_index & 7u))) !=
            0)
            goto fail;
        seen_bones[(size_t)bone_index >> 3u] |= (uint8_t)(1u << ((uint32_t)bone_index & 7u));
        keys64 = vjson_cstr_len(ch_obj, "keyframesBase64", &keys_len);
        if (!keys64)
            goto fail;
        raw = vscn_base64_decode_ex(keys64, keys_len, &raw_len, &raw_err);
        if (!raw || raw_len != (size_t)key_count * (portable_v3 ? VSCN_KEYFRAME_WIRE_V3_BYTES
                                                                : sizeof(vgfx3d_keyframe_t))) {
            free(raw);
            goto fail;
        }
        if (portable_v3) {
            keys = vscn_decode_keyframes_le_v3(raw, (int32_t)key_count);
            free(raw);
            if (!keys)
                goto fail;
        } else {
            keys = (vgfx3d_keyframe_t *)raw;
        }
        if (!vscn_skeletal_keyframes_valid(keys, (int32_t)key_count)) {
            free(keys);
            goto fail;
        }
        ch->bone_index = (int32_t)bone_index;
        ch->keyframes = keys;
        ch->keyframe_count = (int32_t)key_count;
        ch->keyframe_capacity = (int32_t)key_count;
        ch->owned_keyframes = ch->keyframes;
        ch->owned_keyframe_capacity = (int32_t)key_count;
        ch->initialized_keyframe_count = (int32_t)key_count;
        anim->channel_count = (int32_t)(c + 1);
        anim->initialized_channel_count = anim->channel_count;
    }
    return anim;
fail:
    scene3d_release_ref((void **)&anim);
    return NULL;
}

/// @brief Parse one complete VSCN v4+ node/object/morph/camera animation clip.
/// @param animation_obj Borrowed parsed JSON node-animation object.
/// @return New owned NodeAnimation3D handle, or `NULL` on malformed data/failure.
static void *vscn_parse_node_animation(void *animation_obj) {
    const char *name;
    const char *format;
    void *channels;
    int64_t channel_count;
    double duration;
    rt_string runtime_name;
    rt_node_animation3d *animation;
    if (!vjson_is_map(animation_obj) ||
        !vjson_f64_exact(animation_obj, "duration", 1.0, &duration) || duration <= 0.0)
        return NULL;
    name = vjson_cstr(animation_obj, "name");
    format = vjson_cstr(animation_obj, "sampleFormat");
    channels = vjson_get(animation_obj, "channels");
    channel_count = vjson_len(channels);
    if (!name || !format || strcmp(format, "f64le-f32le-v1") != 0 || !vjson_is_seq(channels) ||
        channel_count < 0 || channel_count > 65536)
        return NULL;
    runtime_name = rt_const_cstr(name);
    animation = (rt_node_animation3d *)rt_node_animation3d_new(runtime_name, duration);
    rt_string_unref(runtime_name);
    if (!animation)
        return NULL;
    animation->looping = vjson_bool(animation_obj, "looping", 1) ? 1 : 0;
    for (int64_t channel_index = 0; channel_index < channel_count; ++channel_index) {
        void *channel_obj = rt_seq_get(channels, channel_index);
        const char *target;
        const char *times_text;
        const char *values_text;
        const char *in_text = NULL;
        const char *out_text = NULL;
        size_t times_len = 0;
        size_t values_len = 0;
        size_t in_len = 0;
        size_t out_len = 0;
        int64_t target_node;
        int64_t path;
        int64_t interpolation;
        int64_t key_count;
        int64_t value_width;
        size_t value_count;
        double *times = NULL;
        float *values = NULL;
        float *in_tangents = NULL;
        float *out_tangents = NULL;
        rt_string runtime_target = NULL;
        int64_t added = -1;
        if (!vjson_is_map(channel_obj) ||
            !vjson_i64_exact(channel_obj, "targetNode", -1, &target_node) || target_node < -1 ||
            target_node > INT32_MAX || !vjson_i64_exact(channel_obj, "path", -1, &path) ||
            path < RT_NODE_ANIM_PATH_TRANSLATION || path > RT_NODE_ANIM_PATH_LAST ||
            !vjson_i64_exact(channel_obj, "interpolation", -1, &interpolation) ||
            interpolation < RT_NODE_ANIM_INTERP_LINEAR ||
            interpolation > RT_NODE_ANIM_INTERP_CUBICSPLINE ||
            !vjson_i64_exact(channel_obj, "keyCount", 0, &key_count) || key_count <= 0 ||
            key_count > 1000000 || !vjson_i64_exact(channel_obj, "valueWidth", 0, &value_width) ||
            value_width <= 0 || value_width > 4096 ||
            (size_t)key_count > SIZE_MAX / (size_t)value_width)
            goto fail_channel;
        value_count = (size_t)key_count * (size_t)value_width;
        if (value_count > 4000000u)
            goto fail_channel;
        target = vjson_cstr(channel_obj, "target");
        times_text = vjson_cstr_len(channel_obj, "timesBase64", &times_len);
        values_text = vjson_cstr_len(channel_obj, "valuesBase64", &values_len);
        if (!target || target[0] == '\0' || !times_text || !values_text)
            goto fail_channel;
        times = vscn_base64_decode_f64_le(
            times_text, times_len, (size_t)key_count, "nodeAnimation.timesBase64");
        values = vscn_base64_decode_f32_le(
            values_text, values_len, value_count, "nodeAnimation.valuesBase64");
        if (!times || !values)
            goto fail_channel;
        if (interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE) {
            in_text = vjson_cstr_len(channel_obj, "inTangentsBase64", &in_len);
            out_text = vjson_cstr_len(channel_obj, "outTangentsBase64", &out_len);
            if (!in_text || !out_text)
                goto fail_channel;
            in_tangents = vscn_base64_decode_f32_le(
                in_text, in_len, value_count, "nodeAnimation.inTangentsBase64");
            out_tangents = vscn_base64_decode_f32_le(
                out_text, out_len, value_count, "nodeAnimation.outTangentsBase64");
            if (!in_tangents || !out_tangents)
                goto fail_channel;
        }
        runtime_target = rt_const_cstr(target);
        if (interpolation == RT_NODE_ANIM_INTERP_CUBICSPLINE) {
            added = rt_node_animation3d_add_cubic_channel(animation,
                                                          runtime_target,
                                                          path,
                                                          key_count,
                                                          value_width,
                                                          times,
                                                          values,
                                                          in_tangents,
                                                          out_tangents);
        } else {
            added = rt_node_animation3d_add_channel(animation,
                                                    runtime_target,
                                                    path,
                                                    interpolation,
                                                    key_count,
                                                    value_width,
                                                    times,
                                                    values);
        }
        rt_string_unref(runtime_target);
        runtime_target = NULL;
        if (added != channel_index)
            goto fail_channel;
        rt_node_animation3d_set_channel_target_node_index(animation, added, target_node);
        free(times);
        free(values);
        free(in_tangents);
        free(out_tangents);
        continue;
    fail_channel:
        if (runtime_target)
            rt_string_unref(runtime_target);
        free(times);
        free(values);
        free(in_tangents);
        free(out_tangents);
        scene3d_release_ref((void **)&animation);
        return NULL;
    }
    return animation;
}

/// @brief Parse one VSCN v4+ camera, preserving projection parameters and full view transform.
/// @param camera_obj Borrowed parsed JSON camera object.
/// @return New owned Camera3D handle, or `NULL` on malformed data/failure.
static void *vscn_parse_camera(void *camera_obj) {
    void *eye;
    void *view;
    double fov;
    double aspect;
    double near_plane;
    double far_plane;
    double ortho_size;
    int8_t is_ortho;
    rt_camera3d *camera;
    if (!vjson_is_map(camera_obj) || !vjson_f64_exact(camera_obj, "fov", 60.0, &fov) ||
        !vjson_f64_exact(camera_obj, "aspect", 1.0, &aspect) ||
        !vjson_f64_exact(camera_obj, "near", 0.1, &near_plane) ||
        !vjson_f64_exact(camera_obj, "far", 1000.0, &far_plane) ||
        !vjson_f64_exact(camera_obj, "orthoSize", 10.0, &ortho_size))
        return NULL;
    eye = vjson_get(camera_obj, "eye");
    view = vjson_get(camera_obj, "view");
    is_ortho = vjson_bool(camera_obj, "isOrtho", 0);
    if (!vjson_is_seq(eye) || vjson_len(eye) != 3 || !vjson_is_seq(view) || vjson_len(view) != 16 ||
        aspect <= 0.0 || near_plane <= 0.0 || far_plane <= near_plane || ortho_size <= 0.0 ||
        (!is_ortho && (fov <= 0.0 || fov >= 180.0)))
        return NULL;
    camera =
        (rt_camera3d *)(is_ortho ? rt_camera3d_new_ortho(ortho_size, aspect, near_plane, far_plane)
                                 : rt_camera3d_new(fov, aspect, near_plane, far_plane));
    if (!camera)
        return NULL;
    for (int lane = 0; lane < 3; ++lane) {
        double value = vjson_arr_f64(eye, lane, NAN);
        if (!isfinite(value))
            goto fail;
        camera->eye[lane] = value;
    }
    for (int lane = 0; lane < 16; ++lane) {
        double value = vjson_arr_f64(view, lane, NAN);
        if (!isfinite(value))
            goto fail;
        camera->view[lane] = value;
    }
    return camera;
fail:
    scene3d_release_ref((void **)&camera);
    return NULL;
}

/// @brief Parse a JSON light object from a VSCN file into an `rt_light3d` struct.
/// @details Reads type, direction, color, intensity, attenuation, and spot-cone
///          cosines from the vjson object. Defaults to a point light (type 1) when
///          the type field is absent or out of range [0–3].
/// @param light_obj Borrowed parsed JSON light object.
/// @return New owned Light3D payload, or `NULL` on malformed data/failure.
static rt_light3d *vscn_parse_light(void *light_obj) {
    rt_light3d *light;
    void *arr;
    if (!vjson_is_map(light_obj))
        return NULL;
    light = (rt_light3d *)rt_obj_new_i64(RT_G3D_LIGHT3D_CLASS_ID, (int64_t)sizeof(rt_light3d));
    if (!light)
        return NULL;
    memset(light, 0, sizeof(*light));
    light->type = (int32_t)vjson_i64(light_obj, "type", 1);
    if (light->type < 0 || light->type > 6)
        light->type = 1;
    light->direction[2] = -1.0;
    light->basis_u[0] = 1.0;
    light->basis_v[1] = 1.0;
    light->color[0] = light->color[1] = light->color[2] = 1.0;
    light->width = 1.0;
    light->height = 1.0;
    light->radius = 1.0;
    light->decay_type = 2;
    light->enabled = 1;
    light->enabled = vjson_bool(light_obj, "enabled", 1) ? 1 : 0;
    light->casts_shadows = vjson_bool(light_obj, "castsShadows", 0);
    light->intensity = vscn_nonnegative_or(vjson_f64(light_obj, "intensity", 1.0), 1.0);
    light->attenuation = vscn_nonnegative_or(vjson_f64(light_obj, "attenuation", 0.0), 0.0);
    light->inner_cos = vjson_f64(light_obj, "innerCos", 1.0);
    light->outer_cos = vjson_f64(light_obj, "outerCos", 0.7071067811865476);
    if (!isfinite(light->inner_cos) || light->inner_cos < 0.0 || light->inner_cos > 1.0)
        light->inner_cos = 1.0;
    if (!isfinite(light->outer_cos) || light->outer_cos < 0.0 || light->outer_cos > 1.0)
        light->outer_cos = 0.7071067811865476;
    if (light->type == 3 && light->inner_cos <= light->outer_cos + 1e-6) {
        if (light->inner_cos <= 1e-6)
            light->inner_cos = 1.0;
        light->outer_cos = light->inner_cos - 1e-6;
        if (light->outer_cos < 0.0)
            light->outer_cos = 0.0;
    }

    arr = vjson_get(light_obj, "direction");
    if (arr && vjson_len(arr) >= 3) {
        light->direction[0] = vscn_clamp_abs_or(vjson_arr_f64(arr, 0, light->direction[0]), 0.0);
        light->direction[1] = vscn_clamp_abs_or(vjson_arr_f64(arr, 1, light->direction[1]), 0.0);
        light->direction[2] = vscn_clamp_abs_or(vjson_arr_f64(arr, 2, light->direction[2]), -1.0);
    }
    vscn_normalize_vec3(light->direction, 0.0, 0.0, -1.0);
    arr = vjson_get(light_obj, "position");
    if (arr && vjson_len(arr) >= 3) {
        light->position[0] = vscn_clamp_abs_or(vjson_arr_f64(arr, 0, light->position[0]), 0.0);
        light->position[1] = vscn_clamp_abs_or(vjson_arr_f64(arr, 1, light->position[1]), 0.0);
        light->position[2] = vscn_clamp_abs_or(vjson_arr_f64(arr, 2, light->position[2]), 0.0);
    }
    arr = vjson_get(light_obj, "color");
    if (arr && vjson_len(arr) >= 3) {
        light->color[0] =
            vscn_clamp_or(vjson_arr_f64(arr, 0, light->color[0]), 1.0, 0.0, VSCN_ABS_MAX);
        light->color[1] =
            vscn_clamp_or(vjson_arr_f64(arr, 1, light->color[1]), 1.0, 0.0, VSCN_ABS_MAX);
        light->color[2] =
            vscn_clamp_or(vjson_arr_f64(arr, 2, light->color[2]), 1.0, 0.0, VSCN_ABS_MAX);
    }
    arr = vjson_get(light_obj, "basisU");
    if (arr && !vjson_is_seq(arr))
        goto fail;
    if (arr && vjson_len(arr) >= 3) {
        light->basis_u[0] = vscn_clamp_abs_or(vjson_arr_f64(arr, 0, light->basis_u[0]), 1.0);
        light->basis_u[1] = vscn_clamp_abs_or(vjson_arr_f64(arr, 1, light->basis_u[1]), 0.0);
        light->basis_u[2] = vscn_clamp_abs_or(vjson_arr_f64(arr, 2, light->basis_u[2]), 0.0);
        vscn_normalize_vec3(light->basis_u, 1.0, 0.0, 0.0);
    }
    arr = vjson_get(light_obj, "basisV");
    if (arr && !vjson_is_seq(arr))
        goto fail;
    if (arr && vjson_len(arr) >= 3) {
        light->basis_v[0] = vscn_clamp_abs_or(vjson_arr_f64(arr, 0, light->basis_v[0]), 0.0);
        light->basis_v[1] = vscn_clamp_abs_or(vjson_arr_f64(arr, 1, light->basis_v[1]), 1.0);
        light->basis_v[2] = vscn_clamp_abs_or(vjson_arr_f64(arr, 2, light->basis_v[2]), 0.0);
        vscn_normalize_vec3(light->basis_v, 0.0, 1.0, 0.0);
    }
    light->width = vscn_nonnegative_or(vjson_f64(light_obj, "width", light->width), light->width);
    light->height =
        vscn_nonnegative_or(vjson_f64(light_obj, "height", light->height), light->height);
    light->radius =
        vscn_nonnegative_or(vjson_f64(light_obj, "radius", light->radius), light->radius);
    light->range = vscn_nonnegative_or(vjson_f64(light_obj, "range", light->range), light->range);
    light->decay_type = (int32_t)vjson_i64(light_obj, "decayType", light->decay_type);
    if (light->decay_type < 0 || light->decay_type > 3)
        light->decay_type = 2;
    return light;
fail:
    scene3d_release_ref((void **)&light);
    return NULL;
}

/// @brief Parse one canonical decimal i64 without JSON-number precision loss.
/// @param value Borrowed runtime string containing the canonical decimal representation.
/// @param out Output receiving the exact signed 64-bit value.
/// @return Nonzero when the complete string is canonical and in range.
static int vscn_parse_metadata_i64(rt_string value, int64_t *out) {
    const char *text;
    int64_t length;
    int64_t index = 0;
    uint64_t magnitude = 0;
    uint64_t limit;
    int negative = 0;
    if (!value || !out || !rt_string_is_handle(value))
        return 0;
    text = rt_string_cstr(value);
    length = rt_str_len(value);
    if (!text || length <= 0 || memchr(text, '\0', (size_t)length) != NULL)
        return 0;
    if (text[index] == '-') {
        negative = 1;
        index++;
        if (index >= length)
            return 0;
    }
    if ((text[index] == '0' && index + 1 < length) ||
        (negative && text[index] == '0' && index + 1 == length))
        return 0;
    limit = negative ? (uint64_t)INT64_MAX + UINT64_C(1) : (uint64_t)INT64_MAX;
    for (; index < length; ++index) {
        uint64_t digit;
        if (text[index] < '0' || text[index] > '9')
            return 0;
        digit = (uint64_t)(text[index] - '0');
        if (magnitude > (limit - digit) / UINT64_C(10))
            return 0;
        magnitude = magnitude * UINT64_C(10) + digit;
    }
    if (!negative)
        *out = (int64_t)magnitude;
    else if (magnitude == (uint64_t)INT64_MAX + UINT64_C(1))
        *out = INT64_MIN;
    else
        *out = -(int64_t)magnitude;
    return 1;
}

/// @brief Parse a bounded tagged metadata map onto one newly-created node.
/// @param node Borrowed destination SceneNode3D payload.
/// @param node_obj Borrowed parsed JSON node object.
/// @param version Validated VSCN version; metadata requires version 6 or newer.
/// @return Nonzero when the optional map is absent or fully published, otherwise zero.
static int vscn_parse_node_metadata(rt_scene_node3d *node, void *node_obj, int64_t version) {
    void *metadata;
    void *keys;
    int64_t count;
    int ok = 0;
    if (!node || !node_obj)
        return 0;
    if (!vjson_has(node_obj, "metadata"))
        return 1;
    if (version < 6)
        return 0;
    metadata = vjson_get(node_obj, "metadata");
    if (!vjson_is_map(metadata))
        return 0;
    count = rt_map_len(metadata);
    if (count < 0 || count > RT_SCENE_NODE3D_MAX_METADATA_ENTRIES)
        return 0;
    keys = rt_map_keys(metadata);
    if (!keys)
        return 0;
    for (int64_t index = 0; index < count; ++index) {
        rt_string key = rt_seq_get_str(keys, index);
        void *tagged = key ? rt_map_get(metadata, key) : NULL;
        rt_string kind_value;
        const char *kind;
        int64_t kind_length;
        int has_value;
        void *value;
        int8_t set = 0;
        if (!key || !vjson_is_map(tagged)) {
            if (key)
                rt_string_unref(key);
            goto done;
        }
        kind_value = vjson_string_value(tagged, "kind");
        kind = kind_value ? rt_string_cstr(kind_value) : NULL;
        kind_length = kind_value ? rt_str_len(kind_value) : 0;
        has_value = vjson_has(tagged, "value");
        value = vjson_get(tagged, "value");
        if (!kind || kind_length <= 0 || kind_length > 6 ||
            memchr(kind, '\0', (size_t)kind_length) != NULL) {
            rt_string_unref(key);
            goto done;
        }
        if (kind_length == 4 && memcmp(kind, "null", 4) == 0) {
            if (!has_value)
                set = rt_scene_node3d_metadata_set_null(node, key);
        } else if (kind_length == 4 && memcmp(kind, "bool", 4) == 0) {
            if (has_value && value && rt_box_type(value) == RT_BOX_I1)
                set = rt_scene_node3d_metadata_set_bool(node, key, rt_unbox_i1(value));
        } else if (kind_length == 3 && memcmp(kind, "int", 3) == 0) {
            int64_t parsed;
            if (has_value && rt_string_is_handle(value) &&
                vscn_parse_metadata_i64((rt_string)value, &parsed))
                set = rt_scene_node3d_metadata_set_int(node, key, parsed);
        } else if (kind_length == 5 && memcmp(kind, "float", 5) == 0) {
            double parsed;
            if (has_value && value) {
                if (rt_box_type(value) == RT_BOX_I64)
                    parsed = (double)rt_unbox_i64(value);
                else if (rt_box_type(value) == RT_BOX_F64)
                    parsed = rt_unbox_f64(value);
                else
                    parsed = NAN;
                if (isfinite(parsed))
                    set = rt_scene_node3d_metadata_set_float(node, key, parsed);
            }
        } else if (kind_length == 6 && memcmp(kind, "string", 6) == 0) {
            if (has_value && rt_string_is_handle(value))
                set = rt_scene_node3d_metadata_set_string(node, key, (rt_string)value);
        }
        rt_string_unref(key);
        if (!set)
            goto done;
    }
    ok = 1;
done:
    scene3d_release_ref(&keys);
    return ok;
}

/// @brief Parse one scene node's fields, leaving child attachment to the iterative tree walker.
/// @param node_obj Borrowed parsed JSON node object.
/// @param meshes Borrowed loaded Mesh3D table.
/// @param mesh_count Number of readable mesh slots.
/// @param materials Borrowed loaded Material3D table.
/// @param material_count Number of readable material slots.
/// @param variant_count Validated asset material-variant count.
/// @param cameras Borrowed loaded Camera3D table.
/// @param camera_count Number of readable camera slots.
/// @param version Validated VSCN document version.
/// @param io_error Optional output set nonzero for malformed data/failure.
/// @return New owned detached SceneNode3D payload, or `NULL`.
static rt_scene_node3d *vscn_parse_node_fields(void *node_obj,
                                               rt_mesh3d **meshes,
                                               int mesh_count,
                                               rt_material3d **materials,
                                               int material_count,
                                               int variant_count,
                                               void **cameras,
                                               int camera_count,
                                               int64_t version,
                                               int *io_error) {
    rt_scene_node3d *node;
    void *arr;
    rt_string name;

    if (!vjson_is_map(node_obj)) {
        if (io_error)
            *io_error = 1;
        return NULL;
    }
    node = (rt_scene_node3d *)rt_scene_node3d_new();
    if (!node) {
        if (io_error)
            *io_error = 1;
        return NULL;
    }

    name = vjson_string_value(node_obj, "name");
    if (name)
        rt_scene_node3d_set_name(node, name);

    arr = vjson_get(node_obj, "position");
    if (arr && !vjson_is_seq(arr)) {
        if (io_error)
            *io_error = 1;
        scene3d_release_ref((void **)&node);
        return NULL;
    }
    if (arr && vjson_len(arr) >= 3) {
        node->position[0] = vscn_clamp_abs_or(vjson_arr_f64(arr, 0, node->position[0]), 0.0);
        node->position[1] = vscn_clamp_abs_or(vjson_arr_f64(arr, 1, node->position[1]), 0.0);
        node->position[2] = vscn_clamp_abs_or(vjson_arr_f64(arr, 2, node->position[2]), 0.0);
    }

    arr = vjson_get(node_obj, "rotation");
    if (arr && !vjson_is_seq(arr)) {
        if (io_error)
            *io_error = 1;
        scene3d_release_ref((void **)&node);
        return NULL;
    }
    if (arr && vjson_len(arr) >= 4) {
        node->rotation[0] = vjson_arr_f64(arr, 0, node->rotation[0]);
        node->rotation[1] = vjson_arr_f64(arr, 1, node->rotation[1]);
        node->rotation[2] = vjson_arr_f64(arr, 2, node->rotation[2]);
        node->rotation[3] = vjson_arr_f64(arr, 3, node->rotation[3]);
        vscn_normalize_quat(node->rotation);
    }

    arr = vjson_get(node_obj, "scale");
    if (arr && !vjson_is_seq(arr)) {
        if (io_error)
            *io_error = 1;
        scene3d_release_ref((void **)&node);
        return NULL;
    }
    if (arr && vjson_len(arr) >= 3) {
        node->scale_xyz[0] = vscn_clamp_abs_or(vjson_arr_f64(arr, 0, node->scale_xyz[0]), 1.0);
        node->scale_xyz[1] = vscn_clamp_abs_or(vjson_arr_f64(arr, 1, node->scale_xyz[1]), 1.0);
        node->scale_xyz[2] = vscn_clamp_abs_or(vjson_arr_f64(arr, 2, node->scale_xyz[2]), 1.0);
    }

    node->visible = vjson_bool(node_obj, "visible", 1);
    /* Authoring flags saved by vscn_serialize_node (absent in older files →
     * defaults). Impostor assets are transient: regenerated by the bake. */
    node->is_static = vjson_bool(node_obj, "isStatic", 0) ? 1 : 0;
    {
        int64_t sync_mode = vjson_i64(node_obj, "syncMode", 0);
        if (sync_mode < 0 || sync_mode > INT32_MAX)
            sync_mode = 0;
        node->sync_mode = (int32_t)sync_mode;
    }
    {
        int64_t import_index = -1;
        if (!vjson_i64_exact(node_obj, "importIndex", -1, &import_index) || import_index < -1 ||
            import_index > INT32_MAX) {
            if (io_error)
                *io_error = 1;
            scene3d_release_ref((void **)&node);
            return NULL;
        }
        node->import_index = (int32_t)import_index;
    }
    node->world_dirty = 1;
    if (!vscn_parse_node_metadata(node, node_obj, version)) {
        if (io_error)
            *io_error = 1;
        scene3d_release_ref((void **)&node);
        return NULL;
    }

    {
        /* VSCN v7 prefab reference (ADR 0187). Absolute references are
         * tolerated at load (authoring rejects them); resolution happens in
         * the post-parse graft pass, which needs the referencing file path. */
        rt_string prefab_ref = vjson_string_value(node_obj, "prefab");
        if (prefab_ref && rt_str_len(prefab_ref) > 0)
            node->prefab_path = rt_string_ref(prefab_ref);
    }

    {
        int64_t mesh_index;
        if (!vscn_read_index_ref(node_obj, "mesh", &mesh_index) ||
            (mesh_index >= 0 && (mesh_index >= mesh_count || !meshes || !meshes[mesh_index]))) {
            if (io_error)
                *io_error = 1;
            scene3d_release_ref((void **)&node);
            return NULL;
        }
        if (mesh_index >= 0 && mesh_index < mesh_count && meshes[mesh_index])
            rt_scene_node3d_set_mesh(node, meshes[mesh_index]);
    }
    {
        int64_t material_index;
        if (!vscn_read_index_ref(node_obj, "material", &material_index) ||
            (material_index >= 0 &&
             (material_index >= material_count || !materials || !materials[material_index]))) {
            if (io_error)
                *io_error = 1;
            scene3d_release_ref((void **)&node);
            return NULL;
        }
        if (material_index >= 0 && material_index < material_count && materials[material_index])
            rt_scene_node3d_set_material(node, materials[material_index]);
    }
    {
        void *variants = vjson_get(node_obj, "variantMaterials");
        if (variants) {
            void **table = NULL;
            int64_t count = vjson_len(variants);
            int ok = vjson_is_seq(variants) && count >= 0 && count == variant_count;
            if (ok && count > 0)
                table = (void **)calloc((size_t)count, sizeof(void *));
            if (ok && count > 0 && !table)
                ok = 0;
            for (int64_t i = 0; ok && i < count; ++i) {
                int64_t material_index = -1;
                if (!vjson_arr_i64_exact(variants, i, -1, &material_index) || material_index < -1 ||
                    material_index >= material_count)
                    ok = 0;
                else if (material_index >= 0)
                    table[i] = materials[material_index];
            }
            if (ok)
                ok = rt_scene_node3d_assign_variant_materials(node, table, (int32_t)count);
            free(table);
            if (!ok) {
                if (io_error)
                    *io_error = 1;
                scene3d_release_ref((void **)&node);
                return NULL;
            }
        }
    }
    {
        int64_t camera_index;
        if (!vscn_read_index_ref(node_obj, "camera", &camera_index) ||
            (camera_index >= 0 &&
             (camera_index >= camera_count || !cameras || !cameras[camera_index]))) {
            if (io_error)
                *io_error = 1;
            scene3d_release_ref((void **)&node);
            return NULL;
        }
        if (camera_index >= 0)
            rt_scene_node3d_set_camera(node, cameras[camera_index]);
    }
    {
        void *light_obj = vjson_get(node_obj, "light");
        rt_light3d *light = vscn_parse_light(light_obj);
        if (light_obj && !light) {
            if (io_error)
                *io_error = 1;
            scene3d_release_ref((void **)&node);
            return NULL;
        }
        if (light) {
            rt_scene_node3d_set_light(node, light);
            {
                void *tmp = light;
                scene3d_release_ref(&tmp);
            }
        }
    }

    arr = vjson_get(node_obj, "lod");
    if (arr) {
        if (!vjson_is_seq(arr)) {
            if (io_error)
                *io_error = 1;
            scene3d_release_ref((void **)&node);
            return NULL;
        }
        for (int64_t i = 0; i < vjson_len(arr); i++) {
            void *lod_obj = rt_seq_get(arr, i);
            int64_t mesh_index;
            if (!lod_obj || !vscn_read_index_ref(lod_obj, "mesh", &mesh_index) ||
                (mesh_index >= 0 && (mesh_index >= mesh_count || !meshes || !meshes[mesh_index]))) {
                if (io_error)
                    *io_error = 1;
                scene3d_release_ref((void **)&node);
                return NULL;
            }
            if (mesh_index >= 0 && mesh_index < mesh_count && meshes[mesh_index]) {
                rt_scene_node3d_add_lod(
                    node,
                    vscn_nonnegative_or(vjson_f64(lod_obj, "distance", 0.0), 0.0),
                    meshes[mesh_index]);
            }
        }
    }

    {
        void *auto_lod = vjson_get(node_obj, "autoLOD");
        if (auto_lod && vjson_is_map(auto_lod)) {
            rt_scene_node3d_set_auto_lod(
                node,
                vjson_bool(auto_lod, "enabled", 0),
                vscn_nonnegative_or(vjson_f64(auto_lod, "screenErrorPx", 8.0), 8.0));
        }
    }

    return node;
}

/// @brief Iteratively rebuild a scene-node subtree from JSON.
/// @param node_obj Borrowed parsed JSON subtree root.
/// @param meshes Borrowed loaded Mesh3D table.
/// @param mesh_count Number of readable mesh slots.
/// @param materials Borrowed loaded Material3D table.
/// @param material_count Number of readable material slots.
/// @param variant_count Validated asset material-variant count.
/// @param cameras Borrowed loaded Camera3D table.
/// @param camera_count Number of readable camera slots.
/// @param version Validated VSCN document version.
/// @param io_error Optional output set nonzero for malformed data/failure.
/// @return New owned retained subtree root, or `NULL`.
static rt_scene_node3d *vscn_parse_node(void *node_obj,
                                        rt_mesh3d **meshes,
                                        int mesh_count,
                                        rt_material3d **materials,
                                        int material_count,
                                        int variant_count,
                                        void **cameras,
                                        int camera_count,
                                        int64_t version,
                                        int *io_error) {
    typedef struct vscn_parse_node_frame {
        void *children;
        rt_scene_node3d *node;
        int64_t next_child;
        int64_t child_count;
    } vscn_parse_node_frame_t;

    vscn_parse_node_frame_t frames[VSCN_MAX_NODE_DEPTH] = {{0}};
    rt_scene_node3d *root;
    size_t frame_count = 0;

    root = vscn_parse_node_fields(node_obj,
                                  meshes,
                                  mesh_count,
                                  materials,
                                  material_count,
                                  variant_count,
                                  cameras,
                                  camera_count,
                                  version,
                                  io_error);
    if (!root)
        return NULL;
    frames[frame_count++] = (vscn_parse_node_frame_t){vjson_get(node_obj, "children"), root, 0, 0};

    while (frame_count > 0) {
        vscn_parse_node_frame_t *frame = &frames[frame_count - 1];
        rt_scene_node3d *child;
        void *child_obj;

        if (!frame->children) {
            frame_count--;
            continue;
        }
        if (!vjson_is_seq(frame->children))
            goto fail;
        if (frame->child_count == 0)
            frame->child_count = vjson_len(frame->children);
        if (frame->next_child >= frame->child_count) {
            frame_count--;
            continue;
        }
        if (frame_count >= VSCN_MAX_NODE_DEPTH)
            goto fail;
        child_obj = rt_seq_get(frame->children, frame->next_child++);
        child = vscn_parse_node_fields(child_obj,
                                       meshes,
                                       mesh_count,
                                       materials,
                                       material_count,
                                       variant_count,
                                       cameras,
                                       camera_count,
                                       version,
                                       io_error);
        if (!child)
            goto fail;
        if (!rt_scene_node3d_try_add_child(frame->node, child)) {
            scene3d_release_ref((void **)&child);
            goto fail;
        }
        frames[frame_count++] =
            (vscn_parse_node_frame_t){vjson_get(child_obj, "children"), child, 0, 0};
        scene3d_release_ref((void **)&child);
    }

    return root;

fail:
    if (io_error)
        *io_error = 1;
    scene3d_release_ref((void **)&root);
    return NULL;
}

/// @brief Read an entire file into a newly-malloc'd, NUL-terminated buffer.
///
/// @details Uses the platform's 64-bit seek/tell API and rejects files larger
///   than @c VSCN_MAX_FILE_BYTES before allocating. The extra trailing NUL is
///   for the JSON parser convenience and is not included in @p out_size.
///
/// @param filepath Path to the UTF-8 scene file to read.
/// @param out_size Receives the exact byte length of the file on success.
/// @return The buffer (caller frees) with its byte length in @p out_size, or NULL on I/O error or
///   when the file exceeds VSCN_MAX_FILE_BYTES (256 MiB).
static char *vscn_read_file(const char *filepath, size_t *out_size) {
    if (!out_size)
        return NULL;
    FILE *f = rt_file_stdio_open_utf8(filepath, "rb");
    int64_t file_size;
    char *json;
    *out_size = 0;
    if (!f) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "Scene3D.Load: '%s' not found", filepath);
        return NULL;
    }
    if (vscn_fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNREADABLE, "Scene3D.Load: failed to seek '%s'", filepath);
        return NULL;
    }
    file_size = (int64_t)vscn_ftell(f);
    if (file_size < 0 || vscn_fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNREADABLE, "Scene3D.Load: failed to read '%s'", filepath);
        return NULL;
    }
    if ((uint64_t)file_size > VSCN_MAX_FILE_BYTES || (uint64_t)file_size > SIZE_MAX - 1) {
        fclose(f);
        rt_asset_error_setf(RT_ASSET_ERROR_TOO_LARGE, "Scene3D.Load: '%s' is too large", filepath);
        return NULL;
    }
    json = (char *)malloc((size_t)file_size + 1);
    if (!json) {
        fclose(f);
        return NULL;
    }
    if (file_size > 0 && fread(json, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f);
        free(json);
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNREADABLE, "Scene3D.Load: failed to read '%s'", filepath);
        return NULL;
    }
    fclose(f);
    json[(size_t)file_size] = '\0';
    *out_size = (size_t)file_size;
    return json;
}

/// @brief Parse `nodes_arr` into scene-graph children of @p scene's root (no-op when array absent).
/// @param target_root Borrowed destination SceneNode3D root.
/// @param nodes_arr Borrowed optional parsed JSON node sequence.
/// @param meshes Borrowed loaded Mesh3D table.
/// @param mesh_count Number of readable mesh slots.
/// @param materials Borrowed loaded Material3D table.
/// @param material_count Number of readable material slots.
/// @param variant_count Validated asset material-variant count.
/// @param cameras Borrowed loaded Camera3D table.
/// @param camera_count Number of readable camera slots.
/// @param version Validated VSCN document version.
/// @return 1 on success, 0 on a node parse failure (the offending node is released).
static int vscn_load_nodes_into_root(rt_scene_node3d *target_root,
                                     void *nodes_arr,
                                     rt_mesh3d **meshes,
                                     int mesh_count,
                                     rt_material3d **materials,
                                     int material_count,
                                     int variant_count,
                                     void **cameras,
                                     int camera_count,
                                     int64_t version) {
    if (!target_root)
        return 0;
    if (!nodes_arr)
        return 1;
    for (int64_t i = 0; i < vjson_len(nodes_arr); i++) {
        int parse_error = 0;
        rt_scene_node3d *node = vscn_parse_node(rt_seq_get(nodes_arr, i),
                                                meshes,
                                                mesh_count,
                                                materials,
                                                material_count,
                                                variant_count,
                                                cameras,
                                                camera_count,
                                                version,
                                                &parse_error);
        if (parse_error || !node) {
            scene3d_release_ref((void **)&node);
            return 0;
        }
        if (!rt_scene_node3d_try_add_child(target_root, node)) {
            scene3d_release_ref((void **)&node);
            return 0;
        }
        {
            void *tmp = node;
            scene3d_release_ref(&tmp);
        }
    }
    return 1;
}

/// @brief Copy a NUL-terminated VSCN metadata string into native ownership.
/// @param text Borrowed NUL-terminated source.
/// @return Caller-owned exact copy, or `NULL` on invalid input/allocation failure.
static char *vscn_strdup_cstr(const char *text) {
    size_t length;
    char *copy;
    if (!text)
        return NULL;
    length = strlen(text);
    if (length == SIZE_MAX)
        return NULL;
    copy = (char *)malloc(length + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, length + 1u);
    return copy;
}

/* -------------------------------------------------------------------------
 * VSCN v7 prefab grafting (ADR 0187)
 *
 * One implementation serves every consumer: SceneGraph.Load, SceneAsset,
 * async handles, and streaming all funnel through the buffer loader below,
 * so references resolve identically everywhere. The frame stack carries the
 * canonical path chain for cycle detection, the nesting depth, and a shared
 * per-root instantiation budget. Failures never fail the outer load: the
 * offending node stays an empty placeholder that retains its reference and
 * round-trips byte-identically.
 * ---------------------------------------------------------------------- */

#define VSCN_PREFAB_MAX_DEPTH 8
#define VSCN_PREFAB_MAX_INSTANCES 4096

typedef struct vscn_prefab_frame {
    /// Borrowed canonical path for cycle detection.
    const char *canonical_path;
    /// Borrowed enclosing reference frame.
    const struct vscn_prefab_frame *parent;
    /// One-based nested prefab depth.
    int depth;
    /// Borrowed shared remaining-instantiation budget.
    int32_t *instance_budget;
} vscn_prefab_frame;

/// @brief Parse borrowed VSCN JSON bytes while threading nested-prefab state.
/// @param filepath Borrowed diagnostic/source path.
/// @param json Borrowed JSON byte span; a trailing NUL is not required.
/// @param file_size Exact JSON byte count excluding the terminator.
/// @param prefab_stack Borrowed enclosing prefab frame, or `NULL`.
/// @return New owned Scene3D/Model3D handle, or `NULL` on failure.
static void *rt_scene3d_load_impl_from_buffer(const char *filepath,
                                              const char *json,
                                              size_t file_size,
                                              const vscn_prefab_frame *prefab_stack);

/// @brief Load one referenced scene file with the prefab stack threaded through.
/// @param path Borrowed runtime-string file path.
/// @param prefab_stack Borrowed current prefab-resolution frame.
/// @return New owned loaded scene handle, or `NULL`.
static void *vscn_prefab_load_file(rt_string path, const vscn_prefab_frame *prefab_stack) {
    const char *filepath;
    char *json = NULL;
    size_t file_size;
    if (!path)
        return NULL;
    filepath = rt_string_cstr(path);
    if (!filepath)
        return NULL;
    json = vscn_read_file(filepath, &file_size);
    if (!json)
        return NULL;
    {
        void *result = rt_scene3d_load_impl_from_buffer(filepath, json, file_size, prefab_stack);
        free(json);
        return result;
    }
}

/// @brief Mark one grafted subtree as transient instance content.
/// @param node Borrowed subtree root.
static void vscn_mark_instance_content(rt_scene_node3d *node) {
    if (!node)
        return;
    node->is_instance_content = 1;
    for (int32_t i = 0; i < scene3d_node_child_count(node); ++i)
        vscn_mark_instance_content(scene_node3d_checked(node->children[i]));
}

/// @brief Report one placeholder-producing prefab failure (ADR 0227).
/// @param node Borrowed placeholder node carrying the retained reference.
/// @param reason Borrowed static reason token (missing/cycle/depth/budget/invalid).
static void vscn_warn_unresolved_prefab(const rt_scene_node3d *node, const char *reason) {
    const char *path = node && node->prefab_path ? rt_string_cstr(node->prefab_path) : NULL;
    const char *name = node && node->name ? rt_string_cstr(node->name) : NULL;
    rt_asset_error_add_warningf("Scene3D prefab '%s' on node '%s' left a placeholder: %s",
                                path ? path : "",
                                name ? name : "",
                                reason ? reason : "invalid");
}

/// @brief Resolve and graft one prefab node's referenced content.
/// @details Every guard failure leaves the node an empty placeholder with
///          its reference retained; the surrounding load still succeeds.
///          Each placeholder adds one asset-error warning so authors and
///          games can tell a broken reference from an authored empty node.
/// @param node Borrowed placeholder node containing a retained prefab path.
/// @param base_dir Borrowed directory against which relative references resolve.
/// @param self_frame Borrowed current file's resolution frame and shared budget.
/// @return Unresolved references contributed by this node: 1 for a
///         placeholder, or the referenced scene's own nested count.
static int32_t vscn_graft_one_prefab(rt_scene_node3d *node,
                                     rt_string base_dir,
                                     const vscn_prefab_frame *self_frame) {
    rt_string joined = NULL;
    rt_string canonical = NULL;
    rt_scene3d *child_scene = NULL;
    int32_t nested_unresolved = 0;

    if (!node || !node->prefab_path)
        return 0;
    if (self_frame->depth >= VSCN_PREFAB_MAX_DEPTH) {
        vscn_warn_unresolved_prefab(node, "nesting depth limit");
        return 1;
    }
    if (self_frame->instance_budget && *self_frame->instance_budget <= 0) {
        vscn_warn_unresolved_prefab(node, "instance budget exhausted");
        return 1;
    }

    joined = rt_path_join(base_dir, node->prefab_path);
    if (!joined) {
        vscn_warn_unresolved_prefab(node, "invalid path");
        return 1;
    }
    canonical = rt_path_abs(joined);
    if (!canonical) {
        rt_string_unref(joined);
        vscn_warn_unresolved_prefab(node, "invalid path");
        return 1;
    }
    {
        const char *canonical_cstr = rt_string_cstr(canonical);
        const vscn_prefab_frame *frame = self_frame;
        while (canonical_cstr && frame) {
            if (frame->canonical_path && strcmp(frame->canonical_path, canonical_cstr) == 0) {
                rt_string_unref(joined);
                rt_string_unref(canonical);
                vscn_warn_unresolved_prefab(node, "reference cycle");
                return 1;
            }
            frame = frame->parent;
        }
        if (!canonical_cstr) {
            rt_string_unref(joined);
            rt_string_unref(canonical);
            vscn_warn_unresolved_prefab(node, "invalid path");
            return 1;
        }
        if (self_frame->instance_budget)
            (*self_frame->instance_budget)--;
        {
            vscn_prefab_frame child_frame;
            child_frame.canonical_path = canonical_cstr;
            child_frame.parent = self_frame;
            child_frame.depth = self_frame->depth + 1;
            child_frame.instance_budget = self_frame->instance_budget;
            child_scene = (rt_scene3d *)vscn_prefab_load_file(canonical, &child_frame);
        }
    }
    rt_string_unref(joined);
    rt_string_unref(canonical);
    if (!child_scene) {
        vscn_warn_unresolved_prefab(node, "missing or unloadable source");
        return 1;
    }

    if (child_scene->root) {
        while (scene3d_node_child_count(child_scene->root) > 0) {
            rt_scene_node3d *grafted = scene_node3d_checked(child_scene->root->children[0]);
            if (!grafted || !rt_scene_node3d_try_add_child(node, grafted))
                break;
        }
    }
    for (int32_t i = 0; i < scene3d_node_child_count(node); ++i)
        vscn_mark_instance_content(scene_node3d_checked(node->children[i]));
    nested_unresolved = child_scene->unresolved_prefab_count;
    {
        void *scene_ref = child_scene;
        scene3d_release_ref(&scene_ref);
    }
    return nested_unresolved;
}

/// @brief Resolve every prefab reference in one freshly parsed scene.
/// @param scene Borrowed freshly parsed Scene3D payload.
/// @param filepath Borrowed source file path used to resolve relative references.
/// @param parent_stack Borrowed enclosing prefab frame, or `NULL` at the top level.
/// @return Nonzero after the complete hierarchy was visited; zero on setup, growth, or allocation
/// failure so the caller can reject the partially grafted scene transaction.
static int vscn_graft_prefabs(rt_scene3d *scene,
                              const char *filepath,
                              const vscn_prefab_frame *parent_stack) {
    rt_scene_node3d **stack = NULL;
    size_t count = 0;
    size_t capacity = 256;
    rt_string self_path = NULL;
    rt_string self_abs = NULL;
    rt_string base_dir = NULL;
    int32_t local_budget = VSCN_PREFAB_MAX_INSTANCES;
    vscn_prefab_frame self_frame;

    if (!scene || !scene->root || !filepath)
        return 0;
    self_path = rt_string_from_bytes(filepath, strlen(filepath));
    if (!self_path)
        return 0;
    self_abs = rt_path_abs(self_path);
    base_dir = rt_path_dir(self_path);
    rt_string_unref(self_path);
    if (!self_abs || !base_dir) {
        if (self_abs)
            rt_string_unref(self_abs);
        if (base_dir)
            rt_string_unref(base_dir);
        return 0;
    }
    self_frame.canonical_path = rt_string_cstr(self_abs);
    self_frame.parent = parent_stack;
    self_frame.depth = parent_stack ? parent_stack->depth + 1 : 1;
    self_frame.instance_budget = parent_stack ? parent_stack->instance_budget : &local_budget;

    if (capacity > SIZE_MAX / sizeof(*stack))
        goto fail;
    stack = (rt_scene_node3d **)malloc(capacity * sizeof(*stack));
    if (!stack)
        goto fail;
    stack[count++] = scene->root;
    while (count > 0) {
        rt_scene_node3d *current = stack[--count];
        if (!current)
            continue;
        if (current->prefab_path) {
            scene->unresolved_prefab_count += vscn_graft_one_prefab(current, base_dir, &self_frame);
            continue; /* grafted content never re-resolves */
        }
        for (int32_t i = 0; i < scene3d_node_child_count(current); ++i) {
            if (count >= capacity) {
                size_t next_capacity;
                rt_scene_node3d **grown;
                if (capacity > SIZE_MAX / 2u)
                    goto fail;
                next_capacity = capacity * 2u;
                if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*stack))
                    goto fail;
                grown = (rt_scene_node3d **)realloc(stack, next_capacity * sizeof(*stack));
                if (!grown)
                    goto fail;
                stack = grown;
                capacity = next_capacity;
            }
            stack[count++] = scene_node3d_checked(current->children[i]);
        }
    }
    free(stack);
    rt_string_unref(self_abs);
    rt_string_unref(base_dir);
    return 1;

fail:
    free(stack);
    rt_string_unref(self_abs);
    rt_string_unref(base_dir);
    return 0;
}

/// @brief Deserialize a Scene3D from a `.vscn` (JSON) file; returns NULL on failure.
/// @details Inverts `rt_scene3d_save`: parses the JSON, rebuilds the shared-asset arrays in
///   dependency order (textures, then cubemaps, then materials, then meshes), and finally walks the
///   node tree wiring index references back to the freshly-loaded objects. All partially-loaded
///   refs are released on any failure. glTF/FBX scenes load through rt_gltf_load / rt_fbx_load.
/// @details Borrows @p json only for the duration of the call. @p filepath is used for diagnostics
///   only — no file IO happens here, which lets streaming commit worker-staged VSCN bytes without
///   touching the disk on the main thread. The input is copied exactly once into the runtime string
///   required by the JSON parser; callers do not need to manufacture a second owned staging copy.
/// @param filepath Borrowed diagnostic/source path, or `NULL` for memory input.
/// @param json Borrowed JSON byte span; a trailing NUL is not required.
/// @param file_size Exact JSON byte count excluding the terminator.
/// @param prefab_stack Borrowed enclosing prefab frame, or `NULL`.
/// @return New owned Scene3D/Model3D handle, or `NULL` after transactional rollback.
static void *rt_scene3d_load_impl_from_buffer(const char *filepath,
                                              const char *json,
                                              size_t file_size,
                                              const vscn_prefab_frame *prefab_stack) {
    rt_string json_text = NULL;
    void *root = NULL;
    void *textures_arr = NULL;
    void *cubemaps_arr = NULL;
    void *materials_arr = NULL;
    void *meshes_arr = NULL;
    void *nodes_arr = NULL;
    void *skeletons_arr = NULL;
    void *animations_arr = NULL;
    void *node_animations_arr = NULL;
    void *cameras_arr = NULL;
    void *variant_names_arr = NULL;
    void *scenes_arr = NULL;
    int64_t version = 1;
    int tex_count = 0;
    int cubemap_count = 0;
    int material_count = 0;
    int mesh_count = 0;
    int skeleton_count = 0;
    int animation_count = 0;
    int node_animation_count = 0;
    int camera_count = 0;
    int variant_count = 0;
    int scene_count = 0;
    int asset_document = 0;
    void **textures = NULL;
    rt_cubemap3d **cubemaps = NULL;
    rt_material3d **materials = NULL;
    rt_mesh3d **meshes = NULL;
    void **skeletons = NULL;
    void **animations = NULL;
    void **node_animations = NULL;
    void **cameras = NULL;
    rt_scene3d *scene = NULL;

    if (!json)
        return NULL;
    if (!filepath)
        filepath = "<memory>";

    json_text = rt_string_from_bytes(json, file_size);
    if (!json_text)
        return NULL;
    {
        const char *json_cstr = rt_string_cstr(json_text);
        while (json_cstr && (*json_cstr == ' ' || *json_cstr == '\n' || *json_cstr == '\r' ||
                             *json_cstr == '\t'))
            json_cstr++;
        if (!json_cstr || *json_cstr != '{') {
            rt_string_unref(json_text);
            rt_asset_error_setf(
                RT_ASSET_ERROR_BAD_MAGIC, "Scene3D.Load: '%s' is not a .vscn JSON file", filepath);
            return NULL;
        }
    }
    {
        rt_string parse_message = NULL;
        if (rt_json_try_parse(json_text, &root, &parse_message, NULL, NULL) != 1 || !root) {
            rt_string_unref(parse_message);
            rt_string_unref(json_text);
            rt_asset_error_setf(
                RT_ASSET_ERROR_CORRUPT, "Scene3D.Load: '%s' has invalid JSON", filepath);
            return NULL;
        }
        rt_string_unref(parse_message);
    }
    rt_string_unref(json_text);
    json_text = NULL;

    textures_arr = vjson_get(root, "textures");
    cubemaps_arr = vjson_get(root, "cubemaps");
    materials_arr = vjson_get(root, "materials");
    meshes_arr = vjson_get(root, "meshes");
    nodes_arr = vjson_get(root, "nodes");
    skeletons_arr = vjson_get(root, "skeletons");
    animations_arr = vjson_get(root, "animations");
    node_animations_arr = vjson_get(root, "nodeAnimations");
    cameras_arr = vjson_get(root, "cameras");
    variant_names_arr = vjson_get(root, "variantNames");
    scenes_arr = vjson_get(root, "scenes");

    {
        const char *format = vjson_cstr(root, "format");
        void *version_value = vjson_get(root, "version");
        if (version_value && !vjson_value_i64_exact(version_value, &version))
            goto fail;
        if ((format && strcmp(format, "vscn") != 0) || version < 1 || version > 7)
            goto fail;
        if ((textures_arr && !vjson_is_seq(textures_arr)) ||
            (cubemaps_arr && !vjson_is_seq(cubemaps_arr)) ||
            (materials_arr && !vjson_is_seq(materials_arr)) ||
            (meshes_arr && !vjson_is_seq(meshes_arr)) || (nodes_arr && !vjson_is_seq(nodes_arr)) ||
            (skeletons_arr && !vjson_is_seq(skeletons_arr)) ||
            (animations_arr && !vjson_is_seq(animations_arr)) ||
            (node_animations_arr && !vjson_is_seq(node_animations_arr)) ||
            (cameras_arr && !vjson_is_seq(cameras_arr)) ||
            (variant_names_arr && !vjson_is_seq(variant_names_arr)) ||
            (scenes_arr && !vjson_is_seq(scenes_arr)))
            goto fail;
        asset_document = version >= 4 && scenes_arr && vjson_len(scenes_arr) > 0;
        if ((version == 4 && !asset_document) ||
            (version >= 5 && !asset_document && (!nodes_arr || !vjson_is_seq(nodes_arr))))
            goto fail;
    }

    if (vjson_len(textures_arr) > INT32_MAX || vjson_len(cubemaps_arr) > INT32_MAX ||
        vjson_len(materials_arr) > INT32_MAX || vjson_len(meshes_arr) > INT32_MAX ||
        vjson_len(skeletons_arr) > INT32_MAX || vjson_len(animations_arr) > INT32_MAX ||
        vjson_len(node_animations_arr) > INT32_MAX || vjson_len(cameras_arr) > INT32_MAX ||
        vjson_len(variant_names_arr) > INT32_MAX || vjson_len(scenes_arr) > 65536)
        goto fail;
    tex_count = (int)vjson_len(textures_arr);
    cubemap_count = (int)vjson_len(cubemaps_arr);
    material_count = (int)vjson_len(materials_arr);
    mesh_count = (int)vjson_len(meshes_arr);
    skeleton_count = (int)vjson_len(skeletons_arr);
    animation_count = (int)vjson_len(animations_arr);
    node_animation_count = (int)vjson_len(node_animations_arr);
    camera_count = (int)vjson_len(cameras_arr);
    variant_count = (int)vjson_len(variant_names_arr);
    scene_count = (int)vjson_len(scenes_arr);

    if (tex_count > 0)
        textures = (void **)calloc((size_t)tex_count, sizeof(void *));
    if (cubemap_count > 0)
        cubemaps = (rt_cubemap3d **)calloc((size_t)cubemap_count, sizeof(rt_cubemap3d *));
    if (material_count > 0)
        materials = (rt_material3d **)calloc((size_t)material_count, sizeof(rt_material3d *));
    if (mesh_count > 0)
        meshes = (rt_mesh3d **)calloc((size_t)mesh_count, sizeof(rt_mesh3d *));
    if (skeleton_count > 0)
        skeletons = (void **)calloc((size_t)skeleton_count, sizeof(void *));
    if (animation_count > 0)
        animations = (void **)calloc((size_t)animation_count, sizeof(void *));
    if (node_animation_count > 0)
        node_animations = (void **)calloc((size_t)node_animation_count, sizeof(void *));
    if (camera_count > 0)
        cameras = (void **)calloc((size_t)camera_count, sizeof(void *));
    if ((tex_count > 0 && !textures) || (cubemap_count > 0 && !cubemaps) ||
        (material_count > 0 && !materials) || (mesh_count > 0 && !meshes) ||
        (skeleton_count > 0 && !skeletons) || (animation_count > 0 && !animations) ||
        (node_animation_count > 0 && !node_animations) || (camera_count > 0 && !cameras))
        goto fail;

    for (int i = 0; i < tex_count; i++) {
        textures[i] = vscn_parse_texture(rt_seq_get(textures_arr, (int64_t)i), version);
        if (!textures[i])
            goto fail;
    }
    for (int i = 0; i < cubemap_count; i++) {
        cubemaps[i] = vscn_parse_cubemap(rt_seq_get(cubemaps_arr, (int64_t)i), textures, tex_count);
        if (!cubemaps[i])
            goto fail;
    }
    for (int i = 0; i < material_count; i++) {
        materials[i] = vscn_parse_material(
            rt_seq_get(materials_arr, (int64_t)i), textures, tex_count, cubemaps, cubemap_count);
        if (!materials[i])
            goto fail;
    }
    for (int i = 0; i < mesh_count; i++) {
        meshes[i] = vscn_parse_mesh(rt_seq_get(meshes_arr, (int64_t)i));
        if (!meshes[i])
            goto fail;
    }

    scene = (rt_scene3d *)rt_scene3d_new();
    if (!scene)
        goto fail;

    for (int i = 0; i < skeleton_count; ++i) {
        skeletons[i] = vscn_parse_skeleton(rt_seq_get(skeletons_arr, i));
        if (!skeletons[i])
            goto fail;
    }
    for (int i = 0; i < mesh_count; ++i) {
        int64_t skeleton_index = -1;
        void *mesh_obj = rt_seq_get(meshes_arr, i);
        if (!vjson_i64_exact(mesh_obj, "skeletonIndex", -1, &skeleton_index) ||
            skeleton_index < -1 || skeleton_index >= skeleton_count)
            goto fail;
        if (skeleton_index >= 0) {
            const int32_t skeleton_bones =
                skeleton3d_safe_bone_count((rt_skeleton3d *)skeletons[skeleton_index]);
            if (meshes[i]->bone_map) {
                for (int32_t local_bone = 0; local_bone < meshes[i]->bone_count; ++local_bone) {
                    if (meshes[i]->bone_map[local_bone] < 0 ||
                        meshes[i]->bone_map[local_bone] >= skeleton_bones) {
                        rt_asset_error_set(
                            RT_ASSET_ERROR_CORRUPT,
                            "Scene3D.Load: mesh bone map references a missing skeleton bone");
                        goto fail;
                    }
                }
            }
            rt_mesh3d_set_skeleton(meshes[i], skeletons[skeleton_index]);
        } else if (meshes[i]->bone_map || meshes[i]->extra_influences) {
            rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                               "Scene3D.Load: rigged mesh has no skeleton reference");
            goto fail;
        }
    }
    for (int i = 0; i < animation_count; ++i) {
        animations[i] = vscn_parse_animation(rt_seq_get(animations_arr, i));
        if (!animations[i])
            goto fail;
    }
    for (int i = 0; i < node_animation_count; ++i) {
        node_animations[i] = vscn_parse_node_animation(rt_seq_get(node_animations_arr, i));
        if (!node_animations[i])
            goto fail;
    }
    for (int i = 0; i < camera_count; ++i) {
        cameras[i] = vscn_parse_camera(rt_seq_get(cameras_arr, i));
        if (!cameras[i])
            goto fail;
    }

    if (asset_document) {
        rt_vscn_loaded_asset3d *asset =
            (rt_vscn_loaded_asset3d *)calloc(1, sizeof(rt_vscn_loaded_asset3d));
        if (!asset)
            goto fail;
        scene->baked_asset = asset;
        asset->meshes = (void **)meshes;
        asset->mesh_count = mesh_count;
        meshes = NULL;
        mesh_count = 0;
        asset->materials = (void **)materials;
        asset->material_count = material_count;
        materials = NULL;
        material_count = 0;
        asset->skeletons = skeletons;
        asset->skeleton_count = skeleton_count;
        skeletons = NULL;
        skeleton_count = 0;
        asset->animations = animations;
        asset->animation_count = animation_count;
        animations = NULL;
        animation_count = 0;
        asset->node_animations = node_animations;
        asset->node_animation_count = node_animation_count;
        node_animations = NULL;
        node_animation_count = 0;
        asset->cameras = cameras;
        asset->camera_count = camera_count;
        cameras = NULL;
        camera_count = 0;

        asset->variant_count = variant_count;
        if (variant_count > 0) {
            asset->variant_names = (char **)calloc((size_t)variant_count, sizeof(char *));
            if (!asset->variant_names)
                goto fail;
            for (int i = 0; i < variant_count; ++i) {
                rt_string name = rt_seq_get(variant_names_arr, i);
                if (!rt_string_is_handle(name) ||
                    !(asset->variant_names[i] = vscn_strdup_cstr(rt_string_cstr(name))))
                    goto fail;
            }
        }

        asset->scenes =
            (rt_vscn_loaded_scene3d *)calloc((size_t)scene_count, sizeof(*asset->scenes));
        if (!asset->scenes)
            goto fail;
        asset->scene_count = scene_count;
        for (int scene_index = 0; scene_index < scene_count; ++scene_index) {
            void *scene_obj = rt_seq_get(scenes_arr, scene_index);
            void *scene_nodes = vjson_get(scene_obj, "nodes");
            void *scene_cameras = vjson_get(scene_obj, "cameras");
            const char *scene_name = vjson_cstr(scene_obj, "name");
            rt_scene_node3d *scene_root = NULL;
            int64_t scene_camera_count;
            if (!vjson_is_map(scene_obj) || !scene_name || !vjson_is_seq(scene_nodes) ||
                !vjson_is_seq(scene_cameras))
                goto fail;
            scene_camera_count = vjson_len(scene_cameras);
            if (scene_camera_count < 0 || scene_camera_count > INT32_MAX)
                goto fail;
            asset->scenes[scene_index].name = vscn_strdup_cstr(scene_name);
            if (!asset->scenes[scene_index].name)
                goto fail;
            if (scene_index == 0) {
                scene_root = scene->root;
                rt_obj_retain_maybe(scene_root);
            } else {
                scene_root = (rt_scene_node3d *)rt_scene_node3d_new();
                if (!scene_root)
                    goto fail;
            }
            asset->scenes[scene_index].root = scene_root;
            if (!vscn_load_nodes_into_root(scene_root,
                                           scene_nodes,
                                           (rt_mesh3d **)asset->meshes,
                                           asset->mesh_count,
                                           (rt_material3d **)asset->materials,
                                           asset->material_count,
                                           asset->variant_count,
                                           asset->cameras,
                                           asset->camera_count,
                                           version))
                goto fail;
            asset->scenes[scene_index].camera_count = (int32_t)scene_camera_count;
            if (scene_camera_count > 0) {
                asset->scenes[scene_index].camera_indices =
                    (int32_t *)calloc((size_t)scene_camera_count, sizeof(int32_t));
                if (!asset->scenes[scene_index].camera_indices)
                    goto fail;
            }
            for (int64_t camera_index = 0; camera_index < scene_camera_count; ++camera_index) {
                int64_t table_index;
                if (!vjson_arr_i64_exact(scene_cameras, camera_index, -1, &table_index) ||
                    table_index < 0 || table_index >= asset->camera_count)
                    goto fail;
                asset->scenes[scene_index].camera_indices[camera_index] = (int32_t)table_index;
            }
        }
    } else {
        scene->baked_animations = animations;
        scene->baked_animation_count = animation_count;
        animations = NULL;
        animation_count = 0;
        if (!vscn_load_nodes_into_root(scene->root,
                                       nodes_arr,
                                       meshes,
                                       mesh_count,
                                       materials,
                                       material_count,
                                       0,
                                       cameras,
                                       camera_count,
                                       version))
            goto fail;
    }
    /* Document-level root metadata (scene conventions: bake.*, env.*). The
     * shared node parser reads the same "metadata" member shape. */
    if (!vscn_parse_node_metadata(scene->root, root, version)) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT, "Scene3D.Load: invalid root metadata");
        goto fail;
    }
    if (!vscn_graft_prefabs(scene, filepath, prefab_stack)) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT,
                           "Scene3D.Load: prefab traversal allocation failed");
        goto fail;
    }
    scene->node_count = scene3d_count_subtree(scene->root);
    if (scene->node_count < 0) {
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                           "Scene3D.Load: scene node hierarchy is too large");
        goto fail;
    }
    if (scene->node_count == INT32_MAX) {
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE, "Scene3D.Load: too many nodes");
        goto fail;
    }
    if (scene->node_count <= 0)
        goto fail;
    scene->last_culled_count = 0;

    vscn_release_loaded_refs((void **)skeletons, skeleton_count);
    vscn_release_loaded_refs(animations, animation_count);
    vscn_release_loaded_refs(node_animations, node_animation_count);
    vscn_release_loaded_refs(cameras, camera_count);
    vscn_release_loaded_refs((void **)meshes, mesh_count);
    vscn_release_loaded_refs((void **)materials, material_count);
    vscn_release_loaded_refs((void **)cubemaps, cubemap_count);
    vscn_release_loaded_refs((void **)textures, tex_count);
    scene3d_release_ref(&root);
    return scene;

fail:
    if (json_text)
        rt_string_unref(json_text);
    vscn_release_loaded_refs(skeletons, skeleton_count);
    vscn_release_loaded_refs(animations, animation_count);
    vscn_release_loaded_refs(node_animations, node_animation_count);
    vscn_release_loaded_refs(cameras, camera_count);
    vscn_release_loaded_refs((void **)meshes, mesh_count);
    vscn_release_loaded_refs((void **)materials, material_count);
    vscn_release_loaded_refs((void **)cubemaps, cubemap_count);
    vscn_release_loaded_refs((void **)textures, tex_count);
    scene3d_release_ref((void **)&scene);
    scene3d_release_ref(&root);
    return NULL;
}

/// @brief Read and parse one VSCN file without managing the public asset-error scope.
/// @param path Borrowed runtime-string file path.
/// @return New owned Scene3D/Model3D handle, or `NULL`.
static void *rt_scene3d_load_impl(rt_string path) {
    const char *filepath;
    char *json = NULL;
    size_t file_size;

    if (!path)
        return NULL;
    filepath = rt_string_cstr(path);
    if (!filepath)
        return NULL;

    json = vscn_read_file(filepath, &file_size);
    if (!json)
        return NULL;
    {
        void *result = rt_scene3d_load_impl_from_buffer(filepath, json, file_size, NULL);
        free(json);
        return result;
    }
}

/// @brief Deserialize a Scene3D from already-read `.vscn` JSON text (streaming staging path).
/// @details Borrows @p text while parsing, so the caller keeps ownership without an intermediate
///   native staging copy. @p path is used for diagnostics only. Returns NULL on parse failure with
///   the asset-error state populated — never traps, matching the recoverable-cell contract of the
///   streaming loader.
/// @param path Borrowed optional diagnostic/source path.
/// @param text Borrowed JSON bytes consumed synchronously during parsing.
/// @param len Exact byte count excluding any caller terminator.
/// @return New owned Scene3D/Model3D handle, or `NULL` after recoverable failure.
void *rt_scene3d_load_from_memory(rt_string path, const char *text, size_t len) {
    rt_asset_error_begin_load();
    if (!text || len == 0) {
        rt_asset_error_set(RT_ASSET_ERROR_CORRUPT, "Scene3D.Load: empty scene payload");
        rt_asset_error_end_load_failure();
        return NULL;
    }
    if (len > VSCN_MAX_FILE_BYTES || len > SIZE_MAX - 1) {
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE, "Scene3D.Load: scene payload is too large");
        rt_asset_error_end_load_failure();
        return NULL;
    }
    void *scene =
        rt_scene3d_load_impl_from_buffer(path ? rt_string_cstr(path) : "<memory>", text, len, NULL);
    if (scene) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT, "Scene3D.Load: failed to load scene");
        rt_asset_error_end_load_failure();
    }
    return scene;
}

/// @brief Load a VSCN scene or asset document from a filesystem path.
/// @param path Borrowed non-NULL runtime-string file path.
/// @return New owned Scene3D/Model3D handle, or `NULL` with asset-error state populated.
void *rt_scene3d_load(rt_string path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("Scene3D.Load: path must not be null");
        return NULL;
    }
    if (!rt_string_cstr(path)) {
        rt_asset_error_end_load_failure();
        rt_trap("Scene3D.Load: invalid path");
        return NULL;
    }
    void *scene = rt_scene3d_load_impl(path);
    if (scene) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT, "Scene3D.Load: failed to load scene");
        rt_asset_error_end_load_failure();
    }
    return scene;
}

/// @brief Wrap one owned scene load outcome as a `Zanna.Result` (ADR 0227).
/// @details Ok retains the scene and the local reference is released; err
///          carries the loader's diagnostic text or @p fallback when the
///          asset-error channel is empty.
/// @param value Owned freshly loaded scene, or `NULL` on failure.
/// @param fallback Borrowed static fallback message.
/// @return New Result carrying the scene or the failure text.
static void *scene3d_load_value_to_result(void *value, const char *fallback) {
    if (value) {
        void *result = rt_result_ok(value);
        void *local = value;
        scene3d_release_ref(&local);
        return result;
    }
    {
        const char *message = rt_asset_error_get_message();
        if (!message || message[0] == '\0') {
            rt_string err = rt_const_cstr(fallback ? fallback : "SceneGraph load failed");
            void *result = rt_result_err_str(err);
            rt_string_unref(err);
            return result;
        }
        {
            rt_string err = rt_string_from_bytes(message, strlen(message));
            void *result = rt_result_err_str(err);
            rt_str_release_maybe(err);
            return result;
        }
    }
}

/// @brief `SceneGraph.LoadResult(path)` — Result-carrying peer of `Load` (ADR 0227).
/// @param path Borrowed runtime-string file path.
/// @return New Result: ok wraps the loaded SceneGraph, err carries diagnostics.
void *rt_scene3d_load_result(rt_string path) {
    rt_asset_error_begin_load();
    if (!path || !rt_string_cstr(path)) {
        rt_asset_error_end_load_failure();
        rt_string err = rt_const_cstr("SceneGraph.LoadResult: invalid path");
        void *result = rt_result_err_str(err);
        rt_string_unref(err);
        return result;
    }
    {
        void *scene = rt_scene3d_load_impl(path);
        if (scene) {
            rt_asset_error_end_load_success();
        } else {
            rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT,
                                        "SceneGraph.LoadResult: failed to load scene");
            rt_asset_error_end_load_failure();
        }
        return scene3d_load_value_to_result(scene, "SceneGraph.LoadResult failed");
    }
}

/// @brief `SceneGraph.LoadTextResult(virtualPath, text)` — inverse of `SaveToText` (ADR 0227).
/// @details @p virtualPath names the base directory for relative prefab
///          references exactly as in `SceneAsset.LoadTextResult`; untitled
///          documents accept that relative references cannot resolve.
/// @param virtual_path Borrowed diagnostic/source path.
/// @param text Borrowed canonical VSCN text.
/// @return New Result: ok wraps the loaded SceneGraph, err carries diagnostics.
void *rt_scene3d_load_text_result(rt_string virtual_path, rt_string text) {
    const char *bytes = text ? rt_string_cstr(text) : NULL;
    if (!bytes) {
        rt_string err = rt_const_cstr("SceneGraph.LoadTextResult: invalid scene text");
        void *result = rt_result_err_str(err);
        rt_string_unref(err);
        return result;
    }
    return scene3d_load_value_to_result(
        rt_scene3d_load_from_memory(virtual_path, bytes, strlen(bytes)),
        "SceneGraph.LoadTextResult failed");
}

#endif // ZANNA_ENABLE_GRAPHICS
