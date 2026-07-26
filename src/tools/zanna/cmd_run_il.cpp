//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the `zanna -run` compatibility subcommand that executes textual IL modules through
// the in-process virtual machine.  The driver coordinates command-line parsing,
// debugger configuration, module loading, verification, and VM execution while
// reporting optional profiling information.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Entry point for the `zanna -run` compatibility subcommand.
/// @details Provides CLI parsing helpers, debugger configuration utilities, and
///          the glue that loads IL from disk before launching the VM.  The
///          helpers document how tracing, breakpoints, and summary reporting are
///          wired into the driver so new flags can be added consistently.

#include "break_spec.hpp"
#include "bytecode/BytecodeCompiler.hpp"
#include "bytecode/BytecodeVM.hpp"
#include "cli.hpp"
#include "il/core/Function.hpp"
#include "il/core/Module.hpp"
#include "runtime/core/rt_args.h"
#include "support/diag_expected.hpp"
#include "support/source_manager.hpp"
#include "tools/common/ScopedProcess.hpp"
#include "tools/common/module_loader.hpp"
#include "zanna/vm/VM.hpp"
#include "zanna/vm/debug/Debug.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace il;

namespace {

/// @brief Clears bytecode runtime argv state around a `zanna -run --bytecode` execution.
/// @details Construction clears any host argv fallback, then pushes forwarded
///          program arguments. Destruction clears the runtime argument list so
///          subsequent VM runs do not inherit it.
class RuntimeArgsScope {
  public:
    /// @brief Replace runtime argv with @p programArgs for this scope.
    /// @param programArgs Program arguments to publish through the runtime API.
    explicit RuntimeArgsScope(const std::vector<std::string> &programArgs) {
        rt_args_clear();
        for (const auto &arg : programArgs) {
            rt_string tmp = rt_string_from_bytes(arg.data(), arg.size());
            rt_args_push(tmp);
            rt_string_unref(tmp);
        }
    }

    /// @brief Clear the scope-owned runtime argument list.
    ~RuntimeArgsScope() {
        rt_args_clear();
    }

    RuntimeArgsScope(const RuntimeArgsScope &) = delete;
    RuntimeArgsScope &operator=(const RuntimeArgsScope &) = delete;
};

/// @brief Parsed configuration for the `zanna -run` (run-IL) command.
/// @details Captures the IL file, shared CLI options, debugger settings
///          (breakpoints, watches, step/continue), profiling flags, and the
///          chosen VM backend (tree-walk or bytecode).
struct RunILConfig {
    /// @brief A file:line source breakpoint request.
    struct SourceBreak {
        std::string file;  ///< Source file the breakpoint refers to.
        uint32_t line = 0; ///< 1-based line number.
    };

    std::string ilFile;                           ///< Path to the IL file to run.
    ilc::SharedCliOptions sharedOpts;             ///< Shared CLI settings (trace, steps, IO).
    std::vector<std::string> programArgs;         ///< Args forwarded after `--`.
    std::vector<std::string> breakLabels;         ///< Block-label breakpoints.
    std::vector<SourceBreak> breakSrcLines;       ///< Source file:line breakpoints.
    std::vector<std::string> watchSymbols;        ///< Variables to watch.
    std::string debugScriptPath;                  ///< Optional debugger script path.
    bool stepFlag = false;                        ///< Start in single-step mode.
    bool continueFlag = false;                    ///< Continue immediately after setup.
    bool countFlag = false;                       ///< Print instruction counts.
    bool timeFlag = false;                        ///< Print execution time.
    bool helpRequested = false;                   ///< True when help was requested.
    bool boundsChecksRequested = false;           ///< Enable runtime bounds checks.
    bool useBytecode = false;                     ///< Use the bytecode VM instead of tree-walk.
    bool useBytecodeThreaded = false;             ///< Use threaded-dispatch bytecode VM.
    vm::DebugCtrl debugCtrl;                      ///< Debugger control state.
    std::unique_ptr<vm::DebugScript> debugScript; ///< Loaded debugger script, if any.
};

/// @brief Trim leading and trailing ASCII whitespace from a string.
/// @details Iterates over the input to find the first and last characters that
///          are not classified as whitespace by @c std::isspace before
///          returning the inclusive substring.  An all-whitespace input yields
///          an empty string, making it convenient for downstream validation
///          logic.
/// @param text Candidate string containing surrounding padding.
/// @return Copy of @p text with outer whitespace removed.
std::string trimWhitespace(std::string text) {
    /// @brief Identify the first non-whitespace byte from the beginning.
    /// @param ch Byte to inspect.
    /// @return `true` when `ch` is not whitespace.
    auto begin = std::find_if_not(
        text.begin(), text.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    /// @brief Identify the first non-whitespace byte from the reverse direction.
    /// @param ch Byte to inspect.
    /// @return `true` when `ch` is not whitespace.
    auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
                   return std::isspace(ch) != 0;
               }).base();
    if (begin >= end) {
        return std::string();
    }
    return std::string(begin, end);
}

