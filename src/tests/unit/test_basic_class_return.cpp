//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_basic_class_return.cpp
// Purpose: Repro and guard for BUG-040 — ensure FUNCTIONs returning custom
//          classes return a pointer-typed value.
// Key invariants: The ret operand must originate from a Load typed as Ptr,
//                 wherever in the function that load is.
// Ownership/Lifetime: Standalone unit test executable.
// Links: docs/internals/codemap.md, docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//
#include "frontends/basic/BasicCompiler.hpp"
#include "il/core/Function.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "tests/TestHarness.hpp"

#include <optional>
#include <string>
#include <unordered_map>

using namespace il::frontends::basic;

namespace {
constexpr const char *kSrc = R"BASIC(
10 CLASS Person
20 END CLASS

30 FUNCTION CreatePerson() AS Person
40   DIM p AS Person
50   p = NEW Person()
60   RETURN p
70 END FUNCTION
80 END
)BASIC";

static const il::core::Function *findFunctionCaseInsensitive(const il::core::Module &m,
                                                             std::string_view name) {
    auto ieq = [](char a, char b) {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    };
    auto eq = [&](std::string_view a, std::string_view b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!ieq(a[i], b[i]))
                return false;
        return true;
    };
    for (const auto &fn : m.functions)
        if (eq(fn.name, name))
            return &fn;
    return nullptr;
}
} // namespace

TEST(BasicClassReturn, ReturnUsesPtrLoad) {
    il::support::SourceManager sm;
    BasicCompilerInput input{kSrc, "class_return.bas"};
    BasicCompilerOptions opts{};

    auto result = compileBasic(input, opts, sm);
    ASSERT_TRUE(result.succeeded());

    const il::core::Module &mod = result.module;
    const il::core::Function *fn = findFunctionCaseInsensitive(mod, "CreatePerson");
    ASSERT_NE(fn, nullptr);
    // Sanity: function must declare a ptr return type
    EXPECT_EQ(fn->retType.kind, il::core::Type::Kind::Ptr);

    // Find a Ret, then locate the defining instruction for its operand, and ensure it is a Load
    // Ptr. RETURN releases the procedure's locals before returning, so the load and the ret may
    // sit in different blocks.
    std::unordered_map<unsigned, const il::core::Instr *> defByTemp;
    for (const auto &bb : fn->blocks) {
        for (const auto &ins : bb.instructions) {
            if (ins.result)
                defByTemp[*ins.result] = &ins;
        }
    }
    bool foundPtrLoadRet = false;
    for (const auto &bb : fn->blocks) {
        for (const auto &ins : bb.instructions) {
            if (ins.op != il::core::Opcode::Ret || ins.operands.size() != 1)
                continue;
            const auto &op = ins.operands[0];
            if (op.kind != il::core::Value::Kind::Temp)
                continue;
            auto it = defByTemp.find(op.id);
            if (it == defByTemp.end())
                continue;
            const il::core::Instr *def = it->second;
            if (def->op == il::core::Opcode::Load && def->type.kind == il::core::Type::Kind::Ptr) {
                foundPtrLoadRet = true;
                break;
            }
        }
        if (foundPtrLoadRet)
            break;
    }

    EXPECT_TRUE(foundPtrLoadRet);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
