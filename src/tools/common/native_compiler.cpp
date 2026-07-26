//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/native_compiler.cpp
// Purpose: Implement the shared native compilation utility that turns IL (on
//          disk or in memory) into a native object/binary by dispatching to the
//          ARM64 or x86-64 codegen backend.
// Key invariants: Backend selection is driven solely by the requested
//                 TargetArch; the x64 path additionally fixes the target ABI and
//                 platform from the host build macros. Pipeline stdout/stderr are
//                 forwarded to the process streams and the pipeline exit code is
//                 propagated to the caller.
// Ownership/Lifetime: Callers retain ownership of all path strings.
//                     compileModuleToNative takes the module by value and moves
//                     it into the backend pipeline.
// Links: src/tools/common/native_compiler.hpp,
//        src/codegen/aarch64/CodegenPipeline.hpp,
//        src/codegen/x86_64/CodegenPipeline.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Shared IL-to-native compilation helpers used by the CLI frontends.
/// @details Wraps the per-architecture codegen pipelines behind a small, uniform
///          API so tools can request a native build without knowing backend
///          option layouts, and provides temp-path helpers for transient IL and
///          asset blobs.

#include "tools/common/native_compiler.hpp"

#include "codegen/aarch64/CodegenPipeline.hpp"
#include "codegen/x86_64/CodegenPipeline.hpp"
#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "support/source_manager.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if ZANNA_HOST_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace zanna::tools {
namespace {

/// @brief Build a collision-resistant candidate path in the system temp directory.
///
/// @details Combines four sources of uniqueness so concurrent and repeated
///          compilations are unlikely to choose the same transient name: the
///          process id, a steady-clock tick, and a monotonically increasing
///          process-local atomic counter, all under the OS temp directory.
///          Exclusive reservation by @ref reserveTempPath provides the actual
///          collision guarantee.
///
/// @param prefix Leading filename component identifying the artifact kind.
/// @param extension File extension to append (including the leading dot).
/// @return Absolute path string in the system temporary directory.
std::string generateUniqueTempPath(const char *prefix, const char *extension) {
    static std::atomic<uint64_t> counter{0};

    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec);
    if (ec)
        throw std::runtime_error("failed to locate temporary directory: " + ec.message());
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
#if ZANNA_HOST_WINDOWS
    const auto pid = static_cast<uint64_t>(_getpid());
#else
    const auto pid = static_cast<uint64_t>(getpid());
#endif
    const auto uniqueId = counter.fetch_add(1, std::memory_order_relaxed);
    return zanna::filesystem::pathToUtf8(dir / (std::string(prefix) + "_" + std::to_string(pid) +
                                                "_" + std::to_string(tick) + "_" +
                                                std::to_string(uniqueId) + extension));
}

/// @brief Atomically create @p path so it is reserved against concurrent callers.
/// @details Opens the file with O_CREAT|O_EXCL (the Windows equivalent), so the
///          call fails if the path already exists; this closes the TOCTOU window
///          between generating a unique temp name and actually using it.
/// @param path Candidate filesystem path to reserve as an empty file.
/// @return True when the path was reserved; false when it already existed.
/// @throws std::runtime_error for non-collision failures such as permission or
///         filesystem errors.
bool reserveTempPath(const std::string &path) {
#if ZANNA_HOST_WINDOWS
    const std::filesystem::path nativePath = zanna::filesystem::pathFromUtf8(path);
    const int fd = _wopen(nativePath.c_str(),
                          _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
                          _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        if (errno == EEXIST)
            return false;
        throw std::runtime_error("failed to reserve temporary file: " + path + ": " +
                                 std::strerror(errno));
    }
    if (_close(fd) != 0) {
        std::error_code removeError;
        std::filesystem::remove(nativePath, removeError);
        throw std::runtime_error("failed to close reserved temporary file: " + path);
    }
#else
    const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        if (errno == EEXIST)
            return false;
        throw std::runtime_error("failed to reserve temporary file: " + path + ": " +
                                 std::strerror(errno));
    }
    close(fd);
#endif
    return true;
}

} // namespace

