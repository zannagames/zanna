//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_message_bundle.c
// Purpose: Implementation of Zanna.Localization.MessageBundle. Stores the
//          translation table as an rt_map<rt_string, rt_string>, walks the
//          fallback chain on lookup, and expands {name}-style placeholder
//          templates via a small purpose-built interpolator (the template
//          engine at rt_template.c is broader than we need here).
//
// Key invariants:
//   - Fallback chain cycles are rejected on SetFallback with an explicit
//     trap message. Max chain depth 16 is enforced on every lookup.
//   - Plural key resolution looks up "<key>.<category>" then falls through
//     to "<key>.other"; trapping only when neither exists.
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated; GC-managed.
//   - The internal rt_map retains its entries; freeing the bundle releases
//     the map handle which decrements the entries' refcounts.
//
// Links: src/runtime/localization/rt_message_bundle.h (interface),
//        src/runtime/localization/rt_plural_rules.h (category evaluator),
//        src/runtime/collections/rt_map.h (entry storage).
//
//===----------------------------------------------------------------------===//

#include "rt_message_bundle.h"

#include "rt_asset.h"
#include "rt_bytes.h"
#include "rt_collection_ids.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_locale_manager.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_plural_rules.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Forward declarations for optional external helpers
//===----------------------------------------------------------------------===//

extern rt_string rt_io_file_read_all_text(rt_string path);
extern void *rt_json_parse_object(rt_string text);

//===----------------------------------------------------------------------===//
// Instance struct
//===----------------------------------------------------------------------===//

#define RT_MSG_BUNDLE_MAX_DEPTH 16

typedef struct rt_message_bundle {
    void *locale;                 ///< Locale handle; strong ref through GC
    const rt_locale_data_t *data; ///< retained locale data for plural lookup
    void *entries;                ///< rt_map<rt_string, rt_string>
    void *fallback;               ///< optional bundle to consult on miss
} rt_message_bundle_t;

/// @brief Unchecked cast of an opaque handle to the message-bundle struct.
/// @param obj Opaque handle expected to reference an `rt_message_bundle_t`.
/// @return @p obj reinterpreted as a message-bundle pointer, including NULL.
static rt_message_bundle_t *as_bundle(void *obj) {
    return (rt_message_bundle_t *)obj;
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Runtime object handle to release, or NULL for a no-op.
static void release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief True iff @p obj is a runtime Map handle.
/// @param obj Opaque runtime handle to inspect.
/// @return Non-zero when @p obj is a non-NULL Map; otherwise zero.
static int is_map_object(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_MAP_CLASS_ID;
}

/// @brief True iff @p obj is a runtime List handle (and not a string).
/// @param obj Opaque runtime handle to inspect.
/// @return Non-zero when @p obj is a non-NULL List that is not represented by
///         a string handle; otherwise zero.
static int is_list_like_object(void *obj) {
    if (!obj)
        return 0;
    if (rt_string_is_handle(obj))
        return 0;
    int64_t cid = rt_obj_class_id(obj);
    return cid == RT_LIST_CLASS_ID;
}

/// @brief GC finalizer: release the bundle's locale data, locale handle,
///        entries map, and optional fallback bundle, then NULL the fields.
/// @param obj Message-bundle allocation being finalized. NULL is accepted.
static void bundle_finalizer(void *obj) {
    rt_message_bundle_t *bundle = (rt_message_bundle_t *)obj;
    if (!bundle)
        return;
    rt_locale_manager_release_data(bundle->data);
    release_object(bundle->locale);
    release_object(bundle->entries);
    release_object(bundle->fallback);
    bundle->locale = NULL;
    bundle->data = NULL;
    bundle->entries = NULL;
    bundle->fallback = NULL;
}

/// @brief Validate that every value in @p map is a string handle.
/// @details Enumerates an owned key sequence, borrows each key long enough to
///          query the map, and releases the sequence before returning.
/// @param map Map handle to validate. NULL is invalid.
/// @return 1 if all values are strings (or the map is empty), 0 otherwise.
static int validate_message_map(void *map) {
    if (!map)
        return 0;
    void *keys = rt_map_keys(map);
    int ok = 1;
    int64_t n = keys ? rt_seq_len(keys) : 0;
    for (int64_t i = 0; i < n; ++i) {
        rt_string key = (rt_string)rt_seq_get(keys, i);
        void *value = rt_map_get(map, key);
        if (!value || !rt_string_is_handle(value)) {
            ok = 0;
            break;
        }
    }
    release_object(keys);
    return ok;
}

/// @brief Validate that every element of @p list is a string handle.
/// @details Releases the retained element reference returned by `rt_list_get`
///          after inspecting each entry.
/// @param list List handle to validate, or NULL to represent an empty list.
/// @return 1 if all elements are strings (or the list is NULL/empty), else 0.
static int validate_string_list(void *list) {
    if (!list)
        return 1;
    int64_t n = rt_list_len(list);
    for (int64_t i = 0; i < n; ++i) {
        void *value = rt_list_get(list, i);
        int ok = value && rt_string_is_handle(value);
        release_object(value);
        if (!ok)
            return 0;
    }
    return 1;
}

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// @brief Allocate and initialize a GC-managed message bundle.
/// @param locale   Locale handle (retained); supplies the plural data.
/// @param map      Backing entries map, or NULL to create a fresh empty map.
/// @param take_map Non-zero transfers ownership of @p map (no extra retain);
///                 zero retains it so the caller keeps its own reference.
/// @details Traps on allocation failure. Installs @ref bundle_finalizer.
/// @return Newly allocated message-bundle handle, or NULL if allocation fails
///         and the runtime trap hook returns.
static void *bundle_alloc(void *locale, void *map, int take_map) {
    rt_message_bundle_t *bundle =
        (rt_message_bundle_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_message_bundle_t));
    if (!bundle) {
        rt_trap("Zanna.Localization.MessageBundle: allocation failed");
        return NULL;
    }
    bundle->locale = locale;
    if (bundle->locale)
        rt_heap_retain(bundle->locale);
    bundle->data = rt_locale_get_data(locale);
    rt_locale_manager_retain_data(bundle->data);
    bundle->entries = map ? map : rt_map_new();
    if (bundle->entries && !take_map)
        rt_obj_retain_maybe(bundle->entries);
    bundle->fallback = NULL;
    rt_obj_set_finalizer(bundle, bundle_finalizer);
    return bundle;
}

