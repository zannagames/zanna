//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the GlobalVerifier class, which validates global variable
// declarations within IL modules. Globals represent constant data and mutable
// variables that exist at module scope, accessible across all functions.
//
// The IL specification requires that all global names within a module are unique.
// The GlobalVerifier enforces this constraint during module verification,
// constructing a symbol table mapping global names to their definitions for
// use by downstream verification passes.
//
// Key Responsibilities:
// - Enforce global name uniqueness across the module
// - Build a symbol table mapping global names to Global structures
// - Report duplicate global definition errors with diagnostic context
// - Provide verified global lookups for function-level verification
//
// Design Notes:
// The GlobalVerifier builds a GlobalMap during the run() method, storing pointers
// back into the module's global vector. These pointers remain valid for the
// module's lifetime as the module owns all Global structures by value. The
// verifier does not copy or own the Global structures, only maintains a lookup
// index for efficient verification of global references.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares validation and lookup indexing for module globals.
/// @details `GlobalVerifier` validates symbol names, supported types, linkage
///          rules, and textual initializer compatibility before exposing
///          borrowed global declarations through a name index.

#pragma once

#include "il/verify/DiagSink.hpp"

#include "support/diag_expected.hpp"

#include <string>
#include <unordered_map>

namespace il::core {
struct Module;
struct Global;
} // namespace il::core

namespace il::verify {

/// @brief Ensures module global declarations obey uniqueness rules.
class GlobalVerifier {
  public:
    /// @brief Name-to-declaration index whose pointers borrow module storage.
    using GlobalMap = std::unordered_map<std::string, const il::core::Global *>;

    /// @brief Access the verified global lookup table.
    /// @details After a failed run the table may retain valid declarations that
    ///          preceded the first error.
    /// @return Const reference to the verifier-owned map.
    [[nodiscard]] const GlobalMap &globals() const;

    /// @brief Verify globals in @p module and populate the lookup table.
    /// @details Clears prior state, validates every declaration and initializer,
    ///          and rejects duplicate global names.
    /// @param module Module to inspect.
    /// @param sink Diagnostic sink receiving advisory output (currently unused).
    /// @return Empty Expected on success; diagnostic payload when verification fails.
    [[nodiscard]] il::support::Expected<void> run(const il::core::Module &module, DiagSink &sink);

  private:
    /// @brief Current borrowed global index, rebuilt by each call to @ref run.
    GlobalMap globals_;
};

} // namespace il::verify