/// @brief Determine whether an output path requests a native binary.
/// @details The toolchain treats `.il` as textual IL emission. Any other path,
///          including extensionless paths and common native names such as
///          `a.out`, `.bin`, or `.exe`, selects native code generation.
/// @param path Output path supplied on the command line.
/// @return True when @p path should be treated as a native executable output.
bool isNativeOutputPath(const std::string &path) {
    std::string ext =
        zanna::filesystem::pathToUtf8(zanna::filesystem::pathFromUtf8(path).extension());
    for (char &ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext != ".il";
}

/// @brief Generate a unique temporary path for serialized IL text.
/// @return Path under the system temp directory using the "zanna_build" prefix
///         and a ".il" extension.
std::string generateTempIlPath() {
    return generateTempFilePath("zanna_build", ".il");
}

/// @brief Generate and exclusively reserve a temporary file.
/// @param prefix Leading component used to identify the temporary artifact.
/// @param extension Filename suffix, conventionally including a leading dot.
/// @return UTF-8 path to the newly created empty file.
/// @details Tries up to 64 independently generated candidates and returns after
///          exclusive creation succeeds. The caller is responsible for removal.
/// @throws std::runtime_error When temp-directory discovery, file creation, or
///         all collision retries fail.
std::string generateTempFilePath(const char *prefix, const char *extension) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::string path = generateUniqueTempPath(prefix, extension);
        if (reserveTempPath(path))
            return path;
    }
    throw std::runtime_error("failed to reserve temporary file");
}

/// @brief Compile a textual IL file on disk to a native binary.
/// @details Parses backend selection from @p arch: ARM64 routes through the
///          AArch64 @c CodegenPipeline while every other value uses the x86-64
///          pipeline with its ABI/platform fixed from host build macros. Asset
///          blobs and extra object files are attached to the pipeline options
///          when provided. Pipeline output is forwarded to stdout/stderr and the
///          pipeline's exit code is returned; both backends map a thrown
///          std::exception from pipeline execution to exit code 2.
/// @param ilPath Textual IL input file to parse.
/// @param outputPath Destination native object or executable path.
/// @param arch Backend architecture to select.
/// @param assetBlobPath Optional ZPAK blob path for read-only-data embedding.
/// @param assetObjPath Optional additional object containing asset symbols.
/// @param backendOptimizeLevel Backend optimization level.
/// @param skipIlOptimization Whether frontend IL optimization already ran.
/// @param timePasses Whether to emit per-pass timing statistics.
/// @param fastLink Whether to select the faster link strategy.
/// @param windowsDebugRuntime Optional Windows debug-CRT override.
/// @param stackSize Requested executable stack bytes, or zero for the default.
/// @return Backend pipeline exit code, or two for a caught execution exception.
int compileToNative(const std::string &ilPath,
                    const std::string &outputPath,
                    TargetArch arch,
                    const std::string &assetBlobPath,
                    const std::string &assetObjPath,
                    int backendOptimizeLevel,
                    bool skipIlOptimization,
                    bool timePasses,
                    bool fastLink,
                    std::optional<bool> windowsDebugRuntime,
                    std::size_t stackSize) {
    if (arch == TargetArch::ARM64) {
        zanna::codegen::aarch64::CodegenPipeline::Options opts;
        opts.input_il_path = ilPath;
        opts.output_obj_path = outputPath;
        opts.optimize = backendOptimizeLevel;
        opts.skip_il_optimization = skipIlOptimization;
        opts.asset_blob_path = assetBlobPath;
        opts.time_passes = timePasses;
        opts.fast_link = fastLink;
        opts.windows_debug_runtime = windowsDebugRuntime;
        opts.stack_size = stackSize;
        if (!assetObjPath.empty())
            opts.extra_objects.push_back(assetObjPath);

        zanna::codegen::aarch64::CodegenPipeline pipeline(opts);
        try {
            const auto result = pipeline.run();
            if (!result.stdout_text.empty())
                std::cout << result.stdout_text;
            if (!result.stderr_text.empty())
                std::cerr << result.stderr_text;
            return result.exit_code;
        } catch (const std::exception &e) {
            std::cerr << "error: " << e.what() << "\n";
            return 2;
        }
    }

    // X64: use CodegenPipeline directly
    zanna::codegen::x64::CodegenPipeline::Options opts;
    opts.input_il_path = ilPath;
    opts.output_obj_path = outputPath;
    opts.optimize = backendOptimizeLevel;
    opts.skip_il_optimization = skipIlOptimization;
    opts.asset_blob_path = assetBlobPath;
    opts.time_passes = timePasses;
    opts.fast_link = fastLink;
    opts.windows_debug_runtime = windowsDebugRuntime;
    opts.stack_size = stackSize;
#if ZANNA_HOST_WINDOWS
    opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::Win64;
    opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Windows;
#elif ZANNA_HOST_MACOS
    opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::SysV;
    opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Darwin;
#else
    opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::SysV;
    opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Linux;
#endif
    if (!assetObjPath.empty())
        opts.extra_objects.push_back(assetObjPath);

    zanna::codegen::x64::CodegenPipeline pipeline(opts);
    PipelineResult result;
    try {
        result = pipeline.run();
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }

    if (!result.stdout_text.empty())
        std::cout << result.stdout_text;
    if (!result.stderr_text.empty())
        std::cerr << result.stderr_text;

    return result.exit_code;
}

