//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_message_bundle.h
// Purpose: Public C API for Zanna.Localization.MessageBundle — a translation
//          catalog keyed by message ID. Supports placeholder interpolation
//          ({name} and positional {0}/{1}/…), fallback chains (bundles stack
//          so missing keys cascade up), and CLDR plural selection via
//          PluralRules.
//
// Key invariants:
//   - Keys and values are rt_strings. NUL bytes in keys are not supported.
//   - Fallback chains cannot cycle: SetFallback traps when it detects a
//     proposed or pre-existing cycle; lookup depth is capped at 16.
//   - Get traps when no key in the chain resolves; TryGet returns empty
//     string without trapping.
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated; GC-managed.
//   - Entries are retained by the bundle's internal rt_map.
//
// Links: src/runtime/localization/rt_message_bundle.c (implementation),
//        src/runtime/localization/rt_plural_rules.h (category evaluator),
//        docs/zannalib/localization/messages.md (user documentation).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares locale-bound translation catalogs and formatting services.
 * @details Defines construction from maps, JSON, and assets; owned and optional
 * lookup forms; named, positional, and plural interpolation; bounded fallback
 * chaining; and direct-key enumeration.
 */

#pragma once

#include "rt.hpp"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// @brief Default constructor — empty bundle bound to the current locale.
/// @return Newly allocated MessageBundle handle, or NULL after an allocation
///         failure if the runtime trap hook returns.
void *rt_message_bundle_new(void);

/// @brief Load a translation map from a JSON file at @p path.
/// @details Expects a flat object: {"key": "value", ...}. Non-string values
///          are rejected. Traps on file, JSON, schema, or allocation errors.
/// @param locale Locale handle retained by the bundle and used for plural rules.
/// @param path Runtime string containing the file path. NULL is rejected.
/// @return Newly allocated MessageBundle handle, or NULL on failure if the
///         runtime trap hook returns.
void *rt_message_bundle_load_from_json(void *locale, rt_string path);

/// @brief Load a translation map from a ZPAK-embedded asset by @p name.
/// @details The asset must decode as text containing a flat JSON object whose
///          values are all strings. Load, decode, parse, schema, and allocation
///          failures raise a runtime trap.
/// @param locale Locale handle retained by the bundle and used for plural rules.
/// @param name Runtime string naming the packaged asset. NULL is rejected.
/// @return Newly allocated MessageBundle handle, or NULL on failure if the
///         runtime trap hook returns.
void *rt_message_bundle_load_from_asset(void *locale, rt_string name);

/// @brief Construct from a preexisting rt_map<rt_string, rt_string>.
/// @details Validates and retains @p map; a NULL map creates an empty bundle.
/// @param locale Locale handle retained by the bundle and used for plural rules.
/// @param map Map with string values, or NULL.
/// @return Newly allocated MessageBundle handle, or NULL on validation or
///         allocation failure if the runtime trap hook returns.
void *rt_message_bundle_from_map(void *locale, void *map);

//===----------------------------------------------------------------------===//
// Property accessors
//===----------------------------------------------------------------------===//

/// @brief Return the Locale handle this bundle was built with (borrowed).
/// @param self MessageBundle handle, or NULL.
/// @return Borrowed Locale handle, or NULL when @p self is NULL. The caller
///         must not release the returned reference.
void *rt_message_bundle_get_locale(void *self);

/// @brief Number of entries in this bundle's own map (excludes fallbacks).
/// @param self MessageBundle handle, or NULL.
/// @return Direct entry count, or zero when @p self is NULL.
int64_t rt_message_bundle_get_count(void *self);

//===----------------------------------------------------------------------===//
// Lookup / format
//===----------------------------------------------------------------------===//

/// @brief Resolve @p key through this bundle and its fallback chain.
/// @details Checks direct, locale-qualified, and fallback-bundle entries.
///          Traps for NULL input or when no bundle in the chain has the key.
/// @param self MessageBundle handle.
/// @param key Non-NULL message key.
/// @return Owned resolved string, or an owned empty string after a trapped error.
rt_string rt_message_bundle_get(void *self, rt_string key);

