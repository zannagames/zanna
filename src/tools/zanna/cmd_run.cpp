//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements the `zanna run`, `build`, `build-many`, and `check` commands.
/// @details Provides a frontend-agnostic source-to-IL pipeline for Zia, BASIC,
///          and mixed projects, followed by verification, VM execution, IL
///          emission, or native compilation according to the selected mode.
//
//===----------------------------------------------------------------------===//

#include "cli.hpp"
#include "common/Filesystem.hpp"
#include "frontends/basic/BasicCompiler.hpp"
#include "frontends/zia/Compiler.hpp"
#include "frontends/zia/Warnings.hpp"
#include "il/api/expected_api.hpp"
#include "il/link/InteropThunks.hpp"
#include "il/link/ModuleLinker.hpp"
#include "il/transform/PassManager.hpp"
#include "support/diag_expected.hpp"
#include "support/source_manager.hpp"
#include "tools/common/ScopedProcess.hpp"
#include "tools/common/asset/AssetCompiler.hpp"
#include "tools/common/native_compiler.hpp"
#include "tools/common/packaging/PkgUtils.hpp"
#include "tools/common/project_loader.hpp"
#include "tools/common/source_loader.hpp"
#include "tools/common/vm_executor.hpp"
#include "tools/zanna/DebugAdapter.hpp"
#include "zanna/il/IO.hpp"
#include "zanna/il/Verify.hpp"
#include "zanna/vm/VM.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

using namespace il;
using namespace il::support;
using namespace il::tools::common;

namespace {

/// @brief Terminal operation requested from the shared compile pipeline.
enum class RunMode { Run, Build, Check };

/// @brief Return an ASCII-lowercased copy of @p value.
/// @details Project entry extension checks are command-line syntax, so ASCII folding is enough and
///          avoids locale-sensitive surprises when users write uppercase `.ZIA` or `.BAS` paths.
/// @param value Text to fold in place.
/// @return Lowercase copy of the input using unsigned-character-safe conversion.
std::string lowerAscii(std::string value) {
    /// @brief Fold one byte to lowercase without signed-character undefined behavior.
    /// @param c Byte to normalize.
    /// @return Lowercase representation converted back to `char`.
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

/// @brief Convert a library-side module's `main` into a module initializer.
/// @details A mixed-language project has exactly one program entry, but every BASIC module
///          emits `@main` to carry its top-level statements — module `__mod_init` calls and
///          global initialization included. Linking that directly against the entry module's
///          `@main` is a duplicate-definition error, and simply dropping it would silently
///          skip the library's initialization. Renaming it and wrapping it in a `() -> void`
///          initializer preserves the work and lets the linker run it before the entry's
///          `main`, which is the same ordering a single-language build produces.
/// @param module Library-side module rewritten in place.
/// @param index Library ordinal used to keep synthesized names unique.
void convertLibraryMainToInitializer(il::core::Module &module, size_t index) {
    const std::string suffix = "$mixedlib" + std::to_string(index);
    const std::string bodyName = "__zanna_lib_main" + suffix;
    const std::string initName = "__zanna_lib_init" + suffix;

    il::core::Function *libMain = nullptr;
    for (auto &fn : module.functions) {
        if (fn.name == "main" && fn.linkage != il::core::Linkage::Import) {
            libMain = &fn;
            break;
        }
    }
    if (!libMain)
        return;

    libMain->name = bodyName;
    libMain->linkage = il::core::Linkage::Internal;

    // Wrap rather than rewrite: the body keeps its own return type and terminators.
    il::core::Function initializer;
    initializer.name = initName;
    initializer.retType = il::core::Type(il::core::Type::Kind::Void);
    initializer.linkage = il::core::Linkage::Internal;
    initializer.moduleInitializer = true;

    il::core::BasicBlock entry;
    entry.label = "entry";

    il::core::Instr call;
    call.op = il::core::Opcode::Call;
    call.type = libMain->retType;
    call.setDirectCallee(bodyName);
    if (libMain->retType.kind != il::core::Type::Kind::Void)
        call.result = 0;
    entry.instructions.push_back(std::move(call));

    il::core::Instr ret;
    ret.op = il::core::Opcode::Ret;
    ret.type = il::core::Type(il::core::Type::Kind::Void);
    entry.instructions.push_back(std::move(ret));

    initializer.blocks.push_back(std::move(entry));
    if (libMain->retType.kind != il::core::Type::Kind::Void)
        initializer.valueNames.assign(1, "discarded");
    module.functions.push_back(std::move(initializer));
}

/// @brief Pick a deterministic entry source for the non-entry side of a mixed project.
/// @details Mixed-language manifests have one true executable entry. The other language may still
///          need a file to seed frontend compilation. This helper keeps the historical behavior of
///          compiling that side while avoiding dependence on insertion order by sorting and then
///          preferring conventional `main` filenames.
/// @param files Candidate source paths for the library-side language.
/// @param lang Language whose conventional entry filename should be preferred.
/// @return Deterministic preferred path, or an empty string when @p files is empty.
std::string selectMixedLibraryEntry(std::vector<std::string> files, ProjectLang lang) {
    if (files.empty())
        return {};
    std::sort(files.begin(), files.end());
    const char *mainName = lang == ProjectLang::Zia ? "main.zia" : "main.bas";
    /// @brief Match one candidate path to the language's conventional main filename.
    /// @param path Candidate source path.
    /// @return `true` when its leaf name equals `mainName`.
    const auto it = std::find_if(files.begin(), files.end(), [&](const std::string &path) {
        return zanna::filesystem::pathFromUtf8(path).filename() == mainName;
    });
    return it != files.end() ? *it : files.front();
}

/// @brief Removes a temporary file on scope exit unless no path was assigned.
/// @details Native builds create temporary asset blobs for linker input. This guard ensures those
/// blobs are deleted on success and on every early-return failure path.
class ScopedTempPath {
  public:
    /// @brief Construct an empty guard that owns no temporary path.
    ScopedTempPath() = default;

    /// @brief Construct a guard that owns @p path.
    /// @param path UTF-8 temporary path to remove at scope exit.
    explicit ScopedTempPath(std::string path) : path_(std::move(path)) {}

    /// @brief Remove the guarded temporary file, ignoring cleanup errors.
    ~ScopedTempPath() {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(zanna::filesystem::pathFromUtf8(path_), ec);
        }
    }

    ScopedTempPath(const ScopedTempPath &) = delete;
    ScopedTempPath &operator=(const ScopedTempPath &) = delete;

    /// @brief Replace the guarded path, deleting the previous temp file first if one existed.
    /// @param path UTF-8 temporary path that becomes owned by the guard.
    void reset(std::string path) {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(zanna::filesystem::pathFromUtf8(path_), ec);
        }
        path_ = std::move(path);
    }

    /// @brief Return the guarded temp path as a string.
    /// @return Reference to the owned UTF-8 path, possibly empty.
    const std::string &path() const {
        return path_;
    }

  private:
    std::string path_;
};

/// @brief Run the IL verifier on @p module and print any diagnostics.
/// @details Collects up to 50 diagnostics; prints them (errors always, warnings
///          only when @p showWarnings) using the requested format.
/// @param module IL module to verify.
/// @param err Stream that receives diagnostics.
/// @param sm Source manager used to render diagnostic locations.
/// @param format Text or structured diagnostic output format.
/// @param showWarnings Whether verifier warnings should be printed.
/// @return true when the module has no verifier errors.
bool reportVerifierDiagnostics(il::core::Module &module,
                               std::ostream &err,
                               il::support::SourceManager &sm,
                               ilc::DiagnosticFormat format,
                               bool showWarnings) {
    il::support::DiagnosticEngine diagnostics;
    for (auto diag : il::verify::Verifier::verifyAll(module, 50))
        diagnostics.report(std::move(diag));
    if (diagnostics.errorCount() != 0 || (showWarnings && diagnostics.warningCount() != 0))
        ilc::printDiagnosticEngine(diagnostics, err, &sm, format);
    return diagnostics.errorCount() == 0;
}