/// @brief Create an empty message bundle bound to the current locale.
/// @details Obtains the locale manager's retained current-locale handle, gives
///          the bundle its own reference, and balances the temporary reference.
/// @return Newly allocated message-bundle handle, or NULL after an allocation
///         failure if the runtime trap hook returns.
void *rt_message_bundle_new(void) {
    void *current = rt_locale_manager_current();
    void *bundle = bundle_alloc(current, NULL, 1);
    release_object(current);
    return bundle;
}

/// @brief Create a message bundle from an existing string-to-string map.
/// @details Validates the map kind and every value before retaining @p map for
///          the new bundle. A NULL map creates an empty backing map.
/// @param locale Locale handle to retain and use for plural selection.
/// @param map Runtime Map whose values must all be strings, or NULL.
/// @return Newly allocated bundle, or NULL after invalid input or allocation
///         failure if the runtime trap hook returns.
void *rt_message_bundle_from_map(void *locale, void *map) {
    if (map && !is_map_object(map)) {
        rt_trap("Zanna.Localization.MessageBundle: FromMap requires Map[String, String]");
        return NULL;
    }
    if (map && !validate_message_map(map)) {
        rt_trap("Zanna.Localization.MessageBundle: map values must be strings");
        return NULL;
    }
    return bundle_alloc(locale, map, 0);
}

/// @brief Load a flat message map from a JSON file.
/// @details Reads @p path as text, parses its root object, validates that every
///          value is a string, and transfers the parsed map to the new bundle.
///          File, parse, schema, and allocation failures raise a runtime trap.
/// @param locale Locale handle to retain and use for plural selection.
/// @param path Runtime string naming the JSON file. NULL is rejected.
/// @return Newly allocated bundle, or NULL on failure if the trap hook returns.
void *rt_message_bundle_load_from_json(void *locale, rt_string path) {
    if (!path) {
        rt_trap("Zanna.Localization.MessageBundle: LoadFromJson requires a path");
        return NULL;
    }
    rt_string text = rt_io_file_read_all_text(path);
    if (!text) {
        rt_trap("Zanna.Localization.MessageBundle: cannot read JSON file");
        return NULL;
    }
    void *map = rt_json_parse_object(text);
    rt_string_unref(text);
    if (!map) {
        rt_trap("Zanna.Localization.MessageBundle: malformed JSON");
        return NULL;
    }
    if (!validate_message_map(map)) {
        release_object(map);
        rt_trap("Zanna.Localization.MessageBundle: JSON values must be strings");
        return NULL;
    }
    return bundle_alloc(locale, map, 1);
}

