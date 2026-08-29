//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_codegen_arm64_bugfix.cpp
// Purpose: Regression tests for ARM64 codegen bug fixes #1, #2, #3, #4.
//
//===----------------------------------------------------------------------===//
#include "tests/TestHarness.hpp"
#include <filesystem>
#include <fstream>
#include <string>

#include "tools/zanna/cmd_codegen_arm64.hpp"

using namespace zanna::tools::ilc;

static std::string outPath(const std::string &name) {
    namespace fs = std::filesystem;
    const fs::path dir{"build/test-out/arm64"};
    fs::create_directories(dir);
    return (dir / name).string();
}

static void writeFile(const std::string &path, const std::string &text) {
    std::ofstream ofs(path);
    ASSERT_TRUE(static_cast<bool>(ofs));
    ofs << text;
}

/// Bug #3: void main should exit with code 0, not whatever was in x0.
TEST(Arm64Bugfix, VoidMainExitZero) {
    const std::string in = outPath("arm64_bugfix_void_main.il");
    // A void main that calls a runtime function leaving a non-zero value in x0.
    // Before the fix, this would exit with whatever rt_term_say left in x0.
    const std::string il = "il 0.3.0\n"
                           "extern @Zanna.Terminal.Say(str) -> void\n"
                           "global const str @.msg = \"hello\"\n"
                           "func @main() -> void {\n"
                           "entry_0:\n"
                           "  %t0 = const_str @.msg\n"
                           "  call @Zanna.Terminal.Say(%t0)\n"
                           "  ret\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native"};
    const int rc = cmd_codegen_arm64(2, const_cast<char **>(argv));
    ASSERT_EQ(rc, 0);
}

/// Bug #1: Boolean return values should be masked to i1 (0 or 1).
/// Tests that a runtime function returning bool is correctly captured.
TEST(Arm64Bugfix, BoolReturnMasked) {
    const std::string in = outPath("arm64_bugfix_bool_return.il");
    // Calls rt_str_eq which returns bool (i1). If the masking works,
    // the comparison and conditional branch should function correctly.
    const std::string il = "il 0.3.0\n"
                           "extern @Zanna.String.Equals(str, str) -> i1\n"
                           "global const str @.a = \"hello\"\n"
                           "global const str @.b = \"hello\"\n"
                           "func @main() -> i64 {\n"
                           "entry_0:\n"
                           "  %t0 = const_str @.a\n"
                           "  %t1 = const_str @.b\n"
                           "  %t2 = call @Zanna.String.Equals(%t0, %t1)\n"
                           "  cbr %t2, yes_0, no_0\n"
                           "yes_0:\n"
                           "  ret 0\n"
                           "no_0:\n"
                           "  ret 1\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native"};
    const int rc = cmd_codegen_arm64(2, const_cast<char **>(argv));
    // Equal strings should return exit code 0 (took the yes branch)
    ASSERT_EQ(rc, 0);
}

/// Bug #7: Values live only along phi-edge copies still need to survive calls.
TEST(Arm64Bugfix, CallerSavedLiveOutSurvivesEdgeBlockCall) {
    const std::string in = outPath("arm64_bugfix_call_liveout_edge.il");
    const std::string il = "il 0.1\n"
                           "func @clobber(%x:i64) -> i64 {\n"
                           "entry(%x:i64):\n"
                           "  %a = iadd.ovf %x, 1\n"
                           "  %b = iadd.ovf %a, 2\n"
                           "  %c = iadd.ovf %b, 3\n"
                           "  %d = iadd.ovf %c, 4\n"
                           "  ret %d\n"
                           "}\n"
                           "func @f(%x:i64) -> i64 {\n"
                           "entry(%x:i64):\n"
                           "  %a = iadd.ovf %x, 5\n"
                           "  %d = call @clobber(%x)\n"
                           "  %cond = icmp_eq %d, 51\n"
                           "  cbr %cond, exit(%a), miss(0)\n"
                           "exit(%r:i64):\n"
                           "  ret %r\n"
                           "miss(%z:i64):\n"
                           "  ret %z\n"
                           "}\n"
                           "func @main() -> i64 {\n"
                           "entry:\n"
                           "  %r = call @f(41)\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native", "-O0"};
    const int rc = cmd_codegen_arm64(3, const_cast<char **>(argv));
    ASSERT_EQ(rc, 46);
}

