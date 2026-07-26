//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/native_compiler.hpp
// Purpose: Shared utility for compiling IL to native binaries via codegen backends.
// Key invariants: detectHostArch() is constexpr and determined at compile time.
//                 compileToNative() dispatches to ARM64 or x64 backends based on arch.
// Ownership/Lifetime: Callers retain ownership of all paths.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares shared architecture dispatch and temporary-file support for native builds.
/// @details Frontend tools use this API to feed textual or in-memory IL into the
///          AArch64 and x86-64 codegen pipelines with common optimization,
///          asset, linker, runtime, timing, and stack-size options.

#pragma once

#include "il/core/Module.hpp"

#include <optional>
#include <string>

namespace zanna::tools {

/// @brief Target architecture for native code generation.
/// @details The value chooses a backend independently from the host operating
///          system; platform-specific ABI selection still follows the host build.
enum class TargetArch { ARM64, X64 };

/// @brief Detect the host architecture at compile time.
/// @return ARM64 for recognized AArch64 build macros; X64 as the fallback.
/// @note The fallback assumes supported non-AArch64 builds target x86-64.
constexpr TargetArch detectHostArch() {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return TargetArch::ARM64;
#else
    return TargetArch::X64;
#endif
}

/// @brief Check if an output path implies native binary output.
/// @param path The output path to inspect.
/// @return False only for a `.il` suffix matched case-insensitively; true for
///         every other extension or an extensionless path.
bool isNativeOutputPath(const std::string &path);

/// @brief Compile an IL file on disk to a native binary.
///
/// For ARM64, delegates to the existing cmd_codegen_arm64 entry point.
/// For X64, uses the CodegenPipeline directly.
///
/// @param ilPath Path to the IL text file on disk.
/// @param outputPath Path for the output native binary.
/// @param arch Target architecture (defaults to host architecture).
/// @param assetBlobPath Path to ZPAK asset blob for .rodata embedding (optional).
/// @param assetObjPath  Path to extra .o file with asset blob C array (optional).
/// @param backendOptimizeLevel Backend optimization level to preserve after frontend IL
/// optimization.
/// @param skipIlOptimization True when the input IL has already run the frontend/project pipeline.
/// @param timePasses When true, the codegen pipeline prints per-pass timing statistics.
/// @param fastLink When true, selects the faster (less optimized) link path.
/// @param windowsDebugRuntime Optional override selecting the Windows debug CRT; std::nullopt
/// leaves the platform default in effect (ignored on non-Windows targets).
/// @param stackSize Requested executable stack size in bytes, or zero for the
///        platform/linker default.
/// @return 0 on success, non-zero on failure.
int compileToNative(const std::string &ilPath,
                    const std::string &outputPath,
                    TargetArch arch = detectHostArch(),
                    const std::string &assetBlobPath = "",
                    const std::string &assetObjPath = "",
                    int backendOptimizeLevel = 1,
                    bool skipIlOptimization = true,
                    bool timePasses = false,
                    bool fastLink = false,
                    std::optional<bool> windowsDebugRuntime = std::nullopt,
                    std::size_t stackSize = 0);

/// @brief Compile an already-built IL module to a native binary without reparsing IL text.
///
/// @details Mirrors @ref compileToNative but accepts an in-memory module, avoiding
///          a serialize/reparse round-trip. @p debugSourcePath, or the synthetic
///          label `<in-memory>` when empty, satisfies the pipeline's
///          @c input_il_path option; no temporary IL text is written.
///          The module is moved into the selected backend pipeline.
///
/// @param module IL module to compile; consumed (moved) by the pipeline.
/// @param debugSourcePath Original source path recorded for debug info/diagnostics.
/// @param outputPath Path for the output native binary.
/// @param arch Target architecture (defaults to host architecture).
/// @param assetBlobPath Path to ZPAK asset blob for .rodata embedding (optional).
/// @param assetObjPath Path to extra .o file with asset blob C array (optional).
/// @param backendOptimizeLevel Backend optimization level applied during codegen.
/// @param skipIlOptimization True when the module has already run the frontend/project pipeline.
/// @param moduleAlreadyVerified True to skip re-verifying the module before codegen.
/// @param timePasses When true, the codegen pipeline prints per-pass timing statistics.
/// @param fastLink When true, selects the faster (less optimized) link path.
/// @param windowsDebugRuntime Optional override selecting the Windows debug CRT; std::nullopt
/// leaves the platform default in effect (ignored on non-Windows targets).
/// @param stackSize Requested executable stack size in bytes, or zero for the
///        platform/linker default.
/// @return 0 on success, non-zero on failure.
int compileModuleToNative(il::core::Module module,
                          const std::string &debugSourcePath,
                          const std::string &outputPath,
                          TargetArch arch = detectHostArch(),
                          const std::string &assetBlobPath = "",
                          const std::string &assetObjPath = "",
                          int backendOptimizeLevel = 1,
                          bool skipIlOptimization = true,
                          bool moduleAlreadyVerified = true,
                          bool timePasses = false,
                          bool fastLink = false,
                          std::optional<bool> windowsDebugRuntime = std::nullopt,
                          std::size_t stackSize = 0);

/// @brief Generate and reserve a unique temporary file for IL serialization.
/// @return Path to an empty file in the system temp directory with a `.il` extension.
/// @throws std::runtime_error When the temp directory cannot be located or a
///         unique file cannot be reserved.
std::string generateTempIlPath();

/// @brief Generate and reserve a unique temporary file path.
/// @param prefix Leading filename component.
/// @param extension Suffix including any desired leading dot.
/// @return Path in the system temp directory; an empty file is created exclusively.
/// @throws std::runtime_error When the directory lookup or exclusive creation fails.
/// @note The caller owns cleanup of the reserved file.
std::string generateTempFilePath(const char *prefix, const char *extension);

/// @brief Generate and reserve a unique temporary file for an asset blob.
/// @return Path to an empty file in the system temp directory with a `.zpak` extension.
/// @throws std::runtime_error When the temp directory cannot be located or a
///         unique file cannot be reserved.
std::string generateTempAssetPath();

} // namespace zanna::tools
