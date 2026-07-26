//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_bench.cpp
/// @brief Implements the @c zanna bench subcommand across VM dispatch strategies.
///
/// The command parses a bounded benchmark configuration, loads and verifies each IL module, runs
/// selected standard and bytecode VM strategies, and reports instruction count, elapsed time, and
/// throughput in parse-friendly text or JSON. A default step watchdog prevents accidental hangs.

#include "bytecode/BytecodeCompiler.hpp"
#include "bytecode/BytecodeVM.hpp"
#include "cli.hpp"
#include "il/core/Module.hpp"
#include "runtime/core/rt_args.h"
#include "support/diag_expected.hpp"
#include "support/source_manager.hpp"
#include "tools/common/ScopedProcess.hpp"
#include "tools/common/module_loader.hpp"
#include "zanna/vm/VM.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace il;

namespace {

/// @brief Default step budget applied when `--max-steps` is not given, so benchmarking an IL file
///        with an infinite loop terminates (both VMs trap on the limit) instead of hanging forever.
///        Generous enough for realistic throughput benchmarks; pass `--max-steps 0` for unlimited.
constexpr uint64_t kDefaultBenchMaxSteps = 1'000'000'000;

/// @brief Benchmark configuration parsed from command-line.
struct BenchConfig {
    std::vector<std::string> ilFiles;
    uint32_t iterations = 3;
    uint64_t maxSteps = 0;
    bool maxStepsExplicit = false;
    bool runTable = true;
    bool runSwitch = true;
    bool runThreaded = true;
    bool runBytecodeSwitch = false;
    bool runBytecodeThreaded = false;
    bool jsonOutput = false;
    bool verbose = false;
};

/// @brief Outcome of parsing the bench subcommand arguments.
enum class BenchParseResult {
    Ok,    ///< Arguments parsed successfully into the config.
    Help,  ///< Help was requested; caller should exit successfully.
    Error, ///< Arguments were invalid; usage already printed.
};

/// @brief Result of a single benchmark run.
struct BenchResult {
    std::string file;
    std::string strategy;
    uint64_t instructions = 0;
    double timeMs = 0.0;
    double insnsPerSec = 0.0;
    int64_t returnValue = 0;
    bool success = true;
    bool hitStepBudget = false; ///< True when the run aborted on the step/instruction budget.
    std::string errorMessage;
};

/// @brief Clear runtime argv state for the duration of a benchmark iteration.
/// @details Benchmarks do not forward program arguments. Clearing the runtime bridge before and
///          after each run prevents ambient host-process argv state from changing benchmark
///          behavior or carrying over between VM implementations.
class RuntimeArgsScope {
  public:
    /// @brief Clear ambient runtime argv state before a benchmark iteration.
    RuntimeArgsScope() {
        rt_args_clear();
    }

    /// @brief Clear runtime argv again so no iteration leaks process state.
    ~RuntimeArgsScope() {
        rt_args_clear();
    }

