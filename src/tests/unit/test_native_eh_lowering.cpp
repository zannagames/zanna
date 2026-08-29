//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_native_eh_lowering.cpp
// Purpose: Verify the shared native EH rewrite before backend lowering.
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/common/NativeEHLowering.hpp"
#include "il/api/expected_api.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"

#include <sstream>
#include <string>

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

bool hasExtern(const il::core::Module &mod, const std::string &name) {
    for (const auto &ext : mod.externs) {
        if (ext.name == name)
            return true;
    }
    return false;
}

bool hasOpcode(const il::core::Function &fn, il::core::Opcode opcode) {
    for (const auto &bb : fn.blocks) {
        for (const auto &instr : bb.instructions) {
            if (instr.op == opcode)
                return true;
        }
    }
    return false;
}

const il::core::BasicBlock *findBlock(const il::core::Function &fn, const std::string &label) {
    for (const auto &bb : fn.blocks) {
        if (bb.label == label)
            return &bb;
    }
    return nullptr;
}

} // namespace

TEST(NativeEHLowering, RewritesResumeNextIntoRuntimeHelpers) {
    const std::string il = "il 0.1\n"
                           "func @f() -> i64 {\n"
                           "entry:\n"
                           "  eh.push ^handler\n"
                           "  %q = sdiv.chk0 10, 0\n"
                           "  eh.pop\n"
                           "  ret 42\n"
                           "handler ^handler(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  resume.next %tok\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    ASSERT_TRUE(zanna::codegen::common::lowerNativeEh(mod));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());
    ASSERT_EQ(mod.functions.size(), 1U);

    const auto &fn = mod.functions.front();
    EXPECT_FALSE(hasOpcode(fn, il::core::Opcode::EhPush));
    EXPECT_FALSE(hasOpcode(fn, il::core::Opcode::EhPop));
    EXPECT_FALSE(hasOpcode(fn, il::core::Opcode::EhEntry));
    EXPECT_FALSE(hasOpcode(fn, il::core::Opcode::ResumeNext));
    EXPECT_FALSE(hasOpcode(fn, il::core::Opcode::ResumeSame));
    EXPECT_FALSE(hasOpcode(fn, il::core::Opcode::ResumeLabel));

    EXPECT_TRUE(hasExtern(mod, "rt_native_eh_frame_alloc"));
    EXPECT_TRUE(hasExtern(mod, "rt_native_eh_push"));
    EXPECT_TRUE(hasExtern(mod, "rt_native_eh_pop"));
    EXPECT_TRUE(hasExtern(mod, "rt_native_eh_set_site"));
    EXPECT_TRUE(hasExtern(mod, "rt_native_eh_get_site"));
    EXPECT_TRUE(hasExtern(mod, "setjmp"));

    const auto *handler = findBlock(fn, "handler");
    ASSERT_TRUE(handler != nullptr);
    ASSERT_EQ(handler->params.size(), 2U);
    EXPECT_EQ(handler->params[0].type.kind, il::core::Type::Kind::Ptr);
    EXPECT_EQ(handler->params[1].type.kind, il::core::Type::Kind::I64);

    bool sawSiteBlock = false;
    bool sawInvalidResume = false;
    bool sawFrameAlloc = false;
    bool sawSetSite = false;
    bool sawGetSite = false;
    bool sawSetjmp = false;

    for (const auto &bb : fn.blocks) {
        if (bb.label.find(".__neh.site.") != std::string::npos)
            sawSiteBlock = true;
        if (bb.label.find(".__neh.invalid_resume") != std::string::npos)
            sawInvalidResume = true;
        for (const auto &instr : bb.instructions) {
            if (instr.op != il::core::Opcode::Call)
                continue;
            if (instr.callee == "rt_native_eh_frame_alloc")
                sawFrameAlloc = true;
            if (instr.callee == "rt_native_eh_set_site")
                sawSetSite = true;
            if (instr.callee == "rt_native_eh_get_site")
                sawGetSite = true;
            if (instr.callee == "setjmp")
                sawSetjmp = true;
        }
    }

    EXPECT_TRUE(sawSiteBlock);
    EXPECT_TRUE(sawInvalidResume);
    EXPECT_TRUE(sawFrameAlloc);
    EXPECT_TRUE(sawSetSite);
    EXPECT_TRUE(sawGetSite);
    EXPECT_TRUE(sawSetjmp);
}

TEST(NativeEHLowering, MaskFreeSetjmpTargetsUnderscoreSetjmp) {
    const std::string il = "il 0.1\n"
                           "func @f() -> i64 {\n"
                           "entry:\n"
                           "  eh.push ^handler\n"
                           "  %q = sdiv.chk0 10, 0\n"
                           "  eh.pop\n"
                           "  ret 42\n"
                           "handler ^handler(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  resume.next %tok\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    ASSERT_TRUE(zanna::codegen::common::lowerNativeEh(mod, /*maskFreeSetjmp=*/true));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());

    EXPECT_TRUE(hasExtern(mod, "_setjmp"));
    EXPECT_FALSE(hasExtern(mod, "setjmp"));

    std::size_t maskFreeCalls = 0;
    std::size_t plainCalls = 0;
    for (const auto &bb : mod.functions.front().blocks) {
        for (const auto &instr : bb.instructions) {
            if (instr.op != il::core::Opcode::Call)
                continue;
            if (instr.callee == "_setjmp")
                ++maskFreeCalls;
            if (instr.callee == "setjmp")
                ++plainCalls;
        }
    }
    EXPECT_EQ(maskFreeCalls, 1U);
    EXPECT_EQ(plainCalls, 0U);
}