/// @brief Parsed configuration shared by the `run` and `build` subcommands.
/// @details CLI override fields (optimize level, build profile, arch, link mode,
///          Windows runtime) take precedence over the project manifest when set.
struct RunBuildConfig {
    RunMode mode{RunMode::Run};           ///< Whether this is a run or build invocation.
    std::string target{"."};              ///< Target file/dir/manifest (default: cwd).
    std::string outputPath;               ///< Output path for build (-o), empty for run.
    ilc::SharedCliOptions shared;         ///< Shared CLI settings (trace, dumps, etc.).
    std::vector<std::string> programArgs; ///< Args forwarded to the program after '--'.
    bool debugVm{false};                  ///< Use the standard VM for debugging (run only).
    bool debugAdapter{false};             ///< Run as an interactive debug adapter (run only).
    bool helpRequested{false};            ///< True when help was requested.
    bool noRuntimeNamespaces{false};      ///< Disable runtime namespace binding.

    // CLI overrides (take precedence over manifest)
    std::optional<std::string> optimizeLevelOverride;     ///< -O0/-O1/-O2 override.
    std::optional<std::string> buildProfileOverride;      ///< --build-profile override.
    std::optional<zanna::tools::TargetArch> archOverride; ///< --arch override.
    std::optional<bool> fastLinkOverride;                 ///< --fast-link/--no-fast-link.
    std::optional<bool> windowsDebugRuntimeOverride;      ///< Windows debug/release runtime.
    std::optional<std::size_t> stackSizeOverride;         ///< --stack-size (bytes) for the exe.
    bool stripSymbols = false; ///< --strip-symbols: omit local symbols from the exe.
};

/// @brief Print usage for the `zanna run`, `zanna build`, or `zanna check` subcommand.
/// @param mode Command mode whose accepted options are described.
/// @param out Stream that receives the help text.
void printRunBuildUsage(RunMode mode, std::ostream &out = std::cerr) {
    if (mode == RunMode::Check) {
        out << "Usage: zanna check [target] [options]\n"
            << "\n"
            << "Type-check and verify a .zia file, .bas file, project directory, or\n"
            << "zanna.project without running or emitting anything.\n"
            << "\n"
            << "Check options:\n"
            << "  --diagnostic-format text|json Diagnostic output format (stderr)\n"
            << "  --dump-tokens|--dump-ast      Print frontend debug dumps\n"
            << "  --dump-sema-ast|--dump-il     Print semantic AST or lowered IL\n"
            << "  --time-compile                Print phase timing information\n"
            << "  --pass-stats                  Print optimizer pass statistics\n"
            << "  -Wall|-Werror|-Wno-NAME       Control warning handling\n"
            << "  --no-strict-diagnostics       Keep safety warnings as warnings\n"
            << "  --quiet-warnings              Suppress warning output\n"
            << "  -h, --help                    Show this help\n"
            << "\n"
            << "Exit codes:\n"
            << "  0  no errors (warnings allowed)\n"
            << "  1  usage error or target could not be resolved\n"
            << "  2  compile or verification errors\n";
        return;
    }
    if (mode == RunMode::Run) {
        out << "Usage: zanna run [target] [options] [-- program-args...]\n"
            << "\n"
            << "Run a .zia file, .bas file, .il file, project directory, or zanna.project.\n"
            << "\n"
            << "Run options:\n"
            << "  --debug-vm                    Use the standard VM for debugging\n"
            << "  --debug-adapter               Serve the JSON debug adapter protocol\n"
            << "  --stdin-from FILE             Redirect stdin from file\n"
            << "  --max-steps N                 Limit VM execution steps\n"
            << "  --dump-trap                   Show detailed trap diagnostics\n"
            << "  --trace[=il|src]              Enable execution tracing\n"
            << "  --profile                     Print execution profile data\n"
            << "  --diagnostic-format text|json Diagnostic output format (stderr)\n"
            << "  --bounds-checks               Enable generated bounds checks\n"
            << "  --no-bounds-checks            Disable generated bounds checks\n"
            << "  --dump-tokens|--dump-ast      Print frontend debug dumps\n"
            << "  --dump-sema-ast|--dump-il     Print semantic AST or lowered IL\n"
            << "  --dump-il-opt|--dump-il-passes Print optimizer debug dumps\n"
            << "  --time-compile                Print phase timing information\n"
            << "  --pass-stats                  Print optimizer pass statistics\n"
            << "  --build-profile debug|balanced|release\n"
            << "  -O0|-O1|-O2                   Override optimization level\n"
            << "  -h, --help                    Show this help\n";
        return;
    }

    out << "Usage: zanna build [target] [-o output] [options]\n"
        << "\n"
        << "Build IL or a native binary from a .zia file, .bas file, project directory, or "
           "zanna.project.\n"
        << "\n"
        << "Build options:\n"
        << "  -o PATH                       Output .il or native binary path\n"
        << "  --arch arm64|x64              Override native target architecture\n"
        << "  --fast-link | --no-fast-link  Override linker mode\n"
        << "  --strip-symbols               Omit function/data names from the executable "
           "symbol table\n"
        << "  --stack-size BYTES            Set the native executable stack size "
           "(decimal or 0x hex)\n"
        << "  --windows-debug-runtime       Link Windows debug runtime\n"
        << "  --windows-release-runtime     Link Windows release runtime\n"
        << "  --diagnostic-format text|json Diagnostic output format (stderr)\n"
        << "  --build-profile debug|balanced|release\n"
        << "  -O0|-O1|-O2                   Override optimization level\n"
        << "  --bounds-checks               Enable generated bounds checks\n"
        << "  --no-bounds-checks            Disable generated bounds checks\n"
        << "  --dump-tokens|--dump-ast      Print frontend debug dumps\n"
        << "  --dump-sema-ast|--dump-il     Print semantic AST or lowered IL\n"
        << "  --dump-il-opt|--dump-il-passes Print optimizer debug dumps\n"
        << "  --time-compile                Print phase timing information\n"
        << "  --pass-stats                  Print optimizer pass statistics\n"
        << "  -Wall|-Werror|-Wno-NAME       Control warning handling\n"
        << "  -h, --help                    Show this help\n";
}

/// @brief A compiled project module plus whether it has already been verified.
struct CompiledProjectModule {
    il::core::Module module; ///< The lowered IL module.
    bool verified{false};    ///< True if the module already passed verification.
    /// @brief Class-layout sidecar for the VM debugger (ADR 0138). Populated
    ///        only for pure-Zia debug-adapter runs; empty otherwise.
    il::vm::DebugClassLayoutTable debugLayouts{};
};

/// @brief Convert the frontend's debug-layout export into the VM's table.
/// @details The tool is the composition root: the frontend and the VM each own
///          their plain-data shape (no cross-layer include), and this is the
///          one place both are visible (ADR 0138).
/// @param exported Frontend-owned class-layout records, consumed by the conversion.
/// @return Equivalent VM debugger layout table keyed by class identifier.
il::vm::DebugClassLayoutTable toVmDebugLayouts(
    il::frontends::zia::DebugClassLayoutExport &&exported) {
    using FrontStore = il::frontends::zia::DebugFieldStore;
    using VmStore = il::vm::DebugFieldStorage;
    /// @brief Map frontend field-storage metadata to the VM debugger representation.
    /// @param s Frontend storage classification.
    /// @return Corresponding VM storage classification.
    auto mapStore = [](FrontStore s) {
        switch (s) {
            case FrontStore::I64:
                return VmStore::I64;
            case FrontStore::I32:
                return VmStore::I32;
            case FrontStore::I16:
                return VmStore::I16;
            case FrontStore::I1:
                return VmStore::I1;
            case FrontStore::F64:
                return VmStore::F64;
            case FrontStore::Str:
                return VmStore::Str;
            case FrontStore::Managed:
                return VmStore::Managed;
            case FrontStore::Weak:
                return VmStore::Weak;
            case FrontStore::Raw:
                return VmStore::Raw;
            case FrontStore::Opaque:
            default:
                return VmStore::Opaque;
        }
    };
    il::vm::DebugClassLayoutTable table;
    table.reserve(exported.size());
    for (auto &[classId, cls] : exported) {
        il::vm::DebugClassLayout layout;
        layout.qname = std::move(cls.qname);
        layout.fields.reserve(cls.fields.size());
        for (auto &field : cls.fields) {
            il::vm::DebugFieldLayout vf;
            vf.name = std::move(field.name);
            vf.typeName = std::move(field.typeName);
            vf.offset = field.offset;
            vf.storage = mapStore(field.store);
            vf.boolDisplay = field.boolDisplay;
            layout.fields.push_back(std::move(vf));
        }
        table.emplace(classId, std::move(layout));
    }
    return table;
}

