//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/LinuxRuntimeStubGen.hpp
// Purpose: Generate the Zanna self-extracting Linux bundle runtime stub.
//
// Key invariants:
//   - The payload is appended after a marker line and is a gzip-compressed tar archive.
//   - The generated file is executable without FUSE or squashfs support.
//   - Extraction prefers XDG_CACHE_HOME, then TMPDIR, then /tmp.
//
// Ownership/Lifetime:
//   - Output returned as a byte vector; callers append the payload bytes.
//
// Links: LinuxPackageBuilder.cpp, TarWriter.hpp, PkgGzip.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares Linux self-extracting runtime-stub generation and verification.
/// @details A textual POSIX shell runtime is combined with a gzip-tar payload to
///          form one executable, FUSE-independent bundle.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Marker line separating the runtime stub from its appended binary payload.
/// @details The payload begins on the line immediately following this marker.
constexpr const char *kLinuxRuntimePayloadMarker = "__ZANNA_APPIMAGE_PAYLOAD_BELOW__";

/// @brief Parameters embedded in the self-extracting Linux bundle runtime stub.
struct LinuxRuntimeStubParams {
    std::string cacheName; ///< Stable normalized cache prefix, such as `zanna-1.2.3-x64`.
    std::string entryPath; ///< Normalized payload-relative executable path, such as `bin/zanna`.
    std::string payloadSha256; ///< Optional lowercase SHA-256 of the appended tar.gz payload.
    bool appImageInterface{false}; ///< Expose AppImage-named flags for real application AppImages.
};

/// @brief Build the textual runtime prefix for a self-extracting Linux bundle.
/// @param params Validated values and command-line interface options to embed.
/// @return Executable shell-stub bytes ending with the payload marker line.
/// @throws std::runtime_error If a token is unsafe or the optional digest is invalid.
std::vector<uint8_t> buildLinuxRuntimeStub(const LinuxRuntimeStubParams &params);

/// @brief Build a complete self-extracting Linux bundle.
/// @param params Runtime values; the payload digest is computed from `payloadTarGz`.
/// @param payloadTarGz Valid gzip-compressed tar payload to append.
/// @return Runtime prefix and payload concatenated into one executable byte vector.
/// @throws std::runtime_error If the payload or parameters are invalid.
std::vector<uint8_t> buildLinuxAppImage(const LinuxRuntimeStubParams &params,
                                        const std::vector<uint8_t> &payloadTarGz);

/// @brief Verify a self-extracting Linux bundle and its payload.
/// @param data Complete candidate bundle bytes.
/// @param err Optional destination for the first validation diagnostic.
/// @return `true` if framing, optional SHA-256 metadata, and tar.gz payload are valid.
bool verifyLinuxAppImage(const std::vector<uint8_t> &data, std::string *err = nullptr);

} // namespace zanna::pkg
