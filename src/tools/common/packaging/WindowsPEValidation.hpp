//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/WindowsPEValidation.hpp
// Purpose: Declare bounded structural validation for Windows PE32+ executables.
// Key invariants:
//   - Every integer read is range-checked against the caller-owned byte snapshot.
//   - Accepted images are executable, non-DLL x64 or ARM64 PE32+ files.
//   - Header, section, entry-point, and data-directory ranges cannot overlap or escape.
// Ownership/Lifetime:
//   - Validation borrows the input bytes and writes no process-global state.
//   - Diagnostic and optional image-info outputs remain caller-owned.
// Links: WindowsPEValidation.cpp, PkgVerify.cpp, WindowsInstallerHost.cpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace zanna::pkg {

/// @brief Identifying metadata recovered from a validated Windows executable.
struct WindowsPEImageInfo {
    uint16_t machine{0};       ///< COFF machine (`0x8664` x64 or `0xAA64` ARM64).
    uint16_t subsystem{0};     ///< Windows GUI (`2`) or console (`3`) subsystem.
    uint32_t entryPoint{0};    ///< Validated executable-section entry-point RVA.
    uint32_t sizeOfImage{0};   ///< Loader-visible image extent.
    uint32_t sizeOfHeaders{0}; ///< File and loader header extent.
};

/// @brief Validate one complete PE32+ executable snapshot.
/// @details The validator accepts only bounded x64/ARM64 executable images, checks
///          loader alignment rules, proves raw and virtual section separation,
///          requires the entry point to belong to an executable section, and
///          validates every declared data-directory range. Extra bytes after PE
///          sections remain permitted for ZIP overlays and Authenticode data.
/// @param data Borrowed file bytes.
/// @param size Number of available bytes.
/// @param expectedMachine Required COFF machine, or zero to accept x64/ARM64.
/// @param outInfo Optional receiver cleared before validation.
/// @param error Receiver cleared on entry and populated on validation failure.
/// @return True only when the complete loader-facing structure is valid.
bool validateWindowsPEImage(const uint8_t *data,
                            size_t size,
                            uint16_t expectedMachine,
                            WindowsPEImageInfo *outInfo,
                            std::string &error);

} // namespace zanna::pkg
