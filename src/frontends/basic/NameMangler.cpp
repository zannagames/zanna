//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/NameMangler.cpp
// Purpose: Implement the BASIC name mangler responsible for synthesizing stable
//          identifiers for temporaries and lowered control-flow labels.
// Key invariants: Generated names are deterministic and collision-free within a
//                 compilation unit.
// Links: docs/tutorials/basic-tutorial.md, docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//

/// @file NameMangler.cpp
/// @brief Retains the historical BASIC NameMangler implementation.
/// @details The public BASIC header currently aliases the common frontend
///          mangler, whose interface supplies the same temporary and block
///          naming operations together with collision and overflow handling.

#include "frontends/basic/NameMangler.hpp"

namespace il::frontends::basic {
/// @brief Produce the next compiler-generated temporary identifier.
///
/// The mangler reserves the "%t" prefix for temporaries.  Every invocation
/// increments `tempCounter` and appends the previous value to the prefix,
/// yielding monotonically increasing, collision-free identifiers that remain
/// deterministic across compiler runs.
/// @return The next name in the sequence `%t0`, `%t1`, and so on.
std::string NameMangler::nextTemp() {
    return "%t" + std::to_string(tempCounter++);
}

/// @brief Derive a unique block label from a human-friendly hint.
///
/// The mangler remembers how often each hint has been used.  The first request
/// returns the hint verbatim to preserve readability in the printed IR.  Each
/// subsequent request appends the current counter value, then increments the
/// counter, ensuring unique yet recognizable labels across control-flow
/// lowering passes.
/// @param hint Base text used for the generated label.
/// @return @p hint on its first use, then the hint with `1`, `2`, and so on appended.
std::string NameMangler::block(const std::string &hint) {
    auto &count = blockCounters[hint];
    std::string name = hint;
    if (count > 0)
        name += std::to_string(count);
    ++count;
    return name;
}

} // namespace il::frontends::basic
