//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_metadata.c
// Purpose: Bounded typed gameplay metadata for Graphics3D SceneNode values.
// Key invariants:
//   - Entries remain sorted by exact UTF-8 key bytes for deterministic saves.
//   - Mutations allocate replacement storage before publishing any change.
//   - Keys, strings, entry counts, and floating-point values are bounded.
// Ownership/Lifetime:
//   - Each SceneNode owns its native entry array, keys, and string values.
//   - Returned runtime strings and sequences follow normal caller ownership.
// Links: rt_scene3d.h, rt_scene3d_internal.h,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded, typed gameplay metadata on SceneNode3D values.
/// @details Metadata is stored in deterministic bytewise key order. Native key
///   and string allocations are owned by the node, mutations allocate fallible
///   replacement data before publishing it, and all public reads return runtime
///   values with their documented ownership.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Borrow one validated SceneNode payload.
/// @param obj Borrowed candidate runtime handle.
/// @return Borrowed SceneNode3D payload on a class match, otherwise `NULL`.
static rt_scene_node3d *metadata_node(void *obj) {
    return (rt_scene_node3d *)rt_g3d_checked_or_null(obj, RT_G3D_SCENENODE3D_CLASS_ID);
}

/// @brief Validate bounded native UTF-8 bytes used by one retained metadata entry.
/// @param data Borrowed byte span.
/// @param length Signed byte length.
/// @param maximum Maximum accepted byte length.
/// @param allow_empty Nonzero to permit an empty span.
/// @return Nonzero when the complete span is present, NUL-free, and strict UTF-8.
static int metadata_native_string_valid(const char *data,
                                        int32_t length,
                                        int32_t maximum,
                                        int allow_empty) {
    return data && length >= (allow_empty ? 0 : 1) && length <= maximum &&
           memchr(data, '\0', (size_t)length) == NULL && rt_utf8_span_valid(data, (size_t)length);
}

/// @brief Validate one retained entry's discriminator and active payload.
/// @param entry Borrowed retained metadata entry.
/// @return Nonzero when the key and selected value obey their storage contracts.
static int metadata_entry_valid(const rt_scene3d_metadata_entry *entry) {
    if (!entry || !metadata_native_string_valid(
                      entry->key, entry->key_length, RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES, 0))
        return 0;
    switch (entry->kind) {
        case RT_SCENE3D_METADATA_NULL:
        case RT_SCENE3D_METADATA_INT:
            return 1;
        case RT_SCENE3D_METADATA_BOOL:
            return entry->value.bool_value == 0 || entry->value.bool_value == 1;
        case RT_SCENE3D_METADATA_FLOAT:
            return isfinite(entry->value.float_value);
        case RT_SCENE3D_METADATA_STRING:
            return metadata_native_string_valid(entry->value.string_value.data,
                                                entry->value.string_value.length,
                                                RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES,
                                                1);
    }
    return 0;
}

/// @brief Reject structurally corrupt table bounds before indexing native data.
/// @param node Borrowed node whose metadata storage invariants are checked.
/// @return Nonzero when counts, capacity, limit, and backing storage agree.
static int metadata_table_valid(const rt_scene_node3d *node) {
    if (!node || node->metadata_count < 0 || node->metadata_capacity < 0 ||
        node->metadata_count > node->metadata_capacity ||
        node->metadata_capacity > RT_SCENE_NODE3D_MAX_METADATA_ENTRIES)
        return 0;
    if (node->metadata_capacity > 0 && !node->metadata)
        return 0;
    for (int32_t index = 0; index < node->metadata_count; ++index) {
        const rt_scene3d_metadata_entry *entry = &node->metadata[index];
        if (!metadata_entry_valid(entry))
            return 0;
        if (index > 0) {
            const rt_scene3d_metadata_entry *previous = &node->metadata[index - 1];
            int32_t common =
                previous->key_length < entry->key_length ? previous->key_length : entry->key_length;
            int compared = common > 0 ? memcmp(previous->key, entry->key, (size_t)common) : 0;
            if (compared > 0 || (compared == 0 && previous->key_length >= entry->key_length))
                return 0;
        }
    }
    return 1;
}

