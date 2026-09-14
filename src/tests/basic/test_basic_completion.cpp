//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/basic/test_basic_completion.cpp
// Purpose: Unit tests for the BasicCompletionEngine.
// Key invariants:
//   - Engine returns filtered/ranked CompletionItem results
//   - Keywords, builtins, snippets, and scope symbols are all providers
//   - The builtin list offers exactly the builtins the compiler accepts
//   - Dot-trigger invokes member completion from OopIndex or runtime
// Ownership/Lifetime:
//   - Test-only file
// Links: frontends/basic/BasicCompletion.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/BasicCompletion.hpp"
#include "frontends/basic/BuiltinRegistry.hpp"
#include "tests/TestHarness.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace il::frontends::basic;

// ===== Keyword completions =====

TEST(BasicCompletion, KeywordPrefixMatch) {
    BasicCompletionEngine engine;
    // "PRI" should match PRINT and PRIVATE
    auto items = engine.complete("PRI\n", 1, 4, "test.bas");
    bool foundPrint = false;
    for (const auto &item : items) {
        if (item.label == "PRINT")
            foundPrint = true;
    }
    EXPECT_TRUE(foundPrint);
}

TEST(BasicCompletion, KeywordFullList) {
    BasicCompletionEngine engine;
    // Empty prefix at start of line should return many completions
    auto items = engine.complete("\n", 1, 1, "test.bas");
    // Should include common keywords like DIM, IF, FOR, PRINT, etc.
    EXPECT_TRUE(items.size() > 10u);
}

// ===== Builtin function completions =====

TEST(BasicCompletion, BuiltinFunctions) {
    BasicCompletionEngine engine;
    // "LE" prefix should match LEFT$, LEN, etc.
    auto items = engine.complete("LE\n", 1, 3, "test.bas");
    bool foundLen = false;
    for (const auto &item : items) {
        if (item.label == "LEN")
            foundLen = true;
    }
    EXPECT_TRUE(foundLen);
}

TEST(BasicCompletion, BuiltinListMatchesRegistry) {
    BasicCompletionEngine engine;
    const auto items = engine.complete("\n", 1, 1, "test.bas", 0);
    std::set<std::string> labels;
    for (const auto &item : items)
        labels.insert(item.label);

    // Every builtin the compiler accepts is offered.
    using Builtin = BuiltinCallExpr::Builtin;
    std::string missing;
    for (std::size_t ordinal = 0; ordinal <= static_cast<std::size_t>(Builtin::Err); ++ordinal) {
        const std::string name = getBuiltinInfo(static_cast<Builtin>(ordinal)).name;
        if (!labels.contains(name))
            missing += name + " ";
    }
    EXPECT_EQ(missing, std::string{});

    // Every offered builtin function is accepted. Qualified names are runtime procedures,
    // and LBOUND and UBOUND are keyword intrinsics.
    std::string unknown;
    for (const auto &item : items) {
        if (item.kind != CompletionKind::Function || item.label.find('.') != std::string::npos ||
            item.label == "LBOUND" || item.label == "UBOUND")
            continue;
        if (!lookupBuiltin(item.label))
            unknown += item.label + " ";
    }
    EXPECT_EQ(unknown, std::string{});
}

// ===== Scope symbol completions =====

TEST(BasicCompletion, ScopeVariables) {
    BasicCompletionEngine engine;
    // BASIC lexer uppercases: "myVariable" → "MYVARIABLE"
    std::string source = "DIM myVariable AS INTEGER\nm\n";
    auto items = engine.complete(source, 2, 2, "test.bas");
    bool foundMyVar = false;
    for (const auto &item : items) {
        if (item.label == "MYVARIABLE")
            foundMyVar = true;
    }
    EXPECT_TRUE(foundMyVar);
}

TEST(BasicCompletion, ScopeProcedures) {
    BasicCompletionEngine engine;
    // BASIC lexer uppercases: "MyProc" → "MYPROC"
    std::string source = "SUB MyProc()\nEND SUB\nM\n";
    auto items = engine.complete(source, 3, 2, "test.bas");
    bool foundProc = false;
    for (const auto &item : items) {
        if (item.label == "MYPROC" || item.label == "MyProc")
            foundProc = true;
    }
    EXPECT_TRUE(foundProc);
}

TEST(BasicCompletion, RuntimeMemberCarriesAuthoredClassDocumentation) {
    BasicCompletionEngine engine;
    auto items = engine.complete("Zanna.Terminal.Sa\n", 1, 18, "test.bas", 0);
    const auto say = std::find_if(
        items.begin(), items.end(), [](const CompletionItem &item) { return item.label == "Say"; });
    ASSERT_NE(say, items.end());
    EXPECT_TRUE(say->documentation.find("terminal input, output, styling") != std::string::npos);
    EXPECT_TRUE(say->documentation.find("`Zanna.Terminal`") != std::string::npos);
}

// ===== No-crash edge cases =====

TEST(BasicCompletion, EmptySource) {
    BasicCompletionEngine engine;
    auto items = engine.complete("", 1, 1, "test.bas");
    // Should not crash; may return keywords
    (void)items;
}

TEST(BasicCompletion, CursorBeyondSource) {
    BasicCompletionEngine engine;
    auto items = engine.complete("PRINT 42\n", 100, 100, "test.bas");
    // Should not crash
    (void)items;
}

TEST(BasicCompletion, ClearCache) {
    BasicCompletionEngine engine;
    // Should not crash
    engine.clearCache();
    auto items = engine.complete("DIM x AS INTEGER\n\n", 2, 1, "test.bas");
    (void)items;
}

TEST(BasicCompletion, CacheKeyIncludesFilePath) {
    namespace fs = std::filesystem;

    const fs::path tempRoot = fs::temp_directory_path() / "basic_completion_cache_paths";
    fs::remove_all(tempRoot);

    const fs::path dirA = tempRoot / "a";
    const fs::path dirB = tempRoot / "b";
    fs::create_directories(dirA);
    fs::create_directories(dirB);

    {
        std::ofstream(dirA / "inc.bas") << "DIM Apple AS INTEGER\n";
        std::ofstream(dirB / "inc.bas") << "DIM Apricot AS INTEGER\n";
    }

    const std::string source = "ADDFILE \"inc.bas\"\nA\n";
    BasicCompletionEngine engine;
    auto itemsA = engine.complete(source, 2, 2, (dirA / "main.bas").string(), 0);
    auto itemsB = engine.complete(source, 2, 2, (dirB / "main.bas").string(), 0);

    bool foundAppleA = false;
    bool foundApricotA = false;
    for (const auto &item : itemsA) {
        if (item.label == "APPLE")
            foundAppleA = true;
        if (item.label == "APRICOT")
            foundApricotA = true;
    }

    bool foundApricotB = false;
    for (const auto &item : itemsB) {
        if (item.label == "APRICOT")
            foundApricotB = true;
    }

    EXPECT_TRUE(foundAppleA);
    EXPECT_FALSE(foundApricotA);
    EXPECT_TRUE(foundApricotB);

    fs::remove_all(tempRoot);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
