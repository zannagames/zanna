//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/frontend_tool.hpp
// Purpose: Shared infrastructure for language frontend CLI tools (zbasic, zia).
// Key invariants: All frontend tools share the same argument parsing logic.
// Ownership/Lifetime: Helpers are stateless aside from buildIlcArgs, whose
//                     returned char* vector aliases into a caller-owned storage
//                     vector that must outlive it.
// Links: docs/internals/architecture.md, src/tools/common/native_compiler.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the shared command-line pipeline for standalone language frontends.
/// @details The header centralizes option validation, language-specific callback
///          hooks, frontend argument construction, stdout redirection, temporary
///          IL handling, and optional native compilation for `zia` and `zbasic`.

#pragma once

#include "common/Filesystem.hpp"
#include "tools/common/ScopedProcess.hpp"
#include "tools/common/native_compiler.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zanna::tools {

/// @brief Return a lowercased ASCII copy of @p value.
/// @details Frontend tool file-extension parsing should behave predictably across
///          case-insensitive and case-sensitive filesystems. Only ASCII is folded
///          because source extensions are ASCII command-line syntax.
/// @param value Text whose ASCII uppercase bytes should be folded.
/// @return Owned copy with each byte safely passed through `std::tolower`.
inline std::string lowerAscii(std::string_view value) {
    std::string lowered(value);
    /// @brief Convert through unsigned char so negative plain-char values are not passed to ctype.
    /// @param c Unsigned source byte.
    /// @return Lowercase byte converted back to plain char.
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered;
}

/// @brief Test whether @p path ends with @p extension using ASCII case folding.
/// @details This keeps wrapper tools from rejecting valid source paths such as
///          `MAIN.ZIA` on platforms and editors that preserve uppercase suffixes.
/// @param path Candidate source path.
/// @param extension Required language suffix, including its leading dot.
/// @return True when the complete suffix matches after case folding.
inline bool endsWithExtensionInsensitive(std::string_view path, std::string_view extension) {
    if (path.size() < extension.size())
        return false;
    return lowerAscii(path.substr(path.size() - extension.size())) == lowerAscii(extension);
}

/// @brief Configuration parsed from frontend tool command-line arguments.
/// @details Owns every path and forwarded argument needed after parsing; the
///          optional architecture selects native output independently of IL mode.
struct FrontendToolConfig {
    /// @brief Path to the source file to compile.
    std::string sourcePath;

    /// @brief Path for the output file (IL or native binary).
    std::string outputPath;

    /// @brief Whether to emit IL text instead of running the program.
    bool emitIl = false;

    /// @brief Whether to run the compiled program immediately.
    bool run = false;

    /// @brief Additional flags forwarded to the underlying ilc frontend.
    std::vector<std::string> forwardedArgs;

    /// @brief Arguments passed to the program at runtime (after "--" separator).
    std::vector<std::string> programArgs;

    /// @brief Optional architecture override for native code generation.
    std::optional<TargetArch> archOverride;
};

/// @brief Callbacks for language-specific behavior in frontend tools.
/// @details String views borrow static language metadata, while std::function
///          members own their callable wrappers. All three callbacks must be
///          populated before parsing or running a tool.
struct FrontendToolCallbacks {
    /// @brief File extension for this language (e.g., ".bas", ".zia").
    std::string_view fileExtension;

    /// @brief Language name for error messages (e.g., "BASIC", "Zia").
    std::string_view languageName;

    /// @brief Callback to print usage/help information for the tool.
    std::function<void()> printUsage;

    /// @brief Callback to print version information for the tool.
    std::function<void()> printVersion;

    /// @brief The ilc frontend command to invoke for compilation.
    std::function<int(int, char **)> frontendCommand;
};

/// @brief Structured stop requested while parsing wrapper arguments.
/// @details Help and version are successful stops; malformed arguments are
///          failing stops after the parser has already printed a diagnostic and
///          usage text. Throwing this object unwinds normally instead of calling
///          `std::exit` from a shared helper.
struct FrontendToolParseStop {
    int exitCode = 0; ///< Process exit code requested by the parser.
};

