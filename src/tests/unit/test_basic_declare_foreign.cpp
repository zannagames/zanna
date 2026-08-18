//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_basic_declare_foreign.cpp
// Purpose: Verify DECLARE FOREIGN FUNCTION/SUB analyze and lower as bodyless imports.
// Key invariants:
//   - A foreign FUNCTION is exempt from the B1007 result-flow requirement.
//   - A non-foreign FUNCTION without a result still reports B1007.
//   - Foreign declarations lower to Import linkage with no body.
// Ownership/Lifetime: Each case owns its parser, analyzer, lowerer, and module.
// Links: docs/languages/interop.md, docs/tutorials/basic-tutorial.md,
//        src/frontends/basic/SemanticAnalyzer_Procs.cpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "il/core/Linkage.hpp"
#include "support/source_manager.hpp"
#include "tests/TestHarness.hpp"

#include <sstream>
#include <string>

using namespace il::frontends::basic;
using namespace il::support;

namespace {

/// @brief Diagnostic counts plus rendered text for one analyzed source.
struct AnalysisResult {
    size_t errors = 0;
    size_t warnings = 0;
    std::string text;
};

/// @brief Parses and semantically analyzes @p src, capturing diagnostics.
/// @param src Complete BASIC program text.
/// @return Error/warning counts and the rendered diagnostic stream.
AnalysisResult analyzeSource(const std::string &src) {
    SourceManager sm;
    uint32_t fid = sm.addFile("declare_foreign.bas");

    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);
    emitter.addSource(fid, src);

    Parser parser(src, fid, &emitter);
    auto program = parser.parseProgram();
    EXPECT_TRUE(program != nullptr);

    SemanticAnalyzer analyzer(emitter);
    if (program)
        analyzer.analyze(*program);

    std::ostringstream os;
    emitter.printAll(os);
    return AnalysisResult{de.errorCount(), de.warningCount(), os.str()};
}

/// @brief Lowers @p src to IL without running semantic analysis.
/// @param src Complete BASIC program text.
/// @return Lowered module owned by the caller.
il::core::Module lowerSource(const std::string &src) {
    SourceManager sm;
    uint32_t fid = sm.addFile("declare_foreign.bas");
    Parser parser(src, fid);
    auto program = parser.parseProgram();
    EXPECT_TRUE(program != nullptr);

    Lowerer lowerer;
    return lowerer.lowerProgram(*program);
}

/// @brief Finds a lowered function by IL name.
/// @param module Module to search.
/// @param name Exact IL function name.
/// @return Borrowed function pointer, or null when absent.
const il::core::Function *findFunction(const il::core::Module &module, const std::string &name) {
    for (const auto &fn : module.functions)
        if (fn.name == name)
            return &fn;
    return nullptr;
}

} // namespace

/// A bodyless foreign FUNCTION is an import, not a missing result (B1007).
TEST(BasicDeclareForeign, ForeignFunctionIsExemptFromMissingReturn) {
    const std::string src = "DECLARE FOREIGN FUNCTION ZiaHelper(n AS LONG) AS LONG\n"
                            "PRINT ZiaHelper(42)\n";

    AnalysisResult result = analyzeSource(src);
    EXPECT_EQ(result.errors, static_cast<size_t>(0));
    EXPECT_TRUE(result.text.find("B1007") == std::string::npos);
}

/// The SUB form has no result obligation and must stay clean.
TEST(BasicDeclareForeign, ForeignSubAnalyzesCleanly) {
    const std::string src = "DECLARE FOREIGN SUB InitSystem()\n"
                            "InitSystem()\n";

    AnalysisResult result = analyzeSource(src);
    EXPECT_EQ(result.errors, static_cast<size_t>(0));
}

/// Both forms together — the pairing documented in the interop guide.
TEST(BasicDeclareForeign, ForeignFunctionAndSubTogether) {
    const std::string src = "DECLARE FOREIGN FUNCTION ZiaHelper(n AS LONG) AS LONG\n"
                            "DECLARE FOREIGN SUB InitSystem()\n"
                            "\n"
                            "PRINT ZiaHelper(42)\n"
                            "InitSystem()\n";

    AnalysisResult result = analyzeSource(src);
    EXPECT_EQ(result.errors, static_cast<size_t>(0));
}

/// The exemption must not leak: an ordinary FUNCTION still needs a result.
TEST(BasicDeclareForeign, NonForeignFunctionStillRequiresReturn) {
    const std::string src = "10 FUNCTION F() AS LONG\n"
                            "20   PRINT 1\n"
                            "30 END FUNCTION\n"
                            "40 PRINT F()\n";

    AnalysisResult result = analyzeSource(src);
    EXPECT_EQ(result.errors, static_cast<size_t>(1));
    EXPECT_CONTAINS(result.text, "B1007");
    EXPECT_CONTAINS(result.text, "missing return in FUNCTION F");
}

/// A foreign FUNCTION lowers to an Import declaration carrying no body.
TEST(BasicDeclareForeign, ForeignFunctionLowersToImportDeclaration) {
    const std::string src = "DECLARE FOREIGN FUNCTION ZiaHelper(n AS LONG) AS LONG\n"
                            "PRINT ZiaHelper(42)\n";

    // BASIC is case-insensitive; identifiers lower to their upper-cased spelling.
    il::core::Module module = lowerSource(src);
    const il::core::Function *fn = findFunction(module, "ZIAHELPER");
    ASSERT_TRUE(fn != nullptr);
    EXPECT_TRUE(fn->linkage == il::core::Linkage::Import);
    EXPECT_TRUE(fn->blocks.empty());
    EXPECT_EQ(fn->params.size(), static_cast<size_t>(1));
    EXPECT_TRUE(fn->retType.kind == il::core::Type::Kind::I64);
}

/// A foreign SUB lowers to a void Import declaration carrying no body.
TEST(BasicDeclareForeign, ForeignSubLowersToImportDeclaration) {
    const std::string src = "DECLARE FOREIGN SUB InitSystem()\n"
                            "InitSystem()\n";

    il::core::Module module = lowerSource(src);
    const il::core::Function *fn = findFunction(module, "INITSYSTEM");
    ASSERT_TRUE(fn != nullptr);
    EXPECT_TRUE(fn->linkage == il::core::Linkage::Import);
    EXPECT_TRUE(fn->blocks.empty());
    EXPECT_TRUE(fn->params.empty());
    EXPECT_TRUE(fn->retType.kind == il::core::Type::Kind::Void);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
