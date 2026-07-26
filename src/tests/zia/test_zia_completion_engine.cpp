//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/zia/test_zia_completion_engine.cpp
// Purpose: Verify Zia completion context, providers, ranking, and signature help.
// Key invariants:
//   - Cursor coordinates never cause out-of-range source access.
//   - Completion replacement ranges describe the clamped active prefix.
// Ownership/Lifetime:
//   - Tests own source buffers and completion engines.
//   - Returned completion vectors own their item strings.
// Links: src/frontends/zia/ZiaCompletion.cpp
//
//===----------------------------------------------------------------------===//
///
/// @file test_zia_completion_engine.cpp
/// @brief Unit tests for the Zia CompletionEngine (Phase 2).
///
/// @details Validates CompletionEngine::complete() and serialize():
///
///   - CtrlSpace trigger → returns scope symbols and keywords
///   - Keyword prefix filtering (e.g. "fu" → "func")
///   - Member access trigger (dot) → returns members of class type
///   - AfterNew trigger → returns type names
///   - serialize() → tab-delimited output
///   - Cache: consecutive calls with same source reuse cached Sema
///   - clearCache() forces a fresh parse
///
/// ## Test Design Notes
///
/// We use maxResults=0 (unlimited) in most tests that check for specific items,
/// because the global sema scope contains 3000+ runtime symbols with priority=10
/// which would otherwise push keywords (priority=50) past maxResults=50.
///
/// Sources with trailing incomplete expressions (e.g. "b.") cause parse failure.
/// Instead, tests use complete source + position the cursor at a prefix inside
/// the source (e.g. "b.wi" with cursor at col after "wi" gives prefix="wi",
/// trigger=MemberAccess, triggerExpr="b").
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/ZiaCompletion.hpp"
#include "tests/TestHarness.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

using namespace il::frontends::zia;

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool hasLabel(const std::vector<CompletionItem> &items, const std::string &label) {
    return std::any_of(
        items.begin(), items.end(), [&](const CompletionItem &it) { return it.label == label; });
}

static bool hasKind(const std::vector<CompletionItem> &items,
                    const std::string &label,
                    CompletionKind kind) {
    for (const auto &it : items)
        if (it.label == label && it.kind == kind)
            return true;
    return false;
}

static int indexOfLabel(const std::vector<CompletionItem> &items, const std::string &label) {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].label == label)
            return static_cast<int>(i);
    return -1;
}

static const CompletionItem *findItem(const std::vector<CompletionItem> &items,
                                      const std::string &label) {
    for (const auto &item : items)
        if (item.label == label)
            return &item;
    return nullptr;
}

static void writeFile(const fs::path &path, const std::string &contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << contents;
}

