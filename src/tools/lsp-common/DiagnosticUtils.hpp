//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/lsp-common/DiagnosticUtils.hpp
// Purpose: Shared utility for converting DiagnosticEngine results to
//          server-agnostic DiagnosticInfo structs.
// Key invariants:
//   - Works with any frontend's DiagnosticEngine output
// Ownership/Lifetime:
//   - All returned data is fully owned
// Links: tools/lsp-common/ServerTypes.hpp, support/diagnostics.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Declares compiler-to-server diagnostic conversion.

#include "tools/lsp-common/ServerTypes.hpp"

#include <vector>

namespace il::support {
class DiagnosticEngine;
class SourceManager;
} // namespace il::support

namespace zanna::server {

/// @brief Extract structured diagnostics from a DiagnosticEngine.
/// @details Converts each engine diagnostic into a server-agnostic DiagnosticInfo
///          (message, severity, 1-based location and optional end range, code,
///          stage, help, related notes, and fix-its) suitable for LSP or MCP
///          responses, independent of any particular frontend.
/// @param diag Diagnostic engine holding the collected frontend diagnostics.
/// @param sm Optional source manager used to resolve file paths for the primary
///           location and related notes; pass nullptr when unavailable.
/// @return One DiagnosticInfo per diagnostic, in engine order.
std::vector<DiagnosticInfo> extractDiagnostics(const il::support::DiagnosticEngine &diag,
                                               const il::support::SourceManager *sm = nullptr);

} // namespace zanna::server
