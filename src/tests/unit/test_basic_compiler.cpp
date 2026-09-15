//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_basic_compiler.cpp
// Purpose: Verify the BASIC compiler pipeline produces IL from in-memory input.
// Key invariants: Successful compilation yields IL functions with no diagnostics.
// Ownership/Lifetime: Test owns compiler inputs and source manager.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/BasicCompiler.hpp"
#include "support/source_manager.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace il::support {
struct SourceManagerTestAccess {
    static void setNextFileId(SourceManager &sm, uint64_t next) {
        sm.next_file_id_ = next;
    }
};
} // namespace il::support

using namespace il::frontends::basic;
using namespace il::support;

int main() {
    std::string source = "10 PRINT 1\n20 END\n";
    SourceManager sm;
    BasicCompilerOptions options{};
    BasicCompilerInput input{source, "test.bas"};
    auto result = compileBasic(input, options, sm);

    assert(result.emitter);
    assert(result.succeeded());
    assert(result.fileId != 0);
    assert(!result.module.functions.empty());
    assert(!result.module.functions.front().name.empty());
    assert(result.emitter->warningCount() == 0);

    // A moved result keeps reporting into its own diagnostics. The emitter
    // refers to the engine by address, so without rebinding it kept reporting
    // into the moved-from engine. MSVC Debug builds move the local that
    // compileBasic() returns, so callers read a destroyed engine's counts.
    {
        auto original = compileBasic(input, options, sm);
        assert(original.succeeded());
        BasicCompilerResult moved(std::move(original));
        moved.emitter->emit(Severity::Error, "B9999", {}, 0, "reported after a move");
        assert(moved.diagnostics.errorCount() == 1);
        assert(moved.emitter->errorCount() == 1);
        assert(!moved.succeeded());

        BasicCompilerResult assigned;
        assigned = std::move(moved);
        assigned.emitter->emit(Severity::Warning, "B9998", {}, 0, "reported after assignment");
        assert(assigned.diagnostics.errorCount() == 1);
        assert(assigned.diagnostics.warningCount() == 1);
        assert(assigned.emitter->warningCount() == 1);
    }

    {
        SourceManager exhaustedSm;
        SourceManagerTestAccess::setNextFileId(
            exhaustedSm, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1);

        BasicCompilerInput exhaustedInput{source, "overflow.bas"};
        auto exhaustedResult = compileBasic(exhaustedInput, options, exhaustedSm);

        assert(exhaustedResult.emitter);
        assert(!exhaustedResult.succeeded());
        assert(exhaustedResult.fileId == 0);
        assert(exhaustedResult.emitter->errorCount() == 1);

        std::ostringstream oss;
        exhaustedResult.diagnostics.printAll(oss, &exhaustedSm);
        auto text = oss.str();
        assert(text.find("source manager exhausted file identifier space") != std::string::npos);
    }
    return 0;
}
