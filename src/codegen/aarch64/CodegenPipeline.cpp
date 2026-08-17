//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/CodegenPipeline.cpp
// Purpose: Implements the modular AArch64 code-generation pipeline by wiring
//          all AArch64 passes through the PassManager in the correct order and
//          providing a reusable end-to-end pipeline for the CLI.
// Key invariants:
//   - Pass order: Lowering → PreRegAllocOpt → Legalize → RegAlloc →
//     BlockLayout → Peephole → Scheduler → Peephole → Emit.
//   - Host/native-link availability, runtime archive composition, and system
//     tool invocation are selected at runtime based on TargetPlatform.
// Cross-platform touchpoints:
//   - Native-link archive discovery and platform-specific linker options are
//     routed through codegen/common/LinkerSupport and NativeLinker.
// Ownership/Lifetime:
//   - CodegenPipeline owns the Options struct; all other objects are
//     stack-local or owned by the caller-supplied AArch64Module.
// Links: src/codegen/aarch64/CodegenPipeline.hpp,
//        src/codegen/aarch64/passes/PassManager.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements the modular AArch64 backend and end-to-end native pipeline.
 *
 * The pass-level entry point mutates caller-owned MIR state in ordered stages.
 * The higher-level driver selects an ABI target, captures diagnostics, emits
 * assembly and/or object bytes, discovers runtime archives, and optionally
 * links or executes the result according to `CodegenPipeline::Options`.
 */

#include "codegen/aarch64/CodegenPipeline.hpp"

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/BinaryEmitPass.hpp"
#include "codegen/aarch64/passes/BlockLayoutPass.hpp"
#include "codegen/aarch64/passes/EmitPass.hpp"
#include "codegen/aarch64/passes/LegalizePass.hpp"
#include "codegen/aarch64/passes/LoweringPass.hpp"
#include "codegen/aarch64/passes/PeepholePass.hpp"
#include "codegen/aarch64/passes/PreRegAllocOptPass.hpp"
#include "codegen/aarch64/passes/RegAllocPass.hpp"
#include "codegen/aarch64/passes/SchedulerPass.hpp"
#include "codegen/common/LinkerSupport.hpp"
#include "codegen/common/NativeEHLowering.hpp"
#include "codegen/common/linker/NativeLinker.hpp"
#include "codegen/common/objfile/ObjectFileWriter.hpp"
#include "common/Filesystem.hpp"
#include "common/PlatformCapabilities.hpp"
#include "common/RunProcess.hpp"
#include "il/transform/PassManager.hpp"
#include "tools/common/module_loader.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zanna::codegen::aarch64 {

