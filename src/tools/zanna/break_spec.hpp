//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file break_spec.hpp
/// @brief Declares syntactic recognition of command-line source breakpoints.
///
/// The helper performs no filesystem access and retains no state.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

namespace ilc {

/// @brief Determine whether a --break argument refers to a source line.
///
/// A source break specification is written as `<file>:<line>` where the left
/// side contains at least one non-whitespace character and the right side is a
/// decimal line number.
/// @param spec Single token supplied to the `--break` flag.
/// @return `true` when the token matches the source break format; `false`
/// otherwise.
/// @note This check is purely syntactic and does not verify file existence or
/// line bounds.
bool isSrcBreakSpec(const std::string &spec);

} // namespace ilc