/// Bug #4: AddFpImm operand must be classified as DEF-only (not USE-only) in
/// RegAllocLinear::operandRoles so that the dirty flag is set after alloca
/// address materialisation.
///
/// Without the fix, when register pressure forced the eviction of the AddFpImm
/// result vreg before it was consumed by the following AddRI (GEP offset add),
/// the dirty flag remained false and no spill store was emitted.  The
/// subsequent reload then read an uninitialised frame slot, producing a garbage
/// address that caused a store to crash (EXC_BAD_ACCESS in the chess demo).
///
/// The test uses 22 live temps (> 19 available GPRs) so that the pool is full
/// when the critical field-3 GEP (offset 24) is processed.  All 22 temps have
/// future uses AFTER the critical GEP, so they all have finite next-use
/// distances and are not selected as spill victims — only the AddFpImm result
/// has UINT_MAX distance (no use after AddRI), making the eviction deterministic.
TEST(Arm64Bugfix, AddFpImmDirtyFlagUnderPressure) {
    // Build IL with 22 live temporaries + alloca + GEP store at offset 24.
    // The 22 temps all have future uses (stores after field 3), so only the
    // AddFpImm result vreg (no future use past the AddRI) has UINT_MAX distance
    // and is selected as the eviction victim.
    std::string il = "il 0.1\n"
                     "func @main() -> i64 {\n"
                     "entry:\n";

    // 22 live temps — enough to exceed the 19-register GPR pool
    for (int i = 0; i < 22; ++i)
        il += "  %v" + std::to_string(i) + " = iadd.ovf 0, " + std::to_string(i + 1) + "\n";

    // 192-byte alloca (enough for 24 i64 fields)
    il += "  %base = alloca 192\n";

    // CRITICAL: GEP at offset 24 (field 3) — triggers the AddFpImm dirty-flag bug.
    // Without fix: AddFpImm result evicted with dirty=false → reload from
    // uninitialised slot → garbage address → SIGSEGV.
    // With fix:    AddFpImm result evicted with dirty=true  → slot written →
    // reload correct → store 42 to correct address.
    il += "  %p24 = gep %base, 24\n";
    il += "  store i64, %p24, 42\n";

    // Use all 22 temps in stores AFTER field 3 so they have finite future-use
    // distances during field-3 GEP processing (ensures they are not evicted
    // instead of the AddFpImm result).
    // Offsets: 0,8,16,32,40,...,176 — skipping 24 which holds the sentinel.
    int offset = 0;
    for (int i = 0; i < 22; ++i) {
        if (offset == 24)
            offset += 8; // skip offset 24 (sentinel slot)
        il += "  %q" + std::to_string(i) + " = gep %base, " + std::to_string(offset) + "\n";
        il += "  store i64, %q" + std::to_string(i) + ", %v" + std::to_string(i) + "\n";
        offset += 8;
    }

    // Load sentinel from field 3 and return — must be 42
    il += "  %result = load i64, %p24\n";
    il += "  ret %result\n";
    il += "}\n";

    const std::string in = outPath("arm64_bugfix_addfpimm_dirty.il");
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native"};
    const int rc = cmd_codegen_arm64(2, const_cast<char **>(argv));
    ASSERT_EQ(rc, 42);
}