/// @brief Validate one runtime string as bounded, NUL-free UTF-8 bytes.
/// @param value Borrowed runtime string handle.
/// @param maximum Maximum accepted byte length.
/// @param allow_empty Nonzero to permit a zero-byte string.
/// @param out_data Output receiving a borrowed pointer into @p value.
/// @param out_length Output receiving the validated byte length.
/// @return Nonzero when the handle and bounded byte view are valid.
static int metadata_string_view(
    rt_string value, int32_t maximum, int allow_empty, const char **out_data, int32_t *out_length) {
    const char *data;
    int64_t length;
    if (!value || !rt_string_is_handle(value) || !out_data || !out_length)
        return 0;
    length = rt_str_len(value);
    data = rt_string_cstr(value);
    if (length < (allow_empty ? 0 : 1) || length > maximum ||
        !metadata_native_string_valid(data, (int32_t)length, maximum, allow_empty))
        return 0;
    *out_data = data;
    *out_length = (int32_t)length;
    return 1;
}

/// @brief Compare exact byte strings without relying on NUL termination.
/// @param left Borrowed first byte string.
/// @param left_length Non-negative first byte count.
/// @param right Borrowed second byte string.
/// @param right_length Non-negative second byte count.
/// @return A value below, equal to, or above zero under bytewise lexical ordering.
static int metadata_key_compare(const char *left,
                                int32_t left_length,
                                const char *right,
                                int32_t right_length) {
    int32_t common = left_length < right_length ? left_length : right_length;
    int compared = common > 0 ? memcmp(left, right, (size_t)common) : 0;
    if (compared != 0)
        return compared;
    if (left_length < right_length)
        return -1;
    if (left_length > right_length)
        return 1;
    return 0;
}

/// @brief Locate a key and its insertion point in the sorted table.
/// @param node Borrowed node with a structurally valid sorted metadata table.
/// @param key Borrowed exact key bytes.
/// @param key_length Non-negative key byte count.
/// @param out_found Optional output set nonzero when an equal key occupies the returned index.
/// @return Zero-based matching index or insertion position.
static int32_t metadata_find(const rt_scene_node3d *node,
                             const char *key,
                             int32_t key_length,
                             int *out_found) {
    int32_t low = 0;
    int32_t high = node && node->metadata_count > 0 ? node->metadata_count : 0;
    while (low < high) {
        int32_t middle = low + (high - low) / 2;
        const rt_scene3d_metadata_entry *entry = &node->metadata[middle];
        int compared = metadata_key_compare(entry->key, entry->key_length, key, key_length);
        if (compared < 0)
            low = middle + 1;
        else
            high = middle;
    }
    if (out_found) {
        *out_found =
            node && low < node->metadata_count &&
            metadata_key_compare(
                node->metadata[low].key, node->metadata[low].key_length, key, key_length) == 0;
    }
    return low;
}

/// @brief Allocate one NUL-terminated native byte copy.
/// @param data Borrowed source bytes.
/// @param length Non-negative number of bytes to copy.
/// @return Caller-owned NUL-terminated allocation, or `NULL` on invalid input/failure.
static char *metadata_copy_bytes(const char *data, int32_t length) {
    char *copy;
    if (!data || length < 0 || (size_t)length > SIZE_MAX - 1u)
        return NULL;
    copy = (char *)malloc((size_t)length + 1u);
    if (!copy)
        return NULL;
    if (length > 0)
        memcpy(copy, data, (size_t)length);
    copy[length] = '\0';
    return copy;
}

/// @brief Release only the value payload of one entry.
/// @param entry Borrowed mutable entry whose owned string value is cleared when present.
static void metadata_release_value(rt_scene3d_metadata_entry *entry) {
    if (!entry)
        return;
    if (entry->kind == RT_SCENE3D_METADATA_STRING) {
        free(entry->value.string_value.data);
        entry->value.string_value.data = NULL;
        entry->value.string_value.length = 0;
    }
}