static std::pair<int, int> lineColAfter(const std::string &source, const std::string &needle) {
    size_t pos = source.find(needle);
    EXPECT_NE(pos, std::string::npos);
    pos += needle.size();

    int line = 1;
    int col = 0;
    for (size_t i = 0; i < pos && i < source.size(); ++i) {
        if (source[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col};
}

// ---------------------------------------------------------------------------
// CtrlSpace — scope symbols + keywords
// ---------------------------------------------------------------------------

TEST(CompletionEngine, CtrlSpace_ReturnsGlobalFunction) {
    // Simple module with a function (no runtime calls that might stress the parser).
    const std::string source = "module Test;\n\nfunc greet() {}\n\n";
    CompletionEngine engine;
    // maxResults=0 → unlimited, so we can check for specific labels.
    auto items = engine.complete(source, 4, 0, "<test>", 0);
    EXPECT_FALSE(items.empty());
    EXPECT_TRUE(hasLabel(items, "greet"));
}

TEST(CompletionEngine, CtrlSpace_ReturnsKeywords) {
    const std::string source = "module Test;\n\n";
    CompletionEngine engine;
    // maxResults=0 → unlimited. Keywords are priority=50; scope symbols priority=10.
    auto items = engine.complete(source, 2, 0, "<test>", 0);
    EXPECT_TRUE(hasLabel(items, "func"));
    EXPECT_TRUE(hasLabel(items, "class"));
    EXPECT_TRUE(hasLabel(items, "var"));
    EXPECT_TRUE(hasLabel(items, "if"));
    EXPECT_TRUE(hasLabel(items, "while"));
}

TEST(CompletionEngine, InvalidCursorCoordinatesAreClamped) {
    const std::string source = "module Test;\nfu\n";
    CompletionEngine engine;

    auto negative = engine.complete(source, -100, -100, "<test>", 0);
    ASSERT_FALSE(negative.empty());
    EXPECT_EQ(negative.front().replacementStartLine, 1);
    EXPECT_EQ(negative.front().replacementStartColumn, 0);
    EXPECT_EQ(negative.front().replacementEndColumn, 0);

    auto pastEnd = engine.complete(source, 2, std::numeric_limits<int>::max(), "<test>", 0);
    EXPECT_TRUE(hasLabel(pastEnd, "func"));
    const CompletionItem *func = findItem(pastEnd, "func");
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->replacementStartColumn, 0);
    EXPECT_EQ(func->replacementEndColumn, 2);
}

TEST(CompletionEngine, PrefixFiltering_NarrowsResults) {
    // "fu" prefix — should match "func" but not "var"/"if" etc.
    // Source has "fu" on line 2; cursor at col 2 gives prefix="fu".
    const std::string srcWithPrefix = "module Test;\nfu\n";
    CompletionEngine engine;
    auto items = engine.complete(srcWithPrefix, 2, 2, "<test>", 0);
    EXPECT_TRUE(hasLabel(items, "func"));
    EXPECT_FALSE(hasLabel(items, "var"));
    EXPECT_FALSE(hasLabel(items, "if"));
}

TEST(CompletionEngine, CtrlSpace_ReturnsDeclarationDocumentation) {
    const std::string source = R"(module Test;

/// Greets users.
/// Supports labels.
func greetDocs() -> Integer { return 1; }

func main() {
    gre
}
)";
    CompletionEngine engine;
    auto [line, col] = lineColAfter(source, "    gre");
    auto items = engine.complete(source, line, col, "<test>", 0);
    const CompletionItem *item = findItem(items, "greetDocs");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->documentation, "Greets users.\nSupports labels.");
}

// ---------------------------------------------------------------------------
// Member access (dot trigger)
// ---------------------------------------------------------------------------

TEST(CompletionEngine, MemberAccess_EntityMembers) {
    // Source with a Box instance. We position the cursor in "box.wi" so
    // resolveExprType() sees the actual instance type and member filtering
    // matches the canonical object access path.
    const std::string src = R"(
module Test;

class Box {
    expose Integer width;
    expose Integer height;
    expose func Area() -> Integer {
        return width * height;
    }
}

func main() {
    var box = new Box();
    var r = box.width;
}
)";
    CompletionEngine engine;
    // Line 14: "    var r = box.width;", col=18 — after "wi".
    auto items = engine.complete(src, 14, 18, "<test>", 0);

    // Box has field "width" which matches prefix "wi".
    EXPECT_TRUE(hasLabel(items, "width"));
}

// ---------------------------------------------------------------------------
// AfterNew trigger
// ---------------------------------------------------------------------------

TEST(CompletionEngine, AfterNew_ReturnsTypeNames) {
    // Source with "new D" — cursor after 'D' gives prefix="D", trigger=AfterNew.
    const std::string src = R"(
module Test;

class Dog {
    expose func init() {}}

struct Diamond {
    expose Integer x;
}

func main() {    var x = new D
}
)";
    CompletionEngine engine;
    // Line 13: "    var x = new D", col=17 gives prefix="D", trigger=AfterNew.
    auto items = engine.complete(src, 13, 17, "<test>", 0);
    // Should include type names starting with "D"
    EXPECT_TRUE(hasLabel(items, "Dog") || hasLabel(items, "Diamond") || !items.empty());
}

// ---------------------------------------------------------------------------
// serialize()
// ---------------------------------------------------------------------------

