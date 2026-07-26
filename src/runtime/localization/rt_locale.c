//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale.c
// Purpose: Implementation of the Zanna.Localization.Locale class: BCP-47
//          parsing, canonicalization, and the fallback-chain walk. Integrates
//          with LocaleManager only loosely — registry lookup happens via
//          rt_locale_manager_lookup_data which may return NULL for tags that
//          haven't been loaded yet; in that case the Locale's `data` pointer
//          stays NULL and queries fall through to the invariant.
//
// Key invariants:
//   - Parsing is strict about structure but lenient about case and separators:
//     accepts underscores, accepts mixed case, rejects content that can't be
//     canonicalized to a valid BCP-47 shape.
//   - Fallbacks always terminate in the invariant ("root") entry; the walk
//     never loops even on malformed handles because each step strips one
//     subtag at a minimum.
//   - Locale handles are allocated via rt_obj_new_i64 so they participate in
//     the GC lifetime system; the finalizer releases the handle's retained
//     locale-data reference back to the LocaleManager registry.
//
// Ownership/Lifetime:
//   - Handles own their subtag string bytes (embedded in the struct).
//   - `data` is a retained reference to a registry-owned record; the handle
//     releases it on finalize and the registry remains the allocator/owner.
//
// Links: src/runtime/localization/rt_locale.h (interface),
//        src/runtime/localization/rt_locale_manager.h (registry bridge),
//        src/runtime/localization/rt_locale_data_en_us.c (invariant fallback).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements immutable BCP-47 Locale parsing and fallback traversal.
 * @details Validates and canonicalizes language, extlang, script, region,
 * variant, extension, and private-use subtags, manages retained registry data,
 * exposes value queries, and constructs terminating locale fallback chains.
 */

#include "rt_locale.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_locale_data.h"
#include "rt_locale_manager.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Static helpers: character classification / case normalization
//===----------------------------------------------------------------------===//

/// @brief Check whether @p c is ASCII alpha. BCP-47 only accepts ASCII.
/// @param c Byte to classify.
/// @return 1 for `A`-`Z` or `a`-`z`, otherwise 0.
static int loc_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/// @brief Check whether @p c is an ASCII decimal digit ('0'–'9').
/// @param c Byte to classify.
/// @return 1 for `0` through `9`, otherwise 0.
static int loc_is_digit(char c) {
    return c >= '0' && c <= '9';
}

/// @brief Check whether @p c is ASCII alphanumeric (alpha or digit).
/// @param c Byte to classify.
/// @return 1 for an ASCII letter or decimal digit, otherwise 0.
static int loc_is_alnum(char c) {
    return loc_is_alpha(c) || loc_is_digit(c);
}