/// @brief Parse a decimal breakpoint line number from a CLI token.
/// @details Validates that @p token contains only digits before accumulating the
///          numeric value.  Successful conversions must be strictly positive and
///          no larger than @c std::numeric_limits<uint32_t>::max().  The parsed
///          value is stored in @p line and the helper returns true.  Failures
///          leave @p line untouched and return false so callers can surface
///          consistent diagnostics.
/// @param token Candidate substring containing the numeric portion of a
///        breakpoint spec.
/// @param line Output slot populated with the parsed value on success.
/// @return True when @p token encodes a positive decimal integer.
bool tryParseLineNumber(const std::string &token, uint32_t &line) {
    if (token.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (char ch : token) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(ch - '0');
        if (value > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    line = static_cast<uint32_t>(value);
    return true;
}

/// @brief Report a malformed line-number argument and display usage text.
/// @details Prints the offending token alongside the flag that referenced it
///          and then invokes @ref usage() to show help before returning to the
///          caller.  Keeping this logic centralised guarantees identical wording
///          for @c --break and @c --break-src failures.
/// @param lineToken Token that failed validation (may be empty when missing).
/// @param spec Full argument passed to the flag.
/// @param flag Flag name responsible for the argument, such as "--break".
void reportInvalidLineNumber(const std::string &lineToken,
                             const std::string &spec,
                             const char *flag) {
    std::cerr << "invalid line number '" << lineToken << "' for " << flag;
    if (!spec.empty()) {
        std::cerr << " argument \"" << spec << "\"";
    }
    std::cerr << "\n";
    usage();
}

/// @brief Decode all `ilc run`-specific command-line arguments.
/// @details Extracts the input file, debugger options, and execution summary
///          toggles while delegating shared options to
///          @ref ilc::parseSharedOption.  Missing operands or unknown flags
///          trigger usage output and a false return value so @ref cmdRunIL can
///          abort gracefully.
/// @param argc Number of arguments supplied to the subcommand.
/// @param argv Argument array (first element is the IL file path).
/// @param config Configuration structure populated with parsed state.
/// @return True on success; false after emitting usage information.
bool parseRunILArgs(int argc, char **argv, RunILConfig &config) {
    if (argc < 1) {
        usage();
        return false;
    }

    if (argc == 1 && (std::string_view(argv[0]) == "--help" || std::string_view(argv[0]) == "-h")) {
        usage();
        config.helpRequested = true;
        return true;
    }

    config.ilFile = argv[0];

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--") {
            for (int j = i + 1; j < argc; ++j)
                config.programArgs.emplace_back(argv[j]);
            break;
        } else if (arg == "--break") {
            if (i + 1 >= argc) {
                usage();
                return false;
            }
            std::string spec = argv[++i];
            if (ilc::isSrcBreakSpec(spec)) {
                auto pos = spec.rfind(':');
                std::string file = trimWhitespace(spec.substr(0, pos));
                const std::string lineToken = trimWhitespace(spec.substr(pos + 1));
                uint32_t line = 0;
                if (!tryParseLineNumber(lineToken, line)) {
                    reportInvalidLineNumber(lineToken, spec, "--break");
                    return false;
                }
                config.breakSrcLines.push_back({std::move(file), line});
            } else {
                std::string trimmedSpec = trimWhitespace(spec);
                bool parsedAsSrcBreak = false;
                auto pos = trimmedSpec.rfind(':');
                if (pos != std::string::npos) {
                    std::string file = trimWhitespace(trimmedSpec.substr(0, pos));
                    const std::string lineToken = trimWhitespace(trimmedSpec.substr(pos + 1));
                    if (!lineToken.empty()) {
                        uint32_t line = 0;
                        if (!tryParseLineNumber(lineToken, line)) {
                            // Fall through to label handling when the suffix is not a
                            // valid line number.
                        } else if (file.empty()) {
                            reportInvalidLineNumber(lineToken, spec, "--break");
                            return false;
                        } else {
                            config.breakSrcLines.push_back({std::move(file), line});
                            parsedAsSrcBreak = true;
                        }
                    }
                }

                if (parsedAsSrcBreak) {
                    continue;
                }

                std::string label = std::move(trimmedSpec);
                while (!label.empty() && label.back() == ':') {
                    label.pop_back();
                }
                if (label.empty()) {
                    std::cerr << "error: --break label must not be empty\n";
                    usage();
                    return false;
                }
                config.breakLabels.push_back(std::move(label));
            }
        } else if (arg == "--break-src") {
            if (i + 1 >= argc) {
                usage();
                return false;
            }
            std::string spec = argv[++i];
            auto pos = spec.rfind(':');
            if (pos != std::string::npos) {
                std::string file = trimWhitespace(spec.substr(0, pos));
                const std::string lineToken = trimWhitespace(spec.substr(pos + 1));
                if (file.empty()) {
                    reportInvalidLineNumber(lineToken, spec, "--break-src");
                    return false;
                }
                uint32_t line = 0;
                if (!tryParseLineNumber(lineToken, line)) {
                    reportInvalidLineNumber(lineToken, spec, "--break-src");
                    return false;
                }
                config.breakSrcLines.push_back({std::move(file), line});
            } else {
                reportInvalidLineNumber("", spec, "--break-src");
                return false;
            }
        } else if (arg == "--debug-cmds") {
            if (i + 1 >= argc) {
                usage();
                return false;
            }
            config.debugScriptPath = argv[++i];
        } else if (arg == "--step") {
            config.stepFlag = true;
        } else if (arg == "--continue") {
            config.continueFlag = true;
        } else if (arg == "--watch") {
            if (i + 1 >= argc) {
                usage();
                return false;
            }
            config.watchSymbols.emplace_back(argv[++i]);
        } else if (arg == "--count") {
            config.countFlag = true;
        } else if (arg == "--time") {
            config.timeFlag = true;
        } else if (arg == "--bytecode") {
            config.useBytecode = true;
        } else if (arg == "--bc-threaded") {
            config.useBytecode = true;
            config.useBytecodeThreaded = true;
        } else {
            switch (ilc::parseSharedOption(i, argc, argv, config.sharedOpts)) {
                case ilc::SharedOptionParseResult::Parsed:
                    continue;
                case ilc::SharedOptionParseResult::Error:
                    if (!ilc::lastSharedOptionError().empty())
                        std::cerr << "error: " << ilc::lastSharedOptionError() << "\n";
                    usage();
                    return false;
                case ilc::SharedOptionParseResult::NotMatched:
                    usage();
                    return false;
            }
        }
    }

    if (config.continueFlag) {
        config.stepFlag = false;
    }

    config.boundsChecksRequested = config.sharedOpts.boundsChecksSpecified;

    // --profile enables both instruction counting and wall-clock timing.
    if (config.sharedOpts.profile) {
        config.countFlag = true;
        config.timeFlag = true;
    }

    return true;
}