/// @brief Map a build profile name to its default optimization level string.
/// @param profile Build profile spelling to translate.
/// @return "O0"/"O1"/"O2" for debug/balanced/release, or nullopt if unrecognized.
std::optional<std::string> optimizeForBuildProfile(std::string_view profile) {
    if (profile == "debug")
        return std::string("O0");
    if (profile == "balanced")
        return std::string("O1");
    if (profile == "release")
        return std::string("O2");
    return std::nullopt;
}

/// @brief Map an optimization level string to its numeric value.
/// @param level Optimization spelling such as `O0`.
/// @return 0/1/2 for "O0"/"O1"/"O2", or nullopt if unrecognized.
std::optional<int> optimizeLevelNumber(std::string_view level) {
    if (level == "O0")
        return 0;
    if (level == "O1")
        return 1;
    if (level == "O2")
        return 2;
    return std::nullopt;
}

/// @brief Print elapsed time for a compile @p phase when --time-compile is set.
/// @param shared Shared options (checked for the timeCompile flag).
/// @param phase Human-readable phase label.
/// @param start Phase start timestamp; elapsed is measured against now.
void printCompileTime(const ilc::SharedCliOptions &shared,
                      std::string_view phase,
                      std::chrono::steady_clock::time_point start) {
    if (!shared.timeCompile)
        return;
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    std::cerr << "[time-compile] " << phase << " " << elapsed.count() << "ms\n";
}

/// @brief Decide whether function-level optimizer passes may run in parallel.
/// @details Parallelism is disabled when any per-pass verification or dump option
///          is active, since those require deterministic, observable ordering.
/// @param shared Shared CLI options controlling verification and debug dumps.
/// @return true when no ordering-sensitive option prevents parallel passes.
bool shouldEnableParallelFunctionPasses(const ilc::SharedCliOptions &shared) {
    return !shared.verifyEachPass && !shared.dumpILPasses && !shared.dumpIL && !shared.dumpILOpt &&
           !shared.dumpAst && !shared.dumpSemaAst && !shared.dumpTokens;
}

/// @brief Create a user-facing command diagnostic without a source location.
/// @param message Diagnostic message to display.
/// @param code Optional machine-readable diagnostic code.
/// @return Error-severity diagnostic suitable for text or JSON emission.
il::support::Diagnostic makeCommandError(std::string message, std::string code = {}) {
    return il::support::Diagnostic{
        il::support::Severity::Error, std::move(message), {}, std::move(code)};
}

/// @brief Print a command diagnostic using the user-selected diagnostic format.
/// @param diag Diagnostic to print.
/// @param sm Source manager used for location rendering when present.
/// @param format Output format requested by the user.
void printCommandDiagnostic(const il::support::Diagnostic &diag,
                            const il::support::SourceManager *sm,
                            ilc::DiagnosticFormat format) {
    ilc::printDiagnostic(diag, std::cerr, sm, format);
}

/// @brief Validate shared options whose effects only make sense while running code.
/// @details The shared parser intentionally accepts these options for multiple
///          subcommands. `build` and `check` reject them here so flags like
///          `--stdin-from`, `--trace`, and `--profile` are not silently ignored.
/// @param mode Active command mode.
/// @param arg Current argument token.
/// @return A diagnostic message when @p arg is not valid for @p mode.
std::optional<std::string> executionOnlySharedOptionError(RunMode mode, std::string_view arg) {
    if (mode == RunMode::Run)
        return std::nullopt;
    const auto command = mode == RunMode::Build ? "'build'" : "'check'";
    /// @brief Build the mode-specific diagnostic for an execution-only option.
    /// @param option Option spelling to mention.
    /// @return Human-readable validation error.
    const auto errorFor = [&](std::string_view option) {
        return std::string(option) + " is only valid with 'run', not " + command;
    };
    if (arg == "--stdin-from" || arg.substr(0, 13) == "--stdin-from=")
        return errorFor("--stdin-from");
    if (arg == "--max-steps" || arg.substr(0, 12) == "--max-steps=")
        return errorFor("--max-steps");
    if (arg == "--dump-trap")
        return errorFor("--dump-trap");
    if (arg == "--trace" || arg.substr(0, 8) == "--trace=")
        return errorFor("--trace");
    if (arg == "--profile")
        return errorFor("--profile");
    return std::nullopt;
}

/// @brief Parse a native target architecture string.
/// @param value User-supplied architecture name.
/// @return Target architecture, or nullopt for an unsupported name.
std::optional<zanna::tools::TargetArch> parseTargetArch(std::string_view value) {
    if (value == "arm64")
        return zanna::tools::TargetArch::ARM64;
    if (value == "x64")
        return zanna::tools::TargetArch::X64;
    return std::nullopt;
}