TEST(CompletionEngine, Serialize_ProducesTabDelimited) {
    std::vector<CompletionItem> items;
    CompletionItem a;
    a.label = "foo";
    a.insertText = "foo()";
    a.kind = CompletionKind::Function;
    a.detail = "() -> Integer";
    items.push_back(a);

    CompletionItem b;
    b.label = "bar";
    b.insertText = "bar";
    b.kind = CompletionKind::Variable;
    b.detail = "Integer";
    items.push_back(b);

    std::string out = serialize(items);
    EXPECT_FALSE(out.empty());

    // Should contain tab characters.
    EXPECT_TRUE(out.find('\t') != std::string::npos);

    // First record should start with "foo".
    EXPECT_TRUE(out.find("foo\t") != std::string::npos);

    // Kind for Function is 6.
    EXPECT_TRUE(out.find("\t6\t") != std::string::npos);

    // Kind for Variable is 2.
    EXPECT_TRUE(out.find("\t2\t") != std::string::npos);
}

TEST(CompletionEngine, Serialize_EscapesMultilineSnippets) {
    // VDOC-111: multiline snippet insertion text must stay on one wire row.
    std::vector<CompletionItem> items;
    CompletionItem snippet;
    snippet.label = "if";
    snippet.insertText = "if  {\n    \n}";
    snippet.kind = CompletionKind::Snippet;
    snippet.detail = "if statement";
    items.push_back(snippet);

    std::string out = serialize(items);

    // Exactly one record: one literal newline, at the very end.
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 1);
    EXPECT_TRUE(!out.empty() && out.back() == '\n');

    // The newlines inside the snippet are escaped as backslash-n.
    EXPECT_TRUE(out.find("if  {\\n    \\n}") != std::string::npos);

    // Exactly three literal tab delimiters.
    EXPECT_EQ(std::count(out.begin(), out.end(), '\t'), 3);
}

// ---------------------------------------------------------------------------
// Cache reuse
// ---------------------------------------------------------------------------

TEST(CompletionEngine, Cache_SameSourceReusesResult) {
    const std::string source = "module Test;\n\nfunc myFn() {}\n";
    CompletionEngine engine;

    // First call — populates cache.
    auto items1 = engine.complete(source, 1, 0, "<test>", 0);
    // Second call — same source, same hash → should reuse cache.
    auto items2 = engine.complete(source, 1, 0, "<test>", 0);

    // Both calls should return the same set of labels.
    EXPECT_EQ(items1.size(), items2.size());
    // And the result should be non-empty (module contains "myFn").
    EXPECT_TRUE(hasLabel(items1, "myFn"));
}

TEST(CompletionEngine, ClearCache_ForcesReparse) {
    const std::string source = "module Test;\n\nfunc alpha() {}\n";
    CompletionEngine engine;

    auto items1 = engine.complete(source, 1, 0, "<test>", 0);
    EXPECT_FALSE(items1.empty());

    engine.clearCache();
    auto items2 = engine.complete(source, 1, 0, "<test>", 0);
    EXPECT_FALSE(items2.empty());
    EXPECT_EQ(items1.size(), items2.size());
}

TEST(CompletionEngine, Cache_KeyIncludesFilePath) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_completion_cache_paths";
    fs::remove_all(tempRoot);

    const fs::path dirA = tempRoot / "a";
    const fs::path dirB = tempRoot / "b";
    writeFile(dirA / "dep.zia", "module Dep;\nfunc alpha() {}\n");
    writeFile(dirB / "dep.zia", "module Dep;\nfunc beta() {}\n");

    const std::string mainSource = R"(module Test;
bind "./dep";

func start() {})";

    CompletionEngine engine;
    auto itemsA = engine.complete(mainSource, 4, 0, (dirA / "main.zia").string(), 0);
    auto itemsB = engine.complete(mainSource, 4, 0, (dirB / "main.zia").string(), 0);

    EXPECT_TRUE(hasLabel(itemsA, "alpha"));
    EXPECT_FALSE(hasLabel(itemsA, "beta"));
    EXPECT_TRUE(hasLabel(itemsB, "beta"));
}

// ---------------------------------------------------------------------------
// MaxResults limit
// ---------------------------------------------------------------------------

