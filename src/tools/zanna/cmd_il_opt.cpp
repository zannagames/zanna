//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_il_opt.cpp
/// @brief Implements the optimisation pipeline entry point for `ilc`.
///
/// The driver loads and verifies a module, resolves registered or explicit pass pipelines,
/// configures pass instrumentation and optional bisection, emits statistics, and atomically writes
/// canonical optimized IL.

#include "cli.hpp"
#include "il/transform/ConstFold.hpp"
#include "il/transform/DCE.hpp"
#include "il/transform/Mem2Reg.hpp"
#include "il/transform/PassManager.hpp"
#include "il/transform/Peephole.hpp"
#include "tools/common/module_loader.hpp"
#include "tools/common/packaging/PkgUtils.hpp"
#include "zanna/il/IO.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace il;

namespace {

/// @brief Coarse size metrics for an IL module (used to report opt before/after).
struct ModuleSize {
    std::size_t blocks = 0;       ///< Total basic blocks across all functions.
    std::size_t instructions = 0; ///< Total instructions across all blocks.
};

/// @brief Count the total basic blocks and instructions in @p module.
/// @param module Module whose functions and blocks are traversed.
/// @return Aggregate block and instruction counts.
ModuleSize computeModuleSize(const core::Module &module) {
    ModuleSize size{};
    for (const auto &fn : module.functions) {
        size.blocks += fn.blocks.size();
        for (const auto &block : fn.blocks)
            size.instructions += block.instructions.size();
    }
    return size;
}

/// @brief Print usage for the `zanna il-opt` subcommand to stderr.
void ilOptUsage() {
    std::cerr << "Usage: zanna il-opt <in.il> -o <out.il> [options]\n"
              << "\n"
              << "Options:\n"
              << "  --passes p1,p2       Run explicit comma-separated passes\n"
              << "  --pipeline NAME      Run registered pipeline O0, O1, O2, or rehab-*\n"
              << "  --no-mem2reg         Remove mem2reg from the selected pipeline\n"
              << "  --mem2reg-stats      Print mem2reg statistics\n"
              << "  -print-before        Print IL before each pass\n"
              << "  -print-after         Print IL after each pass\n"
              << "  --verify-each        Verify between passes\n"
              << "  --pass-stats         Print pass statistics\n"
              << "  --bisect-pipeline    Run and report every pipeline prefix\n"
              << "  -h, --help           Show this help\n";
}

} // namespace

