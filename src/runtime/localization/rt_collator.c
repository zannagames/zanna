//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_collator.c
// Purpose: Implementation of Zanna.Localization.Collator. Generates weighted
//          sort keys from UTF-8 input strings via the classifier in
//          rt_collator_table.c, then performs byte-wise comparison on the
//          key bytes. Three strength levels map to three concatenated weight
//          bands; IgnoreCase/IgnoreAccents elide the tertiary/secondary
//          bands from the comparison.
//
// Key invariants:
//   - SortKey output is deterministic for a given (string, locale, strength,
//     flags) combination. Byte-wise compare(SortKey(a), SortKey(b)) matches
//     Compare(a, b).
//   - Inputs longer than 1 MiB trap; raw sort-key allocation is bounded by
//     eight bytes per collected character plus fixed separators, and the
//     public hexadecimal form uses two output bytes per raw byte.
//   - Locale patches are applied at construction via a small O(1) lookup
//     per codepoint (linear scan over the patch list, which is <= 10 entries).
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated; GC-managed.
//
// Links: src/runtime/localization/rt_collator.h (interface),
//        src/runtime/localization/rt_collator_table.c (classifier),
//        src/runtime/localization/rt_locale_manager.h (current-locale lookup).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements locale-aware Collator comparison, keys, and stable sorting.
 * @details Converts UTF-8 strings into bounded primary, secondary, and
 * tertiary weight bands, applies construction-time locale tailoring, exposes
 * configurable strength and ignore flags, and preserves managed ownership
 * while sorting runtime lists.
 */

#include "rt_collator.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_locale_manager.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

// rt_locale_t struct layout lives in rt_locale.h; we reach in here to read
// the canonical tag so locale tailorings can apply even when a locale has
// been parsed but not yet registered (its data pointer falls through to the
// baked invariant, but its tag is still the parsed value).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Maximum UTF-8 byte length accepted by one collation input. */
#define MAX_INPUT_BYTES (1u << 20)

//===----------------------------------------------------------------------===//
// Instance struct
//===----------------------------------------------------------------------===//

/** GC payload storing one Collator's immutable locale and mutable options. */
typedef struct rt_collator {
    void *locale;                              ///< Retained Locale handle, if supplied.
    const rt_locale_data_t *data;              ///< Retained locale-data snapshot.
    int strength;                              ///< Active comparison depth from 1 through 3.
    int8_t ignore_case;                        ///< Whether tertiary case weights are omitted.
    int8_t ignore_accents;                     ///< Whether secondary accent weights are omitted.
    const rt_collator_locale_patch_t *patches; ///< Borrowed immutable locale overrides.
    size_t patch_count;                        ///< Number of records in @ref patches.
} rt_collator_t;

/// @brief Unchecked cast of an opaque handle to the collator instance.
/// @param obj Valid Collator payload.
/// @return The same pointer interpreted as @ref rt_collator_t.
static rt_collator_t *as_col(void *obj) {
    return (rt_collator_t *)obj;
}