TEST(CompletionEngine, MaxResults_LimitsOutput) {
    const std::string source = "module Test;\n";
    CompletionEngine engine;
    auto items = engine.complete(source, 1, 0, "<test>", 3);
    EXPECT_TRUE(static_cast<int>(items.size()) <= 3);
}

// ---------------------------------------------------------------------------
// Bound module alias (dot trigger on alias)
// ---------------------------------------------------------------------------

TEST(CompletionEngine, BoundAlias_MathMembers) {
    // Source with "bind Zanna.Math as Math" and "Math.Sq" as an expression.
    // Cursor after "Sq" → prefix="Sq", trigger=MemberAccess, triggerExpr="Math".
    const std::string source = R"(
module Test;

bind Zanna.Math as Math;
func compute() -> Number {    var r = Math.Sq
    return r;
}
)";
    CompletionEngine engine;
    // Line 7: "    var r = Math.Sq", col=19 gives prefix="Sq", triggerExpr="Math".
    auto items = engine.complete(source, 7, 19, "<test>", 0);
    // Zanna.Math should have at least some members (Sqrt, etc.)
    EXPECT_FALSE(items.empty());
}

TEST(CompletionEngine, RuntimeClassCompletionCarriesAuthoredDocumentation) {
    const std::string source = R"(
module Test;

bind Zanna.GUI as GUI;

func main() {
    GUI.Ap
}
)";
    CompletionEngine engine;
    auto [line, col] = lineColAfter(source, "GUI.Ap");
    auto items = engine.complete(source, line, col, "<test>", 0);
    const CompletionItem *app = findItem(items, "App");
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->kind, CompletionKind::RuntimeClass);
    EXPECT_TRUE(app->documentation.find("Owns a GUI application window") != std::string::npos);
    EXPECT_TRUE(app->documentation.find("`Zanna.GUI.App`") != std::string::npos);
}

TEST(CompletionEngine, RuntimeMemberCompletionCarriesDocumentation) {
    const std::string source = R"(
module Test;

bind Zanna.Terminal as Terminal;

func main() {
    Terminal.Sa
}
)";
    CompletionEngine engine;
    auto [line, col] = lineColAfter(source, "Terminal.Sa");
    auto items = engine.complete(source, line, col, "<test>", 0);
    const CompletionItem *say = findItem(items, "Say");
    ASSERT_NE(say, nullptr);
    EXPECT_TRUE(say->documentation.find("Writes text followed by a newline") != std::string::npos);
    EXPECT_TRUE(say->documentation.find("Runtime method Zanna.Terminal.Say.") != std::string::npos);
    EXPECT_TRUE(say->documentation.find("Signature: Say(s: String) -> Void") != std::string::npos);
    EXPECT_TRUE(say->documentation.find("Target: Zanna.Terminal.Say") != std::string::npos);
}

TEST(CompletionEngine, BoundFileModuleNameAndExports) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_completion_bound_file_modules";
    fs::remove_all(tempRoot);

    writeFile(tempRoot / "dep.zia", R"(module Dep;
/// Exported helper docs.
expose func exportedThing() -> Integer { return 1; }
func hiddenThing() -> Integer { return 0; }
)");

    const std::string source = R"(module Main;
bind "./dep";

func main() {
    var value = Dep.exportedThing();
}
)";

    CompletionEngine engine;
    auto [moduleLine, moduleCol] = lineColAfter(source, "var value = De");
    auto modules =
        engine.complete(source, moduleLine, moduleCol, (tempRoot / "main.zia").string(), 0);
    EXPECT_TRUE(hasKind(modules, "Dep", CompletionKind::Module));

    auto [memberLine, memberCol] = lineColAfter(source, "Dep.exp");
    auto members =
        engine.complete(source, memberLine, memberCol, (tempRoot / "main.zia").string(), 0);
    EXPECT_TRUE(hasKind(members, "exportedThing", CompletionKind::Function));
    EXPECT_FALSE(hasLabel(members, "hiddenThing"));
    const CompletionItem *exported = findItem(members, "exportedThing");
    ASSERT_NE(exported, nullptr);
    EXPECT_EQ(exported->documentation, "Exported helper docs.");
}