/// @brief Parse the arguments for `zanna run`/`zanna build` into a RunBuildConfig.
/// @details Recognises the target, output path, shared options, optimization/profile
///          and architecture/link overrides, and program arguments after `--`.
/// @param mode Whether parsing a run or build invocation (affects accepted flags).
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return The parsed config, or a diagnostic on malformed arguments.
il::support::Expected<RunBuildConfig> parseRunBuildArgs(RunMode mode, int argc, char **argv) {
    RunBuildConfig config;
    config.mode = mode;

    bool hasTarget = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--") {
            if (mode != RunMode::Run) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "program arguments after -- are only valid with 'run'",
                                            {},
                                            {}});
            }
            for (int j = i + 1; j < argc; ++j)
                config.programArgs.emplace_back(argv[j]);
            break;
        } else if (arg == "--help" || arg == "-h") {
            config.helpRequested = true;
            return il::support::Expected<RunBuildConfig>(std::move(config));
        } else if (arg == "-o") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "-o is only valid with 'build'", {}, {}});
            }
            if (i + 1 >= argc) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "missing output path after -o", {}, {}});
            }
            config.outputPath = argv[++i];
            if (config.outputPath.empty()) {
                return il::support::Expected<RunBuildConfig>(
                    makeCommandError("missing output path after -o"));
            }
        } else if (arg.substr(0, 3) == "-o=") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(
                    makeCommandError("-o is only valid with 'build'"));
            }
            config.outputPath = std::string(arg.substr(3));
            if (config.outputPath.empty()) {
                return il::support::Expected<RunBuildConfig>(
                    makeCommandError("missing output path after -o"));
            }
        } else if (arg == "-O0" || arg == "-O1" || arg == "-O2") {
            config.optimizeLevelOverride = std::string(arg.substr(1));
        } else if (arg == "--build-profile") {
            if (i + 1 >= argc) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--build-profile requires debug, balanced, or release",
                                            {},
                                            {}});
            }
            std::string_view value = argv[++i];
            if (!optimizeForBuildProfile(value)) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error,
                    "--build-profile must be 'debug', 'balanced', or 'release'",
                    {},
                    {}});
            }
            config.buildProfileOverride = std::string(value);
        } else if (arg.substr(0, 16) == "--build-profile=") {
            std::string_view value = arg.substr(16);
            if (!optimizeForBuildProfile(value)) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error,
                    "--build-profile must be 'debug', 'balanced', or 'release'",
                    {},
                    {}});
            }
            config.buildProfileOverride = std::string(value);
        } else if (arg == "--debug-vm") {
            if (mode != RunMode::Run) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "--debug-vm is only valid with 'run'", {}, {}});
            }
            if (config.debugAdapter) {
                return il::support::Expected<RunBuildConfig>(
                    makeCommandError("--debug-vm cannot be combined with --debug-adapter"));
            }
            config.debugVm = true;
        } else if (arg == "--debug-adapter") {
            if (mode != RunMode::Run) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--debug-adapter is only valid with 'run'",
                                            {},
                                            {}});
            }
            if (config.debugVm) {
                return il::support::Expected<RunBuildConfig>(
                    makeCommandError("--debug-adapter cannot be combined with --debug-vm"));
            }
            config.debugAdapter = true;
            // Debug unoptimized code so every source line and local survives for
            // breakpoints and variable inspection (unless the user overrode -O).
            if (!config.optimizeLevelOverride)
                config.optimizeLevelOverride = std::string("O0");
        } else if (arg == "--fast-link") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--fast-link is only valid with 'build'",
                                            {},
                                            {}});
            }
            config.fastLinkOverride = true;
        } else if (arg == "--no-fast-link") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--no-fast-link is only valid with 'build'",
                                            {},
                                            {}});
            }
            config.fastLinkOverride = false;
        } else if (arg == "--strip-symbols") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--strip-symbols is only valid with 'build'",
                                            {},
                                            {}});
            }
            config.stripSymbols = true;
        } else if (arg == "--stack-size") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--stack-size is only valid with 'build'",
                                            {},
                                            {}});
            }
            if (i + 1 >= argc) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "--stack-size requires a byte count", {}, {}});
            }
            const std::string val = argv[++i];
            char *end = nullptr;
            const unsigned long long parsed = std::strtoull(val.c_str(), &end, 0);
            if (end == val.c_str() || (end != nullptr && *end != '\0')) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--stack-size must be a decimal or 0x-hex byte count",
                                            {},
                                            {}});
            }
            config.stackSizeOverride = static_cast<std::size_t>(parsed);
        } else if (arg == "--windows-debug-runtime") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--windows-debug-runtime is only valid with 'build'",
                                            {},
                                            {}});
            }
            if (config.windowsDebugRuntimeOverride == false) {
                return il::support::Expected<RunBuildConfig>(
                    makeCommandError("--windows-debug-runtime cannot be combined with "
                                     "--windows-release-runtime"));
            }
            config.windowsDebugRuntimeOverride = true;
        } else if (arg == "--windows-release-runtime") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(
                    il::support::Diagnostic{il::support::Severity::Error,
                                            "--windows-release-runtime is only valid with 'build'",
                                            {},
                                            {}});
            }
            if (config.windowsDebugRuntimeOverride == true) {
                return il::support::Expected<RunBuildConfig>(
                    makeCommandError("--windows-release-runtime cannot be combined with "
                                     "--windows-debug-runtime"));
            }
            config.windowsDebugRuntimeOverride = false;
        } else if (arg == "--no-runtime-namespaces") {
            config.noRuntimeNamespaces = true;
        } else if (arg == "--arch") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "--arch is only valid with 'build'", {}, {}});
            }
            if (i + 1 >= argc) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "--arch requires arm64 or x64", {}, {}});
            }
            std::string_view val = argv[++i];
            if (auto arch = parseTargetArch(val)) {
                config.archOverride = *arch;
            } else {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "--arch must be 'arm64' or 'x64'", {}, {}});
            }
        } else if (arg.substr(0, 7) == "--arch=") {
            if (mode != RunMode::Build) {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "--arch is only valid with 'build'", {}, {}});
            }
            std::string_view val = arg.substr(7);
            if (auto arch = parseTargetArch(val)) {
                config.archOverride = *arch;
            } else {
                return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                    il::support::Severity::Error, "--arch must be 'arm64' or 'x64'", {}, {}});
            }
        } else {
            if (auto error = executionOnlySharedOptionError(mode, arg)) {
                return il::support::Expected<RunBuildConfig>(makeCommandError(std::move(*error)));
            }
            switch (ilc::parseSharedOption(i, argc, argv, config.shared)) {
                case ilc::SharedOptionParseResult::Parsed:
                    continue;
                case ilc::SharedOptionParseResult::Error:
                    return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                        il::support::Severity::Error, ilc::lastSharedOptionError(), {}, {}});
                case ilc::SharedOptionParseResult::NotMatched:
                    if (!arg.empty() && arg[0] != '-' && !hasTarget) {
                        config.target = std::string(arg);
                        hasTarget = true;
                    } else {
                        return il::support::Expected<RunBuildConfig>(il::support::Diagnostic{
                            il::support::Severity::Error,
                            std::string("unknown flag: ") + std::string(arg),
                            {},
                            {}});
                    }
                    break;
            }
        }
    }

    return il::support::Expected<RunBuildConfig>(std::move(config));
}

/// @brief Verify and execute an IL module using the selected runtime path.
/// @details Performs final verification when needed, applies stdin redirection,
///          then dispatches to the debug adapter, tracing VM, profiling VM, or
///          bytecode executor while preserving program arguments and step limits.
/// @param module IL module to verify and execute.
/// @param shared Shared execution, tracing, profiling, and diagnostic options.
/// @param programArgs Arguments exposed to the executed program.
/// @param debugVm Whether to force the standard VM debug execution path.
/// @param debugAdapter Whether to serve the interactive debug-adapter protocol.
/// @param moduleAlreadyVerified Whether final verifier work may be skipped.
/// @param sm Source manager used for verifier and trap locations.
/// @param debugLayouts Optional class-layout metadata consumed by the debug adapter.
/// @return Program exit status, or one for verification, redirection, or trap failure.
int verifyAndExecute(il::core::Module &module,
                     const ilc::SharedCliOptions &shared,
                     const std::vector<std::string> &programArgs,
                     bool debugVm,
                     bool debugAdapter,
                     bool moduleAlreadyVerified,
                     il::support::SourceManager &sm,
                     il::vm::DebugClassLayoutTable debugLayouts = {}) {
    if (!moduleAlreadyVerified &&
        !reportVerifierDiagnostics(
            module, std::cerr, sm, shared.diagnosticFormat, shared.showWarnings)) {
        return 1;
    }

    std::optional<zanna::tools::ScopedStdinRedirect> stdinRedirect;
    if (!shared.stdinPath.empty()) {
        stdinRedirect.emplace(shared.stdinPath);
        if (!stdinRedirect->ok()) {
            printCommandDiagnostic(
                makeCommandError("unable to open stdin file: " + stdinRedirect->errorMessage()),
                &sm,
                shared.diagnosticFormat);
            return 1;
        }
    }

    if (debugAdapter)
        return il::tools::debug::runDebugAdapter(
            module, programArgs, shared.maxSteps, sm, std::move(debugLayouts));

    bool useStandardVm = debugVm || shared.trace.enabled();

    if (useStandardVm || shared.profile) {
        vm::TraceConfig traceCfg = shared.trace;
        traceCfg.sm = &sm;

        vm::RunConfig runCfg;
        runCfg.trace = traceCfg;
        runCfg.maxSteps = shared.maxSteps;
        runCfg.programArgs = programArgs;

        vm::Runner runner(module, std::move(runCfg));

        std::chrono::steady_clock::time_point startTime;
        if (shared.profile)
            startTime = std::chrono::steady_clock::now();

        const int64_t runResult = runner.run();
        int rc = 0;
        const auto intMin = static_cast<int64_t>(std::numeric_limits<int>::min());
        const auto intMax = static_cast<int64_t>(std::numeric_limits<int>::max());
        if (runResult < intMin || runResult > intMax) {
            std::cerr << "program return value " << runResult << " outside host int range ["
                      << intMin << ", " << intMax << "]\n";
            rc = 1;
        } else {
            rc = static_cast<int>(runResult);
        }

        std::chrono::steady_clock::time_point endTime;
        if (shared.profile)
            endTime = std::chrono::steady_clock::now();

        const auto trapMessage = runner.lastTrapMessage();
        if (trapMessage) {
            if (!trapMessage->empty()) {
                std::cerr << *trapMessage;
                if (trapMessage->back() != '\n')
                    std::cerr << '\n';
            }
            if (rc == 0)
                rc = 1;
        }

        if (shared.profile) {
            double ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
            std::cerr << "[SUMMARY] instr=" << runner.instructionCount() << " time_ms=" << ms
                      << "\n";
        }

        return rc;
    }

    il::tools::common::VMExecutorConfig vmConfig;
    vmConfig.programArgs = programArgs;
    vmConfig.outputTrapMessage = true;
    vmConfig.flushStdout = true;
    vmConfig.sourceManager = &sm;
    vmConfig.maxSteps = shared.maxSteps;

    auto vmResult = il::tools::common::executeBytecodeVM(module, vmConfig);
    return vmResult.exitCode;
}