/// @brief Load a flat message map from a packaged text asset.
/// @details Loads the named asset as bytes, decodes it as a runtime string,
///          parses a JSON root object, validates string values, and transfers
///          the parsed map to the new bundle. Each temporary handle is released.
/// @param locale Locale handle to retain and use for plural selection.
/// @param name Runtime string identifying the packaged asset. NULL is rejected.
/// @return Newly allocated bundle, or NULL on load, decoding, parse, schema, or
///         allocation failure if the runtime trap hook returns.
void *rt_message_bundle_load_from_asset(void *locale, rt_string name) {
    if (!name) {
        rt_trap("Zanna.Localization.MessageBundle: LoadFromAsset requires a name");
        return NULL;
    }
    void *bytes = rt_asset_load_bytes(name);
    if (!bytes) {
        rt_trap("Zanna.Localization.MessageBundle: asset not found");
        return NULL;
    }
    rt_string text = rt_bytes_to_str(bytes);
    release_object(bytes);
    if (!text) {
        rt_trap("Zanna.Localization.MessageBundle: asset is not valid text");
        return NULL;
    }
    void *map = rt_json_parse_object(text);
    rt_string_unref(text);
    if (!map) {
        rt_trap("Zanna.Localization.MessageBundle: malformed JSON asset");
        return NULL;
    }
    if (!validate_message_map(map)) {
        release_object(map);
        rt_trap("Zanna.Localization.MessageBundle: JSON asset values must be strings");
        return NULL;
    }
    return bundle_alloc(locale, map, 1);
}

//===----------------------------------------------------------------------===//
// Property accessors
//===----------------------------------------------------------------------===//

/// @brief Return the locale associated with a message bundle.
/// @param self Message-bundle handle, or NULL.
/// @return Borrowed locale handle, or NULL when @p self is NULL. The caller
///         must not release the returned reference.
void *rt_message_bundle_get_locale(void *self) {
    return self ? as_bundle(self)->locale : NULL;
}

/// @brief Count entries stored directly in a message bundle.
/// @details Entries reachable only through the locale-qualified lookup or
///          fallback-bundle chain are not included.
/// @param self Message-bundle handle, or NULL.
/// @return Direct backing-map entry count, or zero when @p self is NULL.
int64_t rt_message_bundle_get_count(void *self) {
    if (!self)
        return 0;
    extern int64_t rt_map_len(void *);
    return rt_map_len(as_bundle(self)->entries);
}

//===----------------------------------------------------------------------===//
// Lookup with fallback walk
//===----------------------------------------------------------------------===//

/// @brief Look up @p key in @p self, walking up fallbacks. Returns NULL when
///        no bundle in the chain has a matching key. The returned rt_string
///        is retained (caller owns the reference and must unref when done).
/// @param self Bundle whose direct entries map will be queried.
/// @param key Message key to find.
/// @return Retained matching value, or NULL when the arguments are invalid or
///         the bundle has no direct entry for @p key.
static rt_string bundle_lookup_direct(rt_message_bundle_t *self, rt_string key) {
    if (!self || !key || !rt_map_has(self->entries, key))
        return NULL;
    return rt_map_get_str(self->entries, key);
}

/// @brief Look up @p key prefixed by each locale fallback tag ("tag:key").
/// @details Tries every entry of the locale's fallback chain in order, building
///          the qualified key on the stack (heap only for >255-byte keys).
/// @param self Bundle whose direct entries map will be queried.
/// @param key Unqualified message key to append after each locale tag.
/// @return A retained string the caller owns, or NULL if no qualified key hit.
static rt_string bundle_lookup_locale_qualified(rt_message_bundle_t *self, rt_string key) {
    if (!self || !self->locale || !key)
        return NULL;
    const char *key_cs = rt_string_cstr(key);
    int64_t key_len = rt_str_len(key);
    if (!key_cs || key_len <= 0)
        return NULL;

    void *fallbacks = rt_locale_fallbacks(self->locale);
    if (!fallbacks)
        return NULL;
    int64_t n = rt_list_len(fallbacks);
    for (int64_t i = 0; i < n; ++i) {
        void *loc = rt_list_get(fallbacks, i);
        rt_string tag = rt_locale_tag(loc);
        const char *tag_cs = rt_string_cstr(tag);
        int64_t tag_len = rt_str_len(tag);
        rt_string qkey = NULL;
        if (tag_cs && tag_len > 0) {
            size_t needed = (size_t)tag_len + 1 + (size_t)key_len;
            char stack[256];
            char *buf = needed + 1 <= sizeof(stack) ? stack : (char *)malloc(needed + 1);
            if (buf) {
                memcpy(buf, tag_cs, (size_t)tag_len);
                buf[tag_len] = ':';
                memcpy(buf + tag_len + 1, key_cs, (size_t)key_len);
                buf[needed] = '\0';
                qkey = rt_string_from_bytes(buf, needed);
                if (buf != stack)
                    free(buf);
            }
        }
        rt_string_unref(tag);
        release_object(loc);
        if (qkey) {
            rt_string v = bundle_lookup_direct(self, qkey);
            rt_string_unref(qkey);
            if (v) {
                release_object(fallbacks);
                return v;
            }
        }
    }
    release_object(fallbacks);
    return NULL;
}

