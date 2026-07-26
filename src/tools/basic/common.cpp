//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/basic/common.cpp
// Purpose: Provide shared command-line helpers used by BASIC developer tools.
// Key invariants: Usage messages remain consistent across tools by reusing the
//                 same usage macro; successful file loads always register a
//                 source-manager entry before returning the identifier to the
//                 caller.
// Ownership/Lifetime: The helper borrows the caller-provided string buffer and
//                     source manager, storing file contents directly into the
//                     buffer so ownership never escapes the tool-specific
//                     entrypoint.
// Links: docs/internals/codemap.md#tools, src/tools/basic/common.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Contains reusable BASIC tool helpers for argument handling and I/O.
/// @details Each BASIC developer tool links this translation unit so they can
///          share usage text, file loading, and diagnostic messaging.

#include "tools/basic/common.hpp"

#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"
#include "tools/common/source_loader.hpp"

#include <iostream>
#include <optional>
#include <sstream>

namespace il::tools::basic {

namespace {
#ifdef ZANNA_BASIC_TOOL_USAGE
constexpr const char *kUsageMessage = ZANNA_BASIC_TOOL_USAGE;
#else
#error "ZANNA_BASIC_TOOL_USAGE must be defined for BASIC tool builds"
#endif
} // namespace

/// @brief Load a BASIC source file and register it with a SourceManager.
///
/// @details A null path emits the build-configured usage message immediately.
///          Otherwise the function delegates bounded reading and file-id
///          registration to @ref il::tools::common::loadSourceBuffer, prints any
///          returned structured diagnostic through the canonical text renderer,
///          and transfers the loaded bytes into @p buffer only after every step
///          succeeds. The caller's buffer therefore remains unchanged on error.
///
/// @param path Filesystem path provided on the command line.
/// @param buffer Destination string that receives the file contents on success.
/// @param sm Source manager used to allocate a file identifier for the buffer.
/// @return File identifier when the load succeeds; `std::nullopt` on error.
std::optional<std::uint32_t> loadBasicSource(const char *path,
                                             std::string &buffer,
                                             il::support::SourceManager &sm) {
    if (path == nullptr) {
        std::cerr << kUsageMessage;
        return std::nullopt;
    }

    auto loaded = il::tools::common::loadSourceBuffer(path, sm);
    if (!loaded) {
        il::support::printDiag(loaded.error(), std::cerr, &sm);
        return std::nullopt;
    }

    const std::uint32_t fileId = loaded.value().fileId;
    buffer = std::move(loaded.value().buffer);
    return fileId;
}

} // namespace il::tools::basic