/// @brief Compile a Zia project and return its lowered IL module.
/// @param project Resolved Zia project configuration.
/// @param shared Shared compiler, diagnostic, dump, and warning options.
/// @param sm Source manager populated during compilation.
/// @param optimizeModule Whether the frontend should run the requested optimizer.
/// @param captureDebugLayouts Whether debugger class-layout metadata is required.
/// @return Compiled module and verification/debug-layout state, or a diagnostic.
il::support::Expected<CompiledProjectModule> compileZiaProject(const ProjectConfig &project,
                                                               const ilc::SharedCliOptions &shared,
                                                               il::support::SourceManager &sm,
                                                               bool optimizeModule = true,
                                                               bool captureDebugLayouts = false) {
    il::frontends::zia::CompilerOptions opts;
    opts.boundsChecks = project.boundsChecks;
    opts.overflowChecks = project.overflowChecks;
    opts.nullChecks = project.nullChecks;
    opts.allowUnsafePointers = shared.allowUnsafePointers;
    opts.captureDebugLayouts = captureDebugLayouts;
    opts.dumpTokens = shared.dumpTokens;
    opts.dumpAst = shared.dumpAst;
    opts.dumpSemaAst = shared.dumpSemaAst;
    opts.dumpIL = shared.dumpIL;
    opts.dumpILOpt = shared.dumpILOpt;
    opts.dumpILPasses = shared.dumpILPasses;
    opts.verifyEachPass = shared.verifyEachPass;
    opts.passStats = shared.passStats;
    opts.timeCompile = shared.timeCompile;
    opts.parallelFunctionPasses = shouldEnableParallelFunctionPasses(shared);

    // Warning policy from CLI flags
    opts.warningPolicy.enableAll = shared.wall;
    opts.warningPolicy.warningsAsErrors = shared.werror;
    opts.warningPolicy.strictSafetyWarnings = shared.strictDiagnostics;
    for (const auto &w : shared.disabledWarnings) {
        if (auto code = il::frontends::zia::parseWarningCode(w)) {
            opts.warningPolicy.disabled.insert(*code);
        } else {
            il::support::Diagnostic diag{il::support::Severity::Error,
                                         "unknown warning name in -Wno-" + w,
                                         {},
                                         "V-CLI-WARNING"};
            ilc::printDiagnostic(diag, std::cerr, &sm, shared.diagnosticFormat);
            return il::support::Expected<CompiledProjectModule>(diag);
        }
    }

    if (!optimizeModule || project.optimizeLevel == "O0")
        opts.optLevel = il::frontends::zia::OptLevel::O0;
    else if (project.optimizeLevel == "O1")
        opts.optLevel = il::frontends::zia::OptLevel::O1;
    else if (project.optimizeLevel == "O2")
        opts.optLevel = il::frontends::zia::OptLevel::O2;

    const bool optimized = opts.optLevel != il::frontends::zia::OptLevel::O0;
    const bool needsLowerVerify = !optimized || shared.paranoidVerify || shared.verifyEachPass ||
                                  shared.dumpIL || shared.dumpILPasses;
    opts.verifyAfterLowering = needsLowerVerify;
    opts.verifyAfterOptimization = optimized || shared.paranoidVerify;

    auto result = il::frontends::zia::compileFile(project.entryFile, opts, sm);
    if (!result.succeeded() || (shared.showWarnings && result.diagnostics.warningCount() != 0)) {
        ilc::printDiagnosticEngine(result.diagnostics, std::cerr, &sm, shared.diagnosticFormat);
    }
    if (!result.succeeded()) {
        return il::support::Expected<CompiledProjectModule>(
            il::support::Diagnostic{il::support::Severity::Error, "compilation failed", {}, {}});
    }

    return CompiledProjectModule{std::move(result.module),
                                 result.moduleVerified,
                                 toVmDebugLayouts(std::move(result.debugClassLayouts))};
}

/// @brief Compile a BASIC project and return its lowered IL module.
/// @details Loads the entry source, applies the BASIC compiler options, optionally
///          runs the canonical IL optimizer pipeline, and verifies optimized output.
/// @param project Resolved BASIC project configuration.
/// @param noRuntimeNamespaces Whether runtime namespace binding is disabled.
/// @param shared Shared compiler, diagnostic, and dump options.
/// @param sm Source manager populated during loading and compilation.
/// @param optimizeModule Whether the project optimization pipeline should run.
/// @return Compiled module and verification state, or a diagnostic.
il::support::Expected<CompiledProjectModule> compileBasicProject(
    const ProjectConfig &project,
    bool noRuntimeNamespaces,
    const ilc::SharedCliOptions &shared,
    il::support::SourceManager &sm,
    bool optimizeModule = true) {
    const auto readStart = std::chrono::steady_clock::now();
    auto source = loadSourceBuffer(project.entryFile, sm);
    if (!source) {
        ilc::printDiagnostic(source.error(), std::cerr, &sm, shared.diagnosticFormat);
        return il::support::Expected<CompiledProjectModule>(
            il::support::Diagnostic{il::support::Severity::Error, "failed to load source", {}, {}});
    }
    printCompileTime(shared, "basic.read", readStart);

    std::optional<zanna::tools::ScopedEnvVar> noRuntimeNamespacesEnv;
    if (noRuntimeNamespaces) {
        noRuntimeNamespacesEnv.emplace("ZANNA_NO_RUNTIME_NAMESPACES", "1");
        if (!noRuntimeNamespacesEnv->ok()) {
            return il::support::Expected<CompiledProjectModule>(il::support::Diagnostic{
                il::support::Severity::Error, noRuntimeNamespacesEnv->errorMessage(), {}, {}});
        }
    }

    il::frontends::basic::BasicCompilerOptions opts;
    opts.boundsChecks = project.boundsChecks;
    opts.dumpTokens = shared.dumpTokens;
    opts.dumpAst = shared.dumpAst;
    opts.dumpIL = shared.dumpIL;
    opts.dumpILOpt = shared.dumpILOpt;
    opts.dumpILPasses = shared.dumpILPasses;
    opts.timeCompile = shared.timeCompile;
    opts.allowUnsafePointers = shared.allowUnsafePointers;

    il::frontends::basic::BasicCompilerInput input{source.value().buffer, project.entryFile};
    input.fileId = source.value().fileId;

    auto result = il::frontends::basic::compileBasic(input, opts, sm);
    const bool shouldPrintDiagnostics =
        !result.succeeded() || (shared.showWarnings && result.diagnostics.warningCount() != 0);
    if (shouldPrintDiagnostics && result.emitter) {
        if (shared.diagnosticFormat == ilc::DiagnosticFormat::Json) {
            ilc::printDiagnosticEngine(result.diagnostics, std::cerr, &sm, shared.diagnosticFormat);
        } else {
            result.emitter->printAll(std::cerr);
        }
    }
    if (!result.succeeded()) {
        return il::support::Expected<CompiledProjectModule>(
            il::support::Diagnostic{il::support::Severity::Error, "compilation failed", {}, {}});
    }

    // Apply the canonical IL optimizer pipeline based on the project's opt level.
    if (optimizeModule && project.optimizeLevel != "O0") {
        il::transform::PassManager pm;
        pm.setVerifyBetweenPasses(shared.verifyEachPass);
        pm.setReportPassStatistics(shared.passStats);
        pm.setInstrumentationStream(std::cerr);
        pm.enableParallelFunctionPasses(shouldEnableParallelFunctionPasses(shared));

        // Enable per-pass IL dumps when requested.
        if (shared.dumpILPasses) {
            pm.setPrintBeforeEach(true);
            pm.setPrintAfterEach(true);
        }

        const std::string pipelineId = (project.optimizeLevel == "O2") ? "O2" : "O1";
        result.moduleVerified = false;
        if (!pm.runPipeline(result.module, pipelineId)) {
            il::support::Diag diag{il::support::Severity::Error,
                                   "IL optimization pipeline '" + pipelineId +
                                       "' failed verification",
                                   {},
                                   "V-OPT-PIPELINE"};
            ilc::printDiagnostic(diag, std::cerr, &sm, shared.diagnosticFormat);
            return il::support::Expected<CompiledProjectModule>(il::support::Diagnostic{
                il::support::Severity::Error, "optimization failed", {}, "V-OPT-PIPELINE"});
        }

        if (!reportVerifierDiagnostics(
                result.module, std::cerr, sm, shared.diagnosticFormat, shared.showWarnings)) {
            return il::support::Expected<CompiledProjectModule>(
                il::support::Diagnostic{il::support::Severity::Error,
                                        "optimized BASIC IL failed verification",
                                        {},
                                        "V-OPT-VERIFY"});
        }
        result.moduleVerified = true;

        // Dump IL after the full optimization pipeline.
        if (shared.dumpILOpt) {
            std::cerr << "=== IL after optimization (" << project.optimizeLevel << ") ===\n";
            io::Serializer::write(result.module, std::cerr);
            std::cerr << "=== End IL ===\n";
        }
    }

    return CompiledProjectModule{std::move(result.module), result.moduleVerified};
}

