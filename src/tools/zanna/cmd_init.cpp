//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_init.cpp
/// @brief Implements safe Zia or BASIC project scaffolding for @c zanna init.
///
/// Project names are constrained before filesystem use, generated files are written beneath one
/// newly created directory, and failures remove the partial scaffold. The printed run hint is
/// shell-quoted for validated names.
//
//===----------------------------------------------------------------------===//

#include "cli.hpp"
#include "common/Filesystem.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

/// @brief Write a text file, returning false on failure.
/// @param path Destination file.
/// @param content Complete bytes to write.
/// @return @c true after a complete write; otherwise @c false after printing a diagnostic.
static bool writeFile(const fs::path &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "error: could not write " << zanna::filesystem::pathToUtf8(path) << "\n";
        return false;
    }
    out << content;
    if (!out) {
        std::cerr << "error: failed while writing " << zanna::filesystem::pathToUtf8(path) << "\n";
        return false;
    }
    return true;
}

/// @brief Validate a project name supplied to `zanna init`.
/// @details Rejects "."/"..", path separators, control characters, and double
///          quotes, printing a specific error to stderr for each case so the
///          generated directory and manifest cannot be corrupted or escape.
/// @param projectName Candidate project name from the command line.
/// @return true if the name is safe to use; false (with an error printed) otherwise.
static bool validateProjectName(const std::string &projectName) {
    if (projectName == "." || projectName == "..") {
        std::cerr << "error: project name must not be '.' or '..'\n";
        return false;
    }
    if (projectName.find('/') != std::string::npos || projectName.find('\\') != std::string::npos) {
        std::cerr << "error: project name must not contain path separators\n";
        return false;
    }
    for (char c : projectName) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            std::cerr << "error: project name must not contain control characters\n";
            return false;
        }
        if (c == '"') {
            std::cerr << "error: project name must not contain double quotes\n";
            return false;
        }
    }
    return true;
}

/// @brief Quote a command argument for POSIX-style shells when needed.
/// @details The generated hint is informational, but it should still be safe to
///          paste for project names containing spaces, shell metacharacters, or
///          leading dashes (after commandPathForProjectName prefixes `./`).
/// @param value Raw argument text to quote.
/// @return Either @p value unchanged or a single-quoted shell token.
static std::string shellQuoteArgument(std::string_view value) {
    /// @brief Test whether one shell-token byte can remain unquoted.
    /// @param c Byte to inspect.
    /// @return `true` for alphanumeric bytes or `_`, `-`, `.`, and `/`.
    const auto safeChar = [](char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || c == '_' || c == '-' || c == '.' || c == '/';
    };
    if (!value.empty() && std::all_of(value.begin(), value.end(), safeChar))
        return std::string(value);
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted.push_back(c);
    }
    quoted += "'";
    return quoted;
}

/// @brief Build a shell-safe project target for the printed `zanna run` hint.
/// @param projectName Validated project directory name.
/// @return Quoted path text suitable for pasting after `zanna run`.
static std::string commandPathForProjectName(const std::string &projectName) {
    std::string target = projectName;
    if (!target.empty() && target.front() == '-')
        target = "./" + target;
    return shellQuoteArgument(target);
}

/// @brief Print usage for the `zanna init` subcommand to stderr.
static void printInitUsage() {
    std::cerr << "Usage: zanna init <project-name> [--lang zia|basic]\n"
              << "\n"
              << "Create a new Zanna project directory with a zanna.project manifest.\n"
              << "\n"
              << "Options:\n"
              << "  --lang zia|basic   Source language for the generated entry file\n"
              << "  --                 Treat the next token as the project name\n"
              << "  -h, --help         Show this help\n";
}

/// @brief Parse options and scaffold a new single-entry Zanna project.
/// @param argc Number of arguments following @c init.
/// @param argv Argument vector.
/// @return Zero on success or help; one on usage, validation, or filesystem failure.
int cmdInit(int argc, char **argv) {
    std::string projectName;
    std::string lang = "zia";
    bool endOfOptions = false;

    // Parse arguments.
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (!endOfOptions && arg == "--") {
            endOfOptions = true;
        } else if (!endOfOptions && arg == "--lang") {
            if (i + 1 >= argc) {
                std::cerr << "error: --lang requires a value (zia or basic)\n";
                printInitUsage();
                return 1;
            }
            lang = argv[++i];
            if (lang != "zia" && lang != "basic") {
                std::cerr << "error: --lang must be 'zia' or 'basic', got '" << lang << "'\n";
                printInitUsage();
                return 1;
            }
        } else if (!endOfOptions && (arg == "--help" || arg == "-h")) {
            printInitUsage();
            return 0;
        } else if (!endOfOptions && arg.size() > 0 && arg[0] == '-') {
            std::cerr << "error: unknown option: " << arg << "\n";
            printInitUsage();
            return 1;
        } else {
            if (!projectName.empty()) {
                std::cerr << "error: unexpected argument: " << arg << "\n";
                return 1;
            }
            projectName = std::string(arg);
        }
    }

    if (projectName.empty()) {
        printInitUsage();
        return 1;
    }

    if (!validateProjectName(projectName))
        return 1;

    fs::path projectDir = fs::current_path() / zanna::filesystem::pathFromUtf8(projectName);

    if (fs::exists(projectDir)) {
        std::cerr << "error: directory '" << projectName << "' already exists\n";
        return 1;
    }

    // Create project directory.
    std::error_code ec;
    if (!fs::create_directory(projectDir, ec) || ec) {
        std::cerr << "error: could not create directory '" << projectName << "': " << ec.message()
                  << "\n";
        return 1;
    }

    // Generate zanna.project manifest.
    std::string entryFile = (lang == "basic") ? "main.bas" : "main.zia";
    std::string manifest = "project " + projectName + "\n" + "version 0.1.0\n" + "lang " + lang +
                           "\n" + "entry " + entryFile + "\n" + "profile balanced\n" +
                           "optimize O1\n";

    if (!writeFile(projectDir / "zanna.project", manifest)) {
        fs::remove_all(projectDir, ec);
        return 1;
    }

    // Generate entry-point source file.
    std::string source;
    if (lang == "zia") {
        source = "module main;\n"
                 "\n"
                 "bind Zanna.Terminal;\n"
                 "\n"
                 "func start() {\n"
                 "    Say(\"Hello from " +
                 projectName +
                 "!\");\n"
                 "}\n";
    } else {
        source = "PRINT \"Hello from " + projectName + "!\"\n";
    }

    if (!writeFile(projectDir / entryFile, source)) {
        fs::remove_all(projectDir, ec);
        return 1;
    }

    std::cerr << "Created " << lang << " project '" << projectName << "'\n"
              << "\n"
              << "  " << projectName << "/zanna.project\n"
              << "  " << projectName << "/" << entryFile << "\n"
              << "\n"
              << "Run with:  zanna run " << commandPathForProjectName(projectName) << "\n";

    return 0;
}
