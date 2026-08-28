//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/zbasic-server/test_basic_compiler_bridge.cpp
// Purpose: Integration tests for BasicCompilerBridge facade methods.
// Key invariants:
//   - Each test uses small, self-contained BASIC source snippets
//   - Tests exercise the full compiler pipeline through the bridge
//   - Runtime query tests verify the inherited default implementations
// Ownership/Lifetime:
//   - Test-only file
// Links: tools/zbasic-server/BasicCompilerBridge.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"
#include "tools/zbasic-server/BasicCompilerBridge.hpp"

#include <algorithm>
#include <string>

using namespace zanna::server;

// ===== check() =====

TEST(BasicBridge, CheckValidSource) {
    BasicCompilerBridge bridge;
    auto diags = bridge.check("PRINT 42\nEND\n", "test.bas");
    for (const auto &d : diags) {
        EXPECT_TRUE(d.severity != 2);
    }
}

TEST(BasicBridge, CheckReportsError) {
    BasicCompilerBridge bridge;
    auto diags = bridge.check("PRINT x + \nEND\n", "test.bas");
    EXPECT_TRUE(diags.size() > 0u);
    bool hasError = false;
    for (const auto &d : diags) {
        if (d.severity == 2)
            hasError = true;
    }
    EXPECT_TRUE(hasError);
}

TEST(BasicBridge, CheckReturnsDiagnosticFields) {
    BasicCompilerBridge bridge;
    auto diags = bridge.check("PRINT x + \nEND\n", "test.bas");
    EXPECT_TRUE(diags.size() > 0u);
    EXPECT_TRUE(!diags[0].message.empty());
}

// ===== compile() =====

TEST(BasicBridge, CompileValidSource) {
    BasicCompilerBridge bridge;
    auto result = bridge.compile("PRINT 42\nEND\n", "test.bas");
    EXPECT_TRUE(result.succeeded);
    for (const auto &d : result.diagnostics) {
        EXPECT_TRUE(d.severity != 2);
    }
}

TEST(BasicBridge, CompileInvalidSource) {
    BasicCompilerBridge bridge;
    auto result = bridge.compile("PRINT x + \nEND\n", "test.bas");
    // May or may not succeed depending on error recovery, but should not crash
    (void)result;
}

// ===== completions() =====

TEST(BasicBridge, CompletionsReturnsResults) {
    BasicCompilerBridge bridge;
    std::string source = "DIM x AS INTEGER\nPRI\n";
    auto items = bridge.completions(source, 2, 4, "test.bas");
    // Should return keyword completions matching "PRI" prefix (PRINT, PRIVATE, etc.)
    EXPECT_TRUE(items.size() > 0u);
}

TEST(BasicBridge, RuntimeCompletionCarriesAuthoredClassDocumentation) {
    BasicCompilerBridge bridge;
    auto items = bridge.completions("Zanna.Terminal.Sa\n", 1, 18, "test.bas");
    const auto say = std::find_if(
        items.begin(), items.end(), [](const CompletionInfo &item) { return item.label == "Say"; });
    ASSERT_NE(say, items.end());
    EXPECT_TRUE(say->documentation.find("terminal input, output, styling") != std::string::npos);
    EXPECT_TRUE(say->documentation.find("`Zanna.Terminal`") != std::string::npos);
}

TEST(BasicBridge, CompletionsAtEmptyPosition) {
    BasicCompilerBridge bridge;
    std::string source = "DIM x AS INTEGER\n\n";
    auto items = bridge.completions(source, 2, 1, "test.bas");
    // Should return completions (keywords, builtins, etc.)
    // Even if zero, the call should not crash
    (void)items;
}

// ===== hover() =====

TEST(BasicBridge, HoverOnVariable) {
    BasicCompilerBridge bridge;
    // Line 2: "PRINT x" — cursor on 'x' at col 7
    // Note: BASIC lexer uppercases all identifiers, so hover returns "X" not "x"
    std::string source = "DIM x AS INTEGER\nPRINT x\nEND\n";
    auto result = bridge.hover(source, 2, 7, "test.bas");
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("X") != std::string::npos);
    EXPECT_TRUE(result.find("INTEGER") != std::string::npos);
}

TEST(BasicBridge, HoverOnRuntimeClassVariableIncludesAuthoredDocumentation) {
    BasicCompilerBridge bridge;
    std::string source = "DIM file AS Zanna.IO.File\nPRINT file\nEND\n";
    auto result = bridge.hover(source, 2, 7, "test.bas");
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("whole-file I/O, metadata") != std::string::npos);
    EXPECT_TRUE(result.find("`Zanna.IO.File`") != std::string::npos);
}

TEST(BasicBridge, HoverOnConst) {
    BasicCompilerBridge bridge;
    // CONST names are already uppercase in source, so they match directly
    std::string source = "CONST MAX = 100\nPRINT MAX\nEND\n";
    auto result = bridge.hover(source, 2, 7, "test.bas");
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("MAX") != std::string::npos);
    EXPECT_TRUE(result.find("CONST") != std::string::npos);
}

TEST(BasicBridge, HoverOnSubName) {
    BasicCompilerBridge bridge;
    // BASIC lexer uppercases: "Hello" → "HELLO"
    std::string source = "SUB Hello()\n  PRINT \"hi\"\nEND SUB\nHello\nEND\n";
    auto result = bridge.hover(source, 4, 1, "test.bas");
    // Should find the SUB declaration (uppercase)
    if (!result.empty()) {
        EXPECT_TRUE(result.find("HELLO") != std::string::npos);
        EXPECT_TRUE(result.find("SUB") != std::string::npos);
    }
}

