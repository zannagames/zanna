//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/io/Serializer.hpp
// Purpose: Declares the Serializer class -- converts IL modules to their
//          textual representation in Pretty (human-readable) or Canonical
//          (minimal, deterministic) mode. Inverse of Parser; output conforms
//          to the IL grammar and round-trips through parse(serialize(m)).
// Key invariants:
//   - Serializer is stateless; all methods are static and thread-safe.
//   - Output conforms to docs/il/il-guide.md grammar.
// Ownership/Lifetime: Stateless facade. The caller owns the ostream and
//          Module passed to write()/toString().
// Links: docs/il/il-guide.md, il/io/Parser.hpp, il/core/fwd.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the stateless textual IL serializer.

#pragma once

#include "il/core/fwd.hpp"
#include <ostream>
#include <string>

namespace il::io {

/// @brief Serializes IL modules to their textual form.
/// @details Pretty and canonical output are accepted by the textual parser and
///          preserve value identity. All methods borrow their input and retain
///          no state after returning.
class Serializer {
  public:
    /// @brief Controls output formatting style and malformed-IR handling.
    /// @details Pretty and Canonical produce parseable IL and throw when the
    ///          module contains malformed instruction shapes. Debug preserves the
    ///          historical best-effort comments for diagnostics and crash dumps.
    ///          Pretty preserves declaration order, Canonical sorts declarations
    ///          where order is insignificant, and Debug tolerates malformed
    ///          instruction shapes.
    enum class Mode { Pretty, Canonical, Debug };

    /// @brief Write module @p m to output stream @p os.
    /// @param m Module to serialize.
    /// @param os Destination stream.
    /// @param mode Formatting mode (pretty by default).
    /// @throws std::invalid_argument if normal-mode input cannot be represented
    ///         as valid textual IL.
    static void write(const il::core::Module &m, std::ostream &os, Mode mode = Mode::Pretty);

    /// @brief Serialize module @p m to a string.
    /// @param m Module to serialize.
    /// @param mode Formatting mode (pretty by default).
    /// @return Textual IL representation.
    /// @throws std::invalid_argument under the same conditions as @ref write.
    static std::string toString(const il::core::Module &m, Mode mode = Mode::Pretty);
};

} // namespace il::io
