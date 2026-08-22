//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/golden/basic_to_il/BasicToIlBatchRunner.cpp
// Purpose: Runs BASIC-to-IL golden checks in one process to avoid per-case
//          compiler startup and CMake-script overhead in the default CTest set.
// Key invariants: Exact-output cases mirror check_il.cmake normalization;
//                 bounds cases mirror check_il_bounds.cmake normalization;
//                 contains cases mirror check_il_contains.cmake substring checks;
//                 goldens are rewritten only when UPDATE_GOLDEN is set.
// Ownership/Lifetime: The runner owns source/golden buffers for each case and
//                     constructs a fresh SourceManager per compilation.
// Links: src/tests/golden/basic_to_il/check_il.cmake,
//        src/tests/golden/basic_to_il/check_il_bounds.cmake,
//        src/tests/golden/basic_to_il/check_il_contains.cmake,
//        scripts/update_goldens.sh
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief In-process BASIC-to-IL golden test batch runner.
/// @details The default CTest entry for BASIC-to-IL goldens uses this binary to
///          compile every case without launching the `zanna` executable for each
///          file.  The old one-test-per-case CTest declarations remain available
///          through `ZANNA_ENABLE_INDIVIDUAL_BASIC_TO_IL_GOLDEN_TESTS`.
///          Setting `UPDATE_GOLDEN=1` in the environment rewrites mismatching
///          goldens instead of failing, which is how scripts/update_goldens.sh
///          refreshes the cases owned by this runner.

#include "frontends/basic/BasicCompiler.hpp"
#include "zanna/il/IO.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// @brief BASIC-to-IL golden case comparison mode.
enum class CaseKind {
    Compare,  ///< Compare normalized emitted IL against a golden file.
    Bounds,   ///< Compile with bounds checks and compare normalized emitted IL.
    Contains, ///< Assert that emitted IL contains or omits configured substrings.
};

/// @brief Single row from the generated BASIC-to-IL batch manifest.
struct TestCase {
    CaseKind kind{CaseKind::Compare}; ///< Comparison mode for this case.
    std::string name;                 ///< Stable case name used in diagnostics.
    std::string basicPath;            ///< BASIC source file path.
    std::string goldenPath;           ///< Golden IL file path, when required.
    std::string mustHave;             ///< Semicolon-separated required substrings.
    std::string mustNotHave;          ///< Semicolon-separated forbidden substrings.
};

/// @brief Result of compiling one BASIC source file to IL text.
struct CompileOutput {
    std::string il; ///< Serialized IL text produced by the BASIC compiler.
};

