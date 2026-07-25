//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/api/expected_api.hpp
// Purpose: Provide Expected-based wrappers for IL parsing and verification entry points.
// Key invariants: Wrapper success mirrors legacy bool-returning APIs; errors carry diagnostic text
// Ownership/Lifetime: Both wrappers borrow their module/stream arguments for
//          the duration of the call. The returned Expected owns any diagnostic
//          payload; parsing mutates the caller-owned module.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

#pragma once

#include <istream>

#include "support/diag_expected.hpp"

namespace il::core {
struct Module;
} // namespace il::core

namespace il::api::v2 {
/// @brief Parse IL text into a module while capturing diagnostics in an Expected result.
/// @param is Input stream containing IL text.
/// @param m Module instance populated on successful parse.
/// @return Empty Expected on success; diagnostic payload on parse failure.
il::support::Expected<void> parse_text_expected(std::istream &is, il::core::Module &m);

/// @brief Verify a module while capturing diagnostics in an Expected result.
/// @param m Module to be verified.
/// @return Empty Expected on success; diagnostic payload on verification failure.
il::support::Expected<void> verify_module_expected(const il::core::Module &m);

} // namespace il::api::v2