/// @brief Compile a mixed-language project and link its Zia and BASIC modules.
/// @details Compiles the true entry module first, compiles each unique source in
///          the other language as a library module, generates boolean interop
///          thunks, links the modules, then optimizes and verifies the result.
/// @param project Resolved mixed-language project configuration.
/// @param noRuntimeNamespaces Whether BASIC runtime namespace binding is disabled.
/// @param shared Shared compiler, optimizer, and diagnostic options.
/// @param sm Source manager shared by all frontend compilations.
/// @return Linked and verified module, or the first compile/link diagnostic.
il::support::Expected<CompiledProjectModule> compileMixedProject(
    const ProjectConfig &project,
    bool noRuntimeNamespaces,
    const ilc::SharedCliOptions &shared,
    il::support::SourceManager &sm) {
    // Determine entry language from file extension.
    std::string entryExt;
    if (project.entryFile.size() >= 4)
        entryExt = lowerAscii(project.entryFile.substr(project.entryFile.size() - 4));

    bool entryIsZia = (entryExt == ".zia");

    // Build a single-language project config for the entry module.
    ProjectConfig entryProject = project;
    entryProject.lang = entryIsZia ? ProjectLang::Zia : ProjectLang::Basic;
    entryProject.sourceFiles = entryIsZia ? project.ziaFiles : project.basicFiles;

    // Build a single-language project config for the library module.
    ProjectConfig libProject = project;
    libProject.lang = entryIsZia ? ProjectLang::Basic : ProjectLang::Zia;
    libProject.sourceFiles = entryIsZia ? project.basicFiles : project.ziaFiles;

    // Compile the entry module.
    il::support::Expected<CompiledProjectModule> entryResult =
        entryIsZia ? compileZiaProject(entryProject, shared, sm, false)
                   : compileBasicProject(entryProject, noRuntimeNamespaces, shared, sm, false);
    if (!entryResult)
        return entryResult;

    // Compile every library-side source file. Mixed projects can contain multiple
    // independent files in the non-entry language, and picking just one makes
    // symbols vanish at link time.
    if (libProject.sourceFiles.empty())
        return entryResult; // No library files, just return the entry module.

    std::vector<std::string> libraryFiles = libProject.sourceFiles;
    std::sort(libraryFiles.begin(), libraryFiles.end());
    libraryFiles.erase(std::unique(libraryFiles.begin(), libraryFiles.end()), libraryFiles.end());
    const std::string preferredEntry = selectMixedLibraryEntry(libraryFiles, libProject.lang);
    if (!preferredEntry.empty()) {
        auto it = std::find(libraryFiles.begin(), libraryFiles.end(), preferredEntry);
        if (it != libraryFiles.end())
            std::rotate(libraryFiles.begin(), it, it + 1);
    }

    std::vector<il::core::Module> libraryModules;
    libraryModules.reserve(libraryFiles.size());
    for (const auto &libraryFile : libraryFiles) {
        ProjectConfig fileProject = libProject;
        fileProject.entryFile = libraryFile;
        fileProject.sourceFiles = {libraryFile};
        il::support::Expected<CompiledProjectModule> libResult =
            entryIsZia ? compileBasicProject(fileProject, noRuntimeNamespaces, shared, sm, false)
                       : compileZiaProject(fileProject, shared, sm, false);
        if (!libResult)
            return libResult;

        auto thunks =
            il::link::generateBooleanThunks(entryResult.value().module, libResult.value().module);
        for (auto &thunk : thunks)
            entryResult.value().module.functions.push_back(std::move(thunk.thunk));
        convertLibraryMainToInitializer(libResult.value().module, libraryModules.size());
        libraryModules.push_back(std::move(libResult.value().module));
    }

    // Link the two modules.
    std::vector<il::core::Module> modules;
    modules.reserve(1 + libraryModules.size());
    modules.push_back(std::move(entryResult.value().module));
    for (auto &libraryModule : libraryModules)
        modules.push_back(std::move(libraryModule));

    auto linkResult = il::link::linkModules(std::move(modules));
    if (!linkResult.succeeded()) {
        // Diagnostics render on one line, so join rather than embedding newlines.
        std::string errMsg = "linking mixed-language modules failed: ";
        for (size_t e = 0; e < linkResult.errors.size(); ++e) {
            if (e != 0)
                errMsg += "; ";
            errMsg += linkResult.errors[e];
        }
        il::support::Diagnostic diag{il::support::Severity::Error, errMsg, {}, {}};
        // The caller assumes every compile*Project has already reported; without
        // this the whole failure is a silent exit status.
        ilc::printDiagnostic(diag, std::cerr, &sm, shared.diagnosticFormat);
        return il::support::Expected<CompiledProjectModule>(std::move(diag));
    }

    CompiledProjectModule compiled{std::move(linkResult.module), false};
    if (project.optimizeLevel != "O0") {
        il::transform::PassManager pm;
        pm.setVerifyBetweenPasses(shared.verifyEachPass);
        pm.setReportPassStatistics(shared.passStats);
        pm.setInstrumentationStream(std::cerr);
        pm.enableParallelFunctionPasses(shouldEnableParallelFunctionPasses(shared));
        const std::string pipelineId = (project.optimizeLevel == "O2") ? "O2" : "O1";
        if (!pm.runPipeline(compiled.module, pipelineId)) {
            il::support::Diagnostic diag{
                il::support::Severity::Error, "linked mixed-module optimization failed", {}, {}};
            ilc::printDiagnostic(diag, std::cerr, &sm, shared.diagnosticFormat);
            return il::support::Expected<CompiledProjectModule>(std::move(diag));
        }
    }

    if (!reportVerifierDiagnostics(
            compiled.module, std::cerr, sm, shared.diagnosticFormat, shared.showWarnings)) {
        return il::support::Expected<CompiledProjectModule>(il::support::Diagnostic{
            il::support::Severity::Error, "linked mixed-module verification failed", {}, {}});
    }
    compiled.verified = true;
    return compiled;
}

/// @brief Execute a fully parsed run/build/check configuration.
/// @param config Parsed command configuration.
/// @return Process exit code for the requested command mode.
int executeRunBuildConfig(RunBuildConfig config);