/// @brief Optimize an IL module using selected passes.
///
/// @details Execution steps:
///          1. Parse subcommand options, requiring an output file via `-o` and
///             optionally collecting a custom `--passes` pipeline. Flags such as
///             `--no-mem2reg` and `--mem2reg-stats` tweak the default pipeline.
///          2. Load the input module from disk using
///             @ref il::tools::common::loadModuleFromFile.
///          3. Instantiate the IL pass manager (pipelines O0/O1/O2 pre-registered)
///             and configure instrumentation hooks for printing/verifying.
///          4. Execute either a named pipeline or an explicit pass list and
///             write the canonicalized IL to @p outFile.
///          The function returns zero on success or one when argument parsing,
///          file I/O, or pass execution fails.
///
/// @param argc Number of subcommand arguments (excluding `il-opt`).
/// @param argv Argument list starting with the input IL file.
/// @return Exit status code (zero on success, one on failure).
int cmdILOpt(int argc, char **argv) {
    if (argc == 1 && (std::string_view(argv[0]) == "--help" || std::string_view(argv[0]) == "-h")) {
        ilOptUsage();
        return 0;
    }
    if (argc < 3) {
        ilOptUsage();
        return 1;
    }
    std::string inFile = argv[0];
    std::string outFile;
    std::vector<std::string> passList;
    bool passesExplicit = false;
    bool noMem2Reg = false;
    bool mem2regStats = false;
    bool printBefore = false;
    bool printAfter = false;
    bool verifyEach = false;
    bool passStats = false;
    bool bisectPipeline = false;
    std::string pipelineName;
    /// @brief Trim ASCII whitespace from both ends of a pass-list token.
    /// @param token Token view to trim.
    /// @return Subview spanning the non-whitespace content.
    auto trimToken = [](std::string_view token) {
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
            token.remove_prefix(1);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
            token.remove_suffix(1);
        return token;
    };
    /// @brief Parse a comma-separated explicit optimization-pass list.
    /// @param passes Pass-list text to split and validate.
    /// @return `true` when every token is nonempty.
    auto parsePassList = [&](std::string_view passes) -> bool {
        size_t pos = 0;
        passesExplicit = true;
        while (pos != std::string::npos) {
            size_t comma = passes.find(',', pos);
            std::string_view token = trimToken(passes.substr(pos, comma - pos));
            if (token.empty()) {
                ilOptUsage();
                return false;
            }
            passList.emplace_back(token);
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outFile = argv[++i];
        } else if (arg == "--passes" && i + 1 < argc) {
            if (!parsePassList(argv[++i]))
                return 1;
        } else if (arg.rfind("--passes=", 0) == 0) {
            if (!parsePassList(std::string(arg.substr(9))))
                return 1;
        } else if (arg == "--no-mem2reg") {
            noMem2Reg = true;
        } else if (arg == "--pipeline" && i + 1 < argc) {
            pipelineName = argv[++i];
        } else if (arg == "--pipeline") {
            std::cerr << "error: --pipeline requires a name\n";
            ilOptUsage();
            return 1;
        } else if (arg == "--mem2reg-stats") {
            mem2regStats = true;
        } else if (arg == "--pass-stats") {
            passStats = true;
        } else if (arg == "--bisect-pipeline") {
            bisectPipeline = true;
        } else if (arg == "-print-before") {
            printBefore = true;
        } else if (arg == "-print-after") {
            printAfter = true;
        } else if (arg == "--verify-each" || arg == "-verify-each") {
            verifyEach = true;
        } else if (arg == "--help" || arg == "-h") {
            ilOptUsage();
            return 0;
        } else {
            ilOptUsage();
            return 1;
        }
    }
    if (outFile.empty()) {
        ilOptUsage();
        return 1;
    }
    core::Module m;
    auto load = il::tools::common::loadModuleFromFile(inFile, m, std::cerr);
    if (!load.succeeded()) {
        return 1;
    }
    transform::PassManager pm;
    pm.setInstrumentationStream(std::cerr);
    pm.setPrintBeforeEach(printBefore);
    pm.setPrintAfterEach(printAfter);
    pm.setReportPassStatistics(passStats);
    if (verifyEach)
        pm.setVerifyBetweenPasses(true);

    transform::PassManager::Pipeline selectedPipeline;
    /// @brief Look up a registered optimization pipeline by name.
    /// @param name Pipeline name to resolve.
    /// @return Pointer to the registered pipeline, or `nullptr` when absent.
    auto resolvePipeline =
        [&](const std::string &name) -> const transform::PassManager::Pipeline * {
        return pm.getPipeline(name);
    };

    if (!pipelineName.empty()) {
        const auto *pipeline = resolvePipeline(pipelineName);
        if (!pipeline) {
            std::string upper = pipelineName;
            /// @brief Fold one pipeline-name byte to uppercase for fallback lookup.
            /// @param c Byte to normalize.
            /// @return Uppercase representation converted back to `char`.
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (upper != pipelineName)
                pipeline = resolvePipeline(upper);
        }
        if (!pipeline) {
            std::cerr << "unknown pipeline '" << pipelineName
                      << "' (use a registered pipeline such as O0/O1/O2 or rehab-*)\n";
            return 1;
        }
        selectedPipeline = *pipeline;
    }

    if (passesExplicit) {
        selectedPipeline = passList;
    } else if (selectedPipeline.empty()) {
        const auto *pipeline = resolvePipeline("O1");
        if (!pipeline) {
            std::cerr << "unknown pipeline 'O1'\n";
            return 1;
        }
        selectedPipeline = *pipeline;
    }

    if (selectedPipeline.empty()) {
        std::cerr << "no passes selected\n";
        return 1;
    }

    if (noMem2Reg) {
        selectedPipeline.erase(
            std::remove(selectedPipeline.begin(), selectedPipeline.end(), "mem2reg"),
            selectedPipeline.end());
    }

    for (const auto &passId : selectedPipeline) {
        if (!pm.passes().lookup(passId)) {
            std::cerr << "unknown pass '" << passId << "'\n";
            return 1;
        }
    }

    if (mem2regStats) {
        if (std::find(selectedPipeline.begin(), selectedPipeline.end(), "mem2reg") ==
            selectedPipeline.end()) {
            std::cerr << "error: --mem2reg-stats requires mem2reg in the selected pipeline\n";
            return 1;
        }
    }

    zanna::passes::Mem2RegStats mem2regStatsData;
    if (mem2regStats) {
        /// @brief Runs mem2reg while accumulating command-line statistics.
        /// @param module Module to transform.
        /// @return No preserved analyses after transformation.
        pm.registerModulePass("mem2reg",
                              [&mem2regStatsData](core::Module &module,
                                                   transform::AnalysisManager &) {
                                  zanna::passes::mem2reg(module, &mem2regStatsData);
                                  return transform::PreservedAnalyses::none();
                              });
    }

    if (bisectPipeline) {
        for (std::size_t prefixLen = 1; prefixLen <= selectedPipeline.size(); ++prefixLen) {
            core::Module probe = m;
            transform::PassManager::Pipeline prefix(selectedPipeline.begin(),
                                                    selectedPipeline.begin() +
                                                        static_cast<std::ptrdiff_t>(prefixLen));
            if (!pm.run(probe, prefix)) {
                std::cerr << "[bisect] failed after " << prefixLen << " pass(es), last='"
                          << prefix.back() << "'\n";
                return 1;
            }
            const ModuleSize size = computeModuleSize(probe);
            std::cerr << "[bisect] prefix " << prefixLen << "/" << selectedPipeline.size()
                      << " last='" << prefix.back() << "' bb=" << size.blocks
                      << " inst=" << size.instructions << "\n";
        }
    }

    if (!pm.run(m, selectedPipeline))
        return 1;
    if (!verifyEach && !il::tools::common::verifyModule(m, std::cerr, nullptr)) {
        std::cerr << "error: optimized module failed final verification\n";
        return 1;
    }
    if (mem2regStats) {
        std::cout << "mem2reg: promoted " << mem2regStatsData.promotedVars
                  << ", removed loads " << mem2regStatsData.removedLoads << ", removed stores "
                  << mem2regStatsData.removedStores << "\n";
    }
    std::ostringstream output;
    io::Serializer::write(m, output, io::Serializer::Mode::Canonical);
    if (!output) {
        std::cerr << "error: failed to serialize optimized IL\n";
        return 1;
    }
    try {
        zanna::pkg::writeTextFileAtomic(outFile, output.str());
    } catch (const std::exception &ex) {
        std::cerr << "error: failed to write IL to " << outFile << ": " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