/// @brief Grow the bounded table without publishing a partial allocation.
/// @param node Borrowed node whose native entry allocation may grow.
/// @param needed Minimum required entry capacity.
/// @return Nonzero on success or when capacity already suffices, otherwise zero.
static int metadata_reserve(rt_scene_node3d *node, int32_t needed) {
    int32_t capacity;
    rt_scene3d_metadata_entry *grown;
    if (!node || needed < 0 || needed > RT_SCENE_NODE3D_MAX_METADATA_ENTRIES)
        return 0;
    if (node->metadata_capacity >= needed)
        return 1;
    capacity = node->metadata_capacity > 0 ? node->metadata_capacity * 2 : 8;
    if (capacity < needed)
        capacity = needed;
    if (capacity > RT_SCENE_NODE3D_MAX_METADATA_ENTRIES)
        capacity = RT_SCENE_NODE3D_MAX_METADATA_ENTRIES;
    grown = (rt_scene3d_metadata_entry *)realloc(
        node->metadata, (size_t)capacity * sizeof(rt_scene3d_metadata_entry));
    if (!grown)
        return 0;
    node->metadata = grown;
    node->metadata_capacity = capacity;
    return 1;
}

/// @brief Publish a validated scalar as an insert or transactional replacement.
/// @details The selected scalar argument is copied according to @p kind. For a
///   new key, native key storage and any string value are allocated before the
///   sorted table is shifted and the entry count is published.
/// @param node Borrowed destination node with a valid metadata table.
/// @param key Borrowed nonempty key bytes.
/// @param key_length Bounded key byte count.
/// @param kind Scalar kind selecting the active value argument.
/// @param bool_value Boolean value used for `RT_SCENE3D_METADATA_BOOL`.
/// @param int_value Integer value used for `RT_SCENE3D_METADATA_INT`.
/// @param float_value Finite value used for `RT_SCENE3D_METADATA_FLOAT`.
/// @param string_value Borrowed bytes used for `RT_SCENE3D_METADATA_STRING`.
/// @param string_length Bounded byte count for @p string_value.
/// @return Nonzero after insertion or replacement, otherwise zero with the table unchanged.
static int8_t metadata_set(rt_scene_node3d *node,
                           const char *key,
                           int32_t key_length,
                           rt_scene3d_metadata_kind kind,
                           int8_t bool_value,
                           int64_t int_value,
                           double float_value,
                           const char *string_value,
                           int32_t string_length) {
    int found = 0;
    int32_t index;
    char *new_key = NULL;
    char *new_string = NULL;
    rt_scene3d_metadata_entry replacement;
    if (!metadata_table_valid(node) || !key || key_length <= 0 ||
        key_length > RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES)
        return 0;
    if (kind == RT_SCENE3D_METADATA_FLOAT && !isfinite(float_value))
        return 0;
    if (kind == RT_SCENE3D_METADATA_STRING) {
        if (!string_value || string_length < 0 ||
            string_length > RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES)
            return 0;
        new_string = metadata_copy_bytes(string_value, string_length);
        if (!new_string)
            return 0;
    }

    index = metadata_find(node, key, key_length, &found);
    memset(&replacement, 0, sizeof(replacement));
    replacement.kind = kind;
    if (kind == RT_SCENE3D_METADATA_BOOL)
        replacement.value.bool_value = bool_value ? 1 : 0;
    else if (kind == RT_SCENE3D_METADATA_INT)
        replacement.value.int_value = int_value;
    else if (kind == RT_SCENE3D_METADATA_FLOAT)
        replacement.value.float_value = float_value;
    else if (kind == RT_SCENE3D_METADATA_STRING) {
        replacement.value.string_value.data = new_string;
        replacement.value.string_value.length = string_length;
    }

    if (found) {
        replacement.key = node->metadata[index].key;
        replacement.key_length = node->metadata[index].key_length;
        metadata_release_value(&node->metadata[index]);
        node->metadata[index] = replacement;
        return 1;
    }
    if (node->metadata_count >= RT_SCENE_NODE3D_MAX_METADATA_ENTRIES ||
        !metadata_reserve(node, node->metadata_count + 1)) {
        free(new_string);
        return 0;
    }
    new_key = metadata_copy_bytes(key, key_length);
    if (!new_key) {
        free(new_string);
        return 0;
    }
    if (index < node->metadata_count) {
        memmove(&node->metadata[index + 1],
                &node->metadata[index],
                (size_t)(node->metadata_count - index) * sizeof(rt_scene3d_metadata_entry));
    }
    replacement.key = new_key;
    replacement.key_length = key_length;
    node->metadata[index] = replacement;
    node->metadata_count++;
    return 1;
}

