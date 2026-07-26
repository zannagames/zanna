//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cli.cpp
/// @brief Parses global command-line options shared by zanna subcommands.
///
/// Centralized parsing keeps trace, input, diagnostics, verification, and instrumentation options
/// consistent across handlers. A thread-local string preserves a detailed error while the public
/// parser retains its compact enum result.

#include "cli.hpp"

#include "support/diag_expected.hpp"
#include "support/diagnostics.hpp"

#include <charconv>
#include <iostream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ilc {

namespace {

thread_local std::string g_lastSharedOptionError;

/// @brief Store a parse error and return the shared parser's error sentinel.
/// @details The parser intentionally returns a compact enum so existing callers
///          stay simple; this helper records the detailed user-facing message for
///          callers that want to surface it.
/// @param message User-facing parse diagnostic to retain.
/// @return Shared-option error sentinel.
SharedOptionParseResult failSharedOption(std::string message) {
    g_lastSharedOptionError = std::move(message);
    return SharedOptionParseResult::Error;
}

} // namespace

/// @brief Parse a zanna global option and update the shared options structure.
///
/// @details Recognised options include tracing (`--trace[=mode]`), stdin
///          redirection, instruction limits, bounds checks, and trap dumping.
///          When the option consumes an additional argument the helper advances
///          @p index so the caller continues scanning from the next flag.
///          Failures—such as a missing argument or malformed numeric value—
///          return @ref SharedOptionParseResult::Error so the caller can surface
///          usage information.  Options that do not match are reported as
///          @ref SharedOptionParseResult::NotMatched, allowing subcommands to
///          parse their own flags.
///
/// @param index Current position in the argv array; advanced when extra tokens
///        are consumed (for example by `--stdin-from` or `--max-steps`).
/// @param argc Total argument count for the subcommand.
/// @param argv Argument vector supplied to the driver.
/// @param opts Structure receiving parsed option values.
/// @return Result indicating whether the option was handled, not matched, or
///         produced a parsing error.
SharedOptionParseResult parseSharedOption(int &index,
                                          int argc,
                                          char **argv,
                                          SharedCliOptions &opts) {
    g_lastSharedOptionError.clear();
    const std::string_view arg = argv[index];
    if (arg == "--trace" || arg == "--trace=il") {
        opts.trace.mode = il::vm::TraceConfig::IL;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--trace=") {
        return failSharedOption("--trace requires il or src when a value is supplied");
    }
    if (arg == "--trace=src") {
        opts.trace.mode = il::vm::TraceConfig::SRC;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--stdin-from") {
        if (index + 1 >= argc) {
            return failSharedOption("--stdin-from requires a file path");
        }
        opts.stdinPath = argv[++index];
        return SharedOptionParseResult::Parsed;
    }
    constexpr std::string_view stdinPrefix = "--stdin-from=";
    if (arg.substr(0, stdinPrefix.size()) == stdinPrefix) {
        std::string_view value = arg.substr(stdinPrefix.size());
        if (value.empty())
            return failSharedOption("--stdin-from requires a file path");
        opts.stdinPath = std::string(value);
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--max-steps") {
        if (index + 1 >= argc) {
            return failSharedOption("--max-steps requires a non-negative integer");
        }
        std::string_view value(argv[index + 1]);
        std::uint64_t parsed = 0;
        const char *const begin = value.data();
        const char *const end = begin + value.size();
        const auto fc = std::from_chars(begin, end, parsed);
        if (fc.ec != std::errc() || fc.ptr != end) {
            return failSharedOption("--max-steps must be a non-negative integer");
        }
        ++index;
        opts.maxSteps = parsed;
        return SharedOptionParseResult::Parsed;
    }
    constexpr std::string_view maxStepsPrefix = "--max-steps=";
    if (arg.substr(0, maxStepsPrefix.size()) == maxStepsPrefix) {
        std::string_view value = arg.substr(maxStepsPrefix.size());
        std::uint64_t parsed = 0;
        const char *const begin = value.data();
        const char *const end = begin + value.size();
        const auto fc = std::from_chars(begin, end, parsed);
        if (value.empty() || fc.ec != std::errc() || fc.ptr != end)
            return failSharedOption("--max-steps must be a non-negative integer");
        opts.maxSteps = parsed;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--bounds-checks") {
        opts.boundsChecks = true;
        opts.boundsChecksSpecified = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--no-bounds-checks") {
        opts.boundsChecks = false;
        opts.boundsChecksSpecified = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--allow-unsafe-pointers") {
        opts.allowUnsafePointers = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--no-unsafe-pointers") {
        opts.allowUnsafePointers = false;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--dump-trap") {
        opts.dumpTrap = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--dump-tokens") {
        opts.dumpTokens = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--dump-ast") {
        opts.dumpAst = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--dump-sema-ast") {
        opts.dumpSemaAst = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--dump-il") {
        opts.dumpIL = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--dump-il-opt") {
        opts.dumpILOpt = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--dump-il-passes") {
        opts.dumpILPasses = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--verify-each") {
        opts.verifyEachPass = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--paranoid-verify") {
        opts.paranoidVerify = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--time-compile") {
        opts.timeCompile = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--pass-stats") {
        opts.passStats = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "-Wall") {
        opts.wall = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "-Werror") {
        opts.werror = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--strict-diagnostics") {
        opts.strictDiagnostics = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--no-strict-diagnostics") {
        opts.strictDiagnostics = false;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--show-warnings") {
        opts.showWarnings = true;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--quiet-warnings" || arg == "--no-warnings") {
        opts.showWarnings = false;
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--diagnostic-format") {
        if (index + 1 >= argc) {
            return failSharedOption("--diagnostic-format requires text or json");
        }
        std::string_view value(argv[++index]);
        if (value == "text") {
            opts.diagnosticFormat = DiagnosticFormat::Text;
        } else if (value == "json") {
            opts.diagnosticFormat = DiagnosticFormat::Json;
        } else {
            return failSharedOption("--diagnostic-format must be 'text' or 'json'");
        }
        return SharedOptionParseResult::Parsed;
    }
    constexpr std::string_view diagFormatPrefix = "--diagnostic-format=";
    if (arg.substr(0, diagFormatPrefix.size()) == diagFormatPrefix) {
        std::string_view value = arg.substr(diagFormatPrefix.size());
        if (value == "text") {
            opts.diagnosticFormat = DiagnosticFormat::Text;
        } else if (value == "json") {
            opts.diagnosticFormat = DiagnosticFormat::Json;
        } else {
            return failSharedOption("--diagnostic-format must be 'text' or 'json'");
        }
        return SharedOptionParseResult::Parsed;
    }
    if (arg.substr(0, 5) == "-Wno-" && arg.size() > 5) {
        opts.disabledWarnings.emplace_back(arg.substr(5));
        return SharedOptionParseResult::Parsed;
    }
    if (arg == "--profile") {
        opts.profile = true;
        return SharedOptionParseResult::Parsed;
    }
    return SharedOptionParseResult::NotMatched;
}