/// @brief Read an entire UTF-8 text file into memory.
/// @details Golden inputs are small, so the runner reads files eagerly and lets
///          standard streams handle platform newline conversion.  Missing or
///          unreadable files are reported with the path that failed.
/// @param path File path to read.
/// @return File contents, or `std::nullopt` when the file cannot be opened.
[[nodiscard]] std::optional<std::string> readTextFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: unable to open " << path << '\n';
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// @brief Overwrite a golden file with verbatim compiler output.
/// @details Mirrors the `file(WRITE)` in check_il.cmake: goldens keep the real
///          IL version header and the symbol names the compiler emits, so the
///          text written here is never the normalized comparison form.
/// @param path Golden file path to overwrite.
/// @param text Text to write.
/// @return True when the file was written successfully.
[[nodiscard]] bool writeTextFile(const std::string &path, const std::string &text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "error: unable to write " << path << '\n';
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

/// @brief Report whether golden files may be rewritten instead of compared.
/// @details Set by scripts/update_goldens.sh for the update pass only. Any
///          value other than an empty string or `0` enables rewriting, matching
///          how the CMake checkers treat a defined `UPDATE_GOLDEN`.
/// @return True when the environment requests golden updates.
[[nodiscard]] bool goldenUpdateEnabled() {
    const char *value = std::getenv("UPDATE_GOLDEN");
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

/// @brief Replace all occurrences of one substring in-place.
/// @details Used for the runtime-alias normalization shared with the historical
///          CMake golden checkers.  Empty needles are ignored to avoid infinite
///          replacement loops.
/// @param text Text to update.
/// @param needle Substring to search for.
/// @param replacement Replacement payload.
void replaceAll(std::string &text, std::string_view needle, std::string_view replacement) {
    if (needle.empty())
        return;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

/// @brief Normalize Windows and old-Mac line endings to LF.
/// @param text Text to normalize.
/// @return Copy of @p text containing only LF line endings.
[[nodiscard]] std::string normalizeNewlines(std::string text) {
    replaceAll(text, "\r\n", "\n");
    replaceAll(text, "\r", "\n");
    return text;
}

/// @brief Normalize the IL header version to the token used by goldens.
/// @details Mirrors the `^il X.Y.Z -> il VERSION` replacement in check_il.cmake
///          and check_il_bounds.cmake so version bumps do not churn golden tests.
/// @param text IL text to normalize.
/// @return Copy of @p text with the leading IL version replaced when present.
[[nodiscard]] std::string normalizeIlVersion(std::string text) {
    return std::regex_replace(text, std::regex("^il [0-9]+\\.[0-9]+\\.[0-9]+"), "il VERSION");
}

/// @brief Normalize legacy runtime helper aliases to canonical Zanna names.
/// @details The BASIC lowering goldens accept either legacy `rt_*` helper names
///          or the canonical `Zanna.*` symbols while the runtime namespace
///          transition remains dual-published.  Bounds-check cases additionally
///          normalize `rt_diag_assert`.
/// @param text IL text to normalize.
/// @param includeDiagAssert True when `rt_diag_assert` should be normalized.
/// @return Copy of @p text with aliased externs and calls rewritten.
[[nodiscard]] std::string normalizeRuntimeAliases(std::string text, bool includeDiagAssert) {
    struct AliasPair {
        std::string_view legacy;    ///< Legacy runtime symbol without `@`.
        std::string_view canonical; ///< Canonical runtime symbol without `@`.
    };

    const std::vector<AliasPair> aliases = {
        {"rt_print_str", "Zanna.Terminal.PrintStr"},
        {"rt_print_i64", "Zanna.Terminal.PrintI64"},
        {"rt_print_f64", "Zanna.Terminal.PrintF64"},
        {"rt_str_substr", "Zanna.String.Substring"},
        {"rt_trap_string", "Zanna.Diagnostics.Trap"},
        {"rt_trap", "Zanna.Diagnostics.Trap"},
        {"rt_str_concat", "Zanna.String.Concat"},
        {"rt_input_line", "Zanna.Terminal.ReadLine"},
        {"rt_to_int", "Zanna.Core.Convert.ToInt64"},
        {"rt_to_double", "Zanna.Core.Convert.ToDouble"},
        {"rt_parse_int64", "Zanna.Core.Parse.TryInt"},
        {"rt_parse_double", "Zanna.Core.Parse.TryDouble"},
        {"rt_int_to_str", "Zanna.Core.Convert.ToStringInt"},
        {"rt_f64_to_str", "Zanna.Core.Convert.ToStringDouble"},
        {"rt_str_split_fields", "Zanna.String.SplitFields"},
        {"rt_str_i16_alloc", "Zanna.String.FromI16"},
        {"rt_str_i32_alloc", "Zanna.String.FromI32"},
        {"rt_str_f_alloc", "Zanna.String.FromSingle"},
    };
    for (const auto &alias : aliases) {
        replaceAll(
            text, "@" + std::string(alias.legacy) + "(", "@" + std::string(alias.canonical) + "(");
        replaceAll(text,
                   "extern @" + std::string(alias.legacy),
                   "extern @" + std::string(alias.canonical));
    }
    if (includeDiagAssert) {
        replaceAll(text, "@rt_diag_assert(", "@Zanna.Diagnostics.Assert(");
        replaceAll(text, "extern @rt_diag_assert", "extern @Zanna.Diagnostics.Assert");
    }
    return text;
}

/// @brief Normalize IL for exact-output comparison cases.
/// @param text IL text from the compiler or golden file.
/// @return Text with line endings, IL version, and runtime aliases normalized.
[[nodiscard]] std::string normalizeCompareIl(std::string text) {
    text = normalizeNewlines(std::move(text));
    text = normalizeIlVersion(std::move(text));
    return normalizeRuntimeAliases(std::move(text), false);
}

/// @brief Normalize IL for bounds-checking comparison cases.
/// @param text IL text from the compiler or golden file.
/// @return Text with line endings, IL version, and bounds helper aliases normalized.
[[nodiscard]] std::string normalizeBoundsIl(std::string text) {
    text = normalizeNewlines(std::move(text));
    text = normalizeIlVersion(std::move(text));
    return normalizeRuntimeAliases(std::move(text), true);
}

/// @brief Return true when @p line is an extern declaration line.
/// @param line One IL line without its trailing newline.
/// @return True for lines beginning with `extern @`.
[[nodiscard]] bool isExternLine(std::string_view line) {
    constexpr std::string_view prefix = "extern @";
    return line.substr(0, prefix.size()) == prefix;
}

/// @brief Collect extern declaration lines from normalized IL text.
/// @param text Normalized IL text.
/// @return Sorted list of extern declaration lines.
[[nodiscard]] std::vector<std::string> sortedExternLines(const std::string &text) {
    std::vector<std::string> externs;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (isExternLine(line))
            externs.push_back(line);
    }
    std::sort(externs.begin(), externs.end());
    return externs;
}

/// @brief Remove extern declaration lines from normalized IL text.
/// @details Exact BASIC-to-IL goldens compare sorted extern sets separately
///          because runtime helper declaration order can vary across platforms.
/// @param text Normalized IL text.
/// @return Text with every `extern @...` line removed.
[[nodiscard]] std::string removeExternLines(const std::string &text) {
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        if (!isExternLine(line))
            out << line << '\n';
    }
    return out.str();
}

/// @brief Split a semicolon-separated CMake list into non-empty strings.
/// @param listText Semicolon-separated list text from the manifest.
/// @return Ordered list of non-empty entries.
[[nodiscard]] std::vector<std::string> splitSemicolonList(std::string_view listText) {
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start <= listText.size()) {
        const std::size_t end = listText.find(';', start);
        const std::size_t len =
            end == std::string_view::npos ? std::string_view::npos : end - start;
        std::string value(listText.substr(start, len));
        if (!value.empty())
            values.push_back(std::move(value));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return values;
}

/// @brief Split a manifest line on tabs while preserving empty trailing fields.
/// @param line One manifest row.
/// @return Fields in the row, including empty fields between adjacent tabs.
[[nodiscard]] std::vector<std::string> splitTabFields(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t end = line.find('\t', start);
        const std::size_t len =
            end == std::string_view::npos ? std::string_view::npos : end - start;
        fields.emplace_back(line.substr(start, len));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return fields;
}

/// @brief Parse a manifest case kind token.
/// @param token Kind token from the first manifest field.
/// @return Parsed kind, or `std::nullopt` for unknown tokens.
[[nodiscard]] std::optional<CaseKind> parseCaseKind(std::string_view token) {
    if (token == "compare")
        return CaseKind::Compare;
    if (token == "bounds")
        return CaseKind::Bounds;
    if (token == "contains")
        return CaseKind::Contains;
    return std::nullopt;
}

/// @brief Parse a non-negative decimal integer.
/// @param token Text to parse.
/// @return Parsed integer, or `std::nullopt` when the token is malformed.
[[nodiscard]] std::optional<int> parseNonNegativeInt(std::string_view token) {
    int value = 0;
    const auto *begin = token.data();
    const auto *end = token.data() + token.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < 0)
        return std::nullopt;
    return value;
}