TEST(NativeEHLowering, RewritesResumeSameIntoFaultSiteDispatch) {
    const std::string il = "il 0.1\n"
                           "func @f() -> i64 {\n"
                           "entry:\n"
                           "  eh.push ^handler\n"
                           "  trap\n"
                           "handler ^handler(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  resume.same %tok\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    ASSERT_TRUE(zanna::codegen::common::lowerNativeEh(mod));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());

    const auto &fn = mod.functions.front();
    bool sawResumeBackedge = false;
    for (const auto &bb : fn.blocks) {
        for (const auto &instr : bb.instructions) {
            if (instr.op != il::core::Opcode::CBr || instr.labels.empty())
                continue;
            for (const auto &label : instr.labels) {
                if (label.find(".__neh.site.") != std::string::npos)
                    sawResumeBackedge = true;
            }
        }
    }
    EXPECT_TRUE(sawResumeBackedge);
}

TEST(NativeEHLowering, RewritesTypedCatchHelperBlocks) {
    const std::string il = "il 0.1\n"
                           "func @f() -> void {\n"
                           "entry:\n"
                           "  eh.push ^handler\n"
                           "  trap.from_err i32 9\n"
                           "after:\n"
                           "  ret\n"
                           "handler ^handler(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  %kind = trap.kind\n"
                           "  %match = icmp_eq %kind, 9\n"
                           "  cbr %match, catch(%err, %tok), rethrow(%err, %tok)\n"
                           "handler ^catch(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  resume.label %tok, ^after\n"
                           "handler ^rethrow(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  resume.same %tok\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    ASSERT_TRUE(zanna::codegen::common::lowerNativeEh(mod));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());
    EXPECT_FALSE(zanna::codegen::common::findResidualStructuredEh(mod).has_value());

    const auto &fn = mod.functions.front();
    for (const auto *label : {"handler", "catch", "rethrow"}) {
        const auto *block = findBlock(fn, label);
        ASSERT_TRUE(block != nullptr);
        ASSERT_EQ(block->params.size(), 2U);
        EXPECT_EQ(block->params[0].type.kind, il::core::Type::Kind::Ptr);
        EXPECT_EQ(block->params[1].type.kind, il::core::Type::Kind::I64);
    }
}

TEST(NativeEHLowering, ResumeLabelValidatesSiteTokenBeforeBranch) {
    const std::string il = "il 0.1\n"
                           "func @f() -> void {\n"
                           "entry:\n"
                           "  eh.push ^handler\n"
                           "  trap\n"
                           "after:\n"
                           "  ret\n"
                           "handler ^handler(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  resume.label %tok, ^after\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    ASSERT_TRUE(zanna::codegen::common::lowerNativeEh(mod));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());

    const auto &fn = mod.functions.front();
    bool sawInvalidResume = false;
    bool sawResumeLabelGuard = false;
    for (const auto &bb : fn.blocks) {
        if (bb.label.find(".__neh.invalid_resume") != std::string::npos)
            sawInvalidResume = true;
        for (const auto &instr : bb.instructions) {
            if (instr.op != il::core::Opcode::CBr)
                continue;
            bool branchesToAfter = false;
            bool branchesToInvalid = false;
            for (const auto &label : instr.labels) {
                branchesToAfter = branchesToAfter || label == "after";
                branchesToInvalid =
                    branchesToInvalid || label.find(".__neh.invalid_resume") != std::string::npos;
            }
            sawResumeLabelGuard = sawResumeLabelGuard || (branchesToAfter && branchesToInvalid);
        }
    }

    EXPECT_TRUE(sawInvalidResume);
    EXPECT_TRUE(sawResumeLabelGuard);
}

TEST(NativeEHLowering, HandlerHelperTrapFromErrUsesOuterNativeSite) {
    const std::string il = "il 0.3.0\n"
                           "extern @may_throw() -> void\n"
                           "func @f() -> void {\n"
                           "entry:\n"
                           "  eh.push ^outer\n"
                           "  eh.push ^inner\n"
                           "  call @may_throw()\n"
                           "  eh.pop\n"
                           "  eh.pop\n"
                           "  br ^after\n"
                           "after:\n"
                           "  ret\n"
                           "handler ^outer(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  br ^outer_catch(%err, %tok)\n"
                           "handler ^outer_catch(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  resume.label %tok, ^after\n"
                           "handler ^inner(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  br ^inner_rethrow(%err, %tok)\n"
                           "handler ^inner_rethrow(%err:Error, %tok:ResumeTok):\n"
                           "  eh.entry\n"
                           "  trap.from_err i32 9\n"
                           "}\n";

    il::core::Module mod = parseModule(il);
    ASSERT_TRUE(zanna::codegen::common::lowerNativeEh(mod));
    auto verify = il::api::v2::verify_module_expected(mod);
    ASSERT_TRUE(verify.hasValue());

    const auto &fn = mod.functions.front();
    bool sawWrappedHelperTrap = false;
    for (const auto &bb : fn.blocks) {
        if (bb.label.find(".__neh.site.") == std::string::npos)
            continue;
        for (const auto &instr : bb.instructions) {
            if (instr.op == il::core::Opcode::TrapFromErr)
                sawWrappedHelperTrap = true;
        }
    }

    EXPECT_TRUE(sawWrappedHelperTrap);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
