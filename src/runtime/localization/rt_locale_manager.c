//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_manager.c
// Purpose: Implementation of Zanna.Localization.LocaleManager — the process
//          global registry of locale-data records plus the current/system
//          locale state. Supports the baked en-US locale, system-detection
//          bootstrap, JSON/ZPAK locale loading, and filesystem search paths.
//
// Key invariants:
//   - A single process-global rwlock guards all mutating access.
//   - First-touch lazy init double-checks a volatile flag to avoid paying
//     the lock cost on every call.
//   - Baked records (arena == NULL) are recorded by pointer; their tags are
//     borrowed from the record and never freed.
//   - The current pointer is always non-NULL after init (invariant locale
//     fallback when detection fails).
//
// Ownership/Lifetime:
//   - The registry owns the `tags` array and its heap entries; baked tag
//     storage is borrowed from the baked record.
//   - current/system are Locale handles held as strong references via the
//     OOP retain path; they survive registry reset.
//
// Links: src/runtime/localization/rt_locale_manager.h (interface),
//        src/runtime/localization/rt_locale.h (handle type),
//        src/runtime/localization/rt_locale_platform.h (system detect),
//        src/runtime/threads/rt_threads.h (rt_rwlock API).
//
//===----------------------------------------------------------------------===//

#include "rt_locale_manager.h"

#include "rt_asset.h"
#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_file_ext.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_json.h"
#include "rt_list.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_locale_manager_internal.h"
#include "rt_locale_platform.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_threads.h"
#include "rt_trap.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#define LOC_PATH_SEP ";"
#else
#define LOC_PATH_SEP ":"
#endif

//===----------------------------------------------------------------------===//
// Registry state (all access serialized by g_lock)
//===----------------------------------------------------------------------===//

typedef struct loc_registry_entry {
    const char *tag;              ///< borrowed when baked; owned copy when loaded
    const rt_locale_data_t *data; ///< borrowed; arena-owned for loaded records
    int is_baked;                 ///< 1 for en-US; 0 for JSON/ZPAK (freed on unload)
} loc_registry_entry_t;

static struct {
    void *lock;
    int initialized;
    loc_registry_entry_t *entries;
    size_t count;
    size_t capacity;

    void *current; ///< Locale handle; strong ref
    void *system;  ///< Locale handle; strong ref

    char **search_paths;
    size_t search_count;
    size_t search_capacity;
} g_mgr;

extern int rt_locale_internal_parse_into(const char *input, size_t input_len, rt_locale_t *out);
extern void rt_locale_internal_finalizer(void *obj);

static int loc_register_entry_locked(const rt_locale_data_t *data, int is_baked);

/// @brief Return the current atomic reference count for a locale-data record.
/// @details Returns 0 for NULL or baked records (arena == NULL); those are immortal.
/// @param data Locale-data record to query; may be NULL.
/// @return The current `formatter_refs` counter value, or 0 for baked/NULL records.
static int64_t loc_data_ref_count(const rt_locale_data_t *data) {
    if (!data || data->arena == NULL)
        return 0;
    const int64_t *counter = (const int64_t *)(const void *)&data->formatter_refs;
    return __atomic_load_n(counter, __ATOMIC_ACQUIRE);
}

//===----------------------------------------------------------------------===//
// Arena + JSON schema helpers
//===----------------------------------------------------------------------===//

/// @brief Allocate a new arena for locale-data JSON loading.
/// @details The arena is a linked-list of individual allocations that can all
///          be freed at once via `loc_arena_free`. Traps on OOM.
/// @return Pointer to a new, empty arena; never NULL.
static loc_arena_t *loc_arena_new(void) {
    loc_arena_t *arena = (loc_arena_t *)calloc(1, sizeof(*arena));
    if (!arena)
        rt_trap("Zanna.Localization.LocaleManager: locale arena allocation failed");
    return arena;
}

/// @brief Allocate @p size zero-initialised bytes from the arena and track them.
/// @details Each allocation appends a sentinel node to the arena's linked list so
///          `loc_arena_free` can walk and release everything in one pass.
///          Traps on OOM; returns NULL for NULL @p arena or zero @p size.
/// @param arena Arena to allocate from; may be NULL (returns NULL).
/// @param size  Number of bytes to allocate; 0 returns NULL without trapping.
/// @return Pointer to zeroed memory, or NULL on invalid input; traps on OOM.
void *loc_arena_alloc(loc_arena_t *arena, size_t size) {
    if (!arena || size == 0)
        return NULL;
    void *ptr = calloc(1, size);
    loc_arena_alloc_t *node = (loc_arena_alloc_t *)malloc(sizeof(*node));
    if (!ptr || !node) {
        free(ptr);
        free(node);
        rt_trap("Zanna.Localization.LocaleManager: locale arena allocation failed");
        return NULL;
    }
    node->ptr = ptr;
    node->next = arena->allocs;
    arena->allocs = node;
    return ptr;
}

