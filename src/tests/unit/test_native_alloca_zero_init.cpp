//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_native_alloca_zero_init.cpp
// Purpose: Verify that native code generation zeroes alloca memory as IL
//          requires: stores cover each allocation exactly, large allocations
//          use a loop, and the rewritten module still verifies.
// Key invariants:
//   - Every test module verifies before and after the pass.
// Ownership/Lifetime:
//   - Modules are parsed from text and owned by each test.
// Links: src/codegen/common/NativeAllocaZeroInit.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/common/NativeAllocaZeroInit.hpp"
#include "il/api/expected_api.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"

#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

il::core::Module parseModule(const std::string &src) {
    std::istringstream in(src);
    il::core::Module mod;
    auto parse = il::api::v2::parse_text_expected(in, mod);
    EXPECT_TRUE(parse.hasValue());
    auto verify = il::api::v2::verify_module_expected(mod);
    EXPECT_TRUE(verify.hasValue());
    return mod;
}

/// Stored type kinds of the stores in @p block, in order.
std::vector<il::core::Type::Kind> storeKinds(const il::core::BasicBlock &block) {
    std::vector<il::core::Type::Kind> kinds;
    for (const auto &instr : block.instructions) {
        if (instr.op == il::core::Opcode::Store)
            kinds.push_back(instr.type.kind);
    }
    return kinds;
}

} // namespace

TEST(NativeAllocaZeroInit, StoresCoverEachAllocationExactly) {
    const std::string il = "il 0.3.0\n"
                           "func @f() -> i64 {\n"
                           "entry:\n"
                           "  %a = alloca 8\n"
                           "  %b = alloca 13\n"
                           "  %c = alloca 1\n"
                           "  %v = load i64, %a\n"
                           "  ret %v\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    ASSERT_TRUE(zanna::codegen::common::zeroInitAllocas(mod));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());

    const auto &entry = mod.functions.front().blocks.front();
    using K = il::core::Type::Kind;
    const std::vector<K> expected = {K::I64, K::I64, K::I32, K::I1, K::I1};
    EXPECT_EQ(storeKinds(entry).size(), expected.size());
    EXPECT_TRUE(storeKinds(entry) == expected);

    // Each store follows its allocation before any later alloca.
    const auto &instrs = entry.instructions;
    ASSERT_TRUE(instrs.size() > 2);
    EXPECT_EQ(instrs.at(0).op, il::core::Opcode::Alloca);
    EXPECT_EQ(instrs.at(1).op, il::core::Opcode::Store);
    EXPECT_EQ(instrs.at(2).op, il::core::Opcode::Alloca);
}

TEST(NativeAllocaZeroInit, LargeAllocationClearsWithALoopAndKeepsTheTail) {
    const std::string il = "il 0.3.0\n"
                           "func @f() -> i64 {\n"
                           "entry:\n"
                           "  %big = alloca 1027\n"
                           "  %v = load i64, %big\n"
                           "  ret %v\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    ASSERT_TRUE(zanna::codegen::common::zeroInitAllocas(mod));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());

    const auto &fn = mod.functions.front();
    ASSERT_EQ(fn.blocks.size(), 4U);
    EXPECT_EQ(fn.blocks.at(0).instructions.back().op, il::core::Opcode::Br);
    EXPECT_EQ(fn.blocks.at(1).params.size(), 1U);
    EXPECT_EQ(fn.blocks.at(1).instructions.back().op, il::core::Opcode::CBr);
    EXPECT_EQ(fn.blocks.at(2).instructions.back().op, il::core::Opcode::Br);

    // The continuation clears the 3 tail bytes, then runs the original code.
    const auto &done = fn.blocks.at(3);
    using K = il::core::Type::Kind;
    const std::vector<K> tail = {K::I16, K::I1};
    EXPECT_TRUE(storeKinds(done) == tail);
    EXPECT_EQ(done.instructions.back().op, il::core::Opcode::Ret);
}

TEST(NativeAllocaZeroInit, NewTempsNeverReuseUnnamedValueIds) {
    // Optimizer passes allocate ids past the end of the value-name table without
    // naming them; the zeroing GEPs must not reuse such an id (here the loop index).
    const std::string il = "il 0.3.0\n"
                           "func @f(i64 %n) -> i64 {\n"
                           "entry(%n0:i64):\n"
                           "  br head(0)\n"
                           "head(%i:i64):\n"
                           "  %more = scmp_lt %i, %n0\n"
                           "  cbr %more, body, exit\n"
                           "body:\n"
                           "  %m = alloca 24\n"
                           "  store i64, %m, %i\n"
                           "  %next = iadd.ovf %i, 1\n"
                           "  br head(%next)\n"
                           "exit:\n"
                           "  ret %i\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    auto &fn = mod.functions.front();
    fn.valueNames.resize(1);
    ASSERT_TRUE(zanna::codegen::common::zeroInitAllocas(mod));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());

    std::set<unsigned> defined;
    size_t definitions = 0;
    auto define = [&](unsigned id) {
        defined.insert(id);
        ++definitions;
    };
    for (const auto &param : fn.params)
        define(param.id);
    for (const auto &block : fn.blocks) {
        for (const auto &param : block.params)
            define(param.id);
        for (const auto &instr : block.instructions) {
            if (instr.result)
                define(*instr.result);
        }
    }
    EXPECT_EQ(defined.size(), definitions);
}

TEST(NativeAllocaZeroInit, ModuleWithoutAllocasIsUnchanged) {
    const std::string il = "il 0.3.0\n"
                           "func @f() -> i64 {\n"
                           "entry:\n"
                           "  ret 7\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    EXPECT_FALSE(zanna::codegen::common::zeroInitAllocas(mod));
    EXPECT_EQ(mod.functions.front().blocks.front().instructions.size(), 1U);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
