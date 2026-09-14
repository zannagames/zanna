//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_basic_parse_try_catch.cpp
// Purpose: Validate parsing and AST shape for TRY/CATCH in BASIC.
// Key invariants:
//   - Parser produces a TryCatchStmt with an optional catch variable.
//   - The catch variable is spelled like every other identifier reference.
// Ownership/Lifetime:
//   - Test constructs parser/source manager per case and inspects AST.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/AstPrinter.hpp"
#include "frontends/basic/Parser.hpp"
#include "support/source_manager.hpp"

#include <cassert>
#include <string>

using namespace il::frontends::basic;
using namespace il::support;

int main() {
    // Positive: basic TRY/CATCH with a catch variable and simple bodies
    {
        std::string src = "10 TRY\n"
                          "20 PRINT 1\n"
                          "30 CATCH e\n"
                          "40 PRINT ERR()\n"
                          "50 END TRY\n"
                          "60 END\n";

        SourceManager sm;
        uint32_t fid = sm.addFile("try_catch_ok.bas");
        Parser p(src, fid);
        auto prog = p.parseProgram();
        assert(prog);

        // Expect two top-level statements: TRY/CATCH and END
        assert(prog->main.size() == 2);

        auto *tc = dynamic_cast<TryCatchStmt *>(prog->main[0].get());
        assert(tc && "first statement should be TryCatchStmt");
        // The catch variable keeps the lexer's canonical spelling, as references do
        assert(tc->catchVar.has_value());
        assert(*tc->catchVar == std::string("E"));

        // TRY body has one PRINT
        assert(tc->tryBody.size() == 1);
        assert(dynamic_cast<PrintStmt *>(tc->tryBody[0].get()) != nullptr);

        // CATCH body has one PRINT using ERR()
        assert(tc->catchBody.size() == 1);
        auto *printCatch = dynamic_cast<PrintStmt *>(tc->catchBody[0].get());
        assert(printCatch != nullptr);
    }

    // Negative: TRY without CATCH should surface a diagnostic but still produce an AST
    {
        std::string src = "10 TRY\n"
                          "20 PRINT 1\n"
                          "30 END TRY\n"
                          "40 END\n";

        SourceManager sm;
        uint32_t fid = sm.addFile("try_without_catch.bas");
        Parser p(src, fid);
        auto prog = p.parseProgram();
        assert(prog && "parser should not crash on missing CATCH");
        // Expect a TryCatchStmt node with only the TRY body collected
        assert(!prog->main.empty());
        auto *tc = dynamic_cast<TryCatchStmt *>(prog->main[0].get());
        assert(tc && "first statement should be TryCatchStmt even if malformed");
        assert(tc->tryBody.size() == 1);
        // No catch var and possibly empty catch body
        assert(!tc->catchVar.has_value());
    }

    // Negative: END TRY without a preceding TRY should not crash
    {
        std::string src = "10 END TRY\n"
                          "20 END\n";

        SourceManager sm;
        uint32_t fid = sm.addFile("end_try_without_try.bas");
        Parser p(src, fid);
        auto prog = p.parseProgram();
        assert(prog && "parser should not crash on stray END TRY");
        // The first statement should at least parse as END
        assert(!prog->main.empty());
        assert(dynamic_cast<EndStmt *>(prog->main[0].get()) != nullptr);
    }

    return 0;
}