/// @brief Load BASIC-to-IL batch cases from a generated manifest.
/// @details Each row is tab-separated:
///          `kind<TAB>name<TAB>basic<TAB>golden<TAB>must_have<TAB>must_not_have`.
///          Empty lines are ignored so generated files can end with a newline.
/// @param manifestPath Path to the generated manifest file.
/// @return Parsed cases, or `std::nullopt` when the manifest is malformed.
[[nodiscard]] std::optional<std::vector<TestCase>> loadCases(const std::string &manifestPath) {
    auto manifest = readTextFile(manifestPath);
    if (!manifest)
        return std::nullopt;

    std::vector<TestCase> cases;
    std::istringstream in(*manifest);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty())
            continue;
        auto fields = splitTabFields(line);
        if (fields.size() != 6) {
            std::cerr << "error: malformed manifest line " << lineNo << ": expected 6 fields\n";
            return std::nullopt;
        }
        auto kind = parseCaseKind(fields[0]);
        if (!kind) {
            std::cerr << "error: malformed manifest line " << lineNo << ": unknown kind '"
                      << fields[0] << "'\n";
            return std::nullopt;
        }
        cases.push_back(TestCase{
            *kind,
            std::move(fields[1]),
            std::move(fields[2]),
            std::move(fields[3]),
            std::move(fields[4]),
            std::move(fields[5]),
        });
    }
    return cases;
}