/// @brief Resolve @p key: direct hit, then locale-qualified, then recurse into
///        the fallback bundle (bounded by RT_MSG_BUNDLE_MAX_DEPTH to stop
///        cyclic fallback chains from recursing without limit).
/// @param self Bundle at the current point in the fallback walk.
/// @param key Message key to resolve.
/// @param depth Zero-based fallback recursion depth.
/// @return A retained string the caller owns, or NULL if unresolved.
static rt_string bundle_lookup(rt_message_bundle_t *self, rt_string key, int depth) {
    if (!self || depth >= RT_MSG_BUNDLE_MAX_DEPTH)
        return NULL;
    rt_string v = bundle_lookup_direct(self, key);
    if (v)
        return v;
    v = bundle_lookup_locale_qualified(self, key);
    if (v)
        return v;
    if (self->fallback)
        return bundle_lookup(as_bundle(self->fallback), key, depth + 1);
    return NULL;
}

/// @brief Resolve a required message key through locale and bundle fallbacks.
/// @details A missing bundle, NULL key, or unresolved key raises a runtime
///          trap. If the trap hook returns, an owned empty string is supplied.
/// @param self Message-bundle handle.
/// @param key Non-NULL message key.
/// @return Owned resolved string, or an owned empty string after a trapped error.
rt_string rt_message_bundle_get(void *self, rt_string key) {
    if (!self || !key) {
        rt_trap("Zanna.Localization.MessageBundle: Get requires a non-null key");
        return rt_string_from_bytes("", 0);
    }
    rt_string r = bundle_lookup(as_bundle(self), key, 0);
    if (!r) {
        rt_trap("Zanna.Localization.MessageBundle: missing key (no fallback found)");
        return rt_string_from_bytes("", 0);
    }
    return r;
}

/// @brief Resolve a message key without trapping when it is absent.
/// @details This legacy API represents both an absent key and a present empty
///          translation as an owned empty string. Use the Option API to
///          distinguish those cases.
/// @param self Message-bundle handle, or NULL.
/// @param key Message key, or NULL.
/// @return Owned resolved value, or an owned empty string when unresolved.
rt_string rt_message_bundle_try_get(void *self, rt_string key) {
    if (!self || !key)
        return rt_string_from_bytes("", 0);
    rt_string r = bundle_lookup(as_bundle(self), key, 0);
    return r ? r : rt_string_from_bytes("", 0);
}

/// @brief Resolve a message key or return a retained default string.
/// @details This modern fallback helper keeps the legacy @ref rt_message_bundle_try_get
///          contract intact while avoiding its empty-string sentinel. A resolved
///          bundle entry is returned with the retained reference supplied by
///          @ref bundle_lookup. If no entry resolves, @p default_value is
///          retained and returned; NULL defaults produce an allocated empty
///          string so callers always receive an owned rt_string.
/// @param self MessageBundle handle; NULL skips directly to the default.
/// @param key Lookup key; NULL skips directly to the default.
/// @param default_value Caller-provided fallback string, or NULL for empty.
/// @return Retained string containing the resolved message or fallback value.
rt_string rt_message_bundle_get_or(void *self, rt_string key, rt_string default_value) {
    if (self && key) {
        rt_string r = bundle_lookup(as_bundle(self), key, 0);
        if (r)
            return r;
    }
    if (default_value)
        return rt_string_ref(default_value);
    return rt_string_from_bytes("", 0);
}

/// @brief Resolve a message key as an Option.
/// @details Returns `Some(str)` for any resolved value, including an explicitly
///          empty translation string, and `None` when no bundle in the fallback
///          chain contains the key. The retained string returned by
///          @ref bundle_lookup is released after @ref rt_option_some_str has
///          retained its own copy for the Option payload.
/// @param self MessageBundle handle.
/// @param key Lookup key.
/// @return Opaque Zanna.Option containing a string, or None.
void *rt_message_bundle_try_get_option(void *self, rt_string key) {
    if (!self || !key)
        return rt_option_none();
    rt_string r = bundle_lookup(as_bundle(self), key, 0);
    if (!r)
        return rt_option_none();
    void *option = rt_option_some_str(r);
    rt_string_unref(r);
    return option;
}

/// @brief Test whether a key resolves through locale and bundle fallbacks.
/// @details Balances the retained value returned by the internal lookup.
/// @param self Message-bundle handle, or NULL.
/// @param key Message key, or NULL.
/// @return 1 when a value resolves, including an empty value; otherwise 0.
int8_t rt_message_bundle_has(void *self, rt_string key) {
    if (!self || !key)
        return 0;
    rt_string r = bundle_lookup(as_bundle(self), key, 0);
    if (r) {
        rt_string_unref(r); // bundle_lookup now retains; unref to balance
        return 1;
    }
    return 0;
}