/// @brief Non-trapping variant: returns empty string when unresolved.
/// @details A missing key and a present empty translation are indistinguishable;
///          use @ref rt_message_bundle_try_get_option when that distinction matters.
/// @param self MessageBundle handle, or NULL.
/// @param key Message key, or NULL.
/// @return Owned resolved value, or an owned empty string when unresolved.
rt_string rt_message_bundle_try_get(void *self, rt_string key);

/// @brief Resolve @p key or return a caller-provided default string.
/// @details The returned string is always retained for the caller: a resolved
///          bundle value is returned from the lookup chain, while
///          @p default_value is retained before returning when the key is
///          missing. A NULL default produces an allocated empty string.
/// @param self MessageBundle handle, or NULL.
/// @param key Message key, or NULL.
/// @param default_value Fallback string to retain, or NULL for empty.
/// @return Owned resolved or fallback string.
rt_string rt_message_bundle_get_or(void *self, rt_string key, rt_string default_value);

/// @brief Resolve @p key as an Option.
/// @details Returns `Some(str)` when the key is present, including when the
///          translation is the empty string, and `None` when the key cannot be
///          resolved through the bundle or fallback chain.
/// @param self MessageBundle handle, or NULL.
/// @param key Message key, or NULL.
/// @return Newly allocated Option containing the resolved string, or None.
void *rt_message_bundle_try_get_option(void *self, rt_string key);

/// @brief Check whether @p key resolves in this bundle or its chain.
/// @param self MessageBundle handle, or NULL.
/// @param key Message key, or NULL.
/// @return 1 if a value resolves, including an empty value; otherwise 0.
int8_t rt_message_bundle_has(void *self, rt_string key);

/// @brief Interpolate a message template with named placeholders.
/// @details Placeholder syntax: `{name}`. Uses the resolved template string
///          via Get (so fallback chain applies). Missing placeholders in
///          @p vars are preserved literally. Doubled braces emit a single
///          literal brace. The map must contain only string values.
/// @param self MessageBundle handle.
/// @param key Message key to resolve.
/// @param vars String-to-string substitution map, or NULL.
/// @return Owned interpolated string, or an owned empty string after a trapped error.
rt_string rt_message_bundle_format(void *self, rt_string key, void *vars);

/// @brief Positional interpolation; `{0}`, `{1}`, … index into @p values.
/// @details @p values must be a List containing only strings. Out-of-range
///          placeholders remain literal, and doubled braces emit one brace.
/// @param self MessageBundle handle.
/// @param key Message key to resolve.
/// @param values List of positional strings, or NULL.
/// @return Owned interpolated string, or an owned empty string after a trapped error.
rt_string rt_message_bundle_format_with(void *self, rt_string key, void *values);

/// @brief Plural-aware lookup and substitution.
/// @details Evaluates the bundle's locale's cardinal plural category for
///          @p n, then looks up "<key>.<category>" (falling back to
///          "<key>.other"). Substitutes `{n}` with the numeric value using a
///          temporary variable map and locale-specific digits; @p vars is not
///          mutated and must contain only string values.
/// @param self MessageBundle handle.
/// @param key Base plural-message key.
/// @param n Signed integer used for category selection and `{n}` substitution.
/// @param vars String-to-string substitution map to clone, or NULL.
/// @return Owned interpolated plural message, or an owned empty string after a
///         trapped error.
rt_string rt_message_bundle_plural(void *self, rt_string key, int64_t n, void *vars);

/// @brief Set the fallback bundle (returns self for chaining).
/// @details Traps if the proposed chain reaches @p self or already contains a
///          cycle. The fallback is retained until replaced or the bundle is
///          finalized; NULL clears it.
/// @param self MessageBundle to update, or NULL.
/// @param fallback MessageBundle to retain as the fallback, or NULL.
/// @return @p self on success, or NULL for NULL @p self or a rejected cycle.
void *rt_message_bundle_set_fallback(void *self, void *fallback);

/// @brief Enumerate the keys defined in this bundle (excludes fallback).
/// @param self MessageBundle handle, or NULL.
/// @return Newly allocated List of direct keys; empty when @p self is NULL.
void *rt_message_bundle_keys(void *self);

#ifdef __cplusplus
}
#endif