/// @brief Return true when a wrapper-forwarded option consumes the next token.
/// @details The standalone `zia` and `zbasic` wrappers forward only options
///          understood by the underlying `zanna front` commands. This prevents
///          spelling mistakes from being accepted as opaque passthrough flags.
/// @param arg Complete option token to classify.
/// @return True when the option requires a separate following value.
inline bool frontendForwardedFlagTakesValue(std::string_view arg) {
    return arg == "--stdin-from" || arg == "--max-steps" || arg == "--diagnostic-format" ||
           arg == "--build-profile";
}

/// @brief Return true when @p arg is a supported forwarded frontend option.
/// @details Inline forms that include values are accepted here; separated forms
///          are handled by @ref frontendForwardedFlagTakesValue.
/// @param arg Complete leading-dash token to validate.
/// @return True when the shared underlying frontend recognizes the option form.
inline bool isKnownFrontendForwardedFlag(std::string_view arg) {
    if (arg == "--debug-vm" || arg == "--dump-trap" || arg == "--bounds-checks" ||
        arg == "--no-bounds-checks" || arg == "--dump-tokens" || arg == "--dump-ast" ||
        arg == "--dump-sema-ast" || arg == "--dump-il" || arg == "--dump-il-opt" ||
        arg == "--dump-il-passes" || arg == "--verify-each" || arg == "--paranoid-verify" ||
        arg == "--time-compile" || arg == "--pass-stats" || arg == "-Wall" || arg == "-Werror" ||
        arg == "--strict-diagnostics" || arg == "--no-strict-diagnostics" ||
        arg == "--show-warnings" || arg == "--quiet-warnings" || arg == "--no-warnings" ||
        arg == "--profile" || arg == "--no-runtime-namespaces" || arg == "-O0" || arg == "-O1" ||
        arg == "-O2") {
        return true;
    }
    return arg == "--trace" || arg == "--trace=il" || arg == "--trace=src" ||
           arg.starts_with("--stdin-from=") || arg.starts_with("--max-steps=") ||
           arg.starts_with("--diagnostic-format=") || arg.starts_with("--build-profile=") ||
           (arg.starts_with("-Wno-") && arg.size() > 5);
}