//===----------------------------------------------------------------------===//
// Placeholder interpolation
//===----------------------------------------------------------------------===//

/// @brief Append bytes to a localization string builder with trap-on-failure.
/// @details Localization formatting used to ignore @ref rt_sb_append_bytes
///          status codes, which could return a truncated result after OOM or
///          overflow. This helper centralizes checked appends and leaves the
///          builder valid for cleanup when a recoverable trap hook returns.
/// @param sb Destination builder.
/// @param bytes Bytes to append; may be NULL only when @p len is zero.
/// @param len Number of bytes to append.
/// @return Non-zero on success; zero after reporting an append failure.
static int bundle_append_bytes_checked(rt_string_builder *sb, const char *bytes, size_t len) {
    rt_sb_status_t status = rt_sb_append_bytes(sb, bytes, len);
    if (status == RT_SB_OK)
        return 1;
    rt_trap(status == RT_SB_ERROR_OVERFLOW
                ? "Zanna.Localization.MessageBundle: formatted message overflow"
                : "Zanna.Localization.MessageBundle: formatted message allocation failed");
    return 0;
}

/// @brief Emit the value for placeholder @p name from @p vars into @p sb.
/// @details rt_map_get_str returns an owned reference. This helper releases it
///          after appending so interpolation does not leak one string per
///          substituted token.
/// @param sb Initialized destination string builder.
/// @param name Placeholder-name bytes without surrounding braces.
/// @param name_len Length of @p name in bytes.
/// @param vars String-to-string substitution map, or NULL.
/// @return 1 when a value was found and appended, 0 when absent, or -1 on
///         append failure.
static int append_value(rt_string_builder *sb, const char *name, size_t name_len, void *vars) {
    if (!vars)
        return 0;
    if (!is_map_object(vars))
        return 0;
    rt_string key = rt_string_from_bytes(name, name_len);
    int found = 0;
    // rt_map_has returns 0 when key is absent; without this guard,
    // rt_map_get_str would return a fresh empty string that we'd leak.
    if (rt_map_has(vars, key)) {
        rt_string value = rt_map_get_str(vars, key);
        if (value) {
            found = 1;
            const char *cs = rt_string_cstr(value);
            int64_t vlen = rt_str_len(value);
            if (cs && vlen > 0 && !bundle_append_bytes_checked(sb, cs, (size_t)vlen))
                found = -1;
            rt_string_unref(value);
        }
    }
    rt_string_unref(key);
    return found;
}

