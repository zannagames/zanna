//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_codegen_arm64.cpp
/// @brief Implements the thin CLI adapter for the reusable AArch64 code-generation pipeline.
///
/// Parsing validates all paths, optimization levels, stack sizes, assembler/linker modes, target
/// platforms, debug-line policy, asset blobs, and extra objects before constructing the pipeline.
//
//===----------------------------------------------------------------------===//

#include "cmd_codegen_arm64.hpp"

#include "codegen/aarch64/CodegenPipeline.hpp"
#include "tools/common/ArgvView.hpp"

#include <charconv>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace zanna::tools::ilc {
namespace {

using zanna::tools::ArgvView;

constexpr std::string_view kUsage =
    "Usage: zanna codegen arm64 <file.il> [-S <file.s>] [-o <a.out>] [-run-native]\n"
    "       [--stack-size=SIZE]\n"
    "       [--dump-mir-before-ra] [--dump-mir-after-ra] [--dump-mir-full]\n"
    "       [--native-asm|--system-asm] [--native-link|--system-link(deprecated)]\n"
    "       [--target-host|--target-darwin|--target-linux|--target-windows] [--debug-lines]\n"
    "       [--fast-link|--no-fast-link]\n"
    "       [--asset-blob <file.zpak>] [--extra-obj <file.o>]\n"
    "       [-O0|-O1|-O2|-O3]\n"
    "       [--skip-il-optimization] [--time-passes] [--verify-mir]\n";
/// @brief Minimum accepted native stack reserve for generated executables.
constexpr std::size_t kMinStackSize = 4096;

using Pipeline = zanna::codegen::aarch64::CodegenPipeline;

/// @brief Result of parsing the arm64 codegen arguments.
/// @details @c opts holds the pipeline options on success; @c diagnostics holds
///          usage/error text when parsing fails (and @c opts is left empty).
struct ParseOutcome {
    std::optional<Pipeline::Options> opts{}; ///< Parsed options, or empty on failure.
    std::string diagnostics{};               ///< Usage/error text when parsing fails.
};

/// @brief Parse @p text as a base-10 int within [minValue, maxValue].
/// @param text Candidate decimal spelling.
/// @param minValue Inclusive lower bound.
/// @param maxValue Inclusive upper bound.
/// @param out Receives the parsed value only on success.
/// @return true on a full, in-range parse; false otherwise (out left unset).
bool parseIntInRange(std::string_view text, int minValue, int maxValue, int &out) {
    int value = 0;
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value < minValue || value > maxValue)
        return false;
    out = value;
    return true;
}

