//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_config.c
/// @file
/// @brief Implements JSON-backed, dotted-path game configuration lookups.
//
// Purpose: Typed game config loader wrapping JSON parse + JsonPath getters.
//
// Ownership/Lifetime:
//   - A Config owns the parsed JSON root stored inside it; its runtime-object
//     finalizer releases that root.
//   - Successful string queries return a new string reference. A failed string
//     query returns the caller-supplied default pointer unchanged.
//
//===----------------------------------------------------------------------===//

#include "rt_config.h"
#include "rt_bytes.h"
#include "rt_file_ext.h"
#include "rt_json.h"
#include "rt_jsonpath.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Private payload stored in each runtime Config object.
typedef struct {
    void *json_root;
} config_impl;

/// @brief Release one runtime-object reference.
/// @param obj Object reference to release; `NULL` is a no-op.
/// @details Frees the object when the released reference was the last one,
///          allowing any registered finalizer to run through rt_obj_free().
static void config_release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief GC finalizer: release the parsed JSON root when the Config is freed.
/// @param obj Config payload being finalized; `NULL` is accepted.
/// @details Clears the stored pointer after releasing it so repeated invocation
///          cannot release the JSON root twice.
static void config_finalizer(void *obj) {
    config_impl *cfg = (config_impl *)obj;
    if (!cfg || !cfg->json_root)
        return;
    config_release_obj(cfg->json_root);
    cfg->json_root = NULL;
}