/// @brief Named-placeholder substitution: scans for `{name}` runs.
/// @details Replaces names present in @p vars, preserves missing or malformed
///          placeholders literally, and converts doubled braces `{{` / `}}`
///          to single literal braces. Builder failures trap and return an owned
///          empty string if the trap hook resumes execution.
/// @param tmpl Template string to scan, or NULL.
/// @param vars Validated string-to-string variable map, or NULL.
/// @return Newly allocated interpolated string.
static rt_string interp_named(rt_string tmpl, void *vars) {
    if (!tmpl)
        return rt_string_from_bytes("", 0);
    const char *cs = rt_string_cstr(tmpl);
    int64_t len = rt_str_len(tmpl);
    if (!cs || len <= 0)
        return rt_string_from_bytes("", 0);

    rt_string_builder sb;
    rt_sb_init(&sb);
    for (int64_t i = 0; i < len;) {
        if (cs[i] == '{' && i + 1 < len && cs[i + 1] == '{') {
            if (!bundle_append_bytes_checked(&sb, "{", 1))
                goto interp_error;
            i += 2;
            continue;
        }
        if (cs[i] == '}' && i + 1 < len && cs[i + 1] == '}') {
            if (!bundle_append_bytes_checked(&sb, "}", 1))
                goto interp_error;
            i += 2;
            continue;
        }
        if (cs[i] == '{') {
            // Scan for matching '}'.
            int64_t j = i + 1;
            while (j < len && cs[j] != '}')
                ++j;
            if (j < len && j > i + 1) {
                int appended = append_value(&sb, cs + i + 1, (size_t)(j - i - 1), vars);
                if (appended < 0)
                    goto interp_error;
                if (!appended && !bundle_append_bytes_checked(&sb, cs + i, (size_t)(j - i + 1)))
                    goto interp_error;
                i = j + 1;
                continue;
            }
        }
        if (!bundle_append_bytes_checked(&sb, cs + i, 1))
            goto interp_error;
        ++i;
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;

interp_error:
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}

/// @brief Positional substitution: `{N}` where N parses as an integer index.
/// @details In-range indexes append their string list element. Out-of-range
///          indexes are preserved literally; an in-range NULL element emits
///          no bytes. Doubled braces emit one literal brace. Index overflow or
///          non-decimal brace contents are copied without substitution.
/// @param tmpl Template string to scan, or NULL.
/// @param values_list Validated list of string values, or NULL.
/// @return Newly allocated interpolated string.
static rt_string interp_positional(rt_string tmpl, void *values_list) {
    if (!tmpl)
        return rt_string_from_bytes("", 0);
    const char *cs = rt_string_cstr(tmpl);
    int64_t len = rt_str_len(tmpl);
    if (!cs || len <= 0)
        return rt_string_from_bytes("", 0);
    int64_t list_len = values_list ? rt_list_len(values_list) : 0;

    rt_string_builder sb;
    rt_sb_init(&sb);
    for (int64_t i = 0; i < len;) {
        if (cs[i] == '{' && i + 1 < len && cs[i + 1] == '{') {
            if (!bundle_append_bytes_checked(&sb, "{", 1))
                goto interp_error;
            i += 2;
            continue;
        }
        if (cs[i] == '}' && i + 1 < len && cs[i + 1] == '}') {
            if (!bundle_append_bytes_checked(&sb, "}", 1))
                goto interp_error;
            i += 2;
            continue;
        }
        if (cs[i] == '{') {
            int64_t j = i + 1;
            int64_t idx = 0;
            int valid = 0;
            int overflow = 0;
            while (j < len && cs[j] >= '0' && cs[j] <= '9') {
                int digit = cs[j] - '0';
                if (idx > (INT64_MAX - digit) / 10)
                    overflow = 1;
                else
                    idx = idx * 10 + digit;
                valid = 1;
                ++j;
            }
            if (valid && !overflow && j < len && cs[j] == '}') {
                if (values_list && idx >= 0 && idx < list_len) {
                    rt_string v = (rt_string)rt_list_get(values_list, idx);
                    if (v) {
                        const char *vcs = rt_string_cstr(v);
                        int64_t vlen = rt_str_len(v);
                        if (vcs && vlen > 0 &&
                            !bundle_append_bytes_checked(&sb, vcs, (size_t)vlen)) {
                            rt_string_unref(v);
                            goto interp_error;
                        }
                        rt_string_unref(v);
                    }
                } else {
                    if (!bundle_append_bytes_checked(&sb, cs + i, (size_t)(j - i + 1)))
                        goto interp_error;
                }
                i = j + 1;
                continue;
            }
        }
        if (!bundle_append_bytes_checked(&sb, cs + i, 1))
            goto interp_error;
        ++i;
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;

interp_error:
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}

/// @brief Resolve and interpolate a message with named variables.
/// @details Validates that @p vars is a Map containing only string values,
///          resolves @p key with the required-key API, and preserves named
///          placeholders that have no entry in the map.
/// @param self Message-bundle handle.
/// @param key Message key to resolve.
/// @param vars String-to-string variable map, or NULL.
/// @return Owned interpolated string, or an owned empty string after a trapped
///         validation, lookup, or builder failure.
rt_string rt_message_bundle_format(void *self, rt_string key, void *vars) {
    if (vars && !is_map_object(vars)) {
        rt_trap("Zanna.Localization.MessageBundle: Format vars must be a Map[String, String]");
        return rt_string_from_bytes("", 0);
    }
    if (vars && !validate_message_map(vars)) {
        rt_trap("Zanna.Localization.MessageBundle: Format vars values must be strings");
        return rt_string_from_bytes("", 0);
    }
    rt_string tmpl = rt_message_bundle_get(self, key);
    rt_string r = interp_named(tmpl, vars);
    rt_string_unref(tmpl);
    return r;
}

/// @brief Resolve and interpolate a message with positional values.
/// @details Validates that @p values is a List containing only strings before
///          resolving the required message template.
/// @param self Message-bundle handle.
/// @param key Message key to resolve.
/// @param values List of positional string values, or NULL.
/// @return Owned interpolated string, or an owned empty string after a trapped
///         validation, lookup, or builder failure.
rt_string rt_message_bundle_format_with(void *self, rt_string key, void *values) {
    if (values && !is_list_like_object(values)) {
        rt_trap("Zanna.Localization.MessageBundle: FormatWith values must be a List[String]");
        return rt_string_from_bytes("", 0);
    }
    if (values && !validate_string_list(values)) {
        rt_trap("Zanna.Localization.MessageBundle: FormatWith values must contain only strings");
        return rt_string_from_bytes("", 0);
    }
    rt_string tmpl = rt_message_bundle_get(self, key);
    rt_string r = interp_positional(tmpl, values);
    rt_string_unref(tmpl);
    return r;
}

//===----------------------------------------------------------------------===//
// Plural
//===----------------------------------------------------------------------===//

/// @brief Render an ASCII digit string with the locale's digit glyphs.
/// @details Walks the locale's 10-code-point digit string (already validated
///          at load time) and substitutes each ASCII digit; the '-' sign and
///          any non-digit bytes pass through unchanged (VDOC-078). The original
///          ASCII form is returned if digit data is absent, Latin, malformed,
///          or cannot be appended.
/// @param data Borrowed locale data containing the ten digit code points.
/// @param num ASCII number bytes to localize.
/// @param len Length of @p num in bytes.
/// @return Newly allocated localized string, or a copy of the original bytes
///         when localization cannot be completed.
static rt_string bundle_localize_digits(const rt_locale_data_t *data, const char *num, size_t len) {
    const char *digits = data ? data->numbers.digits : NULL;
    if (!digits || strcmp(digits, "0123456789") == 0)
        return rt_string_from_bytes(num, len);

    const char *ptr[10];
    size_t dlen[10];
    const char *p = digits;
    for (int i = 0; i < 10; ++i) {
        size_t n_cp = 1;
        unsigned char lead = (unsigned char)*p;
        if (!lead)
            return rt_string_from_bytes(num, len);
        if (lead >= 0xF0)
            n_cp = 4;
        else if (lead >= 0xE0)
            n_cp = 3;
        else if (lead >= 0xC0)
            n_cp = 2;
        ptr[i] = p;
        dlen[i] = n_cp;
        p += n_cp;
    }

    rt_string_builder sb;
    rt_sb_init(&sb);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)num[i];
        rt_sb_status_t st;
        if (c >= '0' && c <= '9')
            st = rt_sb_append_bytes(&sb, ptr[c - '0'], dlen[c - '0']);
        else
            st = rt_sb_append_bytes(&sb, num + i, 1);
        if (st != RT_SB_OK) {
            rt_sb_free(&sb);
            return rt_string_from_bytes(num, len);
        }
    }
    rt_string out = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return out;
}

