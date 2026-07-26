//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_collator.h
// Purpose: Public C API for Zanna.Localization.Collator — locale-aware string
//          comparison with configurable strength, case sensitivity, and
//          accent sensitivity. Uses a DUCET-lite weight table (primary /
//          secondary / tertiary) covering basic Latin, Latin-1 Supplement,
//          and Latin Extended-A diacritics. Swedish locale tailorings are
//          applied as override patches at collator construction time; German
//          currently uses invariant Latin decomposition weights.
//
// Key invariants:
//   - Strength 1 compares primary weights only (base letters).
//   - Strength 2 adds secondary weights (accents differentiate).
//   - Strength 3 adds tertiary weights (case differentiates; default).
//   - Requested strength clamps to 1..3; values above 3 warn on stderr.
//   - Input strings capped at 1 MiB per comparison; above that, trap.
//   - Unsupported scripts fall back to codepoint-order comparison (not a
//     silent failure — behavior is documented).
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated; GC-managed.
//   - Each Collator retains its Locale handle and locale-data snapshot.
//   - GetLocale returns a borrowed handle; Sort/SortKey return fresh objects.
//
// Links: src/runtime/localization/rt_collator.c (implementation),
//        src/runtime/localization/rt_collator_table.c (weight classifier),
//        docs/zannalib/localization/collation.md (user documentation).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares locale-aware Collator construction and comparison services.
 * @details Defines configurable comparison strength and case/accent handling,
 * deterministic sort keys, stable list sorting, and the internal classifier
 * and locale-tailoring contracts shared with the compact weight table.
 */

#pragma once

#include "rt.hpp"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Constructors / properties
//===----------------------------------------------------------------------===//

/// @brief Create a collator bound to the process's current locale.
/// @return Fresh GC-managed Collator, or NULL after an allocation trap.
void *rt_collator_new(void);
/// @brief Create a collator bound to the given @p locale handle.
/// @param locale Locale handle retained by the result; may be NULL for invariant data.
/// @return Fresh GC-managed Collator, or NULL after an allocation trap.
void *rt_collator_for_locale(void *locale);
/// @brief Return the Locale handle this collator was built with (borrowed).
/// @param self Valid Collator handle; may be NULL.
/// @return Borrowed Locale handle, or NULL when absent.
void *rt_collator_get_locale(void *self);

/// @brief Get the comparison strength (1=primary, 2=+accent, 3=+case).
/// @param self Valid Collator handle; may be NULL.
/// @return Active strength, or default strength 3 for NULL.
int64_t rt_collator_get_strength(void *self);
/// @brief Set comparison strength, clamping below 1 and warning/clamping above 3.
/// @param self Valid Collator handle; NULL is a no-op.
/// @param value Requested comparison strength.
void rt_collator_set_strength(void *self, int64_t value);

/// @brief Get whether case differences are ignored (0/1).
/// @param self Valid Collator handle; may be NULL.
/// @return Normalized ignore-case flag, or 0 for NULL.
int8_t rt_collator_get_ignore_case(void *self);
/// @brief Set whether case differences are ignored.
/// @param self Valid Collator handle; NULL is a no-op.
/// @param value Zero to preserve case weights, nonzero to omit them.
void rt_collator_set_ignore_case(void *self, int8_t value);

/// @brief Get whether accent/diacritic differences are ignored (0/1).
/// @param self Valid Collator handle; may be NULL.
/// @return Normalized ignore-accents flag, or 0 for NULL.
int8_t rt_collator_get_ignore_accents(void *self);
/// @brief Set whether accent/diacritic differences are ignored.
/// @param self Valid Collator handle; NULL is a no-op.
/// @param value Zero to preserve accent weights, nonzero to omit them.
void rt_collator_set_ignore_accents(void *self, int8_t value);

//===----------------------------------------------------------------------===//
// Comparisons
//===----------------------------------------------------------------------===//

/// @brief Returns -1 / 0 / +1 per locale-aware comparison.
/// @param self Valid Collator handle; NULL compares equal.
/// @param a First runtime string, or NULL for empty.
/// @param b Second runtime string, or NULL for empty.
/// @return -1, 0, or 1 according to configured collation ordering.
int64_t rt_collator_compare(void *self, rt_string a, rt_string b);

/// @brief Convenience: 1 if @p a and @p b compare equal under this collator.
/// @param self Valid Collator handle; NULL treats all inputs as equal.
/// @param a First runtime string, or NULL for empty.
/// @param b Second runtime string, or NULL for empty.
/// @return 1 for collation equality, otherwise 0.
int8_t rt_collator_equals(void *self, rt_string a, rt_string b);

/// @brief Generate a deterministic byte sequence whose binary comparison
///        matches this collator's Compare. Returned as a hex-encoded string
///        so it fits inside an rt_string without NUL issues.
/// @param self Valid Collator handle.
/// @param s Runtime string to encode.
/// @return Fresh lowercase-hex runtime string, or a fresh empty string on failure.
rt_string rt_collator_sort_key(void *self, rt_string s);

/// @brief Return a stable, sorted copy of a List<str> using this collator.
/// @param self Valid Collator handle.
/// @param items Runtime List of strings; the input is not modified.
/// @return Fresh List containing retained elements in collation order.
void *rt_collator_sort(void *self, void *items);

//===----------------------------------------------------------------------===//
// Internal API shared with the weight table
//===----------------------------------------------------------------------===//

/**
 * @brief Locale-specific replacement weights for one Unicode scalar.
 * @details Patch arrays are immutable borrowed data selected by canonical
 * locale tag and captured by a Collator during construction.
 */
typedef struct rt_collator_locale_patch {
    /// Unicode scalar whose base weights are replaced.
    uint32_t codepoint;
    /// Locale-specific primary ordering weight.
    uint32_t primary_override;
    /// Locale-specific accent weight.
    uint16_t secondary_override;
    /// Locale-specific case weight.
    uint16_t tertiary_override;
} rt_collator_locale_patch_t;

/// @brief Populate P/S/T weights for a codepoint. Returns 0 on known
///        characters; 1 when the codepoint falls into codepoint-order
///        fallback territory (the primary is set to 0x8000 + cp).
/// @param cp Unicode code point to classify.
/// @param primary Optional destination for the primary weight.
/// @param secondary Optional destination for the secondary weight.
/// @param tertiary Optional destination for the tertiary weight.
/// @return 0 for table-covered characters, or 1 for deterministic
///         codepoint-order fallback.
int rt_collator_codepoint_weights(uint32_t cp,
                                  uint32_t *primary,
                                  uint16_t *secondary,
                                  uint16_t *tertiary);

/// @brief Return the locale patch table for @p tag (or NULL). @p out_count
///        is set to the patch count.
/// @param tag Canonical locale tag, matched by supported language prefix.
/// @param out_count Optional destination reset to zero and populated on a match.
/// @return Borrowed immutable patch table, or NULL when no overrides apply.
const rt_collator_locale_patch_t *rt_collator_locale_patches(const char *tag, size_t *out_count);

#ifdef __cplusplus
}
#endif
