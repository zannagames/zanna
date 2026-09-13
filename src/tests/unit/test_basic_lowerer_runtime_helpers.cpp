//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_basic_lowerer_runtime_helpers.cpp
// Purpose: Verify BASIC lowering requests runtime helpers via the shared AST walker.
// Key invariants: Array assignment, PRINT #, and INPUT trigger their respective helpers;
//                 PRINT # formats integers with the 64-bit integer helper.
// Ownership/Lifetime: Test constructs AST via parser and owns emitted module.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/Parser.hpp"
#include "support/source_manager.hpp"
#include <cassert>
#include <string>
#include <unordered_set>

using namespace il::frontends::basic;
using namespace il::support;

namespace {

std::unordered_set<std::string> collectExternNames(const il::core::Module &module) {
    std::unordered_set<std::string> names;
    for (const auto &ext : module.externs)
        names.insert(ext.name);
    return names;
}

} // namespace

int main() {
    SourceManager sm;
    uint32_t fid = sm.addFile("runtime_walk.bas");
    const std::string src = "10 DIM A(3)\n"
                            "20 LET A(1) = 5\n"
                            "30 OPEN \"out.dat\" FOR OUTPUT AS #1\n"
                            "40 PRINT #1, 42\n"
                            "50 INPUT X, Y$\n"
                            "60 CLOSE #1\n";

    Parser parser(src, fid);
    auto program = parser.parseProgram();
    assert(program);

    Lowerer lowerer;
    il::core::Module module = lowerer.lowerProgram(*program);

    auto names = collectExternNames(module);
    assert(names.count("rt_arr_i64_set") == 1);
    // Accept either legacy aliases or canonical runtime names
    assert(names.count("rt_str_split_fields") == 1 || names.count("Zanna.String.SplitFields") == 1);
    assert(names.count("rt_to_int") == 1 || names.count("Zanna.Core.Convert.ToInt64") == 1);

    // PRINT # formats the integer 42 with the 64-bit integer helper; BASIC integers are
    // never narrowed to 16 or 32 bits for formatting.
    assert(names.count("rt_int_to_str") == 1 || names.count("Zanna.Core.Convert.ToStringInt") == 1);
    for (const char *narrow :
         {"rt_str_i16_alloc", "rt_str_i32_alloc", "Zanna.String.FromI16", "Zanna.String.FromI32"})
        assert(names.count(narrow) == 0);
    return 0;
}