    RuntimeArgsScope(const RuntimeArgsScope &) = delete;
    RuntimeArgsScope &operator=(const RuntimeArgsScope &) = delete;
};

/// @brief Print usage information for the bench subcommand.
/// @param out Destination stream; defaults to standard error.
void benchUsage(std::ostream &out = std::cerr) {
    out << "Usage: zanna bench <file.il> [file2.il ...] [options]\n"
        << "Options:\n"
        << "  -n <N>            Number of iterations (default: 3)\n"
        << "  --max-steps <N>   Maximum interpreter steps before aborting (default: 1e9 watchdog\n"
        << "                    so a hung IL file fails instead of hanging; 0 = unlimited)\n"
        << "  --max-steps=<N>   Inline form of --max-steps\n"
        << "  --table           Run only FnTable dispatch (standard VM)\n"
        << "  --switch          Run only Switch dispatch (standard VM)\n"
        << "  --threaded        Run only Threaded dispatch (standard VM)\n"
        << "  --bc-switch       Run Bytecode VM with switch dispatch\n"
        << "  --bc-threaded     Run Bytecode VM with threaded dispatch\n"
        << "  --bytecode        Run both Bytecode VM dispatch strategies\n"
        << "  --all             Run all dispatch strategies (default if none specified)\n"
        << "  --json            Output results as JSON\n"
        << "  -v, --verbose     Verbose output\n"
        << "\n"
        << "Output format (one line per file/strategy):\n"
        << "  BENCH <file> <strategy> instr=<N> time_ms=<T> insns_per_sec=<R>\n";
}

/// @brief Start an explicit benchmark strategy selection when the first specific flag is seen.
/// @details The default selection runs the standard VM strategies. Once a specific strategy flag
/// is parsed, all strategies are cleared and subsequent specific flags add to that set. `--all`
/// resets this mode so a later specific flag can intentionally narrow the selection again.
/// @param config Mutable strategy selection.
/// @param strategySpecified Whether a specific strategy flag has already reset defaults.
void beginExplicitStrategySelection(BenchConfig &config, bool &strategySpecified) {
    if (strategySpecified)
        return;
    config.runTable = false;
    config.runSwitch = false;
    config.runThreaded = false;
    config.runBytecodeSwitch = false;
    config.runBytecodeThreaded = false;
    strategySpecified = true;
}

/// @brief Parse benchmark command-line arguments.
/// @param argc Number of arguments.
/// @param argv Argument vector.
/// @param config Output configuration.
/// @return True if parsing succeeded, false on error.
bool parseBenchArgs(int argc, char **argv, BenchConfig &config) {
    bool strategySpecified = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            benchUsage(std::cout);
            return false;
        } else if (arg == "-n") {
            if (i + 1 >= argc) {
                std::cerr << "error: -n requires a positive iteration count\n";
                benchUsage();
                return false;
            }
            std::string_view value = argv[++i];
            uint32_t parsed = 0;
            const auto *begin = value.data();
            const auto *end = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(begin, end, parsed);
            if (ec != std::errc{} || ptr != end || parsed == 0) {
                std::cerr << "invalid iteration count: " << value << "\n";
                benchUsage();
                return false;
            }
            config.iterations = parsed;
        } else if (arg == "--max-steps") {
            if (i + 1 >= argc) {
                std::cerr << "error: --max-steps requires a non-negative integer\n";
                benchUsage();
                return false;
            }
            std::string_view value = argv[++i];
            uint64_t parsed = 0;
            const auto *begin = value.data();
            const auto *end = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(begin, end, parsed);
            if (ec != std::errc{} || ptr != end) {
                std::cerr << "invalid --max-steps value: " << value << "\n";
                benchUsage();
                return false;
            }
            config.maxSteps = parsed;
            config.maxStepsExplicit = true;
        } else if (arg.substr(0, 12) == "--max-steps=") {
            std::string_view value = arg.substr(12);
            uint64_t parsed = 0;
            const auto *begin = value.data();
            const auto *end = value.data() + value.size();
            auto [ptr, ec] = std::from_chars(begin, end, parsed);
            if (value.empty() || ec != std::errc{} || ptr != end) {
                std::cerr << "invalid --max-steps value: " << value << "\n";
                benchUsage();
                return false;
            }
            config.maxSteps = parsed;
            config.maxStepsExplicit = true;
        } else if (arg == "--table") {
            beginExplicitStrategySelection(config, strategySpecified);
            config.runTable = true;
        } else if (arg == "--switch") {
            beginExplicitStrategySelection(config, strategySpecified);
            config.runSwitch = true;
        } else if (arg == "--threaded") {
            beginExplicitStrategySelection(config, strategySpecified);
            config.runThreaded = true;
        } else if (arg == "--bc-switch") {
            beginExplicitStrategySelection(config, strategySpecified);
            config.runBytecodeSwitch = true;
        } else if (arg == "--bc-threaded") {
            beginExplicitStrategySelection(config, strategySpecified);
            config.runBytecodeThreaded = true;
        } else if (arg == "--bytecode") {
            beginExplicitStrategySelection(config, strategySpecified);
            config.runBytecodeSwitch = true;
            config.runBytecodeThreaded = true;
        } else if (arg == "--all") {
            config.runTable = true;
            config.runSwitch = true;
            config.runThreaded = true;
            config.runBytecodeSwitch = true;
            config.runBytecodeThreaded = true;
            strategySpecified = false;
        } else if (arg == "--json") {
            config.jsonOutput = true;
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            benchUsage();
            return false;
        } else if (arg.empty()) {
            std::cerr << "error: empty input file argument\n";
            benchUsage();
            return false;
        } else {
            config.ilFiles.push_back(std::string(arg));
        }
    }

    if (config.ilFiles.empty()) {
        std::cerr << "No input files specified\n";
        benchUsage();
        return false;
    }

    // Apply the default watchdog step budget unless the user set one explicitly — including an
    // explicit `--max-steps 0`, which opts back into unlimited execution.
    if (!config.maxStepsExplicit)
        config.maxSteps = kDefaultBenchMaxSteps;

    return true;
}