/// Bug #5: Clean FPR values loaded from rodata or memory must still be
/// materialized to a spill slot when they survive a call in caller-saved FPRs.
///
/// Before the fix, LdrFprBaseImm destinations were misclassified as USE-only,
/// leaving their dirty flag clear.  The allocator then dropped those live FP
/// values across a call without writing any spill slot, and later reloaded
/// them from an uninitialized frame offset.
TEST(Arm64Bugfix, CleanFprLoadSurvivesCall) {
    const std::string in = outPath("arm64_bugfix_clean_fpr_survives_call.il");
    const std::string il = "il 0.1\n"
                           "func @tick() -> void {\n"
                           "entry:\n"
                           "  ret\n"
                           "}\n"
                           "func @main() -> i64 {\n"
                           "entry:\n"
                           "  %x = const.f64 42.0\n"
                           "  call @tick()\n"
                           "  %r = cast.fp_to_si.rte.chk %x\n"
                           "  ret %r\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native", "-O0"};
    const int rc = cmd_codegen_arm64(3, const_cast<char **>(argv));
    ASSERT_EQ(rc, 42);
}

/// Bug #6: Current-instruction source operands must not be evicted before the
/// instruction has consumed them under GPR pressure.
///
/// Before the fix, the allocator could spill or drop the base pointer for a
/// `gep` while materializing that same instruction's destination register. The
/// rewritten MIR then used a stale register from the previous instruction as
/// the GEP base, corrupting object field stores. This is the failure pattern
/// that broke BowlingGame.init in the 3dbowling demo.
TEST(Arm64Bugfix, CurrentInstructionUseNotEvictedUnderPressure) {
    std::string il = "il 0.1\n"
                     "extern @rt_obj_new_i64(i64, i64) -> ptr\n"
                     "func @touch(%obj:ptr) -> i64 {\n"
                     "entry(%obj:ptr):\n";

    for (int i = 0; i < 22; ++i)
        il += "  %v" + std::to_string(i) + " = iadd.ovf 0, " + std::to_string(i + 1) + "\n";

    il += "  %f248 = gep %obj, 248\n";
    il += "  store i64, %f248, 11\n";
    il += "  %f256 = gep %obj, 256\n";
    il += "  store i64, %f256, 1\n";
    il += "  %f264 = gep %obj, 264\n";
    il += "  store i64, %f264, 42\n";
    il += "  %f272 = gep %obj, 272\n";
    il += "  store i64, %f272, 0\n";
    il += "  %f280 = gep %obj, 280\n";
    il += "  store i64, %f280, 0\n";
    il += "  %f288 = gep %obj, 288\n";
    il += "  store i64, %f288, 0\n";

    int offset = 0;
    for (int i = 0; i < 22; ++i) {
        il += "  %q" + std::to_string(i) + " = gep %obj, " + std::to_string(offset) + "\n";
        il += "  store i64, %q" + std::to_string(i) + ", %v" + std::to_string(i) + "\n";
        offset += 8;
    }

    il += "  %result = load i64, %f264\n";
    il += "  ret %result\n";
    il += "}\n";
    il += "func @main() -> i64 {\n";
    il += "entry:\n";
    il += "  %obj = call @rt_obj_new_i64(0, 512)\n";
    il += "  %r = call @touch(%obj)\n";
    il += "  ret %r\n";
    il += "}\n";

    const std::string in = outPath("arm64_bugfix_current_use_pressure.il");
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native"};
    const int rc = cmd_codegen_arm64(2, const_cast<char **>(argv));
    ASSERT_EQ(rc, 42);
}

