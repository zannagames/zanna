//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the ExternVerifier class, which validates extern declarations
// in IL modules. Externs represent runtime functions and intrinsics that are
// declared but not defined within the IL module, to be linked or interpreted by
// the runtime system.
//
// The IL specification requires that extern names are unique within a module.
// Functions within the module reference externs by name through call.extern
// instructions. The ExternVerifier enforces name uniqueness and builds a symbol
// table for downstream verification passes to validate call.extern references.
//
// Key Responsibilities:
// - Enforce extern name uniqueness across the module
// - Build a symbol table mapping extern names to Extern structures
// - Report duplicate extern declaration errors with diagnostic context
// - Provide verified extern lookups for call.extern validation
//
// Design Notes:
// The ExternVerifier constructs an ExternMap during run(), storing pointers into
// the module's extern vector. These pointers remain valid for the module's
// lifetime since modules own all Extern structures by value. The verifier does
// not copy or own the Extern structures, only maintains a lookup index for
// efficient name resolution during function verification.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares validation and lookup indexing for IL extern declarations.
/// @details `ExternVerifier` checks names, parameter types, duplicate
///          declarations, and compatibility with canonical runtime signatures.
///          Its resulting map borrows descriptors from the verified module.

#pragma once

#include "il/verify/DiagSink.hpp"

#include "support/diag_expected.hpp"

#include <string>
#include <unordered_map>

namespace il::core {
struct Module;
struct Extern;
} // namespace il::core

namespace il::verify {

/// @brief Validates extern declarations and records them for downstream passes.
class ExternVerifier {
  public:
    /// @brief Name-to-declaration index whose values borrow module storage.
    using ExternMap = std::unordered_map<std::string, const il::core::Extern *>;

    /// @brief Access verified extern descriptors keyed by symbol name.
    /// @details After a failed run the map may contain the valid declarations
    ///          that preceded the first error.
    /// @return Const reference to the verifier-owned lookup map.
    [[nodiscard]] const ExternMap &externs() const;

    /// @brief Verify extern declarations in @p module and populate the lookup map.
    /// @details Clears prior state, rejects malformed names and unsupported
    ///          parameter types, enforces unique names, and compares known
    ///          runtime externs against their canonical signatures and
    ///          behavioral attributes.
    /// @param module Module whose extern table should be validated.
    /// @param sink Diagnostic sink receiving advisory messages (currently unused).
    /// @return Empty on success; diagnostic describing the first failure otherwise.
    [[nodiscard]] il::support::Expected<void> run(const il::core::Module &module, DiagSink &sink);

  private:
    /// @brief Current borrowed extern index, rebuilt by each call to @ref run.
    ExternMap externs_;
};

} // namespace il::verify