/// @brief Select, resolve, and interpolate a cardinal plural message.
/// @details Selects the category for @p n, tries `<key>.<category>` and then
///          `<key>.other` through the normal fallback walk, clones @p vars, and
///          inserts a locale-digit rendering of @p n under the `n` key. The
///          caller's variables map is never mutated.
/// @param self Message-bundle handle.
/// @param key Base plural-message key.
/// @param n Signed integer used for category selection and `{n}` substitution.
/// @param vars String-to-string variables map to clone, or NULL.
/// @return Owned interpolated plural message, or an owned empty string after a
///         trapped validation, allocation, lookup, or formatting failure.
rt_string rt_message_bundle_plural(void *self, rt_string key, int64_t n, void *vars) {
    if (!self || !key) {
        rt_trap("Zanna.Localization.MessageBundle: Plural requires a non-null key");
        return rt_string_from_bytes("", 0);
    }
    rt_message_bundle_t *bundle = as_bundle(self);
    if (vars && !is_map_object(vars)) {
        rt_trap("Zanna.Localization.MessageBundle: Plural vars must be a Map[String, String]");
        return rt_string_from_bytes("", 0);
    }
    if (vars && !validate_message_map(vars)) {
        rt_trap("Zanna.Localization.MessageBundle: Plural vars values must be strings");
        return rt_string_from_bytes("", 0);
    }
    rt_plural_category_t cat = rt_plural_rules_select_cardinal_int(bundle->data, n);
    const char *cat_name = rt_plural_rules_category_name(cat);

    // Build "<key>.<category>".
    const char *key_cs = rt_string_cstr(key);
    int64_t key_len = rt_str_len(key);
    if (!key_cs || key_len <= 0) {
        rt_trap("Zanna.Localization.MessageBundle: Plural key is empty");
        return rt_string_from_bytes("", 0);
    }
    size_t cat_len = strlen(cat_name);
    char stack_buf[256];
    size_t needed = (size_t)key_len + 1 + cat_len;
    char *buf = needed + 1 <= sizeof(stack_buf) ? stack_buf : (char *)malloc(needed + 1);
    if (!buf) {
        rt_trap("Zanna.Localization.MessageBundle: plural key allocation failed");
        return rt_string_from_bytes("", 0);
    }
    memcpy(buf, key_cs, (size_t)key_len);
    buf[key_len] = '.';
    memcpy(buf + key_len + 1, cat_name, cat_len);
    buf[needed] = '\0';
    rt_string full_key = rt_string_from_bytes(buf, needed);

    rt_string tmpl = bundle_lookup(bundle, full_key, 0);
    rt_string_unref(full_key);

    // Fall through to "<key>.other" if the category-specific key is absent.
    if (!tmpl) {
        size_t other_needed = (size_t)key_len + 1 + 5; // "other"
        char other_stack[256];
        char *other_buf = other_needed + 1 <= sizeof(other_stack)
                              ? other_stack
                              : (char *)malloc(other_needed + 1);
        if (!other_buf) {
            if (buf != stack_buf)
                free(buf);
            rt_trap("Zanna.Localization.MessageBundle: plural fallback allocation failed");
            return rt_string_from_bytes("", 0);
        }
        memcpy(other_buf, key_cs, (size_t)key_len);
        other_buf[key_len] = '.';
        memcpy(other_buf + key_len + 1, "other", 5);
        other_buf[other_needed] = '\0';
        rt_string other_key = rt_string_from_bytes(other_buf, other_needed);
        tmpl = bundle_lookup(bundle, other_key, 0);
        rt_string_unref(other_key);
        if (other_buf != other_stack)
            free(other_buf);
    }
    if (buf != stack_buf)
        free(buf);

    if (!tmpl) {
        rt_trap("Zanna.Localization.MessageBundle: missing plural key (no 'other' form)");
        return rt_string_from_bytes("", 0);
    }

    // Seed `{n}` into the vars map so it's available to the substitutor.
    // Digits are rendered with the bundle locale's digit set so Plural
    // agrees with the other formatters bound to the same locale (VDOC-078).
    void *effective_vars = vars ? rt_map_clone(vars) : rt_map_new();
    char num_buf[32];
    int nlen = snprintf(num_buf, sizeof(num_buf), "%lld", (long long)n);
    if (nlen > 0) {
        rt_string n_key = rt_string_from_bytes("n", 1);
        rt_string n_val = bundle_localize_digits(bundle->data, num_buf, (size_t)nlen);
        rt_map_set_str(effective_vars, n_key, n_val);
        rt_string_unref(n_key);
        rt_string_unref(n_val);
    }

    rt_string out = interp_named(tmpl, effective_vars);
    release_object(effective_vars);
    rt_string_unref(tmpl);
    return out;
}