/// @brief Parse @p text as a base-10 size_t value.
/// @param text Candidate decimal spelling.
/// @param out Receives the parsed size only on success.
/// @return true on a full, in-range parse; false otherwise.
bool parseSize(std::string_view text, std::size_t &out) {
    unsigned long long value = 0;
    const auto *begin = text.data();
    const auto *end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end ||
        value > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

/// @brief Parse the arm64 codegen command-line arguments into pipeline options.
/// @details The first argument is the input IL path; remaining flags select
///          output mode (-S/-o), optimization level, and native execution. On any
///          error (including an empty argument list) the returned outcome carries
///          diagnostics text and no options.
/// @param args Non-owning view over the subcommand's arguments.
/// @return A ParseOutcome with options on success or diagnostics on failure.
ParseOutcome parseArgs(const ArgvView &args) {
    ParseOutcome outcome{};
    if (args.empty()) {
        outcome.diagnostics = std::string{kUsage};
        return outcome;
    }
    if (args.front().empty() || args.front()[0] == '-') {
        outcome.diagnostics =
            "error: missing input IL file before codegen options\n" + std::string{kUsage};
        return outcome;
    }

    Pipeline::Options opts{};
    opts.input_il_path = std::string(args.front());

    std::ostringstream diag;
    for (int i = 1; i < args.argc; ++i) {
        const std::string_view tok = args.at(i);
        if (tok == "-S") {
            if (i + 1 >= args.argc) {
                diag << "error: -S requires an output path\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.emit_asm = true;
            opts.output_asm_path = std::string(args.at(++i));
            continue;
        }
        if (tok == "-o") {
            if (i + 1 >= args.argc) {
                diag << "error: -o requires an output path\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.output_obj_path = std::string(args.at(++i));
            continue;
        }
        if (tok == "-run-native") {
            opts.run_native = true;
            continue;
        }
        if (tok.substr(0, 13) == "--stack-size=") {
            const std::string_view sizeText = tok.substr(13);
            std::size_t size = 0;
            if (!parseSize(sizeText, size) || size < kMinStackSize) {
                diag << "error: invalid --stack-size value: " << sizeText << "\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.stack_size = size;
            continue;
        }
        if (tok == "--dump-mir-before-ra") {
            opts.dump_mir_before_ra = true;
            continue;
        }
        if (tok == "--dump-mir-after-ra") {
            opts.dump_mir_after_ra = true;
            continue;
        }
        if (tok == "--dump-mir-full") {
            opts.dump_mir_before_ra = true;
            opts.dump_mir_after_ra = true;
            continue;
        }
        if (tok == "-O" || tok == "--optimize") {
            if (i + 1 >= args.argc) {
                diag << "error: -O requires a level (0, 1, 2, or 3)\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            int level = 0;
            const std::string_view value = args.at(++i);
            if (!parseIntInRange(value, 0, 3, level)) {
                diag << "error: invalid -O level: " << value << "\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.optimize = level;
            continue;
        }
        if (tok.size() == 3 && tok[0] == '-' && tok[1] == 'O' && tok[2] >= '0' && tok[2] <= '3') {
            opts.optimize = tok[2] - '0';
            continue;
        }
        if (tok == "--skip-il-optimization") {
            opts.skip_il_optimization = true;
            continue;
        }
        if (tok == "--time-passes") {
            opts.time_passes = true;
            continue;
        }
        if (tok == "--verify-mir") {
            opts.verify_mir = true;
            continue;
        }
        if (tok == "--native-asm") {
            opts.assembler_mode = Pipeline::AssemblerMode::Native;
            continue;
        }
        if (tok == "--system-asm") {
            opts.assembler_mode = Pipeline::AssemblerMode::System;
            continue;
        }
        if (tok == "--native-link") {
            opts.link_mode = Pipeline::LinkMode::Native;
            continue;
        }
        if (tok == "--system-link") {
            opts.link_mode = Pipeline::LinkMode::System;
            continue;
        }
        if (tok == "--target-host") {
            opts.target_platform = Pipeline::TargetPlatform::Host;
            continue;
        }
        if (tok == "--target-darwin") {
            opts.target_platform = Pipeline::TargetPlatform::Darwin;
            continue;
        }
        if (tok == "--target-linux") {
            opts.target_platform = Pipeline::TargetPlatform::Linux;
            continue;
        }
        if (tok == "--target-windows") {
            opts.target_platform = Pipeline::TargetPlatform::Windows;
            continue;
        }
        if (tok == "--debug-lines") {
            opts.emit_debug_lines = true;
            continue;
        }
        if (tok == "--no-debug-lines") {
            opts.emit_debug_lines = false;
            continue;
        }
        if (tok == "--fast-link") {
            opts.fast_link = true;
            continue;
        }
        if (tok == "--no-fast-link") {
            opts.fast_link = false;
            continue;
        }

        if (tok == "--asset-blob") {
            if (i + 1 >= args.argc) {
                diag << "error: --asset-blob requires a path\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.asset_blob_path = std::string(args.at(++i));
            continue;
        }
        if (tok == "--extra-obj") {
            if (i + 1 >= args.argc) {
                diag << "error: --extra-obj requires a path\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.extra_objects.push_back(std::string(args.at(++i)));
            continue;
        }

        diag << "error: unknown flag '" << tok << "'\n" << kUsage;
        outcome.diagnostics = diag.str();
        return outcome;
    }

    outcome.opts = std::move(opts);
    return outcome;
}

} // namespace

/// @brief Parse and execute the AArch64 native-codegen pipeline.
/// @param argc Number of arguments following the architecture selector.
/// @param argv Argument vector beginning with the IL path or help flag.
/// @return Zero on success, one on help/usage parsing failure, or pipeline exit/error code.
int cmd_codegen_arm64(int argc, char **argv) {
    const ArgvView args{argc, argv};
    if (args.empty()) {
        std::cerr << kUsage;
        return 1;
    }
    if (args.front() == "--help" || args.front() == "-h") {
        std::cout << kUsage;
        return 0;
    }
    const ParseOutcome parsed = parseArgs(args);
    if (!parsed.opts.has_value()) {
        if (!parsed.diagnostics.empty())
            std::cerr << parsed.diagnostics;
        return 1;
    }

    try {
        Pipeline pipeline(*parsed.opts);
        const zanna::codegen::aarch64::PipelineResult result = pipeline.run();
        if (!result.stdout_text.empty())
            std::cout << result.stdout_text;
        if (!result.stderr_text.empty())
            std::cerr << result.stderr_text;
        return result.exit_code;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}

} // namespace zanna::tools::ilc