/// @brief Validate a public metadata key and find it.
/// @param obj Borrowed candidate SceneNode3D handle.
/// @param key Borrowed bounded, nonempty runtime-string key.
/// @param out_node Optional output receiving the validated node, even when the key is absent.
/// @param out_index Optional output receiving the matching or insertion index.
/// @return Borrowed matching entry, or `NULL` for invalid input or an absent key.
static const rt_scene3d_metadata_entry *metadata_get_entry(void *obj,
                                                           rt_string key,
                                                           rt_scene_node3d **out_node,
                                                           int32_t *out_index) {
    rt_scene_node3d *node = metadata_node(obj);
    const char *key_data;
    int32_t key_length;
    int found = 0;
    int32_t index;
    if (out_node)
        *out_node = node;
    if (!metadata_table_valid(node) ||
        !metadata_string_view(
            key, RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES, 0, &key_data, &key_length))
        return NULL;
    index = metadata_find(node, key_data, key_length, &found);
    if (out_index)
        *out_index = index;
    return found ? &node->metadata[index] : NULL;
}

/// @brief Importer entry (ADR 0294): set one metadata entry from native bytes.
/// @details Same bounds and replace-or-insert semantics as the public setters.
int8_t rt_scene_node3d_metadata_import_internal(rt_scene_node3d *node,
                                                const char *key,
                                                int32_t key_length,
                                                int32_t kind,
                                                int8_t bool_value,
                                                int64_t int_value,
                                                double float_value,
                                                const char *string_value,
                                                int32_t string_length) {
    if (kind < (int32_t)RT_SCENE3D_METADATA_NULL || kind > (int32_t)RT_SCENE3D_METADATA_STRING)
        return 0;
    return metadata_set(node,
                        key,
                        key_length,
                        (rt_scene3d_metadata_kind)kind,
                        bool_value,
                        int_value,
                        float_value,
                        string_value,
                        string_length);
}

/// @brief Deep-copy every metadata entry of @p src onto @p dst (ADR 0294).
/// @details Used by the glTF scene-root clone so authored `extras` survive template construction.
int8_t rt_scene_node3d_metadata_copy_internal(rt_scene_node3d *dst, const rt_scene_node3d *src) {
    if (!dst || !src || !metadata_table_valid(src))
        return 0;
    for (int32_t index = 0; index < src->metadata_count; ++index) {
        const rt_scene3d_metadata_entry *entry = &src->metadata[index];
        const char *string_value = NULL;
        int32_t string_length = 0;
        if (entry->kind == RT_SCENE3D_METADATA_STRING) {
            string_value = entry->value.string_value.data;
            string_length = entry->value.string_value.length;
        }
        if (!metadata_set(dst,
                          entry->key,
                          entry->key_length,
                          entry->kind,
                          entry->value.bool_value,
                          entry->value.int_value,
                          entry->value.float_value,
                          string_value,
                          string_length))
            return 0;
    }
    return 1;
}

/// @brief Release every native allocation owned by one node metadata table.
/// @param node Borrowed node whose keys, string values, and entry array are cleared.
void rt_scene_node3d_metadata_clear_internal(rt_scene_node3d *node) {
    if (!node)
        return;
    int32_t count = metadata_table_valid(node) ? node->metadata_count : 0;
    for (int32_t index = 0; index < count; ++index) {
        free(node->metadata[index].key);
        node->metadata[index].key = NULL;
        metadata_release_value(&node->metadata[index]);
    }
    free(node->metadata);
    node->metadata = NULL;
    node->metadata_count = 0;
    node->metadata_capacity = 0;
}

/// @brief Return all metadata keys in deterministic bytewise order.
/// @param obj Borrowed SceneNode3D handle.
/// @return New owned runtime sequence containing newly referenced string keys.
void *rt_scene_node3d_metadata_keys(void *obj) {
    rt_scene_node3d *node = metadata_node(obj);
    void *keys = rt_seq_new_owned();
    if (!metadata_table_valid(node))
        return keys;
    for (int32_t index = 0; index < node->metadata_count; ++index) {
        rt_string key = rt_string_from_bytes(node->metadata[index].key,
                                             (size_t)node->metadata[index].key_length);
        if (!key)
            continue;
        rt_seq_push(keys, key);
        rt_string_unref(key);
    }
    return keys;
}

