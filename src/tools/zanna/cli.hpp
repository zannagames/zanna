//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cli.hpp
/// @brief Declares shared Zanna command-line options, diagnostics, and subcommand handlers.
///
/// Shared option parsing mutates caller-owned configuration and retains only the most recent
/// thread-local parse-error message. Command handlers borrow their argument vectors for the
/// duration of each invocation.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "zanna/vm/debug/Debug.hpp"
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace il::support {
struct Diagnostic;
class DiagnosticEngine;
class SourceManager;
} // namespace il::support

namespace ilc {

/// @brief User-facing diagnostic output encoding.
enum class DiagnosticFormat { Text, Json };

/// @brief Shared configuration for zanna subcommands that execute IL.
struct SharedCliOptions {
    /// @brief Trace settings requested via --trace flags.
    il::vm::TraceConfig trace{};

    /// @brief Optional replacement for standard input.
    std::string stdinPath{};

    /// @brief Maximum number of interpreter steps (0 means unlimited).
    std::uint64_t maxSteps = 0;

    /// @brief Whether bounds checks should be enabled during source lowering.
    bool boundsChecks = true;

    /// @brief Whether the bounds-check setting was explicitly supplied on the CLI.
    bool boundsChecksSpecified = false;

    /// @brief Reserved compatibility flag; frontend source no longer exposes raw pointers.
    bool allowUnsafePointers = false;

    /// @brief Request formatted trap diagnostics on unhandled errors.
    bool dumpTrap = false;

    /// @brief Dump the raw token stream from the lexer.
    bool dumpTokens = false;

    /// @brief Dump the AST after parsing.
    bool dumpAst = false;

    /// @brief Dump the AST after semantic analysis.
    bool dumpSemaAst = false;

    /// @brief Dump IL after lowering (before optimization).
    bool dumpIL = false;

    /// @brief Dump IL after the full optimization pipeline.
    bool dumpILOpt = false;

    /// @brief Dump IL before and after each optimization pass.
    bool dumpILPasses = false;

    /// @brief Verify the full IL module after each optimization pass.
    bool verifyEachPass = false;

    /// @brief Run all normal frontend verifier checkpoints instead of the profile-selected subset.
    bool paranoidVerify = false;

    /// @brief Emit phase timing information for source-to-IL/native compilation.
    bool timeCompile = false;

    /// @brief Emit detailed per-pass optimizer statistics.
    bool passStats = false;

    /// @brief Enable all warnings (corresponds to `-Wall`).
    bool wall = false;

    /// @brief Treat warnings as errors (corresponds to `-Werror`).
    bool werror = false;

    /// @brief Promote warnings that can indicate runtime traps or unsafe binaries.
    bool strictDiagnostics = true;

    /// @brief Print warnings even when compilation otherwise succeeds.
    bool showWarnings = true;

    /// @brief Requested diagnostic output format.
    DiagnosticFormat diagnosticFormat = DiagnosticFormat::Text;

    /// @brief Warning codes/names disabled via `-Wno-XXX`.
    std::vector<std::string> disabledWarnings;

    /// @brief Enable combined profiling (instruction count + wall-clock timing).
    bool profile = false;
};

/// @brief Result of attempting to parse a shared CLI option.
enum class SharedOptionParseResult {
    NotMatched, ///< Argument does not correspond to a shared option.
    Parsed,     ///< Argument consumed and reflected in the configuration.
    Error       ///< Argument looked like a shared option but was malformed.
};

/// @brief Parse a zanna option common to multiple subcommands.
///
/// @param index Index of the current argument; advanced when parameters are consumed.
/// @param argc Total number of arguments available.
/// @param argv Argument vector.
/// @param opts Accumulator receiving parsed option values.
/// @return Parsing outcome describing whether the argument was handled.
SharedOptionParseResult parseSharedOption(int &index,
                                          int argc,
                                          char **argv,
                                          SharedCliOptions &opts);

/// @brief Return the diagnostic from the most recent shared-option parse error.
/// @details parseSharedOption() preserves its historical enum return value for
///          callers, while this accessor lets subcommands print the specific
///          missing-value or invalid-value reason instead of a generic failure.
/// @return Thread-local error message, empty when the most recent parse succeeded or did not match.
[[nodiscard]] const std::string &lastSharedOptionError();

/// @brief Return the diagnostic format requested anywhere in @p argv.
/// @details This lightweight pre-scan lets subcommands honor
///          `--diagnostic-format=json` even when a later argument fails before
///          the normal shared-option parser can produce a full configuration.
///          Malformed or missing values are ignored here and reported by the
///          authoritative parser.
/// @param argc Number of arguments to inspect.
/// @param argv Argument vector to scan.
/// @return The requested diagnostic format, or text when no valid request is present.
[[nodiscard]] DiagnosticFormat detectDiagnosticFormatFlag(int argc, char **argv);

/// @brief Extract the value from a `--name=value` command-line token.
/// @details The helper is intentionally small and allocation-free so individual
///          subcommands can support the common GNU-style inline form without
///          reimplementing prefix and empty-value checks.
/// @param arg Full argument token to inspect.
/// @param optionName Option name including leading dashes, for example `--target`.
/// @param value Receives the substring after `=` when the token matches.
/// @return True when @p arg is exactly `optionName=value`; false otherwise.
[[nodiscard]] bool splitInlineOptionValue(std::string_view arg,
                                          std::string_view optionName,
                                          std::string_view &value);

/// @brief Print one diagnostic according to the requested format.
/// @param diag Diagnostic to render.
/// @param os Destination stream.
/// @param sm Optional source manager used to resolve source locations.
/// @param format Text or JSON output encoding.
void printDiagnostic(const il::support::Diagnostic &diag,
                     std::ostream &os,
                     const il::support::SourceManager *sm,
                     DiagnosticFormat format);

/// @brief Print a list of diagnostics according to the requested format.
/// @param diagnostics Ordered diagnostics to render.
/// @param os Destination stream.
/// @param sm Optional source manager used to resolve source locations.
/// @param format Text or JSON output encoding.
void printDiagnostics(const std::vector<il::support::Diagnostic> &diagnostics,
                      std::ostream &os,
                      const il::support::SourceManager *sm,
                      DiagnosticFormat format);

/// @brief Print diagnostics collected by an engine according to the requested format.
/// @param engine Diagnostic engine whose current snapshot is rendered.
/// @param os Destination stream.
/// @param sm Optional source manager used to resolve source locations.
/// @param format Text or JSON output encoding.
void printDiagnosticEngine(const il::support::DiagnosticEngine &engine,
                           std::ostream &os,
                           const il::support::SourceManager *sm,
                           DiagnosticFormat format);

} // namespace ilc

