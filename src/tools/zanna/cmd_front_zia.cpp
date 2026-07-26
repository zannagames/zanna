//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_front_zia.cpp
/// @brief CLI implementation for the `zanna front zia` subcommand.
///
/// The implementation stages shared-option parsing, source loading, warning policy, compilation,
/// verifier diagnostics, IL emission, stdin redirection, and standard or bytecode VM execution.

#include "cli.hpp"
#include "frontends/zia/Compiler.hpp"
#include "frontends/zia/Warnings.hpp"
#include "il/api/expected_api.hpp"
#include "il/verify/Verifier.hpp"
#include "support/diag_expected.hpp"
#include "support/source_manager.hpp"
#include "tools/common/ScopedProcess.hpp"
#include "tools/common/source_loader.hpp"
#include "tools/common/vm_executor.hpp"
#include "zanna/il/IO.hpp"
#include "zanna/il/Verify.hpp"
#include "zanna/vm/VM.hpp"
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace il;
using namespace il::frontends::zia;
using namespace il::support;

namespace {

/// @brief Run the IL verifier on @p module and print any diagnostics.
/// @details Collects up to 50 diagnostics; prints them (errors always, warnings
///          only when @p showWarnings) using the requested format.
/// @param module Module to verify.
/// @param err Destination diagnostic stream.
/// @param sm Source manager used to resolve locations.
/// @param format Text or JSON output encoding.
/// @param showWarnings Whether verifier warnings are printed alongside errors.
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

/// @brief Parsed configuration for the Zia frontend subcommand.
/// @details Captures whether the user requested IL emission or execution, plus
///          shared CLI options and any extra program arguments.
struct FrontZiaConfig {
    bool emitIl{false};                   ///< True when `-emit-il` is requested.
    bool run{false};                      ///< True when `-run` is requested.
    bool debugVm{false};                  ///< True to use standard VM for debugging.
    bool helpRequested{false};            ///< True when help was requested.
    std::string sourcePath;               ///< Path to the input `.zia` source.
    ilc::SharedCliOptions shared;         ///< Shared CLI settings (trace, steps, IO).
    std::vector<std::string> programArgs; ///< Extra arguments forwarded to the program.
};

/// @brief Print usage for the `zanna front zia` subcommand to stderr.
void frontZiaUsage() {
    std::cerr
        << "Usage: zanna front zia (-emit-il|-run) <file.zia> [options] [-- program-args...]\n"
        << "\n"
        << "Options:\n"
        << "  -emit-il                       Emit IL to stdout\n"
        << "  -run                           Compile and execute\n"
        << "  --debug-vm                     Use the standard VM for debugging\n"
        << "  --trace[=il|src]               Enable execution tracing\n"
        << "  --stdin-from FILE              Redirect stdin from file\n"
        << "  --max-steps N                  Limit VM execution steps\n"
        << "  --dump-trap                    Show detailed trap diagnostics\n"
        << "  --bounds-checks                Enable generated bounds checks\n"
        << "  --no-bounds-checks             Disable generated bounds checks\n"
        << "  --diagnostic-format text|json  Select diagnostic output format\n"
        << "  -Wall, -Werror, -Wno-XXXX      Control Zia warnings\n"
        << "  -h, --help                     Show this help\n";
}

/// @brief Parse CLI arguments for the Zia frontend subcommand.
/// @details Recognizes `-emit-il` and `-run`, delegates shared flags to
///          @ref ilc::parseSharedOption, and collects any program arguments
///          after `--`. When parsing fails, a diagnostic is returned with a
///          precise message such as "unknown flag: X" or
///          "specify exactly one of -emit-il or -run, followed by source file".
/// @param argc Number of arguments in @p argv.
/// @param argv Argument vector for the subcommand (excluding `zanna front zia`).
/// @return Expected configuration on success; diagnostic on failure.
il::support::Expected<FrontZiaConfig> parseFrontZiaArgs(int argc, char **argv) {
    FrontZiaConfig config{};

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "-emit-il") {
            config.emitIl = true;
        } else if (arg == "-run") {
            config.run = true;
        } else if (arg == "--debug-vm") {
            config.debugVm = true;
        } else if (arg == "--help" || arg == "-h") {
            config.helpRequested = true;
            return il::support::Expected<FrontZiaConfig>(std::move(config));
        } else if (arg == "--") {
            for (int j = i + 1; j < argc; ++j)
                config.programArgs.emplace_back(argv[j]);
            break;
        } else {
            switch (ilc::parseSharedOption(i, argc, argv, config.shared)) {
                case ilc::SharedOptionParseResult::Parsed:
                    continue;
                case ilc::SharedOptionParseResult::Error:
                    return il::support::Expected<FrontZiaConfig>(il::support::Diagnostic{
                        il::support::Severity::Error, ilc::lastSharedOptionError(), {}, {}});
                case ilc::SharedOptionParseResult::NotMatched:
                    if (!arg.empty() && arg[0] != '-') {
                        if (!config.sourcePath.empty()) {
                            return il::support::Expected<FrontZiaConfig>(
                                il::support::Diagnostic{il::support::Severity::Error,
                                                        "multiple source files are not supported",
                                                        {},
                                                        {}});
                        }
                        config.sourcePath = arg;
                    } else {
                        return il::support::Expected<FrontZiaConfig>(il::support::Diagnostic{
                            il::support::Severity::Error,
                            std::string("unknown flag: ") + std::string(arg),
                            {},
                            {}});
                    }
                    break;
            }
        }
    }

    if (!config.helpRequested && ((config.emitIl == config.run) || config.sourcePath.empty())) {
        return il::support::Expected<FrontZiaConfig>(il::support::Diagnostic{
            il::support::Severity::Error,
            "specify exactly one of -emit-il or -run, followed by source file",
            {},
            {}});
    }

    return il::support::Expected<FrontZiaConfig>(std::move(config));
}