/// @brief Access the detailed message retained by the latest shared-option parse.
/// @return Thread-local message, empty after a successful or unmatched parse.
const std::string &lastSharedOptionError() {
    return g_lastSharedOptionError;
}

/// @brief Pre-scan arguments for the last valid diagnostic-format selection encountered.
/// @param argc Number of arguments to inspect.
/// @param argv Argument vector.
/// @return Requested text or JSON format; malformed values are ignored for authoritative parsing.
DiagnosticFormat detectDiagnosticFormatFlag(int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--diagnostic-format") {
            if (i + 1 >= argc)
                continue;
            const std::string_view value = argv[i + 1];
            if (value == "json")
                return DiagnosticFormat::Json;
            if (value == "text")
                return DiagnosticFormat::Text;
            continue;
        }
        std::string_view inlineValue;
        if (splitInlineOptionValue(arg, "--diagnostic-format", inlineValue)) {
            if (inlineValue == "json")
                return DiagnosticFormat::Json;
            if (inlineValue == "text")
                return DiagnosticFormat::Text;
        }
    }
    return DiagnosticFormat::Text;
}

/// @brief Match a GNU-style inline option and expose its value without allocation.
/// @param arg Full argument token.
/// @param optionName Expected option spelling including leading dashes.
/// @param value Receives the substring after the equals sign on a match.
/// @return @c true when @p arg exactly begins with @p optionName followed by @c =.
bool splitInlineOptionValue(std::string_view arg,
                            std::string_view optionName,
                            std::string_view &value) {
    if (arg.size() < optionName.size() + 1)
        return false;
    if (arg.substr(0, optionName.size()) != optionName)
        return false;
    if (arg[optionName.size()] != '=')
        return false;
    value = arg.substr(optionName.size() + 1);
    return true;
}

/// @brief Render one diagnostic in the selected user-facing encoding.
/// @param diag Diagnostic to render.
/// @param os Destination stream.
/// @param sm Optional source manager used to resolve locations.
/// @param format Text or JSON output format.
void printDiagnostic(const il::support::Diagnostic &diag,
                     std::ostream &os,
                     const il::support::SourceManager *sm,
                     DiagnosticFormat format) {
    switch (format) {
        case DiagnosticFormat::Text:
            il::support::printDiag(diag, os, sm);
            return;
        case DiagnosticFormat::Json:
            il::support::printDiagJson(diag, os, sm);
            return;
    }
}

/// @brief Render an ordered diagnostic collection in the selected encoding.
/// @param diagnostics Diagnostics to render.
/// @param os Destination stream.
/// @param sm Optional source manager used to resolve locations.
/// @param format Text or JSON output format.
void printDiagnostics(const std::vector<il::support::Diagnostic> &diagnostics,
                      std::ostream &os,
                      const il::support::SourceManager *sm,
                      DiagnosticFormat format) {
    switch (format) {
        case DiagnosticFormat::Text:
            for (const auto &diag : diagnostics)
                il::support::printDiag(diag, os, sm);
            return;
        case DiagnosticFormat::Json:
            il::support::printDiagnosticsJson(diagnostics, os, sm);
            return;
    }
}

/// @brief Render the current diagnostics owned by an engine.
/// @param engine Diagnostic engine to snapshot.
/// @param os Destination stream.
/// @param sm Optional source manager used to resolve locations.
/// @param format Text or JSON output format.
void printDiagnosticEngine(const il::support::DiagnosticEngine &engine,
                           std::ostream &os,
                           const il::support::SourceManager *sm,
                           DiagnosticFormat format) {
    printDiagnostics(engine.diagnostics(), os, sm, format);
}

} // namespace ilc
