//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_codegen_x64.cpp
/// @brief Implements the `ilc codegen x64` command-line entry point.
///
/// Parsing produces deterministic diagnostics, borrows arguments only for the call, validates ABI
/// and target-platform combinations, and delegates artifact ownership and heavy lifting to the
/// reusable x86-64 pipeline.

#include "cmd_codegen_x64.hpp"

#include "codegen/x86_64/CodegenPipeline.hpp"
#include "tools/common/ArgvView.hpp"

#include <charconv>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace zanna::tools::ilc {
namespace {

constexpr std::string_view kUsage =
    "Usage: zanna codegen x64 <file.il> [-S <file.s>] [-o <a.out>] "
    "[-run-native] [--stack-size=SIZE] [--native-asm|--system-asm] "
    "[--native-link|--system-link(deprecated)] [--asset-blob <file.zpak>] "
    "[--extra-obj <file.o>] [--target-host|--target-sysv|--target-win64] "
    "[--target-darwin|--target-linux|--target-windows] [--debug-lines] "
    "[--fast-link|--no-fast-link] [--time-passes] [--verify-mir] "
    "[--skip-il-optimization]\n";
/// @brief Minimum accepted native stack reserve for generated executables.
constexpr std::size_t kMinStackSize = 4096;

// Use shared ArgvView from tools/common
using zanna::tools::ArgvView;

/// @brief Result bundle produced by @ref parseCompileArgs.
/// @details Contains the successfully parsed options or a diagnostic string when
///          parsing failed.
struct ParseOutcome {
    std::optional<zanna::codegen::x64::CodegenPipeline::Options> opts{};
    std::string diagnostics{};
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

/// @brief Validate that the requested x64 ABI and target platform are compatible.
/// @details SysV is valid for Linux/Darwin, Win64 is valid for Windows, and Host remains an
/// automatic selection unless paired with an explicitly incompatible platform by later parsing.
/// @param opts Parsed pipeline options to validate.
/// @param diag Destination for deterministic usage diagnostics.
/// @return @c true when the ABI/platform pair is supported.
bool validateTargetCombination(const zanna::codegen::x64::CodegenPipeline::Options &opts,
                               std::ostream &diag) {
    using ABI = zanna::codegen::x64::CodegenOptions::TargetABI;
    using Platform = zanna::codegen::x64::CodegenOptions::TargetPlatform;
    if (opts.target_abi == ABI::Win64 &&
        (opts.target_platform == Platform::Linux || opts.target_platform == Platform::Darwin)) {
        diag << "error: --target-win64 is only compatible with --target-windows\n" << kUsage;
        return false;
    }
    if (opts.target_abi == ABI::SysV && opts.target_platform == Platform::Windows) {
        diag << "error: --target-sysv is not compatible with --target-windows\n" << kUsage;
        return false;
    }
    return true;
}

/// @brief Decode `ilc codegen x64 compile` arguments into pipeline options.
/// @details Validates positional arguments, handles recognised flags, and emits
///          user-friendly diagnostics on failure.
/// @param args View of the arguments following `codegen x64`.
/// @return Parsed options or diagnostics describing the failure.
ParseOutcome parseCompileArgs(const ArgvView &args) {
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

    zanna::codegen::x64::CodegenPipeline::Options opts{};
    opts.input_il_path = std::string(args.front());
    opts.output_obj_path.clear();
    opts.output_asm_path.clear();

    std::ostringstream diag;
    for (int index = 1; index < args.argc; ++index) {
        const std::string_view arg = args.at(index);
        if (arg == "-S") {
            if (index + 1 >= args.argc) {
                diag << "error: -S requires an output path\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.emit_asm = true;
            opts.output_asm_path = std::string(args.at(++index));
            continue;
        }
        if (arg == "-o") {
            if (index + 1 >= args.argc) {
                diag << "error: -o requires an output path\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.output_obj_path = std::string(args.at(++index));
            continue;
        }
        if (arg == "-run-native") {
            opts.run_native = true;
            continue;
        }
        if (arg == "-O" || arg == "--optimize") {
            if (index + 1 >= args.argc) {
                diag << "error: -O requires a level (0, 1, 2, or 3)\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            int level = 0;
            const std::string_view value = args.at(++index);
            if (!parseIntInRange(value, 0, 3, level)) {
                diag << "error: invalid -O level: " << value << "\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.optimize = level;
            continue;
        }
        if (arg.size() == 3 && arg[0] == '-' && arg[1] == 'O' && arg[2] >= '0' && arg[2] <= '3') {
            opts.optimize = arg[2] - '0';
            continue;
        }
        if (arg == "--time-passes") {
            opts.time_passes = true;
            continue;
        }
        if (arg == "--verify-mir") {
            opts.verify_mir = true;
            continue;
        }
        if (arg == "--skip-il-optimization") {
            opts.skip_il_optimization = true;
            continue;
        }
        if (arg.substr(0, 13) == "--stack-size=") {
            const std::string_view sizeText = arg.substr(13);
            std::size_t size = 0;
            if (!parseSize(sizeText, size) || size < kMinStackSize) {
                diag << "error: invalid --stack-size value: " << sizeText << "\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.stack_size = size;
            continue;
        }
        if (arg == "--native-asm") {
            opts.assembler_mode = zanna::codegen::x64::CodegenPipeline::AssemblerMode::Native;
            continue;
        }
        if (arg == "--system-asm") {
            opts.assembler_mode = zanna::codegen::x64::CodegenPipeline::AssemblerMode::System;
            continue;
        }
        if (arg == "--native-link") {
            opts.link_mode = zanna::codegen::x64::CodegenPipeline::LinkMode::Native;
            continue;
        }
        if (arg == "--system-link") {
            opts.link_mode = zanna::codegen::x64::CodegenPipeline::LinkMode::System;
            continue;
        }
        if (arg == "--target-host") {
            opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::Host;
            opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Host;
            continue;
        }
        if (arg == "--target-sysv") {
            opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::SysV;
            continue;
        }
        if (arg == "--target-win64") {
            opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::Win64;
            opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Windows;
            continue;
        }
        if (arg == "--target-darwin") {
            opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Darwin;
            if (opts.target_abi == zanna::codegen::x64::CodegenOptions::TargetABI::Host)
                opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::SysV;
            continue;
        }
        if (arg == "--target-linux") {
            opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Linux;
            if (opts.target_abi == zanna::codegen::x64::CodegenOptions::TargetABI::Host)
                opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::SysV;
            continue;
        }
        if (arg == "--target-windows") {
            opts.target_platform = zanna::codegen::x64::CodegenOptions::TargetPlatform::Windows;
            if (opts.target_abi == zanna::codegen::x64::CodegenOptions::TargetABI::Host)
                opts.target_abi = zanna::codegen::x64::CodegenOptions::TargetABI::Win64;
            continue;
        }
        if (arg == "--debug-lines") {
            opts.emit_debug_lines = true;
            continue;
        }
        if (arg == "--no-debug-lines") {
            opts.emit_debug_lines = false;
            continue;
        }
        if (arg == "--fast-link") {
            opts.fast_link = true;
            continue;
        }
        if (arg == "--no-fast-link") {
            opts.fast_link = false;
            continue;
        }
        if (arg == "--asset-blob") {
            if (index + 1 >= args.argc) {
                diag << "error: --asset-blob requires a path\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.asset_blob_path = std::string(args.at(++index));
            continue;
        }
        if (arg == "--extra-obj") {
            if (index + 1 >= args.argc) {
                diag << "error: --extra-obj requires a path\n" << kUsage;
                outcome.diagnostics = diag.str();
                return outcome;
            }
            opts.extra_objects.push_back(std::string(args.at(++index)));
            continue;
        }

        diag << "error: unknown flag '" << arg << "'\n" << kUsage;
        outcome.diagnostics = diag.str();
        return outcome;
    }

    if (!validateTargetCombination(opts, diag)) {
        outcome.diagnostics = diag.str();
        return outcome;
    }

    outcome.opts = std::move(opts);
    return outcome;
}

/// @brief Execute the `compile` handler for the x64 codegen driver.
/// @details Parses arguments via @ref parseCompileArgs and, when successful,
///          runs the code generation pipeline before forwarding captured
///          stdout/stderr to the caller.
/// @param args View over the user-provided arguments.
/// @return Zero on success; otherwise non-zero to signal failure.
int handleCompile(const ArgvView &args) {
    const ParseOutcome parsed = parseCompileArgs(args);
    if (!parsed.opts.has_value()) {
        if (!parsed.diagnostics.empty()) {
            std::cerr << parsed.diagnostics;
        }
        return 1;
    }

    try {
        zanna::codegen::x64::CodegenPipeline pipeline(*parsed.opts);
        const PipelineResult result = pipeline.run();

        if (!result.stdout_text.empty()) {
            std::cout << result.stdout_text;
        }
        if (!result.stderr_text.empty()) {
            std::cerr << result.stderr_text;
        }
        return result.exit_code;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}

using Handler = int (*)(const ArgvView &);

const std::unordered_map<std::string, Handler> kHandlers = {
    {"compile", &handleCompile},
};

} // namespace

/// @brief Dispatch entry point for the `codegen x64` driver.
/// @details Routes to known subcommands (currently only `compile`).  Unknown
///          tokens fall back to `compile` so one-off invocations like
///          `ilc codegen x64 foo.il` still succeed.
/// @param argc Argument count supplied by the CLI harness.
/// @param argv Argument vector supplied by the CLI harness.
/// @return Exit code reported by the chosen handler.
int cmd_codegen_x64(int argc, char **argv) {
    const ArgvView args{argc, argv};
    if (args.empty()) {
        std::cerr << kUsage;
        return 1;
    }

    const std::string_view token = args.front();
    if (token == "--help" || token == "-h") {
        std::cout << kUsage;
        return 0;
    }
    if (const auto it = kHandlers.find(std::string(token)); it != kHandlers.end()) {
        return it->second(args.drop_front());
    }

    return handleCompile(args);
}

/// @brief Register x64 codegen commands with the shared CLI object.
/// @details Present for symmetry with other command registration helpers.  The
///          current driver wires subcommands manually so the function is a
///          no-op.
/// @param cli Reserved structured CLI object.
void register_codegen_x64_commands(CLI &cli) {
    (void)cli;
}

} // namespace zanna::tools::ilc