/// @brief Configure debugger state based on parsed CLI options.
/// @details Resets the debugger when @c --continue is present; otherwise interns
///          label and source breakpoints, registers watch expressions, and
///          materialises a @ref vm::DebugScript for scripted or step-driven
///          execution.  Stepping without an existing script creates a transient
///          script that requests a single step.
/// @param config Parsed `run` configuration describing debugger behaviour.
/// @param dbg Debug controller to update.
/// @param script Optional debug script owned by the caller; allocated or
///        cleared depending on CLI flags.
void configureDebugger(const RunILConfig &config,
                       vm::DebugCtrl &dbg,
                       std::unique_ptr<vm::DebugScript> &script) {
    if (config.continueFlag) {
        dbg = vm::DebugCtrl();
        script.reset();
        return;
    }

    for (const auto &label : config.breakLabels) {
        auto sym = dbg.internLabel(label.c_str());
        dbg.addBreak(sym);
    }
    for (const auto &src : config.breakSrcLines) {
        dbg.addBreakSrcLine(src.file, src.line);
    }
    for (const auto &watch : config.watchSymbols) {
        dbg.addWatch(watch);
    }

    if (!config.debugScriptPath.empty()) {
        script = std::make_unique<vm::DebugScript>(config.debugScriptPath);
    }
    if (config.stepFlag) {
        if (!script) {
            script = std::make_unique<vm::DebugScript>();
        }
        script->addStep(1);
    }
}