/// @brief Convert an ASCII uppercase letter to lowercase; pass through everything else.
/// @param c Byte to normalize.
/// @return Lowercase ASCII equivalent or the unchanged byte.
static char loc_to_lower(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

/// @brief Convert an ASCII lowercase letter to uppercase; pass through everything else.
/// @param c Byte to normalize.
/// @return Uppercase ASCII equivalent or the unchanged byte.
static char loc_to_upper(char c) {
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

/// @brief Check whether @p c is a BCP-47 subtag separator ('-' or '_').
/// @param c Byte to classify.
/// @return 1 for hyphen or underscore, otherwise 0.
static int loc_is_separator(char c) {
    return c == '-' || c == '_';
}

/// @brief True when the byte span contains any subtag separator.
/// @param s Bounded input bytes.
/// @param len Number of readable bytes at @p s.
/// @return 1 when any byte is hyphen or underscore, otherwise 0.
static int loc_str_has_separator(const char *s, size_t len) {
    for (size_t i = 0; i < len; ++i)
        if (loc_is_separator(s[i]))
            return 1;
    return 0;
}

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

/// @brief Split the input tag into subtags and classify each.
/// @details Accepts '-' and '_' as separators but rejects empty subtags, so
///          leading, trailing, and duplicate separators are invalid. The parser
///          implements the BCP-47 shape used by the runtime: language,
///          optional script, optional region, optional variants, optional
///          extensions, and optional private-use tail. Variant / extension
///          subtags are preserved in `tag` but do not affect Locale's
///          language/script/region behavioural queries.
/// @param input Bounded candidate tag bytes; no terminating NUL is required.
/// @param input_len Number of readable bytes at @p input.
/// @param out Destination zeroed before parsing and populated only as parsing advances.
/// @return 0 on success, -1 on structural failure.
int rt_locale_internal_parse_into(const char *input, size_t input_len, rt_locale_t *out);

/// @brief Parse and canonicalize the runtime's supported BCP-47 tag shape.
/// @details Accepts hyphen or underscore separators and canonicalizes language,
///          script, region, variants, extensions, and private use. Empty
///          subtags, invalid characters/shapes, duplicate variants or
///          extension singletons, over-capacity output, and incomplete
///          extensions are rejected.
/// @param input Bounded candidate tag bytes; no terminating NUL is required.
/// @param input_len Number of readable bytes at @p input.
/// @param out Destination zeroed before parsing and populated on success.
/// @return 0 on success, or -1 on structural/capacity failure.
int rt_locale_internal_parse_into(const char *input, size_t input_len, rt_locale_t *out) {
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!input || input_len == 0)
        return -1;
    if (input_len > RT_LOCALE_TAG_CAP * 2) // allow some slack for case/underscore input
        return -1;

    if (loc_is_separator(input[0]) || loc_is_separator(input[input_len - 1]))
        return -1;

    enum { MAX_SUBTAGS = 32 };

    char subtags[MAX_SUBTAGS][9];
    size_t sub_lens[MAX_SUBTAGS];
    size_t sub_count = 0;
    size_t i = 0;
    while (i < input_len) {
        if (sub_count >= MAX_SUBTAGS)
            return -1;
        if (loc_is_separator(input[i]))
            return -1;
        size_t start = i;
        while (i < input_len && !loc_is_separator(input[i]))
            ++i;
        size_t sub_len = i - start;
        if (sub_len == 0 || sub_len > 8)
            return -1;
        for (size_t k = 0; k < sub_len; ++k) {
            if (!loc_is_alnum(input[start + k]))
                return -1;
            subtags[sub_count][k] = input[start + k];
        }
        subtags[sub_count][sub_len] = '\0';
        sub_lens[sub_count] = sub_len;
        ++sub_count;
        if (i < input_len) {
            if (!loc_is_separator(input[i]))
                return -1;
            ++i;
            if (i >= input_len || loc_is_separator(input[i]))
                return -1;
        }
    }
    if (sub_count == 0)
        return -1;

    // Special invariant tag.
    if (sub_count == 1 && sub_lens[0] == 4) {
        char root[5];
        for (size_t k = 0; k < 4; ++k)
            root[k] = loc_to_lower(subtags[0][k]);
        root[4] = '\0';
        if (strcmp(root, "root") == 0) {
            memcpy(out->language, "root", 5);
            memcpy(out->tag, "root", 5);
            return 0;
        }
    }

    int subtag_idx = 0;
    size_t canonical_len = 0;
    char canonical[RT_LOCALE_TAG_CAP];
    canonical[0] = '\0';
    int after_extension = 0;
    int saw_private = 0;
    int saw_variant = 0;
    int extlang_count = 0;
    int ext_needs_subtag = 0; // last extension singleton still lacks a subtag
    uint64_t extension_singletons = 0;
    char variants_seen[MAX_SUBTAGS][9];
    size_t variants_seen_lens[MAX_SUBTAGS];
    size_t variants_seen_count = 0;

    // Pure private-use tag: `x-...` with no primary language (RFC 5646
    // `privateuse`). Language/script/region stay empty; only the canonical
    // tag carries content (VDOC-065).
    int private_only = sub_lens[0] == 1 && (subtags[0][0] == 'x' || subtags[0][0] == 'X');

    for (size_t st = 0; st < sub_count; ++st) {
        char *sub = subtags[st];
        size_t sub_len = sub_lens[st];
        int is_script_subtag = 0;
        int is_region_subtag = 0;

        // Classification: position-sensitive, per RFC 5646 conventions
        if (subtag_idx == 0 && private_only) {
            if (st + 1 >= sub_count)
                return -1;
            saw_private = 1;
        } else if (subtag_idx == 0) {
            // Primary language: 2-3 alpha (occasionally 4-8 for registered)
            if (sub_len < 2 || sub_len > 8)
                return -1;
            for (size_t k = 0; k < sub_len; ++k)
                if (!loc_is_alpha(sub[k]))
                    return -1;
            if (sub_len + 1 > RT_LOCALE_LANG_CAP)
                return -1;
            for (size_t k = 0; k < sub_len; ++k)
                out->language[k] = loc_to_lower(sub[k]);
            out->language[sub_len] = '\0';
        } else if (!after_extension && !saw_private && !saw_variant && sub_len == 3 &&
                   out->script[0] == '\0' && out->region[0] == '\0' && extlang_count < 3 &&
                   (size_t)subtag_idx == (size_t)(1 + extlang_count) &&
                   strlen(out->language) <= 3 && loc_is_alpha(sub[0]) && loc_is_alpha(sub[1]) &&
                   loc_is_alpha(sub[2])) {
            // Extended language subtag: up to three 3-letter subtags directly
            // after a 2-3 letter primary language (e.g. zh-cmn-Hans-CN).
            extlang_count++;
        } else if (!after_extension && !saw_private && !saw_variant && sub_len == 4 &&
                   out->script[0] == '\0' && out->region[0] == '\0' && loc_is_alpha(sub[0]) &&
                   loc_is_alpha(sub[1]) && loc_is_alpha(sub[2]) && loc_is_alpha(sub[3])) {
            // Script: exactly 4 letters, Title-case
            is_script_subtag = 1;
            out->script[0] = loc_to_upper(sub[0]);
            for (size_t k = 1; k < 4; ++k)
                out->script[k] = loc_to_lower(sub[k]);
            out->script[4] = '\0';
        } else if (!after_extension && !saw_private && !saw_variant && out->region[0] == '\0' &&
                   ((sub_len == 2 && loc_is_alpha(sub[0]) && loc_is_alpha(sub[1])) ||
                    (sub_len == 3 && loc_is_digit(sub[0]) && loc_is_digit(sub[1]) &&
                     loc_is_digit(sub[2])))) {
            // Region: 2 letters or 3 digits
            is_region_subtag = 1;
            if (sub_len + 1 > RT_LOCALE_REGION_CAP)
                return -1;
            for (size_t k = 0; k < sub_len; ++k)
                out->region[k] = loc_is_alpha(sub[k]) ? loc_to_upper(sub[k]) : sub[k];
            out->region[sub_len] = '\0';
        } else {
            int is_singleton = sub_len == 1 && loc_is_alnum(sub[0]);
            int is_private_singleton = is_singleton && (sub[0] == 'x' || sub[0] == 'X');
            if (saw_private) {
                if (sub_len < 1 || sub_len > 8)
                    return -1;
            } else if (is_private_singleton) {
                // An extension singleton must own at least one subtag before
                // the private-use section starts (rejects `en-a-x-foo`).
                if (ext_needs_subtag)
                    return -1;
                if (st + 1 >= sub_count)
                    return -1;
                saw_private = 1;
            } else if (is_singleton) {
                // Back-to-back singletons form an empty extension
                // (rejects `en-a-b-foo`).
                if (ext_needs_subtag)
                    return -1;
                if (st + 1 >= sub_count)
                    return -1;
                char sc = loc_to_lower(sub[0]);
                int bit = loc_is_digit(sc) ? (sc - '0') : (10 + (sc - 'a'));
                if (bit < 0 || bit >= 64)
                    return -1;
                uint64_t mask = UINT64_C(1) << bit;
                if (extension_singletons & mask)
                    return -1;
                extension_singletons |= mask;
                after_extension = 1;
                ext_needs_subtag = 1;
            } else if (after_extension) {
                if (sub_len < 2 || sub_len > 8)
                    return -1;
                ext_needs_subtag = 0;
            } else {
                int valid_variant =
                    (sub_len >= 5 && sub_len <= 8) || (sub_len == 4 && loc_is_digit(sub[0]));
                if (!valid_variant)
                    return -1;
                // Duplicate variants are invalid (rejects `sl-rozaj-rozaj`).
                char lowered[9];
                for (size_t k = 0; k < sub_len; ++k)
                    lowered[k] = loc_to_lower(sub[k]);
                lowered[sub_len] = '\0';
                for (size_t v = 0; v < variants_seen_count; ++v) {
                    if (variants_seen_lens[v] == sub_len &&
                        memcmp(variants_seen[v], lowered, sub_len) == 0)
                        return -1;
                }
                memcpy(variants_seen[variants_seen_count], lowered, sub_len + 1);
                variants_seen_lens[variants_seen_count] = sub_len;
                variants_seen_count++;
                saw_variant = 1;
            }
        }

        // Append subtag to canonical tag with '-' separator and proper casing.
        if (canonical_len > 0) {
            if (canonical_len + 1 >= RT_LOCALE_TAG_CAP)
                return -1;
            canonical[canonical_len++] = '-';
            canonical[canonical_len] = '\0';
        }
        if (canonical_len + sub_len + 1 > RT_LOCALE_TAG_CAP)
            return -1;
        for (size_t k = 0; k < sub_len; ++k) {
            char c = sub[k];
            // Apply canonical casing by position.
            if (subtag_idx == 0)
                c = loc_to_lower(c);
            else if (is_script_subtag && k == 0)
                c = loc_to_upper(c);
            else if (is_script_subtag)
                c = loc_to_lower(c);
            else if (is_region_subtag && loc_is_alpha(c))
                c = loc_to_upper(c);
            else
                c = loc_to_lower(c);
            canonical[canonical_len + k] = c;
        }
        canonical_len += sub_len;
        canonical[canonical_len] = '\0';

        ++subtag_idx;
    }

    if (out->language[0] == '\0' && !private_only)
        return -1;
    // A trailing extension singleton with no subtags is invalid (`en-a`).
    if (ext_needs_subtag)
        return -1;

    if (canonical_len + 1 > RT_LOCALE_TAG_CAP)
        return -1;
    memcpy(out->tag, canonical, canonical_len + 1);
    return 0;
}

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// @brief GC finalizer for Locale handles — releases the retained locale-data pointer.
/// @details Called by the GC when the Locale handle's reference count drops to zero.
///          Releases the handle's retained `data` reference back to the LocaleManager registry
///          so the data record's formatter_refs counter stays accurate.
/// @param obj Pointer to the rt_locale_t being collected; NULL is safe.
void rt_locale_internal_finalizer(void *obj);

/// @brief Release the locale-data record retained by a Locale payload.
/// @param obj Locale being finalized; NULL is a no-op.
void rt_locale_internal_finalizer(void *obj) {
    rt_locale_t *l = (rt_locale_t *)obj;
    if (!l)
        return;
    rt_locale_manager_release_data(l->data);
    l->data = NULL;
}

/// @brief Allocate a zero-initialised rt_locale_t handle and register its finalizer.
/// @details Uses `rt_obj_new_i64` so the handle participates in the GC lifetime system.
///          Traps on OOM (never returns NULL to the caller in practice).
/// @return Pointer to a fresh, zeroed rt_locale_t registered with the GC.
static void *loc_alloc(void) {
    rt_locale_t *loc = (rt_locale_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_locale_t));
    if (!loc) {
        rt_trap("Zanna.Localization.Locale: memory allocation failed");
        return NULL; // unreachable after trap
    }
    memset(loc, 0, sizeof(*loc));
    rt_obj_set_finalizer(loc, rt_locale_internal_finalizer);
    return loc;
}

