//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file cmd_eval.cpp
/// @brief Implements single-snippet Zia and BASIC evaluation through fresh REPL sessions.
///
/// The command evaluates exactly one input, optionally reads it from bounded stdin, and emits
/// either human output or one structured JSON object. Diagnostics remain on stderr. Exit codes
/// distinguish usage errors, compile/evaluation failures, and runtime traps. The selected language
/// adapter is owned only for the command duration.
//
//===----------------------------------------------------------------------===//

#include "repl/BasicReplAdapter.hpp"
#include "repl/ReplSession.hpp"
#include "repl/ZiaReplAdapter.hpp"
#include "support/diag_expected.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kMaxEvalStdinBytes = 4ULL * 1024ULL * 1024ULL;

/// @brief Print usage for `zanna eval` to the given stream.
/// @param os Destination stream.
void printEvalUsage(std::ostream &os) {
    os << "Usage: zanna eval [options] [code]\n"
       << "\n"
       << "Evaluate a single Zia or BASIC snippet and print the result.\n"
       << "Reads the snippet from stdin when no code argument is given.\n"
       << "\n"
       << "Options:\n"
       << "  --lang zia|basic   Snippet language (default: zia)\n"
       << "  --json             Emit a structured JSON result object on stdout\n"
       << "  --type             Include the inferred expression type (Zia only)\n"
       << "  --il               Include the generated IL (Zia only)\n"
       << "  -h, --help         Show this help\n"
       << "\n"
       << "Exit codes:\n"
       << "  0  evaluation succeeded\n"
       << "  1  usage error\n"
       << "  2  compile or evaluation error\n"
       << "  3  runtime trap\n"
       << "\n"
       << "Examples:\n"
       << "  zanna eval '2 + 3 * 4'\n"
       << "  zanna eval --json --type 'Zanna.Math.Sqrt(2.0)'\n"
       << "  echo 'Say(\"hi\")' | zanna eval\n";
}

/// @brief Map a REPL result type to its stable JSON string name.
/// @param type Adapter result category.
/// @return Stable JSON-facing type spelling.
std::string_view resultTypeName(zanna::repl::ResultType type) {
    switch (type) {
        case zanna::repl::ResultType::None:
            return "none";
        case zanna::repl::ResultType::Statement:
            return "statement";
        case zanna::repl::ResultType::Integer:
            return "Integer";
        case zanna::repl::ResultType::Number:
            return "Number";
        case zanna::repl::ResultType::String:
            return "String";
        case zanna::repl::ResultType::Boolean:
            return "Boolean";
        case zanna::repl::ResultType::Object:
            return "Object";
    }
    return "none";
}

/// @brief Emit a string as a JSON-escaped, double-quoted literal.
/// @param os Destination stream.
/// @param text Unquoted bytes to escape.
void printJsonString(std::ostream &os, std::string_view text) {
    il::support::printJsonStringEscaped(os, text);
}

/// @brief Read an eval snippet from stdin with a hard byte limit.
/// @details `zanna eval` is intended for short snippets. Bounding stdin keeps an
///          accidental pipe of a large file from forcing unbounded memory growth
///          before the compiler ever sees the input.
/// @param out Receives the complete stdin contents on success.
/// @param err Receives a user-facing failure message on read error or overflow.
/// @return True when stdin was read completely within the size limit.
bool readEvalStdin(std::string &out, std::string &err) {
    out.clear();
    char buffer[8192];
    while (std::cin) {
        std::cin.read(buffer, sizeof(buffer));
        const std::streamsize n = std::cin.gcount();
        if (n > 0) {
            if (out.size() > kMaxEvalStdinBytes - static_cast<std::size_t>(n)) {
                err = "stdin input for eval exceeds 4 MiB limit";
                return false;
            }
            out.append(buffer, static_cast<std::size_t>(n));
        }
    }
    if (std::cin.bad()) {
        err = "failed reading eval input from stdin";
        return false;
    }
    return true;
}

} // namespace

/// @brief Entry point for `zanna eval [options] [code]`.
/// @param argc Argument count (after "eval" is stripped).
/// @param argv Argument vector.
/// @return 0 on success, 1 on usage error, 2 on compile/eval error, 3 on trap.
int cmdEval(int argc, char **argv) {
    std::string lang = "zia";
    std::string code;
    bool haveCode = false;
    bool jsonOutput = false;
    bool wantType = false;
    bool wantIL = false;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printEvalUsage(std::cout);
            return 0;
        } else if (arg == "--json") {
            jsonOutput = true;
        } else if (arg == "--type") {
            wantType = true;
        } else if (arg == "--il") {
            wantIL = true;
        } else if (arg == "--lang") {
            if (i + 1 >= argc) {
                std::cerr << "error: --lang requires 'zia' or 'basic'\n";
                return 1;
            }
            lang = argv[++i];
        } else if (arg.substr(0, 7) == "--lang=") {
            lang = std::string(arg.substr(7));
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown flag: " << arg << "\n";
            printEvalUsage(std::cerr);
            return 1;
        } else if (!haveCode) {
            code = std::string(arg);
            haveCode = true;
        } else {
            std::cerr << "error: multiple code arguments; quote the snippet as one argument\n";
            return 1;
        }
    }

    if (lang != "zia" && lang != "basic") {
        std::cerr << "error: --lang must be 'zia' or 'basic'\n";
        return 1;
    }
    if (lang != "zia" && (wantType || wantIL)) {
        std::cerr << "error: --type and --il are only supported with --lang zia\n";
        return 1;
    }

    if (!haveCode) {
        std::string readError;
        if (!readEvalStdin(code, readError)) {
            std::cerr << "error: " << readError << "\n";
            return 1;
        }
    }
    if (code.empty()) {
        std::cerr << "error: no code to evaluate (pass a snippet or pipe it on stdin)\n";
        return 1;
    }

    std::unique_ptr<zanna::repl::ReplAdapter> adapter;
    if (lang == "basic")
        adapter = std::make_unique<zanna::repl::BasicReplAdapter>();
    else
        adapter = std::make_unique<zanna::repl::ZiaReplAdapter>();

    // Type/IL queries run before evaluation so they reflect the same session
    // state the snippet itself sees (an empty session).
    std::string exprType;
    if (wantType)
        exprType = adapter->getExprType(code);
    std::string ilText;
    if (wantIL)
        ilText = adapter->getIL(code);

    auto result = adapter->eval(code);

    if (jsonOutput) {
        std::ostream &os = std::cout;
        os << "{\"success\":" << (result.success ? "true" : "false")
           << ",\"trapped\":" << (result.trapped ? "true" : "false") << ",\"resultType\":";
        printJsonString(os, resultTypeName(result.resultType));
        os << ",\"output\":";
        printJsonString(os, result.output);
        os << ",\"error\":";
        printJsonString(os, result.errorMessage);
        if (wantType) {
            os << ",\"type\":";
            printJsonString(os, exprType);
        }
        if (wantIL) {
            os << ",\"il\":";
            printJsonString(os, ilText);
        }
        os << "}\n";
    } else {
        if (wantType)
            std::cout << "type: " << exprType << "\n";
        if (!result.output.empty()) {
            std::cout << result.output;
            if (result.output.back() != '\n')
                std::cout << '\n';
        }
        if (wantIL)
            std::cout << ilText << (ilText.empty() || ilText.back() == '\n' ? "" : "\n");
        if (!result.success && !result.errorMessage.empty())
            std::cerr << result.errorMessage << "\n";
    }

    if (result.trapped)
        return 3;
    return result.success ? 0 : 2;
}