/// @brief Handle `zanna front basic` subcommands.
///
/// Invoked when the command line begins with `zanna front basic`. After the
/// subcommand tokens are consumed, `argc` and `argv` contain the remaining
/// arguments specific to the BASIC front end.
///
/// @param argc Number of arguments following `front basic`.
/// @param argv Array of argument strings.
/// @return `0` on successful compilation or execution, non‑zero on errors such
///         as parse failures or runtime traps.
///
/// @note May emit IL to `stdout` or run the resulting module via the VM and can
/// redirect `stdin` when `--stdin-from` is provided.
int cmdFrontBasic(int argc, char **argv);

/// @brief Handle `zanna front zia` subcommands.
///
/// @param argc Number of arguments following `front zia`.
/// @param argv Array of argument strings.
/// @return `0` on successful compilation or execution, non‑zero on errors.
int cmdFrontZia(int argc, char **argv);

/// @brief Handle `zanna -run` with an externally managed source manager.
///
/// Allows tests to preconfigure the @ref il::support::SourceManager used by the
/// run command, enabling overflow scenarios to be triggered deterministically.
///
/// @param argc Number of arguments following `-run`.
/// @param argv Argument vector beginning with the IL file path.
/// @param sm Source manager instance supplied by the caller.
/// @return Exit status of the run command; non-zero on failure.
int cmdRunILWithSourceManager(int argc, char **argv, il::support::SourceManager &sm);

/// @brief Handle `zanna -run`.
///
/// Triggered when `zanna` receives the `-run` option. `argv[0]` should hold the
/// path to an IL file and the remaining elements provide optional flags such as
/// tracing or debugger control.
///
/// @param argc Number of arguments following `-run`.
/// @param argv Argument vector beginning with the IL file path.
/// @return Exit code of the executed program or `1` when validation fails or the
///         module lacks `func @main()` (emitting "missing main").
///
/// @note May modify `stdin`, emit trace/debug information to `stderr`, and
/// collect execution statistics.
int cmdRunIL(int argc, char **argv);

/// @brief Handle `zanna il-opt`.
///
/// Invoked for the `zanna il-opt` subcommand. `argv[0]` supplies the input IL
/// file while subsequent arguments select passes and the output file.
///
/// @param argc Number of arguments following `zanna il-opt`.
/// @param argv Argument vector beginning with the input IL file.
/// @return `0` on success or `1` if arguments are malformed or file operations
///         fail.
///
/// @note Writes the optimized module to disk and may print pass statistics to
/// `stdout`.
int cmdILOpt(int argc, char **argv);

/// @brief Handle `zanna bench` subcommand.
///
/// Invoked when the command line begins with `zanna bench`. Benchmarks IL programs
/// using different VM dispatch strategies and reports performance metrics.
///
/// @param argc Number of arguments following `bench`.
/// @param argv Array of argument strings.
/// @return `0` on success, non-zero on failure.
int cmdBench(int argc, char **argv);