/// @brief Compile and optionally execute a Zia program.
/// @details Compiles the provided source into IL, emits IL when requested, or
///          otherwise verifies the IL and executes it via the VM. The runtime
///          configuration respects shared CLI options (trace, max steps, stdin,
///          and program arguments). Traps are reported to stderr, and the
///          return code is coerced to non-zero when a trap occurs.
/// @param config Parsed CLI configuration.
/// @param source Full source text to compile.
/// @param sm Source manager used for diagnostics and trace reporting.
/// @return Zero on success; non-zero on compile, verify, IO, or runtime errors.
int runFrontZia(const FrontZiaConfig &config,
                const std::string &source,
                il::support::SourceManager &sm) {
    CompilerInput compilerInput{source, config.sourcePath};
    CompilerOptions compilerOpts{};
    compilerOpts.boundsChecks = config.shared.boundsChecks;
    compilerOpts.allowUnsafePointers = config.shared.allowUnsafePointers;
    compilerOpts.dumpTokens = config.shared.dumpTokens;
    compilerOpts.dumpAst = config.shared.dumpAst;
    compilerOpts.dumpSemaAst = config.shared.dumpSemaAst;
    compilerOpts.dumpIL = config.shared.dumpIL;
    compilerOpts.dumpILOpt = config.shared.dumpILOpt;
    compilerOpts.dumpILPasses = config.shared.dumpILPasses;
    compilerOpts.verifyEachPass = config.shared.verifyEachPass;
    compilerOpts.passStats = config.shared.passStats;

    // Warning policy from CLI flags
    compilerOpts.warningPolicy.enableAll = config.shared.wall;
    compilerOpts.warningPolicy.warningsAsErrors = config.shared.werror;
    compilerOpts.warningPolicy.strictSafetyWarnings = config.shared.strictDiagnostics;
    for (const auto &w : config.shared.disabledWarnings) {
        if (auto code = parseWarningCode(w)) {
            compilerOpts.warningPolicy.disabled.insert(*code);
        } else {
            ilc::printDiagnostic(il::support::Diagnostic{il::support::Severity::Error,
                                                         "unknown warning name in -Wno-" + w,
                                                         {},
                                                         "V-CLI-WARNING"},
                                 std::cerr,
                                 &sm,
                                 config.shared.diagnosticFormat);
            return 1;
        }
    }

    auto result = compile(compilerInput, compilerOpts, sm);

    if (!result.succeeded() ||
        (config.shared.showWarnings && result.diagnostics.warningCount() != 0)) {
        ilc::printDiagnosticEngine(
            result.diagnostics, std::cerr, &sm, config.shared.diagnosticFormat);
    }

    if (!result.succeeded()) {
        return 1;
    }

    core::Module module = std::move(result.module);

    if (config.emitIl) {
        if (!reportVerifierDiagnostics(module,
                                       std::cerr,
                                       sm,
                                       config.shared.diagnosticFormat,
                                       config.shared.showWarnings)) {
            return 1;
        }
        io::Serializer::write(module, std::cout);
        return 0;
    }

    if (!reportVerifierDiagnostics(module, std::cerr, sm, config.shared.diagnosticFormat, false)) {
        return 1;
    }

    std::optional<zanna::tools::ScopedStdinRedirect> stdinRedirect;
    if (!config.shared.stdinPath.empty()) {
        stdinRedirect.emplace(config.shared.stdinPath);
        if (!stdinRedirect->ok()) {
            ilc::printDiagnostic(il::support::Diagnostic{il::support::Severity::Error,
                                                         "unable to open stdin file: " +
                                                             stdinRedirect->errorMessage(),
                                                         {},
                                                         {}},
                                 std::cerr,
                                 &sm,
                                 config.shared.diagnosticFormat);
            return 1;
        }
    }

    // Use standard VM for debugging (when --debug-vm or --trace is specified)
    bool useStandardVm = config.debugVm || config.shared.trace.enabled();

    if (useStandardVm) {
        vm::TraceConfig traceCfg = config.shared.trace;
        traceCfg.sm = &sm;

        vm::RunConfig runCfg;
        runCfg.trace = traceCfg;
        runCfg.maxSteps = config.shared.maxSteps;
        runCfg.programArgs = config.programArgs;

        vm::Runner runner(module, std::move(runCfg));
        int rc = static_cast<int>(runner.run());

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
        return rc;
    }

    // Default: use fast bytecode VM with threaded dispatch
    il::tools::common::VMExecutorConfig vmConfig;
    vmConfig.programArgs = config.programArgs;
    vmConfig.outputTrapMessage = true;
    vmConfig.flushStdout = true;
    vmConfig.sourceManager = &sm;
    vmConfig.maxSteps = config.shared.maxSteps;

    auto vmResult = il::tools::common::executeBytecodeVM(module, vmConfig);
    return vmResult.exitCode;
}

} // namespace