/// @brief Report the scalar kind stored for a metadata key.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @return Runtime string `"null"`, `"bool"`, `"int"`, `"float"`, or `"string"`;
///   returns the empty runtime string when the key/input is invalid or absent.
rt_string rt_scene_node3d_metadata_kind(void *obj, rt_string key) {
    const rt_scene3d_metadata_entry *entry = metadata_get_entry(obj, key, NULL, NULL);
    if (!entry)
        return rt_const_cstr("");
    switch (entry->kind) {
        case RT_SCENE3D_METADATA_NULL:
            return rt_const_cstr("null");
        case RT_SCENE3D_METADATA_BOOL:
            return rt_const_cstr("bool");
        case RT_SCENE3D_METADATA_INT:
            return rt_const_cstr("int");
        case RT_SCENE3D_METADATA_FLOAT:
            return rt_const_cstr("float");
        case RT_SCENE3D_METADATA_STRING:
            return rt_const_cstr("string");
    }
    return rt_const_cstr("");
}

/// @brief Determine whether a node contains an exact metadata key.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @return Nonzero when the validated key is present.
int8_t rt_scene_node3d_metadata_has(void *obj, rt_string key) {
    return metadata_get_entry(obj, key, NULL, NULL) ? 1 : 0;
}

/// @brief Read an integer metadata value without coercing other kinds.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @param def Fallback returned for invalid input, absence, or a kind mismatch.
/// @return Stored integer or @p def.
int64_t rt_scene_node3d_metadata_get_int(void *obj, rt_string key, int64_t def) {
    const rt_scene3d_metadata_entry *entry = metadata_get_entry(obj, key, NULL, NULL);
    return entry && entry->kind == RT_SCENE3D_METADATA_INT ? entry->value.int_value : def;
}

/// @brief Read floating-point metadata, widening stored integers when necessary.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @param def Fallback returned for invalid input, absence, or an incompatible kind.
/// @return Stored float, widened integer, or @p def.
double rt_scene_node3d_metadata_get_float(void *obj, rt_string key, double def) {
    const rt_scene3d_metadata_entry *entry = metadata_get_entry(obj, key, NULL, NULL);
    if (!entry)
        return def;
    if (entry->kind == RT_SCENE3D_METADATA_FLOAT)
        return entry->value.float_value;
    if (entry->kind == RT_SCENE3D_METADATA_INT)
        return (double)entry->value.int_value;
    return def;
}

/// @brief Read a Boolean metadata value without coercing other kinds.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @param def Fallback truth value for invalid input, absence, or a kind mismatch.
/// @return Canonical `0` or `1` stored/fallback truth value.
int8_t rt_scene_node3d_metadata_get_bool(void *obj, rt_string key, int8_t def) {
    const rt_scene3d_metadata_entry *entry = metadata_get_entry(obj, key, NULL, NULL);
    return entry && entry->kind == RT_SCENE3D_METADATA_BOOL ? (entry->value.bool_value ? 1 : 0)
                                                            : (def ? 1 : 0);
}

/// @brief Read string metadata with caller-supplied fallback semantics.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @param def Borrowed fallback string, or an invalid/NULL handle for an empty fallback.
/// @return New runtime string containing the stored bytes, a retained @p def, or
///   the empty runtime string.
rt_string rt_scene_node3d_metadata_get_string(void *obj, rt_string key, rt_string def) {
    const rt_scene3d_metadata_entry *entry = metadata_get_entry(obj, key, NULL, NULL);
    if (entry && entry->kind == RT_SCENE3D_METADATA_STRING)
        return rt_string_from_bytes(entry->value.string_value.data,
                                    (size_t)entry->value.string_value.length);
    if (def && rt_string_is_handle(def))
        return rt_string_ref(def);
    return rt_const_cstr("");
}

