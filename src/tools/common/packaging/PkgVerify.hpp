//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgVerify.hpp
// Purpose: Post-build verification of generated packages — checks structural
//          correctness of ZIP, .deb (ar), and PE files after creation.
//
// Key invariants:
//   - All functions return true on success, false on structural error.
//   - Errors are reported to the provided ostream.
//   - Read-only verification — no modifications to the input data.
//
// Ownership/Lifetime:
//   - Pure functions operating on in-memory byte vectors.
//
// Links: ZipWriter.hpp, ArWriter.hpp, PEBuilder.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares read-only structural verifiers for Zanna package artifacts.
/// @details Each verifier consumes an in-memory byte vector, returns false on
///          malformed or unsafe content, and writes a human-readable diagnostic
///          to the supplied stream without modifying the artifact.

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief Verify structural correctness of a ZIP archive.
///
/// Checks:
///   - End-of-central-directory signature present
///   - Entry count matches central directory
///   - Local header signatures valid
///
/// @param data ZIP file bytes.
/// @param err  Stream for error messages.
/// @return true if the ZIP is structurally valid.
bool verifyZip(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify a macOS .app ZIP contains the required app payload files.
/// @param data ZIP file bytes.
/// @param appBundleName Expected `.app` bundle directory name inside the ZIP.
/// @param executableName Expected executable name under `Contents/MacOS`.
/// @param err Stream for error messages.
/// @return true if the ZIP is valid and contains the required bundle files.
bool verifyMacOSAppZip(const std::vector<uint8_t> &data,
                       const std::string &appBundleName,
                       const std::string &executableName,
                       std::ostream &err);

/// @brief Verify a macOS .app ZIP contains the standard bundle files plus extra payload files.
/// @param data ZIP file bytes.
/// @param appBundleName Expected `.app` bundle directory name inside the ZIP.
/// @param executableName Expected executable name under `Contents/MacOS`.
/// @param requiredResourcePaths Paths that must exist under `Contents/Resources`.
/// @param err Stream for error messages.
/// @return true if the ZIP is valid and contains every required bundle/resource file.
bool verifyMacOSAppZipPayload(const std::vector<uint8_t> &data,
                              const std::string &appBundleName,
                              const std::string &executableName,
                              const std::vector<std::string> &requiredResourcePaths,
                              std::ostream &err);

/// @brief Verify structural correctness of a .deb (ar) archive.
///
/// Checks:
///   - ar magic "!<arch>\n" present
///   - First member is "debian-binary" with content "2.0\n"
///   - "control.tar.gz" and "data.tar.gz" members present
///
/// @param data .deb file bytes.
/// @param err  Stream for error messages.
/// @return true if the .deb is structurally valid.
bool verifyDeb(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify a .deb archive and assert required data.tar payload paths.
/// @param data .deb file bytes.
/// @param requiredPaths Paths that must be present in the data.tar member.
/// @param err Stream for error messages.
/// @return true if the .deb is valid and contains every required path.
bool verifyDebPayload(const std::vector<uint8_t> &data,
                      const std::vector<std::string> &requiredPaths,
                      std::ostream &err);

/// @brief Verify a gzip-compressed USTAR tarball.
///
/// Checks:
///   - gzip framing, CRC-32, and uncompressed size
///   - tar headers, checksums, end markers, and safe relative paths
///
/// @param data .tar.gz file bytes.
/// @param err  Stream for error messages.
/// @return true if the tarball is structurally valid.
bool verifyTarGz(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify a .tar.gz archive and assert required payload paths.
/// @param data .tar.gz file bytes.
/// @param requiredPaths Paths that must be present in the tarball.
/// @param err Stream for error messages.
/// @return true if the tarball is valid and contains every required path.
bool verifyTarGzPayload(const std::vector<uint8_t> &data,
                        const std::vector<std::string> &requiredPaths,
                        std::ostream &err);

/// @brief Verify structural correctness of a portable ASCII or newc CPIO archive.
/// @param data CPIO archive bytes.
/// @param err Stream for error messages.
/// @return true if the CPIO archive is structurally valid.
bool verifyCpioNewc(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify a CPIO archive and assert required payload paths.
/// @param data CPIO archive bytes.
/// @param requiredPaths Paths that must be present in the archive.
/// @param err Stream for error messages.
/// @return true if the archive is valid and contains every required path.
bool verifyCpioNewcPayload(const std::vector<uint8_t> &data,
                           const std::vector<std::string> &requiredPaths,
                           std::ostream &err);

/// @brief Verify structural correctness of a XAR archive.
/// @param data XAR archive bytes.
/// @param err Stream for error messages.
/// @return true if the XAR archive is structurally valid.
bool verifyXar(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify a macOS flat `.pkg` archive.
/// @param data `.pkg` (XAR) file bytes.
/// @param err Stream for error messages.
/// @return true if the flat package is structurally valid.
bool verifyMacOSPkg(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify a macOS flat `.pkg` archive and assert payload paths.
/// @param data `.pkg` (XAR) file bytes.
/// @param requiredPaths Paths that must be present in the package payload.
/// @param err Stream for error messages.
/// @return true if the package is valid and contains every required path.
bool verifyMacOSPkgPayload(const std::vector<uint8_t> &data,
                           const std::vector<std::string> &requiredPaths,
                           std::ostream &err);

/// @brief Verify structural correctness of a macOS UDIF `.dmg` image.
/// @details Performs dependency-free checks for the UDIF trailer signature and
///          basic image bounds. Runtime build paths may additionally shell out
///          to `hdiutil verify` when available on macOS hosts.
/// @param data `.dmg` file bytes.
/// @param err Stream for error messages.
/// @return true if the disk image has a valid UDIF trailer.
bool verifyMacOSDmg(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify RPM lead, signature/main header bounds, required metadata tags, and payload.
/// @param data Complete `.rpm` bytes.
/// @param err Stream for structural diagnostics.
/// @return true when the RPM container is structurally plausible and non-empty.
bool verifyRpm(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify structural correctness of a PE32+ executable.
///
/// Checks:
///   - DOS header "MZ" magic
///   - PE signature "PE\0\0" at e_lfanew
///   - Section headers non-overlapping
///
/// @param data PE file bytes.
/// @param err  Stream for error messages.
/// @return true if the PE is structurally valid.
bool verifyPE(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify a PE32+ executable that is expected to carry a ZIP overlay.
///
/// Checks:
///   - Base PE structure is valid
///   - Overlay exists after the final section payload
///   - Overlay bytes form a structurally valid ZIP archive
///
/// @param data PE file bytes.
/// @param err  Stream for error messages.
/// @return true if the PE and its ZIP overlay are structurally valid.
bool verifyPEZipOverlay(const std::vector<uint8_t> &data, std::ostream &err);

/// @brief Verify a PE ZIP overlay and assert required ZIP entries.
/// @param data PE file bytes carrying a ZIP overlay.
/// @param requiredEntries Entry names that must be present in the overlay ZIP.
/// @param err Stream for error messages.
/// @return true if the overlay is valid and contains every required entry.
bool verifyPEZipOverlayPayload(const std::vector<uint8_t> &data,
                               const std::vector<std::string> &requiredEntries,
                               std::ostream &err);

/// @brief Verify a PE ZIP overlay plus a nested ZIP entry payload.
/// @details Validates the outer overlay ZIP, confirms @p requiredOuterEntries
///          are present, then opens the nested ZIP stored at @p innerZipEntry and
///          confirms @p requiredInnerEntries are present inside it.
/// @param data PE file bytes carrying a ZIP overlay.
/// @param requiredOuterEntries Entry names required in the outer overlay ZIP.
/// @param innerZipEntry Name of the overlay entry that is itself a ZIP archive.
/// @param requiredInnerEntries Entry names required inside the nested ZIP.
/// @param err Stream for error messages.
/// @return true if both the outer overlay and the nested ZIP satisfy their
///         required-entry lists.
bool verifyPEZipOverlayNestedPayload(const std::vector<uint8_t> &data,
                                     const std::vector<std::string> &requiredOuterEntries,
                                     const std::string &innerZipEntry,
                                     const std::vector<std::string> &requiredInnerEntries,
                                     std::ostream &err);

/// @brief Recursively verify a schema-3 native Windows setup package.
/// @details Checks metadata, every payload/shortcut/outer-file SHA-256, the cleanup
///          PE, the non-recursive maintenance executable, and setup/maintenance
///          identity and payload parity.
/// @param data Complete native setup executable bytes.
/// @param err Stream for PE, metadata, inventory, hash, or recursion diagnostics.
/// @return true when the setup and its embedded maintenance package fully verify.
bool verifyWindowsNativeInstaller(const std::vector<uint8_t> &data, std::ostream &err);

} // namespace zanna::pkg