/// @brief Parse frontend tool arguments into a @ref FrontendToolConfig.
///
/// @details Walks @p argv recognising the shared option vocabulary:
///          - `-h`/`--help` and `--version` invoke the matching callback and
///            throw @ref FrontendToolParseStop with exit code 0.
///          - `-o`/`--output` records the output path and implies `--emit-il`.
///          - `--arch arm64|x64` overrides the native target architecture.
///          - `--` ends option parsing; everything after it becomes a program
///            argument forwarded to the running program.
///          - Any other leading-dash token is forwarded verbatim to the ilc
///            frontend, consuming a following value for the flags that take one
///            (`--stdin-from`, `--max-steps`, `--diagnostic-format`,
///            `--build-profile`).
///          - A token ending in @ref FrontendToolCallbacks::fileExtension is the
///            single source file; a second source file is an error.
///          Missing required values, unknown tokens, or a missing source file
///          print usage and throw @ref FrontendToolParseStop with exit code 1.
///          When no output mode is requested the config defaults to running the
///          program.
///
/// @param argc Number of command-line arguments.
/// @param argv Array of argument strings.
/// @param callbacks Language-specific callbacks (usage/version text, extension).
/// @return Parsed configuration on success.
/// @throws FrontendToolParseStop for help, version, and parse failures.
/// @pre @p argv references at least @p argc non-null C strings and all callbacks
///      required by the encountered control path are callable.
inline FrontendToolConfig parseArgs(int argc, char **argv, const FrontendToolCallbacks &callbacks) {
    FrontendToolConfig config{};

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            callbacks.printUsage();
            throw FrontendToolParseStop{0};
        } else if (arg == "--version") {
            callbacks.printVersion();
            throw FrontendToolParseStop{0};
        } else if (arg == "--emit-il") {
            config.emitIl = true;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "error: " << arg << " requires an output path\n\n";
                callbacks.printUsage();
                throw FrontendToolParseStop{1};
            }
            config.outputPath = argv[++i];
            if (config.outputPath.empty()) {
                std::cerr << "error: " << arg << " requires an output path\n\n";
                callbacks.printUsage();
                throw FrontendToolParseStop{1};
            }
            config.emitIl = true; // -o implies emit-il
        } else if (arg == "--arch" || arg.starts_with("--arch=")) {
            std::string_view val;
            if (arg.starts_with("--arch=")) {
                val = arg.substr(std::string_view("--arch=").size());
            } else if (i + 1 >= argc) {
                std::cerr << "error: --arch requires arm64 or x64\n\n";
                callbacks.printUsage();
                throw FrontendToolParseStop{1};
            } else {
                val = argv[++i];
            }
            if (val == "arm64")
                config.archOverride = TargetArch::ARM64;
            else if (val == "x64")
                config.archOverride = TargetArch::X64;
            else {
                std::cerr << "error: --arch must be 'arm64' or 'x64'\n\n";
                callbacks.printUsage();
                throw FrontendToolParseStop{1};
            }
        } else if (arg == "--") {
            // Remaining arguments are program arguments
            for (int j = i + 1; j < argc; ++j)
                config.programArgs.emplace_back(argv[j]);
            break;
        } else if (arg.starts_with("-")) {
            if (!isKnownFrontendForwardedFlag(arg)) {
                std::cerr << "error: unknown option: " << arg << "\n\n";
                callbacks.printUsage();
                throw FrontendToolParseStop{1};
            }
            // Forward other flags to underlying ilc implementation
            config.forwardedArgs.push_back(std::string(arg));

            // Check if this flag takes an argument
            if (frontendForwardedFlagTakesValue(arg)) {
                if (i + 1 >= argc) {
                    std::cerr << "error: " << arg << " requires an argument\n\n";
                    callbacks.printUsage();
                    throw FrontendToolParseStop{1};
                }
                config.forwardedArgs.push_back(argv[++i]);
            }
        } else if (endsWithExtensionInsensitive(arg, callbacks.fileExtension)) {
            if (!config.sourcePath.empty()) {
                std::cerr << "error: multiple source files not supported\n\n";
                callbacks.printUsage();
                throw FrontendToolParseStop{1};
            }
            config.sourcePath = std::string(arg);
        } else {
            std::cerr << "error: unknown argument or file type: " << arg << "\n";
            std::cerr << "       (expected " << callbacks.fileExtension << " file)\n\n";
            callbacks.printUsage();
            throw FrontendToolParseStop{1};
        }
    }

    // Validate configuration
    if (config.sourcePath.empty()) {
        std::cerr << "error: no input file specified\n\n";
        callbacks.printUsage();
        throw FrontendToolParseStop{1};
    }

    // Default action: run the program
    if (!config.emitIl) {
        config.run = true;
    }

    return config;
}

/// @brief Build the argument vector for the ilc frontend subcommand.
///
/// @details Assembles the equivalent of an `argv` array for the underlying ilc
///          frontend: a leading mode flag (`-emit-il` or `-run`), the source
///          path, every forwarded flag, and the program arguments after a `--`
///          separator. The strings are first appended to @p outStorage so they
///          have a stable address, then a parallel vector of @c char* pointers
///          into that storage is returned.
///
/// @warning The returned vector aliases into @p outStorage. Callers must keep
///          @p outStorage alive (and unmodified) for as long as the returned
///          pointers are used; mutating @p outStorage afterwards may invalidate
///          them.
///
/// @param config Parsed frontend configuration.
/// @param outStorage Cleared and populated with the backing argument strings.
/// @return Argument vector of @c char* suitable for the frontend command.
inline std::vector<char *> buildIlcArgs(const FrontendToolConfig &config,
                                        std::vector<std::string> &outStorage) {
    outStorage.clear();
    outStorage.reserve(2 + config.forwardedArgs.size() +
                       (config.programArgs.empty() ? 0 : 1 + config.programArgs.size()));

    std::vector<char *> args;

    // Add mode flag
    if (config.emitIl) {
        outStorage.push_back("-emit-il");
    } else {
        outStorage.push_back("-run");
    }

    // Add source path
    outStorage.push_back(config.sourcePath);

    // Add forwarded arguments
    for (const auto &fwd : config.forwardedArgs) {
        outStorage.push_back(fwd);
    }

    // Forward program arguments after '--' separator
    if (!config.programArgs.empty()) {
        outStorage.push_back("--");
        for (const auto &parg : config.programArgs) {
            outStorage.push_back(parg);
        }
    }

    // Build char* array from stable strings
    for (auto &str : outStorage) {
        args.push_back(str.data());
    }

    return args;
}

