//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/CodegenPipeline.cpp
// Purpose: Implement the reusable IL-to-x86-64 compilation pipeline used by
//          CLI front ends. Coordinates module loading, verification, backend
//          pass execution, and optional linking/execution.
// Key invariants:
//   - Passes execute sequentially with early exits on failure, ensuring
//     diagnostics are recorded deterministically.
//   - No partial artefacts leak on error.
//   - Host ABI selection, native-link fallback, and system tool invocation
//     are resolved at runtime based on TargetPlatform.
// Cross-platform touchpoints:
//   - Native-link archive discovery and platform-specific linker options are
//     routed through codegen/common/LinkerSupport and NativeLinker.
// Ownership/Lifetime:
//   - The pipeline borrows IL modules and writes assembly/binaries to
//     caller-specified locations without assuming ownership of resources.
// Links: src/codegen/x86_64/CodegenPipeline.hpp,
//        src/codegen/x86_64/Backend.hpp,
//        src/codegen/x86_64/passes/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/CodegenPipeline.hpp"

#include "codegen/common/LinkerSupport.hpp"
#include "codegen/common/NativeEHLowering.hpp"
#include "codegen/common/linker/NativeLinker.hpp"
#include "codegen/common/objfile/ObjectFileWriter.hpp"
#include "codegen/x86_64/passes/BinaryEmitPass.hpp"
#include "codegen/x86_64/passes/EmitPass.hpp"
#include "codegen/x86_64/passes/LegalizePass.hpp"
#include "codegen/x86_64/passes/LoweringPass.hpp"
#include "codegen/x86_64/passes/PassManager.hpp"
#include "codegen/x86_64/passes/PeepholePass.hpp"
#include "codegen/x86_64/passes/PreRegAllocOptPass.hpp"
#include "codegen/x86_64/passes/RegAllocPass.hpp"
#include "codegen/x86_64/passes/SchedulerPass.hpp"
#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "il/transform/PassManager.hpp"
#include "tools/common/module_loader.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Implements the x86-64 CLI compilation, object, link, and run workflow.
 *
 * The pipeline combines architecture-independent IL preparation and linker
 * discovery with the x86-64 MIR pass stack. It supports native object encoding
 * and a system-assembler path, then funnels executable production through the
 * built-in linker with runtime archives selected from referenced symbols.
 */