/// @brief Compile an in-memory IL module to a native binary.
/// @details Backend dispatch mirrors @ref compileToNative, but the module is fed
///          to the pipeline via @c runWithModule (moved in) instead of being
///          reparsed from disk. The debug source path, or `<in-memory>` when it
///          is empty, populates @c input_il_path for diagnostics without writing
///          any IL text.
///          @p moduleAlreadyVerified lets callers skip a redundant verification
///          pass.
/// @param module IL module transferred into the selected pipeline.
/// @param debugSourcePath Source path used for debug metadata and diagnostics.
/// @param outputPath Destination native object or executable path.
/// @param arch Backend architecture to select.
/// @param assetBlobPath Optional ZPAK blob path for read-only-data embedding.
/// @param assetObjPath Optional additional object containing asset symbols.
/// @param backendOptimizeLevel Backend optimization level.
/// @param skipIlOptimization Whether frontend IL optimization already ran.
/// @param moduleAlreadyVerified Whether the caller has completed verification.
/// @param timePasses Whether to emit per-pass timing statistics.
/// @param fastLink Whether to select the faster link strategy.
/// @param windowsDebugRuntime Optional Windows debug-CRT override.
/// @param stackSize Requested executable stack bytes, or zero for the default.
/// @return Backend pipeline exit code, or two for a caught execution exception.
int compileModuleToNative(il::core::Module module,
                          const std::string &debugSourcePath,
                          const std::string &outputPath,
                          TargetArch arch,
                          const std::string &assetBlobPath,
                          const std::string &assetObjPath,
                          int backendOptimizeLevel,
                          bool skipIlOptimization,
                          bool moduleAlreadyVerified,
                          bool timePasses,
                          bool fastLink,
                          std::optional<bool> windowsDebugRuntime,
                          std::size_t stackSize) {
    const std::string syntheticInputPath =
        debugSourcePath.empty() ? std::string{"<in-memory>"} : debugSourcePath;

    if (arch == TargetArch::ARM64) {
        zanna::codegen::aarch64::CodegenPipeline::Options opts;
        opts.input_il_path = syntheticInputPath;
        opts.output_obj_path = outputPath;
        opts.optimize = backendOptimizeLevel;
        opts.skip_il_optimization = skipIlOptimization;
        opts.asset_blob_path = assetBlobPath;
        opts.time_passes = timePasses;
        opts.fast_link = fastLink;
        opts.windows_debug_runtime = windowsDebugRuntime;
        opts.stack_size = stackSize;
        if (!assetObjPath.empty())
            opts.extra_objects.push_back(assetObjPath);

        zanna::codegen::aarch64::CodegenPipeline pipeline(opts);
        try {
            const auto result =
                pipeline.runWithModule(std::move(module), debugSourcePath, moduleAlreadyVerified);
            if (!result.stdout_text.empty())
                std::cout << result.stdout_text;
            if (!result.stderr_text.empty())
                std::cerr << result.stderr_text;
            return result.exit_code;
        } catch (const std::exception &e) {
            std::cerr << "error: " << e.what() << "\n";
            return 2;
        }
    }

    zanna::codegen::x64::CodegenPipeline::Options opts;
    opts.input_il_path = syntheticInputPath;
    opts.output_obj_path = outputPath;
    opts.optimize = backendOptimizeLevel;
    opts.skip_il_optimization = skipIlOptimization;
    opts.asset_blob_path = assetBlobPath;
    opts.time_passes = timePasses;
    opts.fast_link = fastLink;
    opts.windows_debug_runtime = windowsDebugRuntime;
    opts.stack_size = stackSize;
#if ZANNA_HOST_WINDOWS
    opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::Win64;
    opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Windows;
#elif ZANNA_HOST_MACOS
    opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::SysV;
    opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Darwin;
#else
    opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::SysV;
    opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Linux;
#endif
    if (!assetObjPath.empty())
        opts.extra_objects.push_back(assetObjPath);

    zanna::codegen::x64::CodegenPipeline pipeline(opts);
    PipelineResult result;
    try {
        result = pipeline.runWithModule(std::move(module), debugSourcePath, moduleAlreadyVerified);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }

    if (!result.stdout_text.empty())
        std::cout << result.stdout_text;
    if (!result.stderr_text.empty())
        std::cerr << result.stderr_text;

    return result.exit_code;
}

/// @brief Generate a unique temporary path for an asset blob.
/// @return Path under the system temp directory using the "zanna_assets" prefix
///         and a ".zpak" extension.
std::string generateTempAssetPath() {
    return generateTempFilePath("zanna_assets", ".zpak");
}

} // namespace zanna::tools