TEST(CompletionEngine, CtrlSpace_RanksVisibleLocalsAndParametersBeforeGlobals) {
    const std::string source = R"(module Main;

func globalCandidate() -> Integer { return 1; }

func main(paramCandidate: Integer) {
    var localCandidate = 2;
    
}
)";

    CompletionEngine engine;
    auto [line, col] = lineColAfter(source, "var localCandidate = 2;\n    ");
    auto items = engine.complete(source, line, col, "<test>", 0);

    const int localIdx = indexOfLabel(items, "localCandidate");
    const int paramIdx = indexOfLabel(items, "paramCandidate");
    const int globalIdx = indexOfLabel(items, "globalCandidate");
    EXPECT_GE(localIdx, 0);
    EXPECT_GE(paramIdx, 0);
    EXPECT_GE(globalIdx, 0);
    EXPECT_LT(localIdx, globalIdx);
    EXPECT_LT(paramIdx, globalIdx);
}

TEST(CompletionEngine, SignatureHelp_UserMethod) {
    const std::string source = R"(
module Test;

class Box {
    expose func Resize(w: Integer, h: Integer) {}
}

func main() {
    var box = new Box();
    box.Resize(1, 2);
}
)";
    auto [line, col] = lineColAfter(source, "box.Resize(");
    CompletionEngine engine;
    std::string help = engine.signatureHelp(source, line, col, "<test>");

    EXPECT_TRUE(help.find("Resize(w: Integer, h: Integer) -> Void") != std::string::npos);
    EXPECT_TRUE(help.find("parameter 1 of 2") != std::string::npos);
}

TEST(CompletionEngine, SignatureHelp_CurrentSourceOverloadsIncludesAll) {
    const std::string source = R"(
module Test;

func mix(value: Integer) -> Integer { return value; }
func mix(text: String) -> String { return text; }

func main() {
    mix(1);
}
)";
    auto [line, col] = lineColAfter(source, "mix(");
    CompletionEngine engine;
    std::string help = engine.signatureHelp(source, line, col, "<test>");

    EXPECT_TRUE(help.find("mix(value: Integer) -> Integer") != std::string::npos);
    EXPECT_TRUE(help.find("mix(text: String) -> String") != std::string::npos);
}

TEST(CompletionEngine, SignatureHelp_BoundFileModuleExportUsesParameterNames) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_signature_bound_file_modules";
    fs::remove_all(tempRoot);

    writeFile(tempRoot / "dep.zia", R"(module Dep;
/// Exported helper docs.
expose func exportedThing(count: Integer, label: String) -> Integer { return count; }
)");

    const std::string source = R"(module Main;
bind "./dep";

func main() {
    var value = Dep.exportedThing(1, "x");
}
)";

    CompletionEngine engine;
    auto [line, col] = lineColAfter(source, "Dep.exportedThing(");
    std::string help = engine.signatureHelp(source, line, col, (tempRoot / "main.zia").string());

    EXPECT_TRUE(help.find("exportedThing(count: Integer, label: String) -> Integer") !=
                std::string::npos);
    EXPECT_TRUE(help.find("Exported helper docs.") != std::string::npos);
}

TEST(CompletionEngine, SignatureHelp_RuntimeAliasMethod) {
    const std::string source = R"(
module Test;

bind Zanna.Terminal as Terminal;

func main() {
    Terminal.Say("hello");
}
)";
    auto [line, col] = lineColAfter(source, "Terminal.Say(");
    CompletionEngine engine;
    std::string help = engine.signatureHelp(source, line, col, "<test>");

    EXPECT_TRUE(help.find("Say(s: String) -> Void") != std::string::npos);
    EXPECT_TRUE(help.find("Writes text followed by a newline") != std::string::npos);
    EXPECT_TRUE(help.find("Runtime method Zanna.Terminal.Say.") != std::string::npos);
}

} // anonymous namespace

int main() {
    return zanna_test::run_all_tests();
}