/// @brief Duplicate at most @p len bytes of @p s into the arena.
/// @details NULL @p s is treated as an empty string. Strings longer than 256 bytes
///          are rejected (returns NULL) to guard against malformed locale JSON.
/// @param arena Arena to allocate from.
/// @param s     Source string bytes; NULL is treated as "".
/// @param len   Number of bytes to copy (must be ≤ 256).
/// @return Arena-owned NUL-terminated copy, or NULL if @p len > 256.
static char *loc_arena_strndup(loc_arena_t *arena, const char *s, size_t len) {
    if (!s)
        s = "";
    if (len > 256)
        return NULL;
    char *copy = (char *)loc_arena_alloc(arena, len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Duplicate the full NUL-terminated string @p s into the arena.
/// @details Convenience wrapper around `loc_arena_strndup` for known-valid strings.
/// @param arena Arena to allocate from.
/// @param s     NUL-terminated source string; NULL is treated as "".
/// @return Arena-owned NUL-terminated copy of @p s.
static char *loc_arena_strdup(loc_arena_t *arena, const char *s) {
    return loc_arena_strndup(arena, s ? s : "", strlen(s ? s : ""));
}

/// @brief Free all allocations in the arena and the arena struct itself.
/// @details Walks the linked list of `loc_arena_alloc_t` nodes, freeing each
///          tracked allocation and its sentinel. Safe to call with NULL.
/// @param arena Arena to destroy; may be NULL (no-op).
static void loc_arena_free(loc_arena_t *arena) {
    if (!arena)
        return;
    loc_arena_alloc_t *node = arena->allocs;
    while (node) {
        loc_arena_alloc_t *next = node->next;
        free(node->ptr);
        free(node);
        node = next;
    }
    free(arena);
}

/// @brief Release all memory for a loaded (non-baked) locale-data record.
/// @details No-op for NULL or baked records (arena == NULL). For loaded records,
///          frees the arena, which releases all strings, arrays, and plural-rule
///          nodes in a single pass.
/// @param data Locale-data record to free; may be NULL or baked (both no-ops).
static void loc_free_loaded_data(const rt_locale_data_t *data) {
    if (!data || !data->arena)
        return;
    loc_arena_free((loc_arena_t *)data->arena);
}

/// @brief Decrement the GC refcount on a runtime object and free it if it hits zero.
/// @param obj GC-tracked object; NULL is safe.
static void loc_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Conditionally trap with @p msg, then return 0.
/// @details Provides a uniform "fail path" for JSON schema validation: either
///          traps (for the Load* strict path) or returns 0 silently (for TryLoad*).
/// @param trap_on_error Non-zero to trap immediately; 0 to return silently.
/// @param msg           Trap message passed to `rt_trap`.
/// @return Always 0.
static int loc_fail(int trap_on_error, const char *msg) {
    if (trap_on_error)
        rt_trap(msg);
    return 0;
}

/// @brief Return non-zero if @p obj is a runtime string handle.
/// @param obj Candidate runtime object; may be NULL.
/// @return 1 for a valid runtime string, otherwise 0.
static int loc_is_string(void *obj) {
    return obj && rt_string_is_handle(obj);
}

/// @brief Return non-zero if @p obj is a runtime map (JSON object) handle.
/// @param obj Candidate runtime object; may be NULL.
/// @return 1 when the object's class identifier is @ref RT_MAP_CLASS_ID, otherwise 0.
static int loc_is_map(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_MAP_CLASS_ID;
}

/// @brief Return non-zero if @p obj is a runtime seq (JSON array) handle.
/// @param obj Candidate runtime object; may be NULL.
/// @return 1 when the object's class identifier is @ref RT_SEQ_CLASS_ID, otherwise 0.
static int loc_is_seq(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_SEQ_CLASS_ID;
}

/// @brief Decrement the GC refcount on a Locale handle and free it if it hits zero.
/// @param obj Locale handle; NULL is safe.
static void loc_release_handle(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Look up a key in a JSON map object, returning the value or NULL.
/// @param map Runtime map handle (JSON object); NULL or non-map returns NULL.
/// @param key NUL-terminated key string.
/// @return Runtime value handle for the key, or NULL if absent/invalid.
static void *json_get(void *map, const char *key) {
    if (!loc_is_map(map) || !key)
        return NULL;
    rt_string k = rt_string_from_bytes(key, strlen(key));
    void *v = rt_map_get(map, k);
    rt_string_unref(k);
    return v;
}

/// @brief Return the raw C-string pointer for a runtime string object.
/// @details Sets @p *ok to 1 on success, 0 when @p obj is not a string.
///          The returned pointer is valid only as long as the string object lives.
/// @param obj Runtime object to check; non-string returns NULL.
/// @param ok  Out-parameter set to 1 on success, 0 on type mismatch; may be NULL.
/// @return Pointer to the NUL-terminated string data, or NULL on type mismatch.
static const char *json_string_cstr(void *obj, int *ok) {
    if (ok)
        *ok = 0;
    if (!loc_is_string(obj))
        return NULL;
    const char *s = rt_string_cstr((rt_string)obj);
    if (!s)
        return NULL;
    // Locale JSON values are validated and consumed as C strings downstream;
    // an embedded NUL (e.g. "zz\u0000-garbage") would silently truncate and
    // let hidden content evade tag/digit/pattern checks (VDOC-068).
    if (strlen(s) != (size_t)rt_str_len((rt_string)obj))
        return NULL;
    if (ok)
        *ok = 1;
    return s;
}

/// @brief Return the byte length of the first UTF-8 code-point starting at @p s.
/// @details Supports 1–4-byte sequences per RFC 3629. Returns 0 for NULL, empty,
///          or malformed leading bytes. Used to validate locale digit strings.
/// @param s Pointer to the first byte of a UTF-8 sequence.
/// @return 1, 2, 3, or 4 for valid sequences; 0 for invalid/empty input.
static size_t loc_utf8_cp_len(const char *s) {
    if (!s || !*s)
        return 0;
    unsigned char lead = (unsigned char)s[0];
    if (lead < 0x80)
        return 1;
    // Strict decode (VDOC-069): require continuation-byte shapes and reject
    // overlong encodings, UTF-16 surrogates, and code points above U+10FFFF
    // so malformed bytes cannot register as digit glyphs.
    size_t extra;
    uint32_t cp;
    if (lead >= 0xC2 && lead <= 0xDF) {
        extra = 1;
        cp = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        extra = 2;
        cp = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        extra = 3;
        cp = lead & 0x07u;
    } else {
        return 0;
    }
    for (size_t k = 1; k <= extra; ++k) {
        unsigned char ch = (unsigned char)s[k];
        if ((ch & 0xC0u) != 0x80u)
            return 0;
        cp = (cp << 6) | (uint32_t)(ch & 0x3Fu);
    }
    if ((extra == 2 && cp < 0x800u) || (extra == 3 && cp < 0x10000u))
        return 0;
    if ((cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
        return 0;
    return extra + 1;
}

/// @brief Validate that @p digits is a UTF-8 string of exactly 10 code-points.
/// @details The locale number digit string must have exactly 10 entries (0–9
///          equivalents). Each entry may be multibyte (e.g., Arabic-Indic digits).
/// @param digits NUL-terminated UTF-8 string to validate.
/// @return 1 if the string contains exactly 10 valid code-points; 0 otherwise.
static int loc_digits_are_valid(const char *digits) {
    if (!digits || !*digits)
        return 0;
    const char *p = digits;
    for (int i = 0; i < 10; ++i) {
        size_t n = loc_utf8_cp_len(p);
        if (n == 0)
            return 0;
        p += n;
    }
    return *p == '\0';
}

/// @brief Return 1 if @p code is a valid ISO 4217 three-letter uppercase currency code.
/// @param code NUL-terminated string to test.
/// @return 1 when @p code is exactly 3 uppercase ASCII letters; 0 otherwise.
static int loc_currency_code_valid(const char *code) {
    if (!code || strlen(code) != 3)
        return 0;
    return code[0] >= 'A' && code[0] <= 'Z' && code[1] >= 'A' && code[1] <= 'Z' && code[2] >= 'A' &&
           code[2] <= 'Z';
}

/// @brief Return 1 if @p pattern contains exactly one `{n}` and one `{s}` placeholder.
/// @details Currency format patterns must embed both the numeric value placeholder
///          (`{n}`) and the symbol placeholder (`{s}`). Any other `{...}` syntax
///          or unmatched `}` makes the pattern invalid.
/// @param pattern NUL-terminated currency pattern string.
/// @return 1 when both `{n}` and `{s}` are present; 0 otherwise.
static int loc_currency_pattern_valid(const char *pattern) {
    if (!pattern || !*pattern)
        return 0;
    int saw_n = 0;
    int saw_s = 0;
    for (const char *p = pattern; *p; ++p) {
        if (p[0] == '{') {
            if (p[1] == 'n' && p[2] == '}') {
                saw_n++;
                p += 2;
            } else if (p[1] == 's' && p[2] == '}') {
                saw_s++;
                p += 2;
            } else {
                return 0;
            }
        } else if (p[0] == '}') {
            return 0;
        }
    }
    // Exactly one of each: duplicated placeholders emit output that
    // TryParseCurrency cannot round-trip (VDOC-070).
    return saw_n == 1 && saw_s == 1;
}

/// @brief Validate a date/time pattern at load time against the letters the
///        formatter supports, so accepted locale data never traps later in
///        DateFormat (VDOC-070). Mirrors the formatter's scan: quoted
///        literals with '' escapes must be terminated, and letter runs are
///        restricted to y M d E H h m s a.
/// @param pattern NUL-terminated pattern to validate; NULL means an absent
///                optional pattern and is accepted.
/// @return 1 when the pattern is absent or fully supported, otherwise 0.
static int loc_date_pattern_valid(const char *pattern) {
    if (!pattern)
        return 1; // absent patterns fall back to defaults
    size_t i = 0;
    size_t len = strlen(pattern);
    while (i < len) {
        char ch = pattern[i];
        if (ch == '\'') {
            ++i;
            if (i < len && pattern[i] == '\'') {
                ++i;
                continue;
            }
            int closed = 0;
            while (i < len) {
                if (pattern[i] == '\'') {
                    if (i + 1 < len && pattern[i + 1] == '\'') {
                        i += 2;
                        continue;
                    }
                    ++i;
                    closed = 1;
                    break;
                }
                ++i;
            }
            if (!closed)
                return 0;
            continue;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
            if (ch != 'y' && ch != 'M' && ch != 'd' && ch != 'E' && ch != 'H' && ch != 'h' &&
                ch != 'm' && ch != 's' && ch != 'a')
                return 0;
            while (i < len && pattern[i] == ch)
                ++i;
            continue;
        }
        ++i;
    }
    return 1;
}

/// @brief True when @p tmpl contains the "{n}" count placeholder.
/// @param tmpl NUL-terminated relative-time template; may be NULL.
/// @return 1 when the exact placeholder occurs, otherwise 0.
static int loc_template_has_n(const char *tmpl) {
    return tmpl && strstr(tmpl, "{n}") != NULL;
}

/// @brief True when @p tmpl contains both "{0}" and "{1}" placeholders.
/// @param tmpl NUL-terminated list template; may be NULL.
/// @return 1 when both exact positional placeholders occur, otherwise 0.
static int loc_list_template_valid(const char *tmpl) {
    return tmpl && strstr(tmpl, "{0}") != NULL && strstr(tmpl, "{1}") != NULL;
}

/// @brief Fetch and arena-duplicate a string field from a JSON map.
/// @details When the key is absent and @p required is non-zero, records failure via
///          @p ok and calls `loc_fail`. When absent and not required, returns a
///          duplicate of @p fallback. Enforces a 256-byte length limit.
/// @param arena         Arena for the duplicate string.
/// @param map           JSON map to look up in.
/// @param key           Field name to look up.
/// @param fallback      Value to use when the field is absent and not required; may be NULL.
/// @param required      Non-zero to treat absence as an error.
/// @param trap_on_error Non-zero to trap on validation failure.
/// @param ok            Out-parameter cleared to 0 on error; may be NULL.
/// @return Arena-owned duplicate of the field value, or @p fallback copy on absence.
static char *json_dup_string(loc_arena_t *arena,
                             void *map,
                             const char *key,
                             const char *fallback,
                             int required,
                             int trap_on_error,
                             int *ok) {
    void *v = json_get(map, key);
    if (!v) {
        if (required) {
            if (ok)
                *ok = 0;
            loc_fail(trap_on_error,
                     "Zanna.Localization.LocaleManager: locale JSON missing string field");
            return NULL;
        }
        return loc_arena_strdup(arena, fallback ? fallback : "");
    }
    int is_str = 0;
    const char *cs = json_string_cstr(v, &is_str);
    if (!is_str) {
        if (ok)
            *ok = 0;
        loc_fail(trap_on_error,
                 "Zanna.Localization.LocaleManager: locale JSON field must be a string");
        return NULL;
    }
    int64_t len = rt_str_len((rt_string)v);
    if (len < 0 || len > 256) {
        if (ok)
            *ok = 0;
        loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: locale JSON string too long");
        return NULL;
    }
    return loc_arena_strndup(arena, cs, (size_t)len);
}

/// @brief Fetch an integer field from a JSON map, converting from float or int64.
/// @details Accepts both JSON number types (RT_BOX_F64 or RT_BOX_I64). Validates
///          that the value is representable as int32_t and is an integer (no fraction).
///          Returns @p fallback on absence (and on range error when @p trap_on_error is 0).
/// @param map           JSON map to look up in.
/// @param key           Field name to look up.
/// @param fallback      Value returned when the field is absent.
/// @param required      Non-zero to treat absence as an error.
/// @param trap_on_error Non-zero to trap on type/range error.
/// @param ok            Out-parameter cleared to 0 on error; may be NULL.
/// @return The field value as int32_t, or @p fallback on absence or error.
static int32_t json_get_i32(
    void *map, const char *key, int32_t fallback, int required, int trap_on_error, int *ok) {
    void *v = json_get(map, key);
    if (!v) {
        if (required) {
            if (ok)
                *ok = 0;
            loc_fail(trap_on_error,
                     "Zanna.Localization.LocaleManager: locale JSON missing numeric field");
        }
        return fallback;
    }
    int64_t type = rt_box_type(v);
    double d = 0.0;
    if (type == RT_BOX_F64)
        d = rt_unbox_f64(v);
    else if (type == RT_BOX_I64)
        d = (double)rt_unbox_i64(v);
    else {
        if (ok)
            *ok = 0;
        loc_fail(trap_on_error,
                 "Zanna.Localization.LocaleManager: locale JSON field must be numeric");
        return fallback;
    }
    if (d < (double)INT32_MIN || d > (double)INT32_MAX || d != (double)(int32_t)d) {
        if (ok)
            *ok = 0;
        loc_fail(trap_on_error,
                 "Zanna.Localization.LocaleManager: locale JSON integer out of range");
        return fallback;
    }
    return (int32_t)d;
}

/// @brief Map a CLDR plural category name string to the `rt_plural_category_t` enum.
/// @details Accepts "zero", "one", "two", "few", "many"; anything else maps to
///          RT_PLURAL_OTHER (the safe default for unknown category names).
/// @param s NUL-terminated category name; NULL maps to RT_PLURAL_OTHER.
/// @return The matching `rt_plural_category_t` value.
static rt_plural_category_t parse_category(const char *s) {
    if (!s)
        return RT_PLURAL_OTHER;
    if (strcmp(s, "zero") == 0)
        return RT_PLURAL_ZERO;
    if (strcmp(s, "one") == 0)
        return RT_PLURAL_ONE;
    if (strcmp(s, "two") == 0)
        return RT_PLURAL_TWO;
    if (strcmp(s, "few") == 0)
        return RT_PLURAL_FEW;
    if (strcmp(s, "many") == 0)
        return RT_PLURAL_MANY;
    return RT_PLURAL_OTHER;
}

/// @brief Return 1 if @p s is one of the six valid CLDR plural category names.
/// @details Valid names are: "zero", "one", "two", "few", "many", "other".
/// @param s NUL-terminated string to test; NULL returns 0.
/// @return 1 if @p s is a valid CLDR plural category name; 0 otherwise.
static int valid_category_name(const char *s) {
    return s && (strcmp(s, "zero") == 0 || strcmp(s, "one") == 0 || strcmp(s, "two") == 0 ||
                 strcmp(s, "few") == 0 || strcmp(s, "many") == 0 || strcmp(s, "other") == 0);
}

/// @brief Load a fixed-length JSON string array into the arena, falling back to a default.
/// @details When the field is absent, @p *out is set to @p fallback (no allocation).
///          When present, validates that it is a seq of exactly @p expected strings,
///          each at most 256 bytes, and copies each into the arena.
/// @param arena         Arena for duplicated strings.
/// @param map           JSON map to look up in.
/// @param key           Field name of the array.
/// @param fallback      Fallback pointer array when the field is absent.
/// @param expected      Required number of array elements.
/// @param out           Written with a pointer to the loaded (or fallback) array.
/// @param trap_on_error Non-zero to trap on schema violations.
/// @return 1 on success (or absent-with-fallback); 0 on validation failure.
static int load_string_array(loc_arena_t *arena,
                             void *map,
                             const char *key,
                             const char *const *fallback,
                             size_t expected,
                             const char *const **out,
                             int trap_on_error) {
    void *v = json_get(map, key);
    if (!v) {
        *out = fallback;
        return 1;
    }
    if (!loc_is_seq(v) || rt_seq_len(v) != (int64_t)expected)
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: locale JSON array has wrong length");
    const char **arr = (const char **)loc_arena_alloc(arena, expected * sizeof(char *));
    for (size_t i = 0; i < expected; ++i) {
        void *item = rt_seq_get(v, (int64_t)i);
        int ok = 0;
        const char *cs = json_string_cstr(item, &ok);
        if (!ok)
            return loc_fail(
                trap_on_error,
                "Zanna.Localization.LocaleManager: locale JSON array item must be string");
        int64_t len = rt_str_len((rt_string)item);
        if (len < 0 || len > 256)
            return loc_fail(trap_on_error,
                            "Zanna.Localization.LocaleManager: locale JSON array string too long");
        arr[i] = loc_arena_strndup(arena, cs, (size_t)len);
    }
    *out = arr;
    return 1;
}

/// @brief Load a plural rule chain (cardinal or ordinal) from a JSON array field.
/// @details When the field is absent, @p *out_entries / @p *out_count are set to
///          the fallback values (no allocation). When present, validates category
///          names, parses each rule expression into an AST, and stores the entries.
/// @param arena           Arena for parsed rule nodes.
/// @param root            JSON map object containing the field.
/// @param key             Field name (e.g., "plural_cardinal").
/// @param fallback_entries Fallback entry array used when the field is absent.
/// @param fallback_count  Element count of @p fallback_entries.
/// @param out_entries     Written with the loaded (or fallback) entry pointer.
/// @param out_count       Written with the entry count.
/// @param trap_on_error   Non-zero to trap on schema violations.
/// @return 1 on success; 0 on validation failure.
static int load_plural_chain(loc_arena_t *arena,
                             void *root,
                             const char *key,
                             const rt_plural_rule_entry_t *fallback_entries,
                             size_t fallback_count,
                             const rt_plural_rule_entry_t **out_entries,
                             size_t *out_count,
                             int trap_on_error) {
    void *v = json_get(root, key);
    if (!v) {
        *out_entries = fallback_entries;
        *out_count = fallback_count;
        return 1;
    }
    if (!loc_is_seq(v))
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: plural rule field must be an array");
    int64_t n = rt_seq_len(v);
    if (n <= 0 || n > 32)
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: plural rule count out of range");
    rt_plural_rule_entry_t *entries =
        (rt_plural_rule_entry_t *)loc_arena_alloc(arena, (size_t)n * sizeof(*entries));
    for (int64_t i = 0; i < n; ++i) {
        void *entry = rt_seq_get(v, i);
        if (!loc_is_map(entry))
            return loc_fail(trap_on_error,
                            "Zanna.Localization.LocaleManager: plural rule entry must be object");
        int ok = 1;
        char *cat = json_dup_string(arena, entry, "category", "other", 1, trap_on_error, &ok);
        char *rule = json_dup_string(arena, entry, "rule", "true", 1, trap_on_error, &ok);
        if (!ok)
            return 0;
        if (!valid_category_name(cat))
            return loc_fail(trap_on_error,
                            "Zanna.Localization.LocaleManager: invalid plural category");
        rt_plural_rule_node_t *head = parse_rule(arena, rule);
        if (!head)
            return loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: invalid plural rule");
        entries[i].category = parse_category(cat);
        entries[i].head = head;
    }
    *out_entries = entries;
    *out_count = (size_t)n;
    return 1;
}

/// @brief Load a single relative-time unit object from a JSON map into @p out.
/// @details When the unit key is absent, copies @p fallback unchanged. When present,
///          validates that it is a map and loads each category string (other, zero,
///          one, two, few, many) as optional fields.
/// @param arena         Arena for duplicated strings.
/// @param units         Parent JSON map containing the unit key.
/// @param name          Unit key name (e.g., "second", "day").
/// @param fallback      Fallback unit data copied when the key is absent.
/// @param out           Written with the loaded unit data.
/// @param trap_on_error Non-zero to trap on schema violations.
/// @return 1 on success; 0 on validation failure.
static int load_reltime_unit(loc_arena_t *arena,
                             void *units,
                             const char *name,
                             const rt_locdata_reltime_unit_t *fallback,
                             rt_locdata_reltime_unit_t *out,
                             int trap_on_error) {
    *out = *fallback;
    void *u = json_get(units, name);
    if (!u)
        return 1;
    if (!loc_is_map(u))
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: relative_time unit must be object");
    int ok = 1;
    out->other = json_dup_string(arena, u, "other", fallback->other, 0, trap_on_error, &ok);
    out->zero = json_dup_string(arena, u, "zero", fallback->zero, 0, trap_on_error, &ok);
    out->one = json_dup_string(arena, u, "one", fallback->one, 0, trap_on_error, &ok);
    out->two = json_dup_string(arena, u, "two", fallback->two, 0, trap_on_error, &ok);
    out->few = json_dup_string(arena, u, "few", fallback->few, 0, trap_on_error, &ok);
    out->many = json_dup_string(arena, u, "many", fallback->many, 0, trap_on_error, &ok);
    return ok;
}

/// @brief Load a list-format style (pair, start, middle, end) from a JSON field.
/// @details When the key is absent, copies @p fallback unchanged. When present,
///          validates it is a map and loads the four template strings as optional fields.
/// @param arena         Arena for duplicated strings.
/// @param parent        Parent JSON map containing the style key.
/// @param key           Style key name (e.g., "and", "or", "unit").
/// @param fallback      Fallback style data copied when the key is absent.
/// @param out           Written with the loaded style.
/// @param trap_on_error Non-zero to trap on schema violations.
/// @return 1 on success; 0 on validation failure.
static int load_list_style(loc_arena_t *arena,
                           void *parent,
                           const char *key,
                           const rt_locdata_list_style_t *fallback,
                           rt_locdata_list_style_t *out,
                           int trap_on_error) {
    *out = *fallback;
    void *style = json_get(parent, key);
    if (!style)
        return 1;
    if (!loc_is_map(style))
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: list style must be object");
    int ok = 1;
    out->pair = json_dup_string(arena, style, "pair", fallback->pair, 0, trap_on_error, &ok);
    out->start = json_dup_string(arena, style, "start", fallback->start, 0, trap_on_error, &ok);
    out->middle = json_dup_string(arena, style, "middle", fallback->middle, 0, trap_on_error, &ok);
    out->end = json_dup_string(arena, style, "end", fallback->end, 0, trap_on_error, &ok);
    if (!ok)
        return 0;
    // Every list template joins two operands; a missing positional
    // placeholder would silently drop list items (VDOC-070).
    if (!loc_list_template_valid(out->pair) || !loc_list_template_valid(out->start) ||
        !loc_list_template_valid(out->middle) || !loc_list_template_valid(out->end))
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: list template missing {0}/{1}");
    return ok;
}

/// @brief Parse a JSON object into a fully-populated rt_locale_data_t record.
/// @details Allocates a fresh arena, copies the en-US defaults as the base record,
///          then applies all fields present in @p root (tag, names, numbers,
///          currency, dates, relative_time, plural chains, list_format, collation).
///          The arena is freed and NULL is returned on any schema violation.
/// @param root          Runtime map handle for the parsed JSON object.
/// @param trap_on_error Non-zero to trap on schema violations; 0 to return NULL silently.
/// @return Heap-allocated, arena-owned locale-data record; NULL on failure.
static rt_locale_data_t *loc_data_from_json(void *root, int trap_on_error) {
    if (!loc_is_map(root)) {
        loc_fail(trap_on_error,
                 "Zanna.Localization.LocaleManager: locale JSON root must be object");
        return NULL;
    }

    loc_arena_t *arena = loc_arena_new();
    rt_locale_data_t *data = (rt_locale_data_t *)loc_arena_alloc(arena, sizeof(*data));
    *data = *rt_locale_data_en_us();
    data->arena = arena;
    data->formatter_refs = 0;

    int ok = 1;
    char *raw_tag = json_dup_string(arena, root, "tag", NULL, 1, trap_on_error, &ok);
    if (!ok || !raw_tag) {
        loc_arena_free(arena);
        return NULL;
    }
    rt_locale_t parsed;
    if (rt_locale_internal_parse_into(raw_tag, strlen(raw_tag), &parsed) != 0 ||
        strcmp(parsed.tag, "root") == 0) {
        loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: invalid locale tag in JSON");
        loc_arena_free(arena);
        return NULL;
    }
    data->tag = loc_arena_strdup(arena, parsed.tag);

    void *names = json_get(root, "names");
    if (names && !loc_is_map(names)) {
        loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: names must be object");
        loc_arena_free(arena);
        return NULL;
    }
    if (names) {
        data->names.language =
            json_dup_string(arena, names, "language", data->names.language, 0, trap_on_error, &ok);
        data->names.region =
            json_dup_string(arena, names, "region", data->names.region, 0, trap_on_error, &ok);
        data->names.display =
            json_dup_string(arena, names, "display", data->names.display, 0, trap_on_error, &ok);
    }

    char *text_dir =
        json_dup_string(arena, root, "text_direction", data->text_direction, 0, trap_on_error, &ok);
    if (text_dir && (strcmp(text_dir, "ltr") == 0 || strcmp(text_dir, "rtl") == 0)) {
        memset(data->text_direction, 0, sizeof(data->text_direction));
        memcpy(data->text_direction, text_dir, strlen(text_dir));
    } else if (text_dir) {
        loc_fail(trap_on_error,
                 "Zanna.Localization.LocaleManager: text_direction must be ltr or rtl");
        loc_arena_free(arena);
        return NULL;
    }
    data->first_day_of_week =
        json_get_i32(root, "first_day_of_week", data->first_day_of_week, 0, trap_on_error, &ok);
    if (data->first_day_of_week < 0 || data->first_day_of_week > 6) {
        loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: first_day_of_week out of range");
        loc_arena_free(arena);
        return NULL;
    }
    char *measurement =
        json_dup_string(arena, root, "measurement", data->measurement, 0, trap_on_error, &ok);
    if (measurement) {
        if (strcmp(measurement, "metric") != 0 && strcmp(measurement, "us") != 0 &&
            strcmp(measurement, "uk") != 0) {
            loc_fail(trap_on_error,
                     "Zanna.Localization.LocaleManager: measurement must be metric/us/uk");
            loc_arena_free(arena);
            return NULL;
        }
        memset(data->measurement, 0, sizeof(data->measurement));
        strncpy(data->measurement, measurement, sizeof(data->measurement) - 1);
    }

    void *numbers = json_get(root, "numbers");
    if (numbers) {
        if (!loc_is_map(numbers)) {
            loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: numbers must be object");
            loc_arena_free(arena);
            return NULL;
        }
        data->numbers.decimal_sep = json_dup_string(
            arena, numbers, "decimal_sep", data->numbers.decimal_sep, 0, trap_on_error, &ok);
        data->numbers.group_sep = json_dup_string(
            arena, numbers, "group_sep", data->numbers.group_sep, 0, trap_on_error, &ok);
        data->numbers.group_size =
            json_get_i32(numbers, "group_size", data->numbers.group_size, 0, trap_on_error, &ok);
        data->numbers.secondary_group_size = json_get_i32(numbers,
                                                          "secondary_group_size",
                                                          data->numbers.secondary_group_size,
                                                          0,
                                                          trap_on_error,
                                                          &ok);
        data->numbers.minus =
            json_dup_string(arena, numbers, "minus", data->numbers.minus, 0, trap_on_error, &ok);
        data->numbers.plus =
            json_dup_string(arena, numbers, "plus", data->numbers.plus, 0, trap_on_error, &ok);
        data->numbers.percent = json_dup_string(
            arena, numbers, "percent", data->numbers.percent, 0, trap_on_error, &ok);
        data->numbers.infinity = json_dup_string(
            arena, numbers, "infinity", data->numbers.infinity, 0, trap_on_error, &ok);
        data->numbers.nan =
            json_dup_string(arena, numbers, "nan", data->numbers.nan, 0, trap_on_error, &ok);
        data->numbers.exponent = json_dup_string(
            arena, numbers, "exponent", data->numbers.exponent, 0, trap_on_error, &ok);
        data->numbers.digits =
            json_dup_string(arena, numbers, "digits", data->numbers.digits, 0, trap_on_error, &ok);
        if (data->numbers.group_size < 1 || data->numbers.group_size > 9 ||
            data->numbers.secondary_group_size < 0 || data->numbers.secondary_group_size > 9 ||
            !data->numbers.decimal_sep || !*data->numbers.decimal_sep || !data->numbers.minus ||
            !*data->numbers.minus || !data->numbers.plus || !*data->numbers.plus ||
            !loc_digits_are_valid(data->numbers.digits) ||
            // Equal separators make formatted numbers ambiguous to parse.
            (data->numbers.group_sep && *data->numbers.group_sep &&
             strcmp(data->numbers.decimal_sep, data->numbers.group_sep) == 0)) {
            loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: invalid numbers schema");
            loc_arena_free(arena);
            return NULL;
        }
    }

    void *currency = json_get(root, "currency");
    if (currency) {
        if (!loc_is_map(currency)) {
            loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: currency must be object");
            loc_arena_free(arena);
            return NULL;
        }
        data->currency.default_code = json_dup_string(
            arena, currency, "default_code", data->currency.default_code, 0, trap_on_error, &ok);
        data->currency.symbol = json_dup_string(
            arena, currency, "symbol", data->currency.symbol, 0, trap_on_error, &ok);
        data->currency.pattern_positive = json_dup_string(arena,
                                                          currency,
                                                          "pattern_positive",
                                                          data->currency.pattern_positive,
                                                          0,
                                                          trap_on_error,
                                                          &ok);
        data->currency.pattern_negative = json_dup_string(arena,
                                                          currency,
                                                          "pattern_negative",
                                                          data->currency.pattern_negative,
                                                          0,
                                                          trap_on_error,
                                                          &ok);
        data->currency.fraction_digits = json_get_i32(
            currency, "fraction_digits", data->currency.fraction_digits, 0, trap_on_error, &ok);
        if (!loc_currency_code_valid(data->currency.default_code) ||
            !loc_currency_pattern_valid(data->currency.pattern_positive) ||
            !loc_currency_pattern_valid(data->currency.pattern_negative) ||
            data->currency.fraction_digits < 0 || data->currency.fraction_digits > 9) {
            loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: invalid currency schema");
            loc_arena_free(arena);
            return NULL;
        }
    }

    void *dates = json_get(root, "dates");
    if (dates) {
        if (!loc_is_map(dates)) {
            loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: dates must be object");
            loc_arena_free(arena);
            return NULL;
        }
        if (!load_string_array(arena,
                               dates,
                               "months_wide",
                               data->dates.months_wide,
                               12,
                               &data->dates.months_wide,
                               trap_on_error) ||
            !load_string_array(arena,
                               dates,
                               "months_abbr",
                               data->dates.months_abbr,
                               12,
                               &data->dates.months_abbr,
                               trap_on_error) ||
            !load_string_array(arena,
                               dates,
                               "days_wide",
                               data->dates.days_wide,
                               7,
                               &data->dates.days_wide,
                               trap_on_error) ||
            !load_string_array(arena,
                               dates,
                               "days_abbr",
                               data->dates.days_abbr,
                               7,
                               &data->dates.days_abbr,
                               trap_on_error)) {
            loc_arena_free(arena);
            return NULL;
        }
        data->dates.am = json_dup_string(arena, dates, "am", data->dates.am, 0, trap_on_error, &ok);
        data->dates.pm = json_dup_string(arena, dates, "pm", data->dates.pm, 0, trap_on_error, &ok);
        void *patterns = json_get(dates, "patterns");
        if (patterns) {
            if (!loc_is_map(patterns)) {
                loc_fail(trap_on_error,
                         "Zanna.Localization.LocaleManager: date patterns must be object");
                loc_arena_free(arena);
                return NULL;
            }
            data->dates.patterns.short_p = json_dup_string(
                arena, patterns, "short", data->dates.patterns.short_p, 0, trap_on_error, &ok);
            data->dates.patterns.medium_p = json_dup_string(
                arena, patterns, "medium", data->dates.patterns.medium_p, 0, trap_on_error, &ok);
            data->dates.patterns.long_p = json_dup_string(
                arena, patterns, "long", data->dates.patterns.long_p, 0, trap_on_error, &ok);
            data->dates.patterns.full_p = json_dup_string(
                arena, patterns, "full", data->dates.patterns.full_p, 0, trap_on_error, &ok);
            data->dates.patterns.time_short = json_dup_string(arena,
                                                              patterns,
                                                              "time_short",
                                                              data->dates.patterns.time_short,
                                                              0,
                                                              trap_on_error,
                                                              &ok);
            data->dates.patterns.time_medium = json_dup_string(arena,
                                                               patterns,
                                                               "time_medium",
                                                               data->dates.patterns.time_medium,
                                                               0,
                                                               trap_on_error,
                                                               &ok);
            data->dates.patterns.datetime_short =
                json_dup_string(arena,
                                patterns,
                                "datetime_short",
                                data->dates.patterns.datetime_short,
                                0,
                                trap_on_error,
                                &ok);
            data->dates.patterns.datetime_medium =
                json_dup_string(arena,
                                patterns,
                                "datetime_medium",
                                data->dates.patterns.datetime_medium,
                                0,
                                trap_on_error,
                                &ok);
            if (!loc_date_pattern_valid(data->dates.patterns.short_p) ||
                !loc_date_pattern_valid(data->dates.patterns.medium_p) ||
                !loc_date_pattern_valid(data->dates.patterns.long_p) ||
                !loc_date_pattern_valid(data->dates.patterns.full_p) ||
                !loc_date_pattern_valid(data->dates.patterns.time_short) ||
                !loc_date_pattern_valid(data->dates.patterns.time_medium) ||
                !loc_date_pattern_valid(data->dates.patterns.datetime_short) ||
                !loc_date_pattern_valid(data->dates.patterns.datetime_medium)) {
                loc_fail(trap_on_error,
                         "Zanna.Localization.LocaleManager: unsupported date pattern letter");
                loc_arena_free(arena);
                return NULL;
            }
        }
    }

    void *rt = json_get(root, "relative_time");
    if (rt) {
        if (!loc_is_map(rt)) {
            loc_fail(trap_on_error,
                     "Zanna.Localization.LocaleManager: relative_time must be object");
            loc_arena_free(arena);
            return NULL;
        }
        data->reltime.now =
            json_dup_string(arena, rt, "now", data->reltime.now, 0, trap_on_error, &ok);
        data->reltime.past =
            json_dup_string(arena, rt, "past", data->reltime.past, 0, trap_on_error, &ok);
        data->reltime.future =
            json_dup_string(arena, rt, "future", data->reltime.future, 0, trap_on_error, &ok);
        data->reltime.short_past = json_dup_string(
            arena, rt, "short_past", data->reltime.short_past, 0, trap_on_error, &ok);
        data->reltime.short_future = json_dup_string(
            arena, rt, "short_future", data->reltime.short_future, 0, trap_on_error, &ok);
        if (!loc_template_has_n(data->reltime.past) || !loc_template_has_n(data->reltime.future) ||
            !loc_template_has_n(data->reltime.short_past) ||
            !loc_template_has_n(data->reltime.short_future)) {
            loc_fail(trap_on_error,
                     "Zanna.Localization.LocaleManager: relative_time template missing {n}");
            loc_arena_free(arena);
            return NULL;
        }
        void *units = json_get(rt, "units");
        static const char *const unit_names[7] = {
            "second", "minute", "hour", "day", "week", "month", "year"};
        if (units) {
            if (!loc_is_map(units)) {
                loc_fail(trap_on_error,
                         "Zanna.Localization.LocaleManager: relative_time.units must be object");
                loc_arena_free(arena);
                return NULL;
            }
            for (size_t i = 0; i < 7; ++i) {
                if (!load_reltime_unit(arena,
                                       units,
                                       unit_names[i],
                                       &data->reltime.units[i],
                                       &data->reltime.units[i],
                                       trap_on_error)) {
                    loc_arena_free(arena);
                    return NULL;
                }
            }
        }
        void *short_units = json_get(rt, "short_units");
        if (short_units) {
            if (!loc_is_map(short_units)) {
                loc_fail(
                    trap_on_error,
                    "Zanna.Localization.LocaleManager: relative_time.short_units must be object");
                loc_arena_free(arena);
                return NULL;
            }
            for (size_t i = 0; i < 7; ++i) {
                if (!load_reltime_unit(arena,
                                       short_units,
                                       unit_names[i],
                                       &data->reltime.short_units[i],
                                       &data->reltime.short_units[i],
                                       trap_on_error)) {
                    loc_arena_free(arena);
                    return NULL;
                }
            }
        }
        if (!data->reltime.now || !*data->reltime.now || !data->reltime.past ||
            !strstr(data->reltime.past, "{n}") || !strstr(data->reltime.past, "{unit}") ||
            !data->reltime.future || !strstr(data->reltime.future, "{n}") ||
            !strstr(data->reltime.future, "{unit}")) {
            loc_fail(trap_on_error,
                     "Zanna.Localization.LocaleManager: invalid relative_time schema");
            loc_arena_free(arena);
            return NULL;
        }
    }

    if (!load_plural_chain(arena,
                           root,
                           "plural_cardinal",
                           data->plural_cardinal,
                           data->cardinal_count,
                           &data->plural_cardinal,
                           &data->cardinal_count,
                           trap_on_error) ||
        !load_plural_chain(arena,
                           root,
                           "plural_ordinal",
                           data->plural_ordinal,
                           data->ordinal_count,
                           &data->plural_ordinal,
                           &data->ordinal_count,
                           trap_on_error)) {
        loc_arena_free(arena);
        return NULL;
    }

    void *list_format = json_get(root, "list_format");
    if (list_format) {
        if (!loc_is_map(list_format)) {
            loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: list_format must be object");
            loc_arena_free(arena);
            return NULL;
        }
        if (!load_list_style(arena,
                             list_format,
                             "and",
                             &data->list_format.and_p,
                             &data->list_format.and_p,
                             trap_on_error) ||
            !load_list_style(arena,
                             list_format,
                             "or",
                             &data->list_format.or_p,
                             &data->list_format.or_p,
                             trap_on_error) ||
            !load_list_style(arena,
                             list_format,
                             "unit",
                             &data->list_format.unit_p,
                             &data->list_format.unit_p,
                             trap_on_error)) {
            loc_arena_free(arena);
            return NULL;
        }
    }

    void *collation = json_get(root, "collation");
    if (collation) {
        if (!loc_is_map(collation)) {
            loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: collation must be object");
            loc_arena_free(arena);
            return NULL;
        }
        data->collation.strength =
            json_get_i32(collation, "strength", data->collation.strength, 0, trap_on_error, &ok);
        // The collator implements strengths 1..3 only (quaternary is
        // unsupported and would be silently clamped); reject 4 at load time
        // so accepted settings always take effect (VDOC-082).
        if (data->collation.strength < 1 || data->collation.strength > 3) {
            loc_fail(trap_on_error,
                     "Zanna.Localization.LocaleManager: collation.strength out of range");
            loc_arena_free(arena);
            return NULL;
        }
        data->collation.reorder = NULL;
        data->collation.reorder_len = 0;
    }

    if (!ok) {
        loc_arena_free(arena);
        return NULL;
    }
    return data;
}

/// @brief Return 1 if @p text contains a JSON object (`{`) as its first non-whitespace char.
/// @details Used as a quick pre-check before calling the full JSON parser, to give
///          a clearer error for obviously wrong input (bare strings, arrays, etc.).
/// @param text rt_string to inspect; NULL or empty returns 0.
/// @return 1 if the first non-whitespace byte is '{'; 0 otherwise.
static int loc_text_starts_object(rt_string text) {
    if (!text)
        return 0;
    const char *cs = rt_string_cstr(text);
    int64_t len = rt_str_len(text);
    for (int64_t i = 0; cs && i < len; ++i) {
        char c = cs[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        return c == '{';
    }
    return 0;
}

/// @brief Parse a JSON string containing locale data and register it in the manager.
/// @details Validates the JSON string (size, structure, parseable), delegates to
///          `loc_data_from_json`, then registers under the write lock. Returns 0
///          if registration fails (e.g., locale in use).
/// @param text          rt_string containing the raw JSON blob.
/// @param trap_on_error Non-zero to trap on validation or parse failure.
/// @return 1 on successful registration; 0 on failure.
static int loc_register_json_text(rt_string text, int trap_on_error) {
    if (!text)
        return loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: empty locale JSON");
    int64_t len = rt_str_len(text);
    if (len <= 0 || len > 256 * 1024)
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: locale JSON size out of range");
    if (!loc_text_starts_object(text) || rt_json_is_valid(text) != 1)
        return loc_fail(trap_on_error, "Zanna.Localization.LocaleManager: malformed locale JSON");

    void *root = rt_json_parse_object(text);
    if (!root)
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: malformed locale JSON object");
    rt_locale_data_t *data = loc_data_from_json(root, trap_on_error);
    loc_release_object(root);
    if (!data)
        return 0;

    rt_rwlock_write_enter(g_mgr.lock);
    int registered = loc_register_entry_locked(data, /*is_baked=*/0);
    rt_rwlock_write_exit(g_mgr.lock);
    if (!registered)
        return loc_fail(trap_on_error,
                        "Zanna.Localization.LocaleManager: cannot replace locale while in use");
    return 1;
}

//===----------------------------------------------------------------------===//
// Init and teardown
//===----------------------------------------------------------------------===//

/// @brief Double the capacity of the locale registry entry array.
/// @details Starts at 4, doubles each time. Traps on capacity overflow or OOM.
///          Caller must hold the write lock.
static void loc_registry_grow(void) {
    size_t new_cap = g_mgr.capacity == 0 ? 4 : g_mgr.capacity * 2;
    if (g_mgr.capacity > SIZE_MAX / 2 || new_cap > SIZE_MAX / sizeof(*g_mgr.entries)) {
        rt_trap("Zanna.Localization.LocaleManager: registry capacity overflow");
        return;
    }
    loc_registry_entry_t *new_entries =
        (loc_registry_entry_t *)realloc(g_mgr.entries, new_cap * sizeof(*new_entries));
    if (!new_entries) {
        rt_trap("Zanna.Localization.LocaleManager: registry allocation failed");
        return;
    }
    g_mgr.entries = new_entries;
    g_mgr.capacity = new_cap;
}

/// @brief Register (or replace) a locale-data record in the registry.
/// @details Caller must hold the write lock. Baked records (is_baked=1) take
///          permanent precedence over loaded records for the same tag. Loaded records
///          can only replace loaded records if the existing record has no live
///          references; if they do, the new data is freed and 0 is returned.
/// @param data      Locale-data record to register; must have a non-NULL `tag`.
/// @param is_baked  1 for permanent built-in records; 0 for JSON/ZPAK loaded records.
/// @return 1 on success; 0 if the existing record is in use and cannot be replaced.
static int loc_register_entry_locked(const rt_locale_data_t *data, int is_baked) {
    if (!data || !data->tag)
        return 0;
    // Replace existing entry (idempotent load).
    for (size_t i = 0; i < g_mgr.count; ++i) {
        if (strcmp(g_mgr.entries[i].tag, data->tag) == 0) {
            if (g_mgr.entries[i].is_baked && !is_baked) {
                loc_free_loaded_data(data);
                return 1; // baked en-US remains authoritative
            }
            if (!g_mgr.entries[i].is_baked && g_mgr.entries[i].data != data) {
                if (loc_data_ref_count(g_mgr.entries[i].data) != 0) {
                    loc_free_loaded_data(data);
                    return 0;
                }
                loc_free_loaded_data(g_mgr.entries[i].data);
            }
            g_mgr.entries[i].data = data;
            g_mgr.entries[i].is_baked = is_baked;
            g_mgr.entries[i].tag = data->tag;
            return 1;
        }
    }
    if (g_mgr.count == g_mgr.capacity)
        loc_registry_grow();
    g_mgr.entries[g_mgr.count].tag = data->tag;
    g_mgr.entries[g_mgr.count].data = data;
    g_mgr.entries[g_mgr.count].is_baked = is_baked;
    g_mgr.count++;
    return 1;
}

/// @brief Construct a Locale handle for the given tag, resolving its data
///        pointer from the current registry. Caller holds the write lock.
/// @details Intentionally does NOT go through rt_locale_parse / try_parse
///          because those re-enter the manager's init path, causing
///          infinite recursion during bootstrap. We build the handle
///          directly and bind the data pointer via internal helpers.
/// @param tag Canonical NUL-terminated tag to parse while the manager lock is held.
/// @return Fresh Locale handle with retained matching data when registered,
///         or NULL on allocation/parse failure.
static void *loc_make_handle_locked(const char *tag) {
    // Use a direct construction path: allocate + set fields + walk registry.
    void *loc = NULL;
    rt_locale_t *l = (rt_locale_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_locale_t));
    if (!l) {
        return NULL;
    }
    memset(l, 0, sizeof(*l));
    rt_obj_set_finalizer(l, rt_locale_internal_finalizer);
    // Parse directly into the struct — uses the same helper as public parse.
    if (rt_locale_internal_parse_into(tag, strlen(tag), l) != 0) {
        if (rt_obj_release_check0(l))
            rt_obj_free(l);
        return NULL;
    }
    loc = l;
    // Registry lookup without calling the public API (we already hold the lock).
    for (size_t i = 0; i < g_mgr.count; ++i) {
        if (strcmp(g_mgr.entries[i].tag, tag) == 0) {
            l->data = g_mgr.entries[i].data;
            rt_locale_manager_retain_data(l->data);
            break;
        }
    }
    return loc;
}

/// @brief Lazily initialise the LocaleManager singleton on first use.
/// @details Uses a double-checked locking pattern with acquire/release atomics.
///          On the slow path, allocates the rwlock via a CAS idiom (concurrent
///          allocators lose their extra lock to avoid a leak), registers the baked
///          en-US data, detects the system locale, and builds `current` and `system`
///          Locale handles. Safe to call from multiple threads concurrently.
static void loc_mgr_ensure_init(void) {
    // Fast path: double-checked.
    if (__atomic_load_n(&g_mgr.initialized, __ATOMIC_ACQUIRE))
        return;

    // Slow path: lazy allocate the lock and take the write section.
    // Creating the lock itself races with other first-callers; use a
    // CAS-style idiom: try creating, then check if someone else beat us.
    void *lock = g_mgr.lock;
    if (!lock) {
        void *fresh = rt_rwlock_new();
        if (!fresh) {
            rt_trap("Zanna.Localization.LocaleManager: rwlock allocation failed");
            return;
        }
        // First writer wins; everyone else discards their rwlock. This is
        // benign because rt_rwlock_new objects can be leaked without side
        // effects until shutdown; under concurrent load the odds of more
        // than 2-3 allocations are low in practice.
#if RT_COMPILER_MSVC
        if (!rt_atomic_compare_exchange_ptr(
                (void *volatile *)&g_mgr.lock, &lock, fresh, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
#else
        if (!__atomic_compare_exchange_n(
                &g_mgr.lock, &lock, fresh, /*weak=*/0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
#endif
        {
            if (rt_obj_release_check0(fresh))
                rt_obj_free(fresh);
        }
    }

    rt_rwlock_write_enter(g_mgr.lock);
    if (__atomic_load_n(&g_mgr.initialized, __ATOMIC_ACQUIRE)) {
        rt_rwlock_write_exit(g_mgr.lock);
        return;
    }

    // Register baked en-US.
    loc_register_entry_locked(rt_locale_data_en_us(), /*is_baked=*/1);

    // Detect system locale.
    char sysbuf[RT_LOCALE_TAG_CAP];
    const char *detected = NULL;
    if (rt_locale_platform_detect_system(sysbuf, sizeof(sysbuf)) == 0)
        detected = sysbuf;

    // Build system handle. If detection failed, use invariant-shaped en-US.
    // Use the internal parse helper (not rt_locale_try_parse) because the
    // public try_parse re-enters the manager, and we are mid-init.
    if (detected && *detected) {
        g_mgr.system = loc_make_handle_locked(detected);
    }
    if (!g_mgr.system) {
        g_mgr.system = loc_make_handle_locked("en-US");
    }

    // Choose current: detected system when it's loaded, else en-US.
    const char *cur_tag = NULL;
    if (g_mgr.system) {
        rt_locale_t *s = (rt_locale_t *)g_mgr.system;
        // Is the detected tag loaded?
        for (size_t i = 0; i < g_mgr.count; ++i) {
            if (strcmp(g_mgr.entries[i].tag, s->tag) == 0) {
                cur_tag = s->tag;
                break;
            }
        }
    }
    if (!cur_tag)
        cur_tag = "en-US";

    g_mgr.current = loc_make_handle_locked(cur_tag);

    __atomic_store_n(&g_mgr.initialized, 1, __ATOMIC_RELEASE);
    rt_rwlock_write_exit(g_mgr.lock);
}

//===----------------------------------------------------------------------===//
// Lookup / retain / release — internal helpers called by rt_locale.c
//===----------------------------------------------------------------------===//

/// @brief Look up locale data for a canonical BCP-47 tag without incrementing the reference count.
/// @details Returns the raw data pointer; the caller must not store it across any
///          manager call that could free the record. Prefer `_retained` when the
///          pointer must outlive the lookup.
/// @param tag Canonical BCP-47 tag string (e.g., "en-US"); NULL or empty returns NULL.
/// @return Non-owning pointer to the locale data, or NULL if not registered.
const rt_locale_data_t *rt_locale_manager_lookup_data(const char *tag) {
    if (!tag || !*tag)
        return NULL;
    loc_mgr_ensure_init();
    rt_rwlock_read_enter(g_mgr.lock);
    const rt_locale_data_t *data = NULL;
    for (size_t i = 0; i < g_mgr.count; ++i) {
        if (strcmp(g_mgr.entries[i].tag, tag) == 0) {
            data = g_mgr.entries[i].data;
            break;
        }
    }
    rt_rwlock_read_exit(g_mgr.lock);
    return data;
}

/// @brief Look up locale data for a tag and atomically increment its reference count.
/// @details The caller owns the resulting reference and must call
///          `rt_locale_manager_release_data` when done. Used by `rt_locale_parse`
///          so Locale handles keep their data alive.
/// @param tag Canonical BCP-47 tag string; NULL or empty returns NULL.
/// @return Retained pointer to the locale data, or NULL if not registered.
const rt_locale_data_t *rt_locale_manager_lookup_data_retained(const char *tag) {
    if (!tag || !*tag)
        return NULL;
    loc_mgr_ensure_init();
    rt_rwlock_read_enter(g_mgr.lock);
    const rt_locale_data_t *data = NULL;
    for (size_t i = 0; i < g_mgr.count; ++i) {
        if (strcmp(g_mgr.entries[i].tag, tag) == 0) {
            data = g_mgr.entries[i].data;
            rt_locale_manager_retain_data(data);
            break;
        }
    }
    rt_rwlock_read_exit(g_mgr.lock);
    return data;
}

/// @brief Increment the reference count of a locale-data record.
/// @details No-op for NULL or baked records (arena == NULL) — baked records are
///          immortal and need no reference tracking. Thread-safe via atomic fetch-add.
/// @param data Locale-data record to retain; NULL and baked records are ignored.
void rt_locale_manager_retain_data(const rt_locale_data_t *data) {
    if (!data || data->arena == NULL)
        return; // baked records are immortal; no counter needed
    // Cast away const for the atomic increment — the counter is the only
    // mutable field on a loaded record and lives behind this helper. Route
    // through uintptr_t to avoid the const-qualifier-drop warning.
    int64_t *counter = (int64_t *)(uintptr_t)&data->formatter_refs;
    __atomic_fetch_add(counter, 1, __ATOMIC_ACQ_REL);
}

/// @brief Decrement the reference count of a locale-data record.
/// @details No-op for NULL or baked records. Traps if the count would go negative
///          (underflow guard). Does NOT free the record — that happens via
///          `loc_free_loaded_data` when the registry removes the entry.
/// @param data Locale-data record to release; NULL and baked records are ignored.
void rt_locale_manager_release_data(const rt_locale_data_t *data) {
    if (!data || data->arena == NULL)
        return;
    int64_t *counter = (int64_t *)(uintptr_t)&data->formatter_refs;
    int64_t old = __atomic_fetch_sub(counter, 1, __ATOMIC_ACQ_REL);
    if (old <= 0) {
        __atomic_fetch_add(counter, 1, __ATOMIC_ACQ_REL);
        rt_trap("Zanna.Localization.LocaleManager: locale data refcount underflow");
    }
}

//===----------------------------------------------------------------------===//
// Public class surface
//===----------------------------------------------------------------------===//

/// @brief Return the current locale handle with an incremented GC retain count.
/// @details The caller is responsible for releasing the handle. Initialises the
///          manager on first call. Thread-safe under the read lock.
/// @return Retained Locale handle for the current locale; never NULL after init.
void *rt_locale_manager_current(void) {
    loc_mgr_ensure_init();
    rt_rwlock_read_enter(g_mgr.lock);
    void *handle = g_mgr.current;
    if (handle)
        rt_heap_retain(handle);
    rt_rwlock_read_exit(g_mgr.lock);
    return handle;
}

/// @brief Set the current locale to @p locale, trapping if it is not loaded.
/// @details Binds the locale's data pointer from the registry, retains the new
///          handle, and releases the old `current` handle. Traps on NULL input.
/// @param locale Opaque Locale handle that must be registered in the manager.
void rt_locale_manager_set_current(void *locale) {
    loc_mgr_ensure_init();
    if (!locale) {
        rt_trap("Zanna.Localization.LocaleManager: SetCurrent requires a non-null Locale");
        return;
    }
    rt_locale_t *target = (rt_locale_t *)locale;
    const char *tag = target->tag[0] ? target->tag : "root";

    rt_rwlock_write_enter(g_mgr.lock);
    int loaded = 0;
    const rt_locale_data_t *bound_data = NULL;
    for (size_t i = 0; i < g_mgr.count; ++i) {
        if (strcmp(g_mgr.entries[i].tag, tag) == 0) {
            loaded = 1;
            bound_data = g_mgr.entries[i].data;
            break;
        }
    }
    if (!loaded) {
        rt_rwlock_write_exit(g_mgr.lock);
        rt_trap("Zanna.Localization.LocaleManager: locale not loaded (call Load* first)");
        return;
    }
    rt_locale_bind_data(locale, bound_data);
    void *old = g_mgr.current;
    g_mgr.current = locale;
    rt_heap_retain(locale);
    rt_rwlock_write_exit(g_mgr.lock);

    if (old)
        loc_release_handle(old);
}

/// @brief Return the detected system locale handle with an incremented GC retain count.
/// @details The system locale is detected at first-init from the platform API.
///          Falls back to en-US when detection fails. The caller owns the returned
///          reference and must release it when done.
/// @return Retained Locale handle for the detected system locale; never NULL after init.
void *rt_locale_manager_system(void) {
    loc_mgr_ensure_init();
    rt_rwlock_read_enter(g_mgr.lock);
    void *handle = g_mgr.system;
    if (handle)
        rt_heap_retain(handle);
    rt_rwlock_read_exit(g_mgr.lock);
    return handle;
}

/// @brief Return a new List of BCP-47 tag strings for all registered locales.
/// @details Includes both baked and JSON-loaded records. The list is owned by
///          the caller. Tags are returned in registration order.
/// @return A new rt_list of rt_string BCP-47 tags; caller must release.
void *rt_locale_manager_available(void) {
    loc_mgr_ensure_init();
    void *list = rt_list_new();
    if (!list)
        return NULL;
    rt_rwlock_read_enter(g_mgr.lock);
    for (size_t i = 0; i < g_mgr.count; ++i) {
        rt_string s = rt_string_from_bytes(g_mgr.entries[i].tag, strlen(g_mgr.entries[i].tag));
        rt_list_push(list, s);
        rt_string_unref(s);
    }
    rt_rwlock_read_exit(g_mgr.lock);
    return list;
}

/// @brief Return 1 if the locale for @p locale's tag is registered in the manager.
/// @details Uses the canonical tag of @p locale for the registry lookup. Treats
///          the invariant locale (empty tag) as "root".
/// @param locale Opaque Locale handle to check; NULL returns 0.
/// @return 1 if registered; 0 if absent or @p locale is NULL.
int8_t rt_locale_manager_is_loaded(void *locale) {
    if (!locale)
        return 0;
    loc_mgr_ensure_init();
    rt_locale_t *l = (rt_locale_t *)locale;
    const char *tag = l->tag[0] ? l->tag : "root";
    rt_rwlock_read_enter(g_mgr.lock);
    int8_t found = 0;
    for (size_t i = 0; i < g_mgr.count; ++i) {
        if (strcmp(g_mgr.entries[i].tag, tag) == 0) {
            found = 1;
            break;
        }
    }
    rt_rwlock_read_exit(g_mgr.lock);
    return found;
}

/// @brief Load and register a locale from a JSON file at @p path, trapping on failure.
/// @details Reads the file as text, parses it, and registers it under the write lock.
///          Traps if the path is NULL, the file cannot be read, or the JSON is invalid.
/// @param path rt_string file-system path to the locale JSON file.
void rt_locale_manager_load_from_json(rt_string path) {
    loc_mgr_ensure_init();
    if (!path) {
        rt_trap("Zanna.Localization.LocaleManager: LoadFromJson requires a path");
        return;
    }
    rt_string text = rt_io_file_read_all_text(path);
    if (!text) {
        rt_trap("Zanna.Localization.LocaleManager: cannot read locale JSON");
        return;
    }
    (void)loc_register_json_text(text, /*trap_on_error=*/1);
    rt_string_unref(text);
}

/// @brief Attempt to load and register a locale from a JSON file, returning 0 on failure.
/// @details Checks that the file exists before reading. Returns 0 silently on any
///          failure instead of trapping. Used internally by `rt_locale_manager_load`
///          when scanning search paths.
/// @param path rt_string file-system path to the locale JSON file.
/// @return 1 on successful registration; 0 on any failure.
int8_t rt_locale_manager_try_load_from_json(rt_string path) {
    loc_mgr_ensure_init();
    if (!path || rt_io_file_exists(path) != 1)
        return 0;
    rt_string text = rt_io_file_read_all_text(path);
    if (!text)
        return 0;
    int ok = loc_register_json_text(text, /*trap_on_error=*/0);
    rt_string_unref(text);
    return ok ? 1 : 0;
}

/// @brief Load and register a locale from a named ZPAK asset, trapping on failure.
/// @details Loads the asset as bytes, converts to a string, and passes through the
///          JSON registration path. Traps if the name is NULL, the asset is missing,
///          or the JSON is invalid.
/// @param name rt_string asset name as registered in the ZPAK package.
void rt_locale_manager_load_from_asset(rt_string name) {
    loc_mgr_ensure_init();
    if (!name) {
        rt_trap("Zanna.Localization.LocaleManager: LoadFromAsset requires a name");
        return;
    }
    void *bytes = rt_asset_load_bytes(name);
    if (!bytes) {
        rt_trap("Zanna.Localization.LocaleManager: locale asset not found");
        return;
    }
    rt_string text = rt_bytes_to_str(bytes);
    loc_release_object(bytes);
    (void)loc_register_json_text(text, /*trap_on_error=*/1);
    rt_string_unref(text);
}

/// @brief Attempt to load and register a locale from a named ZPAK asset, returning 0 on failure.
/// @details Non-trapping counterpart to `rt_locale_manager_load_from_asset`.
/// @param name rt_string asset name to look up.
/// @return 1 on successful registration; 0 if the asset is absent or the JSON is invalid.
int8_t rt_locale_manager_try_load_from_asset(rt_string name) {
    loc_mgr_ensure_init();
    if (!name)
        return 0;
    void *bytes = rt_asset_load_bytes(name);
    if (!bytes)
        return 0;
    rt_string text = rt_bytes_to_str(bytes);
    loc_release_object(bytes);
    int ok = loc_register_json_text(text, /*trap_on_error=*/0);
    rt_string_unref(text);
    return ok ? 1 : 0;
}

/// @brief Register a baked built-in locale by BCP-47 tag, trapping if it is unknown.
/// @details Currently only "en-US" is supported. Traps on invalid, too-long, or
///          unknown tag strings. Re-registering "en-US" is idempotent.
/// @param tag rt_string BCP-47 tag for the built-in locale (currently only "en-US").
void rt_locale_manager_load_builtin(rt_string tag) {
    loc_mgr_ensure_init();
    const char *bytes = tag ? rt_string_cstr(tag) : NULL;
    int64_t len = tag ? rt_str_len(tag) : 0;
    if (!bytes || len <= 0) {
        rt_trap("Zanna.Localization.LocaleManager: LoadBuiltin requires a non-empty tag");
        return;
    }

    char input[RT_LOCALE_TAG_CAP * 2];
    if ((size_t)len >= sizeof(input)) {
        rt_trap("Zanna.Localization.LocaleManager: builtin tag too long");
        return;
    }
    memcpy(input, bytes, (size_t)len);
    input[len] = '\0';

    rt_locale_t parsed;
    if (rt_locale_internal_parse_into(input, (size_t)len, &parsed) != 0) {
        rt_trap("Zanna.Localization.LocaleManager: invalid builtin locale tag");
        return;
    }

    if (strcmp(parsed.tag, "en-US") == 0) {
        rt_rwlock_write_enter(g_mgr.lock);
        loc_register_entry_locked(rt_locale_data_en_us(), /*is_baked=*/1);
        rt_rwlock_write_exit(g_mgr.lock);
        return;
    }
    rt_trap("Zanna.Localization.LocaleManager: unknown builtin locale (only 'en-US' is baked)");
}

/// @brief Find, load (if needed), and return a Locale handle for @p tag.
/// @details If the tag is already registered, returns a parsed handle immediately.
///          Otherwise walks the search-path list, trying `<path>/<tag>.json` for
///          each directory. Returns NULL if no matching file is found or the tag
///          is invalid. The returned handle is owned by the caller.
/// @param tag rt_string BCP-47 tag to load (e.g., "fr-FR").
/// @return A new Locale handle on success; NULL if the locale cannot be found.
void *rt_locale_manager_load(rt_string tag) {
    loc_mgr_ensure_init();
    const char *bytes = tag ? rt_string_cstr(tag) : NULL;
    int64_t len = tag ? rt_str_len(tag) : 0;
    if (!bytes || len <= 0)
        return NULL;
    char input[RT_LOCALE_TAG_CAP * 2];
    if ((size_t)len >= sizeof(input))
        return NULL;
    memcpy(input, bytes, (size_t)len);
    input[len] = '\0';

    rt_locale_t parsed;
    if (rt_locale_internal_parse_into(input, (size_t)len, &parsed) != 0)
        return NULL;

    rt_rwlock_read_enter(g_mgr.lock);
    int registered = 0;
    for (size_t i = 0; i < g_mgr.count; ++i) {
        if (strcmp(g_mgr.entries[i].tag, parsed.tag) == 0) {
            registered = 1;
            break;
        }
    }
    rt_rwlock_read_exit(g_mgr.lock);
    if (!registered) {
        size_t path_index = 0;
        while (!registered) {
            rt_rwlock_read_enter(g_mgr.lock);
            if (path_index >= g_mgr.search_count) {
                rt_rwlock_read_exit(g_mgr.lock);
                break;
            }
            const char *base = g_mgr.search_paths[path_index++];
            char *base_copy = NULL;
            if (base) {
                size_t base_len = strlen(base);
                base_copy = (char *)malloc(base_len + 1);
                if (base_copy) {
                    memcpy(base_copy, base, base_len);
                    base_copy[base_len] = '\0';
                }
            }
            rt_rwlock_read_exit(g_mgr.lock);
            if (!base_copy)
                break;
            size_t bl = strlen(base_copy);
            size_t tl = strlen(parsed.tag);
            if (bl <= 4096 && tl <= 128) {
                char candidate[4608];
                const char sep =
#if RT_PLATFORM_WINDOWS
                    (bl > 0 && (base_copy[bl - 1] == '/' || base_copy[bl - 1] == '\\')) ? '\0'
                                                                                        : '\\';
#else
                    (bl > 0 && base_copy[bl - 1] == '/') ? '\0' : '/';
#endif
                if (sep)
                    snprintf(
                        candidate, sizeof(candidate), "%s%c%s.json", base_copy, sep, parsed.tag);
                else
                    snprintf(candidate, sizeof(candidate), "%s%s.json", base_copy, parsed.tag);
                rt_string path = rt_string_from_bytes(candidate, strlen(candidate));
                if (rt_locale_manager_try_load_from_json(path) == 1)
                    registered = 1;
                rt_string_unref(path);
            }
            free(base_copy);
        }
    }
    if (!registered)
        return NULL;

    rt_string s = rt_string_from_bytes(parsed.tag, strlen(parsed.tag));
    void *loc = rt_locale_try_parse(s);
    rt_string_unref(s);
    return loc;
}

/// @brief Return the current locale search path as a platform-separator-delimited string.
/// @details Directories are joined with `:` on POSIX and `;` on Windows, mirroring
///          the PATH environment variable convention. Returns an empty string when no
///          paths are registered.
/// @return A freshly allocated rt_string with the concatenated search path.
rt_string rt_locale_manager_search_path(void) {
    loc_mgr_ensure_init();
    rt_rwlock_read_enter(g_mgr.lock);
    if (g_mgr.search_count == 0) {
        rt_rwlock_read_exit(g_mgr.lock);
        return rt_string_from_bytes("", 0);
    }
    // Compute concatenation length.
    size_t total = 0;
    for (size_t i = 0; i < g_mgr.search_count; ++i) {
        total += strlen(g_mgr.search_paths[i]);
        if (i + 1 < g_mgr.search_count)
            total += strlen(LOC_PATH_SEP);
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        rt_rwlock_read_exit(g_mgr.lock);
        return rt_string_from_bytes("", 0);
    }
    char *p = buf;
    for (size_t i = 0; i < g_mgr.search_count; ++i) {
        size_t l = strlen(g_mgr.search_paths[i]);
        memcpy(p, g_mgr.search_paths[i], l);
        p += l;
        if (i + 1 < g_mgr.search_count) {
            size_t sl = strlen(LOC_PATH_SEP);
            memcpy(p, LOC_PATH_SEP, sl);
            p += sl;
        }
    }
    *p = '\0';
    rt_rwlock_read_exit(g_mgr.lock);
    rt_string result = rt_string_from_bytes(buf, total);
    free(buf);
    return result;
}

/// @brief Append a directory path to the locale JSON search list.
/// @details Copies the path string so the caller's storage can be freed immediately.
///          Traps on OOM or embedded NUL bytes. Duplicate paths are ignored.
///          The paths are searched in insertion order by `rt_locale_manager_load`.
/// @param path rt_string directory path to append; empty or NULL is silently ignored.
void rt_locale_manager_add_search_path(rt_string path) {
    loc_mgr_ensure_init();
    const char *bytes = path ? rt_string_cstr(path) : NULL;
    int64_t len = path ? rt_str_len(path) : 0;
    if (!bytes || len <= 0)
        return;
    if ((uint64_t)len > (uint64_t)SIZE_MAX) {
        rt_trap("Zanna.Localization.LocaleManager: search path length overflow");
        return;
    }
    size_t path_len = (size_t)len;
    if (memchr(bytes, '\0', path_len)) {
        rt_trap("Zanna.Localization.LocaleManager: search path contains embedded NUL");
        return;
    }

    rt_rwlock_write_enter(g_mgr.lock);
    for (size_t i = 0; i < g_mgr.search_count; ++i) {
        if (strlen(g_mgr.search_paths[i]) == path_len &&
            memcmp(g_mgr.search_paths[i], bytes, path_len) == 0) {
            rt_rwlock_write_exit(g_mgr.lock);
            return;
        }
    }
    if (g_mgr.search_count == g_mgr.search_capacity) {
        size_t new_cap = g_mgr.search_capacity == 0 ? 4 : g_mgr.search_capacity * 2;
        if (g_mgr.search_capacity > SIZE_MAX / 2 ||
            new_cap > SIZE_MAX / sizeof(*g_mgr.search_paths)) {
            rt_rwlock_write_exit(g_mgr.lock);
            rt_trap("Zanna.Localization.LocaleManager: search path capacity overflow");
            return;
        }
        char **new_paths = (char **)realloc(g_mgr.search_paths, new_cap * sizeof(*new_paths));
        if (!new_paths) {
            rt_rwlock_write_exit(g_mgr.lock);
            rt_trap("Zanna.Localization.LocaleManager: search path grow failed");
            return;
        }
        g_mgr.search_paths = new_paths;
        g_mgr.search_capacity = new_cap;
    }
    char *copy = (char *)malloc(path_len + 1);
    if (!copy) {
        rt_rwlock_write_exit(g_mgr.lock);
        rt_trap("Zanna.Localization.LocaleManager: search path allocation failed");
        return;
    }
    memcpy(copy, bytes, path_len);
    copy[path_len] = '\0';
    g_mgr.search_paths[g_mgr.search_count++] = copy;
    rt_rwlock_write_exit(g_mgr.lock);
}

/// @brief Unload a registered non-baked locale from the registry.
/// @details Refuses to unload: baked records, the current locale, the system locale,
///          or records with remaining live references. Frees the arena on success.
/// @param locale Opaque Locale handle whose tag identifies the record to unload.
/// @return 1 if the record was removed; 0 if it could not be unloaded.
int8_t rt_locale_manager_unload(void *locale) {
    loc_mgr_ensure_init();
    if (!locale)
        return 0;
    rt_locale_t *target = (rt_locale_t *)locale;
    const char *tag = target->tag[0] ? target->tag : "root";

    rt_rwlock_write_enter(g_mgr.lock);
    // Refuse to unload baked entries or the current/system locale.
    rt_locale_t *cur = (rt_locale_t *)g_mgr.current;
    rt_locale_t *sys = (rt_locale_t *)g_mgr.system;
    if ((cur && strcmp(cur->tag, tag) == 0) || (sys && strcmp(sys->tag, tag) == 0)) {
        rt_rwlock_write_exit(g_mgr.lock);
        return 0;
    }
    size_t idx = (size_t)-1;
    for (size_t i = 0; i < g_mgr.count; ++i) {
        if (strcmp(g_mgr.entries[i].tag, tag) == 0) {
            idx = i;
            break;
        }
    }
    if (idx == (size_t)-1) {
        rt_rwlock_write_exit(g_mgr.lock);
        return 0;
    }
    if (g_mgr.entries[idx].is_baked) {
        rt_rwlock_write_exit(g_mgr.lock);
        return 0; // baked records are never unloaded
    }
    const rt_locale_data_t *data = g_mgr.entries[idx].data;
    int64_t refs = loc_data_ref_count(data);
    if (refs == 1 && target->data == data) {
        rt_locale_manager_release_data(target->data);
        target->data = NULL;
        refs = 0;
    }
    if (refs != 0) {
        rt_rwlock_write_exit(g_mgr.lock);
        return 0;
    }
    loc_registry_entry_t removed = g_mgr.entries[idx];
    for (size_t j = idx + 1; j < g_mgr.count; ++j)
        g_mgr.entries[j - 1] = g_mgr.entries[j];
    g_mgr.count--;
    rt_rwlock_write_exit(g_mgr.lock);
    loc_free_loaded_data(removed.data);
    return 1;
}

/// @brief Reset the manager to a clean state: current and system revert to en-US.
/// @details Clears all search paths and removes all non-baked, unreferenced locale
///          records. Records that still have live references are kept until those
///          references are released. The baked en-US record always survives.
///          Thread-safe; acquires the write lock twice (once for handle swap, once
///          for registry pruning).
void rt_locale_manager_reset(void) {
    loc_mgr_ensure_init();
    rt_rwlock_write_enter(g_mgr.lock);

    void *old_current = g_mgr.current;
    void *old_system = g_mgr.system;
    g_mgr.current = loc_make_handle_locked("en-US");
    g_mgr.system = loc_make_handle_locked("en-US");

    // Clear extra search paths (keep zero — user must re-add).
    for (size_t i = 0; i < g_mgr.search_count; ++i)
        free(g_mgr.search_paths[i]);
    g_mgr.search_count = 0;
    rt_rwlock_write_exit(g_mgr.lock);

    loc_release_handle(old_current);
    loc_release_handle(old_system);

    rt_rwlock_write_enter(g_mgr.lock);
    // Remove every non-baked entry not still retained by user-visible handles.
    // Baked entries survive Reset.
    size_t w = 0;
    for (size_t r = 0; r < g_mgr.count; ++r) {
        if (g_mgr.entries[r].is_baked) {
            g_mgr.entries[w++] = g_mgr.entries[r];
        } else if (g_mgr.entries[r].data && loc_data_ref_count(g_mgr.entries[r].data) == 0) {
            loc_free_loaded_data(g_mgr.entries[r].data);
        } else if (g_mgr.entries[r].data) {
            g_mgr.entries[w++] = g_mgr.entries[r];
        }
    }
    g_mgr.count = w;
    rt_rwlock_write_exit(g_mgr.lock);
}