/// @brief Parse bench args, distinguishing a help request from a parse error.
/// @details Scans for --help/-h first (returning Help), otherwise delegates to
///          parseBenchArgs and maps its bool result to Ok/Error.
/// @param argc Number of benchmark arguments.
/// @param argv Argument vector.
/// @param config Output configuration on successful parsing.
/// @return Help, error, or successful parse outcome.
BenchParseResult parseBenchArgsChecked(int argc, char **argv, BenchConfig &config) {
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            benchUsage(std::cout);
            return BenchParseResult::Help;
        }
    }
    return parseBenchArgs(argc, argv, config) ? BenchParseResult::Ok : BenchParseResult::Error;
}

/// @brief Run a single benchmark iteration.
/// @param mod Module to execute.
/// @param strategy Dispatch strategy name ("table", "switch", "threaded").
/// @param maxSteps Maximum steps (0 = unlimited).
/// @return Benchmark result.
BenchResult runBenchmarkIteration(const core::Module &mod,
                                  const std::string &strategy,
                                  uint64_t maxSteps) {
    BenchResult result;
    result.strategy = strategy;

    RuntimeArgsScope runtimeArgs;

    // Create Runner with minimal configuration
    vm::RunConfig runCfg;
    runCfg.maxSteps = maxSteps;

    try {
        vm::Runner runner(mod, std::move(runCfg));
        auto start = std::chrono::steady_clock::now();
        result.returnValue = runner.run();
        auto end = std::chrono::steady_clock::now();
        result.timeMs =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
        result.instructions = runner.instructionCount();
        // The step budget stops the VM without a trap message, so detect it from the count.
        result.hitStepBudget = (maxSteps > 0 && result.instructions >= maxSteps);
        if (const auto trap = runner.lastTrapMessage()) {
            result.success = false;
            result.errorMessage = *trap;
            return result;
        }
    } catch (const std::exception &ex) {
        result.success = false;
        result.errorMessage = ex.what();
        return result;
    }

    if (result.timeMs > 0) {
        result.insnsPerSec = (result.instructions / result.timeMs) * 1000.0;
    }

    return result;
}

/// @brief Run a single bytecode VM benchmark iteration.
/// @param mod Module to execute.
/// @param bcModule Pre-compiled bytecode module.
/// @param strategy Dispatch strategy name ("bc-switch" or "bc-threaded").
/// @param maxSteps Maximum bytecode instruction count; zero means unlimited.
/// @return Benchmark result.
BenchResult runBytecodeBenchmarkIteration(const core::Module &mod,
                                          const zanna::bytecode::BytecodeModule &bcModule,
                                          const std::string &strategy,
                                          uint64_t maxSteps) {
    BenchResult result;
    result.strategy = strategy;

    bool useThreaded = (strategy == "bc-threaded");
    RuntimeArgsScope runtimeArgs;

    try {
        zanna::bytecode::BytecodeVM vm;
        vm.setThreadedDispatch(useThreaded);
        vm.setRuntimeBridgeEnabled(true);
        vm.setMaxInstructions(maxSteps);
        vm.load(&bcModule);

        auto start = std::chrono::steady_clock::now();
        auto retSlot = vm.exec("main", {});
        auto end = std::chrono::steady_clock::now();
        result.returnValue = retSlot.i64;
        result.instructions = vm.instrCount();
        result.timeMs =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

        // The instruction budget stops the VM without trapping, so detect it from the count.
        result.hitStepBudget = (maxSteps > 0 && result.instructions >= maxSteps);
        if (vm.state() == zanna::bytecode::VMState::Trapped) {
            result.success = false;
            result.errorMessage = vm.trapMessage();
            return result;
        }
    } catch (const std::exception &ex) {
        result.success = false;
        result.errorMessage = ex.what();
        return result;
    }

    if (result.timeMs > 0) {
        result.insnsPerSec = (result.instructions / result.timeMs) * 1000.0;
    }

    return result;
}