/// @brief Insert or replace a metadata key with the explicit null kind.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed bounded, nonempty metadata key.
/// @return Nonzero after a successful mutation, otherwise zero.
int8_t rt_scene_node3d_metadata_set_null(void *obj, rt_string key) {
    rt_scene_node3d *node = metadata_node(obj);
    const char *key_data;
    int32_t key_length;
    if (!node || !metadata_string_view(
                     key, RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES, 0, &key_data, &key_length))
        return 0;
    return metadata_set(node, key_data, key_length, RT_SCENE3D_METADATA_NULL, 0, 0, 0.0, NULL, 0);
}

/// @brief Insert or replace an integer metadata value.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed bounded, nonempty metadata key.
/// @param value Integer value to store.
/// @return Nonzero after a successful mutation, otherwise zero.
int8_t rt_scene_node3d_metadata_set_int(void *obj, rt_string key, int64_t value) {
    rt_scene_node3d *node = metadata_node(obj);
    const char *key_data;
    int32_t key_length;
    if (!node || !metadata_string_view(
                     key, RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES, 0, &key_data, &key_length))
        return 0;
    return metadata_set(
        node, key_data, key_length, RT_SCENE3D_METADATA_INT, 0, value, 0.0, NULL, 0);
}

/// @brief Insert or replace a finite floating-point metadata value.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed bounded, nonempty metadata key.
/// @param value Finite double-precision value to store.
/// @return Nonzero after a successful mutation, otherwise zero.
int8_t rt_scene_node3d_metadata_set_float(void *obj, rt_string key, double value) {
    rt_scene_node3d *node = metadata_node(obj);
    const char *key_data;
    int32_t key_length;
    if (!node || !isfinite(value) ||
        !metadata_string_view(
            key, RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES, 0, &key_data, &key_length))
        return 0;
    return metadata_set(
        node, key_data, key_length, RT_SCENE3D_METADATA_FLOAT, 0, 0, value, NULL, 0);
}

/// @brief Insert or replace a Boolean metadata value.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed bounded, nonempty metadata key.
/// @param value Truth value canonicalized to zero or one.
/// @return Nonzero after a successful mutation, otherwise zero.
int8_t rt_scene_node3d_metadata_set_bool(void *obj, rt_string key, int8_t value) {
    rt_scene_node3d *node = metadata_node(obj);
    const char *key_data;
    int32_t key_length;
    if (!node || !metadata_string_view(
                     key, RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES, 0, &key_data, &key_length))
        return 0;
    return metadata_set(
        node, key_data, key_length, RT_SCENE3D_METADATA_BOOL, value, 0, 0.0, NULL, 0);
}

/// @brief Insert or replace a bounded string metadata value.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed bounded, nonempty metadata key.
/// @param value Borrowed bounded, NUL-free runtime string; an empty value is allowed.
/// @return Nonzero after copying and publishing the value, otherwise zero.
int8_t rt_scene_node3d_metadata_set_string(void *obj, rt_string key, rt_string value) {
    rt_scene_node3d *node = metadata_node(obj);
    const char *key_data;
    const char *value_data;
    int32_t key_length;
    int32_t value_length;
    if (!node ||
        !metadata_string_view(
            key, RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES, 0, &key_data, &key_length) ||
        !metadata_string_view(
            value, RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES, 1, &value_data, &value_length))
        return 0;
    return metadata_set(node,
                        key_data,
                        key_length,
                        RT_SCENE3D_METADATA_STRING,
                        0,
                        0,
                        0.0,
                        value_data,
                        value_length);
}

/// @brief Remove an exact metadata key and release its native storage.
/// @param obj Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @return Nonzero when an entry was removed, otherwise zero.
int8_t rt_scene_node3d_metadata_remove(void *obj, rt_string key) {
    rt_scene_node3d *node = NULL;
    int32_t index = 0;
    const rt_scene3d_metadata_entry *entry = metadata_get_entry(obj, key, &node, &index);
    if (!node || !entry)
        return 0;
    free(node->metadata[index].key);
    metadata_release_value(&node->metadata[index]);
    if (index + 1 < node->metadata_count) {
        memmove(&node->metadata[index],
                &node->metadata[index + 1],
                (size_t)(node->metadata_count - index - 1) * sizeof(rt_scene3d_metadata_entry));
    }
    node->metadata_count--;
    memset(&node->metadata[node->metadata_count], 0, sizeof(rt_scene3d_metadata_entry));
    return 1;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