/// @brief Handle `zanna run` subcommand.
///
/// Compiles and executes a Zanna project. The target may be a single source file,
/// a directory (with optional zanna.project manifest), or a manifest path.
/// Auto-detects language (Zia or BASIC) and entry point.
///
/// @param argc Number of arguments following `run`.
/// @param argv Array of argument strings.
/// @return `0` on success, non-zero on failure.
int cmdRun(int argc, char **argv);

/// @brief Handle `zanna build` subcommand.
///
/// Compiles a Zanna project and emits IL to stdout or a file specified with -o.
///
/// @param argc Number of arguments following `build`.
/// @param argv Array of argument strings.
/// @return `0` on success, non-zero on failure.
int cmdBuild(int argc, char **argv);

/// @brief Handle `zanna build-many` for persistent multi-project native builds.
/// @details Builds named project targets sequentially inside one process so
///          compiler registries, parsed runtime archives, and linker metadata
///          caches can be reused. Each target is written beneath one output
///          directory using `name=project-path` syntax.
/// @param argc Number of arguments following `build-many`.
/// @param argv Array containing options and named project specifications.
/// @return Zero when every project builds successfully; otherwise non-zero.
int cmdBuildMany(int argc, char **argv);

/// @brief Build a project to a native executable for packaging workflows.
/// @details This is the programmatic equivalent of `zanna build <target> -o
///          <output> --arch <arch>` and shares the same project-resolution,
///          asset, verification, diagnostic, and native-codegen path as the
///          public build command. It avoids constructing synthetic argv arrays
///          in higher-level packaging code.
/// @param target Project target path, source file, or manifest path.
/// @param outputPath Native executable output path.
/// @param arch Target architecture text (`x64` or `arm64`).
/// @param windowsReleaseRuntime True to force the Windows release CRT.
/// @return `0` on success; non-zero on build or validation failure.
int buildProjectToNativeForPackage(const std::string &target,
                                   const std::string &outputPath,
                                   const std::string &arch,
                                   bool windowsReleaseRuntime);

/// @brief Handle `zanna check` subcommand.
///
/// Compiles and verifies a Zanna project without executing or emitting output.
/// Diagnostics honor --diagnostic-format for machine-readable JSON.
///
/// @param argc Number of arguments following `check`.
/// @param argv Array of argument strings.
/// @return `0` when no errors, `1` on usage/target-resolution errors, `2` on
///         compile or verification errors.
int cmdCheck(int argc, char **argv);

/// @brief Handle `zanna init` subcommand.
///
/// Scaffolds a new Zanna project with a zanna.project manifest and an
/// entry-point source file.
///
/// @param argc Number of arguments following `init`.
/// @param argv Array of argument strings.
/// @return `0` on success, non-zero on failure.
int cmdInit(int argc, char **argv);

/// @brief Handle `zanna package` subcommand.
///
/// Compiles a project to a native binary and packages it into a
/// platform-specific installer (.zip for macOS, .deb for Linux,
/// .exe for Windows).
///
/// @param argc Number of arguments following `package`.
/// @param argv Array of argument strings.
/// @return `0` on success, non-zero on failure.
int cmdPackage(int argc, char **argv);

/// @brief Handle `zanna install-package` subcommand.
/// @param argc Number of arguments following `install-package`.
/// @param argv Array of argument strings.
/// @return Zero on success; nonzero on validation, package, or installation failure.
int cmdInstallPackage(int argc, char **argv);

/// @brief Handle `zanna repl` subcommand.
///
/// Launches an interactive REPL session for the specified language.
/// Default language is Zia.
///
/// @param argc Number of arguments following `repl`.
/// @param argv Array of argument strings.
/// @return `0` on success, non-zero on failure.
int cmdRepl(int argc, char **argv);

/// @brief Handle `zanna eval` subcommand.
///
/// Evaluates a single Zia or BASIC snippet through a fresh REPL session and
/// prints the result, optionally as a structured JSON object (--json).
///
/// @param argc Number of arguments following `eval`.
/// @param argv Array of argument strings.
/// @return `0` on success, `1` on usage errors, `2` on compile/eval errors,
///         `3` on runtime traps.
int cmdEval(int argc, char **argv);

/// @brief Handle `zanna explain` subcommand.
///
/// Describes a diagnostic code from the central catalog, or lists the whole
/// catalog with `--list`. Supports `--json` for machine-readable output.
///
/// @param argc Number of arguments following `explain`.
/// @param argv Array of argument strings.
/// @return `0` when the code resolves, `1` on usage error or unknown code.
int cmdExplain(int argc, char **argv);

/// @brief Implement the `--print-error-codes [--json]` driver flag.
///
/// @param json True to emit the catalog as a JSON array.
/// @return `0` on success.
int printErrorCodes(bool json);

/// @brief Print usage information for zanna.
///
/// Called when no or invalid arguments are supplied to `zanna` or when a handler
/// needs to display help text.
///
/// @return void
/// @note Writes a synopsis and option hints to `stderr` and has no other side
/// effects.
void usage();