/// @brief Compute median of a vector of doubles.
/// @param values Samples copied for in-place sorting.
/// @return Middle sample, mean of the middle pair, or zero for no samples.
double computeMedian(std::vector<double> values) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    size_t n = values.size();
    if (n % 2 == 0)
        return (values[n / 2 - 1] + values[n / 2]) / 2.0;
    return values[n / 2];
}

/// @brief Return the benchmark strategy names selected by @p config.
/// @param config Strategy-selection flags.
/// @return Ordered standard and bytecode strategy names.
std::vector<std::string> selectedBenchStrategies(const BenchConfig &config) {
    std::vector<std::string> strategies;
    if (config.runTable)
        strategies.push_back("table");
    if (config.runSwitch)
        strategies.push_back("switch");
    if (config.runThreaded)
        strategies.push_back("threaded");
    if (config.runBytecodeSwitch)
        strategies.push_back("bc-switch");
    if (config.runBytecodeThreaded)
        strategies.push_back("bc-threaded");
    return strategies;
}

/// @brief Append failed setup results for every selected benchmark strategy.
/// @param file Input file whose setup failed.
/// @param config Benchmark configuration selecting strategies.
/// @param message Failure message to attach to each synthetic result.
/// @param results Output result list to extend.
void appendSetupFailureResults(const std::string &file,
                               const BenchConfig &config,
                               const std::string &message,
                               std::vector<BenchResult> &results) {
    auto strategies = selectedBenchStrategies(config);
    if (strategies.empty())
        strategies.push_back("setup");
    for (const auto &strategy : strategies) {
        BenchResult result;
        result.file = file;
        result.strategy = strategy;
        result.success = false;
        result.errorMessage = message;
        results.push_back(std::move(result));
    }
}