/// @brief Validate and cast an opaque Config handle.
/// @param cfg Candidate Config handle; `NULL` is accepted.
/// @param api Trap message used when @p cfg has the wrong runtime class ID.
/// @return The private Config payload when valid; otherwise `NULL`.
/// @details A non-null handle of another runtime class raises a runtime trap
///          before this function returns `NULL`.
static config_impl *checked_config(void *cfg, const char *api) {
    if (!cfg)
        return NULL;
    if (rt_obj_class_id(cfg) != RT_CONFIG_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (config_impl *)cfg;
}

/// @brief Load a JSON config file and return a queryable config handle.
/// @param path Runtime string containing the file path.
/// @return A newly allocated Config reference, or `NULL` when @p path is null,
///         the file does not exist, the file is empty or unreadable, conversion
///         to text yields no content, JSON parsing fails, or allocation fails.
/// @details Reads the file, parses as JSON, and wraps the root node for typed
///          key lookups via JsonPath queries such as `player.speed`. The
///          existence precheck makes a missing file a soft failure instead of
///          invoking the file reader's missing-file trap. Temporary byte and
///          string references are released on every path.
void *rt_config_load(void *path) {
    if (!path)
        return NULL;

    // rt_file_read_bytes now traps on a missing file (hardened I/O path),
    // but Config.Load's contract is "missing config → NULL so the caller
    // can fall back to defaults." Pre-check existence with the non-trapping
    // helper so a missing config stays a soft failure.
    if (!rt_io_file_exists((rt_string)path))
        return NULL;

    void *bytes = rt_file_read_bytes((rt_string)path);
    if (!bytes)
        return NULL;
    if (rt_bytes_len(bytes) == 0) {
        config_release_obj(bytes);
        return NULL;
    }

    rt_string text = rt_bytes_to_str(bytes);
    config_release_obj(bytes);
    if (!text || rt_str_len(text) == 0) {
        if (text)
            rt_string_unref(text);
        return NULL;
    }

    void *root = rt_json_parse((rt_string)text);
    rt_string_unref(text);
    if (!root)
        return NULL;

    config_impl *cfg =
        (config_impl *)rt_obj_new_i64(RT_CONFIG_CLASS_ID, (int64_t)sizeof(config_impl));
    if (!cfg) {
        config_release_obj(root);
        return NULL;
    }
    memset(cfg, 0, sizeof(config_impl));
    rt_obj_set_finalizer(cfg, config_finalizer);
    cfg->json_root = root;
    return cfg;
}

/// @brief Create a config from a JSON string (no file I/O).
/// @param json_str Runtime string containing a complete JSON document.
/// @return A newly allocated Config reference, or `NULL` for a null input,
///         invalid JSON, or allocation failure.
/// @details On success, the Config takes ownership of the parsed root. The
///          input string remains caller-owned.
void *rt_config_from_string(void *json_str) {
    if (!json_str)
        return NULL;
    void *root = rt_json_parse((rt_string)json_str);
    if (!root)
        return NULL;

    config_impl *cfg =
        (config_impl *)rt_obj_new_i64(RT_CONFIG_CLASS_ID, (int64_t)sizeof(config_impl));
    if (!cfg) {
        config_release_obj(root);
        return NULL;
    }
    memset(cfg, 0, sizeof(config_impl));
    rt_obj_set_finalizer(cfg, config_finalizer);
    cfg->json_root = root;
    return cfg;
}

/// @brief Get an integer value at a JsonPath, or default_val if missing.
/// @param cfg Config handle to query.
/// @param path Runtime string containing a dotted JSON path.
/// @param default_val Value returned when the lookup cannot produce an integer.
/// @return The converted integer, or @p default_val when either pointer is
///         null, the Config is invalid or empty, the path is absent, or its
///         value is not integer-convertible.
/// @details A non-null handle of another runtime class raises a runtime trap.
///          The resolved JSON node is not retained.
int64_t rt_config_get_int(void *cfg, void *path, int64_t default_val) {
    if (!cfg || !path)
        return default_val;
    config_impl *c = checked_config(cfg, "Config.GetInt: expected Zanna.Game.Config");
    if (!c)
        return default_val;
    if (!c->json_root)
        return default_val;
    // Return the caller's default when the path is absent OR the present value is
    // not int-convertible (object/array/null, or non-numeric string), rather than
    // the coercing helper's 0 (VDOC-245). The try-variant resolves and releases the
    // node internally, so there is no retained-reference leak (VDOC-236).
    int64_t out = default_val;
    return rt_jsonpath_try_get_int(c->json_root, (rt_string)path, &out) ? out : default_val;
}

/// @brief Get a string value at a JsonPath, or default_val if missing.
/// @param cfg Config handle to query.
/// @param path Runtime string containing a dotted JSON path.
/// @param default_val Pointer returned unchanged when no string can be read.
/// @return A fresh caller-owned runtime string on success; otherwise
///         @p default_val without changing its ownership.
/// @details Null arguments, an empty Config, an absent path, or a value that is
///          not string-convertible select the default. A non-null Config handle
///          of another runtime class raises a trap. The resolved JSON node is
///          released internally.
void *rt_config_get_str(void *cfg, void *path, void *default_val) {
    if (!cfg || !path)
        return default_val;
    config_impl *c = checked_config(cfg, "Config.GetStr: expected Zanna.Game.Config");
    if (!c)
        return default_val;
    if (!c->json_root)
        return default_val;
    // Return the caller's default when the path is absent OR the present value is
    // not string-convertible (object/array/null), rather than the coercing helper's
    // empty string (VDOC-245). On success the try-variant yields a fresh
    // caller-owned string; it resolves/releases the node internally (VDOC-236).
    rt_string out = NULL;
    if (rt_jsonpath_try_get_str(c->json_root, (rt_string)path, &out))
        return out;
    return default_val;
}

/// @brief Get a boolean value at a JsonPath, or default_val if missing.
/// @param cfg Config handle to query.
/// @param path Runtime string containing a dotted JSON path.
/// @param default_val Value returned when the lookup cannot produce an integer.
/// @return `1` for a nonzero integer-convertible value, `0` for a converted
///         zero, or @p default_val when lookup or conversion fails.
/// @details A non-null handle of another runtime class raises a runtime trap.
///          The function returns the supplied default verbatim, so it does not
///          normalize a non-boolean default to zero or one.
int8_t rt_config_get_bool(void *cfg, void *path, int8_t default_val) {
    if (!cfg || !path)
        return default_val;
    config_impl *c = checked_config(cfg, "Config.GetBool: expected Zanna.Game.Config");
    if (!c)
        return default_val;
    if (!c->json_root)
        return default_val;
    // Boolean coercion goes through the same int-convertibility check: absent or a
    // non-int-convertible value yields the caller's default rather than false
    // (VDOC-245); a convertible value is truthy when nonzero.
    int64_t out = 0;
    if (rt_jsonpath_try_get_int(c->json_root, (rt_string)path, &out))
        return out != 0;
    return default_val;
}

/// @brief Check whether a JsonPath key exists in the config.
/// @param cfg Config handle to query.
/// @param path Runtime string containing a dotted JSON path.
/// @return The JsonPath existence result, or `0` for null arguments, an empty
///         Config, or an invalid handle.
/// @details A non-null handle of another runtime class raises a runtime trap.
///          The existence probe does not retain the resolved JSON node.
int8_t rt_config_has(void *cfg, void *path) {
    if (!cfg || !path)
        return 0;
    config_impl *c = checked_config(cfg, "Config.Has: expected Zanna.Game.Config");
    if (!c)
        return 0;
    if (!c->json_root)
        return 0;
    // rt_jsonpath_has is the non-retaining existence check; the prior
    // rt_jsonpath_get probe leaked a retained reference on every hit (VDOC-236).
    return rt_jsonpath_has(c->json_root, (rt_string)path);
}

/// @brief Return the borrowed parsed JSON root of a Config (internal/testing).
/// @param cfg Config handle to inspect.
/// @return The borrowed JSON root, or `NULL` for a null handle or a handle with
///         the wrong runtime class ID.
/// @details Unlike the public typed getters, this inspection helper does not
///          raise a trap for a mismatched class. The returned pointer remains
///          owned by @p cfg and is valid only while that Config is alive.
void *rt_config_json_root(void *cfg) {
    if (!cfg)
        return NULL;
    if (rt_obj_class_id(cfg) != RT_CONFIG_CLASS_ID)
        return NULL;
    return ((config_impl *)cfg)->json_root;
}