/// @brief Entry point for the `zanna front zia` subcommand.
/// @details Parses command-line flags, loads the source file, and delegates to
///          @ref runFrontZia for compilation and execution.
/// @param argc Number of arguments in @p argv.
/// @param argv Argument vector for the subcommand.
/// @return Zero on success; non-zero on parsing, IO, or runtime failure.
int cmdFrontZia(int argc, char **argv) {
    SourceManager sm;
    const auto earlyFormat = ilc::detectDiagnosticFormatFlag(argc, argv);

    auto parsed = parseFrontZiaArgs(argc, argv);
    if (!parsed) {
        const auto &diag = parsed.error();
        ilc::printDiagnostic(diag, std::cerr, &sm, earlyFormat);
        if (earlyFormat == ilc::DiagnosticFormat::Text)
            frontZiaUsage();
        return 1;
    }

    FrontZiaConfig config = std::move(parsed.value());
    if (config.helpRequested) {
        frontZiaUsage();
        return 0;
    }

    auto source = il::tools::common::loadSourceBuffer(config.sourcePath, sm);
    if (!source) {
        const auto &diag = source.error();
        ilc::printDiagnostic(diag, std::cerr, &sm, config.shared.diagnosticFormat);
        return 1;
    }

    return runFrontZia(config, source.value().buffer, sm);
}
