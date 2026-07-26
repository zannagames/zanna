//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the CLI entry point that tokenises BASIC source files and prints
// their tokens for golden tests. The tool reuses the shared loader utilities so
// diagnostics match other BASIC tooling.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides the BASIC lexical dump developer utility.
/// @details Sharing usage text and file-loading logic with the other BASIC
///          helpers keeps the tooling suite uniform.

#include "common/Utf8CommandLine.hpp"
#include "frontends/basic/Lexer.hpp"
#include "frontends/basic/Token.hpp"
#include "support/source_manager.hpp"
#include "tools/basic/common.hpp"

#include <iostream>
#include <optional>
#include <string>

using namespace il::frontends::basic;
using namespace il::support;
using il::tools::basic::loadBasicSource;

/// @brief Tool entry point that dumps BASIC source tokens for golden tests.
///
/// @details Control flow mirrors the other BASIC developer tools:
///          1. Validate that a single path argument is supplied.
///          2. Load the file via @ref il::tools::basic::loadBasicSource so
///             diagnostics stay consistent with the rest of the toolchain.
///          3. Run the BASIC lexer until
///             @ref il::frontends::basic::TokenKind::EndOfFile is encountered.
///          4. Print each token as `<line>:<column> <token-kind> [<lexeme>]`,
///             emitting lexemes only for identifiers, literals, and strings.
///          Any failure to load the source file results in an error message and
///          a non-zero exit status so the calling scripts can detect the issue.
///
/// @param argc Argument count supplied by the C runtime.
/// @param argv Argument vector supplied by the C runtime.
/// @return Zero on success; one when command-line conversion or loading fails.
int main(int argc, char **argv) {
    zanna::tools::Utf8CommandLine commandLine(argc, argv);
    if (!commandLine.applyOrReport(argc, argv))
        return 1;
    std::string src;
    SourceManager sm;
    std::optional<std::uint32_t> fileId = loadBasicSource(argc == 2 ? argv[1] : nullptr, src, sm);
    if (!fileId) {
        return 1;
    }

    Lexer lex(src, *fileId);
    for (Token t = lex.next();; t = lex.next()) {
        std::cout << t.loc.line << ":" << t.loc.column << " " << tokenKindToString(t.kind);
        if (t.kind == TokenKind::Number || t.kind == TokenKind::String ||
            t.kind == TokenKind::Identifier)
            std::cout << " " << t.lexeme;
        std::cout << "\n";
        if (t.kind == TokenKind::EndOfFile)
            break;
    }
    return 0;
}