/// Regression: Current-instruction destination operands must not be evicted before
/// the instruction has written them under GPR pressure.
///
/// The chess board renderer computes square coordinates as
/// `boardOrigin + squareIndex * squareSize` and then carries those coordinates
/// through block-argument edge copies before issuing draw calls.  Before this
/// fix, the linear allocator protected only current-instruction uses.  While
/// materializing an `AddsRRR` source under pressure, it could select the
/// just-assigned destination as a spill victim, store the register's old
/// contents, and later reload that stale spill slot for the edge copy.  Chess
/// then repainted every board square at stale coordinates.
TEST(Arm64Bugfix, CurrentInstructionDefNotEvictedUnderPressure) {
    std::string il = "il 0.1\n"
                     "func @calc(%file:i64, %base:i64) -> i64 {\n"
                     "entry(%file:i64, %base:i64):\n";

    for (int i = 0; i < 22; ++i)
        il += "  %v" + std::to_string(i) + " = iadd.ovf %file, " + std::to_string(i + 1) + "\n";

    il += "  %square = iadd.ovf 0, 80\n";
    il += "  %offset = imul.ovf %file, %square\n";
    il += "  %coord = iadd.ovf %base, %offset\n";
    il += "  %always = icmp_eq %file, 3\n";
    il += "  cbr %always, join(%coord";
    for (int i = 0; i < 22; ++i)
        il += ", %v" + std::to_string(i);
    il += "), fail(1)\n";

    il += "join(%j:i64";
    for (int i = 0; i < 22; ++i)
        il += ", %p" + std::to_string(i) + ":i64";
    il += "):\n";

    il += "  %sum0 = iadd.ovf %j, %p0\n";
    for (int i = 1; i < 22; ++i)
        il += "  %sum" + std::to_string(i) + " = iadd.ovf %sum" + std::to_string(i - 1) + ", %p" +
              std::to_string(i) + "\n";

    // Expected: coord 42 + 3*80 = 282, plus sum(4..25) = 319, total 601.
    il += "  %ok = icmp_eq %sum21, 601\n";
    il += "  cbr %ok, pass(0), fail(2)\n";
    il += "pass(%r:i64):\n";
    il += "  ret %r\n";
    il += "fail(%r:i64):\n";
    il += "  ret %r\n";
    il += "}\n";
    il += "func @main() -> i64 {\n";
    il += "entry:\n";
    il += "  %r = call @calc(3, 42)\n";
    il += "  ret %r\n";
    il += "}\n";

    const std::string in = outPath("arm64_bugfix_current_def_pressure.il");
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native", "-O1"};
    const int rc = cmd_codegen_arm64(3, const_cast<char **>(argv));
    ASSERT_EQ(rc, 0);
}

/// Bug #9: Cross-block f64 reloads must use the temp's IL type even when the
/// use block is lowered before the defining block in textual IL order.
///
/// The game3d showcase's forest predicate had this shape after O1 IL
/// simplification: a block used an f64 block parameter from a later textual
/// merge block. The AArch64 lowerer had not seen the defining block yet, so it
/// defaulted the cross-block reload to GPR and converted the double bit pattern
/// with SCvtF. The comparison then failed for every tree site.
TEST(Arm64Bugfix, ForwardDefinedF64BlockParamReloadKeepsFprClass) {
    const std::string in = outPath("arm64_bugfix_forward_f64_block_param.il");
    const std::string il = "il 0.3.0\n"
                           "func @main() -> i64 {\n"
                           "entry:\n"
                           "  %seed = const.f64 1.5\n"
                           "  br join(%seed)\n"
                           "use:\n"
                           "  %ok = fcmp_lt %x, 2.0\n"
                           "  cbr %ok, pass, fail\n"
                           "join(%x:f64):\n"
                           "  br use\n"
                           "pass:\n"
                           "  ret 0\n"
                           "fail:\n"
                           "  ret 1\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native", "-O1", "--skip-il-optimization"};
    const int rc = cmd_codegen_arm64(4, const_cast<char **>(argv));
    ASSERT_EQ(rc, 0);
}