/// @brief Run benchmarks for a single file.
/// @param file Path to IL file.
/// @param config Benchmark configuration.
/// @param results Output vector of results.
/// @return True if benchmarking succeeded.
bool benchmarkFile(const std::string &file,
                   const BenchConfig &config,
                   std::vector<BenchResult> &results) {
    il::support::SourceManager sm;
    core::Module mod;

    auto load =
        il::tools::common::loadModuleFromFile(file, mod, std::cerr, "unable to open ", false);
    if (!load.succeeded()) {
        appendSetupFailureResults(
            file, config, load.diag ? load.diag->message : "failed to load IL module", results);
        if (!config.jsonOutput)
            il::tools::common::printLoadResult(load, std::cerr, &sm);
        return false;
    }

    auto verify = il::tools::common::verifyModuleResult(mod);
    if (!verify.succeeded()) {
        appendSetupFailureResults(file,
                                  config,
                                  verify.diag ? verify.diag->message
                                              : "IL module verification failed",
                                  results);
        if (!config.jsonOutput)
            il::tools::common::printLoadResult(verify, std::cerr, &sm);
        return false;
    }

    // Standard VM strategies
    std::vector<std::string> strategies;
    if (config.runTable)
        strategies.push_back("table");
    if (config.runSwitch)
        strategies.push_back("switch");
    if (config.runThreaded)
        strategies.push_back("threaded");

    for (const auto &strategy : strategies) {
        std::vector<double> times;
        std::vector<double> insnsPerSec;
        uint64_t instructions = 0;
        int64_t returnValue = 0;
        bool allSuccess = true;
        bool hitStepBudget = false;
        std::string errorMessage;

        if (config.verbose) {
            std::cerr << "Running " << file << " with " << strategy << " (" << config.iterations
                      << " iterations)...\n";
        }

        zanna::tools::ScopedEnvVar dispatchOverride("ZANNA_DISPATCH", strategy.c_str());
        if (!dispatchOverride.ok()) {
            BenchResult result;
            result.file = file;
            result.strategy = strategy;
            result.success = false;
            result.errorMessage = dispatchOverride.errorMessage();
            results.push_back(std::move(result));
            continue;
        }

        for (uint32_t iter = 0; iter < config.iterations; ++iter) {
            auto iterResult = runBenchmarkIteration(mod, strategy, config.maxSteps);
            if (iterResult.hitStepBudget) {
                allSuccess = false;
                hitStepBudget = true;
                errorMessage = "exceeded the step budget (" + std::to_string(config.maxSteps) +
                               "); the program may not terminate (pass --max-steps 0 to disable)";
                break;
            }
            if (!iterResult.success) {
                allSuccess = false;
                errorMessage = iterResult.errorMessage;
                break;
            }
            times.push_back(iterResult.timeMs);
            insnsPerSec.push_back(iterResult.insnsPerSec);
            instructions = iterResult.instructions;
            returnValue = iterResult.returnValue;
        }

        BenchResult result;
        result.file = file;
        result.strategy = strategy;
        result.success = allSuccess;
        result.instructions = instructions;
        result.returnValue = returnValue;
        result.errorMessage = std::move(errorMessage);
        result.hitStepBudget = hitStepBudget;

        if (allSuccess && !times.empty()) {
            result.timeMs = computeMedian(times);
            result.insnsPerSec = computeMedian(insnsPerSec);
        }

        results.push_back(result);
        if (hitStepBudget)
            break; // Non-terminating program — it hits the same budget on every strategy.
    }

    // Bytecode VM strategies
    std::vector<std::string> bcStrategies;
    if (config.runBytecodeSwitch)
        bcStrategies.push_back("bc-switch");
    if (config.runBytecodeThreaded)
        bcStrategies.push_back("bc-threaded");

    if (!bcStrategies.empty()) {
        // Compile IL to bytecode once
        zanna::bytecode::BytecodeCompiler compiler;
        auto compiled = compiler.compileChecked(mod, &sm, true);
        if (!compiled) {
            BenchConfig bytecodeOnly = config;
            bytecodeOnly.runTable = false;
            bytecodeOnly.runSwitch = false;
            bytecodeOnly.runThreaded = false;
            appendSetupFailureResults(file, bytecodeOnly, compiled.error().message, results);
            if (!config.jsonOutput)
                il::support::printDiag(compiled.error(), std::cerr, &sm);
            return false;
        }
        zanna::bytecode::BytecodeModule bcModule = std::move(compiled.value());

        for (const auto &strategy : bcStrategies) {
            std::vector<double> times;
            std::vector<double> insnsPerSec;
            uint64_t instructions = 0;
            int64_t returnValue = 0;
            bool allSuccess = true;
            bool hitStepBudget = false;
            std::string errorMessage;

            if (config.verbose) {
                std::cerr << "Running " << file << " with " << strategy << " (" << config.iterations
                          << " iterations)...\n";
            }

            for (uint32_t iter = 0; iter < config.iterations; ++iter) {
                auto iterResult =
                    runBytecodeBenchmarkIteration(mod, bcModule, strategy, config.maxSteps);
                if (iterResult.hitStepBudget) {
                    allSuccess = false;
                    hitStepBudget = true;
                    errorMessage =
                        "exceeded the step budget (" + std::to_string(config.maxSteps) +
                        "); the program may not terminate (pass --max-steps 0 to disable)";
                    break;
                }
                if (!iterResult.success) {
                    allSuccess = false;
                    errorMessage = iterResult.errorMessage;
                    break;
                }
                times.push_back(iterResult.timeMs);
                insnsPerSec.push_back(iterResult.insnsPerSec);
                instructions = iterResult.instructions;
                returnValue = iterResult.returnValue;
            }

            BenchResult result;
            result.file = file;
            result.strategy = strategy;
            result.success = allSuccess;
            result.instructions = instructions;
            result.returnValue = returnValue;
            result.errorMessage = std::move(errorMessage);
            result.hitStepBudget = hitStepBudget;

            if (allSuccess && !times.empty()) {
                result.timeMs = computeMedian(times);
                result.insnsPerSec = computeMedian(insnsPerSec);
            }

            results.push_back(result);
            if (hitStepBudget)
                break; // Non-terminating program — it hits the same budget on every strategy.
        }
    }

    return true;
}