/// @brief Load, verify, and execute the requested IL module.
/// @details Sets up diagnostics infrastructure, applies tracing configuration,
///          loads the module from disk, and runs verification before launching
///          the VM.  When execution finishes optional instruction counts and
///          timing summaries are printed, and any trap messages are surfaced on
///          stderr.  Returns a non-zero status when any phase fails.
/// @param config Fully populated configuration for the run.
/// @param sm Source manager used to register the IL file and render diagnostics.
/// @return Process-style exit status; zero indicates success.
int executeRunIL(const RunILConfig &config, il::support::SourceManager &sm) {
    if (config.boundsChecksRequested) {
        std::cerr << "error: --bounds-checks is not supported when running existing IL modules;";
        std::cerr << " recompile the source with the desired bounds-check setting and rerun.\n";
        return 1;
    }
    if (config.useBytecode && (!config.breakLabels.empty() || !config.breakSrcLines.empty() ||
                               !config.watchSymbols.empty() || !config.debugScriptPath.empty() ||
                               config.stepFlag || config.continueFlag)) {
        std::cerr << "error: debugger flags are not supported with --bytecode/--bc-threaded\n";
        return 1;
    }

    const uint32_t fileId = sm.addFile(config.ilFile);
    if (fileId == 0) {
        ilc::printDiagnostic(
            il::support::makeErrorWithCode(
                {}, "V-SRC-FILE-ID", std::string{il::support::kSourceManagerFileIdOverflowMessage}),
            std::cerr,
            &sm,
            config.sharedOpts.diagnosticFormat);
        return 1;
    }

    vm::TraceConfig traceCfg = config.sharedOpts.trace;
    traceCfg.sm = &sm;

    vm::DebugCtrl dbg = config.debugCtrl;
    dbg.setSourceManager(&sm);

    core::Module m;
    auto load = il::tools::common::loadModuleFromFile(
        config.ilFile, m, std::cerr, "unable to open ", false);
    if (!load.succeeded()) {
        if (load.diag)
            ilc::printDiagnostic(*load.diag, std::cerr, &sm, config.sharedOpts.diagnosticFormat);
        else
            il::tools::common::printLoadResult(load, std::cerr, &sm);
        return 1;
    }

    auto verify = il::tools::common::verifyModuleResult(m);
    if (!verify.succeeded()) {
        if (verify.diag)
            ilc::printDiagnostic(*verify.diag, std::cerr, &sm, config.sharedOpts.diagnosticFormat);
        else
            il::tools::common::printLoadResult(verify, std::cerr, &sm);
        return 1;
    }

    std::optional<zanna::tools::ScopedStdinRedirect> stdinRedirect;
    if (!config.sharedOpts.stdinPath.empty()) {
        stdinRedirect.emplace(config.sharedOpts.stdinPath);
        if (!stdinRedirect->ok()) {
            ilc::printDiagnostic(il::support::Diagnostic{il::support::Severity::Error,
                                                         "unable to open stdin file: " +
                                                             stdinRedirect->errorMessage(),
                                                         {},
                                                         {}},
                                 std::cerr,
                                 &sm,
                                 config.sharedOpts.diagnosticFormat);
            return 1;
        }
    }

    if (config.stepFlag) {
        /// @brief Locate the IL entry function used for the initial step breakpoint.
        /// @param f Function to inspect.
        /// @return `true` when the function is named `main`.
        auto it = std::find_if(m.functions.begin(), m.functions.end(), [](const core::Function &f) {
            return f.name == "main";
        });
        if (it != m.functions.end() && !it->blocks.empty()) {
            auto sym = dbg.internLabel(it->blocks.front().label);
            dbg.addBreak(sym);
        }
    }

    // Use bytecode VM if requested
    if (config.useBytecode) {
        // Compile IL to bytecode
        zanna::bytecode::BytecodeCompiler compiler;
        auto compiled = compiler.compileChecked(m, &sm, true);
        if (!compiled) {
            ilc::printDiagnostic(
                compiled.error(), std::cerr, &sm, config.sharedOpts.diagnosticFormat);
            return 1;
        }
        zanna::bytecode::BytecodeModule bcModule = std::move(compiled.value());

        RuntimeArgsScope runtimeArgs(config.programArgs);

        // Create and configure VM
        zanna::bytecode::BytecodeVM vm;
        vm.setThreadedDispatch(config.useBytecodeThreaded);
        vm.setRuntimeBridgeEnabled(true);
        vm.setMaxInstructions(config.sharedOpts.maxSteps);
        vm.load(&bcModule);

        std::chrono::steady_clock::time_point start;
        if (config.timeFlag) {
            start = std::chrono::steady_clock::now();
        }

        // Execute main function
        zanna::bytecode::BCSlot result = vm.exec("main", {});

        std::chrono::steady_clock::time_point end;
        if (config.timeFlag) {
            end = std::chrono::steady_clock::now();
        }

        int rc = 0;
        if (vm.state() == zanna::bytecode::VMState::Trapped) {
            std::cerr << vm.trapMessage() << "\n";
            rc = 1;
        } else {
            const auto intMin = static_cast<int64_t>(std::numeric_limits<int>::min());
            const auto intMax = static_cast<int64_t>(std::numeric_limits<int>::max());
            if (result.i64 < intMin || result.i64 > intMax) {
                std::cerr << "zanna -run: program return value " << result.i64
                          << " outside host int range [" << intMin << ", " << intMax << "]\n";
                rc = 1;
            } else {
                rc = static_cast<int>(result.i64);
            }
        }

        if (config.countFlag || config.timeFlag) {
            std::cerr << "[SUMMARY]";
            if (config.countFlag) {
                std::cerr << " instr=" << vm.instrCount();
            }
            if (config.timeFlag) {
                double ms = std::chrono::duration<double, std::milli>(end - start).count();
                std::cerr << " time_ms=" << ms;
            }
            std::cerr << "\n";
        }

        return rc;
    }

    // Standard IL VM execution path
    vm::RunConfig runCfg;
    runCfg.trace = traceCfg;
    runCfg.maxSteps = config.sharedOpts.maxSteps;
    runCfg.debug = std::move(dbg);
    runCfg.debugScript = config.debugScript ? config.debugScript.get() : nullptr;
    runCfg.programArgs = config.programArgs;

    vm::Runner runner(m, std::move(runCfg));

    std::chrono::steady_clock::time_point start;
    if (config.timeFlag) {
        start = std::chrono::steady_clock::now();
    }
    const int64_t runResult = runner.run();
    int rc = 0;
    const auto intMin = std::numeric_limits<int>::min();
    const auto intMax = std::numeric_limits<int>::max();
    if (runResult < intMin || runResult > intMax) {
        std::cerr << "zanna -run: program return value " << runResult << " outside host int range ["
                  << intMin << ", " << intMax << "]\n";
        rc = 1;
    } else {
        rc = static_cast<int>(runResult);
    }
    const auto trapMessage = runner.lastTrapMessage();
    if (trapMessage) {
        if (!trapMessage->empty()) {
            std::cerr << *trapMessage;
            if (trapMessage->back() != '\n') {
                std::cerr << '\n';
            }
        }
        if (rc == 0) {
            rc = 1;
        }
    }
    std::chrono::steady_clock::time_point end;
    if (config.timeFlag) {
        end = std::chrono::steady_clock::now();
    }

    if (config.countFlag || config.timeFlag) {
        std::cerr << "[SUMMARY]";
        if (config.countFlag) {
            std::cerr << " instr=" << runner.instructionCount();
        }
        if (config.timeFlag) {
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            std::cerr << " time_ms=" << ms;
        }
        std::cerr << "\n";
    }

    return rc;
}

} // namespace