/// @brief Decrement the GC reference count on a Locale handle and free it if it hits zero.
/// @param obj Opaque Locale handle, or NULL (safe to call).
static void loc_release_handle(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Produce the canonical invariant ("root") locale handle.
/// @details Language/script/region remain empty, the tag is `root`, and the
///          data pointer references the baked invariant en-US record.
/// @return Fresh GC-managed Locale; allocation failure traps.
static void *loc_make_invariant(void) {
    rt_locale_t *loc = (rt_locale_t *)loc_alloc();
    memcpy(loc->tag, "root", 5);
    loc->data = rt_locale_data_en_us();
    return loc;
}

/// @brief Allocate a Locale handle from a known-canonical BCP-47 tag string.
/// @details Falls back to the invariant locale when @p tag is NULL, "root", or
///          fails to parse. Calls `rt_locale_manager_lookup_data_retained` so the
///          returned handle holds a retained reference to any matching data record.
/// @param tag NUL-terminated canonical BCP-47 tag (e.g., "en-US"), or NULL.
/// @return A fresh, GC-tracked Locale handle; never NULL.
static void *loc_from_canonical_tag(const char *tag) {
    if (!tag || strcmp(tag, "root") == 0)
        return loc_make_invariant();
    rt_locale_t parsed;
    if (rt_locale_internal_parse_into(tag, strlen(tag), &parsed) != 0)
        return loc_make_invariant();
    rt_locale_t *loc = (rt_locale_t *)loc_alloc();
    *loc = parsed;
    loc->data = rt_locale_manager_lookup_data_retained(loc->tag);
    return loc;
}

/// @brief Push @p loc onto @p list and immediately release the caller's reference.
/// @details The list takes ownership via `rt_list_push`, so the caller's local
///          reference is released to avoid a double-retain. NULL @p loc is silently
///          ignored to simplify the fallback-chain loop.
/// @param list rt_list handle to append to.
/// @param loc  Locale handle to transfer; may be NULL.
static void loc_list_push_owned(void *list, void *loc) {
    if (!loc)
        return;
    rt_list_push(list, loc);
    loc_release_handle(loc);
}

/// @brief Construct a new Locale handle initialised to the invariant ("root") locale.
/// @return A fresh, GC-tracked Locale handle pointing at the en-US invariant data.
void *rt_locale_new(void) {
    return loc_make_invariant();
}

/// @brief Return a Locale handle for the invariant ("root") locale.
/// @details Equivalent to `rt_locale_new()`; provided as a named entry-point for
///          the `Locale.Invariant` property path in the Zia runtime binding.
/// @return A fresh, GC-tracked Locale handle for the root/invariant locale.
void *rt_locale_invariant(void) {
    return loc_make_invariant();
}

/// @brief Parse a BCP-47 tag string into a Locale handle.
/// @details When @p strict is non-zero, traps on empty/invalid input (matches
///          the `Locale.Parse` contract). When @p strict is zero, returns NULL
///          on any failure (matches `Locale.TryParse`). On success resolves and
///          retains any matching locale-data record from the LocaleManager.
/// @param tag    rt_string containing the BCP-47 tag; NULL treated as empty.
/// @param strict 1 to trap on failure; 0 to return NULL silently.
/// @return A fresh, GC-tracked Locale handle on success; NULL on failure (non-strict).
void *rt_locale_parse_internal(rt_string tag, int strict) {
    const char *bytes = tag ? rt_string_cstr(tag) : NULL;
    int64_t sl = tag ? rt_str_len(tag) : 0;

    // Accept empty/NULL as the invariant locale when non-strict, or trap
    // when strict (matches the plan's "Parse traps, TryParse returns NULL").
    if (!bytes || sl <= 0) {
        if (strict) {
            rt_trap("Zanna.Localization.Locale: invalid BCP-47 tag '' (empty)");
            return NULL;
        }
        return NULL;
    }

    rt_locale_t parsed;
    if (rt_locale_internal_parse_into(bytes, (size_t)sl, &parsed) != 0) {
        if (strict) {
            rt_trap("Zanna.Localization.Locale: invalid BCP-47 tag (parse failed)");
            return NULL;
        }
        return NULL;
    }

    // Special case: "root" is allowed and maps to the invariant locale.
    if (strcmp(parsed.language, "root") == 0 && parsed.script[0] == '\0' &&
        parsed.region[0] == '\0') {
        return loc_make_invariant();
    }

    rt_locale_t *loc = (rt_locale_t *)loc_alloc();
    *loc = parsed;

    // Try to resolve registered locale-data.
    loc->data = rt_locale_manager_lookup_data_retained(loc->tag);
    return loc;
}

/// @brief Parse a BCP-47 tag, trapping if the tag is invalid or empty.
/// @details Implements `Locale.Parse(tag)` — the throwing variant. Use
///          `rt_locale_try_parse` when failure should return NULL instead.
/// @param tag rt_string containing the BCP-47 tag.
/// @return A fresh, GC-tracked Locale handle; never returns on invalid input.
void *rt_locale_parse(rt_string tag) {
    return rt_locale_parse_internal(tag, /*strict=*/1);
}

/// @brief Attempt to parse a BCP-47 tag, returning NULL on failure instead of trapping.
/// @details Implements `Locale.TryParse(tag)` — the non-throwing variant.
/// @param tag rt_string containing the BCP-47 tag; NULL or empty returns NULL.
/// @return A fresh, GC-tracked Locale handle, or NULL if the tag is invalid.
void *rt_locale_try_parse(rt_string tag) {
    return rt_locale_parse_internal(tag, /*strict=*/0);
}

/// @brief Attempt to parse a BCP-47 tag and return an Option.
/// @details Returns `Some(Locale)` when the tag parses successfully and `None`
///          for NULL, empty, or invalid input. The temporary Locale handle
///          produced by @ref rt_locale_try_parse is released after the Option
///          has retained it.
/// @param tag rt_string containing the BCP-47 tag; NULL or empty returns None.
/// @return Opaque Zanna.Option containing a Locale handle, or None.
void *rt_locale_try_parse_option(rt_string tag) {
    void *locale = rt_locale_try_parse(tag);
    if (!locale)
        return rt_option_none();
    void *option = rt_option_some(locale);
    if (rt_obj_release_check0(locale))
        rt_obj_free(locale);
    return option;
}

/// @brief Construct a Locale from separate language, script, and region subtag strings.
/// @details Assembles the subtags into a synthetic BCP-47 tag and routes through
///          the normal parser so validation and canonicalization are applied exactly
///          once. Traps if @p language is empty, or if any subtag combination would
///          fail BCP-47 validation. @p script and @p region may be NULL or empty.
/// @param language Required BCP-47 primary language subtag (e.g., "en").
/// @param script   Optional 4-letter script subtag (e.g., "Hans"), or NULL.
/// @param region   Optional 2-letter/3-digit region subtag (e.g., "US"), or NULL.
/// @return A fresh, GC-tracked Locale handle; traps on invalid input.
void *rt_locale_from_parts(rt_string language, rt_string script, rt_string region) {
    const char *ls = language ? rt_string_cstr(language) : NULL;
    int64_t ll = language ? rt_str_len(language) : 0;
    if (!ls || ll <= 0) {
        rt_trap("Zanna.Localization.Locale: language subtag required");
        return NULL;
    }

    // Each argument must be exactly one pre-split subtag: embedded
    // separators would let a caller smuggle extra components through any
    // field (VDOC-066).
    if (loc_str_has_separator(ls, (size_t)ll)) {
        rt_trap("Zanna.Localization.Locale: language must be a single subtag");
        return NULL;
    }

    // Build a canonical string and reuse the parser — keeps validation logic
    // in one place and ensures FromParts and Parse agree byte-for-byte.
    char buf[RT_LOCALE_TAG_CAP];
    size_t pos = 0;
    if ((size_t)ll >= sizeof(buf)) {
        rt_trap("Zanna.Localization.Locale: language subtag too long");
        return NULL;
    }
    memcpy(buf, ls, (size_t)ll);
    pos = (size_t)ll;

    int script_given = 0;
    int region_given = 0;
    if (script) {
        const char *ss = rt_string_cstr(script);
        int64_t sll = rt_str_len(script);
        if (ss && sll > 0) {
            if (loc_str_has_separator(ss, (size_t)sll)) {
                rt_trap("Zanna.Localization.Locale: script must be a single subtag");
                return NULL;
            }
            script_given = 1;
            if (pos + 1 + (size_t)sll >= sizeof(buf)) {
                rt_trap("Zanna.Localization.Locale: script subtag overflow");
                return NULL;
            }
            buf[pos++] = '-';
            memcpy(buf + pos, ss, (size_t)sll);
            pos += (size_t)sll;
        }
    }
    if (region) {
        const char *rs = rt_string_cstr(region);
        int64_t rl = rt_str_len(region);
        if (rs && rl > 0) {
            if (loc_str_has_separator(rs, (size_t)rl)) {
                rt_trap("Zanna.Localization.Locale: region must be a single subtag");
                return NULL;
            }
            region_given = 1;
            if (pos + 1 + (size_t)rl >= sizeof(buf)) {
                rt_trap("Zanna.Localization.Locale: region subtag overflow");
                return NULL;
            }
            buf[pos++] = '-';
            memcpy(buf + pos, rs, (size_t)rl);
            pos += (size_t)rl;
        }
    }
    buf[pos] = '\0';

    rt_locale_t parsed;
    if (rt_locale_internal_parse_into(buf, pos, &parsed) != 0) {
        rt_trap("Zanna.Localization.Locale: invalid subtag combination");
        return NULL;
    }
    // The parser classifies by shape; verify each supplied part landed in
    // the field the caller named (rejects FromParts("en", "US", "")).
    if (parsed.language[0] == '\0') {
        rt_trap("Zanna.Localization.Locale: language is not a valid language subtag");
        return NULL;
    }
    if (script_given && parsed.script[0] == '\0') {
        rt_trap("Zanna.Localization.Locale: script is not a valid script subtag");
        return NULL;
    }
    if (region_given && parsed.region[0] == '\0') {
        rt_trap("Zanna.Localization.Locale: region is not a valid region subtag");
        return NULL;
    }
    rt_locale_t *loc = (rt_locale_t *)loc_alloc();
    *loc = parsed;
    loc->data = rt_locale_manager_lookup_data_retained(loc->tag);
    return loc;
}

//===----------------------------------------------------------------------===//
// Accessors
//===----------------------------------------------------------------------===//

/// @brief Convert a NUL-terminated C string to a freshly allocated rt_string.
/// @details NULL input produces an empty rt_string. Unlike `loc_info_str` in
///          rt_locale_info.c, this variant does not treat "" as "missing" — the
///          empty string is a valid output for a Locale with no script or region.
/// @param s NUL-terminated C string, or NULL.
/// @return A freshly allocated rt_string copy of @p s, or an empty string for NULL.
static rt_string loc_str(const char *s) {
    if (!s)
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(s, strlen(s));
}

/// @brief Return the primary language subtag of @p locale (e.g., "en").
/// @details Returns an empty string for a NULL handle or the invariant locale.
/// @param locale Opaque Locale handle; may be NULL.
/// @return A freshly allocated rt_string with the language subtag.
rt_string rt_locale_language(void *locale) {
    if (!locale)
        return rt_string_from_bytes("", 0);
    return loc_str(((rt_locale_t *)locale)->language);
}

/// @brief Return the script subtag of @p locale (e.g., "Hans"), or empty if absent.
/// @param locale Opaque Locale handle; may be NULL.
/// @return A freshly allocated rt_string with the 4-letter script subtag, or empty.
rt_string rt_locale_script(void *locale) {
    if (!locale)
        return rt_string_from_bytes("", 0);
    return loc_str(((rt_locale_t *)locale)->script);
}

/// @brief Return the region subtag of @p locale (e.g., "US"), or empty if absent.
/// @param locale Opaque Locale handle; may be NULL.
/// @return A freshly allocated rt_string with the 2-letter or 3-digit region subtag, or empty.
rt_string rt_locale_region(void *locale) {
    if (!locale)
        return rt_string_from_bytes("", 0);
    return loc_str(((rt_locale_t *)locale)->region);
}

/// @brief Return the full canonical BCP-47 tag for @p locale (e.g., "en-US").
/// @details Returns "root" for a NULL handle or a Locale with no language subtag.
/// @param locale Opaque Locale handle; may be NULL.
/// @return A freshly allocated rt_string with the canonical tag.
rt_string rt_locale_tag(void *locale) {
    if (!locale)
        return rt_string_from_bytes("root", 4);
    rt_locale_t *l = (rt_locale_t *)locale;
    if (l->tag[0] == '\0')
        return rt_string_from_bytes("root", 4);
    return loc_str(l->tag);
}

/// @brief Return a string representation of @p locale (identical to `rt_locale_tag`).
/// @details Provided as the `ToString()` entry-point for the Zia object model.
/// @param locale Opaque Locale handle; may be NULL.
/// @return A freshly allocated rt_string with the canonical BCP-47 tag.
rt_string rt_locale_to_string(void *locale) {
    return rt_locale_tag(locale);
}

/// @brief Return 1 if two Locale handles represent the same canonical BCP-47 tag.
/// @details Compares the already-normalised tag strings directly; handles the edge
///          case where an empty tag and the literal string "root" are both invariant.
///          Same-pointer fast-path avoids the string compare for common usage.
/// @param a First Locale handle; may be NULL (treated as invariant).
/// @param b Second Locale handle; may be NULL (treated as invariant).
/// @return 1 if equal, 0 if different.
int8_t rt_locale_equals(void *a, void *b) {
    if (a == b)
        return 1;
    // A null handle is invariant-shaped everywhere else in the API
    // (Tag/ToString/Fallbacks), so equality treats it as "root" too
    // (VDOC-067).
    rt_locale_t *la = (rt_locale_t *)a;
    rt_locale_t *lb = (rt_locale_t *)b;
    // Compare canonical tags directly; both sides are already normalized so
    // strcmp suffices. Tag "root", empty tag, and NULL all represent invariant.
    const char *ta = (la && la->tag[0]) ? la->tag : "root";
    const char *tb = (lb && lb->tag[0]) ? lb->tag : "root";
    return strcmp(ta, tb) == 0 ? (int8_t)1 : (int8_t)0;
}

//===----------------------------------------------------------------------===//
// Fallback chain
//===----------------------------------------------------------------------===//

/// @brief Build the ordered fallback chain for @p locale as a List of Locale handles.
/// @details The chain follows CLDR conventions: full tag → base lang+script+region →
///          lang+region (if script present) → lang-only → invariant ("root").
///          Variant/extension subtags are preserved on the first element but dropped
///          on all subsequent steps. The list always terminates with the invariant so
///          callers can unconditionally use the last entry as a safe fallback.
/// @param locale Opaque Locale handle; NULL produces a list containing only invariant.
/// @return A new rt_list owned by the caller, containing at least one entry.
void *rt_locale_fallbacks(void *locale) {
    void *list = rt_list_new();
    if (!list)
        return NULL;

    if (!locale) {
        // Invariant only.
        loc_list_push_owned(list, loc_make_invariant());
        return list;
    }

    rt_locale_t *l = (rt_locale_t *)locale;

    // Short-circuit: the invariant ("root") locale's chain is just [root].
    if (l->language[0] == '\0') {
        loc_list_push_owned(list, loc_make_invariant());
        return list;
    }

    // Emit the full handle first, preserving variants/extensions.
    loc_list_push_owned(list, loc_from_canonical_tag(l->tag));

    char base_tag[RT_LOCALE_TAG_CAP];
    size_t base_pos = 0;
    size_t ll = strnlen(l->language, RT_LOCALE_LANG_CAP);
    memcpy(base_tag, l->language, ll);
    base_pos += ll;
    if (l->script[0] != '\0') {
        base_tag[base_pos++] = '-';
        size_t sl = strnlen(l->script, RT_LOCALE_SCRIPT_CAP);
        memcpy(base_tag + base_pos, l->script, sl);
        base_pos += sl;
    }
    if (l->region[0] != '\0') {
        base_tag[base_pos++] = '-';
        size_t rl = strnlen(l->region, RT_LOCALE_REGION_CAP);
        memcpy(base_tag + base_pos, l->region, rl);
        base_pos += rl;
    }
    base_tag[base_pos] = '\0';

    // If variants/extensions were present, fall back to the base
    // language/script/region tag before dropping script/region.
    if (base_tag[0] != '\0' && strcmp(base_tag, l->tag) != 0)
        loc_list_push_owned(list, loc_from_canonical_tag(base_tag));

    // If a script is present, produce a `<lang>-<region>` step (drop script).
    if (l->script[0] != '\0' && l->region[0] != '\0') {
        char tag[RT_LOCALE_TAG_CAP];
        size_t pos = 0;
        memcpy(tag, l->language, ll);
        pos += ll;
        tag[pos++] = '-';
        size_t rl = strnlen(l->region, RT_LOCALE_REGION_CAP);
        memcpy(tag + pos, l->region, rl);
        pos += rl;
        tag[pos] = '\0';
        loc_list_push_owned(list, loc_from_canonical_tag(tag));
    }

    // Language-only step, unless the original was already language-only.
    if (l->language[0] != '\0' && (l->script[0] != '\0' || l->region[0] != '\0')) {
        char tag[RT_LOCALE_TAG_CAP];
        memcpy(tag, l->language, ll);
        tag[ll] = '\0';
        loc_list_push_owned(list, loc_from_canonical_tag(tag));
    }

    // Invariant terminator.
    loc_list_push_owned(list, loc_make_invariant());
    return list;
}

//===----------------------------------------------------------------------===//
// Internal helpers used by LocaleManager / LocaleInfo
//===----------------------------------------------------------------------===//

/// @brief Bind a locale-data record to a Locale handle, replacing any prior binding.
/// @details Performs a retain-then-release swap so the reference counts stay accurate
///          even when @p data is the same record that is already bound. No-op for
///          NULL @p locale. Called by the LocaleManager when a new data record is
///          loaded and the handle's tag matches.
/// @param locale Opaque Locale handle; may be NULL.
/// @param data   New locale-data record to bind; may be NULL to clear.
void rt_locale_bind_data(void *locale, const rt_locale_data_t *data) {
    if (!locale)
        return;
    rt_locale_t *l = (rt_locale_t *)locale;
    if (l->data == data)
        return;
    rt_locale_manager_retain_data(data);
    rt_locale_manager_release_data(l->data);
    l->data = data;
}

/// @brief Return the locale-data record bound to @p locale, falling back to en-US invariant.
/// @details The returned pointer is non-owning; its lifetime is managed by the
///          LocaleManager. Never returns NULL — the invariant record is always available.
/// @param locale Opaque Locale handle; NULL returns the en-US invariant.
/// @return Pointer to the bound rt_locale_data_t, or the en-US invariant record.
const rt_locale_data_t *rt_locale_get_data(void *locale) {
    if (!locale)
        return rt_locale_data_en_us();
    rt_locale_t *l = (rt_locale_t *)locale;
    if (l->data)
        return l->data;
    return rt_locale_data_en_us();
}