/// @brief Run a frontend tool end to end with the given callbacks.
///
/// @details Drives the full compile/run pipeline:
///          1. Parse arguments via @ref parseArgs.
///          2. Detect whether `-o` requests a native binary (a non-`.il` output
///             extension); if so, redirect IL emission to a temporary file so the
///             native code generator can consume it afterwards.
///          3. When an output path is set, redirect @c stdout with
///             @ref ScopedStdoutRedirect so the descriptor is restored before
///             native codegen writes to the terminal.
///          4. Delegate to @ref FrontendToolCallbacks::frontendCommand to emit IL
///             or run the program.
///          5. On success with native output, invoke @ref compileToNative using
///             the requested or host-detected architecture.
///          6. After frontend/native execution, make a best-effort removal of
///             the temporary IL file. Cleanup errors do not replace the command
///             result.
///
/// @param argc Number of command-line arguments.
/// @param argv Array of argument strings.
/// @param callbacks Language-specific callbacks.
/// @return Exit status: 0 on success, non-zero on error.
/// @pre @p argv references at least @p argc non-null C strings and the callback
///      bundle remains valid and callable for the duration of the invocation.
inline int runFrontendTool(int argc, char **argv, const FrontendToolCallbacks &callbacks) {
    if (argc < 2) {
        callbacks.printUsage();
        return 1;
    }

    FrontendToolConfig config;
    try {
        config = parseArgs(argc, argv, callbacks);
    } catch (const FrontendToolParseStop &stop) {
        return stop.exitCode;
    }

    // Detect native output: -o with non-.il extension
    const bool nativeOutput = !config.outputPath.empty() && isNativeOutputPath(config.outputPath);

    std::string realOutputPath = config.outputPath;
    std::string tempIlPath;

    if (nativeOutput) {
        // Redirect IL to temp file; we'll codegen from it later
        tempIlPath = generateTempIlPath();
        config.outputPath = tempIlPath;
    }

    // Build argument vector for ilc frontend
    std::vector<std::string> argStorage;
    std::vector<char *> ilcArgs = buildIlcArgs(config, argStorage);

    // Handle -o output redirection (either to real file or temp file for native)
    std::optional<ScopedStdoutRedirect> stdoutRedirect;
    if (!config.outputPath.empty()) {
        const std::filesystem::path outputParent =
            zanna::filesystem::pathFromUtf8(config.outputPath).parent_path();
        if (!outputParent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(outputParent, ec);
            if (ec) {
                std::cerr << "error: failed to create output directory for " << config.outputPath
                          << ": " << ec.message() << "\n";
                return 1;
            }
        }
        stdoutRedirect.emplace(config.outputPath);
        if (!stdoutRedirect->ok()) {
            std::cerr << "error: " << stdoutRedirect->errorMessage() << "\n";
            return 1;
        }
    }

    // Delegate to frontend implementation
    int result = callbacks.frontendCommand(static_cast<int>(ilcArgs.size()), ilcArgs.data());

    // Restore stdout if we redirected it
    if (stdoutRedirect) {
        if (!stdoutRedirect->finish() && result == 0) {
            std::cerr << "error: " << stdoutRedirect->errorMessage() << "\n";
            result = 1;
        }
    }

    // Native compilation step: compile the IL temp file to a binary
    if (result == 0 && nativeOutput) {
        auto arch = config.archOverride.value_or(detectHostArch());
        result = compileToNative(tempIlPath, realOutputPath, arch);
    }

    // Clean up temp file
    if (!tempIlPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(zanna::filesystem::pathFromUtf8(tempIlPath), ec);
    }

    return result;
}

} // namespace zanna::tools