/// @brief Compile a BASIC file to serialized IL in-process.
/// @details Mirrors `zanna front basic -emit-il` for the pieces relevant to
///          golden tests: it reads the file, invokes @ref compileBasic, prints
///          BASIC diagnostics on failure, and serializes the resulting module.
/// @param sourcePath BASIC source file path.
/// @param boundsChecks Whether generated bounds checks should be enabled.
/// @return Serialized IL text, or `std::nullopt` when compilation fails.
[[nodiscard]] std::optional<CompileOutput> compileBasicToIl(const std::string &sourcePath,
                                                            bool boundsChecks) {
    auto source = readTextFile(sourcePath);
    if (!source)
        return std::nullopt;

    il::support::SourceManager sourceManager;
    il::frontends::basic::BasicCompilerOptions options{};
    options.boundsChecks = boundsChecks;
    il::frontends::basic::BasicCompilerInput input{*source, sourcePath};
    auto result = il::frontends::basic::compileBasic(input, options, sourceManager);
    if (!result.succeeded()) {
        if (result.emitter)
            result.emitter->printAll(std::cerr);
        else
            std::cerr << "error: BASIC compiler failed before diagnostics were initialized\n";
        return std::nullopt;
    }

    return CompileOutput{il::io::Serializer::toString(result.module)};
}

/// @brief Rewrite one case's golden file with the compiler's verbatim output.
/// @details Only reached when @ref goldenUpdateEnabled is true, so a normal test
///          run can never silently absorb drift.
/// @param test Case metadata naming the golden file.
/// @param actualIl Raw serialized IL produced by the compiler.
/// @return True when the golden was rewritten.
[[nodiscard]] bool updateGolden(const TestCase &test, const std::string &actualIl) {
    if (!writeTextFile(test.goldenPath, normalizeNewlines(actualIl)))
        return false;
    std::cout << "Updated golden: " << test.goldenPath << '\n';
    return true;
}

/// @brief Compare one normalized exact-output case.
/// @details Extern drift and body drift both reach the update branch, because a
///          newly always-declared runtime function changes only the extern set
///          of every golden and must still be refreshable.
/// @param test Case metadata.
/// @param actualIl Serialized IL produced by the compiler.
/// @return True when the normalized output matches the golden.
[[nodiscard]] bool runCompareCase(const TestCase &test, const std::string &actualIl) {
    auto expected = readTextFile(test.goldenPath);
    if (!expected)
        return false;

    const std::string actualNorm = normalizeCompareIl(actualIl);
    const std::string expectedNorm = normalizeCompareIl(*expected);
    const auto actualExterns = sortedExternLines(actualNorm);
    const auto expectedExterns = sortedExternLines(expectedNorm);
    const std::string actualBody = removeExternLines(actualNorm);
    const std::string expectedBody = removeExternLines(expectedNorm);
    if (actualExterns == expectedExterns && actualBody == expectedBody)
        return true;

    if (goldenUpdateEnabled())
        return updateGolden(test, actualIl);

    if (actualExterns != expectedExterns) {
        std::cerr << "FAIL: " << test.name << ": extern declarations mismatch\nExpected:\n";
        for (const auto &line : expectedExterns)
            std::cerr << line << '\n';
        std::cerr << "Got:\n";
        for (const auto &line : actualExterns)
            std::cerr << line << '\n';
        return false;
    }

    std::cerr << "FAIL: " << test.name << ": IL body mismatch\nExpected:\n"
              << expectedBody << "\nGot:\n"
              << actualBody;
    return false;
}

/// @brief Compare one normalized bounds-checking case.
/// @param test Case metadata.
/// @param actualIl Serialized IL produced by the compiler with bounds checks.
/// @return True when the normalized output matches the golden exactly.
[[nodiscard]] bool runBoundsCase(const TestCase &test, const std::string &actualIl) {
    auto expected = readTextFile(test.goldenPath);
    if (!expected)
        return false;
    const std::string actualNorm = normalizeBoundsIl(actualIl);
    const std::string expectedNorm = normalizeBoundsIl(*expected);
    if (actualNorm != expectedNorm) {
        if (goldenUpdateEnabled())
            return updateGolden(test, actualIl);
        std::cerr << "FAIL: " << test.name << ": bounds-check IL mismatch\nExpected:\n"
                  << expectedNorm << "\nGot:\n"
                  << actualNorm;
        return false;
    }
    return true;
}