/// @brief Quote one benchmark text field using JSON-style escapes.
/// @details Text output is meant to be parseable even when file paths or error
///          messages contain whitespace, quotes, or control characters. This
///          helper returns a double-quoted token with common C escapes.
/// @param input Raw field contents.
/// @return Double-quoted escaped token.
std::string quoteBenchField(std::string_view input) {
    std::string out = "\"";
    for (char c : input) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    constexpr char hex[] = "0123456789abcdef";
                    out.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                    out.push_back(hex[static_cast<unsigned char>(c) & 0xF]);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

/// @brief Print benchmark results in a parse-friendly text format.
/// @param results Ordered per-file, per-strategy results.
void printTextResults(const std::vector<BenchResult> &results) {
    for (const auto &r : results) {
        if (!r.success) {
            std::cout << "BENCH " << quoteBenchField(r.file) << " " << r.strategy << " FAILED";
            if (!r.errorMessage.empty())
                std::cout << " error=" << quoteBenchField(r.errorMessage);
            std::cout << "\n";
            continue;
        }
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "BENCH " << quoteBenchField(r.file) << " " << r.strategy
                  << " instr=" << r.instructions << " time_ms=" << r.timeMs
                  << " insns_per_sec=" << std::setprecision(0) << r.insnsPerSec << "\n";
    }
}

/// @brief Print results in JSON format.
/// @param results Ordered per-file, per-strategy results.
void printJsonResults(const std::vector<BenchResult> &results) {
    /// @brief Escape one string for inclusion in a JSON string literal.
    /// @param input Unescaped UTF-8 text.
    /// @return Text with JSON control characters and delimiters escaped.
    auto escapeJson = [](std::string_view input) {
        std::string out;
        out.reserve(input.size() + 8);
        for (char c : input) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    } else {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    };

    std::cout << "[\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i];
        std::cout << "  {\n";
        std::cout << "    \"file\": \"" << escapeJson(r.file) << "\",\n";
        std::cout << "    \"strategy\": \"" << escapeJson(r.strategy) << "\",\n";
        std::cout << "    \"success\": " << (r.success ? "true" : "false") << ",\n";
        std::cout << "    \"instructions\": " << r.instructions << ",\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "    \"time_ms\": " << r.timeMs << ",\n";
        std::cout << std::setprecision(0);
        std::cout << "    \"insns_per_sec\": " << r.insnsPerSec << ",\n";
        std::cout << "    \"return_value\": " << r.returnValue << ",\n";
        std::cout << "    \"error\": \"" << escapeJson(r.errorMessage) << "\"\n";
        std::cout << "  }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    std::cout << "]\n";
}

} // namespace

/// @brief Handle the `ilc bench` subcommand.
/// @param argc Number of arguments after "bench".
/// @param argv Argument vector.
/// @return Exit status; 0 on success.
int cmdBench(int argc, char **argv) {
    BenchConfig config;
    switch (parseBenchArgsChecked(argc, argv, config)) {
        case BenchParseResult::Ok:
            break;
        case BenchParseResult::Help:
            return 0;
        case BenchParseResult::Error:
            return 1;
    }

    std::vector<BenchResult> allResults;

    bool hadFailure = false;
    for (const auto &file : config.ilFiles) {
        if (!benchmarkFile(file, config, allResults)) {
            hadFailure = true;
        }
    }

    if (allResults.empty()) {
        std::cerr << "No benchmark results\n";
        return 1;
    }

    if (config.jsonOutput) {
        printJsonResults(allResults);
    } else {
        printTextResults(allResults);
    }

    return hadFailure ? 1 : 0;
}