/// Bug #8: post-RA scheduling must preserve aliasing between copied base+imm
/// addresses that resolve to the same object field. Without that, a reload of
/// `top` could stay ahead of the preceding store to `top`, reintroducing the
/// stale `-1` value and tripping array bounds checks.
TEST(Arm64Bugfix, SchedulerPreservesAliasedBaseRegisterStores) {
    const std::string in = outPath("arm64_bugfix_scheduler_alias_base.il");
    const std::string il = "il 0.3.0\n"
                           "extern @rt_obj_new_i64(i64, i64) -> ptr\n"
                           "extern @rt_arr_i64_new(i64) -> ptr\n"
                           "extern @rt_arr_i64_len(ptr) -> i64\n"
                           "extern @rt_arr_i64_get(ptr, i64) -> i64\n"
                           "extern @rt_arr_i64_set(ptr, i64, i64) -> void\n"
                           "extern @rt_arr_oob_panic(i64, i64) -> void\n"
                           "func @main() -> i64 {\n"
                           "entry:\n"
                           "  %slot = alloca 8\n"
                           "  %obj = call @rt_obj_new_i64(0, 24)\n"
                           "  store ptr, %slot, %obj\n"
                           "  %obj0 = load ptr, %slot\n"
                           "  %arr = call @rt_arr_i64_new(21)\n"
                           "  %arrf0 = gep %obj0, 8\n"
                           "  store ptr, %arrf0, %arr\n"
                           "  %obj1 = load ptr, %slot\n"
                           "  %topf0 = gep %obj1, 16\n"
                           "  store i64, %topf0, -1\n"
                           "  %obj2 = load ptr, %slot\n"
                           "  %topf1 = gep %obj2, 16\n"
                           "  %top = load i64, %topf1\n"
                           "  %newtop = iadd.ovf %top, 1\n"
                           "  %obj3 = load ptr, %slot\n"
                           "  %topf2 = gep %obj3, 16\n"
                           "  store i64, %topf2, %newtop\n"
                           "  %obj4 = load ptr, %slot\n"
                           "  %arrf1 = gep %obj4, 8\n"
                           "  %arr1 = load ptr, %arrf1\n"
                           "  %obj5 = load ptr, %slot\n"
                           "  %topf3 = gep %obj5, 16\n"
                           "  %idx = load i64, %topf3\n"
                           "  %len = call @rt_arr_i64_len(%arr1)\n"
                           "  %lt0 = scmp_lt %idx, 0\n"
                           "  %ge = scmp_ge %idx, %len\n"
                           "  %lt0i = zext1 %lt0\n"
                           "  %gei = zext1 %ge\n"
                           "  %bad = or %lt0i, %gei\n"
                           "  %isbad = icmp_ne %bad, 0\n"
                           "  cbr %isbad, oob, ok\n"
                           "ok:\n"
                           "  call @rt_arr_i64_set(%arr1, %idx, 10)\n"
                           "  %ret = call @rt_arr_i64_get(%arr1, 0)\n"
                           "  ret %ret\n"
                           "oob:\n"
                           "  call @rt_arr_oob_panic(%idx, %len)\n"
                           "  trap\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-run-native"};
    const int rc = cmd_codegen_arm64(2, const_cast<char **>(argv));
    ASSERT_EQ(rc, 10);
}

// ZB-31: a load whose GEP-chain root was materialized in ANOTHER block (the
// shape LICM produces at -O2: the base load and the gep hoisted above a loop,
// the dereference inside a later block) must guard the address it actually
// dereferences. The old guard resolved the root through the function-wide
// temp cache to a vreg that was not live in the loading block and trapped
// "null pointer access" on a perfectly valid object.
TEST(Arm64CLI, NullGuardUsesLiveAddressForCrossBlockRoot) {
    const std::string in = outPath("arm64_nullguard_crossblock_root.il");
    const std::string il = "il 0.1\n"
                           "extern @rt_arr_obj_new(i64) -> ptr\n"
                           "extern @rt_arr_obj_len(ptr) -> i64\n"
                           "func @main() -> i64 {\n"
                           "entry:\n"
                           "  %cell = alloca 8\n"
                           "  %obj = call @rt_arr_obj_new(3)\n"
                           "  store ptr, %cell, %obj\n"
                           "  %root = load ptr, %cell\n"
                           "  %addr = gep %root, 8\n"
                           "  br ^loop(0)\n"
                           "loop(%i:i64):\n"
                           "  %n = call @rt_arr_obj_len(%obj)\n"
                           "  %done = scmp_ge %i, %n\n"
                           "  cbr %done, ^exit, ^body\n"
                           "body:\n"
                           "  %v = load i64, %addr\n"
                           "  %next = iadd.ovf %i, 1\n"
                           "  br ^loop(%next)\n"
                           "exit:\n"
                           "  ret 0\n"
                           "}\n";
    writeFile(in, il);
    const char *argv[] = {in.c_str(), "-O0", "--skip-il-optimization", "-run-native"};
    const int rc = cmd_codegen_arm64(4, const_cast<char **>(argv));
    ASSERT_EQ(rc, 0);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