/// @brief Parse and execute a run, build, or check command.
/// @param mode Requested terminal operation.
/// @param argc Number of command arguments in @p argv.
/// @param argv Command arguments excluding the executable and subcommand.
/// @return Process exit code produced by parsing or command execution.
int runOrBuild(RunMode mode, int argc, char **argv) {
    const ilc::DiagnosticFormat earlyDiagnosticFormat = ilc::detectDiagnosticFormatFlag(argc, argv);
    auto parsed = parseRunBuildArgs(mode, argc, argv);
    if (!parsed) {
        const auto &diag = parsed.error();
        SourceManager sm;
        printCommandDiagnostic(diag, &sm, earlyDiagnosticFormat);
        if (earlyDiagnosticFormat == ilc::DiagnosticFormat::Text)
            printRunBuildUsage(mode);
        return 1;
    }

    return executeRunBuildConfig(std::move(parsed.value()));
}

/// @brief Execute a fully parsed run, build, or check configuration.
/// @details Resolves the project, applies command-line policy, compiles the
///          appropriate frontend combination, and then verifies, emits, builds,
///          or executes the resulting IL module.
/// @param config Parsed command configuration, consumed during execution.
/// @return Zero on success, two for check-mode compile/verifier errors, or one
///         for other command failures.
int executeRunBuildConfig(RunBuildConfig config) {
    const RunMode mode = config.mode;
    if (config.helpRequested) {
        printRunBuildUsage(mode, std::cout);
        return 0;
    }

    // Resolve the project
    const auto projectStart = std::chrono::steady_clock::now();
    auto project = resolveProject(config.target);
    if (!project) {
        SourceManager sm;
        printCommandDiagnostic(project.error(), &sm, config.shared.diagnosticFormat);
        return 1;
    }

    ProjectConfig &proj = project.value();
    printCompileTime(config.shared, "project-resolve", projectStart);

    // Apply CLI overrides
    if (config.shared.boundsChecksSpecified)
        proj.boundsChecks = config.shared.boundsChecks;
    if (config.buildProfileOverride) {
        proj.buildProfile = *config.buildProfileOverride;
        proj.optimizeLevel = *optimizeForBuildProfile(*config.buildProfileOverride);
    }
    if (config.optimizeLevelOverride)
        proj.optimizeLevel = *config.optimizeLevelOverride;
    if (mode == RunMode::Run && !config.buildProfileOverride && !config.optimizeLevelOverride &&
        !proj.buildProfileExplicit && !proj.optimizeLevelExplicit) {
        // Run keeps instruction-to-source mapping unless the project or CLI asks otherwise.
        proj.buildProfile = "debug";
        proj.optimizeLevel = "O0";
    }

    // Compile
    SourceManager sm;
    const auto compileStart = std::chrono::steady_clock::now();
    il::support::Expected<CompiledProjectModule> moduleResult =
        (proj.lang == ProjectLang::Mixed)
            ? compileMixedProject(proj, config.noRuntimeNamespaces, config.shared, sm)
        : (proj.lang == ProjectLang::Zia)
            ? compileZiaProject(proj,
                                config.shared,
                                sm,
                                /*optimizeModule=*/true,
                                /*captureDebugLayouts=*/config.debugAdapter)
            : compileBasicProject(proj, config.noRuntimeNamespaces, config.shared, sm);
    printCompileTime(config.shared, "source-to-il", compileStart);

    if (!moduleResult)
        return mode == RunMode::Check ? 2 : 1; // diagnostics already printed

    CompiledProjectModule compiled = std::move(moduleResult.value());
    il::core::Module module = std::move(compiled.module);
    bool moduleVerified = compiled.verified;

    // Check mode: verify and stop without emitting or executing.
    if (mode == RunMode::Check) {
        const auto verifyStart = std::chrono::steady_clock::now();
        if (!moduleVerified && !reportVerifierDiagnostics(module,
                                                          std::cerr,
                                                          sm,
                                                          config.shared.diagnosticFormat,
                                                          config.shared.showWarnings)) {
            return 2;
        }
        printCompileTime(config.shared, "final-verify", verifyStart);
        return 0;
    }

    // Build mode: emit IL or compile to native binary
    if (mode == RunMode::Build) {
        // Verify before emitting
        const auto verifyStart = std::chrono::steady_clock::now();
        if (!moduleVerified && !reportVerifierDiagnostics(module,
                                                          std::cerr,
                                                          sm,
                                                          config.shared.diagnosticFormat,
                                                          config.shared.showWarnings)) {
            return 1;
        }
        moduleVerified = true;
        printCompileTime(config.shared, "final-verify", verifyStart);

        // No -o: emit IL to stdout (backwards compat)
        if (config.outputPath.empty()) {
            io::Serializer::write(module, std::cout);
            return 0;
        }

        // -o path ends in .il: emit IL text (backwards compat)
        if (!zanna::tools::isNativeOutputPath(config.outputPath)) {
            const auto outputParent =
                zanna::filesystem::pathFromUtf8(config.outputPath).parent_path();
            if (!outputParent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(outputParent, ec);
                if (ec) {
                    printCommandDiagnostic(
                        makeCommandError("cannot create output directory: " +
                                         zanna::filesystem::pathToUtf8(outputParent) + ": " +
                                         ec.message()),
                        &sm,
                        config.shared.diagnosticFormat);
                    return 1;
                }
            }
            std::ostringstream ilText;
            io::Serializer::write(module, ilText);
            if (!ilText) {
                printCommandDiagnostic(
                    makeCommandError("failed to serialize IL for " + config.outputPath),
                    &sm,
                    config.shared.diagnosticFormat);
                return 1;
            }
            try {
                zanna::pkg::writeTextFileAtomic(config.outputPath, ilText.str());
            } catch (const std::exception &ex) {
                printCommandDiagnostic(makeCommandError("failed to write IL to " +
                                                        config.outputPath + ": " + ex.what()),
                                       &sm,
                                       config.shared.diagnosticFormat);
                return 1;
            }
            return 0;
        }

        // Native binary output: hand the verified module directly to codegen.
        auto arch = config.archOverride.value_or(zanna::tools::detectHostArch());

        // Compile assets (embed → blob, pack → .zpak files)
        ScopedTempPath assetBlobTemp;
        std::string assetBlobPath;
        if (!proj.embedAssets.empty() || !proj.packGroups.empty()) {
            const auto assetStart = std::chrono::steady_clock::now();
            std::string outputDir = zanna::filesystem::pathToUtf8(
                zanna::filesystem::pathFromUtf8(config.outputPath).parent_path());
            if (outputDir.empty())
                outputDir = ".";

            std::string assetErr;
            auto bundle = zanna::asset::compileAssets(proj, outputDir, assetErr);
            if (!bundle) {
                printCommandDiagnostic(makeCommandError("asset compilation failed: " + assetErr),
                                       &sm,
                                       config.shared.diagnosticFormat);
                return 1;
            }

            // Write the ZPAK blob to a temp file; codegen injects it into .rodata
            // (the same path as `zanna codegen --asset-blob`). We deliberately do
            // NOT also emit/link a separate asset .o here: doing both defines the
            // `zanna_asset_blob` symbol twice and fails linking with
            // "multiply defined symbol 'zanna_asset_blob'". assetObjPath stays
            // empty so compileModuleToNative skips the redundant extra object.
            if (!bundle->embeddedBlob.empty()) {
                assetBlobTemp.reset(zanna::tools::generateTempAssetPath());
                assetBlobPath = assetBlobTemp.path();
                try {
                    zanna::pkg::writeFileAtomic(assetBlobPath, bundle->embeddedBlob);
                } catch (const std::exception &ex) {
                    printCommandDiagnostic(
                        makeCommandError("failed to write temporary asset blob: " + assetBlobPath +
                                         ": " + ex.what()),
                        &sm,
                        config.shared.diagnosticFormat);
                    return 1;
                }
            }
            printCompileTime(config.shared, "assets", assetStart);
        }

        const int backendOptimizeLevel = optimizeLevelNumber(proj.optimizeLevel).value_or(1);
        const bool fastLink = config.fastLinkOverride.value_or(backendOptimizeLevel == 0);
        const auto nativeStart = std::chrono::steady_clock::now();
        int rc = zanna::tools::compileModuleToNative(std::move(module),
                                                     proj.entryFile,
                                                     config.outputPath,
                                                     arch,
                                                     assetBlobPath,
                                                     std::string{},
                                                     backendOptimizeLevel,
                                                     true,
                                                     moduleVerified,
                                                     config.shared.timeCompile,
                                                     fastLink,
                                                     config.windowsDebugRuntimeOverride,
                                                     config.stackSizeOverride.value_or(0),
                                                     !config.stripSymbols);
        printCompileTime(config.shared, "native-codegen-link", nativeStart);
        return rc;
    }

    // Run mode: verify and execute
    return verifyAndExecute(module,
                            config.shared,
                            config.programArgs,
                            config.debugVm,
                            config.debugAdapter,
                            moduleVerified,
                            sm,
                            std::move(compiled.debugLayouts));
}

} // namespace

