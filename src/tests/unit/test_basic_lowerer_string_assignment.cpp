//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_basic_lowerer_string_assignment.cpp
// Purpose: Verify BASIC lowerer retains and releases strings on assignment and
//          releases string variables when the program ends.
// Key invariants:
//   - Assigning a borrowed string retains the new value before releasing the
//     old one.
//   - The exit block releases each string variable once (ADR 0147).
// Ownership/Lifetime:
//   - Test owns the parser, lowerer, and resulting module.
// Links: docs/adr/0147-managed-reference-lowering-and-native-retain-elision.md
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/Parser.hpp"
#include "support/source_manager.hpp"
#include <cassert>
#include <string>
#include <unordered_map>
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
    const std::string src = "10 LET S$ = \"HELLO\"\n"
                            "20 LET S$ = \"WORLD\"\n";

    SourceManager sm;
    uint32_t fid = sm.addFile("string_assign.bas");
    Parser parser(src, fid);
    auto program = parser.parseProgram();
    assert(program);

    Lowerer lowerer;
    il::core::Module module = lowerer.lowerProgram(*program);

    auto externs = collectExternNames(module);
    assert(externs.count("rt_str_release_maybe") == 1);
    assert(externs.count("rt_str_retain_maybe") == 1);

    const il::core::Function *mainFn = nullptr;
    for (const auto &fn : module.functions) {
        if (fn.name == "main") {
            mainFn = &fn;
            break;
        }
    }
    assert(mainFn);

    std::unordered_map<int, int> releaseCounts;
    std::unordered_map<int, int> retainCounts;
    std::unordered_set<int> assignmentLines;
    int exitReleases = 0;

    for (const auto &block : mainFn->blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.op != il::core::Opcode::Call)
                continue;
            const int line = instr.loc.line;
            if (instr.callee == "rt_str_release_maybe") {
                // Exit cleanup carries no source line.
                if (line == 0) {
                    ++exitReleases;
                    continue;
                }
                assert(retainCounts[line] > 0);
                ++releaseCounts[line];
                assignmentLines.insert(line);
            } else if (instr.callee == "rt_str_retain_maybe") {
                ++retainCounts[line];
                assignmentLines.insert(line);
            }
        }
    }

    assert(assignmentLines.size() == 2);
    for (int line : assignmentLines) {
        assert(releaseCounts[line] == 1);
        assert(retainCounts[line] == 1);
    }
    assert(exitReleases == 1);

    return 0;
}
