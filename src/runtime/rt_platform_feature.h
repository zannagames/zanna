//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/rt_platform_feature.h
// Purpose: Centralized feature-test macro adapter for runtime translation
//   units that need POSIX/GNU/Darwin declarations before including system
//   headers.
//
// Key invariants:
//   - This header must be included before any C library or platform header in
//     a translation unit that relies on the feature macros below.
//   - Raw platform probes live here as an approved platform adapter; product
//     code should include this header instead of open-coding _WIN32/__APPLE__.
//
// Ownership/Lifetime:
//   - Pure preprocessor adapter; it introduces no declarations or runtime state.
//
// Links: src/runtime/rt_platform.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_platform_feature.h
 * @brief Enables centralized libc feature-test declarations where required.
 * @details Translation units include this adapter before all system headers to
 *          expose GNU and Darwin extension declarations on supported POSIX
 *          platforms. It contains only preprocessor policy and introduces no
 *          symbols or runtime state.
 */

#pragma once

/// @brief Enable GNU and Darwin extension declarations on non-Windows C runtimes.
/// @details Some libc declarations used by graphics/runtime loaders are hidden unless
///          feature-test macros are defined before any system header is included. Keeping
///          this logic in one adapter avoids raw platform checks in product translation
///          units while preserving the previous declaration surface.
#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