/// @brief Execute a source project or delegate a direct IL target to the IL runner.
/// @param argc Number of command arguments in @p argv.
/// @param argv Command arguments excluding the executable and `run` subcommand.
/// @return Program exit status or a command failure code.
int cmdRun(int argc, char **argv) {
    // `zanna run file.il` executes IL directly: delegate to the IL runner so the
    // discoverable run subcommand covers the project's normative intermediate
    // language, not only source-language targets. Any pre-`--` argument ending in
    // .il selects the IL runner (flag values are indistinguishable from targets
    // here, and an .il value only occurs for IL runs); all arguments are
    // forwarded so IL-runner flags keep working.
    for (int i = 0; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--")
            break;
        if (arg.size() > 3 && lowerAscii(std::string(arg.substr(arg.size() - 3))) == ".il")
            return cmdRunIL(argc, argv);
    }
    return runOrBuild(RunMode::Run, argc, argv);
}

/// @brief Build IL text or a native executable from a source project.
/// @param argc Number of command arguments in @p argv.
/// @param argv Command arguments excluding the executable and `build` subcommand.
/// @return Zero on success; one on option, compilation, verification, or emission failure.
int cmdBuild(int argc, char **argv) {
    return runOrBuild(RunMode::Build, argc, argv);
}

/// @brief Build several named projects while retaining process-local compiler caches.
/// @details Each positional target has the form `name=project`. The command invokes
///          the ordinary native-build pipeline once per project in stable argument
///          order, placing executables beneath `--output-dir`. Keeping the builds in
///          one process allows immutable runtime archives and other process-wide
///          compiler data to be reused without changing the semantics of an
///          individual `zanna build` invocation. A failed target does not prevent
///          later targets from being attempted, so callers receive a complete batch
///          result and deterministic diagnostics.
/// @param argc Number of arguments following the `build-many` command name.
/// @param argv Argument vector following the `build-many` command name.
/// @return Zero when every target builds successfully; one for invalid arguments,
///         output-directory failures, or one or more failed project builds.
int cmdBuildMany(int argc, char **argv) {
    constexpr std::string_view usage =
        "Usage: zanna build-many --output-dir DIR [--arch x64|arm64] "
        "[-O0|-O1|-O2] [--fast-link] [--time-compile] name=project [...]\n";
    std::filesystem::path outputDir;
    std::optional<std::string> arch;
    std::optional<std::string> optimize;
    bool fastLink = false;
    bool timeCompile = false;
    std::vector<std::pair<std::string, std::string>> projects;

    for (int index = 0; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "-h" || arg == "--help") {
            std::cout << usage;
            return 0;
        }
        if (arg == "--output-dir") {
            if (++index >= argc) {
                std::cerr << "error: --output-dir requires a path\n" << usage;
                return 1;
            }
            outputDir = zanna::filesystem::pathFromUtf8(argv[index]);
            continue;
        }
        if (arg == "--arch") {
            if (++index >= argc || !parseTargetArch(argv[index])) {
                std::cerr << "error: --arch requires x64 or arm64\n" << usage;
                return 1;
            }
            arch = argv[index];
            continue;
        }
        if (arg == "--fast-link") {
            fastLink = true;
            continue;
        }
        if (arg == "--time-compile") {
            timeCompile = true;
            continue;
        }
        if (arg == "-O0" || arg == "-O1" || arg == "-O2") {
            optimize = std::string(arg.substr(1));
            continue;
        }
        const std::size_t equals = arg.find('=');
        if (equals == std::string_view::npos || equals == 0 || equals + 1 >= arg.size()) {
            std::cerr << "error: expected name=project target, got '" << arg << "'\n" << usage;
            return 1;
        }
        const std::string name(arg.substr(0, equals));
        if (zanna::filesystem::pathFromUtf8(name).filename() !=
                zanna::filesystem::pathFromUtf8(name) ||
            name == "." || name == "..") {
            std::cerr << "error: build-many output name must be one path component: '" << name
                      << "'\n";
            return 1;
        }
        projects.emplace_back(name, std::string(arg.substr(equals + 1)));
    }

    if (outputDir.empty() || projects.empty()) {
        std::cerr << "error: build-many requires --output-dir and at least one project\n" << usage;
        return 1;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(outputDir, directoryError);
    if (directoryError) {
        std::cerr << "error: cannot create build-many output directory: "
                  << directoryError.message() << "\n";
        return 1;
    }

    int failures = 0;
    for (const auto &[name, target] : projects) {
        RunBuildConfig config;
        config.mode = RunMode::Build;
        config.target = target;
        config.outputPath =
            zanna::filesystem::pathToUtf8(outputDir / zanna::filesystem::pathFromUtf8(name));
        config.shared.timeCompile = timeCompile;
        config.fastLinkOverride = fastLink;
        if (arch)
            config.archOverride = *parseTargetArch(*arch);
        if (optimize)
            config.optimizeLevelOverride = *optimize;
        std::cerr << "[build-many] " << name << " <- " << target << "\n";
        if (executeRunBuildConfig(std::move(config)) != 0)
            ++failures;
    }
    if (failures != 0)
        std::cerr << "error: build-many failed for " << failures << " project(s)\n";
    return failures == 0 ? 0 : 1;
}

/// @brief Type-check and verify a project without emitting or executing it.
/// @param argc Number of command arguments in @p argv.
/// @param argv Command arguments excluding the executable and `check` subcommand.
/// @return Zero when clean, one for usage/resolution errors, or two for compile errors.
int cmdCheck(int argc, char **argv) {
    return runOrBuild(RunMode::Check, argc, argv);
}

/// @brief Build a project's native executable for the package command.
/// @details Constructs a build-mode configuration without reparsing command-line
///          arguments and optionally forces the Windows release runtime.
/// @param target Project file, directory, or manifest path.
/// @param outputPath Destination native executable path.
/// @param arch Optional `arm64` or `x64` target architecture.
/// @param windowsReleaseRuntime Whether Windows builds must use the release runtime.
/// @return Zero on success; one for invalid inputs or native-build failure.
int buildProjectToNativeForPackage(const std::string &target,
                                   const std::string &outputPath,
                                   const std::string &arch,
                                   bool windowsReleaseRuntime) {
    if (target.empty()) {
        SourceManager sm;
        printCommandDiagnostic(
            makeCommandError("package build target is empty"), &sm, ilc::DiagnosticFormat::Text);
        return 1;
    }
    if (outputPath.empty()) {
        SourceManager sm;
        printCommandDiagnostic(makeCommandError("package native output path is empty"),
                               &sm,
                               ilc::DiagnosticFormat::Text);
        return 1;
    }

    RunBuildConfig config;
    config.mode = RunMode::Build;
    config.target = target;
    config.outputPath = outputPath;
    if (!arch.empty()) {
        auto parsedArch = parseTargetArch(arch);
        if (!parsedArch) {
            SourceManager sm;
            printCommandDiagnostic(makeCommandError("package build architecture must be 'arm64' or "
                                                    "'x64'"),
                                   &sm,
                                   ilc::DiagnosticFormat::Text);
            return 1;
        }
        config.archOverride = *parsedArch;
    }
    if (windowsReleaseRuntime)
        config.windowsDebugRuntimeOverride = false;
    return executeRunBuildConfig(std::move(config));
}