/// @brief Execute the `run` subcommand with a caller-provided source manager.
/// @details Parses arguments, configures debugger state, and then dispatches to
///          @ref executeRunIL using the supplied @p sm.  Tests use this helper
///          to preconfigure overflow conditions deterministically.
/// @param argc Number of subcommand arguments (excluding the subcommand).
/// @param argv Argument vector beginning with the IL file path.
/// @param sm Source manager instance prepared by the caller.
/// @return Zero on success; non-zero when parsing or execution fails.
int cmdRunILWithSourceManager(int argc, char **argv, il::support::SourceManager &sm) {
    RunILConfig config;
    if (!parseRunILArgs(argc, argv, config)) {
        return 1;
    }
    if (config.helpRequested) {
        return 0;
    }

    try {
        configureDebugger(config, config.debugCtrl, config.debugScript);
    } catch (const std::exception &ex) {
        std::cerr << "error: failed to configure debugger: " << ex.what() << "\n";
        return 1;
    }
    return executeRunIL(config, sm);
}

/// @brief Execute the `run` subcommand end-to-end.
/// @details Parses arguments, configures debugger state, and then dispatches to
///          @ref executeRunIL.  Parsing failures are surfaced via a non-zero
///          return code so the outer driver can present diagnostics.
/// @param argc Number of subcommand arguments (excluding the subcommand).
/// @param argv Argument vector beginning with the IL file path.
/// @return Zero on success; non-zero when parsing or execution fails.
int cmdRunIL(int argc, char **argv) {
    il::support::SourceManager sm;
    return cmdRunILWithSourceManager(argc, argv, sm);
}
