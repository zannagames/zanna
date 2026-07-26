//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_streaming.inc
// Purpose: Private WorldStream3D implementation included by rt_game3d.c —
//   bounded manifest parsing, cell and terrain staging, load/unload selection,
//   proxy and sidecar management, residency budgets, and stream telemetry.
// Key invariants:
//   - Manifest-derived counts, sizes, paths, and coordinates are validated
//     before allocation or use.
//   - Recompute order is deterministic by squared distance and manifest index.
//   - Worker staging publishes mutations through the main-thread commit queue.
// Ownership/Lifetime:
//   - The enclosing WorldStream3D payload owns parsed arrays, staged resources,
//     retained strings, proxies, terrain attachments, and candidate buffers.
// Links: rt_game3d.c, rt_game3d_internal.h, rt_untrusted_count.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements private manifest-driven WorldStream3D residency management.
/// @details This include is compiled as part of `rt_game3d.c`. It parses
/// untrusted cell and terrain manifests, stages resources synchronously or on
/// workers, commits them under frame and memory budgets, manages lower-cost
/// proxies, and keeps load ordering and telemetry deterministic.

/// @brief Measure a manifest string in bytes with a saturating signed result.
/// @param value Borrowed runtime string, or `NULL`.
/// @return Manifest byte length bounded to `INT64_MAX`.
static int64_t game3d_stream_manifest_bytes(rt_string value);

/// @brief Add two signed integers without overflow.
/// @param a First addend.
/// @param b Second addend.
/// @return Saturated signed sum.
static int64_t game3d_i64_saturating_add(int64_t a, int64_t b);

#define game3d_stream_fseek(fp, off, whence) rt_file_stdio_seek64((fp), (off), (whence))
#define game3d_stream_ftell(fp) rt_file_stdio_tell64((fp))

/* Recompute passes a budget-evicted item waits before it may reload (~0.5 s at 60 Hz). */
#define GAME3D_STREAM_BUDGET_EVICT_COOLDOWN 30

/// @brief Whether a parsed JSON value is a map/object.
/// @param obj Borrowed parsed JSON handle.
/// @return Nonzero when @p obj has the runtime map class.
static int game3d_json_is_map(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_MAP_CLASS_ID;
}

/// @brief Whether a parsed JSON value is an array/sequence.
/// @param obj Borrowed parsed JSON handle.
/// @return Nonzero when @p obj has the runtime sequence class.
static int game3d_json_is_seq(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_SEQ_CLASS_ID;
}

/// @brief Look up @p key in a parsed JSON map (NULL if absent or not a map).
/// @param map Borrowed parsed JSON map handle.
/// @param key Null-terminated object key.
/// @return Borrowed mapped value, or `NULL` when absent, invalid, or allocation fails.
static void *game3d_json_get(void *map, const char *key) {
    if (!game3d_json_is_map(map) || !key)
        return NULL;
    rt_string k = rt_string_from_bytes(key, strlen(key));
    if (!k)
        return NULL;
    void *value = rt_map_get(map, k);
    rt_string_unref(k);
    return value;
}

/// @brief Coerce a parsed JSON value to a double; returns 0 if it is not numeric.
/// @param obj Borrowed boxed numeric candidate.
/// @param[out] out Required destination written only after successful coercion.
/// @return Nonzero for a boxed floating-point or integer value; otherwise zero.
static int game3d_json_number(void *obj, double *out) {
    if (!out)
        return 0;
    double f = 0.0;
    if (rt_box_try_to_f64(obj, &f)) {
        *out = f;
        return 1;
    }
    int64_t i = 0;
    if (rt_box_try_to_i64(obj, &i)) {
        *out = (double)i;
        return 1;
    }
    return 0;
}

/// @brief Read map[key] as an int64, returning @p fallback if absent or non-numeric.
/// @param map Borrowed parsed JSON map handle.
/// @param key Null-terminated object key.
/// @param fallback Value returned for missing or non-numeric data.
/// @return Decoded integer, a saturated conversion from finite floating point, or @p fallback.
static int64_t game3d_json_i64_or(void *map, const char *key, int64_t fallback) {
    void *value = game3d_json_get(map, key);
    int64_t i = 0;
    if (rt_box_try_to_i64(value, &i))
        return i;
    double f = 0.0;
    if (rt_box_try_to_f64(value, &f) && isfinite(f)) {
        if (f <= (double)INT64_MIN)
            return INT64_MIN;
        if (f >= (double)INT64_MAX)
            return INT64_MAX;
        return (int64_t)f;
    }
    return fallback;
}

/// @brief Read map[key] as a double, returning @p fallback if absent or non-numeric.
/// @param map Borrowed parsed JSON map handle.
/// @param key Null-terminated object key.
/// @param fallback Value returned for missing, non-numeric, or non-finite data.
/// @return Finite decoded number or @p fallback.
static double game3d_json_f64_or(void *map, const char *key, double fallback) {
    double v = 0.0;
    if (game3d_json_number(game3d_json_get(map, key), &v) && isfinite(v))
        return v;
    return fallback;
}

/// @brief Read map[key] as a boolean, accepting boxed bools and numeric 0/1 values.
/// @param map Borrowed parsed JSON map handle.
/// @param key Null-terminated object key.
/// @param fallback Value returned for unsupported or missing data.
/// @return Normalized zero or one from the decoded value, or @p fallback.
static int8_t game3d_json_bool_or(void *map, const char *key, int8_t fallback) {
    void *value = game3d_json_get(map, key);
    int8_t b = 0;
    if (rt_box_try_to_i1(value, &b))
        return b ? 1 : 0;
    int64_t i = 0;
    if (rt_box_try_to_i64(value, &i))
        return i != 0 ? 1 : 0;
    double f = 0.0;
    if (rt_box_try_to_f64(value, &f) && isfinite(f))
        return f != 0.0 ? 1 : 0;
    return fallback;
}

/// @brief Read map[key] as a borrowed string reference (NULL if absent or not a string).
/// @param map Borrowed parsed JSON map handle.
/// @param key Null-terminated object key.
/// @return New reference to the runtime string, or `NULL` when absent or not a string.
static rt_string game3d_json_string_ref(void *map, const char *key) {
    void *value = game3d_json_get(map, key);
    if (!value || !rt_string_is_handle(value))
        return NULL;
    return rt_string_ref((rt_string)value);
}

/// @brief Read map[key] as a 3-element number array into @p out, falling back to @p f on absence.
/// @param map Borrowed parsed JSON map handle.
/// @param key Null-terminated object key.
/// @param[out] out Required three-component destination.
/// @param fallback Optional three-component fallback; `NULL` selects zero.
/// @return Nonzero when all three finite components were decoded; otherwise zero after writing the
/// fallback.
static int game3d_json_vec3_or(void *map,
                               const char *key,
                               double out[3],
                               const double fallback[3]) {
    if (!out)
        return 0;
    out[0] = fallback ? fallback[0] : 0.0;
    out[1] = fallback ? fallback[1] : 0.0;
    out[2] = fallback ? fallback[2] : 0.0;
    void *seq = game3d_json_get(map, key);
    if (!game3d_json_is_seq(seq) || rt_seq_len(seq) < 3)
        return 0;
    double v[3];
    for (int i = 0; i < 3; ++i) {
        if (!game3d_json_number(rt_seq_get(seq, i), &v[i]) || !isfinite(v[i]))
            return 0;
    }
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    return 1;
}

