//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_numfmt_internal.h
// Purpose: Internal hooks into rt_numfmt's digit-grouping pipeline, exposed so
//          the localization module's NumberFormat can emit locale-specific
//          grouping without duplicating the group-every-N-digits logic. The
//          same helper is used by Zanna.Text.InvariantNumberFormat.Thousands and by
//          Zanna.Localization.NumberFormat.{Decimal, Integer, Currency, ...}.
//
// Key invariants:
//   - Implementation-only: must not be included from public-facing headers.
//   - Helper operates on a pre-built decimal digit buffer; sign handling is
//     the caller's responsibility.
//   - Traps via rt_trap on string-builder allocation failure (same contract
//     as the parent Zanna.Text.InvariantNumberFormat functions).
//
// Ownership/Lifetime:
//   - Caller owns the string builder and the digit buffer; the helper only
//     appends bytes.
//
// Links: src/runtime/text/rt_numfmt.c (canonical implementation host),
//        src/runtime/localization/rt_numformat.c (primary client).
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_numfmt_internal.h
 * @brief Declares shared decimal digit-grouping support.
 * @details Invariant and localized number formatters call this
 *          implementation-only helper to append a caller-owned digit span into
 *          a caller-owned StringBuilder with a configurable byte separator and
 *          right-to-left group width.
 */

#pragma once

#include "rt_string_builder.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Append @p digits into @p sb with @p sep inserted every @p group_size
///        digits from the right.
/// @details Traps via `rt_trap()` when the string builder reports allocation
///          failure. @p digits must contain exactly @p dlen ASCII decimal
///          characters. A null or empty separator, nonpositive group size, or
///          group size at least @p dlen appends the input unchanged.
/// @param sb          Destination string builder (non-null).
/// @param digits      Pointer to the digit bytes (non-null).
/// @param dlen        Number of digit bytes; nonpositive values append nothing.
/// @param sep         Group separator bytes; NULL or zero-length means no
///                    grouping (digits emitted as-is).
/// @param sep_len     Length of @p sep in bytes.
/// @param group_size  Digits per group from the right (typically 3).
void rt_numfmt_group_digits(rt_string_builder *sb,
                            const char *digits,
                            int dlen,
                            const char *sep,
                            size_t sep_len,
                            int group_size);

#ifdef __cplusplus
}
#endif