namespace zanna::codegen::x64 {
namespace {

/// Concise alias for the public platform-selection enum.
using TargetPlatform = CodegenOptions::TargetPlatform;

/// @brief Resolve the concrete target platform from pipeline options.
/// @details Honors an explicit non-Host @c target_platform; otherwise infers
///          it from the ABI (Win64 → Windows) or the detected host linker
///          platform. Falls back to Linux if detection is inconclusive.
/// @param opts Pipeline options containing platform and ABI selections.
/// @return Explicit Darwin, Linux, or Windows target platform.
[[nodiscard]] TargetPlatform effectiveTargetPlatform(const CodegenPipeline::Options &opts) {
    if (opts.target_platform != TargetPlatform::Host)
        return opts.target_platform;
    if (opts.target_abi == CodegenOptions::TargetABI::Win64)
        return TargetPlatform::Windows;
    switch (linker::detectLinkPlatform()) {
        case linker::LinkPlatform::macOS:
            return TargetPlatform::Darwin;
        case linker::LinkPlatform::Windows:
            return TargetPlatform::Windows;
        case linker::LinkPlatform::Linux:
            return TargetPlatform::Linux;
    }
    return TargetPlatform::Linux;
}

/// @brief Map a target platform to its native object-file format
///        (Darwin→Mach-O, Linux→ELF, Windows→COFF; Host→detected).
/// @param platform Target platform to map.
/// @return Mach-O, ELF, or COFF object format.
[[nodiscard]] objfile::ObjFormat targetObjectFormat(TargetPlatform platform) {
    switch (platform) {
        case TargetPlatform::Darwin:
            return objfile::ObjFormat::MachO;
        case TargetPlatform::Linux:
            return objfile::ObjFormat::ELF;
        case TargetPlatform::Windows:
            return objfile::ObjFormat::COFF;
        case TargetPlatform::Host:
            return objfile::detectHostFormat();
    }
    return objfile::detectHostFormat();
}

/// @brief Map a target platform to the linker's LinkPlatform enum
///        (Host resolves via the detected host linker platform).
/// @param platform Code-generation platform selection.
/// @return Equivalent native-linker platform value.
[[nodiscard]] linker::LinkPlatform targetLinkPlatform(TargetPlatform platform) {
    switch (platform) {
        case TargetPlatform::Darwin:
            return linker::LinkPlatform::macOS;
        case TargetPlatform::Linux:
            return linker::LinkPlatform::Linux;
        case TargetPlatform::Windows:
            return linker::LinkPlatform::Windows;
        case TargetPlatform::Host:
            return linker::detectLinkPlatform();
    }
    return linker::detectLinkPlatform();
}

/// @brief Build the system assembler command prefix for @p platform.
/// @details On a native host uses `cc`, the POSIX-mandated C driver name, so
///          the mode works on distributions whose default toolchain is GCC;
///          cross-targets use `clang` with the matching `--target=` x86-64
///          triple for the target OS, since only Clang assembles for a
///          foreign triple without a separate cross toolchain.
/// @param platform Platform whose assembler driver and target flags are needed.
/// @return Executable name and fixed arguments preceding input/output arguments.
[[nodiscard]] std::vector<std::string> systemAssemblerArgs(TargetPlatform platform) {
    switch (platform) {
        case TargetPlatform::Darwin:
            if constexpr (zanna::platform::kHostMacOS) {
                return {"cc", "-arch", "x86_64"};
            }
            return {"clang", "--target=x86_64-apple-macos11"};
        case TargetPlatform::Linux:
            if constexpr (zanna::platform::kHostLinux) {
                return {"cc"};
            }
            return {"clang", "--target=x86_64-unknown-linux-gnu"};
        case TargetPlatform::Windows:
            return {"clang", "--target=x86_64-pc-windows-msvc"};
        case TargetPlatform::Host:
            switch (targetLinkPlatform(TargetPlatform::Host)) {
                case linker::LinkPlatform::macOS:
                    return systemAssemblerArgs(TargetPlatform::Darwin);
                case linker::LinkPlatform::Windows:
                    return systemAssemblerArgs(TargetPlatform::Windows);
                case linker::LinkPlatform::Linux:
                    return systemAssemblerArgs(TargetPlatform::Linux);
            }
    }
    return {"clang", "--target=x86_64-unknown-linux-gnu"};
}

/// @brief Return an ASCII-lowercase copy of a path extension.
/// @details Object-file extension checks must be case-insensitive for Windows
///          paths but should not depend on locale. This helper only folds
///          ASCII A-Z, which is sufficient for ".o" and ".obj".
/// @param ext Extension string including the leading dot.
/// @return Lowercase ASCII copy of @p ext.
[[nodiscard]] std::string lowercaseAsciiExtension(std::string_view ext) {
    std::string out;
    out.reserve(ext.size());
    for (char ch : ext) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

/// @brief Test whether a user-provided output path denotes an object file.
/// @details Recognizes .o and .obj with ASCII case-insensitive matching so
///          Windows-style uppercase extensions are handled consistently in both
///          native and system assembler paths.
/// @param path Output path supplied through pipeline options.
/// @return True when the path extension is an object-file extension.
[[nodiscard]] bool looksLikeObjectFilePath(const std::string &path) {
    const std::size_t dotPos = path.rfind('.');
    if (dotPos == std::string::npos)
        return false;
    const std::string ext = lowercaseAsciiExtension(std::string_view{path}.substr(dotPos));
    return ext == ".o" || ext == ".obj";
}

/// @brief Read an entire binary file with checked sizing and stream-state validation.
/// @details Used for optional codegen asset blobs. The helper reports missing
///          files, invalid sizes, seek failures, and short reads explicitly so
///          user-specified asset inputs are never ignored silently.
/// @param path Source file path to read.
/// @param outBytes Destination byte buffer, replaced on success.
/// @param err Stream receiving a human-readable diagnostic on failure.
/// @return True when the file was read successfully.
[[nodiscard]] bool readBinaryFileChecked(const std::filesystem::path &path,
                                         std::vector<uint8_t> &outBytes,
                                         std::ostream &err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        err << "error: cannot open asset blob '" << zanna::filesystem::pathToUtf8(path) << "'\n";
        return false;
    }
    const std::streampos endPos = in.tellg();
    if (endPos < 0) {
        err << "error: cannot determine size of asset blob '" << zanna::filesystem::pathToUtf8(path)
            << "'\n";
        return false;
    }
    const auto fileSize = static_cast<std::uintmax_t>(endPos);
    if (fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        err << "error: asset blob '" << zanna::filesystem::pathToUtf8(path) << "' is too large\n";
        return false;
    }
    outBytes.assign(static_cast<std::size_t>(fileSize), 0);
    in.seekg(0);
    if (!in) {
        err << "error: cannot seek asset blob '" << zanna::filesystem::pathToUtf8(path) << "'\n";
        return false;
    }
    if (!outBytes.empty()) {
        in.read(reinterpret_cast<char *>(outBytes.data()),
                static_cast<std::streamsize>(outBytes.size()));
        if (!in) {
            err << "error: failed to read asset blob '" << zanna::filesystem::pathToUtf8(path)
                << "'\n";
            return false;
        }
    }
    return true;
}

/// @brief Compute the output assembly path from pipeline options.
/// @details Falls back to sensible defaults when the input path is empty
///          or refers to a directory, mirroring traditional compiler
///          behaviour.
/// @param opts Pipeline configuration specifying the IL input path.
/// @return Filesystem location for the generated assembly file.
std::filesystem::path deriveAssemblyPath(const CodegenPipeline::Options &opts) {
    std::filesystem::path assembly = zanna::filesystem::pathFromUtf8(opts.input_il_path);
    if (assembly.empty()) {
        return std::filesystem::path("out.s");
    }
    assembly.replace_extension(".s");
    if (assembly.filename().empty()) {
        assembly = assembly.parent_path() / "out.s";
    }
    return assembly;
}

/// @brief Determine the executable output path based on user input.
/// @details Strips the IL extension when present and ensures the result has
///          a filename component so the linker output is predictable.
///          On Windows, adds the .exe extension.
/// @param opts Pipeline configuration describing the IL input.
/// @return Filesystem path for the linked executable.
std::filesystem::path deriveExecutablePath(const CodegenPipeline::Options &opts) {
    const bool isWindowsTarget = effectiveTargetPlatform(opts) == TargetPlatform::Windows;
    std::filesystem::path exe = zanna::filesystem::pathFromUtf8(opts.input_il_path);
    if (exe.empty()) {
        return isWindowsTarget ? std::filesystem::path("a.exe") : std::filesystem::path("a.out");
    }
    exe.replace_extension("");
    if (exe.filename().empty() || exe.filename() == ".") {
        return isWindowsTarget ? (exe.parent_path() / "a.exe") : (exe.parent_path() / "a.out");
    }
    if (isWindowsTarget)
        exe.replace_extension(".exe");
    return exe;
}

/// @brief Persist generated assembly to disk.
/// @details Writes @p text to @p path, reporting any failures to the
///          provided error stream. The helper returns @c false when I/O
///          errors occur so the pipeline can stop before invoking the linker.
/// @param path Destination path for the assembly file.
/// @param text Assembly text to write.
/// @param err  Stream receiving human-readable error messages.
/// @return @c true when the file was written successfully.
bool writeAssemblyFile(const std::filesystem::path &path,
                       const std::string &text,
                       std::ostream &err) {
    return common::writeTextFile(path, text, err);
}

/// @brief Convert a path to use native separators on the current platform.
/// @details On Windows, forward slashes in paths can confuse cmd.exe when
///          passed through run_process. This helper ensures backslashes are used.
/// @param path Original path to normalize.
/// @return String with platform-native path separators.
std::string toNativePath(const std::filesystem::path &path) {
    std::filesystem::path native = path;
    native.make_preferred();
    return common::pathToUtf8(native);
}

/// @brief Assemble emitted assembly into an object file.
/// @details Invokes the system C compiler with the `-c` flag so the pipeline
///          can stop after producing a relocatable object when no executable is
///          required.
/// @param asmPath Path to the assembly file to assemble.
/// @param objPath Destination path for the object file.
/// @param targetPlatform Platform used to select the assembler target triple.
/// @param out     Stream receiving the assembler's standard output.
/// @param err     Stream receiving the assembler's standard error.
/// @return Normalised assembler exit code (-1 when the command could not start).
int invokeAssembler(const std::filesystem::path &asmPath,
                    const std::filesystem::path &objPath,
                    TargetPlatform targetPlatform,
                    std::ostream &out,
                    std::ostream &err) {
    const std::vector<std::string> ccArgs = systemAssemblerArgs(targetPlatform);
    const int exitCode =
        common::invokeAssembler(ccArgs, toNativePath(asmPath), toNativePath(objPath), out, err);
    if (exitCode != 0) {
        err << "error: assembler driver exited with status " << exitCode << "\n";
    }
    return exitCode;
}

/// @brief Execute a freshly linked binary and capture its output.
/// @details Delegates to the shared executable runner after normalising the
///          path representation for the host platform.
/// @param exePath Path to the executable to run.
/// @param out     Stream receiving program stdout.
/// @param err     Stream receiving program stderr.
/// @return Process exit code (-1 when the process could not be started).
int runExecutable(const std::filesystem::path &exePath, std::ostream &out, std::ostream &err) {
    return common::runExecutable(toNativePath(exePath), out, err);
}

/// @brief Append runtime archive paths required by @p ctx in dependency order.
/// @details Deduplicates by absolute path. On Windows, when the Base component
///          is required, also pulls in Oop/Arrays/Collections/Threads/Text/IoFs
///          because Windows CRT startup expects them. Graphics and Audio
///          support libraries are appended when the corresponding runtime
///          components are present in the link context. The GUI support library
///          also pulls in zanna_text_core because CodeEditor uses the shared
///          text-buffer C ABI.
/// @param ctx Runtime-component and build-directory discovery result.
/// @param windowsDebugRuntime Optional override for the MSVC runtime flavor.
/// @param archives Destination list extended with existing required archives.
void collectNativeLinkArchives(const common::LinkContext &ctx,
                               std::optional<bool> windowsDebugRuntime,
                               std::vector<std::string> &archives) {
    std::unordered_set<std::string> seenArchives;

    /// Append an existing archive once using its lexically normalized path.
    auto appendIfExists = [&](const std::filesystem::path &path) {
        if (!common::fileExists(path))
            return;
        const std::string archivePath = common::pathToUtf8(path.lexically_normal());
        if (seenArchives.insert(archivePath).second)
            archives.push_back(archivePath);
    };

    /// Resolve and append one runtime component archive when it exists.
    auto appendComponent = [&](RtComponent comp) {
        appendIfExists(common::runtimeArchivePath(ctx.buildDir, archiveNameForComponent(comp)));
    };

    for (const auto &entry : ctx.requiredArchives)
        appendIfExists(entry.second);

    // The Windows runtime build does not preserve the Unix weak-link defaults
    // used by zanna_rt_base. Pull in the concrete component archives that
    // satisfy those cross-component references without regressing to
    // "link every runtime archive".
    if constexpr (zanna::platform::kHostWindows) {
        if (common::hasComponent(ctx, RtComponent::Base)) {
            appendComponent(RtComponent::Oop);
            appendComponent(RtComponent::Arrays);
            appendComponent(RtComponent::Collections);
            appendComponent(RtComponent::Threads);
            appendComponent(RtComponent::Text);
            appendComponent(RtComponent::IoFs);
        }
    }

    if (common::hasComponent(ctx, RtComponent::Graphics)) {
        const auto guiLib = common::supportLibraryPath(ctx.buildDir, "zannagui");
        const auto textCoreLib = common::supportLibraryPath(ctx.buildDir, "zanna_text_core");
        const auto gfxLib = common::supportLibraryPath(ctx.buildDir, "zannagfx");
        appendIfExists(guiLib);
        appendIfExists(textCoreLib);
        appendIfExists(gfxLib);
    }

    if (common::hasComponent(ctx, RtComponent::Audio)) {
        const auto audLib = common::supportLibraryPath(ctx.buildDir, "zannaaud");
        appendIfExists(audLib);
    }

    // Embedding either language-service frontend pulls in RuntimeRegistry, which
    // references the entire rt_* catalog regardless of what the program itself
    // uses. Link every runtime component so those references resolve.
    if (ctx.needsZiaFrontend || ctx.needsBasicFrontend) {
        for (int i = 0; i < static_cast<int>(RtComponent::Count); ++i)
            appendComponent(static_cast<RtComponent>(i));
    }
    if (ctx.needsZiaFrontend) {
        for (const auto &lib : common::ziaFrontendClosureLibs())
            appendIfExists(common::supportLibraryPath(ctx.buildDir, lib));
    }
    if (ctx.needsBasicFrontend) {
        for (const auto &lib : common::basicFrontendClosureLibs())
            appendIfExists(common::supportLibraryPath(ctx.buildDir, lib));
    }

    if constexpr (zanna::platform::kHostWindows) {
        const bool useDebugRuntime =
            windowsDebugRuntime.value_or(common::windowsArchivePathsUseDebugRuntime(archives));
        for (const auto &archive :
             common::windowsMsvcCxxRuntimeArchives(ctx.buildDir, "x64", useDebugRuntime)) {
            appendIfExists(archive);
        }
    }
}

/// @brief Link @p objData or @p objPath into @p exePath with the in-process linker.
/// @details Fills NativeLinkerOptions from @p ctx and the caller's flags,
///          collects archive paths via collectNativeLinkArchives, and invokes
///          linker::nativeLink. Errors are written to @p err and surfaced via
///          a non-zero return value.
/// @param objPath Object display name and filesystem fallback when @p objData is absent.
/// @param objData Optional serialized object bytes transferred directly from codegen.
/// @param exePath Destination executable path.
/// @param ctx Prepared runtime archive and frontend dependency context.
/// @param targetPlatform Platform conventions used by the native linker.
/// @param extraObjects Additional relocatable object paths to link.
/// @param stackSize Requested stack reservation, or zero for the linker default.
/// @param fastLink Skip optional size-reduction work when @c true.
/// @param preserveDebugSections Retain emitted debugging sections when @c true.
/// @param windowsDebugRuntime Optional override for the MSVC runtime flavor.
/// @param out Stream receiving informational linker output.
/// @param err Stream receiving linker diagnostics.
/// @return Native linker status; zero indicates success.
int linkObjectWithNativeLinker(const std::filesystem::path &objPath,
                               std::optional<std::vector<uint8_t>> objData,
                               const std::filesystem::path &exePath,
                               const common::LinkContext &ctx,
                               TargetPlatform targetPlatform,
                               const std::vector<std::string> &extraObjects,
                               std::size_t stackSize,
                               bool fastLink,
                               bool preserveDebugSections,
                               std::optional<bool> windowsDebugRuntime,
                               std::ostream &out,
                               std::ostream &err) {
    linker::NativeLinkerOptions linkOpts;
    linkOpts.objPath = common::pathToUtf8(objPath);
    linkOpts.objData = std::move(objData);
    linkOpts.exePath = common::pathToUtf8(exePath);
    linkOpts.entrySymbol = "main";
    linkOpts.platform = targetLinkPlatform(targetPlatform);
    linkOpts.arch = linker::LinkArch::X86_64;
    linkOpts.stackSize = stackSize;
    linkOpts.fastLink = fastLink;
    linkOpts.preserveDebugSections = preserveDebugSections;
    linkOpts.windowsDebugRuntime = windowsDebugRuntime;
    collectNativeLinkArchives(ctx, windowsDebugRuntime, linkOpts.archivePaths);
    if (ctx.needsZiaFrontend) {
        const auto ziaEditorLib = common::supportLibraryPath(ctx.buildDir, "zia_editor_services");
        if (common::fileExists(ziaEditorLib))
            linkOpts.forceLoadArchivePaths.push_back(
                common::pathToUtf8(ziaEditorLib.lexically_normal()));
    }
    if (ctx.needsBasicFrontend) {
        const auto basicLib = common::supportLibraryPath(ctx.buildDir, "fe_basic");
        if (common::fileExists(basicLib))
            linkOpts.forceLoadArchivePaths.push_back(
                common::pathToUtf8(basicLib.lexically_normal()));
    }
    linkOpts.extraObjPaths = extraObjects;
    return linker::nativeLink(linkOpts, out, err);
}

} // namespace

/// @copydoc CodegenPipeline::CodegenPipeline
CodegenPipeline::CodegenPipeline(Options opts) : opts_(std::move(opts)) {}

/// @copydoc CodegenPipeline::run
PipelineResult CodegenPipeline::run() {
    PipelineResult result{0, "", ""};
    std::ostringstream out;
    std::ostringstream err;
    /// Snapshot captured streams into the process-style result at an exit point.
    auto finish = [&]() -> PipelineResult {
        result.stdout_text = out.str();
        result.stderr_text = err.str();
        return result;
    };

    il::core::Module module;
    const auto loadResult = il::tools::common::loadModuleFromFile(opts_.input_il_path, module, err);
    if (!loadResult.succeeded()) {
        result.exit_code = 1;
        return finish();
    }
    if (!il::tools::common::verifyModule(module, err)) {
        result.exit_code = 1;
        return finish();
    }

    return runWithModule(std::move(module), opts_.input_il_path, true);
}

/// @copydoc CodegenPipeline::runWithModule
PipelineResult CodegenPipeline::runWithModule(il::core::Module module,
                                              std::string debugSourcePath,
                                              bool moduleAlreadyVerified) {
    PipelineResult result{0, "", ""};
    std::ostringstream out;
    std::ostringstream err;

    /// Flush accumulated output into the result at any success or failure exit.
    auto finish = [&]() -> PipelineResult {
        result.stdout_text = out.str();
        result.stderr_text = err.str();
        return result;
    };

    if (debugSourcePath.empty())
        debugSourcePath = opts_.input_il_path;

    if (!moduleAlreadyVerified && !il::tools::common::verifyModule(module, err)) {
        result.exit_code = 1;
        return finish();
    }

    zanna::codegen::common::lowerNativeEh(module);
    if (const auto residualEh = zanna::codegen::common::findResidualStructuredEh(module)) {
        err << "error: " << *residualEh << "\n";
        result.exit_code = 1;
        return finish();
    }

    // Run the canonical IL optimization pipeline before lowering to MIR so native
    // backends stay aligned with frontend/VM optimization behavior.
    if (!opts_.skip_il_optimization && opts_.optimize >= 1) {
        il::transform::PassManager ilpm;
        ilpm.enableParallelFunctionPasses(true);
        const bool ok = ilpm.runPipeline(module, opts_.optimize >= 2 ? "O2" : "O1");

        if (!ok) {
            err << "error: failed to run x86-64 IL optimization pipeline\n";
            result.exit_code = 1;
            return finish();
        }
        // Re-verify IL after optimization to catch optimizer bugs early.
        if (!il::tools::common::verifyModule(module, err)) {
            err << "error: IL verification failed after optimization\n";
            result.exit_code = 1;
            return finish();
        }
    }

    passes::Module pipelineModule{};
    pipelineModule.il = std::move(module);

    const bool useNativeAsm = (opts_.assembler_mode == AssemblerMode::Native);
    if (!useNativeAsm && !opts_.asset_blob_path.empty() && opts_.extra_objects.empty()) {
        err << "error: x64 --asset-blob requires --native-asm or a companion --extra-obj\n";
        result.exit_code = 1;
        return finish();
    }

    CodegenOptions codegenOpts{};
    const TargetPlatform targetPlatform = effectiveTargetPlatform(opts_);
    codegenOpts.optimizeLevel = opts_.optimize;
    codegenOpts.targetABI = opts_.target_abi;
    codegenOpts.targetPlatform = targetPlatform;
    codegenOpts.debugSourcePath = debugSourcePath;
    codegenOpts.emitDebugLines = opts_.emit_debug_lines;
    pipelineModule.options = codegenOpts;
    pipelineModule.target = &selectTarget(pipelineModule.options.targetABI);

    passes::Diagnostics diagnostics{};
    passes::PassManager manager{};
    if (opts_.time_passes)
        manager.setTimingStream(&err, "x86_64");
    manager.addPass(std::make_unique<passes::LoweringPass>());
    manager.addPass(std::make_unique<passes::LegalizePass>());
    if (codegenOpts.optimizeLevel >= 1)
        manager.addPass(std::make_unique<passes::PreRegAllocOptPass>());
    manager.addPass(std::make_unique<passes::RegAllocPass>());
    manager.addPass(std::make_unique<passes::SchedulerPass>());
    manager.addPass(std::make_unique<passes::PeepholePass>());

    if (useNativeAsm) {
        manager.addPass(std::make_unique<passes::BinaryEmitPass>(codegenOpts));
    } else {
        manager.addPass(std::make_unique<passes::EmitPass>(codegenOpts));
    }

    if (!manager.run(pipelineModule, diagnostics)) {
        diagnostics.flush(err);
        result.exit_code = 1;
        return finish();
    }

    diagnostics.flush(err);

    // --- Inject asset blob into .rodata (if present) ---
    if (useNativeAsm && pipelineModule.binaryRodata && !opts_.asset_blob_path.empty()) {
        std::vector<uint8_t> assetBlob;
        if (!readBinaryFileChecked(
                zanna::filesystem::pathFromUtf8(opts_.asset_blob_path), assetBlob, err)) {
            result.exit_code = 1;
            return finish();
        }

        const char *blobLabel = "zanna_asset_blob";
        const char *sizeLabel = "zanna_asset_blob_size";
        auto &rodata = *pipelineModule.binaryRodata;
        rodata.alignTo(16);
        rodata.defineSymbol(
            blobLabel, objfile::SymbolBinding::Global, objfile::SymbolSection::Rodata);
        if (!assetBlob.empty()) {
            rodata.emitBytes(assetBlob.data(), assetBlob.size());
        }
        rodata.alignTo(8);
        rodata.defineSymbol(
            sizeLabel, objfile::SymbolBinding::Global, objfile::SymbolSection::Rodata);
        rodata.emit64LE(static_cast<uint64_t>(assetBlob.size()));
    }

    // --- Native assembler path: write .o directly via ObjectFileWriter ---
    if (useNativeAsm) {
        if (!pipelineModule.binaryText && pipelineModule.binaryTextSections.empty()) {
            err << "error: binary emit pass did not produce machine code\n";
            result.exit_code = 1;
            return finish();
        }

        // If user requested assembly text output via -S, we still need text emit.
        // Fall through to the text path by also running text emit.
        if (opts_.emit_asm && !opts_.output_asm_path.empty()) {
            // For -S with --native-asm, we don't produce .s — just stop after .o.
            // Users should use --system-asm if they want assembly text.
            err << "warning: -S is not supported with --native-asm; ignoring -S\n";
        }

        const bool wantsObjectOnly = !opts_.output_obj_path.empty() && !opts_.run_native &&
                                     looksLikeObjectFilePath(opts_.output_obj_path);

        // Derive the intermediate .o path. For a native-exe build, place it next
        // to the (unique) output binary rather than the shared source/IL path —
        // otherwise two concurrent builds of the same source to different -o
        // outputs (the -O0/-O2 struct-return ABI tests) race on the same .o.
        std::filesystem::path objPath;
        if (wantsObjectOnly) {
            objPath = zanna::filesystem::pathFromUtf8(opts_.output_obj_path);
        } else if (!opts_.output_obj_path.empty()) {
            objPath = zanna::filesystem::pathFromUtf8(opts_.output_obj_path);
            objPath.replace_extension(".o");
        } else {
            objPath = zanna::filesystem::pathFromUtf8(opts_.input_il_path);
            objPath.replace_extension(".o");
        }

        // Write the object file using the native writer for x86_64.
        // Must specify ObjArch::X86_64 explicitly — createHostObjectFileWriter()
        // would pick the host arch (e.g. arm64 on Apple Silicon).
        auto writer = objfile::createObjectFileWriter(targetObjectFormat(targetPlatform),
                                                      objfile::ObjArch::X86_64);
        if (!writer) {
            err << "error: no native object file writer for this platform\n";
            result.exit_code = 1;
            return finish();
        }

        // Pass pre-encoded DWARF .debug_line data to the writer.
        const bool hasDebugLine = !pipelineModule.debugLineData.empty();
        if (hasDebugLine && !pipelineModule.binaryText) {
            err << "error: binary emit pass did not produce merged debug machine code\n";
            result.exit_code = 1;
            return finish();
        }
        if (hasDebugLine)
            writer->setDebugLineData(std::move(pipelineModule.debugLineData));
        if (pipelineModule.binaryData && !pipelineModule.binaryData->bytes().empty())
            writer->setDataSection(*pipelineModule.binaryData);

        std::optional<std::vector<uint8_t>> objectData;
        if (!wantsObjectOnly)
            objectData.emplace();
        const bool wroteObject =
            hasDebugLine
                ? (wantsObjectOnly ? writer->write(zanna::filesystem::pathToUtf8(objPath),
                                                   *pipelineModule.binaryText,
                                                   *pipelineModule.binaryRodata,
                                                   err)
                                   : writer->writeToMemory(*objectData,
                                                           *pipelineModule.binaryText,
                                                           *pipelineModule.binaryRodata,
                                                           err))
                : (wantsObjectOnly ? writer->write(zanna::filesystem::pathToUtf8(objPath),
                                                   pipelineModule.binaryTextSections,
                                                   *pipelineModule.binaryRodata,
                                                   err)
                                   : writer->writeToMemory(*objectData,
                                                           pipelineModule.binaryTextSections,
                                                           *pipelineModule.binaryRodata,
                                                           err));
        if (!wroteObject) {
            result.exit_code = 1;
            return finish();
        }

        if (wantsObjectOnly) {
            result.exit_code = 0;
            return finish();
        }

        // Link the emitted object with the native linker, deriving the runtime
        // archive set from the binary symbol table because there is no assembly
        // text on the native-assembler path.
        const std::filesystem::path exePath =
            opts_.output_obj_path.empty() ? deriveExecutablePath(opts_)
                                          : zanna::filesystem::pathFromUtf8(opts_.output_obj_path);

        std::unordered_set<std::string> extSymbols;
        if (!pipelineModule.binaryTextSections.empty()) {
            for (const auto &section : pipelineModule.binaryTextSections) {
                for (const auto &sym : section.symbols()) {
                    if (sym.binding == objfile::SymbolBinding::External)
                        extSymbols.insert(sym.name);
                }
            }
        } else if (pipelineModule.binaryText) {
            for (const auto &sym : pipelineModule.binaryText->symbols()) {
                if (sym.binding == objfile::SymbolBinding::External)
                    extSymbols.insert(sym.name);
            }
        }
        if (pipelineModule.binaryRodata) {
            for (const auto &sym : pipelineModule.binaryRodata->symbols()) {
                if (sym.binding == objfile::SymbolBinding::External)
                    extSymbols.insert(sym.name);
            }
        }
        // Scalar globals are defined intra-object in .data; drop their names from the
        // runtime-archive symbol closure so they are not treated as external imports.
        if (pipelineModule.binaryData) {
            for (const auto &sym : pipelineModule.binaryData->symbols()) {
                if (sym.binding == objfile::SymbolBinding::Global)
                    extSymbols.erase(sym.name);
            }
        }

        common::LinkContext ctx;
        if (const int rc = common::prepareLinkContextFromSymbols(extSymbols, ctx, out, err);
            rc != 0) {
            result.exit_code = 1;
            return finish();
        }

        if (opts_.link_mode == LinkMode::System)
            err << "warning: --system-link is deprecated; using the native linker\n";

        const int linkExit = linkObjectWithNativeLinker(objPath,
                                                        std::move(objectData),
                                                        exePath,
                                                        ctx,
                                                        targetPlatform,
                                                        opts_.extra_objects,
                                                        opts_.stack_size,
                                                        opts_.fast_link,
                                                        opts_.emit_debug_lines,
                                                        opts_.windows_debug_runtime,
                                                        out,
                                                        err);
        if (linkExit != 0) {
            result.exit_code = linkExit == -1 ? 1 : linkExit;
            return finish();
        }

        if (!opts_.run_native) {
            result.exit_code = 0;
            return finish();
        }

        const int runExit = runExecutable(exePath, out, err);
        result.exit_code = (runExit == -1) ? 1 : runExit;
        if (opts_.output_obj_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(exePath, ec);
        }
        return finish();
    }

    // --- System assembler path (existing): text assembly → cc -c → .o ---
    if (!pipelineModule.codegenResult) {
        err << "error: emit pass did not produce assembly output\n";
        result.exit_code = 1;
        return finish();
    }

    const std::string &asmText = pipelineModule.codegenResult->asmText;

    const std::filesystem::path asmPath =
        opts_.output_asm_path.empty() ? deriveAssemblyPath(opts_)
                                      : zanna::filesystem::pathFromUtf8(opts_.output_asm_path);
    if (!writeAssemblyFile(asmPath, asmText, err)) {
        result.exit_code = 1;
        return finish();
    }

    // If user requested assembly output via -S with a specific path, stop here.
    // Don't try to assemble or link - just emit the assembly file.
    if (opts_.emit_asm && !opts_.output_asm_path.empty()) {
        result.exit_code = 0;
        return finish();
    }

    // Check if -o path looks like an executable (ends with .exe or has no extension)
    // vs an object file (ends with .o or .obj).
    const bool wantsObjectOnly = !opts_.output_obj_path.empty() && !opts_.run_native &&
                                 looksLikeObjectFilePath(opts_.output_obj_path);
    if (wantsObjectOnly) {
        const std::filesystem::path objPath =
            zanna::filesystem::pathFromUtf8(opts_.output_obj_path);
        const int assembleExit = invokeAssembler(asmPath, objPath, targetPlatform, out, err);
        if (assembleExit != 0) {
            result.exit_code = assembleExit == -1 ? 1 : assembleExit;
        } else {
            result.exit_code = 0;
            // Clean up intermediate assembly after successful object file creation.
            if (!opts_.emit_asm) {
                std::error_code ec;
                std::filesystem::remove(asmPath, ec);
            }
        }
        return finish();
    }

    // Link to executable if: running native, no output path specified, or output looks like .exe
    const bool needsExecutable = opts_.run_native || opts_.output_obj_path.empty() ||
                                 !looksLikeObjectFilePath(opts_.output_obj_path);
    if (!needsExecutable) {
        result.exit_code = 0;
        return finish();
    }

    const std::filesystem::path exePath =
        opts_.output_obj_path.empty() ? deriveExecutablePath(opts_)
                                      : zanna::filesystem::pathFromUtf8(opts_.output_obj_path);
    std::filesystem::path objPath = zanna::filesystem::pathFromUtf8(opts_.input_il_path);
    objPath.replace_extension(".o");

    const int assembleExit = invokeAssembler(asmPath, objPath, targetPlatform, out, err);
    if (assembleExit != 0) {
        result.exit_code = assembleExit == -1 ? 1 : assembleExit;
        return finish();
    }

    common::LinkContext ctx;
    if (const int rc =
            common::prepareLinkContext(zanna::filesystem::pathToUtf8(asmPath), ctx, out, err);
        rc != 0) {
        result.exit_code = 1;
        return finish();
    }

    if (opts_.link_mode == LinkMode::System)
        err << "warning: --system-link is deprecated; using the native linker\n";

    const int linkExit = linkObjectWithNativeLinker(objPath,
                                                    std::nullopt,
                                                    exePath,
                                                    ctx,
                                                    targetPlatform,
                                                    opts_.extra_objects,
                                                    opts_.stack_size,
                                                    opts_.fast_link,
                                                    opts_.emit_debug_lines,
                                                    opts_.windows_debug_runtime,
                                                    out,
                                                    err);

    {
        std::error_code ec;
        std::filesystem::remove(objPath, ec);
    }
    if (linkExit != 0) {
        result.exit_code = linkExit == -1 ? 1 : linkExit;
        return finish();
    }

    // Clean up the intermediate assembly file after successful linking,
    // unless the user explicitly requested assembly output via -S.
    if (!opts_.emit_asm) {
        std::error_code ec;
        std::filesystem::remove(asmPath, ec);
    }

    if (!opts_.run_native) {
        result.exit_code = 0;
        return finish();
    }

    const int runExit = runExecutable(exePath, out, err);
    if (runExit == -1) {
        result.exit_code = 1;
    } else {
        result.exit_code = runExit;
    }
    if (opts_.output_obj_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(exePath, ec);
    }
    return finish();
}

} // namespace zanna::codegen::x64