namespace {

using zanna::codegen::common::LinkContext;
using TargetPlatform = CodegenPipeline::TargetPlatform;

/// @brief Dump all MIR functions to the provided stream with a header tag.
/// @param module Module whose machine functions are formatted.
/// @param tag Stage label printed in each dump header.
/// @param os Destination diagnostic stream.
static void dumpMir(const passes::AArch64Module &module, const char *tag, std::ostream &os) {
    for (const auto &fn : module.mir) {
        os << "=== MIR " << tag << ": " << fn.name << " ===\n";
        os << toString(fn) << "\n";
    }
}

/// @brief Map a TargetPlatform to the object-file format used by that OS.
/// @param platform Requested target, or `Host` for runtime detection.
/// @return Matching object container format.
static objfile::ObjFormat targetObjectFormat(TargetPlatform platform) {
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

/// @brief Map a TargetPlatform to the linker's LinkPlatform enum.
/// @param platform Requested target, or `Host` for runtime detection.
/// @return Matching native-link platform.
static linker::LinkPlatform targetLinkPlatform(TargetPlatform platform) {
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

/// @brief Return the system assembler command-line prefix for a given target platform.
/// @details On native hosts builds a `cc` invocation, the POSIX-mandated driver
///          name, so the mode works where the default toolchain is GCC;
///          cross-target paths use the Clang `--target=` triple matching the
///          requested ABI, since only Clang assembles for a foreign triple
///          without a separate cross toolchain.
/// @param platform Requested target, or `Host` for host-platform mapping.
/// @return Executable and fixed target arguments, excluding input/output paths.
static std::vector<std::string> systemAssemblerArgs(TargetPlatform platform) {
    switch (platform) {
        case TargetPlatform::Darwin:
            if constexpr (zanna::platform::kHostMacOS) {
                return {"cc", "-arch", "arm64"};
            }
            return {"clang", "--target=arm64-apple-macos11"};
        case TargetPlatform::Linux:
            if constexpr (zanna::platform::kHostLinux) {
                return {"cc"};
            }
            return {"clang", "--target=aarch64-unknown-linux-gnu"};
        case TargetPlatform::Windows:
            return {"clang", "--target=aarch64-pc-windows-msvc"};
        case TargetPlatform::Host:
            return systemAssemblerArgs(
                targetLinkPlatform(TargetPlatform::Host) == linker::LinkPlatform::macOS
                    ? TargetPlatform::Darwin
                : targetLinkPlatform(TargetPlatform::Host) == linker::LinkPlatform::Windows
                    ? TargetPlatform::Windows
                    : TargetPlatform::Linux);
    }
    return {"clang", "--target=aarch64-unknown-linux-gnu"};
}

/// @brief Return true if @p path has a .o or .obj extension (object file output).
/// @param path UTF-8 output path.
/// @return `true` for case-insensitive `.o` or `.obj` extensions.
static bool isObjectOutputPath(const std::string &path) {
    std::string ext =
        zanna::filesystem::pathToUtf8(zanna::filesystem::pathFromUtf8(path).extension());
    for (char &ch : ext) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return ext == ".o" || ext == ".obj";
}

/// @brief Forward declaration; defined later after collectNativeLinkArchives.
static int linkObjToExe(const std::string &objPath,
                        std::optional<std::vector<uint8_t>> objData,
                        const std::string &exePath,
                        const LinkContext &ctx,
                        TargetPlatform targetPlatform,
                        std::size_t stackSize,
                        const std::vector<std::string> &extraObjects,
                        bool fastLink,
                        std::optional<bool> windowsDebugRuntime,
                        bool preserveDebugSections,
                        std::ostream &out,
                        std::ostream &err);

/// @brief Assemble @p asmPath to a temporary .o, then link to @p exePath via
///        the system assembler. Cleans up the temporary object file on return.
/// @param asmPath Input assembly source path.
/// @param exePath Destination executable path.
/// @param targetPlatform Requested target ABI.
/// @param stackSize Requested stack size, or zero for the platform default.
/// @param extraObjects Additional object paths passed to the linker.
/// @param fastLink Whether to skip optional size-reduction work.
/// @param windowsDebugRuntime Optional Windows CRT flavor override.
/// @param preserveDebugSections Whether linked output retains debug sections.
/// @param out Stream receiving normal tool output.
/// @param err Stream receiving diagnostics.
/// @return Zero on success, otherwise the first assembler/linker error code.
static int linkToExe(const std::string &asmPath,
                     const std::string &exePath,
                     TargetPlatform targetPlatform,
                     std::size_t stackSize,
                     const std::vector<std::string> &extraObjects,
                     bool fastLink,
                     std::optional<bool> windowsDebugRuntime,
                     bool preserveDebugSections,
                     std::ostream &out,
                     std::ostream &err) {
    using namespace zanna::codegen::common;

    LinkContext ctx;
    if (const int rc = prepareLinkContext(asmPath, ctx, out, err); rc != 0)
        return rc;

    std::filesystem::path objPath = zanna::filesystem::pathFromUtf8(asmPath);
    objPath.replace_extension(".o");
    const int arc = invokeAssembler(
        systemAssemblerArgs(targetPlatform), asmPath, common::pathToUtf8(objPath), out, err);
    if (arc != 0)
        return arc;

    const int lrc = linkObjToExe(common::pathToUtf8(objPath),
                                 std::nullopt,
                                 exePath,
                                 ctx,
                                 targetPlatform,
                                 stackSize,
                                 extraObjects,
                                 fastLink,
                                 windowsDebugRuntime,
                                 preserveDebugSections,
                                 out,
                                 err);
    std::error_code ec;
    std::filesystem::remove(objPath, ec);
    return lrc;
}

/// @brief Populate @p archives with all runtime archive paths required for linking.
/// @details Deduplicates by absolute path. When the requested target platform is
///          Windows, always pulls in Oop/Arrays/Collections/Threads/Text/IoFs
///          alongside the Base archive because the Windows CRT startup expects
///          them to be present. Graphics and Audio support libraries are
///          appended when the link context declares those components. The GUI
///          support library also pulls in zanna_text_core because CodeEditor
///          uses the shared text-buffer C ABI. Base, Text, and GUI consumers
///          share the separately packaged zanna_regex_engine archive.
/// @param ctx Link context produced by prepareLinkContext/prepareLinkContextFromSymbols.
/// @param targetPlatform Requested OS platform; Host is resolved through targetLinkPlatform().
/// @param windowsDebugRuntime Optional Windows CRT flavor override.
/// @param archives Output list; entries are absolute paths appended in dependency order.
static void collectNativeLinkArchives(const common::LinkContext &ctx,
                                      TargetPlatform targetPlatform,
                                      std::optional<bool> windowsDebugRuntime,
                                      std::vector<std::string> &archives) {
    std::unordered_set<std::string> seenArchives;

    /// @brief Append one existing archive path exactly once.
    /// @param path Candidate archive path.
    auto appendIfExists = [&](const std::filesystem::path &path) {
        if (!common::fileExists(path))
            return;
        const std::string archivePath = common::pathToUtf8(path.lexically_normal());
        if (seenArchives.insert(archivePath).second)
            archives.push_back(archivePath);
    };

    /// @brief Resolve and append one runtime component archive.
    /// @param comp Runtime component whose archive is required.
    auto appendComponent = [&](RtComponent comp) {
        appendIfExists(common::runtimeArchivePath(ctx.buildDir, archiveNameForComponent(comp)));
    };

    for (const auto &entry : ctx.requiredArchives)
        appendIfExists(entry.second);

    if (targetLinkPlatform(targetPlatform) == linker::LinkPlatform::Windows &&
        common::hasComponent(ctx, RtComponent::Base)) {
        appendComponent(RtComponent::Oop);
        appendComponent(RtComponent::Arrays);
        appendComponent(RtComponent::Collections);
        appendComponent(RtComponent::Threads);
        appendComponent(RtComponent::Text);
        appendComponent(RtComponent::IoFs);
    }

    if (common::hasComponent(ctx, RtComponent::Graphics)) {
        appendIfExists(common::supportLibraryPath(ctx.buildDir, "zannagui"));
        appendIfExists(common::supportLibraryPath(ctx.buildDir, "zanna_text_core"));
        appendIfExists(common::supportLibraryPath(ctx.buildDir, "zannagfx"));
    }

    if (common::hasComponent(ctx, RtComponent::Audio))
        appendIfExists(common::supportLibraryPath(ctx.buildDir, "zannaaud"));

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

    if (common::requiresRegexEngineArchive(ctx))
        appendIfExists(common::supportLibraryPath(ctx.buildDir, "zanna_regex_engine"));

    if (targetLinkPlatform(targetPlatform) == linker::LinkPlatform::Windows) {
        const bool useDebugRuntime =
            windowsDebugRuntime.value_or(common::windowsArchivePathsUseDebugRuntime(archives));
        for (const auto &archive :
             common::windowsMsvcCxxRuntimeArchives(ctx.buildDir, "arm64", useDebugRuntime)) {
            appendIfExists(archive);
        }
    }
}

/// @brief Link a single .o file into a native executable using the Zanna native linker.
/// @details Fills a NativeLinkerOptions struct from the link context (runtime archives,
///          extra objects, stack size) and dispatches to nativeLink(). AArch64 arch is always
///          used; platform is mapped from the TargetPlatform enum.
/// @param objPath       Path to the compiled object file.
/// @param objData       Optional serialized object bytes supplied by native codegen.
/// @param exePath       Destination path for the output executable.
/// @param ctx           Link context (runtime archive set, build dir, etc.).
/// @param targetPlatform Target OS; determines symbol mangling and ABI format.
/// @param stackSize     Requested thread stack size in bytes; 0 means platform default.
/// @param extraObjects  Additional .o files to include in the link (e.g. runtime stubs).
/// @param fastLink      Skip non-essential size-reduction passes in the linker.
/// @param windowsDebugRuntime Optional Windows CRT flavor override.
/// @param preserveDebugSections Keep non-alloc DWARF/debug sections in linked output.
/// @param out           Stream for linker stdout diagnostics.
/// @param err           Stream for linker stderr diagnostics.
/// @return 0 on success, non-zero on link failure.
static int linkObjToExe(const std::string &objPath,
                        std::optional<std::vector<uint8_t>> objData,
                        const std::string &exePath,
                        const LinkContext &ctx,
                        TargetPlatform targetPlatform,
                        std::size_t stackSize,
                        const std::vector<std::string> &extraObjects,
                        bool fastLink,
                        std::optional<bool> windowsDebugRuntime,
                        bool preserveDebugSections,
                        std::ostream &out,
                        std::ostream &err) {
    using namespace zanna::codegen::common;
    using zanna::codegen::archiveNameForComponent;
    using zanna::codegen::RtComponent;

    linker::NativeLinkerOptions linkOpts;
    linkOpts.objPath = objPath;
    linkOpts.objData = std::move(objData);
    linkOpts.exePath = exePath;
    linkOpts.platform = targetLinkPlatform(targetPlatform);
    linkOpts.arch = linker::LinkArch::AArch64;
    linkOpts.entrySymbol = "main";
    linkOpts.stackSize = stackSize;
    linkOpts.fastLink = fastLink;
    linkOpts.preserveDebugSections = preserveDebugSections;
    linkOpts.windowsDebugRuntime = windowsDebugRuntime;
    linkOpts.extraObjPaths = extraObjects;
    collectNativeLinkArchives(ctx, targetPlatform, windowsDebugRuntime, linkOpts.archivePaths);
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

    return zanna::codegen::linker::nativeLink(linkOpts, out, err);
}

/// @brief Run IL-level optimization passes on @p mod before machine-code lowering.
/// @details Skips all work when optimizeLevel < 1. At O1 the "O1" preset is used;
///          at O2+ the "O2" preset is used. The IL PassManager applies DCE, inlining,
///          constant folding, and SimplifyCFG in the selected order.
/// @param mod           Module to optimize in place.
/// @param optimizeLevel 0 = no optimization; 1 = O1; 2+ = O2.
/// @return true on success, false if any IL optimizer pass returns an error.
static bool runIlOptimizations(il::core::Module &mod, int optimizeLevel) {
    if (optimizeLevel < 1)
        return true;

    il::transform::PassManager ilpm;
    ilpm.enableParallelFunctionPasses(true);
    return ilpm.runPipeline(mod, optimizeLevel >= 2 ? "O2" : "O1");
}

/// @brief Return the AArch64 TargetInfo for the host OS detected at compile time.
/// @details Dispatches through PlatformCapabilities so ordinary C++ code does
///          not need to duplicate raw host preprocessor probes.
/// @return Reference to the appropriate singleton target; lifetime is static.
static const TargetInfo &hostAArch64Target() {
    if constexpr (zanna::platform::kHostWindows) {
        return windowsTarget();
    } else if constexpr (zanna::platform::kHostMacOS) {
        return darwinTarget();
    }
    return linuxTarget();
}

/// @brief Map a TargetPlatform enum value to the corresponding AArch64 TargetInfo singleton.
/// @param platform Darwin, Linux, Windows, or Host (auto-detected via hostAArch64Target()).
/// @return Const reference to the selected singleton; lifetime is static.
static const TargetInfo &selectAArch64Target(CodegenPipeline::TargetPlatform platform) {
    switch (platform) {
        case CodegenPipeline::TargetPlatform::Darwin:
            return darwinTarget();
        case CodegenPipeline::TargetPlatform::Linux:
            return linuxTarget();
        case CodegenPipeline::TargetPlatform::Windows:
            return windowsTarget();
        case CodegenPipeline::TargetPlatform::Host:
            return hostAArch64Target();
    }
    return hostAArch64Target();
}

} // namespace

/// @brief Run the full AArch64 codegen pipeline: lower → legalize → regalloc → optimize → emit.
/// @details Orchestrates all passes via PassManager in order: IL-to-MIR lowering,
///          pre-RA MIR legalization, register allocation (with optional coalescing),
///          post-RA layout/peephole/scheduling (O1+), and assembly/binary emission.
/// @param module Caller-owned pipeline state mutated by each pass.
/// @param opts Dump, optimization, timing, and emission controls.
/// @param diagOut Destination for pass diagnostics, dumps, and timings.
/// @return `true` when every enabled pass succeeds.
bool runCodegenPipeline(passes::AArch64Module &module,
                        const PipelineOptions &opts,
                        std::ostream &diagOut) {
    passes::Diagnostics diags;
    /// @brief Flush accumulated diagnostics after a pass failure.
    /// @return Always `false` for direct propagation by the caller.
    auto flushOnFailure = [&]() {
        diags.flush(diagOut);
        return false;
    };

    {
        passes::PassManager manager;
        if (opts.timePasses)
            manager.setTimingStream(&diagOut, "aarch64");
        manager.addPass(std::make_unique<passes::LoweringPass>());
        manager.addPass(std::make_unique<passes::LegalizePass>());
        if (opts.optimizeLevel >= 1)
            manager.addPass(std::make_unique<passes::PreRegAllocOptPass>());
        if (!manager.run(module, diags))
            return flushOnFailure();
    }

    if (opts.dumpMirBeforeRA)
        dumpMir(module, "before RA", diagOut);

    {
        passes::PassManager manager;
        if (opts.timePasses)
            manager.setTimingStream(&diagOut, "aarch64");
        manager.addPass(std::make_unique<passes::RegAllocPass>());
        if (!manager.run(module, diags))
            return flushOnFailure();
    }

    if (opts.dumpMirAfterRA)
        dumpMir(module, "after RA", diagOut);

    if (opts.optimizeLevel >= 1) {
        passes::PassManager manager;
        if (opts.timePasses)
            manager.setTimingStream(&diagOut, "aarch64");
        manager.addPass(std::make_unique<passes::BlockLayoutPass>());
        manager.addPass(std::make_unique<passes::PeepholePass>());
        manager.addPass(std::make_unique<passes::SchedulerPass>());
        manager.addPass(std::make_unique<passes::PeepholePass>(
            passes::PeepholePass::Mode::PostScheduleCleanup));
        if (!manager.run(module, diags))
            return flushOnFailure();
    }

    if (opts.dumpMirAfterRA && opts.optimizeLevel >= 1)
        dumpMir(module, "after peephole", diagOut);

    {
        passes::PassManager manager;
        if (opts.timePasses)
            manager.setTimingStream(&diagOut, "aarch64");
        if (opts.emitAssemblyText)
            manager.addPass(std::make_unique<passes::EmitPass>());
        if (opts.useBinaryEmit)
            manager.addPass(std::make_unique<passes::BinaryEmitPass>());
        if (!manager.run(module, diags))
            return flushOnFailure();
    }

    diags.flush(diagOut);
    return true;
}

/// @brief Construct a CodegenPipeline, capturing all options by value.
/// @param opts Full pipeline configuration; moved into opts_ to avoid a copy.
CodegenPipeline::CodegenPipeline(Options opts) : opts_(std::move(opts)) {}

/// @brief Run the pipeline reading the IL module from Options::input_il_path.
/// @details Loads and verifies the module from disk, then delegates to runWithModule()
///          with moduleAlreadyVerified = true to avoid a redundant verification pass.
/// @return PipelineResult with exit_code 0 on success; stderr_text holds any diagnostics.
PipelineResult CodegenPipeline::run() {
    PipelineResult result{};
    std::ostringstream out;
    std::ostringstream err;
    /// @brief Capture buffered output streams in the current pipeline result.
    /// @return Completed result value.
    auto finish = [&]() -> PipelineResult {
        result.stdout_text = out.str();
        result.stderr_text = err.str();
        return result;
    };

    il::core::Module mod;
    const auto load = il::tools::common::loadModuleFromFile(opts_.input_il_path, mod, err);
    if (!load.succeeded()) {
        result.exit_code = 1;
        return finish();
    }
    if (!il::tools::common::verifyModule(mod, err)) {
        result.exit_code = 1;
        return finish();
    }

    return runWithModule(std::move(mod), opts_.input_il_path, true);
}

/// @brief Run the pipeline using an already-loaded IL module.
/// @details Entry point used by both run() and by callers that own the module (e.g. the
///          REPL or test harness). Steps: optional re-verification → EH lowering →
///          IL optimization → target selection → MIR codegen → assembly/object emission →
///          optional native linking → optional execution.
/// @param mod                    IL module to compile; consumed/moved into the pipeline.
/// @param debugSourcePath        Source path embedded in debug line-number directives.
///                               Falls back to opts_.input_il_path when empty.
/// @param moduleAlreadyVerified  When true, skips the IL verifier pass at entry
///                               (saves a redundant O(n) traversal when run() already verified).
/// @return PipelineResult with exit_code 0 on success; stderr_text holds any diagnostics.
PipelineResult CodegenPipeline::runWithModule(il::core::Module mod,
                                              std::string debugSourcePath,
                                              bool moduleAlreadyVerified) {
    PipelineResult result{};
    std::ostringstream out;
    std::ostringstream err;
    /// @brief Capture buffered output streams in the current pipeline result.
    /// @return Completed result value.
    auto finish = [&]() -> PipelineResult {
        result.stdout_text = out.str();
        result.stderr_text = err.str();
        return result;
    };

    if (debugSourcePath.empty())
        debugSourcePath = opts_.input_il_path;

    if (!moduleAlreadyVerified && !il::tools::common::verifyModule(mod, err)) {
        result.exit_code = 1;
        return finish();
    }

    zanna::codegen::common::lowerNativeEh(mod);
    if (const auto residualEh = zanna::codegen::common::findResidualStructuredEh(mod)) {
        err << "error: " << *residualEh << "\n";
        result.exit_code = 1;
        return finish();
    }

    if (!opts_.skip_il_optimization && !runIlOptimizations(mod, opts_.optimize)) {
        err << "error: failed to run AArch64 IL optimization pipeline\n";
        result.exit_code = 1;
        return finish();
    }
    if (!opts_.skip_il_optimization && opts_.optimize >= 1 &&
        !il::tools::common::verifyModule(mod, err)) {
        err << "error: IL verification failed after optimization\n";
        result.exit_code = 1;
        return finish();
    }

    if (opts_.run_native) {
        if (opts_.target_platform != TargetPlatform::Host) {
            err << "error: --run-native requires --target-host on the AArch64 backend\n";
            result.exit_code = 1;
            return finish();
        }
#if !(defined(__aarch64__) || defined(__arm64__))
        err << "error: --run-native is only supported on AArch64 hosts\n";
        result.exit_code = 1;
        return finish();
#endif
    }

    const TargetInfo &ti = selectAArch64Target(opts_.target_platform);

    passes::AArch64Module pipelineModule;
    pipelineModule.ilMod = &mod;
    pipelineModule.ti = &ti;
    pipelineModule.debugSourcePath = debugSourcePath;
    pipelineModule.emitDebugLines = opts_.emit_debug_lines;
    pipelineModule.coalesceTextSections = opts_.fast_link || opts_.optimize == 0;

    PipelineOptions pipeOpts;
    pipeOpts.dumpMirBeforeRA = opts_.dump_mir_before_ra;
    pipeOpts.dumpMirAfterRA = opts_.dump_mir_after_ra;
    pipeOpts.emitAssemblyText = opts_.assembler_mode == AssemblerMode::System || opts_.emit_asm ||
                                (opts_.output_obj_path.empty() && !opts_.run_native);
    pipeOpts.useBinaryEmit = opts_.assembler_mode == AssemblerMode::Native;
    pipeOpts.optimizeLevel = opts_.optimize;
    pipeOpts.timePasses = opts_.time_passes;

    if (!runCodegenPipeline(pipelineModule, pipeOpts, err)) {
        result.exit_code = 1;
        return finish();
    }

    std::string asmText = pipelineModule.assembly;
    std::string asmPath = opts_.output_asm_path;
    if (asmPath.empty()) {
        std::filesystem::path p = zanna::filesystem::pathFromUtf8(opts_.input_il_path);
        p.replace_extension(".s");
        asmPath = common::pathToUtf8(p);
    }

    if (opts_.emit_asm) {
        if (!common::writeTextFile(zanna::filesystem::pathFromUtf8(asmPath), asmText, err)) {
            result.exit_code = 1;
            return finish();
        }
    }

    if (opts_.output_obj_path.empty() && !opts_.run_native) {
        if (!opts_.emit_asm &&
            !common::writeTextFile(zanna::filesystem::pathFromUtf8(asmPath), asmText, err)) {
            result.exit_code = 1;
            return finish();
        }
        return finish();
    }

    // --- Inject asset blob into .rodata (if present) ---
    if (pipelineModule.binaryRodata && !opts_.asset_blob_path.empty()) {
        std::ifstream af(zanna::filesystem::pathFromUtf8(opts_.asset_blob_path),
                         std::ios::binary | std::ios::ate);
        if (!af.is_open()) {
            err << "error: failed to open asset blob '" << opts_.asset_blob_path << "'\n";
            result.exit_code = 1;
            return finish();
        }
        const std::streampos blobSizePos = af.tellg();
        if (blobSizePos < 0) {
            err << "error: failed to determine asset blob size '" << opts_.asset_blob_path << "'\n";
            result.exit_code = 1;
            return finish();
        }
        const auto blobSizeU64 = static_cast<uint64_t>(blobSizePos);
        if (blobSizeU64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            err << "error: asset blob '" << opts_.asset_blob_path
                << "' is too large for this host\n";
            result.exit_code = 1;
            return finish();
        }
        const auto blobSize = static_cast<size_t>(blobSizeU64);
        if (blobSize > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
            err << "error: asset blob '" << opts_.asset_blob_path
                << "' is too large for a single stream read\n";
            result.exit_code = 1;
            return finish();
        }
        std::vector<uint8_t> assetBlob(blobSize);
        af.seekg(0);
        if (!af.good()) {
            err << "error: failed to seek asset blob '" << opts_.asset_blob_path << "'\n";
            result.exit_code = 1;
            return finish();
        }
        if (blobSize != 0) {
            af.read(reinterpret_cast<char *>(assetBlob.data()),
                    static_cast<std::streamsize>(assetBlob.size()));
            if (af.gcount() != static_cast<std::streamsize>(assetBlob.size())) {
                err << "error: failed to read complete asset blob '" << opts_.asset_blob_path
                    << "'\n";
                result.exit_code = 1;
                return finish();
            }
        }

        // AArch64 MachO writer adds _ prefix to global symbols automatically.
        auto &rodata = *pipelineModule.binaryRodata;
        rodata.alignTo(16);
        rodata.defineSymbol(
            "zanna_asset_blob", objfile::SymbolBinding::Global, objfile::SymbolSection::Rodata);
        rodata.emitBytes(assetBlob.data(), assetBlob.size());
        rodata.alignTo(8);
        rodata.defineSymbol("zanna_asset_blob_size",
                            objfile::SymbolBinding::Global,
                            objfile::SymbolSection::Rodata);
        rodata.emit64LE(static_cast<uint64_t>(assetBlob.size()));
    }

    if (opts_.assembler_mode == AssemblerMode::Native && pipelineModule.binaryText) {
        std::filesystem::path objPath;
        bool outputIsObj = false;
        if (!opts_.output_obj_path.empty() && isObjectOutputPath(opts_.output_obj_path)) {
            objPath = zanna::filesystem::pathFromUtf8(opts_.output_obj_path);
            outputIsObj = true;
        } else if (!opts_.output_obj_path.empty()) {
            // Native-exe build: the intermediate object belongs next to the
            // (unique) output binary, not next to the shared source/IL path.
            // Deriving it from input_il_path lets two concurrent builds of the
            // same source to different -o outputs (e.g. the -O0/-O2 struct-return
            // ABI tests) clobber each other's intermediate .o under parallel ctest.
            objPath =
                zanna::filesystem::pathFromUtf8(opts_.output_obj_path).replace_extension(".o");
        } else {
            objPath = zanna::filesystem::pathFromUtf8(opts_.input_il_path).replace_extension(".o");
        }

        using namespace zanna::codegen::objfile;
        auto writer =
            createObjectFileWriter(targetObjectFormat(opts_.target_platform), ObjArch::AArch64);
        if (!writer) {
            err << "error: no native object file writer for this platform\n";
            result.exit_code = 1;
            return finish();
        }
        const bool hasDebugLine = !pipelineModule.debugLineData.empty();
        if (hasDebugLine)
            writer->setDebugLineData(std::move(pipelineModule.debugLineData));
        if (pipelineModule.binaryData && !pipelineModule.binaryData->bytes().empty())
            writer->setDataSection(*pipelineModule.binaryData);
        std::optional<std::vector<uint8_t>> objectData;
        if (!outputIsObj)
            objectData.emplace();
        const bool wroteObject =
            !pipelineModule.binaryTextSections.empty()
                ? (outputIsObj ? writer->write(common::pathToUtf8(objPath),
                                               pipelineModule.binaryTextSections,
                                               *pipelineModule.binaryRodata,
                                               err)
                               : writer->writeToMemory(*objectData,
                                                       pipelineModule.binaryTextSections,
                                                       *pipelineModule.binaryRodata,
                                                       err))
                : (outputIsObj ? writer->write(common::pathToUtf8(objPath),
                                               *pipelineModule.binaryText,
                                               *pipelineModule.binaryRodata,
                                               err)
                               : writer->writeToMemory(*objectData,
                                                       *pipelineModule.binaryText,
                                                       *pipelineModule.binaryRodata,
                                                       err));
        if (!wroteObject) {
            err << "error: failed to write object file '" << common::pathToUtf8(objPath) << "'\n";
            result.exit_code = 1;
            return finish();
        }

        if (outputIsObj) {
            return finish();
        }

        std::unordered_set<std::string> extSymbols;
        for (const auto &section : pipelineModule.binaryTextSections)
            for (const auto &sym : section.symbols()) {
                if (sym.binding == zanna::codegen::objfile::SymbolBinding::External)
                    extSymbols.insert(sym.name);
            }
        if (pipelineModule.binaryRodata) {
            for (const auto &sym : pipelineModule.binaryRodata->symbols()) {
                if (sym.binding == zanna::codegen::objfile::SymbolBinding::External)
                    extSymbols.insert(sym.name);
            }
        }
        // Scalar globals are defined intra-object in __data; the text section names
        // them as undefined externals, but the system linker resolves them locally —
        // so drop them from the runtime-archive symbol closure.
        if (pipelineModule.binaryData) {
            for (const auto &sym : pipelineModule.binaryData->symbols()) {
                if (sym.binding == zanna::codegen::objfile::SymbolBinding::Global)
                    extSymbols.erase(sym.name);
            }
        }

        LinkContext ctx;
        if (const int rc =
                zanna::codegen::common::prepareLinkContextFromSymbols(extSymbols, ctx, out, err);
            rc != 0) {
            result.exit_code = 1;
            return finish();
        }

        std::filesystem::path exe =
            opts_.output_obj_path.empty()
                ? zanna::filesystem::pathFromUtf8(opts_.input_il_path).replace_extension("")
                : zanna::filesystem::pathFromUtf8(opts_.output_obj_path);

        if (opts_.link_mode == LinkMode::System)
            err << "warning: --system-link is deprecated; using the native linker\n";

        const int lrc = linkObjToExe(common::pathToUtf8(objPath),
                                     std::move(objectData),
                                     common::pathToUtf8(exe),
                                     ctx,
                                     opts_.target_platform,
                                     opts_.stack_size,
                                     opts_.extra_objects,
                                     opts_.fast_link,
                                     opts_.windows_debug_runtime,
                                     opts_.emit_debug_lines,
                                     out,
                                     err);

        if (lrc != 0) {
            result.exit_code = 1;
            return finish();
        }

        if (opts_.run_native) {
            const int rc = zanna::codegen::common::runExecutable(common::pathToUtf8(exe), out, err);
            result.exit_code = rc == -1 ? 1 : rc;
            if (opts_.output_obj_path.empty()) {
                std::error_code ec;
                std::filesystem::remove(exe, ec);
            }
        }

        return finish();
    }

    if (!opts_.emit_asm &&
        !common::writeTextFile(zanna::filesystem::pathFromUtf8(asmPath), asmText, err)) {
        result.exit_code = 1;
        return finish();
    }

    if (!opts_.output_obj_path.empty() && !opts_.run_native) {
        const std::string &outPath = opts_.output_obj_path;
        if (isObjectOutputPath(outPath)) {
            const int arc = zanna::codegen::common::invokeAssembler(
                systemAssemblerArgs(opts_.target_platform), asmPath, outPath, out, err);
            result.exit_code = arc == 0 ? 0 : 1;
            return finish();
        }

        if (opts_.link_mode == LinkMode::System)
            err << "warning: --system-link is deprecated; using the native linker\n";
        const int lrc = linkToExe(asmPath,
                                  outPath,
                                  opts_.target_platform,
                                  opts_.stack_size,
                                  opts_.extra_objects,
                                  opts_.fast_link,
                                  opts_.windows_debug_runtime,
                                  opts_.emit_debug_lines,
                                  out,
                                  err);
        if (lrc == 0 && !opts_.emit_asm) {
            std::error_code ec;
            std::filesystem::remove(asmPath, ec);
        }
        result.exit_code = lrc == 0 ? 0 : 1;
        return finish();
    }

    std::filesystem::path exe =
        opts_.output_obj_path.empty()
            ? zanna::filesystem::pathFromUtf8(opts_.input_il_path).replace_extension("")
            : zanna::filesystem::pathFromUtf8(opts_.output_obj_path);

    if (opts_.link_mode == LinkMode::System)
        err << "warning: --system-link is deprecated; using the native linker\n";
    if (linkToExe(asmPath,
                  common::pathToUtf8(exe),
                  opts_.target_platform,
                  opts_.stack_size,
                  opts_.extra_objects,
                  opts_.fast_link,
                  opts_.windows_debug_runtime,
                  opts_.emit_debug_lines,
                  out,
                  err) != 0) {
        result.exit_code = 1;
        return finish();
    }

    if (!opts_.emit_asm) {
        std::error_code ec;
        std::filesystem::remove(asmPath, ec);
    }

    if (opts_.run_native) {
        const int rc = zanna::codegen::common::runExecutable(common::pathToUtf8(exe), out, err);
        result.exit_code = rc == -1 ? 1 : rc;
        if (opts_.output_obj_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(exe, ec);
        }
    }

    return finish();
}

} // namespace zanna::codegen::aarch64
