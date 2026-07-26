//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_version.h
// Purpose: Declares SemVer-style parsing, validation, access, comparison,
//          constraint testing, and component-bump operations.
//
// Key invariants:
//   - Version format: MAJOR.MINOR.PATCH[-pre-release][+build].
//   - An optional leading v/V is accepted; core components must fit int64_t.
//   - Prerelease identifiers follow SemVer numeric/alphanumeric precedence.
//   - Build metadata is ignored in comparisons.
//   - rt_version_compare returns negative/zero/positive for less/equal/greater.
//
// Ownership/Lifetime:
//   - Version objects are opaque runtime-managed allocations.
//   - Returned strings are newly allocated; caller must release.
//
// Links: src/runtime/text/rt_version.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_version.h
 * @brief Declares semantic-version parsing, access, comparison, and constraints.
 * @details The API validates three signed-64-bit core components with optional
 *          prerelease and build identifiers, exposes caller-owned component
 *          text, compares SemVer precedence, tests supported constraints, and
 *          returns managed bumped versions.
 */

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Parse a semantic version string.
/// @details Requires all three core components, validates identifiers, and
///          accepts an optional leading `v`/`V`.
/// @param str Borrowed version string.
/// @return Caller-owned runtime-managed Version, or null if invalid.
void *rt_version_parse(rt_string str);

/// @brief Check if a string is a valid semantic version.
/// @param str Borrowed version string.
/// @return `1` if parsing succeeds, otherwise `0`.
int8_t rt_version_is_valid(rt_string str);

/// @brief Get the major version number.
/// @param ver Borrowed Version object; null reports zero.
/// @return Major component.
int64_t rt_version_major(void *ver);

/// @brief Get the minor version number.
/// @param ver Borrowed Version object; null reports zero.
/// @return Minor component.
int64_t rt_version_minor(void *ver);

/// @brief Get the patch version number.
/// @param ver Borrowed Version object; null reports zero.
/// @return Patch component.
int64_t rt_version_patch(void *ver);

/// @brief Get the pre-release string.
/// @param ver Borrowed Version object.
/// @return Newly allocated prerelease string, or empty if absent.
rt_string rt_version_prerelease(void *ver);

/// @brief Get the build metadata string.
/// @param ver Borrowed Version object.
/// @return Newly allocated build metadata, or empty if absent.
rt_string rt_version_build(void *ver);

/// @brief Format version object back to string.
/// @param ver Borrowed Version object.
/// @return Newly allocated canonical text, or empty for null.
rt_string rt_version_to_string(void *ver);

/// @brief Compare two versions (ignoring build metadata per SemVer spec).
/// @details Null sorts below non-null; two null pointers compare equal.
/// @param a Borrowed first Version.
/// @param b Borrowed second Version.
/// @return `-1`, `0`, or `1`.
int64_t rt_version_cmp(void *a, void *b);

/// @brief Parse and compare two semantic version strings.
/// @details Invalid strings parse as null and follow null comparison ordering.
/// @param a Borrowed first version string.
/// @param b Borrowed second version string.
/// @return `-1`, `0`, or `1`.
int64_t rt_version_compare(rt_string a, rt_string b);

/// @brief Parse a semantic version string and return its major component.
/// @param str Borrowed version string.
/// @return Major component, or zero when invalid.
int64_t rt_version_major_str(rt_string str);

/// @brief Parse a semantic version string and return its minor component.
/// @param str Borrowed version string.
/// @return Minor component, or zero when invalid.
int64_t rt_version_minor_str(rt_string str);

/// @brief Parse a semantic version string and return its patch component.
/// @param str Borrowed version string.
/// @return Patch component, or zero when invalid.
int64_t rt_version_patch_str(rt_string str);

/// @brief Check if version satisfies a simple constraint.
/// @details Supports exact/no operator, `=`, `!=`, comparisons, caret, and
///          tilde against a complete three-component version. Empty trimmed
///          constraints match every non-null Version.
/// @param ver Borrowed Version object.
/// @param constraint Borrowed constraint such as `">=1.0.0"` or `"^1.2.0"`.
/// @return `1` if satisfied, otherwise `0`.
int8_t rt_version_satisfies(void *ver, rt_string constraint);

/// @brief Bump major version (resets minor and patch to 0).
/// @details Discards prerelease/build metadata and traps on component overflow.
/// @param ver Borrowed Version object.
/// @return Newly allocated bumped text, or empty for null/overflow.
rt_string rt_version_bump_major(void *ver);

/// @brief Bump minor version (resets patch to 0).
/// @details Discards prerelease/build metadata and traps on component overflow.
/// @param ver Borrowed Version object.
/// @return Newly allocated bumped text, or empty for null/overflow.
rt_string rt_version_bump_minor(void *ver);

/// @brief Bump patch version.
/// @details Discards prerelease/build metadata and traps on component overflow.
/// @param ver Borrowed Version object.
/// @return Newly allocated bumped text, or empty for null/overflow.
rt_string rt_version_bump_patch(void *ver);

#ifdef __cplusplus
}
#endif