//===----------------------------------------------------------------------===//
// Fallback chain
//===----------------------------------------------------------------------===//

/// @brief Replace the bundle consulted after local and locale-qualified misses.
/// @details Walks the complete proposed chain with cycle detection before
///          retaining @p fallback. On success, releases the previous fallback.
///          Reassigning the existing fallback is a no-op.
/// @param self Message bundle to update, or NULL.
/// @param fallback Message bundle to retain as the fallback, or NULL to clear it.
/// @return @p self on success, or NULL when @p self is NULL or the proposed
///         chain contains a cycle. Cycle failures raise a runtime trap.
void *rt_message_bundle_set_fallback(void *self, void *fallback) {
    if (!self)
        return NULL;
    rt_message_bundle_t *bundle = as_bundle(self);
    // Cycle detection: walk the WHOLE proposed chain (no depth cutoff — a
    // cycle beyond a fixed cap would be retained forever, VDOC-079). Floyd's
    // two-pointer walk also terminates if the chain somehow already cycles.
    void *slow = fallback;
    void *fast = fallback;
    while (fast) {
        if (slow == self || fast == self) {
            rt_trap("Zanna.Localization.MessageBundle: Fallback would create a cycle");
            return NULL;
        }
        fast = as_bundle(fast)->fallback;
        if (fast == self) {
            rt_trap("Zanna.Localization.MessageBundle: Fallback would create a cycle");
            return NULL;
        }
        if (fast)
            fast = as_bundle(fast)->fallback;
        slow = as_bundle(slow)->fallback;
        if (fast && fast == slow) {
            rt_trap("Zanna.Localization.MessageBundle: Fallback chain already contains a cycle");
            return NULL;
        }
    }
    if (bundle->fallback == fallback)
        return self;
    if (fallback)
        rt_obj_retain_maybe(fallback);
    void *old = bundle->fallback;
    bundle->fallback = fallback;
    release_object(old);
    return self;
}

/// @brief Copy the keys stored directly in a bundle into a runtime List.
/// @details Does not include keys available from locale qualification or
///          fallback bundles. Converts the map's temporary key sequence into a
///          fresh list and releases that sequence.
/// @param self Message-bundle handle, or NULL.
/// @return Newly allocated List of direct keys; an empty List for NULL @p self.
void *rt_message_bundle_keys(void *self) {
    if (!self)
        return rt_list_new();
    // rt_map_keys returns a Seq; the plan specifies Keys() as a List so
    // callers can use the same rt_list_* API as the other localization
    // methods. Walk the Seq and copy keys into a fresh List.
    extern int64_t rt_seq_len(void *);
    extern void *rt_seq_get(void *, int64_t);
    void *seq = rt_map_keys(as_bundle(self)->entries);
    void *list = rt_list_new();
    if (!seq)
        return list;
    int64_t n = rt_seq_len(seq);
    for (int64_t i = 0; i < n; ++i) {
        rt_list_push(list, rt_seq_get(seq, i));
    }
    release_object(seq);
    return list;
}