/// @brief Run substring assertions for one contains-style case.
/// @param test Case metadata containing semicolon-separated expectations.
/// @param actualIl Serialized IL produced by the compiler.
/// @return True when every required substring is present and every forbidden
///         substring is absent.
[[nodiscard]] bool runContainsCase(const TestCase &test, const std::string &actualIl) {
    const std::string normalizedIl = normalizeNewlines(actualIl);
    for (const auto &needle : splitSemicolonList(test.mustHave)) {
        if (normalizedIl.find(needle) == std::string::npos) {
            std::cerr << "FAIL: " << test.name << ": IL does not contain expected token '" << needle
                      << "'\n"
                      << normalizedIl;
            return false;
        }
    }
    for (const auto &needle : splitSemicolonList(test.mustNotHave)) {
        if (normalizedIl.find(needle) != std::string::npos) {
            std::cerr << "FAIL: " << test.name << ": IL unexpectedly contains token '" << needle
                      << "'\n"
                      << normalizedIl;
            return false;
        }
    }
    return true;
}

/// @brief Execute one manifest case.
/// @param test Case metadata.
/// @return True when the case passes.
[[nodiscard]] bool runCase(const TestCase &test) {
    // `zanna front basic -emit-il` enables bounds checks by default.
    const bool boundsChecks = true;
    auto compiled = compileBasicToIl(test.basicPath, boundsChecks);
    if (!compiled) {
        std::cerr << "FAIL: " << test.name << ": BASIC compilation failed\n";
        return false;
    }

    switch (test.kind) {
        case CaseKind::Compare:
            return runCompareCase(test, compiled->il);
        case CaseKind::Bounds:
            return runBoundsCase(test, compiled->il);
        case CaseKind::Contains:
            return runContainsCase(test, compiled->il);
    }
    return false;
}

} // namespace

/// @brief Program entry point for the BASIC-to-IL batch runner.
/// @details Expects the generated manifest path and, optionally, a shard index
///          plus shard count.  Every case assigned to the shard is attempted so
///          failures are reported together rather than stopping at the first
///          mismatch.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return Zero when all cases pass; non-zero otherwise.
int main(int argc, char **argv) {
    if (argc != 2 && argc != 4) {
        std::cerr
            << "usage: test_basic_to_il_goldens_batch <cases.tsv> [shard-index shard-count]\n";
        return 2;
    }

    auto cases = loadCases(argv[1]);
    if (!cases)
        return 2;

    int shardIndex = 0;
    int shardCount = 1;
    if (argc == 4) {
        auto parsedShardIndex = parseNonNegativeInt(argv[2]);
        auto parsedShardCount = parseNonNegativeInt(argv[3]);
        if (!parsedShardIndex || !parsedShardCount || *parsedShardCount <= 0 ||
            *parsedShardIndex >= *parsedShardCount) {
            std::cerr << "error: invalid shard arguments\n";
            return 2;
        }
        shardIndex = *parsedShardIndex;
        shardCount = *parsedShardCount;
    }

    int failures = 0;
    int attempted = 0;
    for (std::size_t i = 0; i < cases->size(); ++i) {
        if (static_cast<int>(i % static_cast<std::size_t>(shardCount)) != shardIndex)
            continue;
        const auto &test = (*cases)[i];
        ++attempted;
        if (!runCase(test))
            ++failures;
    }
    if (failures != 0) {
        std::cerr << failures << " BASIC-to-IL golden case(s) failed\n";
        if (goldenUpdateEnabled()) {
            std::cerr << "note: UPDATE_GOLDEN cannot refresh compile failures or contains-style "
                         "cases, which have no golden file\n";
        }
        return 1;
    }
    std::cout << attempted << " BASIC-to-IL golden case(s) passed";
    if (shardCount > 1)
        std::cout << " in shard " << shardIndex << "/" << shardCount;
    std::cout << '\n';
    return 0;
}