/// @brief Read optional manifest text through the asset manager or filesystem without trapping.
/// @details Packed/embedded asset lookup is tried first so `asset://` stream manifests work. The
///          byte cap is enforced for both sources; missing or oversized manifests return NULL.
/// @param path Borrowed asset or UTF-8 filesystem path.
/// @return New runtime string containing the bytes, or `NULL` when unavailable, oversized, or
/// unreadable.
static rt_string game3d_read_text_file_no_trap(rt_string path) {
    const char *cpath = path ? rt_string_cstr(path) : NULL;
    if (!cpath || cpath[0] == '\0')
        return NULL;
    size_t asset_size = 0;
    uint8_t *asset_bytes = rt_asset_load_raw(path, &asset_size);
    if (asset_bytes) {
        rt_string result = NULL;
        if ((uint64_t)asset_size <= RT_GAME3D_STREAM_MANIFEST_MAX_BYTES)
            result = rt_string_from_bytes((const char *)asset_bytes, asset_size);
        else
            rt_trap("Game3D.WorldStream3D: manifest asset exceeds byte budget");
        free(asset_bytes);
        return result;
    }
    FILE *file = rt_file_stdio_open_utf8(cpath, "rb");
    if (!file)
        return NULL;
    if (game3d_stream_fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    int64_t size = (int64_t)game3d_stream_ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    if ((uint64_t)size > RT_GAME3D_STREAM_MANIFEST_MAX_BYTES) {
        rt_trap("Game3D.WorldStream3D: manifest file exceeds byte budget");
        fclose(file);
        return NULL;
    }
    if (game3d_stream_fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *bytes = (char *)malloc((size_t)size + 1u);
    if (!bytes) {
        fclose(file);
        return NULL;
    }
    if (size > 0 && fread(bytes, 1u, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    bytes[(size_t)size] = '\0';
    rt_string result = rt_string_from_bytes(bytes, (size_t)size);
    free(bytes);
    return result;
}

/// @brief Validate an optional manifest count declaration against the manifest byte budget.
/// @param root Borrowed parsed manifest object.
/// @param key Null-terminated count-field name.
/// @param manifest_bytes Total bytes available to justify the declared count.
/// @return Nonzero when absent or within the untrusted-count bound; otherwise zero.
static int game3d_stream_declared_count_ok(void *root, const char *key, size_t manifest_bytes) {
    if (!root || !key)
        return 0;
    if (!game3d_json_get(root, key))
        return 1;
    return rt_untrusted_count_ok(game3d_json_i64_or(root, key, -1), 1u, manifest_bytes);
}

/// @brief Whether @p path is absolute (leading '/' on POSIX, drive/UNC prefix on Windows).
/// @param path Null-terminated UTF-8 path.
/// @return Nonzero for an absolute POSIX, drive-prefixed, rooted, or UNC-style path.
static int game3d_path_is_absolute(const char *path) {
    if (!path || path[0] == '\0')
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 1;
#if RT_PLATFORM_WINDOWS
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':')
        return 1;
#endif
    return 0;
}

/// @brief Return whether @p path begins with a URI-like scheme before any slash.
/// @details Used to preserve `asset://...` manifest references while applying
///          filesystem traversal checks only to filesystem-style sidecars.
/// @param path Null-terminated path or URI candidate.
/// @return Nonzero when a syntactically valid scheme is followed by `://`.
static int game3d_path_has_scheme(const char *path) {
    if (!path || !isalpha((unsigned char)path[0]))
        return 0;
    for (const char *p = path + 1; *p; ++p) {
        if (*p == ':' && p[1] == '/' && p[2] == '/')
            return 1;
        if (*p == '/' || *p == '\\')
            return 0;
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.')
            return 0;
    }
    return 0;
}

/// @brief Return whether a filesystem-style path contains a parent-directory segment.
/// @details Checks complete path segments separated by `/` or `\\`, so names
///          like `level..json` remain valid while `../level` and `a/../b` are rejected.
/// @param path Null-terminated filesystem-style path.
/// @return Nonzero when any complete segment is exactly `..`.
static int game3d_path_has_parent_segment(const char *path) {
    const char *seg = path;
    if (!path)
        return 0;
    while (*seg) {
        while (*seg == '/' || *seg == '\\')
            ++seg;
        const char *end = seg;
        while (*end && *end != '/' && *end != '\\')
            ++end;
        if ((size_t)(end - seg) == 2u && seg[0] == '.' && seg[1] == '.')
            return 1;
        seg = end;
    }
    return 0;
}

/// @brief Resolve a manifest-relative resource path against the manifest's own directory.
/// @details Absolute references pass through; relative ones are joined to the manifest's folder so
/// a
///          streaming manifest can reference sibling assets portably.
/// @param manifest_path Borrowed path of the owning manifest.
/// @param payload Borrowed resource reference from the manifest.
/// @return New runtime-string reference containing an absolute/pass-through or joined path.
static rt_string game3d_stream_resolve_manifest_path(rt_string manifest_path, rt_string payload) {
    const char *raw = payload ? rt_string_cstr(payload) : NULL;
    const char *manifest = manifest_path ? rt_string_cstr(manifest_path) : NULL;
    if (!raw || raw[0] == '\0')
        return rt_string_ref(rt_const_cstr(""));
    if (game3d_path_is_absolute(raw) || !manifest || manifest[0] == '\0')
        return rt_string_ref(payload);
    const char *slash = strrchr(manifest, '/');
    const char *backslash = strrchr(manifest, '\\');
    const char *sep =
        slash && backslash ? (slash > backslash ? slash : backslash) : (slash ? slash : backslash);
    if (!sep)
        return rt_string_ref(payload);
    size_t dir_len = (size_t)(sep - manifest + 1);
    size_t raw_len = strlen(raw);
    if (dir_len > SIZE_MAX - raw_len)
        return rt_string_ref(payload);
    char *joined = (char *)malloc(dir_len + raw_len + 1u);
    if (!joined)
        return rt_string_ref(payload);
    memcpy(joined, manifest, dir_len);
    memcpy(joined + dir_len, raw, raw_len + 1u);
    rt_string result = rt_string_from_bytes(joined, dir_len + raw_len);
    free(joined);
    return result;
}

/// @brief Resolve a manifest entry's "sidecar" (or fallback "binarySidecar") path against the
/// manifest.
/// @param entry Borrowed parsed cell or terrain entry.
/// @param manifest_path Borrowed path of the owning manifest.
/// @return Owned resolved path string, or an empty string when neither key is present.
static rt_string game3d_json_resolved_sidecar_ref(void *entry, rt_string manifest_path) {
    rt_string raw = game3d_json_string_ref(entry, "sidecar");
    if (!raw)
        raw = game3d_json_string_ref(entry, "binarySidecar");
    if (!raw)
        return rt_string_ref(rt_const_cstr(""));
    const char *raw_cstr = rt_string_cstr(raw);
    if (!game3d_path_has_scheme(raw_cstr) && game3d_path_has_parent_segment(raw_cstr)) {
        rt_string_unref(raw);
        return rt_string_ref(rt_const_cstr(""));
    }
    if (game3d_path_has_scheme(raw_cstr))
        return raw;
    rt_string resolved = game3d_stream_resolve_manifest_path(manifest_path, raw);
    rt_string_unref(raw);
    return resolved;
}

/// @brief Return @p layer when it is a valid layer index, otherwise @p fallback.
/// @param layer Candidate collision layer.
/// @param fallback Replacement for an out-of-range layer.
/// @return @p layer when valid; otherwise @p fallback.
static int64_t game3d_stream_layer_or_valid(int64_t layer, int64_t fallback) {
    return game3d_valid_layer(layer) ? layer : fallback;
}

/// @brief Parse collision metadata for a streaming cell/tile from its JSON manifest entry.
/// @details Reads the collision layer ("layer", with "collisionLayer" and a nested
/// "collision.layer"
///          taking precedence), the collision mask ("collisionMask" or nested "collision.mask",
///          sanitized to valid bits), and the enabled flag (defaulting on, overridden by a boolean
///          "collision" value or nested "collision.enabled"). Every out-pointer is optional; layer
///          and mask fall back to @p default_layer and all-bits when unspecified, and the matching
///          has-flag reports whether the manifest actually supplied a value.
/// @param entry Borrowed parsed cell or terrain JSON object.
/// @param default_layer Layer used when no valid manifest layer is present.
/// @param[out] out_layer Optional resolved collision-layer destination.
/// @param[out] out_has_layer Optional flag reporting an explicit valid layer.
/// @param[out] out_mask Optional sanitized collision-mask destination.
/// @param[out] out_has_mask Optional flag reporting an explicit mask.
/// @param[out] out_enabled Optional normalized collision-enabled destination.
static void game3d_stream_parse_collision_metadata(void *entry,
                                                   int64_t default_layer,
                                                   int64_t *out_layer,
                                                   int8_t *out_has_layer,
                                                   int64_t *out_mask,
                                                   int8_t *out_has_mask,
                                                   int8_t *out_enabled) {
    if (out_enabled)
        *out_enabled = 1;
    int64_t layer = game3d_json_i64_or(entry, "layer", 0);
    int8_t has_layer = game3d_valid_layer(layer) ? 1 : 0;
    int64_t collision_layer = game3d_json_i64_or(entry, "collisionLayer", 0);
    if (game3d_valid_layer(collision_layer)) {
        layer = collision_layer;
        has_layer = 1;
    }
    int64_t collision_mask = game3d_json_i64_or(entry, "collisionMask", 0);
    int8_t has_mask = game3d_json_get(entry, "collisionMask") ? 1 : 0;
    if (out_enabled && game3d_json_get(entry, "collision"))
        *out_enabled = game3d_json_bool_or(entry, "collision", 1);

    void *collision = game3d_json_get(entry, "collision");
    if (game3d_json_is_map(collision)) {
        if (out_enabled)
            *out_enabled = game3d_json_bool_or(collision, "enabled", 1);
        int64_t nested_layer = game3d_json_i64_or(collision, "layer", 0);
        if (game3d_valid_layer(nested_layer)) {
            layer = nested_layer;
            has_layer = 1;
        }
        int64_t nested_mask = game3d_json_i64_or(collision, "mask", 0);
        if (game3d_json_get(collision, "mask")) {
            collision_mask = nested_mask;
            has_mask = 1;
        }
    }

    if (out_layer)
        *out_layer = has_layer ? layer : default_layer;
    if (out_has_layer)
        *out_has_layer = has_layer;
    if (out_mask)
        *out_mask = has_mask ? game3d_sanitize_mask_bits(collision_mask) : ~(int64_t)0;
    if (out_has_mask)
        *out_has_mask = has_mask;
}

/// @brief Read a manifest entry's "traversalCost", defaulting to 1.0 and clamping to the range (0,
/// 1e6].
/// @param entry Borrowed parsed cell or terrain JSON object.
/// @return Positive finite traversal cost no greater than one million.
static double game3d_stream_traversal_cost_or_default(void *entry) {
    double cost = game3d_json_f64_or(entry, "traversalCost", 1.0);
    if (!isfinite(cost) || cost <= 0.0)
        cost = 1.0;
    if (cost > 1000000.0)
        cost = 1000000.0;
    return cost;
}

/// @brief Resident byte size attributed to a streaming cell (0 if not loaded).
/// @param cell Borrowed cell record, or `NULL`.
/// @return Positive measured or declared residency estimate, defaulting to 64 KiB.
static int64_t game3d_stream_cell_bytes(const rt_game3d_stream_cell *cell) {
    if (!cell)
        return 64 * 1024;
    if (cell->resident && cell->measured_resident_bytes > 0)
        return cell->measured_resident_bytes;
    if (cell->resident_bytes <= 0)
        return 64 * 1024;
    return cell->resident_bytes;
}

/// @brief Resident byte size attributed to a streaming terrain tile (0 if not loaded).
/// @param tile Borrowed terrain-tile record, or `NULL`.
/// @return Saturated sum of resident and sidecar bytes, defaulting to 256 KiB.
static int64_t game3d_stream_terrain_tile_bytes(const rt_game3d_stream_terrain_tile *tile) {
    if (!tile || tile->resident_bytes <= 0)
        return 256 * 1024;
    if (tile->sidecar_bytes <= 0)
        return tile->resident_bytes;
    return game3d_i64_saturating_add(tile->resident_bytes, tile->sidecar_bytes);
}

/// @brief Squared distance from the stream center to a cell's center (drives load/unload ordering).
/// @param stream Borrowed stream defining the current center.
/// @param cell Borrowed manifest cell.
/// @return Finite squared XZ distance, or `HUGE_VAL` for invalid or non-finite input.
static double game3d_stream_cell_distance_sq(const rt_game3d_world_stream *stream,
                                             const rt_game3d_stream_cell *cell) {
    if (!stream || !cell || !isfinite(stream->center[0]) || !isfinite(stream->center[2]) ||
        !isfinite(cell->center[0]) || !isfinite(cell->center[2]))
        return HUGE_VAL;
    double sx = game3d_clamp_coord_or(stream->center[0], 0.0);
    double sz = game3d_clamp_coord_or(stream->center[2], 0.0);
    double cx = game3d_clamp_coord_or(cell->center[0], 0.0);
    double cz = game3d_clamp_coord_or(cell->center[2], 0.0);
    double dx = sx - cx;
    double dz = sz - cz;
    if (!isfinite(dx) || !isfinite(dz))
        return HUGE_VAL;
    double distance_sq = dx * dx + dz * dz;
    return isfinite(distance_sq) ? distance_sq : HUGE_VAL;
}

/// @brief Squared distance from the stream center to a terrain tile's center.
/// @param stream Borrowed stream defining the current center.
/// @param tile Borrowed manifest terrain tile.
/// @return Finite squared XZ distance, or `HUGE_VAL` for invalid or non-finite input.
static double game3d_stream_terrain_tile_distance_sq(const rt_game3d_world_stream *stream,
                                                     const rt_game3d_stream_terrain_tile *tile) {
    if (!stream || !tile || !isfinite(stream->center[0]) || !isfinite(stream->center[2]) ||
        !isfinite(tile->center[0]) || !isfinite(tile->center[2]))
        return HUGE_VAL;
    double sx = game3d_clamp_coord_or(stream->center[0], 0.0);
    double sz = game3d_clamp_coord_or(stream->center[2], 0.0);
    double tx = game3d_clamp_coord_or(tile->center[0], 0.0);
    double tz = game3d_clamp_coord_or(tile->center[2], 0.0);
    double dx = sx - tx;
    double dz = sz - tz;
    if (!isfinite(dx) || !isfinite(dz))
        return HUGE_VAL;
    double distance_sq = dx * dx + dz * dz;
    return isfinite(distance_sq) ? distance_sq : HUGE_VAL;
}

/// @brief Sanitize a terrain axis scale to a positive finite value (defaulting when invalid).
/// @param value Candidate scale component.
/// @return Positive finite scale no greater than one million, defaulting to one.
static double game3d_stream_terrain_axis_scale(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 1.0;
    if (value > 1000000.0)
        return 1000000.0;
    return value;
}

/// @brief Compute the render-space bounds of a terrain tile from its manifest center + scale.
/// @param tile Borrowed terrain-tile record, or `NULL` for unit bounds at the origin.
/// @param[out] min_x Optional minimum X destination.
/// @param[out] max_x Optional maximum X destination.
/// @param[out] min_z Optional minimum Z destination.
/// @param[out] max_z Optional maximum Z destination.
static void game3d_stream_terrain_tile_bounds(const rt_game3d_stream_terrain_tile *tile,
                                              double *min_x,
                                              double *max_x,
                                              double *min_z,
                                              double *max_z) {
    double sx = game3d_stream_terrain_axis_scale(tile ? tile->scale[0] : 1.0);
    double sz = game3d_stream_terrain_axis_scale(tile ? tile->scale[2] : 1.0);
    double width = tile && tile->width > 1 ? (double)(tile->width - 1) * sx : sx;
    double depth = tile && tile->depth > 1 ? (double)(tile->depth - 1) * sz : sz;
    double cx = game3d_clamp_coord_or(tile ? tile->center[0] : 0.0, 0.0);
    double cz = game3d_clamp_coord_or(tile ? tile->center[2] : 0.0, 0.0);
    if (!isfinite(width) || width < 0.0)
        width = 0.0;
    if (!isfinite(depth) || depth < 0.0)
        depth = 0.0;
    if (min_x)
        *min_x = cx - width * 0.5;
    if (max_x)
        *max_x = cx + width * 0.5;
    if (min_z)
        *min_z = cz - depth * 0.5;
    if (max_z)
        *max_z = cz + depth * 0.5;
}

/// @brief Floating-point tolerance for deciding whether two tile edges touch.
/// @param a Borrowed first terrain tile, or `NULL`.
/// @param b Borrowed second terrain tile, or `NULL`.
/// @return Finite tolerance scaled to the largest participating terrain sample spacing.
static double game3d_stream_terrain_seam_tolerance(const rt_game3d_stream_terrain_tile *a,
                                                   const rt_game3d_stream_terrain_tile *b) {
    double tol = 0.00001;
    if (a) {
        double ax = game3d_stream_terrain_axis_scale(a->scale[0]) * 0.0001;
        double az = game3d_stream_terrain_axis_scale(a->scale[2]) * 0.0001;
        if (ax > tol)
            tol = ax;
        if (az > tol)
            tol = az;
    }
    if (b) {
        double bx = game3d_stream_terrain_axis_scale(b->scale[0]) * 0.0001;
        double bz = game3d_stream_terrain_axis_scale(b->scale[2]) * 0.0001;
        if (bx > tol)
            tol = bx;
        if (bz > tol)
            tol = bz;
    }
    return tol;
}

/// @brief Finite-aware approximate equality: true when both values are finite and within @p
/// tolerance.
/// @param a First value.
/// @param b Second value.
/// @param tolerance Maximum absolute difference.
/// @return Nonzero only when both inputs are finite and within @p tolerance.
static int game3d_stream_almost_equal(double a, double b, double tolerance) {
    return isfinite(a) && isfinite(b) && fabs(a - b) <= tolerance;
}

/// @brief True when two [min,max] ranges match end-to-end within @p tolerance (terrain seam test).
/// @param a_min First range minimum.
/// @param a_max First range maximum.
/// @param b_min Second range minimum.
/// @param b_max Second range maximum.
/// @param tolerance Maximum endpoint difference.
/// @return Nonzero when both range endpoints approximately match.
static int game3d_stream_range_matches(
    double a_min, double a_max, double b_min, double b_max, double tolerance) {
    return game3d_stream_almost_equal(a_min, b_min, tolerance) &&
           game3d_stream_almost_equal(a_max, b_max, tolerance);
}

/// @brief Square a sanitized stream radius without producing a non-finite result.
/// @param radius Candidate radius.
/// @return Finite non-negative squared radius, saturating at `DBL_MAX`.
static double game3d_stream_radius_sq(double radius);

/// @brief qsort comparator ordering stream load candidates by ascending squared
///   distance, breaking ties by ascending index; NULL/non-finite distances sort last.
/// @param lhs Borrowed pointer to the first load candidate.
/// @param rhs Borrowed pointer to the second load candidate.
/// @return Negative, zero, or positive according to deterministic load order.
static int game3d_stream_load_candidate_cmp(const void *lhs, const void *rhs) {
    const rt_game3d_stream_load_candidate *a = (const rt_game3d_stream_load_candidate *)lhs;
    const rt_game3d_stream_load_candidate *b = (const rt_game3d_stream_load_candidate *)rhs;
    double ad = a ? a->distance_sq : HUGE_VAL;
    double bd = b ? b->distance_sq : HUGE_VAL;
    if (!isfinite(ad))
        ad = HUGE_VAL;
    if (!isfinite(bd))
        bd = HUGE_VAL;
    if (ad < bd)
        return -1;
    if (ad > bd)
        return 1;
    int32_t ai = a ? a->index : INT32_MAX;
    int32_t bi = b ? b->index : INT32_MAX;
    return (ai > bi) - (ai < bi);
}

/// @brief Return the number of readable manifest cell slots.
/// @param stream Borrowed stream payload.
/// @return Cell count clamped to allocated capacity, or zero without valid storage.
static int32_t game3d_world_stream_safe_cell_count(const rt_game3d_world_stream *stream);

/// @brief Return the number of readable manifest terrain-tile slots.
/// @param stream Borrowed stream payload.
/// @return Terrain count clamped to allocated capacity, or zero without valid storage.
static int32_t game3d_world_stream_safe_terrain_tile_count(const rt_game3d_world_stream *stream);

/// @brief Release a cell's staged full-detail payload.
/// @param cell Cell record whose staging fields are cleared.
/// @param record_stale Nonzero to increment stale-result telemetry.
static void game3d_stream_cell_drop_staged(rt_game3d_stream_cell *cell, int record_stale);

/// @brief Release a terrain tile's staged payload.
/// @param tile Terrain record whose staging fields are cleared.
/// @param record_stale Nonzero to increment stale-result telemetry.
static void game3d_stream_terrain_tile_drop_staged(rt_game3d_stream_terrain_tile *tile,
                                                   int record_stale);

/// @brief Release a cell's staged proxy payload.
/// @param cell Cell record whose proxy staging fields are cleared.
/// @param record_stale Nonzero to increment stale-result telemetry.
static void game3d_stream_cell_drop_staged_proxy(rt_game3d_stream_cell *cell, int record_stale);

/// @brief Unload a resident cell proxy and release its scene/entity references.
/// @param stream Borrowed owning stream.
/// @param cell Cell record whose proxy is removed.
static void game3d_world_stream_unload_cell_proxy(rt_game3d_world_stream *stream,
                                                  rt_game3d_stream_cell *cell);

/// @brief Mark a cell proxy load as failed and apply retry bookkeeping.
/// @param cell Cell record whose proxy request failed.
static void game3d_stream_cell_proxy_failed(rt_game3d_stream_cell *cell);

/// @brief Test whether a cell should be resident under current hysteresis.
/// @param stream Borrowed stream defining center and radii.
/// @param cell Borrowed cell record.
/// @return Nonzero when the cell lies within its active threshold.
static int game3d_stream_cell_desired(const rt_game3d_world_stream *stream,
                                      const rt_game3d_stream_cell *cell);

/// @brief Test whether a terrain tile should be resident under current hysteresis.
/// @param stream Borrowed stream defining center and radii.
/// @param tile Borrowed terrain-tile record.
/// @return Nonzero when the tile lies within its active threshold.
static int game3d_stream_terrain_tile_desired(const rt_game3d_world_stream *stream,
                                              const rt_game3d_stream_terrain_tile *tile);

/// @brief Grow a reusable stream-load candidate buffer to at least @p needed entries.
/// @details The stream owns candidate arrays for the lifetime of the WorldStream3D
///   object. Reusing them avoids per-update allocation churn while still allowing
///   manifests to grow after remount. Returns 0 on allocation failure; callers then
///   use the deterministic no-allocation scanner.
/// @param[in,out] items Address of the stream-owned candidate-array pointer.
/// @param[in,out] capacity Address of the current allocated entry capacity.
/// @param needed Minimum entries required.
/// @return Nonzero when existing or grown storage is usable; zero on allocation failure.
static int game3d_stream_reserve_load_candidates(rt_game3d_stream_load_candidate **items,
                                                 int32_t *capacity,
                                                 int32_t needed) {
    if (!items || !capacity || needed <= 0)
        return 1;
    if (*capacity >= needed && *items)
        return 1;
    rt_game3d_stream_load_candidate *grown =
        (rt_game3d_stream_load_candidate *)realloc(*items, (size_t)needed * sizeof(**items));
    if (!grown)
        return 0;
    *items = grown;
    *capacity = needed;
    return 1;
}

/// @brief True if a load candidate tuple sorts after the previously emitted tuple.
/// @details Used by the no-allocation nearest-first fallback. Distances are sanitized
///   to put non-finite values last, and index breaks ties so the ordering exactly
///   matches `game3d_stream_load_candidate_cmp`.
/// @param distance_sq Candidate squared distance.
/// @param index Candidate manifest index.
/// @param previous_distance_sq Previously emitted squared distance.
/// @param previous_index Previously emitted manifest index, or a negative sentinel.
/// @return Nonzero when the candidate sorts strictly after the previous tuple.
static int game3d_stream_candidate_after(double distance_sq,
                                         int32_t index,
                                         double previous_distance_sq,
                                         int32_t previous_index) {
    if (!isfinite(distance_sq))
        distance_sq = HUGE_VAL;
    if (!isfinite(previous_distance_sq))
        previous_distance_sq = HUGE_VAL;
    if (previous_index < 0)
        return 1;
    if (distance_sq > previous_distance_sq)
        return 1;
    if (distance_sq < previous_distance_sq)
        return 0;
    return index > previous_index;
}

/// @brief Pick the next desired cell in sorted order without allocating or marking cells.
/// @param stream Borrowed stream containing the manifest cells.
/// @param previous_distance_sq Squared distance of the previously emitted cell.
/// @param previous_index Index of the previously emitted cell, or a negative sentinel.
/// @param[out] out_distance_sq Optional destination for the selected distance.
/// @return Selected cell index, or `-1` when no later desired cell exists.
static int32_t game3d_world_stream_pick_next_cell_noalloc(const rt_game3d_world_stream *stream,
                                                          double previous_distance_sq,
                                                          int32_t previous_index,
                                                          double *out_distance_sq) {
    int32_t best_index = -1;
    double best_distance = HUGE_VAL;
    int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
    for (int32_t i = 0; i < cell_count; ++i) {
        const rt_game3d_stream_cell *cell = &stream->cells[i];
        if (!game3d_stream_cell_desired(stream, cell))
            continue;
        double distance_sq = game3d_stream_cell_distance_sq(stream, cell);
        if (!isfinite(distance_sq))
            distance_sq = HUGE_VAL;
        if (!game3d_stream_candidate_after(distance_sq, i, previous_distance_sq, previous_index))
            continue;
        if (best_index < 0 || distance_sq < best_distance ||
            (distance_sq == best_distance && i < best_index)) {
            best_index = i;
            best_distance = distance_sq;
        }
    }
    if (out_distance_sq)
        *out_distance_sq = best_distance;
    return best_index;
}

/// @brief Pick the next desired terrain tile in sorted order without allocating.
/// @param stream Borrowed stream containing the manifest terrain tiles.
/// @param previous_distance_sq Squared distance of the previously emitted tile.
/// @param previous_index Index of the previously emitted tile, or a negative sentinel.
/// @param[out] out_distance_sq Optional destination for the selected distance.
/// @return Selected terrain index, or `-1` when no later desired tile exists.
static int32_t game3d_world_stream_pick_next_terrain_noalloc(const rt_game3d_world_stream *stream,
                                                             double previous_distance_sq,
                                                             int32_t previous_index,
                                                             double *out_distance_sq) {
    int32_t best_index = -1;
    double best_distance = HUGE_VAL;
    int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
    for (int32_t i = 0; i < tile_count; ++i) {
        const rt_game3d_stream_terrain_tile *tile = &stream->terrain_tiles[i];
        if (!game3d_stream_terrain_tile_desired(stream, tile))
            continue;
        double distance_sq = game3d_stream_terrain_tile_distance_sq(stream, tile);
        if (!isfinite(distance_sq))
            distance_sq = HUGE_VAL;
        if (!game3d_stream_candidate_after(distance_sq, i, previous_distance_sq, previous_index))
            continue;
        if (best_index < 0 || distance_sq < best_distance ||
            (distance_sq == best_distance && i < best_index)) {
            best_index = i;
            best_distance = distance_sq;
        }
    }
    if (out_distance_sq)
        *out_distance_sq = best_distance;
    return best_index;
}

/// @brief Increment a pending-work counter, saturating at INT64_MAX and ignoring NULL.
/// @param[in,out] pending Optional counter to increment.
static void game3d_stream_increment_pending(int64_t *pending) {
    if (!pending || *pending == INT64_MAX)
        return;
    ++*pending;
}

/// @brief Number of stream cells safe to read from the private manifest array.
/// @param stream Borrowed stream payload.
/// @return Cell count clamped to allocated capacity, or zero without valid storage.
static int32_t game3d_world_stream_safe_cell_count(const rt_game3d_world_stream *stream) {
    if (!stream || !stream->cells || stream->cell_count <= 0 || stream->cell_capacity <= 0)
        return 0;
    return stream->cell_count < stream->cell_capacity ? stream->cell_count : stream->cell_capacity;
}

/// @brief Number of terrain tiles safe to read from the private manifest array.
/// @param stream Borrowed stream payload.
/// @return Terrain count clamped to allocated capacity, or zero without valid storage.
static int32_t game3d_world_stream_safe_terrain_tile_count(const rt_game3d_world_stream *stream) {
    if (!stream || !stream->terrain_tiles || stream->terrain_tile_count <= 0 ||
        stream->terrain_tile_capacity <= 0)
        return 0;
    return stream->terrain_tile_count < stream->terrain_tile_capacity
               ? stream->terrain_tile_count
               : stream->terrain_tile_capacity;
}

/// @brief Clamp a signed telemetry counter to a non-negative value.
/// @param value Candidate counter.
/// @return @p value when positive; otherwise zero.
static int64_t game3d_world_stream_nonnegative_i64(int64_t value) {
    return value > 0 ? value : 0;
}

/// @brief Resident cell telemetry, capped to parsed manifest size when array-backed.
/// @param stream Borrowed stream payload.
/// @return Non-negative resident-cell count capped to readable manifest slots.
static int64_t game3d_world_stream_safe_resident_cell_count(const rt_game3d_world_stream *stream) {
    int64_t count = stream ? game3d_world_stream_nonnegative_i64(stream->resident_cell_count) : 0;
    if (stream && stream->cells_manifest_loaded) {
        int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
        if (count > cell_count)
            count = cell_count;
    }
    return count;
}

/// @brief Resident terrain telemetry, capped to parsed manifest size when array-backed.
/// @param stream Borrowed stream payload.
/// @return Non-negative resident-tile count capped to readable manifest slots.
static int64_t game3d_world_stream_safe_resident_terrain_tile_count(
    const rt_game3d_world_stream *stream) {
    int64_t count =
        stream ? game3d_world_stream_nonnegative_i64(stream->resident_terrain_tile_count) : 0;
    if (stream && stream->terrain_manifest_loaded) {
        int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
        if (count > tile_count)
            count = tile_count;
    }
    return count;
}

/// @brief True when a streaming cell lies within its activation radius. Resident cells
///   test against the larger unload radius, giving load/unload hysteresis.
/// @param stream Borrowed stream defining the center and hysteresis radii.
/// @param cell Borrowed manifest cell.
/// @return Nonzero when the cell should remain or become resident.
static int game3d_stream_cell_desired(const rt_game3d_world_stream *stream,
                                      const rt_game3d_stream_cell *cell) {
    if (!stream || !cell)
        return 0;
    double radius = cell->radius;
    double threshold = (cell->resident ? stream->unload_radius : stream->load_radius) + radius;
    if (!isfinite(threshold) || threshold < 0.0)
        threshold = radius;
    return game3d_stream_cell_distance_sq(stream, cell) <= game3d_stream_radius_sq(threshold);
}

/// @brief True when a terrain tile lies within its activation radius. Resident tiles
///   test against the larger unload radius, giving load/unload hysteresis.
/// @param stream Borrowed stream defining the center and hysteresis radii.
/// @param tile Borrowed manifest terrain tile.
/// @return Nonzero when the tile should remain or become resident.
static int game3d_stream_terrain_tile_desired(const rt_game3d_world_stream *stream,
                                              const rt_game3d_stream_terrain_tile *tile) {
    if (!stream || !tile)
        return 0;
    double radius = tile->radius;
    double threshold = (tile->resident ? stream->unload_radius : stream->load_radius) + radius;
    if (!isfinite(threshold) || threshold < 0.0)
        threshold = radius;
    return game3d_stream_terrain_tile_distance_sq(stream, tile) <=
           game3d_stream_radius_sq(threshold);
}

/// @brief Total resident bytes across all currently-loaded streaming cells.
/// @param stream Borrowed stream payload.
/// @return Saturated sum of manifest and resident cell bytes.
static int64_t game3d_world_stream_resident_cell_bytes(const rt_game3d_world_stream *stream) {
    if (!stream)
        return 0;
    int64_t bytes = game3d_stream_manifest_bytes(stream->cells_manifest);
    int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
    for (int32_t i = 0; i < cell_count; ++i) {
        if (stream->cells[i].resident)
            bytes = game3d_i64_saturating_add(bytes, game3d_stream_cell_bytes(&stream->cells[i]));
    }
    return bytes;
}

/// @brief Unload a resident terrain tile and all attached resources.
/// @param stream Borrowed owning stream.
/// @param tile Terrain record to unload.
static void game3d_world_stream_unload_terrain_tile(rt_game3d_world_stream *stream,
                                                    rt_game3d_stream_terrain_tile *tile);

/// @brief Return a terrain tile's runtime Terrain3D only when the private slot is still valid.
/// @param tile Borrowed terrain-tile record.
/// @return Borrowed live Terrain3D handle, or `NULL` when absent, stale, or non-resident.
static void *game3d_stream_terrain_tile_terrain_ref(const rt_game3d_stream_terrain_tile *tile) {
    return tile && tile->resident ? rt_g3d_checked_or_null(tile->terrain, RT_G3D_TERRAIN3D_CLASS_ID)
                                  : NULL;
}

/// @brief Return a terrain tile's attached Game3D entity only when the private slot is valid.
/// @param entity Borrowed opaque entity handle.
/// @return Borrowed live Entity3D payload, or `NULL` when invalid or stale.
static void *game3d_stream_terrain_tile_entity_ref(void *entity) {
    rt_game3d_entity *typed =
        (rt_game3d_entity *)rt_g3d_checked_or_null(entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    return typed && typed->alive ? typed : NULL;
}

/// @brief Despawn/release collider and nav entities attached to a resident terrain tile.
/// @param stream Borrowed owning stream, or `NULL` during detached cleanup.
/// @param tile Terrain record whose spatial-source entities are removed.
static void game3d_world_stream_detach_terrain_spatial_sources(
    rt_game3d_world_stream *stream, rt_game3d_stream_terrain_tile *tile) {
    if (!tile)
        return;
    if (tile->nav_entity && stream && stream->world) {
        rt_game3d_entity *entity =
            (rt_game3d_entity *)game3d_stream_terrain_tile_entity_ref(tile->nav_entity);
        if (entity && entity->spawned && entity->world == stream->world)
            rt_game3d_world_despawn(stream->world, tile->nav_entity);
    }
    game3d_release_typed_ref(&tile->nav_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    if (tile->collider_entity && stream && stream->world) {
        rt_game3d_entity *entity =
            (rt_game3d_entity *)game3d_stream_terrain_tile_entity_ref(tile->collider_entity);
        if (entity && entity->spawned && entity->world == stream->world)
            rt_game3d_world_despawn(stream->world, tile->collider_entity);
    }
    game3d_release_typed_ref(&tile->collider_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
}

/// @brief Unload and free every terrain tile tracked by the stream.
/// @param stream Borrowed stream payload to clear.
static void game3d_world_stream_clear_terrain_tiles(rt_game3d_world_stream *stream) {
    if (!stream)
        return;
    int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
    stream->terrain_generation++; /* in-flight staging results for these tiles are now stale */
    for (int32_t i = 0; i < tile_count; ++i) {
        rt_game3d_stream_terrain_tile *tile = &stream->terrain_tiles[i];
        game3d_world_stream_unload_terrain_tile(stream, tile);
        game3d_stream_terrain_tile_drop_staged(tile, tile->staged ? 1 : 0);
        free(tile->holes);
        tile->holes = NULL;
        tile->manifest_hole_count = 0;
        game3d_release_ref((void **)&tile->name);
        game3d_release_ref((void **)&tile->path);
        game3d_release_ref((void **)&tile->heightmap_path);
        game3d_release_ref((void **)&tile->material);
        game3d_release_ref((void **)&tile->nav_area);
        game3d_release_ref((void **)&tile->sidecar_path);
    }
    free(stream->terrain_tiles);
    stream->terrain_tiles = NULL;
    stream->terrain_tile_count = 0;
    stream->terrain_tile_capacity = 0;
    stream->terrain_manifest_loaded = 0;
}

/// @brief Unload a single terrain tile: detach its collider/nav source and release its resources.
/// @param stream Borrowed owning stream, or `NULL` during detached cleanup.
/// @param tile Terrain record to reset; `NULL` is ignored.
static void game3d_world_stream_unload_terrain_tile(rt_game3d_world_stream *stream,
                                                    rt_game3d_stream_terrain_tile *tile) {
    if (!tile)
        return;
    game3d_world_stream_detach_terrain_spatial_sources(stream, tile);
    game3d_release_typed_ref(&tile->terrain, RT_G3D_TERRAIN3D_CLASS_ID);
    free(tile->sidecar_data);
    tile->sidecar_data = NULL;
    tile->sidecar_bytes = 0;
    tile->resident = 0;
}

/// @brief Advance past ASCII whitespace in a heightmap text buffer.
/// @param p Current null-terminated parser position, or `NULL`.
/// @return Pointer to the first non-space byte, end terminator, or `NULL`.
static const char *game3d_heightmap_skip_space(const char *p) {
    while (p && *p && isspace((unsigned char)*p))
        ++p;
    return p;
}

/// @brief Read the next whitespace-delimited token from a heightmap text buffer into @p out.
/// @param[in,out] cursor Address of the current parser position, advanced on success.
/// @param[out] out Destination buffer receiving a null-terminated token.
/// @param out_size Available destination bytes including the terminator.
/// @return 1 on success, 0 at end of input or if the token would overflow.
static int game3d_heightmap_read_word(const char **cursor, char *out, size_t out_size) {
    if (!cursor || !*cursor || !out || out_size == 0)
        return 0;
    const char *p = game3d_heightmap_skip_space(*cursor);
    if (!p || *p == '\0')
        return 0;
    size_t len = 0;
    while (p[len] && !isspace((unsigned char)p[len]))
        ++len;
    if (len == 0 || len >= out_size)
        return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    *cursor = p + len;
    return 1;
}

/// @brief Read the next integer token from a heightmap text buffer (0 if none/invalid).
/// @param[in,out] cursor Address of the current parser position, advanced on success.
/// @param[out] out Required destination for the decoded signed integer.
/// @return Nonzero on a complete in-range token; otherwise zero without advancing.
static int game3d_heightmap_read_i64(const char **cursor, int64_t *out) {
    if (!cursor || !*cursor || !out)
        return 0;
    char *end = NULL;
    const char *p = game3d_heightmap_skip_space(*cursor);
    errno = 0;
    long long value = strtoll(p, &end, 10);
    if (!end || end == p || errno == ERANGE || (*end != '\0' && !isspace((unsigned char)*end)) ||
        value < (long long)INT64_MIN || value > (long long)INT64_MAX)
        return 0;
    *cursor = end;
    *out = (int64_t)value;
    return 1;
}

/// @brief Read the next floating-point token from a heightmap text buffer (0 if none/invalid).
/// @param[in,out] cursor Address of the current parser position, advanced on success.
/// @param[out] out Required destination for the decoded finite value.
/// @return Nonzero on a complete finite token; otherwise zero without advancing.
static int game3d_heightmap_read_f64(const char **cursor, double *out) {
    if (!cursor || !*cursor || !out)
        return 0;
    char *end = NULL;
    const char *p = game3d_heightmap_skip_space(*cursor);
    errno = 0;
    double value = strtod(p, &end);
    if (!end || end == p || errno == ERANGE || !isfinite(value) ||
        (*end != '\0' && !isspace((unsigned char)*end)))
        return 0;
    *cursor = end;
    *out = value;
    return 1;
}

/// @brief Map a normalized height value to a grayscale RGBA color for the heightmap preview image.
/// @param h Height sample clamped to `[0, 1]`.
/// @return Packed two-byte grayscale representation in the RG channels with opaque alpha.
static uint32_t game3d_heightmap_sample_rgba(double h) {
    if (h < 0.0)
        h = 0.0;
    else if (h > 1.0)
        h = 1.0;
    uint32_t sample = (uint32_t)llround(h * 65535.0);
    if (sample > 65535u)
        sample = 65535u;
    return ((sample >> 8) & 0xFFu) << 24 | (sample & 0xFFu) << 16 | 0x000000FFu;
}

/// @brief Map a destination heightmap sample to a source sample, preserving both endpoints.
/// @param dst Destination sample index.
/// @param dst_count Number of destination samples on the axis.
/// @param src_count Number of source samples on the axis.
/// @return Nearest bounded source index.
static int64_t game3d_heightmap_resample_index(int64_t dst, int64_t dst_count, int64_t src_count) {
    int64_t numerator;
    int64_t denominator;
    int64_t value;
    if (dst_count <= 1 || src_count <= 1 || dst <= 0)
        return 0;
    if (dst >= dst_count - 1)
        return src_count - 1;
    numerator = dst * (src_count - 1);
    denominator = dst_count - 1;
    value = (numerator + denominator / 2) / denominator;
    if (value < 0)
        return 0;
    if (value >= src_count)
        return src_count - 1;
    return value;
}

/// @brief Parse heightmap text into a malloc-owned POD sample grid (worker-safe: no rt objects).
/// @param cursor Null-terminated `zanna-heightmap-v1` payload.
/// @param[out] out_samples Receives caller-owned sample storage on success.
/// @param[out] out_width Receives the decoded width.
/// @param[out] out_depth Receives the decoded depth.
/// @return 1 on success (caller owns *out_samples), 0 on a malformed payload.
static int game3d_heightmap_parse_text(const char *cursor,
                                       double **out_samples,
                                       int64_t *out_width,
                                       int64_t *out_depth) {
    char magic[32];
    int64_t width = 0;
    int64_t depth = 0;
    if (!cursor || !out_samples || !out_width || !out_depth)
        return 0;
    *out_samples = NULL;
    *out_width = 0;
    *out_depth = 0;
    if (!game3d_heightmap_read_word(&cursor, magic, sizeof(magic)) ||
        strcmp(magic, "zanna-heightmap-v1") != 0 || !game3d_heightmap_read_i64(&cursor, &width) ||
        !game3d_heightmap_read_i64(&cursor, &depth) || width < 1 || depth < 1 || width > 4096 ||
        depth > 4096)
        return 0;

    size_t sample_count = (size_t)width * (size_t)depth;
    if (sample_count > SIZE_MAX / sizeof(double))
        return 0;
    double *samples = (double *)calloc(sample_count, sizeof(double));
    if (!samples)
        return 0;

    for (int64_t z = 0; z < depth; ++z) {
        for (int64_t x = 0; x < width; ++x) {
            double h = 0.0;
            if (!game3d_heightmap_read_f64(&cursor, &h)) {
                free(samples);
                return 0;
            }
            if (h < 0.0)
                h = 0.0;
            else if (h > 1.0)
                h = 1.0;
            samples[(size_t)z * (size_t)width + (size_t)x] = h;
        }
    }
    cursor = game3d_heightmap_skip_space(cursor);
    if (cursor && *cursor != '\0') {
        free(samples);
        return 0;
    }
    *out_samples = samples;
    *out_width = width;
    *out_depth = depth;
    return 1;
}

/// @brief Resample a POD height grid into a heightmap Pixels image (main thread only).
/// @param samples Borrowed row-major normalized source samples.
/// @param width Source grid width.
/// @param depth Source grid depth.
/// @param target_width Requested output width; invalid values use @p width.
/// @param target_depth Requested output depth; invalid values use @p depth.
/// @return New GC-managed Pixels handle, or `NULL` for invalid input or allocation failure.
static void *game3d_heightmap_pixels_from_samples(const double *samples,
                                                  int64_t width,
                                                  int64_t depth,
                                                  int64_t target_width,
                                                  int64_t target_depth) {
    if (!samples || width < 1 || depth < 1)
        return NULL;
    if (target_width < 1 || target_width > 4096)
        target_width = width;
    if (target_depth < 1 || target_depth > 4096)
        target_depth = depth;
    void *pixels = rt_pixels_new(target_width, target_depth);
    if (!pixels)
        return NULL;
    for (int64_t z = 0; z < target_depth; ++z) {
        int64_t src_z = game3d_heightmap_resample_index(z, target_depth, depth);
        for (int64_t x = 0; x < target_width; ++x) {
            int64_t src_x = game3d_heightmap_resample_index(x, target_width, width);
            double h = samples[(size_t)src_z * (size_t)width + (size_t)src_x];
            rt_pixels_set(pixels, x, z, (int64_t)game3d_heightmap_sample_rgba(h));
        }
    }
    return pixels;
}

/// @brief Load a terrain tile's heightmap sidecar file into a height grid (and optional preview
/// image).
/// @details Parses the text heightmap format (dimensions + height samples); returns NULL if the
///          sidecar is missing or malformed.
/// @param path Borrowed asset or filesystem path.
/// @param target_width Requested resampled image width.
/// @param target_depth Requested resampled image depth.
/// @return New GC-managed Pixels heightmap, or `NULL` when absent, malformed, or allocation fails.
static void *game3d_load_heightmap_sidecar(rt_string path,
                                           int64_t target_width,
                                           int64_t target_depth) {
    if (!game3d_string_has_bytes(path))
        return NULL;
    rt_string text = game3d_read_text_file_no_trap(path);
    if (!text)
        return NULL;

    double *samples = NULL;
    int64_t width = 0;
    int64_t depth = 0;
    if (!game3d_heightmap_parse_text(rt_string_cstr(text), &samples, &width, &depth)) {
        rt_string_unref(text);
        return NULL;
    }
    void *pixels =
        game3d_heightmap_pixels_from_samples(samples, width, depth, target_width, target_depth);
    free(samples);
    rt_string_unref(text);
    return pixels;
}

/// @brief Load a terrain tile's optional binary sidecar payload into residency.
/// @details Terrain sidecars are opaque editor/gameplay metadata parallel to scene
///   cell sidecars. Missing, empty, or unreadable files are recoverable and simply
///   leave the tile with zero sidecar bytes. Any previous payload is freed first so
///   remounts and reloads do not leak memory.
/// @param tile Terrain record whose owned sidecar buffer is replaced.
static void game3d_stream_terrain_tile_load_sidecar(rt_game3d_stream_terrain_tile *tile) {
    if (!tile)
        return;
    free(tile->sidecar_data);
    tile->sidecar_data = NULL;
    tile->sidecar_bytes = 0;
    if (!game3d_string_has_bytes(tile->sidecar_path))
        return;
    size_t size = 0;
    uint8_t *data = game3d_asset_read_file_bytes(rt_string_cstr(tile->sidecar_path), &size);
    if (!data)
        return;
    if (size == 0) {
        free(data);
        return;
    }
    tile->sidecar_data = data;
    tile->sidecar_bytes = (int64_t)(size > (size_t)INT64_MAX ? (size_t)INT64_MAX : size);
}

/// @brief Build the unique collider name for a terrain tile (used to register/find its physics
/// collider).
/// @param tile Borrowed terrain record supplying the base name.
/// @return New runtime string containing the derived name, or a fallback constant reference.
static rt_string game3d_terrain_collider_name(const rt_game3d_stream_terrain_tile *tile) {
    const char *base = tile && tile->name ? rt_string_cstr(tile->name) : "terrain_tile";
    const char *suffix = "_heightfield_collider";
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffix);
    if (base_len > SIZE_MAX - suffix_len - 1u)
        return rt_const_cstr("terrain_tile_heightfield_collider");
    char *name = (char *)malloc(base_len + suffix_len + 1u);
    if (!name)
        return rt_const_cstr("terrain_tile_heightfield_collider");
    memcpy(name, base, base_len);
    memcpy(name + base_len, suffix, suffix_len + 1u);
    rt_string result = rt_string_from_bytes(name, base_len + suffix_len);
    free(name);
    return result;
}

/// @brief Create and register a heightfield collider for a loaded terrain tile.
/// @param stream Borrowed owning stream with a live world and physics service.
/// @param tile Terrain record receiving the retained collider entity.
/// @param heightmap Borrowed Pixels heightmap used to construct the collider.
static void game3d_world_stream_attach_terrain_collider(rt_game3d_world_stream *stream,
                                                        rt_game3d_stream_terrain_tile *tile,
                                                        void *heightmap) {
    if (!stream || !tile || !heightmap)
        return;
    if (tile->collider_entity) {
        if (game3d_stream_terrain_tile_entity_ref(tile->collider_entity))
            return;
        game3d_release_typed_ref(&tile->collider_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    }
    if (!tile->collision_enabled)
        return;
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(stream->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (!world || world->destroyed || !world->physics)
        return;

    void *collider =
        rt_collider3d_new_heightfield(heightmap,
                                      game3d_stream_terrain_axis_scale(tile->scale[0]),
                                      game3d_stream_terrain_axis_scale(tile->scale[1]),
                                      game3d_stream_terrain_axis_scale(tile->scale[2]));
    if (!collider)
        return;
    void *body = rt_body3d_new(0.0);
    if (!body) {
        game3d_release_ref(&collider);
        return;
    }
    rt_body3d_set_static(body, 1);
    rt_body3d_set_collision_layer(body,
                                  game3d_stream_layer_or_valid(tile->layer, RT_GAME3D_LAYER_WORLD));
    rt_body3d_set_collision_mask(body,
                                 tile->has_collision_mask ? tile->collision_mask : ~(int64_t)0);
    rt_body3d_set_collider(body, collider);
    {
        /* Holes carved into the tile terrain fall through physics too. */
        void *tile_terrain = game3d_stream_terrain_tile_terrain_ref(tile);
        if (tile_terrain) {
            int32_t hole_cells_x = 0;
            int32_t hole_cells_z = 0;
            const uint8_t *hole_mask =
                rt_terrain3d_get_hole_mask_raw(tile_terrain, &hole_cells_x, &hole_cells_z);
            if (hole_mask)
                (void)rt_collider3d_heightfield_set_holes_raw(
                    collider, hole_mask, hole_cells_x, hole_cells_z);
        }
    }

    void *entity = rt_game3d_entity_new();
    if (entity) {
        rt_string name = game3d_terrain_collider_name(tile);
        rt_game3d_entity_set_name(entity, name);
        rt_string_unref(name);
        rt_game3d_entity_set_position(entity,
                                      game3d_clamp_coord_or(tile->center[0], 0.0),
                                      game3d_clamp_coord_or(tile->center[1], 0.0),
                                      game3d_clamp_coord_or(tile->center[2], 0.0));
        rt_game3d_entity_attach_body(entity, body);
        rt_game3d_world_spawn(stream->world, entity);
        game3d_assign_typed_ref(&tile->collider_entity, entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    }

    game3d_release_ref(&entity);
    game3d_release_ref(&body);
    game3d_release_ref(&collider);
}

/// @brief Build the unique nav-source name for a terrain tile (used to register/find its nav
/// geometry).
/// @param tile Borrowed terrain record supplying the base name.
/// @return New runtime string containing the derived name, or a fallback constant reference.
static rt_string game3d_terrain_nav_source_name(const rt_game3d_stream_terrain_tile *tile) {
    const char *base = tile && tile->name ? rt_string_cstr(tile->name) : "terrain_tile";
    const char *suffix = "_navmesh_source";
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffix);
    if (base_len > SIZE_MAX - suffix_len - 1u)
        return rt_const_cstr("terrain_tile_navmesh_source");
    char *name = (char *)malloc(base_len + suffix_len + 1u);
    if (!name)
        return rt_const_cstr("terrain_tile_navmesh_source");
    memcpy(name, base, base_len);
    memcpy(name + base_len, suffix, suffix_len + 1u);
    rt_string result = rt_string_from_bytes(name, base_len + suffix_len);
    free(name);
    return result;
}

/// @brief Register a loaded terrain tile's geometry as a navmesh source for pathfinding.
/// @param stream Borrowed owning stream with a live world and scene.
/// @param tile Terrain record receiving the retained hidden nav-source entity.
static void game3d_world_stream_attach_terrain_nav_source(rt_game3d_world_stream *stream,
                                                          rt_game3d_stream_terrain_tile *tile) {
    if (!stream || !tile)
        return;
    void *terrain = game3d_stream_terrain_tile_terrain_ref(tile);
    if (!terrain)
        return;
    if (tile->nav_entity) {
        if (game3d_stream_terrain_tile_entity_ref(tile->nav_entity))
            return;
        game3d_release_typed_ref(&tile->nav_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    }
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(stream->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (!world || world->destroyed || !world->scene)
        return;

    void *mesh = rt_terrain3d_build_nav_mesh(terrain, 4);
    if (!mesh)
        return;
    void *entity = rt_game3d_entity_new();
    if (entity) {
        rt_game3d_entity_set_mesh(entity, mesh);
        rt_string name = game3d_terrain_nav_source_name(tile);
        rt_game3d_entity_set_name(entity, name);
        rt_string_unref(name);
        double sx = game3d_stream_terrain_axis_scale(tile->scale[0]);
        double sz = game3d_stream_terrain_axis_scale(tile->scale[2]);
        double width = tile->width > 1 ? (double)(tile->width - 1) * sx : sx;
        double depth = tile->depth > 1 ? (double)(tile->depth - 1) * sz : sz;
        double cx = game3d_clamp_coord_or(tile->center[0], 0.0);
        double cy = game3d_clamp_coord_or(tile->center[1], 0.0);
        double cz = game3d_clamp_coord_or(tile->center[2], 0.0);
        double tx = game3d_clamp_coord_or(cx - width * 0.5, cx);
        double tz = game3d_clamp_coord_or(cz - depth * 0.5, cz);
        rt_game3d_entity_set_position(entity, tx, cy, tz);
        rt_game3d_entity *impl = (rt_game3d_entity *)entity;
        if (impl->node)
            rt_scene_node3d_set_visible(impl->node, 0);
        rt_game3d_world_spawn(stream->world, entity);
        game3d_assign_typed_ref(&tile->nav_entity, entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    }
    game3d_release_ref(&entity);
    game3d_release_ref(&mesh);
}

/// @brief Recreate collider/nav sources so they reflect the terrain's post-stitch height grid.
/// @param stream Borrowed owning stream.
/// @param tile Resident terrain record whose derived spatial sources are rebuilt.
static void game3d_world_stream_refresh_terrain_spatial_sources(
    rt_game3d_world_stream *stream, rt_game3d_stream_terrain_tile *tile) {
    void *terrain = game3d_stream_terrain_tile_terrain_ref(tile);
    if (!stream || !terrain)
        return;
    game3d_world_stream_detach_terrain_spatial_sources(stream, tile);
    void *heightmap = rt_terrain3d_build_heightmap_pixels(terrain);
    if (heightmap) {
        game3d_world_stream_attach_terrain_collider(stream, tile, heightmap);
        game3d_release_ref(&heightmap);
    }
    game3d_world_stream_attach_terrain_nav_source(stream, tile);
}

/// @brief Stitch a pair of resident terrain tiles when their full manifest edges touch.
/// @param a First resident terrain record.
/// @param b Second resident terrain record.
/// @return Number of edge samples changed, or zero when the tiles do not share a full edge.
static int64_t game3d_world_stream_stitch_terrain_pair(rt_game3d_stream_terrain_tile *a,
                                                       rt_game3d_stream_terrain_tile *b) {
    void *terrain_a = game3d_stream_terrain_tile_terrain_ref(a);
    void *terrain_b = game3d_stream_terrain_tile_terrain_ref(b);
    if (!terrain_a || !terrain_b)
        return 0;
    double a_min_x, a_max_x, a_min_z, a_max_z;
    double b_min_x, b_max_x, b_min_z, b_max_z;
    game3d_stream_terrain_tile_bounds(a, &a_min_x, &a_max_x, &a_min_z, &a_max_z);
    game3d_stream_terrain_tile_bounds(b, &b_min_x, &b_max_x, &b_min_z, &b_max_z);
    double tolerance = game3d_stream_terrain_seam_tolerance(a, b);
    if (game3d_stream_range_matches(a_min_z, a_max_z, b_min_z, b_max_z, tolerance)) {
        if (game3d_stream_almost_equal(a_max_x, b_min_x, tolerance)) {
            return rt_terrain3d_stitch_edge(
                terrain_a, RT_TERRAIN3D_EDGE_EAST, terrain_b, RT_TERRAIN3D_EDGE_WEST);
        }
        if (game3d_stream_almost_equal(b_max_x, a_min_x, tolerance)) {
            return rt_terrain3d_stitch_edge(
                terrain_a, RT_TERRAIN3D_EDGE_WEST, terrain_b, RT_TERRAIN3D_EDGE_EAST);
        }
    }
    if (game3d_stream_range_matches(a_min_x, a_max_x, b_min_x, b_max_x, tolerance)) {
        if (game3d_stream_almost_equal(a_max_z, b_min_z, tolerance)) {
            return rt_terrain3d_stitch_edge(
                terrain_a, RT_TERRAIN3D_EDGE_SOUTH, terrain_b, RT_TERRAIN3D_EDGE_NORTH);
        }
        if (game3d_stream_almost_equal(b_max_z, a_min_z, tolerance)) {
            return rt_terrain3d_stitch_edge(
                terrain_a, RT_TERRAIN3D_EDGE_NORTH, terrain_b, RT_TERRAIN3D_EDGE_SOUTH);
        }
    }
    return 0;
}

/// @brief Stitch @p tile against all currently resident terrain neighbors.
/// @param stream Borrowed stream containing candidate neighbors.
/// @param tile Newly resident terrain record.
/// @return Saturated total number of edge samples changed.
static int64_t game3d_world_stream_stitch_loaded_terrain_neighbors(
    rt_game3d_world_stream *stream, rt_game3d_stream_terrain_tile *tile) {
    if (!stream || !game3d_stream_terrain_tile_terrain_ref(tile))
        return 0;
    int64_t changed = 0;
    int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
    for (int32_t i = 0; i < tile_count; ++i) {
        rt_game3d_stream_terrain_tile *neighbor = &stream->terrain_tiles[i];
        if (neighbor == tile)
            continue;
        int64_t delta = game3d_world_stream_stitch_terrain_pair(tile, neighbor);
        if (delta > 0) {
            if (changed > INT64_MAX - delta)
                changed = INT64_MAX;
            else
                changed += delta;
        }
    }
    return changed;
}

/// @brief Refresh all resident streamed terrain collider/nav sources after seam changes.
/// @param stream Borrowed stream whose resident terrain attachments are rebuilt.
static void game3d_world_stream_refresh_all_terrain_spatial_sources(
    rt_game3d_world_stream *stream) {
    if (!stream)
        return;
    int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
    for (int32_t i = 0; i < tile_count; ++i) {
        rt_game3d_stream_terrain_tile *tile = &stream->terrain_tiles[i];
        if (game3d_stream_terrain_tile_terrain_ref(tile))
            game3d_world_stream_refresh_terrain_spatial_sources(stream, tile);
    }
}

/// @brief Build and install a tile's Terrain3D from a prepared heightmap (shared core).
/// @details Consumes nothing; @p heightmap is borrowed (may be NULL for a blank tile).
/// @param stream Borrowed owning stream.
/// @param tile Terrain record receiving the newly allocated Terrain3D.
/// @param heightmap Borrowed Pixels heightmap, or `NULL` to allocate a blank grid.
///   Sidecar handling stays with the caller so staged bytes avoid main-thread file IO.
/// @return 1 when the tile became resident.
static int game3d_world_stream_install_terrain_tile(rt_game3d_world_stream *stream,
                                                    rt_game3d_stream_terrain_tile *tile,
                                                    void *heightmap) {
    int64_t width = tile->width;
    int64_t depth = tile->depth;
    if (width < 2)
        width = 2;
    if (depth < 2)
        depth = 2;
    if (width > 4096)
        width = 4096;
    if (depth > 4096)
        depth = 4096;
    void *terrain = rt_terrain3d_new(width, depth);
    if (!terrain)
        return 0;
    void *blank = NULL;
    if (!heightmap) {
        blank = rt_pixels_new(width, depth);
        heightmap = blank;
    }
    if (heightmap)
        rt_terrain3d_set_heightmap(terrain, heightmap);
    rt_terrain3d_set_scale(terrain,
                           game3d_stream_terrain_axis_scale(tile->scale[0]),
                           game3d_stream_terrain_axis_scale(tile->scale[1]),
                           game3d_stream_terrain_axis_scale(tile->scale[2]));
    void *material = rt_material3d_new_color(0.20, 0.34, 0.22);
    if (material) {
        rt_material3d_set_shading_model(material, RT_GAME3D_SHADING_PBR);
        rt_material3d_set_metallic(material, 0.0);
        rt_material3d_set_roughness(material, 0.88);
        rt_material3d_set_ao(material, 1.0);
        rt_terrain3d_set_material(terrain, material);
        game3d_release_ref(&material);
    }
    tile->terrain = terrain;
    tile->resident = 1;
    for (int32_t hi = 0; hi < tile->manifest_hole_count; ++hi)
        (void)rt_terrain3d_set_hole(terrain,
                                    tile->holes[hi][0],
                                    tile->holes[hi][1],
                                    tile->holes[hi][2],
                                    tile->holes[hi][3]);
    game3d_release_ref(&blank);
    return 1;
}

/// @brief Stitch and re-register a freshly resident tile's spatial sources (shared tail).
/// @param stream Borrowed owning stream.
/// @param tile Newly resident terrain record.
static void game3d_world_stream_finish_terrain_install(rt_game3d_world_stream *stream,
                                                       rt_game3d_stream_terrain_tile *tile) {
    int64_t stitched = game3d_world_stream_stitch_loaded_terrain_neighbors(stream, tile);
    if (stitched > 0)
        game3d_world_stream_refresh_all_terrain_spatial_sources(stream);
    else
        game3d_world_stream_refresh_terrain_spatial_sources(stream, tile);
}

/// @brief Load a terrain tile synchronously from its heightmap and sidecar paths.
/// @param stream Borrowed owning stream.
/// @param tile Terrain record to load.
/// @return Nonzero when already or newly resident; zero on load or allocation failure.
static int game3d_world_stream_load_terrain_tile(rt_game3d_world_stream *stream,
                                                 rt_game3d_stream_terrain_tile *tile) {
    if (!tile)
        return 0;
    if (game3d_stream_terrain_tile_terrain_ref(tile))
        return 1;
    game3d_release_typed_ref(&tile->terrain, RT_G3D_TERRAIN3D_CLASS_ID);
    if (tile->collider_entity || tile->nav_entity)
        game3d_world_stream_unload_terrain_tile(stream, tile);
    int64_t width = tile->width < 2 ? 2 : (tile->width > 4096 ? 4096 : tile->width);
    int64_t depth = tile->depth < 2 ? 2 : (tile->depth > 4096 ? 4096 : tile->depth);
    void *heightmap = game3d_load_heightmap_sidecar(tile->heightmap_path, width, depth);
    if (!game3d_world_stream_install_terrain_tile(stream, tile, heightmap)) {
        game3d_release_ref(&heightmap);
        return 0;
    }
    game3d_stream_terrain_tile_load_sidecar(tile);
    game3d_world_stream_finish_terrain_install(stream, tile);
    game3d_release_ref(&heightmap);
    return 1;
}

/// @brief Commit a worker-staged terrain tile: build the heightmap from staged POD
///   samples and install it with no main-thread file IO.
/// @param stream Borrowed owning stream.
/// @param tile Terrain record containing staged samples and sidecar bytes.
/// @return 1 when the tile became resident.
static int game3d_world_stream_commit_staged_terrain_tile(rt_game3d_world_stream *stream,
                                                          rt_game3d_stream_terrain_tile *tile) {
    if (!stream || !tile || !tile->staged)
        return 0;
    if (game3d_stream_terrain_tile_terrain_ref(tile)) {
        game3d_stream_terrain_tile_drop_staged(tile, 1);
        return 1;
    }
    game3d_release_typed_ref(&tile->terrain, RT_G3D_TERRAIN3D_CLASS_ID);
    if (tile->collider_entity || tile->nav_entity)
        game3d_world_stream_unload_terrain_tile(stream, tile);
    int64_t width = tile->width < 2 ? 2 : (tile->width > 4096 ? 4096 : tile->width);
    int64_t depth = tile->depth < 2 ? 2 : (tile->depth > 4096 ? 4096 : tile->depth);
    void *heightmap = NULL;
    if (tile->staged_heights)
        heightmap = game3d_heightmap_pixels_from_samples(
            tile->staged_heights, tile->staged_hm_width, tile->staged_hm_depth, width, depth);
    if (!game3d_world_stream_install_terrain_tile(stream, tile, heightmap)) {
        game3d_release_ref(&heightmap);
        game3d_stream_terrain_tile_drop_staged(tile, 0);
        tile->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
        rt_game3d_diag_record_stream_staging_error();
        return 0;
    }
    /* Adopt staged sidecar bytes directly. */
    free(tile->sidecar_data);
    tile->sidecar_data = tile->staged_sidecar;
    tile->sidecar_bytes =
        (int64_t)(tile->staged_sidecar_len > (size_t)INT64_MAX ? (size_t)INT64_MAX
                                                               : tile->staged_sidecar_len);
    tile->staged_sidecar = NULL;
    tile->staged_sidecar_len = 0;
    game3d_world_stream_finish_terrain_install(stream, tile);
    game3d_release_ref(&heightmap);
    game3d_stream_terrain_tile_drop_staged(tile, 0);
    return 1;
}

/// @brief Load a streaming cell's optional binary sidecar payload into residency.
/// @details Reads the whole referenced file as opaque cell metadata. A missing, empty,
///   or oversized sidecar is recoverable: the cell simply carries zero sidecar bytes
///   (no trap). Frees any previously loaded payload first so reloads stay idempotent.
/// @param cell Cell record whose owned sidecar buffer is replaced.
static void game3d_stream_cell_load_sidecar(rt_game3d_stream_cell *cell) {
    if (!cell)
        return;
    free(cell->sidecar_data);
    cell->sidecar_data = NULL;
    cell->sidecar_bytes = 0;
    if (!game3d_string_has_bytes(cell->sidecar_path))
        return;
    size_t size = 0;
    uint8_t *data = game3d_asset_read_file_bytes(rt_string_cstr(cell->sidecar_path), &size);
    if (!data)
        return;
    if (size == 0) {
        free(data);
        return;
    }
    cell->sidecar_data = data;
    cell->sidecar_bytes = (int64_t)(size > (size_t)INT64_MAX ? (size_t)INT64_MAX : size);
}

/// @brief Unload a streaming cell: despawn its spawned entities/nodes and release its scene.
/// @param stream Borrowed owning stream, or `NULL` during detached cleanup.
/// @param cell Cell record to reset.
static void game3d_world_stream_unload_cell(rt_game3d_world_stream *stream,
                                            rt_game3d_stream_cell *cell) {
    if (!cell)
        return;
    if (cell->resident && cell->entity && stream && stream->world) {
        rt_game3d_entity *entity =
            (rt_game3d_entity *)rt_g3d_checked_or_null(cell->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        if (entity && entity->alive && entity->spawned && entity->world == stream->world)
            game3d_world_despawn_entity_tree((rt_game3d_world *)stream->world, entity, 1);
    }
    game3d_release_ref(&cell->entity);
    game3d_release_ref(&cell->scene);
    free(cell->sidecar_data);
    cell->sidecar_data = NULL;
    cell->sidecar_bytes = 0;
    cell->resident = 0;
    cell->measured_resident_bytes = 0;
}

/// @brief Fold a loaded sidecar payload into a resident cell's measured residency bytes.
/// @param cell Resident cell record whose measured byte count is updated.
static void game3d_stream_cell_add_sidecar_bytes(rt_game3d_stream_cell *cell) {
    if (!cell || cell->sidecar_bytes <= 0)
        return;
    cell->measured_resident_bytes = game3d_i64_from_u64_saturating(game3d_u64_saturating_add(
        (uint64_t)cell->measured_resident_bytes, (uint64_t)cell->sidecar_bytes));
}

/// @brief Spawn a loaded cell scene into the world (shared by blocking and staged loads).
/// @details Consumes @p scene (retained into the cell on success, released on failure).
/// @param stream Borrowed owning stream.
/// @param cell Cell record receiving the resident scene and root entity.
/// @param scene Owned Scene3D reference consumed on every path.
/// @return 1 when the cell became resident.
static int game3d_world_stream_adopt_cell_scene(rt_game3d_world_stream *stream,
                                                rt_game3d_stream_cell *cell,
                                                void *scene) {
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(stream->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (!world || world->destroyed || !scene) {
        game3d_release_ref(&scene);
        return 0;
    }
    void *root = rt_scene3d_get_root(scene);
    void *entity = rt_game3d_entity_from_node(root);
    if (entity && cell->name)
        rt_game3d_entity_set_name(entity, cell->name);
    if (entity) {
        if (cell->has_layer)
            rt_game3d_entity_set_layer(entity, cell->layer);
        if (cell->has_collision_mask) {
            void *mask = game3d_layermask_new_bits(cell->collision_mask);
            rt_game3d_entity_set_collision_mask(entity, mask);
            game3d_release_ref(&mask);
        }
        rt_game3d_entity_set_position(entity,
                                      game3d_clamp_coord_or(cell->center[0], 0.0),
                                      game3d_clamp_coord_or(cell->center[1], 0.0),
                                      game3d_clamp_coord_or(cell->center[2], 0.0));
        int64_t next_id = world->next_entity_id;
        if (game3d_world_spawn_entity_tree(world, (rt_game3d_entity *)entity, 1, &next_id)) {
            world->next_entity_id = next_id;
            cell->scene = scene;
            cell->entity = entity;
            cell->resident = 1;
            game3d_stream_push_loaded_event(stream, cell->name);
            uint64_t measured = game3d_entity_estimate_resident_bytes(entity);
            if (measured == 0)
                measured = game3d_scene_estimate_resident_bytes(scene);
            cell->measured_resident_bytes = game3d_i64_from_u64_saturating(measured);
            if (cell->measured_resident_bytes <= 0)
                cell->measured_resident_bytes =
                    cell->resident_bytes > 0 ? cell->resident_bytes : 64 * 1024;
            scene = NULL;
        } else {
            game3d_release_ref(&entity);
        }
    }
    game3d_release_ref(&scene);
    return cell->resident ? 1 : 0;
}

/// @brief Load a streaming cell: load its scene/model and spawn its content into the world.
/// @param stream Borrowed owning stream.
/// @param cell Cell record to load synchronously.
/// @return 1 on success, 0 on load failure.
static int game3d_world_stream_load_cell(rt_game3d_world_stream *stream,
                                         rt_game3d_stream_cell *cell) {
    if (!stream || !cell)
        return 0;
    if (cell->resident)
        return 1;
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(stream->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (!world || world->destroyed || !cell->path)
        return 0;
    void *scene = rt_scene3d_load(cell->path);
    if (!scene)
        return 0;
    if (!game3d_world_stream_adopt_cell_scene(stream, cell, scene))
        return 0;
    game3d_stream_cell_load_sidecar(cell);
    game3d_stream_cell_add_sidecar_bytes(cell);
    return 1;
}

/// @brief Commit a worker-staged cell payload: parse the staged VSCN text and spawn it.
/// @details Consumes the staged payload on every path. A parse failure is recoverable —
///   the cell is skipped with a reload cooldown and a StreamStagingErrors diagnostic.
/// @param stream Borrowed owning stream.
/// @param cell Cell record containing staged scene text and sidecar bytes.
/// @return 1 when the cell became resident.
static int game3d_world_stream_commit_staged_cell(rt_game3d_world_stream *stream,
                                                  rt_game3d_stream_cell *cell) {
    if (!stream || !cell || !cell->staged)
        return 0;
    if (cell->resident) {
        game3d_stream_cell_drop_staged(cell, 1);
        return 1;
    }
    void *scene = rt_scene3d_load_from_memory(cell->path, cell->staged_text, cell->staged_text_len);
    if (!scene || !game3d_world_stream_adopt_cell_scene(stream, cell, scene)) {
        game3d_stream_cell_drop_staged(cell, 0);
        cell->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
        rt_game3d_diag_record_stream_staging_error();
        return 0;
    }
    /* Adopt the staged sidecar bytes directly — no main-thread file IO. */
    free(cell->sidecar_data);
    cell->sidecar_data = cell->staged_sidecar;
    cell->sidecar_bytes =
        (int64_t)(cell->staged_sidecar_len > (size_t)INT64_MAX ? (size_t)INT64_MAX
                                                               : cell->staged_sidecar_len);
    cell->staged_sidecar = NULL;
    cell->staged_sidecar_len = 0;
    game3d_stream_cell_add_sidecar_bytes(cell);
    game3d_stream_cell_drop_staged(cell, 0);
    return 1;
}

/// @brief Remeasure a loaded cell's entity hierarchy after mesh/texture residency changes.
/// @details Loaded Scene3D roots are transactionally transferred into the spawned entity, so the
///   entity is authoritative. The retained source scene is consulted only as a defensive fallback.
/// @param cell Resident cell record whose byte telemetry is refreshed.
static void game3d_world_stream_refresh_cell_residency_bytes(rt_game3d_stream_cell *cell) {
    uint64_t measured;
    if (!cell || !cell->resident || !cell->entity)
        return;
    measured = game3d_entity_estimate_resident_bytes(cell->entity);
    if (measured == 0)
        measured = game3d_scene_estimate_resident_bytes(cell->scene);
    cell->measured_resident_bytes = game3d_i64_from_u64_saturating(measured);
    if (cell->measured_resident_bytes <= 0)
        cell->measured_resident_bytes = cell->resident_bytes > 0 ? cell->resident_bytes : 64 * 1024;
    if (cell->sidecar_bytes > 0)
        cell->measured_resident_bytes = game3d_i64_from_u64_saturating(game3d_u64_saturating_add(
            (uint64_t)cell->measured_resident_bytes, (uint64_t)cell->sidecar_bytes));
}

/// @brief Unload and free every streaming cell tracked by the stream.
/// @param stream Borrowed stream payload to clear.
static void game3d_world_stream_clear_cells(rt_game3d_world_stream *stream) {
    if (!stream)
        return;
    int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
    stream->cell_generation++; /* in-flight staging results for these cells are now stale */
    for (int32_t i = 0; i < cell_count; ++i) {
        rt_game3d_stream_cell *cell = &stream->cells[i];
        game3d_world_stream_unload_cell(stream, cell);
        game3d_world_stream_unload_cell_proxy(stream, cell);
        game3d_stream_cell_drop_staged(cell, cell->staged ? 1 : 0);
        game3d_stream_cell_drop_staged_proxy(cell, cell->proxy_staged ? 1 : 0);
        game3d_release_ref((void **)&cell->name);
        game3d_release_ref((void **)&cell->proxy_path);
        game3d_release_ref((void **)&cell->path);
        game3d_release_ref((void **)&cell->material);
        game3d_release_ref((void **)&cell->nav_area);
        game3d_release_ref((void **)&cell->sidecar_path);
    }
    free(stream->cells);
    stream->cells = NULL;
    stream->cell_count = 0;
    stream->cell_capacity = 0;
    stream->cells_manifest_loaded = 0;
}

/// @brief Parse the cells manifest (JSON) into the stream's cell table (name, center, path per
/// cell).
/// @param stream Borrowed stream whose retained manifest is parsed and whose prior table is
/// replaced.
/// @return 1 on success, 0 on a missing or malformed manifest.
static int game3d_world_stream_parse_cells_manifest(rt_game3d_world_stream *stream) {
    if (!stream || !game3d_string_has_bytes(stream->cells_manifest))
        return 0;
    rt_string text = game3d_read_text_file_no_trap(stream->cells_manifest);
    if (!text)
        return 0;
    void *root = NULL;
    if (!rt_json_try_parse(text, &root, NULL, NULL, NULL) || !game3d_json_is_map(root)) {
        rt_string_unref(text);
        game3d_release_ref(&root);
        return 0;
    }
    void *cells = game3d_json_get(root, "cells");
    if (!game3d_json_is_seq(cells)) {
        rt_string_unref(text);
        game3d_release_ref(&root);
        return 0;
    }
    int64_t count64 = rt_seq_len(cells);
    int64_t manifest_len = rt_str_len(text);
    size_t manifest_bytes = manifest_len > 0 ? (size_t)manifest_len : 0u;
    if (count64 < 0 || count64 > INT32_MAX || !rt_untrusted_count_ok(count64, 1u, manifest_bytes) ||
        !game3d_stream_declared_count_ok(root, "cellCount", manifest_bytes)) {
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                           "Game3D.WorldStream3D: cells manifest declares too many cells");
        rt_string_unref(text);
        game3d_release_ref(&root);
        return 0;
    }
    rt_game3d_stream_cell *parsed = NULL;
    int32_t parsed_count = 0;
    if (count64 > 0) {
        parsed = (rt_game3d_stream_cell *)calloc((size_t)count64, sizeof(*parsed));
        if (!parsed) {
            rt_string_unref(text);
            game3d_release_ref(&root);
            return 0;
        }
    }
    for (int64_t i = 0; i < count64; ++i) {
        void *entry = rt_seq_get(cells, i);
        if (!game3d_json_is_map(entry))
            continue;
        rt_string raw_path = game3d_json_string_ref(entry, "path");
        if (!raw_path)
            continue;
        rt_game3d_stream_cell *cell = &parsed[parsed_count++];
        cell->path = game3d_stream_resolve_manifest_path(stream->cells_manifest, raw_path);
        rt_string_unref(raw_path);
        cell->name = game3d_json_string_ref(entry, "name");
        cell->material = game3d_json_string_ref(entry, "material");
        cell->nav_area = game3d_json_string_ref(entry, "navArea");
        cell->sidecar_path = game3d_json_resolved_sidecar_ref(entry, stream->cells_manifest);
        game3d_stream_parse_collision_metadata(entry,
                                               0,
                                               &cell->layer,
                                               &cell->has_layer,
                                               &cell->collision_mask,
                                               &cell->has_collision_mask,
                                               &cell->collision_enabled);
        cell->traversal_cost = game3d_stream_traversal_cost_or_default(entry);
        double zero[3] = {0.0, 0.0, 0.0};
        (void)game3d_json_vec3_or(entry, "center", cell->center, zero);
        cell->radius = game3d_json_f64_or(entry, "radius", 0.0);
        if (!isfinite(cell->radius) || cell->radius < 0.0)
            cell->radius = 0.0;
        cell->resident_bytes = game3d_json_i64_or(entry, "bytes", 64 * 1024);
        if (cell->resident_bytes < 0)
            cell->resident_bytes = 0;
        {
            rt_string raw_proxy = game3d_json_string_ref(entry, "proxy");
            if (raw_proxy) {
                cell->proxy_path =
                    game3d_stream_resolve_manifest_path(stream->cells_manifest, raw_proxy);
                rt_string_unref(raw_proxy);
            }
        }
        cell->proxy_bytes = game3d_json_i64_or(entry, "proxyBytes", 16 * 1024);
        if (cell->proxy_bytes < 0)
            cell->proxy_bytes = 0;
    }
    game3d_world_stream_clear_cells(stream);
    stream->cells = parsed;
    stream->cell_count = parsed_count;
    stream->cell_capacity = (int32_t)count64;
    stream->cells_manifest_loaded = 1;
    rt_string_unref(text);
    game3d_release_ref(&root);
    return 1;
}

/// @brief Parse the terrain manifest (JSON) into the stream's terrain-tile table.
/// @param stream Borrowed stream whose retained manifest is parsed and whose prior table is
/// replaced.
/// @return 1 on success, 0 on a missing or malformed manifest.
static int game3d_world_stream_parse_terrain_manifest(rt_game3d_world_stream *stream) {
    if (!stream || !game3d_string_has_bytes(stream->terrain_manifest))
        return 0;
    rt_string text = game3d_read_text_file_no_trap(stream->terrain_manifest);
    if (!text)
        return 0;
    void *root = NULL;
    if (!rt_json_try_parse(text, &root, NULL, NULL, NULL) || !game3d_json_is_map(root)) {
        rt_string_unref(text);
        game3d_release_ref(&root);
        return 0;
    }
    void *tiles = game3d_json_get(root, "tiles");
    if (!game3d_json_is_seq(tiles)) {
        rt_string_unref(text);
        game3d_release_ref(&root);
        return 0;
    }
    int64_t count64 = rt_seq_len(tiles);
    int64_t manifest_len = rt_str_len(text);
    size_t manifest_bytes = manifest_len > 0 ? (size_t)manifest_len : 0u;
    if (count64 < 0 || count64 > INT32_MAX || !rt_untrusted_count_ok(count64, 1u, manifest_bytes) ||
        !game3d_stream_declared_count_ok(root, "tileCount", manifest_bytes) ||
        !game3d_stream_declared_count_ok(root, "terrainTileCount", manifest_bytes)) {
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                           "Game3D.WorldStream3D: terrain manifest declares too many tiles");
        rt_string_unref(text);
        game3d_release_ref(&root);
        return 0;
    }
    rt_game3d_stream_terrain_tile *parsed = NULL;
    int32_t parsed_count = 0;
    if (count64 > 0) {
        parsed = (rt_game3d_stream_terrain_tile *)calloc((size_t)count64, sizeof(*parsed));
        if (!parsed) {
            rt_string_unref(text);
            game3d_release_ref(&root);
            return 0;
        }
    }
    for (int64_t i = 0; i < count64; ++i) {
        void *entry = rt_seq_get(tiles, i);
        if (!game3d_json_is_map(entry))
            continue;
        rt_string raw_path = game3d_json_string_ref(entry, "path");
        if (!raw_path)
            continue;
        rt_game3d_stream_terrain_tile *tile = &parsed[parsed_count++];
        tile->path = game3d_stream_resolve_manifest_path(stream->terrain_manifest, raw_path);
        rt_string_unref(raw_path);
        rt_string raw_heightmap = game3d_json_string_ref(entry, "heightmap");
        tile->heightmap_path =
            raw_heightmap
                ? game3d_stream_resolve_manifest_path(stream->terrain_manifest, raw_heightmap)
                : rt_string_ref(rt_const_cstr(""));
        rt_string_unref(raw_heightmap);
        tile->name = game3d_json_string_ref(entry, "name");
        tile->material = game3d_json_string_ref(entry, "material");
        tile->nav_area = game3d_json_string_ref(entry, "navArea");
        tile->sidecar_path = game3d_json_resolved_sidecar_ref(entry, stream->terrain_manifest);
        game3d_stream_parse_collision_metadata(entry,
                                               RT_GAME3D_LAYER_WORLD,
                                               &tile->layer,
                                               &tile->has_layer,
                                               &tile->collision_mask,
                                               &tile->has_collision_mask,
                                               &tile->collision_enabled);
        tile->traversal_cost = game3d_stream_traversal_cost_or_default(entry);
        double zero[3] = {0.0, 0.0, 0.0};
        (void)game3d_json_vec3_or(entry, "center", tile->center, zero);
        double unit[3] = {1.0, 1.0, 1.0};
        (void)game3d_json_vec3_or(entry, "scale", tile->scale, unit);
        {
            void *holes_seq = game3d_json_get(entry, "holes");
            if (game3d_json_is_seq(holes_seq)) {
                int64_t hole_len = rt_seq_len(holes_seq);
                if (hole_len > 0 && hole_len <= 256) {
                    tile->holes = (double (*)[4])calloc((size_t)hole_len, sizeof(*tile->holes));
                    if (tile->holes) {
                        for (int64_t hi = 0; hi < hole_len; ++hi) {
                            void *rect = rt_seq_get(holes_seq, hi);
                            if (!game3d_json_is_seq(rect) || rt_seq_len(rect) < 4)
                                continue;
                            double hv[4];
                            int valid = 1;
                            for (int hc = 0; hc < 4; ++hc) {
                                if (!game3d_json_number(rt_seq_get(rect, hc), &hv[hc]) ||
                                    !isfinite(hv[hc]))
                                    valid = 0;
                            }
                            if (!valid || hv[2] <= 0.0 || hv[3] <= 0.0)
                                continue;
                            tile->holes[tile->manifest_hole_count][0] = hv[0];
                            tile->holes[tile->manifest_hole_count][1] = hv[1];
                            tile->holes[tile->manifest_hole_count][2] = hv[2];
                            tile->holes[tile->manifest_hole_count][3] = hv[3];
                            tile->manifest_hole_count++;
                        }
                    }
                }
            }
        }
        tile->width = game3d_json_i64_or(entry, "width", 2);
        tile->depth = game3d_json_i64_or(entry, "depth", tile->width);
        if (tile->width < 2)
            tile->width = 2;
        if (tile->depth < 2)
            tile->depth = 2;
        if (tile->width > 4096)
            tile->width = 4096;
        if (tile->depth > 4096)
            tile->depth = 4096;
        tile->radius = game3d_json_f64_or(entry, "radius", 0.0);
        if (!isfinite(tile->radius) || tile->radius < 0.0)
            tile->radius = 0.0;
        tile->resident_bytes = game3d_json_i64_or(entry, "bytes", 256 * 1024);
        if (tile->resident_bytes < 0)
            tile->resident_bytes = 0;
    }
    game3d_world_stream_clear_terrain_tiles(stream);
    stream->terrain_tiles = parsed;
    stream->terrain_tile_count = parsed_count;
    stream->terrain_tile_capacity = (int32_t)count64;
    stream->terrain_manifest_loaded = 1;
    rt_string_unref(text);
    game3d_release_ref(&root);
    return 1;
}

/// @brief GC finalizer for WorldStream3D: release world and manifest references.
/// @param obj Finalized WorldStream3D payload; `NULL` is ignored.
static void game3d_world_stream_finalize(void *obj) {
    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)obj;
    if (!stream)
        return;
    game3d_world_stream_clear_cells(stream);
    game3d_world_stream_clear_terrain_tiles(stream);
    if (stream->retains_world)
        game3d_release_ref(&stream->world);
    else
        stream->world = NULL;
    game3d_release_ref((void **)&stream->terrain_manifest);
    game3d_release_ref((void **)&stream->cells_manifest);
    free(stream->cell_candidates);
    stream->cell_candidates = NULL;
    stream->cell_candidate_capacity = 0;
    free(stream->terrain_candidates);
    stream->terrain_candidates = NULL;
    stream->terrain_candidate_capacity = 0;
    game3d_stream_persistence_release(stream);
}

/// @brief Convert a world-space load/unload radius to a cell-count radius (ceil of radius /
/// cell_size).
/// @param radius World-space stream radius.
/// @param cell_size Positive world-space cell size.
/// @return Odd-square slot estimate, bounded to `INT64_MAX`.
static int64_t game3d_stream_radius_slots(double radius, double cell_size) {
    if (!isfinite(radius) || radius <= 0.0)
        return 1;
    double slots = ceil(radius / cell_size);
    if (slots < 0.0)
        slots = 0.0;
    if (slots > 32767.0)
        slots = 32767.0;
    int64_t r = (int64_t)slots;
    int64_t side = r * 2 + 1;
    if (side > INT64_MAX / side)
        return INT64_MAX;
    return side * side;
}

/// @brief Case-insensitive ASCII equality for byte-size suffixes.
///
/// Manifest budget strings are ASCII configuration values, so this helper keeps
/// parsing independent of the process locale and avoids treating non-ASCII
/// bytes as meaningful suffix characters.
///
/// @param text Candidate suffix text.
/// @param len  Candidate suffix length in bytes.
/// @param lit  NUL-terminated literal to compare against.
/// @return Non-zero when @p text equals @p lit ignoring ASCII case.
static int game3d_ascii_ieq_len(const char *text, size_t len, const char *lit) {
    size_t lit_len;
    if (!text || !lit)
        return 0;
    lit_len = strlen(lit);
    if (len != lit_len)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char a = (unsigned char)text[i];
        unsigned char b = (unsigned char)lit[i];
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return 0;
    }
    return 1;
}

/// @brief Parse a byte-size literal with optional SI/IEC unit suffix.
///
/// Accepted examples include `1024`, `1024B`, `8 KB`, `8KiB`, `2MB`, and
/// `1 GiB`. Invalid text returns -1 so callers can fall back to non-literal
/// accounting paths such as manifest string storage.
///
/// @param text NUL-terminated candidate byte-size text.
/// @return Parsed byte count clamped to `INT64_MAX`, or -1 when not a literal.
static int64_t game3d_parse_byte_size_literal(const char *text) {
    const char *p = text;
    const char *suffix_start;
    const char *suffix_end;
    uint64_t value = 0;
    uint64_t multiplier = 1;
    int saw_digit = 0;
    if (!p)
        return -1;
    while (*p && isspace((unsigned char)*p))
        p++;
    while (*p && isdigit((unsigned char)*p)) {
        saw_digit = 1;
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (uint64_t)INT64_MAX / 10u ||
            (value == (uint64_t)INT64_MAX / 10u && digit > (uint64_t)INT64_MAX % 10u)) {
            value = (uint64_t)INT64_MAX;
            while (isdigit((unsigned char)*p))
                p++;
            break;
        }
        value = value * 10u + digit;
        p++;
    }
    if (!saw_digit)
        return -1;
    while (*p && isspace((unsigned char)*p))
        p++;
    suffix_start = p;
    while (*p && !isspace((unsigned char)*p))
        p++;
    suffix_end = p;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '\0')
        return -1;
    {
        size_t suffix_len = (size_t)(suffix_end - suffix_start);
        if (suffix_len == 0 || game3d_ascii_ieq_len(suffix_start, suffix_len, "b")) {
            multiplier = 1;
        } else if (game3d_ascii_ieq_len(suffix_start, suffix_len, "kb")) {
            multiplier = 1000u;
        } else if (game3d_ascii_ieq_len(suffix_start, suffix_len, "kib")) {
            multiplier = 1024u;
        } else if (game3d_ascii_ieq_len(suffix_start, suffix_len, "mb")) {
            multiplier = 1000u * 1000u;
        } else if (game3d_ascii_ieq_len(suffix_start, suffix_len, "mib")) {
            multiplier = 1024u * 1024u;
        } else if (game3d_ascii_ieq_len(suffix_start, suffix_len, "gb")) {
            multiplier = 1000u * 1000u * 1000u;
        } else if (game3d_ascii_ieq_len(suffix_start, suffix_len, "gib")) {
            multiplier = 1024u * 1024u * 1024u;
        } else {
            return -1;
        }
    }
    if (value > (uint64_t)INT64_MAX / multiplier)
        return INT64_MAX;
    return (int64_t)(value * multiplier);
}

/// @brief Measure a manifest reference or parse a byte-size literal.
///
/// Numeric manifest values are parsed as byte counts with optional units. Other
/// strings are treated as manifest path/reference storage and measured as their
/// NUL-terminated byte length to preserve existing residency accounting.
///
/// @param value Manifest reference/path or byte-size literal.
/// @return Byte count, clamped to `INT64_MAX`; 0 for NULL/empty strings.
static int64_t game3d_stream_manifest_bytes(rt_string value) {
    const char *bytes = value ? rt_string_cstr(value) : "";
    int64_t parsed;
    if (!bytes || bytes[0] == '\0')
        return 0;
    parsed = game3d_parse_byte_size_literal(bytes);
    if (parsed >= 0)
        return parsed;
    size_t len = strlen(bytes);
    if (len > (size_t)INT64_MAX - 1u)
        return INT64_MAX;
    return (int64_t)len + 1;
}

/// @brief Saturating signed 64-bit addition (clamps to INT64_MIN/MAX instead of overflowing).
/// @param a First addend.
/// @param b Second addend.
/// @return Saturated signed sum.
static int64_t game3d_i64_saturating_add(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Saturating signed 64-bit multiplication (clamps to INT64_MIN/MAX on overflow).
/// @param a First non-negative factor.
/// @param b Second non-negative factor.
/// @return Saturated product, or zero when either factor is non-positive.
static int64_t game3d_i64_saturating_mul(int64_t a, int64_t b) {
    if (a <= 0 || b <= 0)
        return 0;
    if (a > INT64_MAX / b)
        return INT64_MAX;
    return a * b;
}

/// @brief Compute this frame's streaming load budget (bytes) from the frame delta @p dt.
/// @details Caps how much may be streamed per frame so loading spikes don't stall the simulation.
/// @param dt Candidate frame delta in seconds.
/// @return Integral per-frame work allowance from one through eight.
static int64_t game3d_world_stream_update_load_budget(double dt) {
    if (!isfinite(dt) || dt <= 0.0)
        return 1;
    double budget = ceil(dt * 60.0);
    if (budget < 1.0)
        budget = 1.0;
    if (budget > 8.0)
        budget = 8.0;
    return (int64_t)budget;
}

/// @brief True when adding @p cost to @p current stays within @p budget without signed overflow.
/// @param current Non-negative cost already committed.
/// @param budget Total budget, or a negative value for unlimited.
/// @param cost Non-negative additional cost.
/// @return Nonzero when the addition fits without overflow; otherwise zero.
static int game3d_i64_budget_can_fit(int64_t current, int64_t budget, int64_t cost) {
    if (budget < 0)
        return 1;
    if (current < 0 || cost < 0 || current > budget)
        return 0;
    return cost <= budget - current;
}

/* ===== Worker-backed streaming staging (async cell/tile loads) ============ */
/* Workers do pure IO + POD parsing (never rt objects), mirroring the async-
 * asset architecture: results ride a process-global commit queue back to the
 * main thread, land on the owning stream's records, and commit inside the
 * deterministic nearest-first recompute pass so resident order matches
 * blocking mode at any worker count. Jobs retain their stream, so a stream
 * with staging in flight cannot finalize until its results drain. */

typedef struct rt_game3d_stream_stage_job {
    struct rt_game3d_stream_stage_job *failed_next; /* intrusive OOM fallback stack */
    void *stream;        /* retained WorldStream3D; released on the main thread */
    uint64_t generation; /* stream_generation snapshot at submit */
    int32_t kind;        /* 0 = scene cell, 1 = terrain tile */
    int32_t index;       /* manifest index at submit */
    char *primary_path;  /* .vscn path (cells) / heightmap path (tiles), or NULL */
    char *sidecar_path;  /* optional sidecar path, or NULL */
    char *text;          /* staged cell .vscn text (malloc-owned) */
    size_t text_len;
    double *heights; /* staged tile POD height grid (malloc-owned) */
    int64_t hm_width;
    int64_t hm_depth;
    uint8_t *sidecar; /* staged sidecar bytes (malloc-owned) */
    size_t sidecar_len;
    int8_t error; /* primary payload missing/corrupt (cells only) */
} rt_game3d_stream_stage_job;

/* Process-global staging result queue; drained only on the main thread. */
static void *g_game3d_stream_stage_queue;
/* If the commit queue cannot allocate its wrapper item, the already-allocated job itself becomes
 * a lock-free emergency-stack node. The next main-thread drain owns cleanup and releases the
 * runtime retain without ever finalizing a stream on a worker. */
static void *volatile g_game3d_stream_failed_stage_jobs;

/// @brief malloc-duplicate a C string (workers must not touch rt_string refcounts).
/// @param text Null-terminated source string, or `NULL`.
/// @return Caller-owned duplicate, or `NULL` for null input or allocation failure.
static char *game3d_stream_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    char *copy = (char *)malloc(len + 1);
    if (copy)
        memcpy(copy, text, len + 1);
    return copy;
}

/// @brief Release the worker-produced buffers owned by a staging job.
/// @param job Staging job whose text, height, and sidecar payloads are cleared.
static void game3d_stream_stage_job_free_payload(rt_game3d_stream_stage_job *job) {
    if (!job)
        return;
    free(job->text);
    job->text = NULL;
    job->text_len = 0;
    free(job->heights);
    job->heights = NULL;
    free(job->sidecar);
    job->sidecar = NULL;
    job->sidecar_len = 0;
}

/// @brief Free a job's paths and struct. Does NOT release the stream retain.
/// @param job Staging job to destroy; `NULL` is ignored.
static void game3d_stream_stage_job_free_shallow(rt_game3d_stream_stage_job *job) {
    if (!job)
        return;
    game3d_stream_stage_job_free_payload(job);
    free(job->primary_path);
    free(job->sidecar_path);
    free(job);
}

/// @brief Commit-queue cancel hook: runs only at process teardown when the queue dies
///   with items still pending; frees the payload and drops the stream retain.
/// @param user_data Owned staging-job pointer supplied to the commit queue.
static void game3d_stream_stage_cancel(void *user_data) {
    rt_game3d_stream_stage_job *job = (rt_game3d_stream_stage_job *)user_data;
    if (!job)
        return;
    game3d_release_ref(&job->stream);
    game3d_stream_stage_job_free_shallow(job);
}

/// @brief Publish a worker result to the allocation-free emergency stack.
/// @param job Owned staging job whose stream retain will be released by the main-thread drain.
static void game3d_stream_stage_publish_enqueue_failure(rt_game3d_stream_stage_job *job) {
    void *expected;
    if (!job)
        return;
    do {
        expected = rt_atomic_load_ptr(&g_game3d_stream_failed_stage_jobs, __ATOMIC_ACQUIRE);
        job->failed_next = (rt_game3d_stream_stage_job *)expected;
    } while (!rt_atomic_compare_exchange_ptr(
        &g_game3d_stream_failed_stage_jobs, &expected, job, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
}

/// @brief Drop a cell's staged payload without committing it (cancellation/unload path).
/// @param cell Cell record whose staged buffers and flags are cleared.
/// @param record_stale Nonzero to increment the stale-stage diagnostic when payload exists.
static void game3d_stream_cell_drop_staged(rt_game3d_stream_cell *cell, int record_stale) {
    if (!cell)
        return;
    if (cell->staged && record_stale)
        rt_game3d_diag_record_stream_stale_stage_dropped();
    free(cell->staged_text);
    cell->staged_text = NULL;
    cell->staged_text_len = 0;
    free(cell->staged_sidecar);
    cell->staged_sidecar = NULL;
    cell->staged_sidecar_len = 0;
    cell->staged = 0;
    cell->staged_error = 0;
    cell->prefetched = 0;
}

/// @brief Drop a terrain tile's staged payload without committing it.
/// @param tile Terrain record whose staged buffers and flags are cleared.
/// @param record_stale Nonzero to increment the stale-stage diagnostic when payload exists.
static void game3d_stream_terrain_tile_drop_staged(rt_game3d_stream_terrain_tile *tile,
                                                   int record_stale) {
    if (!tile)
        return;
    if (tile->staged && record_stale)
        rt_game3d_diag_record_stream_stale_stage_dropped();
    free(tile->staged_heights);
    tile->staged_heights = NULL;
    tile->staged_hm_width = 0;
    tile->staged_hm_depth = 0;
    free(tile->staged_sidecar);
    tile->staged_sidecar = NULL;
    tile->staged_sidecar_len = 0;
    tile->staged = 0;
    tile->staged_error = 0;
    tile->prefetched = 0;
}

/// @brief Main-thread drain callback: land a worker's staged payload on its stream record.
/// @details Stale results (remounted stream, out-of-range index) are freed and counted;
///   staging errors start the same reload cooldown budget evictions use so a corrupt
///   payload cannot hot-loop worker reads every recompute pass.
/// @param user_data Owned staging-job pointer delivered by the commit queue.
static void game3d_stream_stage_commit(void *user_data) {
    rt_game3d_stream_stage_job *job = (rt_game3d_stream_stage_job *)user_data;
    if (!job)
        return;
    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)rt_g3d_checked_or_null(
        job->stream, RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID);
    int stale = !stream || (job->kind == 1 ? stream->terrain_generation
                                           : stream->cell_generation) != job->generation;
    if (!stale && job->kind == 2) {
        int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
        if (job->index < 0 || job->index >= cell_count) {
            stale = 1;
        } else {
            rt_game3d_stream_cell *cell = &stream->cells[job->index];
            cell->proxy_staging = 0;
            if (job->error) {
                game3d_stream_cell_proxy_failed(cell);
            } else {
                game3d_stream_cell_drop_staged_proxy(cell, 0);
                cell->staged_proxy_text = job->text;
                cell->staged_proxy_text_len = job->text_len;
                job->text = NULL;
                cell->proxy_staged = 1;
            }
        }
    } else if (!stale && job->kind == 0) {
        int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
        if (job->index < 0 || job->index >= cell_count) {
            stale = 1;
        } else {
            rt_game3d_stream_cell *cell = &stream->cells[job->index];
            cell->staging = 0;
            if (job->error) {
                game3d_stream_cell_drop_staged(cell, 0);
                cell->staged_error = 1;
                cell->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
                rt_game3d_diag_record_stream_staging_error();
            } else {
                game3d_stream_cell_drop_staged(cell, 0);
                cell->staged_text = job->text;
                cell->staged_text_len = job->text_len;
                cell->staged_sidecar = job->sidecar;
                cell->staged_sidecar_len = job->sidecar_len;
                job->text = NULL;
                job->sidecar = NULL;
                cell->staged = 1;
            }
        }
    } else if (!stale) {
        int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
        if (job->index < 0 || job->index >= tile_count) {
            stale = 1;
        } else {
            rt_game3d_stream_terrain_tile *tile = &stream->terrain_tiles[job->index];
            tile->staging = 0;
            game3d_stream_terrain_tile_drop_staged(tile, 0);
            /* A missing/corrupt heightmap is recoverable for tiles (blank terrain),
             * matching the blocking loader — only cells treat it as an error. */
            tile->staged_heights = job->heights;
            tile->staged_hm_width = job->hm_width;
            tile->staged_hm_depth = job->hm_depth;
            tile->staged_sidecar = job->sidecar;
            tile->staged_sidecar_len = job->sidecar_len;
            job->heights = NULL;
            job->sidecar = NULL;
            tile->staged = 1;
        }
    }
    if (stale && !job->error)
        rt_game3d_diag_record_stream_stale_stage_dropped();
    game3d_release_ref(&job->stream);
    game3d_stream_stage_job_free_shallow(job);
}

/// @brief Drain emergency-stack jobs on the main thread through the normal commit path.
static void game3d_stream_stage_drain_enqueue_failures(void) {
    rt_game3d_stream_stage_job *jobs = (rt_game3d_stream_stage_job *)rt_atomic_exchange_ptr(
        &g_game3d_stream_failed_stage_jobs, NULL, __ATOMIC_ACQ_REL);
    while (jobs) {
        rt_game3d_stream_stage_job *next = jobs->failed_next;
        jobs->failed_next = NULL;
        game3d_stream_stage_commit(jobs);
        jobs = next;
    }
}

/// @brief Worker-thread entry: read the payload files and parse heightmap text to POD.
/// @details Touches no rt objects — file IO plus plain-C parsing only, then hands the
///   result to the main thread through the staging commit queue.
/// @param user_data Owned staging-job pointer submitted to the worker pool.
static void game3d_stream_stage_worker(void *user_data) {
    rt_game3d_stream_stage_job *job = (rt_game3d_stream_stage_job *)user_data;
    if (!job)
        return;
    if (job->kind == 0 || job->kind == 2) {
        size_t size = 0;
        uint8_t *data =
            job->primary_path ? game3d_asset_read_file_bytes(job->primary_path, &size) : NULL;
        if (!data || size == 0) {
            free(data);
            job->error = 1;
        } else {
            job->text = (char *)data;
            job->text_len = size;
        }
    } else if (job->primary_path && job->primary_path[0]) {
        size_t size = 0;
        uint8_t *data = game3d_asset_read_file_bytes(job->primary_path, &size);
        if (data && size > 0 && size < SIZE_MAX - 1) {
            char *text = (char *)malloc(size + 1);
            if (text) {
                memcpy(text, data, size);
                text[size] = '\0';
                (void)game3d_heightmap_parse_text(
                    text, &job->heights, &job->hm_width, &job->hm_depth);
                free(text);
            }
        }
        free(data);
    }
    if (job->sidecar_path && job->sidecar_path[0]) {
        size_t size = 0;
        uint8_t *data = game3d_asset_read_file_bytes(job->sidecar_path, &size);
        if (data && size > 0) {
            job->sidecar = data;
            job->sidecar_len = size;
        } else {
            free(data);
        }
    }
    if (!rt_g3d_commit_queue_enqueue_cost_cancel(g_game3d_stream_stage_queue,
                                                 game3d_stream_stage_commit,
                                                 job,
                                                 RT_G3D_COMMIT_COST_UNIT,
                                                 game3d_stream_stage_cancel)) {
        game3d_stream_stage_publish_enqueue_failure(job);
    }
}

/// @brief Lazily create the staging queue (and the shared async worker pool).
/// @return Nonzero when both async runtime and commit queue are available.
static int game3d_stream_stage_ensure_queue(void) {
    if (!game3d_asset_async_ensure_runtime())
        return 0;
    game3d_model_cache_lock();
    if (!g_game3d_stream_stage_queue)
        g_game3d_stream_stage_queue = rt_g3d_commit_queue_new();
    game3d_model_cache_unlock();
    return g_game3d_stream_stage_queue != NULL;
}

/// @brief Submit a staging job for a cell (kind 0) or terrain tile (kind 1).
/// @param stream Borrowed stream retained by the submitted job.
/// @param kind Payload kind: zero for a cell, one for terrain, or two for a proxy.
/// @param index Valid manifest-array index for the selected kind.
/// @param prefetched Nonzero when submitted speculatively rather than demanded.
static void game3d_stream_stage_submit(rt_game3d_world_stream *stream,
                                       int32_t kind,
                                       int32_t index,
                                       int8_t prefetched) {
    if (!stream || !game3d_stream_stage_ensure_queue())
        return;
    rt_game3d_stream_stage_job *job =
        (rt_game3d_stream_stage_job *)calloc(1, sizeof(rt_game3d_stream_stage_job));
    if (!job)
        return;
    job->kind = kind;
    job->index = index;
    job->generation = kind == 1 ? stream->terrain_generation : stream->cell_generation;
    if (kind == 0) {
        rt_game3d_stream_cell *cell = &stream->cells[index];
        job->primary_path = game3d_stream_strdup(cell->path ? rt_string_cstr(cell->path) : NULL);
        if (game3d_string_has_bytes(cell->sidecar_path))
            job->sidecar_path = game3d_stream_strdup(rt_string_cstr(cell->sidecar_path));
        if (!job->primary_path) {
            free(job);
            return;
        }
        cell->staging = 1;
        cell->prefetched = prefetched;
    } else if (kind == 2) {
        rt_game3d_stream_cell *cell = &stream->cells[index];
        job->primary_path =
            game3d_stream_strdup(cell->proxy_path ? rt_string_cstr(cell->proxy_path) : NULL);
        if (!job->primary_path) {
            free(job);
            return;
        }
        cell->proxy_staging = 1;
    } else {
        rt_game3d_stream_terrain_tile *tile = &stream->terrain_tiles[index];
        if (game3d_string_has_bytes(tile->heightmap_path))
            job->primary_path = game3d_stream_strdup(rt_string_cstr(tile->heightmap_path));
        if (game3d_string_has_bytes(tile->sidecar_path))
            job->sidecar_path = game3d_stream_strdup(rt_string_cstr(tile->sidecar_path));
        tile->staging = 1;
        tile->prefetched = prefetched;
    }
    job->stream = stream;
    rt_obj_retain_maybe(stream);
    if (!rt_threadpool_submit(
            g_game3d_asset_async_pool, game3d_task_fnptr(game3d_stream_stage_worker), job)) {
        if (kind == 0)
            stream->cells[index].staging = 0;
        else if (kind == 2)
            stream->cells[index].proxy_staging = 0;
        else
            stream->terrain_tiles[index].staging = 0;
        game3d_release_ref(&job->stream);
        game3d_stream_stage_job_free_shallow(job);
    }
}

/// @brief Drain worker staging results onto stream records (main thread, cheap moves).
static void game3d_stream_stage_drain(void) {
    if (g_game3d_stream_stage_queue)
        (void)rt_g3d_commit_queue_drain(g_game3d_stream_stage_queue, -1);
    game3d_stream_stage_drain_enqueue_failures();
}

/// @brief Eviction watermark for the residency budget: budget + budget/8.
/// @details New loads respect the strict budget, but already-resident content is only evicted
///   once the running total passes this higher watermark. The 12.5% gap gives budget-bound
///   residency the same hysteresis the load/unload radii give distance-bound residency, so
///   byte-count jitter or nearest-first reordering at the budget line cannot thrash a boundary
///   cell in and out every pass. Non-positive budgets stay strict (<0 unlimited, 0 evict-all).
/// @param budget Configured residency budget in bytes.
/// @return Budget plus a 12.5 percent hysteresis margin, saturated on overflow.
static int64_t game3d_stream_budget_high_watermark(int64_t budget) {
    if (budget <= 0)
        return budget;
    return game3d_i64_saturating_add(budget, budget / 8);
}

/// @brief Square a non-negative stream radius, saturating overflow and rejecting invalid radii.
/// @param radius Candidate radius.
/// @return Finite squared radius, `DBL_MAX` on overflow, or `-1` for invalid input.
static double game3d_stream_radius_sq(double radius) {
    if (!isfinite(radius) || radius < 0.0)
        return -1.0;
    if (radius == 0.0)
        return 0.0;
    if (radius > sqrt(DBL_MAX))
        return DBL_MAX;
    double sq = radius * radius;
    return isfinite(sq) ? sq : DBL_MAX;
}

/// @brief Per-pass counters, gates, and budgets accumulated during stream reconciliation.
typedef struct game3d_stream_recompute_state {
    int64_t load_budget;
    int64_t pending;
    int64_t cells;
    int64_t cell_bytes;
    int64_t terrain;
    int64_t terrain_bytes;
    int8_t async;            /* stream->async_streaming snapshot */
    int8_t cell_gate_closed; /* in-order commit gate: a nearer cell is not ready */
    int8_t terrain_gate_closed;
    int64_t commit_budget; /* staged bytes committable this pass; -1 unlimited */
    int64_t committed_bytes;
    int64_t proxy_count;       /* cells holding only their HLOD proxy this pass */
    int64_t proxy_bytes_total; /* measured bytes of resident proxy subtrees */
} game3d_stream_recompute_state;

/// @brief True when a staged commit of @p cost bytes fits this pass's commit budget.
/// @details Mirrors the commit-queue drain rule: a zero budget holds everything, and
///   an oversized first item commits alone so it can never deadlock the stream.
/// @param state Borrowed recompute state containing the commit budget and total.
/// @param cost Candidate staged payload cost in bytes.
/// @return Nonzero when this payload may commit during the current pass.
static int game3d_stream_commit_budget_allows(const game3d_stream_recompute_state *state,
                                              int64_t cost) {
    if (state->commit_budget < 0)
        return 1;
    if (state->commit_budget == 0)
        return 0;
    if (state->committed_bytes == 0)
        return 1;
    if (cost < 0)
        cost = 0;
    if (state->committed_bytes > state->commit_budget)
        return 0;
    return cost <= state->commit_budget - state->committed_bytes;
}

/// @brief Record a commit slice's wall time into the stream's stall telemetry.
/// @param stream Borrowed stream whose maximum stall sample is updated.
/// @param t0_us Monotonic start timestamp in microseconds.
static void game3d_stream_note_commit_slice(rt_game3d_world_stream *stream, int64_t t0_us) {
    int64_t t1_us = rt_clock_ticks_us();
    double ms = t1_us > t0_us ? (double)(t1_us - t0_us) / 1000.0 : 0.0;
    if (ms > stream->stream_stall_ms)
        stream->stream_stall_ms = ms;
}

/// @brief True when @p cell falls inside the velocity-prefetch window (XZ plane).
/// @param stream Borrowed stream defining center, velocity, and lookahead.
/// @param cell Borrowed cell to test.
/// @return Nonzero when asynchronous prefetch should stage the cell.
static int game3d_stream_cell_in_prefetch_window(const rt_game3d_world_stream *stream,
                                                 const rt_game3d_stream_cell *cell) {
    if (!stream || !cell || stream->prefetch_lookahead <= 0.0 || !stream->async_streaming)
        return 0;
    double speed_sq =
        stream->velocity[0] * stream->velocity[0] + stream->velocity[2] * stream->velocity[2];
    if (!isfinite(speed_sq) || speed_sq < 1e-6)
        return 0;
    double px = game3d_clamp_coord_or(
        stream->center[0] + stream->velocity[0] * stream->prefetch_lookahead, 0.0);
    double pz = game3d_clamp_coord_or(
        stream->center[2] + stream->velocity[2] * stream->prefetch_lookahead, 0.0);
    double cx = game3d_clamp_coord_or(cell->center[0], 0.0);
    double cz = game3d_clamp_coord_or(cell->center[2], 0.0);
    double dx = px - cx;
    double dz = pz - cz;
    double distance_sq = dx * dx + dz * dz;
    if (!isfinite(distance_sq))
        return 0;
    double threshold = stream->load_radius + cell->radius;
    return distance_sq <= game3d_stream_radius_sq(threshold);
}

/// @brief True when @p tile falls inside the velocity-prefetch window (XZ plane).
/// @param stream Borrowed stream defining center, velocity, and lookahead.
/// @param tile Borrowed terrain tile to test.
/// @return Nonzero when asynchronous prefetch should stage the tile.
static int game3d_stream_terrain_tile_in_prefetch_window(
    const rt_game3d_world_stream *stream, const rt_game3d_stream_terrain_tile *tile) {
    if (!stream || !tile || stream->prefetch_lookahead <= 0.0 || !stream->async_streaming)
        return 0;
    double speed_sq =
        stream->velocity[0] * stream->velocity[0] + stream->velocity[2] * stream->velocity[2];
    if (!isfinite(speed_sq) || speed_sq < 1e-6)
        return 0;
    double px = game3d_clamp_coord_or(
        stream->center[0] + stream->velocity[0] * stream->prefetch_lookahead, 0.0);
    double pz = game3d_clamp_coord_or(
        stream->center[2] + stream->velocity[2] * stream->prefetch_lookahead, 0.0);
    double cx = game3d_clamp_coord_or(tile->center[0], 0.0);
    double cz = game3d_clamp_coord_or(tile->center[2], 0.0);
    double dx = px - cx;
    double dz = pz - cz;
    double distance_sq = dx * dx + dz * dz;
    if (!isfinite(distance_sq))
        return 0;
    double threshold = stream->load_radius + tile->radius;
    return distance_sq <= game3d_stream_radius_sq(threshold);
}

/* ===== HLOD proxy ring (cell-level merged stand-ins) ====================== */

/// @brief Drop a cell's staged proxy payload without committing it.
/// @param cell Cell record whose staged proxy bytes and flag are cleared.
/// @param record_stale Nonzero to count an existing payload as stale.
static void game3d_stream_cell_drop_staged_proxy(rt_game3d_stream_cell *cell, int record_stale) {
    if (!cell)
        return;
    if (cell->proxy_staged && record_stale)
        rt_game3d_diag_record_stream_stale_stage_dropped();
    free(cell->staged_proxy_text);
    cell->staged_proxy_text = NULL;
    cell->staged_proxy_text_len = 0;
    cell->proxy_staged = 0;
}

/// @brief Effective HLOD proxy ring radius (explicit value or 4x the load radius).
/// @param stream Borrowed stream payload.
/// @return Configured positive proxy radius, four times load radius, or zero when invalid.
static double game3d_stream_effective_proxy_radius(const rt_game3d_world_stream *stream) {
    if (!stream)
        return 0.0;
    if (isfinite(stream->proxy_radius) && stream->proxy_radius > 0.0)
        return stream->proxy_radius;
    return stream->load_radius * 4.0;
}

/// @brief True when @p cell should hold (at least) its proxy at the current center.
/// @param stream Borrowed stream defining the proxy ring and hysteresis.
/// @param cell Borrowed cell with an optional proxy path.
/// @return Nonzero when the cell lies within its proxy-residency threshold.
static int game3d_stream_cell_proxy_desired(const rt_game3d_world_stream *stream,
                                            const rt_game3d_stream_cell *cell) {
    if (!stream || !cell || !cell->proxy_path)
        return 0;
    double ring = game3d_stream_effective_proxy_radius(stream);
    if (!(ring > 0.0))
        return 0;
    /* Same hysteresis width the full-residency radii use. */
    double margin = stream->unload_radius > stream->load_radius
                        ? stream->unload_radius - stream->load_radius
                        : 0.0;
    double threshold = (cell->proxy_resident ? ring + margin : ring) + cell->radius;
    if (!isfinite(threshold) || threshold < 0.0)
        threshold = cell->radius;
    return game3d_stream_cell_distance_sq(stream, cell) <= game3d_stream_radius_sq(threshold);
}

/// @brief Build "<cell-name>_proxy" for the spawned proxy subtree root.
/// @param cell Borrowed cell supplying the base name.
/// @return New runtime string containing the proxy name, or a fallback constant reference.
static rt_string game3d_stream_cell_proxy_name(const rt_game3d_stream_cell *cell) {
    const char *base = cell && cell->name ? rt_string_cstr(cell->name) : "cell";
    const char *suffix = "_proxy";
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffix);
    if (base_len > SIZE_MAX - suffix_len - 1u)
        return rt_const_cstr("cell_proxy");
    char *name = (char *)malloc(base_len + suffix_len + 1u);
    if (!name)
        return rt_const_cstr("cell_proxy");
    memcpy(name, base, base_len);
    memcpy(name + base_len, suffix, suffix_len + 1u);
    rt_string result = rt_string_from_bytes(name, base_len + suffix_len);
    free(name);
    return result;
}

/// @brief Despawn and release a cell's proxy subtree (idempotent).
/// @param stream Borrowed owning stream, or `NULL` during detached cleanup.
/// @param cell Cell record whose proxy fields are cleared.
static void game3d_world_stream_unload_cell_proxy(rt_game3d_world_stream *stream,
                                                  rt_game3d_stream_cell *cell) {
    if (!cell)
        return;
    if (cell->proxy_resident && cell->proxy_entity && stream && stream->world) {
        rt_game3d_entity *entity = (rt_game3d_entity *)rt_g3d_checked_or_null(
            cell->proxy_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        if (entity && entity->alive && entity->spawned && entity->world == stream->world)
            game3d_world_despawn_entity_tree((rt_game3d_world *)stream->world, entity, 1);
    }
    game3d_release_ref(&cell->proxy_entity);
    game3d_release_ref(&cell->proxy_scene);
    cell->proxy_resident = 0;
    cell->measured_proxy_bytes = 0;
}

/// @brief Spawn a loaded proxy scene as the cell's render-only stand-in.
/// @details Consumes @p scene. Proxies are render-only by construction: baked proxy
///   scenes carry a single mesh node, so no colliders or nav sources spawn with them.
/// @param stream Borrowed owning stream.
/// @param cell Cell record receiving the resident proxy.
/// @param scene Owned proxy Scene3D reference consumed on every path.
/// @return 1 when the proxy became resident.
static int game3d_world_stream_adopt_cell_proxy_scene(rt_game3d_world_stream *stream,
                                                      rt_game3d_stream_cell *cell,
                                                      void *scene) {
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(stream->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (!world || world->destroyed || !scene) {
        game3d_release_ref(&scene);
        return 0;
    }
    void *root = rt_scene3d_get_root(scene);
    void *entity = rt_game3d_entity_from_node(root);
    if (entity) {
        rt_string proxy_name = game3d_stream_cell_proxy_name(cell);
        rt_game3d_entity_set_name(entity, proxy_name);
        game3d_release_ref((void **)&proxy_name);
        rt_game3d_entity_set_position(entity,
                                      game3d_clamp_coord_or(cell->center[0], 0.0),
                                      game3d_clamp_coord_or(cell->center[1], 0.0),
                                      game3d_clamp_coord_or(cell->center[2], 0.0));
        int64_t next_id = world->next_entity_id;
        if (game3d_world_spawn_entity_tree(world, (rt_game3d_entity *)entity, 1, &next_id)) {
            world->next_entity_id = next_id;
            cell->proxy_scene = scene;
            cell->proxy_entity = entity;
            cell->proxy_resident = 1;
            uint64_t measured = game3d_entity_estimate_resident_bytes(entity);
            if (measured == 0)
                measured = game3d_scene_estimate_resident_bytes(scene);
            cell->measured_proxy_bytes = game3d_i64_from_u64_saturating(measured);
            if (cell->measured_proxy_bytes <= 0)
                cell->measured_proxy_bytes = cell->proxy_bytes > 0 ? cell->proxy_bytes : 16 * 1024;
            scene = NULL;
        } else {
            game3d_release_ref(&entity);
        }
    }
    game3d_release_ref(&scene);
    return cell->proxy_resident ? 1 : 0;
}

/// @brief A proxy payload failed to load/parse: disable the proxy for this cell.
/// @details Recoverable — the cell keeps streaming at full residency exactly as a
///   proxy-less cell would; the failure is surfaced through StreamStagingErrors.
/// @param cell Cell record whose proxy path and staged state are disabled.
static void game3d_stream_cell_proxy_failed(rt_game3d_stream_cell *cell) {
    if (!cell)
        return;
    game3d_stream_cell_drop_staged_proxy(cell, 0);
    game3d_release_ref((void **)&cell->proxy_path);
    rt_game3d_diag_record_stream_staging_error();
}

/// @brief Blocking-path proxy load (inline file IO).
/// @param stream Borrowed owning stream.
/// @param cell Cell record whose proxy path is loaded.
/// @return Nonzero when already or newly proxy-resident; zero on load failure.
static int game3d_world_stream_load_cell_proxy(rt_game3d_world_stream *stream,
                                               rt_game3d_stream_cell *cell) {
    if (!stream || !cell || !cell->proxy_path)
        return 0;
    if (cell->proxy_resident)
        return 1;
    void *scene = rt_scene3d_load(cell->proxy_path);
    if (!scene || !game3d_world_stream_adopt_cell_proxy_scene(stream, cell, scene)) {
        game3d_stream_cell_proxy_failed(cell);
        return 0;
    }
    return 1;
}

/// @brief Commit a worker-staged proxy payload (no main-thread file IO).
/// @param stream Borrowed owning stream.
/// @param cell Cell record containing staged proxy scene bytes.
/// @return Nonzero when already or newly proxy-resident; zero on parse or spawn failure.
static int game3d_world_stream_commit_staged_cell_proxy(rt_game3d_world_stream *stream,
                                                        rt_game3d_stream_cell *cell) {
    if (!stream || !cell || !cell->proxy_staged)
        return 0;
    if (cell->proxy_resident) {
        game3d_stream_cell_drop_staged_proxy(cell, 1);
        return 1;
    }
    void *scene = rt_scene3d_load_from_memory(
        cell->proxy_path, cell->staged_proxy_text, cell->staged_proxy_text_len);
    game3d_stream_cell_drop_staged_proxy(cell, 0);
    if (!scene || !game3d_world_stream_adopt_cell_proxy_scene(stream, cell, scene)) {
        game3d_stream_cell_proxy_failed(cell);
        return 0;
    }
    return 1;
}

/// @brief Per-pass proxy ring reconciliation for one cell.
/// @details Runs before the full-residency decision so a receding cell can swap
///   full→proxy in the same pass, and a fully-committed cell releases its proxy.
/// @param stream Borrowed owning stream.
/// @param[in,out] state Current recompute counters and async mode.
/// @param cell Cell record whose proxy state is reconciled.
/// @param full_desired Nonzero when the cell also lies inside the full-detail ring.
static void game3d_world_stream_process_cell_proxy(rt_game3d_world_stream *stream,
                                                   game3d_stream_recompute_state *state,
                                                   rt_game3d_stream_cell *cell,
                                                   int full_desired) {
    if (!cell || !cell->proxy_path)
        return;
    if (!game3d_stream_cell_proxy_desired(stream, cell)) {
        game3d_world_stream_unload_cell_proxy(stream, cell);
        game3d_stream_cell_drop_staged_proxy(cell, cell->proxy_staged ? 1 : 0);
        return;
    }
    if (cell->resident && full_desired) {
        /* Full residency owns the ring: release the proxy (it stayed attached through
         * the commit pass, so there is never a frame with neither subtree). A receding
         * cell (!full_desired) instead falls through and prepares its proxy so the
         * full->proxy swap can complete without a gap frame. */
        game3d_world_stream_unload_cell_proxy(stream, cell);
        game3d_stream_cell_drop_staged_proxy(cell, cell->proxy_staged ? 1 : 0);
        return;
    }
    if (!cell->proxy_resident) {
        if (state->async) {
            if (cell->proxy_staged) {
                int64_t slice_start_us = rt_clock_ticks_us();
                (void)game3d_world_stream_commit_staged_cell_proxy(stream, cell);
                game3d_stream_note_commit_slice(stream, slice_start_us);
            } else if (!cell->proxy_staging) {
                game3d_stream_stage_submit(stream, 2, (int32_t)(cell - stream->cells), 0);
            }
        } else {
            (void)game3d_world_stream_load_cell_proxy(stream, cell);
        }
    }
    if (cell->proxy_resident && !cell->resident) {
        state->proxy_count++;
        state->proxy_bytes_total =
            game3d_i64_saturating_add(state->proxy_bytes_total, cell->measured_proxy_bytes);
    }
}

/// @brief Reconcile one desired full-detail cell against budgets, staging state, and cooldown.
/// @param stream Borrowed owning stream.
/// @param[in,out] state Current recompute counters, gates, and work allowance.
/// @param cell Cell record to load, retain, or evict.
/// @param budget Full-detail cell residency budget in bytes, or negative for unlimited.
static void game3d_world_stream_process_cell(rt_game3d_world_stream *stream,
                                             game3d_stream_recompute_state *state,
                                             rt_game3d_stream_cell *cell,
                                             int64_t budget) {
    int64_t bytes = game3d_stream_cell_bytes(cell);
    if (cell->resident) {
        /* Resident cells survive until the high watermark so budget-line jitter cannot
         * thrash them in and out; a budget eviction also starts a reload cooldown. */
        if (!game3d_i64_budget_can_fit(
                state->cell_bytes, game3d_stream_budget_high_watermark(budget), bytes)) {
            game3d_world_stream_unload_cell(stream, cell);
            cell->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
            return;
        }
        state->cells++;
        state->cell_bytes = game3d_i64_saturating_add(state->cell_bytes, bytes);
        return;
    }
    /* New loads respect the strict budget (low watermark). */
    if (!game3d_i64_budget_can_fit(state->cell_bytes, budget, bytes))
        return;
    if (cell->reload_cooldown > 0) {
        /* Recently budget-evicted: hold as pending so the cell cannot reload the very next
         * pass and resume the thrash cycle. */
        game3d_stream_increment_pending(&state->pending);
        return;
    }
    if (state->async) {
        if (!cell->staged) {
            /* Not staged yet: kick a worker job and close the in-order commit gate so
             * farther cells cannot become resident before this one (keeps the resident
             * sequence identical to blocking mode at any worker count). */
            if (!cell->staging)
                game3d_stream_stage_submit(stream, 0, (int32_t)(cell - stream->cells), 0);
            else
                cell->prefetched = 0; /* promoted from prefetch to a demand load */
            state->cell_gate_closed = 1;
            game3d_stream_increment_pending(&state->pending);
            return;
        }
        int64_t commit_cost = game3d_i64_saturating_add((int64_t)cell->staged_text_len,
                                                        (int64_t)cell->staged_sidecar_len);
        if (state->cell_gate_closed || state->load_budget == 0 ||
            !game3d_stream_commit_budget_allows(state, commit_cost)) {
            state->cell_gate_closed = 1;
            game3d_stream_increment_pending(&state->pending);
            return;
        }
        if (state->load_budget > 0)
            state->load_budget--;
        int64_t slice_start_us = rt_clock_ticks_us();
        int committed = game3d_world_stream_commit_staged_cell(stream, cell);
        game3d_stream_note_commit_slice(stream, slice_start_us);
        state->committed_bytes = game3d_i64_saturating_add(state->committed_bytes, commit_cost);
        if (committed) {
            bytes = game3d_stream_cell_bytes(cell);
            if (!game3d_i64_budget_can_fit(
                    state->cell_bytes, game3d_stream_budget_high_watermark(budget), bytes)) {
                game3d_world_stream_unload_cell(stream, cell);
                cell->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
                return;
            }
            state->cells++;
            state->cell_bytes = game3d_i64_saturating_add(state->cell_bytes, bytes);
        }
        return;
    }
    if (state->load_budget == 0) {
        game3d_stream_increment_pending(&state->pending);
        return;
    }
    if (state->load_budget > 0)
        state->load_budget--;
    if (game3d_world_stream_load_cell(stream, cell)) {
        bytes = game3d_stream_cell_bytes(cell);
        if (!game3d_i64_budget_can_fit(
                state->cell_bytes, game3d_stream_budget_high_watermark(budget), bytes)) {
            /* Measured size busted even the eviction watermark — undo and back off. */
            game3d_world_stream_unload_cell(stream, cell);
            cell->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
            return;
        }
        state->cells++;
        state->cell_bytes = game3d_i64_saturating_add(state->cell_bytes, bytes);
    }
}

/// @brief Reconcile manifest-backed scene cells and update resident cell counters.
/// @param stream Borrowed stream whose cells are processed nearest-first.
/// @param[in,out] state Current recompute state receiving cell and proxy telemetry.
static void game3d_world_stream_recompute_cells(rt_game3d_world_stream *stream,
                                                game3d_stream_recompute_state *state) {
    if (stream->cells_manifest_loaded) {
        int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
        int64_t manifest_bytes = game3d_stream_manifest_bytes(stream->cells_manifest);
        int64_t budget = stream->residency_budget_bytes;
        if (budget < 0 || manifest_bytes <= budget) {
            state->cell_bytes = manifest_bytes;
            rt_game3d_stream_load_candidate *candidates = NULL;
            int32_t candidate_count = 0;
            if (cell_count > 0 &&
                game3d_stream_reserve_load_candidates(
                    &stream->cell_candidates, &stream->cell_candidate_capacity, cell_count))
                candidates = stream->cell_candidates;
            for (int32_t i = 0; i < cell_count; ++i) {
                rt_game3d_stream_cell *cell = &stream->cells[i];
                if (cell->reload_cooldown > 0)
                    cell->reload_cooldown--;
                if (cell->resident)
                    game3d_world_stream_refresh_cell_residency_bytes(cell);
                if (!game3d_stream_cell_desired(stream, cell)) {
                    /* Receding swap: keep the full cell attached until its proxy is
                     * ready so there is never a frame with neither subtree drawn. */
                    game3d_world_stream_process_cell_proxy(stream, state, cell, 0);
                    if (cell->resident && cell->proxy_path &&
                        game3d_stream_cell_proxy_desired(stream, cell) && !cell->proxy_resident) {
                        state->cells++;
                        state->cell_bytes = game3d_i64_saturating_add(
                            state->cell_bytes, game3d_stream_cell_bytes(cell));
                        continue;
                    }
                    {
                        int8_t was_resident = cell->resident;
                        game3d_world_stream_unload_cell(stream, cell);
                        if (was_resident && cell->proxy_resident) {
                            /* The full->proxy recede swap completed this pass: the
                             * proxy became this cell's representation after the
                             * telemetry check inside process_cell_proxy already ran
                             * (while the full cell was still attached), so count it
                             * here. Non-swap passes were already counted there. */
                            state->proxy_count++;
                            state->proxy_bytes_total = game3d_i64_saturating_add(
                                state->proxy_bytes_total, cell->measured_proxy_bytes);
                        }
                    }
                    if ((cell->staged || cell->staging) &&
                        !game3d_stream_cell_in_prefetch_window(stream, cell))
                        game3d_stream_cell_drop_staged(cell, cell->staged ? 1 : 0);
                    continue;
                }
                game3d_world_stream_process_cell_proxy(stream, state, cell, 1);
                if (candidates) {
                    candidates[candidate_count].index = i;
                    candidates[candidate_count].distance_sq =
                        game3d_stream_cell_distance_sq(stream, cell);
                    candidate_count++;
                }
            }
            if (candidates) {
                if (candidate_count > 1)
                    qsort(candidates,
                          (size_t)candidate_count,
                          sizeof(*candidates),
                          game3d_stream_load_candidate_cmp);
                for (int32_t ci = 0; ci < candidate_count; ++ci) {
                    rt_game3d_stream_cell *cell = &stream->cells[candidates[ci].index];
                    game3d_world_stream_process_cell(stream, state, cell, budget);
                }
            } else {
                double previous_distance_sq = -1.0;
                int32_t previous_index = -1;
                for (;;) {
                    double next_distance_sq = HUGE_VAL;
                    int32_t next_index = game3d_world_stream_pick_next_cell_noalloc(
                        stream, previous_distance_sq, previous_index, &next_distance_sq);
                    if (next_index < 0)
                        break;
                    game3d_world_stream_process_cell(
                        stream, state, &stream->cells[next_index], budget);
                    previous_distance_sq = next_distance_sq;
                    previous_index = next_index;
                }
            }
        } else {
            for (int32_t i = 0; i < cell_count; ++i) {
                game3d_world_stream_unload_cell(stream, &stream->cells[i]);
                game3d_world_stream_unload_cell_proxy(stream, &stream->cells[i]);
            }
            state->cell_bytes = 0;
        }
    } else {
        state->cells = game3d_string_has_bytes(stream->cells_manifest)
                           ? game3d_stream_radius_slots(stream->load_radius, 128.0)
                           : 0;
        state->cell_bytes =
            game3d_i64_saturating_add(game3d_stream_manifest_bytes(stream->cells_manifest),
                                      game3d_i64_saturating_mul(state->cells, 64 * 1024));
    }
}

/// @brief Reconcile one desired terrain tile against budgets, staging state, and cooldown.
/// @param stream Borrowed owning stream.
/// @param[in,out] state Current recompute counters, gates, and work allowance.
/// @param tile Terrain record to load, retain, or evict.
/// @param terrain_unlimited Nonzero when terrain is not constrained by remaining bytes.
/// @param remaining_budget Terrain-specific residency allowance.
static void game3d_world_stream_process_terrain_tile(rt_game3d_world_stream *stream,
                                                     game3d_stream_recompute_state *state,
                                                     rt_game3d_stream_terrain_tile *tile,
                                                     int terrain_unlimited,
                                                     int64_t remaining_budget) {
    int64_t tile_bytes = game3d_stream_terrain_tile_bytes(tile);
    if (game3d_stream_terrain_tile_terrain_ref(tile)) {
        /* Resident tiles survive until the high watermark (see process_cell). */
        if (!terrain_unlimited &&
            !game3d_i64_budget_can_fit(state->terrain_bytes,
                                       game3d_stream_budget_high_watermark(remaining_budget),
                                       tile_bytes)) {
            game3d_world_stream_unload_terrain_tile(stream, tile);
            tile->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
            return;
        }
        state->terrain++;
        state->terrain_bytes = game3d_i64_saturating_add(state->terrain_bytes, tile_bytes);
        return;
    }
    if (!terrain_unlimited &&
        !game3d_i64_budget_can_fit(state->terrain_bytes, remaining_budget, tile_bytes)) {
        if (tile->resident)
            game3d_world_stream_unload_terrain_tile(stream, tile);
        return;
    }
    if (tile->reload_cooldown > 0) {
        game3d_stream_increment_pending(&state->pending);
        return;
    }
    if (state->async) {
        if (!tile->staged) {
            if (!tile->staging)
                game3d_stream_stage_submit(stream, 1, (int32_t)(tile - stream->terrain_tiles), 0);
            else
                tile->prefetched = 0;
            state->terrain_gate_closed = 1;
            game3d_stream_increment_pending(&state->pending);
            return;
        }
        int64_t commit_cost = game3d_i64_saturating_add(
            game3d_i64_saturating_mul(tile->staged_hm_width * tile->staged_hm_depth,
                                      (int64_t)sizeof(double)),
            (int64_t)tile->staged_sidecar_len);
        if (state->terrain_gate_closed || state->load_budget == 0 ||
            !game3d_stream_commit_budget_allows(state, commit_cost)) {
            state->terrain_gate_closed = 1;
            game3d_stream_increment_pending(&state->pending);
            return;
        }
        if (state->load_budget > 0)
            state->load_budget--;
        int64_t slice_start_us = rt_clock_ticks_us();
        int committed = game3d_world_stream_commit_staged_terrain_tile(stream, tile);
        game3d_stream_note_commit_slice(stream, slice_start_us);
        state->committed_bytes = game3d_i64_saturating_add(state->committed_bytes, commit_cost);
        if (committed) {
            tile_bytes = game3d_stream_terrain_tile_bytes(tile);
            if (!terrain_unlimited &&
                !game3d_i64_budget_can_fit(state->terrain_bytes,
                                           game3d_stream_budget_high_watermark(remaining_budget),
                                           tile_bytes)) {
                game3d_world_stream_unload_terrain_tile(stream, tile);
                tile->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
                return;
            }
            state->terrain++;
            state->terrain_bytes = game3d_i64_saturating_add(state->terrain_bytes, tile_bytes);
        }
        return;
    }
    if (state->load_budget == 0) {
        game3d_stream_increment_pending(&state->pending);
        return;
    }
    if (state->load_budget > 0)
        state->load_budget--;
    if (game3d_world_stream_load_terrain_tile(stream, tile)) {
        tile_bytes = game3d_stream_terrain_tile_bytes(tile);
        if (!terrain_unlimited &&
            !game3d_i64_budget_can_fit(state->terrain_bytes,
                                       game3d_stream_budget_high_watermark(remaining_budget),
                                       tile_bytes)) {
            game3d_world_stream_unload_terrain_tile(stream, tile);
            tile->reload_cooldown = GAME3D_STREAM_BUDGET_EVICT_COOLDOWN;
            return;
        }
        state->terrain++;
        state->terrain_bytes = game3d_i64_saturating_add(state->terrain_bytes, tile_bytes);
    }
}

/// @brief Reduce projected fallback terrain/cell counts until their estimated bytes fit.
/// @param stream Borrowed stream supplying manifest byte costs.
/// @param[in,out] state Recompute state whose projected counts and bytes are reduced.
/// @param budget Total residency budget, or negative for unlimited.
static void game3d_world_stream_clamp_projected_terrain(rt_game3d_world_stream *stream,
                                                        game3d_stream_recompute_state *state,
                                                        int64_t budget) {
    int64_t bytes = game3d_i64_saturating_add(state->cell_bytes, state->terrain_bytes);
    while (budget >= 0 && bytes > budget && (state->cells > 0 || state->terrain > 0)) {
        if (!stream->cells_manifest_loaded && state->cells >= state->terrain && state->cells > 0)
            state->cells--;
        else if (state->terrain > 0)
            state->terrain--;
        else if (stream->cells_manifest_loaded)
            break;
        if (stream->cells_manifest_loaded)
            state->cell_bytes = game3d_world_stream_resident_cell_bytes(stream);
        else
            state->cell_bytes =
                game3d_i64_saturating_add(game3d_stream_manifest_bytes(stream->cells_manifest),
                                          game3d_i64_saturating_mul(state->cells, 64 * 1024));
        state->terrain_bytes =
            game3d_i64_saturating_add(game3d_stream_manifest_bytes(stream->terrain_manifest),
                                      game3d_i64_saturating_mul(state->terrain, 256 * 1024));
        bytes = game3d_i64_saturating_add(state->cell_bytes, state->terrain_bytes);
    }
}

/// @brief Reconcile manifest-backed terrain tiles and update resident terrain counters.
/// @param stream Borrowed stream whose terrain tiles are processed nearest-first.
/// @param[in,out] state Current recompute state receiving terrain telemetry.
static void game3d_world_stream_recompute_terrain(rt_game3d_world_stream *stream,
                                                  game3d_stream_recompute_state *state) {
    int64_t budget = stream->residency_budget_bytes;
    if (stream->terrain_manifest_loaded) {
        int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
        int64_t manifest_bytes = game3d_stream_manifest_bytes(stream->terrain_manifest);
        int terrain_unlimited = budget < 0;
        int64_t remaining_budget =
            terrain_unlimited ? INT64_MAX
                              : (budget >= state->cell_bytes ? budget - state->cell_bytes : -1);
        if (terrain_unlimited || (remaining_budget >= 0 && manifest_bytes <= remaining_budget)) {
            state->terrain_bytes = manifest_bytes;
            rt_game3d_stream_load_candidate *candidates = NULL;
            int32_t candidate_count = 0;
            if (tile_count > 0 &&
                game3d_stream_reserve_load_candidates(
                    &stream->terrain_candidates, &stream->terrain_candidate_capacity, tile_count))
                candidates = stream->terrain_candidates;
            for (int32_t i = 0; i < tile_count; ++i) {
                rt_game3d_stream_terrain_tile *tile = &stream->terrain_tiles[i];
                if (tile->reload_cooldown > 0)
                    tile->reload_cooldown--;
                if (!game3d_stream_terrain_tile_desired(stream, tile)) {
                    game3d_world_stream_unload_terrain_tile(stream, tile);
                    if ((tile->staged || tile->staging) &&
                        !game3d_stream_terrain_tile_in_prefetch_window(stream, tile))
                        game3d_stream_terrain_tile_drop_staged(tile, tile->staged ? 1 : 0);
                    continue;
                }
                if (candidates) {
                    candidates[candidate_count].index = i;
                    candidates[candidate_count].distance_sq =
                        game3d_stream_terrain_tile_distance_sq(stream, tile);
                    candidate_count++;
                }
            }
            if (candidates) {
                if (candidate_count > 1)
                    qsort(candidates,
                          (size_t)candidate_count,
                          sizeof(*candidates),
                          game3d_stream_load_candidate_cmp);
                for (int32_t ci = 0; ci < candidate_count; ++ci) {
                    rt_game3d_stream_terrain_tile *tile =
                        &stream->terrain_tiles[candidates[ci].index];
                    game3d_world_stream_process_terrain_tile(
                        stream, state, tile, terrain_unlimited, remaining_budget);
                }
            } else {
                double previous_distance_sq = -1.0;
                int32_t previous_index = -1;
                for (;;) {
                    double next_distance_sq = HUGE_VAL;
                    int32_t next_index = game3d_world_stream_pick_next_terrain_noalloc(
                        stream, previous_distance_sq, previous_index, &next_distance_sq);
                    if (next_index < 0)
                        break;
                    game3d_world_stream_process_terrain_tile(stream,
                                                             state,
                                                             &stream->terrain_tiles[next_index],
                                                             terrain_unlimited,
                                                             remaining_budget);
                    previous_distance_sq = next_distance_sq;
                    previous_index = next_index;
                }
            }
        } else {
            for (int32_t i = 0; i < tile_count; ++i)
                game3d_world_stream_unload_terrain_tile(stream, &stream->terrain_tiles[i]);
            state->terrain_bytes = 0;
        }
    } else {
        state->terrain = game3d_string_has_bytes(stream->terrain_manifest)
                             ? game3d_stream_radius_slots(stream->load_radius, 256.0)
                             : 0;
        state->terrain_bytes =
            game3d_i64_saturating_add(game3d_stream_manifest_bytes(stream->terrain_manifest),
                                      game3d_i64_saturating_mul(state->terrain, 256 * 1024));
        game3d_world_stream_clamp_projected_terrain(stream, state, budget);
    }
}

/// @brief Apply final residency-budget clamping and publish telemetry counters.
/// @param stream Borrowed stream whose public telemetry fields are updated.
/// @param[in,out] state Completed recompute state, possibly cleared after a high-watermark
/// violation.
static void game3d_world_stream_commit_recompute(rt_game3d_world_stream *stream,
                                                 game3d_stream_recompute_state *state) {
    int64_t budget = stream->residency_budget_bytes;
    int64_t bytes = game3d_i64_saturating_add(state->cell_bytes, state->terrain_bytes);
    bytes = game3d_i64_saturating_add(bytes, state->proxy_bytes_total);
    if (budget >= 0 && bytes > game3d_stream_budget_high_watermark(budget)) {
        if (stream->cells_manifest_loaded) {
            int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
            for (int32_t i = 0; i < cell_count; ++i) {
                game3d_world_stream_unload_cell(stream, &stream->cells[i]);
                game3d_world_stream_unload_cell_proxy(stream, &stream->cells[i]);
            }
            state->proxy_count = 0;
            state->proxy_bytes_total = 0;
        }
        if (stream->terrain_manifest_loaded) {
            int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
            for (int32_t i = 0; i < tile_count; ++i)
                game3d_world_stream_unload_terrain_tile(stream, &stream->terrain_tiles[i]);
        }
        state->cells = 0;
        state->terrain = 0;
        bytes = 0;
    }
    stream->resident_cell_count = state->cells;
    stream->resident_terrain_tile_count = state->terrain;
    stream->resident_bytes = bytes;
    stream->pending_request_count = state->pending;
    stream->proxy_resident_count = state->proxy_count;
    stream->proxy_resident_bytes = state->proxy_bytes_total;
}

/// @brief Reconcile streamed content with the camera: load in-radius cells/tiles and unload far
/// ones.
/// @details Loads nearest-first within the per-frame byte budget and the residency budget, and
/// unloads
///          anything beyond the unload radius — the heart of the open-world streaming loop.
/// @param stream Borrowed stream payload to reconcile.
/// @param load_budget Maximum new loads or commits allowed this pass; negative means unlimited.
static void game3d_world_stream_recompute(rt_game3d_world_stream *stream, int64_t load_budget) {
    if (!stream)
        return;
    game3d_stream_recompute_state state;
    memset(&state, 0, sizeof(state));
    state.load_budget = load_budget;
    state.async = stream->async_streaming;
    state.commit_budget = stream->commit_budget_bytes;
    game3d_world_stream_recompute_cells(stream, &state);
    game3d_world_stream_recompute_terrain(stream, &state);
    game3d_world_stream_commit_recompute(stream, &state);
}

/// @brief Update the smoothed center-velocity estimate used by prefetch.
/// @details A jump larger than the unload radius is a teleport: the estimate resets so
///   a spurious velocity spike cannot prefetch phantom cells along the jump vector.
/// @param stream Borrowed stream whose center history and velocity are updated.
/// @param dt Positive elapsed time in seconds; non-positive values initialize history only.
static void game3d_world_stream_update_velocity(rt_game3d_world_stream *stream, double dt) {
    if (!stream)
        return;
    if (!stream->has_prev_center || !(dt > 0.0)) {
        stream->prev_center[0] = stream->center[0];
        stream->prev_center[1] = stream->center[1];
        stream->prev_center[2] = stream->center[2];
        stream->has_prev_center = 1;
        return;
    }
    double dx = stream->center[0] - stream->prev_center[0];
    double dy = stream->center[1] - stream->prev_center[1];
    double dz = stream->center[2] - stream->prev_center[2];
    double jump_sq = dx * dx + dz * dz;
    double teleport = stream->unload_radius > 1.0 ? stream->unload_radius : 1.0;
    if (!isfinite(jump_sq) || jump_sq > teleport * teleport) {
        stream->velocity[0] = 0.0;
        stream->velocity[1] = 0.0;
        stream->velocity[2] = 0.0;
    } else {
        double inv_dt = 1.0 / dt;
        stream->velocity[0] = stream->velocity[0] * 0.8 + dx * inv_dt * 0.2;
        stream->velocity[1] = stream->velocity[1] * 0.8 + dy * inv_dt * 0.2;
        stream->velocity[2] = stream->velocity[2] * 0.8 + dz * inv_dt * 0.2;
        if (!isfinite(stream->velocity[0]) || !isfinite(stream->velocity[1]) ||
            !isfinite(stream->velocity[2])) {
            stream->velocity[0] = 0.0;
            stream->velocity[1] = 0.0;
            stream->velocity[2] = 0.0;
        }
    }
    stream->prev_center[0] = stream->center[0];
    stream->prev_center[1] = stream->center[1];
    stream->prev_center[2] = stream->center[2];
}

/// @brief Stage-ahead pass: submit worker jobs for cells/tiles the moving center will
///   want within the prefetch lookahead, and publish the prefetch telemetry counter.
/// @param stream Borrowed stream whose non-resident manifest entries are considered.
static void game3d_world_stream_prefetch(rt_game3d_world_stream *stream) {
    if (!stream)
        return;
    stream->prefetched_cell_count = 0;
    if (!stream->async_streaming || stream->prefetch_lookahead <= 0.0)
        return;
    if (stream->cells_manifest_loaded) {
        int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
        for (int32_t i = 0; i < cell_count; ++i) {
            rt_game3d_stream_cell *cell = &stream->cells[i];
            if (cell->resident || cell->reload_cooldown > 0)
                continue;
            if (game3d_stream_cell_desired(stream, cell))
                continue; /* the demand path owns it */
            if (!game3d_stream_cell_in_prefetch_window(stream, cell))
                continue;
            if (!cell->staging && !cell->staged && cell->path)
                game3d_stream_stage_submit(stream, 0, i, 1);
            if (cell->prefetched && (cell->staging || cell->staged))
                stream->prefetched_cell_count++;
        }
    }
    if (stream->terrain_manifest_loaded) {
        int32_t tile_count = game3d_world_stream_safe_terrain_tile_count(stream);
        for (int32_t i = 0; i < tile_count; ++i) {
            rt_game3d_stream_terrain_tile *tile = &stream->terrain_tiles[i];
            if (tile->resident || tile->reload_cooldown > 0)
                continue;
            if (game3d_stream_terrain_tile_desired(stream, tile))
                continue;
            if (!game3d_stream_terrain_tile_in_prefetch_window(stream, tile))
                continue;
            if (!tile->staging && !tile->staged)
                game3d_stream_stage_submit(stream, 1, i, 1);
        }
    }
}

/* ===== HLOD proxy bake + impostor automation (editor/authoring hooks) ===== */

#define GAME3D_PROXY_TRIANGLE_BUDGET 800
#define GAME3D_PROXY_MERGE_TRI_CAP 300000
#define GAME3D_PROXY_MAX_MATERIALS 64
#define GAME3D_IMPOSTOR_FRAME_COUNT 8
#define GAME3D_IMPOSTOR_FRAME_SIZE 128

/// @brief One drawable gathered from a loaded cell subtree during a proxy bake.
typedef struct game3d_proxy_bake_source {
    void *mesh;         /* borrowed */
    void *material;     /* borrowed (may be NULL) */
    double matrix[16];  /* node world matrix, row-major */
    int32_t color_slot; /* atlas block index for this drawable's material */
} game3d_proxy_bake_source;

/// @brief Gather drawable mesh/matrix pairs from a SceneNode3D hierarchy iteratively.
/// @param root Borrowed hierarchy root, normally the detached root owned by a streamed entity.
/// @param sources Caller-owned output array populated in traversal order.
/// @param capacity Maximum number of output records and the traversal's drawable cap.
/// @return Number of sources gathered (capped at @p capacity).
static int32_t game3d_proxy_gather_sources(void *root,
                                           game3d_proxy_bake_source *sources,
                                           int32_t capacity) {
    if (!root || !rt_g3d_has_class(root, RT_G3D_SCENENODE3D_CLASS_ID) || !sources || capacity <= 0)
        return 0;
    void *stack[256];
    int32_t stack_count = 0;
    int32_t count = 0;
    stack[stack_count++] = root;
    while (stack_count > 0 && count < capacity) {
        void *node = stack[--stack_count];
        void *mesh = rt_scene_node3d_get_mesh(node);
        if (mesh && rt_mesh3d_get_triangle_count(mesh) > 0) {
            game3d_proxy_bake_source *src = &sources[count];
            src->mesh = mesh;
            src->material = rt_scene_node3d_get_material(node);
            if (rt_scene_node3d_get_world_matrix_components(node, src->matrix)) {
                src->color_slot = 0;
                count++;
            }
        }
        int64_t child_count = rt_scene_node3d_child_count(node);
        for (int64_t i = 0; i < child_count && stack_count < 256; ++i) {
            void *child = rt_scene_node3d_get_child(node, i);
            if (child)
                stack[stack_count++] = child;
        }
    }
    return count;
}

/// @brief Read a material's diffuse color as packed 0xRRGGBBAA (white when NULL).
/// @param material Borrowed Material3D handle, or `NULL` for opaque white.
/// @return Packed clamped diffuse color with an opaque alpha channel.
static int64_t game3d_proxy_material_rgba(void *material) {
    double rgb[3] = {1.0, 1.0, 1.0};
    if (material) {
        void *color = rt_material3d_get_color(material);
        if (color) {
            (void)game3d_read_vec3(color, rgb, "Game3D.WorldStream3D.bakeCellProxy: color");
            game3d_release_ref(&color);
        }
    }
    int64_t r = (int64_t)(fmin(fmax(rgb[0], 0.0), 1.0) * 255.0 + 0.5);
    int64_t g = (int64_t)(fmin(fmax(rgb[1], 0.0), 1.0) * 255.0 + 0.5);
    int64_t b = (int64_t)(fmin(fmax(rgb[2], 0.0), 1.0) * 255.0 + 0.5);
    return (r << 24) | (g << 16) | (b << 8) | 0xFF;
}

/// @brief Derive "<cell path minus its scene extension>_proxy.<same ext>".
/// @details Cells may use the canonical `.scene3d` extension or the legacy
///          `.vscn` alias (ADR 0182); the proxy keeps the cell's spelling.
/// @param cell_path Borrowed source cell path.
/// @return New runtime string containing the derived proxy path, or `NULL` on invalid input or
/// allocation failure.
static rt_string game3d_proxy_default_path(rt_string cell_path) {
    const char *base = cell_path ? rt_string_cstr(cell_path) : NULL;
    if (!base)
        return NULL;
    size_t len = strlen(base);
    size_t stem = len;
    const char *suffix = "_proxy.vscn";
    if (len > 8 && strcmp(base + len - 8, ".scene3d") == 0) {
        stem = len - 8;
        suffix = "_proxy.scene3d";
    } else if (len > 5 && strcmp(base + len - 5, ".vscn") == 0) {
        stem = len - 5;
    }
    size_t suffix_len = strlen(suffix);
    if (stem > SIZE_MAX - suffix_len - 1u)
        return NULL;
    char *path = (char *)malloc(stem + suffix_len + 1u);
    if (!path)
        return NULL;
    memcpy(path, base, stem);
    memcpy(path + stem, suffix, suffix_len + 1u);
    rt_string result = rt_string_from_bytes(path, stem + suffix_len);
    free(path);
    return result;
}

/// @brief Bake a merged low-poly proxy `.vscn` for a resident cell.
/// @details Authoring-time hook (never runs during play): merges the cell's drawable
///   meshes in cell-local space, simplifies to the proxy triangle budget, bakes each
///   source material's diffuse color into a small block atlas, and saves the result
///   next to the cell (or at the manifest "proxy" path when authored). Returns 1 on
///   success with the cell's proxy path/bytes updated for this session.
/// @param obj Borrowed WorldStream3D handle.
/// @param index Zero-based index of a currently resident manifest cell.
/// @return Nonzero when a proxy scene was built and saved; otherwise zero.
int8_t rt_game3d_world_stream_bake_cell_proxy(void *obj, int64_t index) {
    rt_game3d_world_stream *stream =
        game3d_world_stream_checked(obj, "Game3D.WorldStream3D.bakeCellProxy: invalid stream");
    if (!stream)
        return 0;
    int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
    if (index < 0 || index >= cell_count)
        return 0;
    rt_game3d_stream_cell *cell = &stream->cells[index];
    if (!cell->resident || !cell->entity || !cell->path)
        return 0;
    rt_game3d_entity *cell_entity =
        (rt_game3d_entity *)rt_g3d_checked_or_null(cell->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    void *entity_root = game3d_entity_node_ref(cell_entity);
    if (!entity_root)
        return 0;

    game3d_proxy_bake_source *sources = (game3d_proxy_bake_source *)calloc(
        GAME3D_PROXY_MAX_MATERIALS * 4, sizeof(game3d_proxy_bake_source));
    if (!sources)
        return 0;
    int32_t source_count =
        game3d_proxy_gather_sources(entity_root, sources, GAME3D_PROXY_MAX_MATERIALS * 4);
    if (source_count <= 0) {
        free(sources);
        return 0;
    }

    /* Cell-local space: subtract the spawned root's translation (rotation identity). */
    double root_matrix[16];
    double root_tx = 0.0, root_ty = 0.0, root_tz = 0.0;
    if (rt_scene_node3d_get_world_matrix_components(entity_root, root_matrix)) {
        root_tx = root_matrix[3];
        root_ty = root_matrix[7];
        root_tz = root_matrix[11];
    }

    /* Unique materials -> flat-color atlas blocks. */
    void *unique_materials[GAME3D_PROXY_MAX_MATERIALS];
    int32_t material_count = 0;
    for (int32_t i = 0; i < source_count; ++i) {
        int32_t slot = -1;
        for (int32_t m = 0; m < material_count; ++m) {
            if (unique_materials[m] == sources[i].material) {
                slot = m;
                break;
            }
        }
        if (slot < 0 && material_count < GAME3D_PROXY_MAX_MATERIALS) {
            slot = material_count;
            unique_materials[material_count++] = sources[i].material;
        }
        sources[i].color_slot = slot < 0 ? 0 : slot;
    }
    if (material_count <= 0) {
        unique_materials[0] = NULL;
        material_count = 1;
    }
    void *atlas = rt_pixels_new((int64_t)material_count * 4, 4);
    if (!atlas) {
        free(sources);
        return 0;
    }
    for (int32_t m = 0; m < material_count; ++m) {
        int64_t rgba = game3d_proxy_material_rgba(unique_materials[m]);
        for (int64_t y = 0; y < 4; ++y)
            for (int64_t x = 0; x < 4; ++x)
                rt_pixels_set(atlas, (int64_t)m * 4 + x, y, rgba);
    }

    /* Merge everything into one transform-baked mesh with block-center UVs. */
    void *merged = rt_mesh3d_new();
    if (!merged) {
        game3d_release_ref(&atlas);
        free(sources);
        return 0;
    }
    int64_t merged_tris = 0;
    for (int32_t i = 0; i < source_count && merged_tris < GAME3D_PROXY_MERGE_TRI_CAP; ++i) {
        game3d_proxy_bake_source *src = &sources[i];
        const double *mtx = src->matrix;
        double u = ((double)src->color_slot + 0.5) / (double)material_count;
        int64_t tri_count = rt_mesh3d_get_triangle_count(src->mesh);
        for (int64_t t = 0; t < tri_count && merged_tris < GAME3D_PROXY_MERGE_TRI_CAP; ++t) {
            int64_t idx[3];
            if (!rt_mesh3d_get_triangle_raw(src->mesh, t, idx))
                break;
            int64_t base = rt_mesh3d_get_vertex_count(merged);
            int ok = 1;
            for (int k = 0; k < 3 && ok; ++k) {
                double pos[3], normal[3], uv[2];
                if (!rt_mesh3d_get_vertex_raw(src->mesh, idx[k], pos, normal, uv)) {
                    ok = 0;
                    break;
                }
                double wx = mtx[0] * pos[0] + mtx[1] * pos[1] + mtx[2] * pos[2] + mtx[3];
                double wy = mtx[4] * pos[0] + mtx[5] * pos[1] + mtx[6] * pos[2] + mtx[7];
                double wz = mtx[8] * pos[0] + mtx[9] * pos[1] + mtx[10] * pos[2] + mtx[11];
                double nx = mtx[0] * normal[0] + mtx[1] * normal[1] + mtx[2] * normal[2];
                double ny = mtx[4] * normal[0] + mtx[5] * normal[1] + mtx[6] * normal[2];
                double nz = mtx[8] * normal[0] + mtx[9] * normal[1] + mtx[10] * normal[2];
                double nlen = sqrt(nx * nx + ny * ny + nz * nz);
                if (nlen > 1e-12) {
                    nx /= nlen;
                    ny /= nlen;
                    nz /= nlen;
                } else {
                    nx = 0.0;
                    ny = 1.0;
                    nz = 0.0;
                }
                rt_mesh3d_add_vertex(
                    merged, wx - root_tx, wy - root_ty, wz - root_tz, nx, ny, nz, u, 0.5);
            }
            if (!ok)
                break;
            rt_mesh3d_add_triangle(merged, base, base + 1, base + 2);
            merged_tris++;
        }
    }
    free(sources);
    if (merged_tris <= 0) {
        game3d_release_ref(&merged);
        game3d_release_ref(&atlas);
        return 0;
    }

    void *proxy_mesh = merged;
    if (merged_tris > GAME3D_PROXY_TRIANGLE_BUDGET) {
        void *simplified = rt_mesh3d_simplify(merged, GAME3D_PROXY_TRIANGLE_BUDGET);
        if (simplified) {
            game3d_release_ref(&merged);
            proxy_mesh = simplified;
        }
    }

    /* Assemble and save the single-node proxy scene. */
    int8_t saved = 0;
    void *proxy_scene = rt_scene3d_new();
    void *proxy_node = rt_scene_node3d_new();
    void *proxy_material = rt_material3d_new();
    if (proxy_scene && proxy_node && proxy_material) {
        rt_string node_name = game3d_stream_cell_proxy_name(cell);
        rt_scene_node3d_set_name(proxy_node, node_name);
        game3d_release_ref((void **)&node_name);
        rt_material3d_set_texture(proxy_material, atlas);
        rt_material3d_set_unlit(proxy_material, 1);
        rt_scene_node3d_set_mesh(proxy_node, proxy_mesh);
        rt_scene_node3d_set_material(proxy_node, proxy_material);
        rt_scene3d_add(proxy_scene, proxy_node);

        rt_string out_path = cell->proxy_path ? rt_string_ref(cell->proxy_path)
                                              : game3d_proxy_default_path(cell->path);
        if (out_path) {
            if (rt_scene3d_save(proxy_scene, out_path) == 1) {
                if (!cell->proxy_path)
                    game3d_assign_ref((void **)&cell->proxy_path, out_path);
                cell->proxy_bytes = game3d_stream_manifest_bytes(cell->proxy_path);
                saved = 1;
            }
            rt_string_unref(out_path);
        }
    }
    game3d_release_ref(&proxy_node);
    game3d_release_ref(&proxy_scene);
    game3d_release_ref(&proxy_material);
    game3d_release_ref(&proxy_mesh);
    game3d_release_ref(&atlas);
    return saved;
}

/// @brief Generate yaw-strip impostors for every cell currently holding its proxy.
/// @details Authoring/loading-screen hook: renders each resident proxy mesh from
///   GAME3D_IMPOSTOR_FRAME_COUNT yaw angles into a horizontal strip via an
///   off-screen RenderTarget3D, then installs the strip as a multi-frame impostor
///   on the proxy's mesh node, active beyond @p distance. Returns the number of
///   cells that received impostors.
/// @param obj Borrowed WorldStream3D handle.
/// @param distance Non-negative world-space activation distance for generated impostors.
/// @return Number of resident cell proxies that received complete yaw-strip impostors.
int64_t rt_game3d_world_stream_generate_impostors(void *obj, double distance) {
    rt_game3d_world_stream *stream =
        game3d_world_stream_checked(obj, "Game3D.WorldStream3D.generateImpostors: invalid stream");
    if (!stream)
        return 0;
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(stream->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (!world || world->destroyed || !world->canvas)
        return 0;
    distance = game3d_finite_or(distance, 0.0);
    if (distance < 0.0)
        distance = 0.0;

    int64_t generated = 0;
    int32_t cell_count = game3d_world_stream_safe_cell_count(stream);
    for (int32_t i = 0; i < cell_count; ++i) {
        rt_game3d_stream_cell *cell = &stream->cells[i];
        if (!cell->proxy_resident || !cell->proxy_entity)
            continue;
        rt_game3d_entity *proxy_entity = (rt_game3d_entity *)rt_g3d_checked_or_null(
            cell->proxy_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
        void *proxy_root = game3d_entity_node_ref(proxy_entity);
        if (!proxy_root || rt_scene_node3d_child_count(proxy_root) < 1)
            continue;
        void *proxy_node = rt_scene_node3d_get_child(proxy_root, 0);
        void *proxy_mesh = proxy_node ? rt_scene_node3d_get_mesh(proxy_node) : NULL;
        void *proxy_material = proxy_node ? rt_scene_node3d_get_material(proxy_node) : NULL;
        if (!proxy_mesh)
            continue;

        /* Bounding sphere from the proxy mesh (cheap: proxies are <= budget tris). */
        double min_p[3] = {0.0, 0.0, 0.0};
        double max_p[3] = {0.0, 0.0, 0.0};
        int64_t vertex_count = rt_mesh3d_get_vertex_count(proxy_mesh);
        for (int64_t v = 0; v < vertex_count; ++v) {
            double pos[3];
            if (!rt_mesh3d_get_vertex_raw(proxy_mesh, v, pos, NULL, NULL))
                break;
            if (v == 0) {
                min_p[0] = max_p[0] = pos[0];
                min_p[1] = max_p[1] = pos[1];
                min_p[2] = max_p[2] = pos[2];
            } else {
                for (int a = 0; a < 3; ++a) {
                    if (pos[a] < min_p[a])
                        min_p[a] = pos[a];
                    if (pos[a] > max_p[a])
                        max_p[a] = pos[a];
                }
            }
        }
        double center[3] = {
            0.5 * (min_p[0] + max_p[0]), 0.5 * (min_p[1] + max_p[1]), 0.5 * (min_p[2] + max_p[2])};
        double ex = max_p[0] - min_p[0];
        double ey = max_p[1] - min_p[1];
        double ez = max_p[2] - min_p[2];
        double diameter = sqrt(ex * ex + ey * ey + ez * ez);
        if (!isfinite(diameter) || diameter <= 0.0)
            diameter = 1.0;

        /* Isolated capture scene sharing the proxy's mesh/material. */
        void *capture_scene = rt_scene3d_new();
        void *capture_node = rt_scene_node3d_new();
        void *strip =
            rt_pixels_new((int64_t)GAME3D_IMPOSTOR_FRAME_COUNT * GAME3D_IMPOSTOR_FRAME_SIZE,
                          GAME3D_IMPOSTOR_FRAME_SIZE);
        void *target =
            rt_rendertarget3d_new(GAME3D_IMPOSTOR_FRAME_SIZE, GAME3D_IMPOSTOR_FRAME_SIZE);
        void *camera = rt_camera3d_new_ortho(diameter * 1.05, 1.0, 0.05, diameter * 8.0);
        if (capture_scene && capture_node && strip && target && camera) {
            rt_scene_node3d_set_mesh(capture_node, proxy_mesh);
            if (proxy_material)
                rt_scene_node3d_set_material(capture_node, proxy_material);
            rt_scene3d_add(capture_scene, capture_node);
            int frames_ok = 1;
            for (int f = 0; f < GAME3D_IMPOSTOR_FRAME_COUNT && frames_ok; ++f) {
                double yaw = (double)f * (6.283185307179586 / (double)GAME3D_IMPOSTOR_FRAME_COUNT);
                void *eye = rt_vec3_new(center[0] + sin(yaw) * diameter * 2.0,
                                        center[1],
                                        center[2] + cos(yaw) * diameter * 2.0);
                void *look = rt_vec3_new(center[0], center[1], center[2]);
                void *up = rt_vec3_new(0.0, 1.0, 0.0);
                if (eye && look && up) {
                    rt_camera3d_look_at(camera, eye, look, up);
                    rt_canvas3d_set_render_target(world->canvas, target);
                    rt_scene3d_draw(capture_scene, world->canvas, camera);
                    rt_canvas3d_reset_render_target(world->canvas);
                    void *frame_pixels = rt_rendertarget3d_as_pixels(target);
                    if (frame_pixels) {
                        for (int64_t y = 0; y < GAME3D_IMPOSTOR_FRAME_SIZE; ++y)
                            for (int64_t x = 0; x < GAME3D_IMPOSTOR_FRAME_SIZE; ++x)
                                rt_pixels_set(strip,
                                              (int64_t)f * GAME3D_IMPOSTOR_FRAME_SIZE + x,
                                              y,
                                              rt_pixels_get(frame_pixels, x, y));
                        game3d_release_ref(&frame_pixels);
                    } else {
                        frames_ok = 0;
                    }
                } else {
                    frames_ok = 0;
                }
                game3d_release_ref(&eye);
                game3d_release_ref(&look);
                game3d_release_ref(&up);
            }
            if (frames_ok) {
                rt_scene_node3d_set_impostor_frames(
                    proxy_node, distance, strip, GAME3D_IMPOSTOR_FRAME_COUNT);
                generated++;
            }
        }
        game3d_release_ref(&capture_node);
        game3d_release_ref(&capture_scene);
        game3d_release_ref(&strip);
        game3d_release_ref(&target);
        game3d_release_ref(&camera);
    }
    return generated;
}