/// @brief Emit a non-fatal collator warning to stderr (NULL message ignored).
/// @param message Diagnostic text without the warning prefix; may be NULL.
static void col_warn(const char *message) {
    if (message)
        fprintf(stderr, "warning: %s\n", message);
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Owned runtime-managed handle reference; NULL is a no-op.
static void col_release_handle(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief GC finalizer: release the collator's locale data and locale handle.
/// @details The data record and Locale handle were retained independently
///          during construction and are each released exactly once.
/// @param obj Collator payload being finalized; NULL is a no-op.
static void col_finalizer(void *obj) {
    rt_collator_t *c = (rt_collator_t *)obj;
    if (!c)
        return;
    rt_locale_manager_release_data(c->data);
    col_release_handle(c->locale);
    c->locale = NULL;
    c->data = NULL;
}

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// @brief Allocate and initialize a GC-managed collator for @p locale.
/// @details Retains the locale handle + its collation data, clamps strength to
///          1-3, and binds locale tailoring patches keyed by the *handle's*
///          tag (not the data record's) so e.g. Swedish ordering still applies
///          when no JSON locale file was loaded. Traps on allocation failure;
///          installs @ref col_finalizer.
/// @param locale Optional Locale handle to retain; NULL selects the invariant
///               locale data supplied by @ref rt_locale_get_data.
/// @return Fresh GC-managed Collator, or NULL after an allocation trap.
static void *col_alloc(void *locale) {
    rt_collator_t *c = (rt_collator_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_collator_t));
    if (!c) {
        rt_trap("Zanna.Localization.Collator: allocation failed");
        return NULL;
    }
    c->locale = locale;
    if (c->locale)
        rt_heap_retain(c->locale);
    c->data = rt_locale_get_data(locale);
    rt_locale_manager_retain_data(c->data);
    c->strength = c->data->collation.strength > 0 ? c->data->collation.strength : 3;
    if (c->strength > 3)
        c->strength = 3;
    c->ignore_case = 0;
    c->ignore_accents = 0;

    // Capture tailoring patches by locale tag. Read from the Locale handle
    // directly (not from `c->data->tag`) because data-records fall back to
    // the baked invariant when the locale isn't registered — which would
    // make Collator.ForLocale("sv-SE") silently skip the Swedish patches
    // just because no fr-FR-style JSON file was loaded.
    c->patches = NULL;
    c->patch_count = 0;
    if (locale) {
        rt_locale_t *l = (rt_locale_t *)locale;
        const char *tag = l->tag[0] ? l->tag : (c->data->tag ? c->data->tag : "");
        c->patches = rt_collator_locale_patches(tag, &c->patch_count);
    } else if (c->data->tag) {
        c->patches = rt_collator_locale_patches(c->data->tag, &c->patch_count);
    }

    rt_obj_set_finalizer(c, col_finalizer);
    return c;
}

/// @brief Create a Collator bound to the process's current Locale snapshot.
/// @details The temporary reference returned by the locale manager is released
///          after the Collator acquires its own Locale and data references.
/// @return Fresh GC-managed Collator, or NULL after an allocation trap.
void *rt_collator_new(void) {
    void *current = rt_locale_manager_current();
    void *col = col_alloc(current);
    col_release_handle(current);
    return col;
}

/// @brief Create a Collator bound to a specified Locale handle.
/// @param locale Locale to retain for the Collator lifetime; may be NULL for
///               invariant fallback data.
/// @return Fresh GC-managed Collator, or NULL after an allocation trap.
void *rt_collator_for_locale(void *locale) {
    return col_alloc(locale);
}

/// @brief Get the Locale handle retained by a Collator.
/// @param self Valid Collator handle; may be NULL.
/// @return Borrowed Locale handle, or NULL when no Locale was supplied.
void *rt_collator_get_locale(void *self) {
    return self ? as_col(self)->locale : NULL;
}

//===----------------------------------------------------------------------===//
// Property accessors
//===----------------------------------------------------------------------===//

/// @brief Get the active primary/secondary/tertiary comparison depth.
/// @param self Valid Collator handle; may be NULL.
/// @return Strength from 1 through 3, or the default 3 for NULL.
int64_t rt_collator_get_strength(void *self) {
    return self ? (int64_t)as_col(self)->strength : 3;
}

/// @brief Clamp and set the active comparison strength.
/// @details Values below 1 silently clamp to 1. Values above 3 emit a warning
///          to stderr and clamp to 3 because quaternary strength is unsupported.
/// @param self Valid Collator handle; NULL is a no-op.
/// @param value Requested strength.
void rt_collator_set_strength(void *self, int64_t value) {
    if (!self)
        return;
    if (value < 1)
        value = 1;
    if (value > 3) {
        // Strength 4 (quaternary) isn't supported in v1; clamp with a
        // diagnostic so callers see the downgrade.
        col_warn("Zanna.Localization.Collator: strength 4 unsupported; clamped to 3");
        value = 3;
    }
    as_col(self)->strength = (int)value;
}

/// @brief Test whether tertiary case weights are omitted.
/// @param self Valid Collator handle; may be NULL.
/// @return Normalized 1/0 flag, or 0 for NULL.
int8_t rt_collator_get_ignore_case(void *self) {
    return self ? as_col(self)->ignore_case : 0;
}

/// @brief Enable or disable omission of tertiary case weights.
/// @param self Valid Collator handle; NULL is a no-op.
/// @param value Zero to preserve case differences, nonzero to ignore them.
void rt_collator_set_ignore_case(void *self, int8_t value) {
    if (self)
        as_col(self)->ignore_case = value ? 1 : 0;
}

/// @brief Test whether secondary accent weights are omitted.
/// @param self Valid Collator handle; may be NULL.
/// @return Normalized 1/0 flag, or 0 for NULL.
int8_t rt_collator_get_ignore_accents(void *self) {
    return self ? as_col(self)->ignore_accents : 0;
}

/// @brief Enable or disable omission of secondary accent weights.
/// @param self Valid Collator handle; NULL is a no-op.
/// @param value Zero to preserve accent differences, nonzero to ignore them.
void rt_collator_set_ignore_accents(void *self, int8_t value) {
    if (self)
        as_col(self)->ignore_accents = value ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// UTF-8 decode
//===----------------------------------------------------------------------===//

/// @brief Decode one UTF-8 codepoint from @p s starting at @c *pos, advancing
///        @c *pos past it. Returns 0 at end of input.
/// @details Strict decoder: rejects overlong forms, surrogates (U+D800–DFFF),
///          and out-of-range scalars, yielding U+FFFD and advancing one byte
///          so a malformed stream still makes forward progress.
/// @param s Bounded UTF-8 byte string.
/// @param len Total readable bytes at @p s.
/// @param pos In/out byte offset, advanced past the decoded or rejected sequence.
/// @return Unicode scalar, U+FFFD for malformed input, or zero at end.
static uint32_t col_decode(const char *s, size_t len, size_t *pos) {
    size_t i = *pos;
    if (i >= len)
        return 0;
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    size_t need;
    uint32_t min_cp = 0;
    if (c < 0x80) {
        cp = c;
        need = 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        cp = c & 0x1F;
        need = 2;
        min_cp = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        cp = c & 0x0F;
        need = 3;
        min_cp = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        cp = c & 0x07;
        need = 4;
        min_cp = 0x10000;
    } else {
        *pos = i + 1;
        return 0xFFFD;
    }
    if (i + need > len) {
        *pos = len;
        return 0xFFFD;
    }
    for (size_t k = 1; k < need; ++k) {
        uint8_t nc = (uint8_t)s[i + k];
        if ((nc & 0xC0) != 0x80) {
            *pos = i + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (nc & 0x3F);
    }
    if ((need > 1 && cp < min_cp) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        *pos = i + 1;
        return 0xFFFD;
    }
    *pos = i + need;
    return cp;
}

//===----------------------------------------------------------------------===//
// Weight extraction (with locale patches)
//===----------------------------------------------------------------------===//

/// @brief Compute the primary/secondary/tertiary collation weights for @p cp.
/// @details Locale tailoring patches win over the base classifier; otherwise
///          weights come from the shared collation table plus the combining-
///          mark accent weight. Outputs feed the multi-level sort key.
/// @param col Collator providing locale-tailoring patches.
/// @param cp Unicode code point to classify.
/// @param pri Receives the primary ordering weight.
/// @param sec Receives the secondary accent weight.
/// @param ter Receives the tertiary case weight.
static void get_weights(
    rt_collator_t *col, uint32_t cp, uint32_t *pri, uint16_t *sec, uint16_t *ter) {
    // Apply any locale patches first (they override the base classifier).
    if (col->patches) {
        for (size_t i = 0; i < col->patch_count; ++i) {
            if (col->patches[i].codepoint == cp) {
                *pri = col->patches[i].primary_override;
                *sec = col->patches[i].secondary_override;
                *ter = col->patches[i].tertiary_override;
                return;
            }
        }
    }
    (void)rt_collator_codepoint_weights(cp, pri, sec, ter);
}

typedef struct sort_weight {
    uint32_t pri;
    uint16_t sec;
    uint16_t ter;
} sort_weight_t;

/// @brief Secondary collation weight for a combining diacritic codepoint.
/// @details Gives common accents (grave/acute/circumflex/…) a stable accent
///          ordering so e.g. "é" sorts after "e" but before "f"; any other
///          combining mark in U+0300–U+036F gets a generic weight (9), all
///          else 0 (no secondary contribution).
/// @param cp Unicode code point to classify.
/// @return Stable secondary weight, or zero when not a recognized combining mark.
static uint16_t combining_secondary(uint32_t cp) {
    switch (cp) {
        case 0x0300:
            return 1; // grave
        case 0x0301:
            return 2; // acute
        case 0x0302:
            return 3; // circumflex
        case 0x0303:
            return 4; // tilde
        case 0x0308:
            return 5; // diaeresis
        case 0x030A:
            return 6; // ring
        case 0x0327:
            return 7; // cedilla
        default:
            if (cp >= 0x0300 && cp <= 0x036F)
                return 9;
            return 0;
    }
}

/// @brief Build the array of per-character sort weights (primary + secondary
///        accent + tertiary case) for a UTF-8 string — the raw material of a
///        collation key. Caller frees the returned array; @p out_count gets
///        the element count. Traps on the 1 MiB input cap or allocation
///        failure (returns NULL).
/// @details Combining marks do not create elements: they raise the preceding
///          element's secondary weight, while leading combining marks are ignored.
///          Malformed UTF-8 contributes replacement-character fallback weights.
/// @param col Collator supplying base and locale-tailored weights.
/// @param s Bounded UTF-8 bytes.
/// @param len Number of readable bytes at @p s.
/// @param out_count Receives the number of collected non-combining elements.
/// @return Malloc-owned weight array for the caller to free, or NULL after a trap.
static sort_weight_t *collect_weights(rt_collator_t *col,
                                      const char *s,
                                      size_t len,
                                      size_t *out_count) {
    if (len > MAX_INPUT_BYTES) {
        rt_trap("Zanna.Localization.Collator: input exceeds 1 MiB cap");
        return NULL;
    }
    size_t cap = len > 0 ? len : 1;
    if (cap > SIZE_MAX / sizeof(sort_weight_t)) {
        rt_trap("Zanna.Localization.Collator: key allocation overflow");
        return NULL;
    }
    sort_weight_t *weights = (sort_weight_t *)malloc(cap * sizeof(sort_weight_t));
    if (!weights) {
        rt_trap("Zanna.Localization.Collator: key allocation failed");
        return NULL;
    }
    size_t count = 0;
    size_t p = 0;
    while (p < len) {
        uint32_t cp = col_decode(s, len, &p);
        uint16_t comb = combining_secondary(cp);
        if (comb) {
            if (count > 0 && weights[count - 1].sec == 0)
                weights[count - 1].sec = comb;
            else if (count > 0 && comb > weights[count - 1].sec)
                weights[count - 1].sec = comb;
            continue;
        }

        uint32_t pri = 0;
        uint16_t sec = 0, ter = 0;
        get_weights(col, cp, &pri, &sec, &ter);
        weights[count].pri = pri;
        weights[count].sec = sec;
        weights[count].ter = ter;
        ++count;
    }
    *out_count = count;
    return weights;
}

//===----------------------------------------------------------------------===//
// Raw sort key bytes
//===----------------------------------------------------------------------===//

/// @brief Build the raw sort key byte sequence for @p s. Each level's
///        weights are emitted in order with a 0x00 separator between levels.
///        Big-endian 32-bit emission for primaries preserves numeric ordering.
/// @details Secondary weights use two big-endian bytes and tertiary weights
///          use their low byte. Strength and ignore flags determine which
///          bands contain weights, but the two-byte band separators remain.
/// @param col Collator whose configuration controls key levels.
/// @param s Bounded UTF-8 input bytes.
/// @param len Number of readable bytes at @p s.
/// @param out_len Receives the raw key length on success.
/// @return Malloc-owned key bytes for the caller to free, or NULL after a trap.
static uint8_t *build_raw_key(rt_collator_t *col, const char *s, size_t len, size_t *out_len) {
    if (len > MAX_INPUT_BYTES) {
        rt_trap("Zanna.Localization.Collator: input exceeds 1 MiB cap");
        return NULL;
    }

    size_t n = 0;
    sort_weight_t *weights = collect_weights(col, s, len, &n);
    if (!weights)
        return NULL;

    // Per codepoint: up to 4 bytes primary + 2 bytes secondary + 1 byte tertiary,
    // plus 3 level separators (2 bytes each). Worst case ~7 bytes/cp + 6.
    if (n > (SIZE_MAX - 16) / 8) {
        free(weights);
        rt_trap("Zanna.Localization.Collator: key allocation overflow");
        return NULL;
    }
    size_t cap = n * 8 + 16;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        free(weights);
        rt_trap("Zanna.Localization.Collator: key allocation failed");
        return NULL;
    }
    size_t off = 0;

    // Primary level.
    {
        for (size_t i = 0; i < n; ++i) {
            uint32_t pri = weights[i].pri;
            if (col->strength >= 1) {
                // Big-endian 4-byte primary.
                buf[off++] = (uint8_t)((pri >> 24) & 0xFF);
                buf[off++] = (uint8_t)((pri >> 16) & 0xFF);
                buf[off++] = (uint8_t)((pri >> 8) & 0xFF);
                buf[off++] = (uint8_t)(pri & 0xFF);
            }
        }
    }
    buf[off++] = 0x00;
    buf[off++] = 0x00; // primary/secondary separator

    // Secondary level.
    if (col->strength >= 2 && !col->ignore_accents) {
        for (size_t i = 0; i < n; ++i) {
            uint16_t sec = weights[i].sec;
            buf[off++] = (uint8_t)((sec >> 8) & 0xFF);
            buf[off++] = (uint8_t)(sec & 0xFF);
        }
    }
    buf[off++] = 0x00;
    buf[off++] = 0x00; // secondary/tertiary separator

    // Tertiary level.
    if (col->strength >= 3 && !col->ignore_case) {
        for (size_t i = 0; i < n; ++i) {
            uint16_t ter = weights[i].ter;
            buf[off++] = (uint8_t)(ter & 0xFF);
        }
    }

    free(weights);
    *out_len = off;
    return buf;
}

//===----------------------------------------------------------------------===//
// Compare / Equals
//===----------------------------------------------------------------------===//

/// @brief Compare two runtime strings using configured collation keys.
/// @details NULL strings are treated as empty. Raw keys are compared bytewise
///          and then by length, producing only the normalized values -1, 0,
///          and 1. Key-generation failure returns 0 after cleanup.
/// @param self Valid Collator handle; NULL compares equal.
/// @param a First valid runtime string, or NULL for empty.
/// @param b Second valid runtime string, or NULL for empty.
/// @return -1 when @p a sorts first, 1 when @p b sorts first, otherwise 0.
int64_t rt_collator_compare(void *self, rt_string a, rt_string b) {
    if (!self)
        return 0;
    const char *as = a ? rt_string_cstr(a) : "";
    const char *bs = b ? rt_string_cstr(b) : "";
    int64_t alen = a ? rt_str_len(a) : 0;
    int64_t blen = b ? rt_str_len(b) : 0;

    size_t ka_len = 0, kb_len = 0;
    uint8_t *ka = build_raw_key(as_col(self), as, (size_t)alen, &ka_len);
    uint8_t *kb = build_raw_key(as_col(self), bs, (size_t)blen, &kb_len);
    if (!ka || !kb) {
        free(ka);
        free(kb);
        return 0;
    }

    size_t min_len = ka_len < kb_len ? ka_len : kb_len;
    int cmp = min_len > 0 ? memcmp(ka, kb, min_len) : 0;
    if (cmp == 0 && ka_len != kb_len)
        cmp = ka_len < kb_len ? -1 : 1;

    free(ka);
    free(kb);

    if (cmp < 0)
        return -1;
    if (cmp > 0)
        return 1;
    return 0;
}

/// @brief Test collation equality at the Collator's configured strength.
/// @param self Valid Collator handle; NULL treats all inputs as equal.
/// @param a First valid runtime string, or NULL for empty.
/// @param b Second valid runtime string, or NULL for empty.
/// @return 1 when @ref rt_collator_compare returns zero, otherwise 0.
int8_t rt_collator_equals(void *self, rt_string a, rt_string b) {
    return rt_collator_compare(self, a, b) == 0 ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// SortKey (hex-encoded)
//===----------------------------------------------------------------------===//

/// @brief Generate a persistent hexadecimal representation of a raw collation key.
/// @details Lowercase two-digit hex encoding avoids embedded NUL while
///          preserving raw byte ordering under bytewise string comparison.
///          Invalid input, key failure, or builder failure returns a fresh
///          empty runtime string.
/// @param self Valid Collator handle.
/// @param s Valid runtime string to encode.
/// @return Fresh runtime string containing the deterministic hexadecimal key.
rt_string rt_collator_sort_key(void *self, rt_string s) {
    if (!self || !s)
        return rt_string_from_bytes("", 0);
    const char *cs = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!cs || len < 0)
        return rt_string_from_bytes("", 0);

    size_t key_len = 0;
    uint8_t *key = build_raw_key(as_col(self), cs, (size_t)len, &key_len);
    if (!key)
        return rt_string_from_bytes("", 0);

    // Hex-encode so callers can store the result in an rt_string without
    // embedded NUL issues. Byte-wise comparison of hex strings preserves
    // the same ordering as the raw key bytes.
    rt_string_builder sb;
    rt_sb_init(&sb);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < key_len; ++i) {
        char pair[2] = {hex[(key[i] >> 4) & 0xF], hex[key[i] & 0xF]};
        if (rt_sb_append_bytes(&sb, pair, 2) != RT_SB_OK)
            goto sort_key_error;
    }
    free(key);

    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;

sort_key_error:
    free(key);
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}

//===----------------------------------------------------------------------===//
// Sort (simple insertion sort — fine for lists up to a few thousand)
//===----------------------------------------------------------------------===//

/// @brief Return a stable collation-sorted copy of a runtime string List.
/// @details One raw key is cached per item, then stable insertion sort
///          preserves the relative order of equal keys. The input List is not
///          mutated. NULL input/self and failures yield a fresh empty List
///          after any applicable trap.
/// @param self Valid Collator handle.
/// @param items Runtime List whose elements are valid strings or NULL.
/// @return Fresh runtime List containing retained elements in collation order.
void *rt_collator_sort(void *self, void *items) {
    if (!self || !items)
        return rt_list_new();
    int64_t n = rt_list_len(items);
    void *out = rt_list_new();
    if (n <= 0)
        return out;

    typedef struct sort_item {
        rt_string value;
        uint8_t *key;
        size_t key_len;
    } sort_item_t;

    sort_item_t *arr = (sort_item_t *)calloc((size_t)n, sizeof(sort_item_t));
    if (!arr) {
        rt_trap("Zanna.Localization.Collator: Sort allocation failed");
        return out;
    }
    for (int64_t i = 0; i < n; ++i) {
        arr[i].value = (rt_string)rt_list_get(items, i);
        const char *cs = arr[i].value ? rt_string_cstr(arr[i].value) : "";
        int64_t len = arr[i].value ? rt_str_len(arr[i].value) : 0;
        arr[i].key = build_raw_key(as_col(self), cs, (size_t)len, &arr[i].key_len);
        if (!arr[i].key) {
            for (int64_t j = 0; j < i; ++j)
                free(arr[j].key);
            free(arr);
            return out;
        }
    }

    // Stable insertion sort using cached keys. This avoids rebuilding sort
    // keys O(n^2) times and keeps equal-key input order intact.
    for (int64_t i = 1; i < n; ++i) {
        sort_item_t key = arr[i];
        int64_t j = i - 1;
        while (j >= 0) {
            size_t min_len = arr[j].key_len < key.key_len ? arr[j].key_len : key.key_len;
            int cmp = min_len > 0 ? memcmp(arr[j].key, key.key, min_len) : 0;
            if (cmp == 0 && arr[j].key_len != key.key_len)
                cmp = arr[j].key_len < key.key_len ? -1 : 1;
            if (cmp <= 0)
                break;
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }

    for (int64_t i = 0; i < n; ++i)
        rt_list_push(out, arr[i].value);

    for (int64_t i = 0; i < n; ++i)
        free(arr[i].key);
    free(arr);
    return out;
}