TEST(BasicBridge, HoverOnWhitespace) {
    BasicCompilerBridge bridge;
    std::string source = "DIM x AS INTEGER\n   PRINT x\nEND\n";
    auto result = bridge.hover(source, 2, 1, "test.bas");
    EXPECT_TRUE(result.empty());
}

TEST(BasicBridge, HoverOnInvalidSource) {
    BasicCompilerBridge bridge;
    auto result = bridge.hover("this is not valid basic", 1, 1, "test.bas");
    // Should not crash; empty result is acceptable
    (void)result;
}

// ===== symbols() =====

TEST(BasicBridge, SymbolsListsDeclarations) {
    BasicCompilerBridge bridge;
    // BASIC lexer uppercases all identifiers
    auto syms = bridge.symbols("DIM x AS INTEGER\nPRINT x\nEND\n", "test.bas");
    bool foundX = false;
    for (const auto &s : syms) {
        if (s.name == "X")
            foundX = true;
    }
    EXPECT_TRUE(foundX);
}

TEST(BasicBridge, SymbolsIncludesProcedures) {
    BasicCompilerBridge bridge;
    // BASIC lexer uppercases: "Hello" → "HELLO"
    auto syms = bridge.symbols("SUB Hello()\n  PRINT \"hi\"\nEND SUB\nHello\nEND\n", "test.bas");
    bool foundHello = false;
    for (const auto &s : syms) {
        if (s.name == "HELLO" || s.name == "Hello")
            foundHello = true;
    }
    EXPECT_TRUE(foundHello);
}

// ===== dumpIL() =====

TEST(BasicBridge, DumpILValidSource) {
    BasicCompilerBridge bridge;
    auto il = bridge.dumpIL("PRINT 42\nEND\n", "test.bas", false);
    EXPECT_TRUE(il.find("Compilation failed") == std::string::npos);
    EXPECT_TRUE(!il.empty());
}

TEST(BasicBridge, DumpILInvalidSource) {
    BasicCompilerBridge bridge;
    auto il = bridge.dumpIL("PRINT x + \nEND\n", "test.bas", false);
    // Should contain error message or partial output — should not crash
    (void)il;
}

// ===== dumpAst() =====

TEST(BasicBridge, DumpAstValidSource) {
    BasicCompilerBridge bridge;
    auto ast = bridge.dumpAst("PRINT 42\nEND\n", "test.bas");
    EXPECT_TRUE(!ast.empty());
    EXPECT_TRUE(ast != "(no AST produced)");
}

TEST(BasicBridge, DumpAstInvalidSyntax) {
    BasicCompilerBridge bridge;
    auto ast = bridge.dumpAst("this is not valid basic source", "test.bas");
    // May produce partial AST or "(no AST produced)" — should not crash
    (void)ast;
}

// ===== dumpTokens() =====

TEST(BasicBridge, DumpTokensValidSource) {
    BasicCompilerBridge bridge;
    auto tokens = bridge.dumpTokens("PRINT 42\nEND\n", "test.bas");
    EXPECT_TRUE(!tokens.empty());
    EXPECT_TRUE(tokens.find("PRINT") != std::string::npos ||
                tokens.find("Print") != std::string::npos);
}

TEST(BasicBridge, DumpTokensEmptySource) {
    BasicCompilerBridge bridge;
    auto tokens = bridge.dumpTokens("", "test.bas");
    // Empty source should produce empty or near-empty token output
    (void)tokens;
}

// ===== runtimeClasses() =====

TEST(BasicBridge, RuntimeClassesNotEmpty) {
    BasicCompilerBridge bridge;
    auto classes = bridge.runtimeClasses();
    EXPECT_TRUE(classes.size() > 0u);
}

TEST(BasicBridge, RuntimeClassesHaveNames) {
    BasicCompilerBridge bridge;
    auto classes = bridge.runtimeClasses();
    for (const auto &cls : classes) {
        EXPECT_TRUE(!cls.qname.empty());
    }
}

// ===== runtimeMembers() =====

TEST(BasicBridge, RuntimeMembersForKnownClass) {
    BasicCompilerBridge bridge;
    auto members = bridge.runtimeMembers("Zanna.Terminal");
    EXPECT_TRUE(members.size() > 0u);
    bool foundSay = false;
    for (const auto &m : members) {
        if (m.name == "Say")
            foundSay = true;
    }
    EXPECT_TRUE(foundSay);
}

TEST(BasicBridge, RuntimeMembersForUnknownClass) {
    BasicCompilerBridge bridge;
    auto members = bridge.runtimeMembers("NonExistent.Class");
    EXPECT_EQ(members.size(), 0u);
}

// ===== runtimeSearch() =====

TEST(BasicBridge, RuntimeSearchFindsResults) {
    BasicCompilerBridge bridge;
    auto results = bridge.runtimeSearch("Say");
    EXPECT_TRUE(results.size() > 0u);
}

TEST(BasicBridge, RuntimeSearchNoResults) {
    BasicCompilerBridge bridge;
    auto results = bridge.runtimeSearch("zzzzNonExistentApiNamezzzz");
    EXPECT_EQ(results.size(), 0u);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
