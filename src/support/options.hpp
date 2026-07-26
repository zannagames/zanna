//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/options.hpp
// Purpose: Declares command-line option parsing helpers.
// Key invariants: None.
// Ownership/Lifetime: Caller owns option values.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the shared value type for global compiler options.
/// @details The support layer exposes backend-neutral tracing, verification,
///          and target-selection settings that command drivers can populate
///          before constructing compiler pipelines. The structure owns its
///          target string and otherwise contains independent scalar flags.

#pragma once

#include <string>
#include <string_view>

namespace il::support {

/// @brief Holds global command-line settings that influence compiler behavior.
/// @details These options control tracing, verification, and target selection.
///          Default construction enables verification, disables tracing, and
///          defers target resolution to the host-platform adapters.
/// @invariant Flags are independent booleans.
/// @ownership Value type that owns its target selector string.
struct Options {
    /// @brief Default target selector used when callers do not choose a backend target.
    /// @details The support-level options type is intentionally generic and does
    ///          not own backend-specific target triples.  The value "host" lets
    ///          downstream command layers resolve the actual platform/architecture
    ///          through their cross-platform capability adapters.
    static constexpr std::string_view kDefaultTarget = "host";

    /// @brief Enable verbose tracing of compilation steps.
    /// @details Drivers and compiler stages may use this independent flag to
    ///          emit additional progress information without changing results.
    bool trace = false;

    /// @brief Run the IL verifier before execution or code generation.
    /// @details Command layers may disable verification explicitly for trusted
    ///          input or specialized workflows; the safe default is enabled.
    bool verify = true;

    /// @brief Target selector used for code generation.
    /// @details The default `"host"` value requests platform-native resolution;
    ///          command layers may replace it with a supported explicit target.
    std::string target = std::string{kDefaultTarget};
};
} // namespace il::support
