//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "il/analysis/BasicAA.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Global.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/internal/io/ParserUtil.hpp"
#include "il/io/Parser.hpp"
#include "il/io/Serializer.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/signatures/Registry.hpp"
#include "il/transform/ConstFold.hpp"
#include "il/transform/DCE.hpp"
#include "il/transform/LoadSafety.hpp"
#include "il/transform/SimplifyCFG.hpp"
#include "il/transform/SimplifyCFG/ParamCanonicalization.hpp"
#include "il/transform/SimplifyCFG/ReachabilityCleanup.hpp"
#include "il/transform/ValueKey.hpp"
#include "il/verify/Verifier.hpp"
#include "il/verify/VerifierTable.hpp"
#include "tests/TestHarness.hpp"
#include "zanna/vm/VM.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>

namespace il::runtime::signatures {
void register_array_signatures();
}

using namespace il::core;

namespace {

Module parseModule(const char *text) {
    Module module;
    std::istringstream input{text};
    auto parsed = il::io::Parser::parse(input, module);
    EXPECT_TRUE(parsed.hasValue());
    return module;
}

bool parseFailsWith(const char *text, const std::string &needle) {
    Module module;
    std::istringstream input{text};
    auto parsed = il::io::Parser::parse(input, module);
    if (parsed)
        return false;
    return parsed.error().message.find(needle) != std::string::npos;
}

bool parseFailsWithLimits(const char *text,
                          const il::io::ParserLimits &limits,
                          const std::string &needle) {
    Module module;
    std::istringstream input{text};
    auto parsed = il::io::Parser::parse(input, module, limits);
    if (parsed)
        return false;
    return parsed.error().message.find(needle) != std::string::npos;
}

bool verifyFailsWith(const Module &module, const std::string &needle) {
    auto result = il::verify::Verifier::verify(module);
    if (result)
        return false;
    return result.error().message.find(needle) != std::string::npos;
}

Function makeMain(Type ret = Type(Type::Kind::I64)) {
    Function fn;
    fn.name = "main";
    fn.retType = ret;
    BasicBlock entry;
    entry.label = "entry";
    fn.blocks.push_back(std::move(entry));
    return fn;
}

bool hasBlockLabel(const Function &fn, const std::string &label) {
    return std::any_of(fn.blocks.begin(), fn.blocks.end(), [&](const BasicBlock &block) {
        return block.label == label;
    });
}

size_t countOpcode(const Function &fn, Opcode opcode) {
    size_t count = 0;
    for (const auto &block : fn.blocks)
        for (const auto &instr : block.instructions)
            if (instr.op == opcode)
                ++count;
    return count;
}

} // namespace

TEST(ILCorrectness, ParserAcceptsInlineCommentsAndScalarGlobals) {
    Module module = parseModule(R"(il 0.3.0 # version comment
global const str @.msg = "http://example/#frag" # string comment
global const i64 @.answer = 42 # scalar comment
func @main() -> i64 {
entry:
  %p = gaddr @.answer # address scalar global
  %v = load i64, %p
  ret %v # done
}

)");

    ASSERT_EQ(module.globals.size(), 2u);
    EXPECT_EQ(module.globals[0].init, "http://example/#frag");
    EXPECT_EQ(module.globals[1].type.kind, Type::Kind::I64);
    EXPECT_EQ(module.globals[1].init, "42");
    EXPECT_TRUE(module.globals[1].isConst);

    auto verified = il::verify::Verifier::verify(module);
    ASSERT_TRUE(verified.hasValue());

    const std::string text = il::io::Serializer::toString(module);
    EXPECT_CONTAINS(text, "global const i64 @.answer = 42");
}

TEST(ILCorrectness, ParserEnforcesConfigurableResourceLimits) {
    il::io::ParserLimits limits;
    limits.maxLineBytes = 4;
    EXPECT_TRUE(parseFailsWithLimits("il 0.3.0\n", limits, "resource limit exceeded: line bytes"));

    limits = {};
    limits.maxFunctions = 0;
    EXPECT_TRUE(parseFailsWithLimits(R"(il 0.3.0
func @main() -> i64 {
entry:
  ret 0
}
)",
                                     limits,
                                     "resource limit exceeded: functions"));

    limits = {};
    limits.maxBlocks = 0;
    EXPECT_TRUE(parseFailsWithLimits(R"(il 0.3.0
func @main() -> i64 {
entry:
  ret 0
}
)",
                                     limits,
                                     "resource limit exceeded: basic blocks"));

    limits = {};
    limits.maxInstructions = 0;
    EXPECT_TRUE(parseFailsWithLimits(R"(il 0.3.0
func @main() -> i64 {
entry:
  ret 0
}
)",
                                     limits,
                                     "resource limit exceeded: instructions"));

    limits = {};
    limits.maxValuesPerInstruction = 1;
    EXPECT_TRUE(parseFailsWithLimits(R"(il 0.3.0
func @main() -> i64 {
entry:
  %sum = add 1, 2
  ret %sum
}
)",
                                     limits,
                                     "resource limit exceeded: instruction operands"));

    limits = {};
    limits.maxTempsPerFunction = 2;
    EXPECT_TRUE(parseFailsWithLimits(R"(il 0.3.0
func @main() -> i64 {
entry:
  %a = add 1, 2
  %b = add %a, 3
  %c = add %b, 4
  ret %c
}
)",
                                     limits,
                                     "resource limit exceeded: function temporaries"));

    limits = {};
    limits.maxTempsPerFunction = 8;
    EXPECT_TRUE(parseFailsWithLimits(R"(il 0.3.0
func @main() -> i64 {
entry:
  ret %t4294967295
}
)",
                                     limits,
                                     "resource limit exceeded: function temporaries"));
}

TEST(ILCorrectness, ParserRollsBackModuleOnFailure) {
    Module module;
    Global existing;
    existing.name = "existing";
    existing.type = Type(Type::Kind::I64);
    existing.init = "7";
    existing.hasInitializer = true;
    module.globals.push_back(existing);
    module.internOwnedIdentifiers();
    const size_t symbolsBefore = module.symbols.size();

    std::istringstream input{R"(il 0.3.0
global i64 @added = 9
this is not valid IL
)"};
    auto parsed = il::io::Parser::parse(input, module);
    EXPECT_FALSE(parsed.hasValue());
    ASSERT_EQ(module.globals.size(), 1u);
    EXPECT_EQ(module.globals.front().name, "existing");
    EXPECT_EQ(module.globals.front().init, "7");
    EXPECT_EQ(module.symbols.size(), symbolsBefore);
}

TEST(ILCorrectness, ParserReportsInputStreamFailures) {
    class FailingBuffer final : public std::streambuf {
      protected:
        int_type underflow() override {
            throw std::runtime_error("synthetic read failure");
        }
    } buffer;

    std::istream input{&buffer};
    Module module;
    auto parsed = il::io::Parser::parse(input, module);
    EXPECT_FALSE(parsed.hasValue());
    EXPECT_CONTAINS(parsed.error().message, "input stream read failure");
    EXPECT_TRUE(module.functions.empty());
    EXPECT_TRUE(module.globals.empty());
}

TEST(ILCorrectness, LoadSafetyTracksSignedGepChainsWithoutWrapping) {
    Function fn = makeMain();
    BasicBlock &entry = fn.blocks.front();

    Instr alloca;
    alloca.op = Opcode::Alloca;
    alloca.result = 0;
    alloca.type = Type(Type::Kind::Ptr);
    alloca.operands = {Value::constInt(16)};
    entry.instructions.push_back(alloca);

    Instr forward;
    forward.op = Opcode::GEP;
    forward.result = 1;
    forward.type = Type(Type::Kind::Ptr);
    forward.operands = {Value::temp(0), Value::constInt(8)};
    entry.instructions.push_back(forward);

    Instr backward;
    backward.op = Opcode::GEP;
    backward.result = 2;
    backward.type = Type(Type::Kind::Ptr);
    backward.operands = {Value::temp(1), Value::constInt(-4)};
    entry.instructions.push_back(backward);

    Instr load;
    load.op = Opcode::Load;
    load.result = 3;
    load.type = Type(Type::Kind::I64);
    load.operands = {Value::temp(2)};
    entry.instructions.push_back(load);

    il::transform::LoadSafetyContext safety(fn);
    EXPECT_TRUE(il::transform::isLoadKnownNonTrapping(safety, entry.instructions.back()));

    entry.instructions[2].operands[1] = Value::constInt(-12);
    il::transform::LoadSafetyContext beforeAllocation(fn);
    EXPECT_FALSE(
        il::transform::isLoadKnownNonTrapping(beforeAllocation, entry.instructions.back()));
}

TEST(ILCorrectness, ParserRejectsControlByteIdentifiersAndUnterminatedTokens) {
    const std::string source =
        std::string("il 0.3.0\nfunc @bad") + '\x01' + "name() -> i64 {\nentry:\nret 0\n}\n";
    Module module;
    std::istringstream input{source};
    auto parsed = il::io::Parser::parse(input, module);
    EXPECT_FALSE(parsed.hasValue());

    std::istringstream tokenStream{"\"unterminated"};
    const std::string token = il::io::readToken(tokenStream);
    EXPECT_EQ(token, "\"unterminated");
    EXPECT_TRUE(tokenStream.fail());
}

TEST(ILCorrectness, ParserAcceptsImportAndStringGlobalTrailingComments) {
    Module module = parseModule(R"(il 0.3.0
func import @helper() -> i64 // imported from another module
global const str @s = "hello"// string comment
global const str @semi = "world"; semicolon comment
)");

    ASSERT_EQ(module.functions.size(), 1u);
    EXPECT_EQ(module.functions.front().name, "helper");
    EXPECT_EQ(module.functions.front().retType.kind, Type::Kind::I64);
    ASSERT_EQ(module.globals.size(), 2u);
    EXPECT_EQ(module.globals[0].init, "hello");
    EXPECT_EQ(module.globals[1].init, "world");
}

TEST(ILCorrectness, ModuleInitializerAttributeRoundTripsAndIsVerified) {
    Module valid = parseModule(R"(il 0.3.0
func @startup() -> void [module_init] {
entry:
  ret
}
func @main() -> i64 {
entry:
  ret 0
}
)");
    ASSERT_TRUE(valid.functions.front().moduleInitializer);
    EXPECT_TRUE(il::verify::Verifier::verify(valid).hasValue());
    EXPECT_CONTAINS(il::io::Serializer::toString(valid), "[module_init]");

    Module invalid = parseModule(R"(il 0.3.0
func @startup(i64 %value) -> void [module_init] {
entry:
  ret
}
func @main() -> i64 {
entry:
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(invalid, "module initializer must be"));
}

TEST(ILCorrectness, ParserRejectsJunkBeforeCallCallee) {
    EXPECT_TRUE(parseFailsWith(R"(il 0.3.0
func @callee() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call junk @callee()
  ret 0
}
)",
                               "malformed call"));
}

TEST(ILCorrectness, VmInitializesScalarGlobals) {
    Module module = parseModule(R"(il 0.3.0
global i64 @counter = 41
func @main() -> i64 {
entry:
  %p = gaddr @counter
  %v = load i64, %p
  ret %v
}
)");
    auto verified = il::verify::Verifier::verify(module);
    ASSERT_TRUE(verified.hasValue());

    il::vm::Runner runner(module, {});
    EXPECT_EQ(runner.run(), 41);
}

TEST(ILCorrectness, VerifierRejectsBadGlobalReferences) {
    Module missing = parseModule(R"(il 0.3.0
func @main() -> str {
entry:
  %s = const_str @missing
  ret %s
}
)");
    EXPECT_TRUE(verifyFailsWith(missing, "unknown string global @missing"));

    Module wrongKind = parseModule(R"(il 0.3.0
global i64 @n = 1
func @main() -> str {
entry:
  %s = const_str @n
  ret %s
}
)");
    EXPECT_TRUE(verifyFailsWith(wrongKind, "const.str operand must name a string global"));

    Module stringGAddr = parseModule(R"(il 0.3.0
global const str @s = "x"
func @main() -> ptr {
entry:
  %p = gaddr @s
  ret %p
}
)");
    EXPECT_TRUE(verifyFailsWith(stringGAddr, "gaddr requires a scalar storage global"));
}

TEST(ILCorrectness, DirectGlobalAddressRequiresAddressMaterializationForMemory) {
    Module module = parseModule(R"(il 0.3.0
global i64 @counter = 7
func @main() -> i64 {
entry:
  %v = load i64, @counter
  ret %v
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "load requires a pointer value"));
}

TEST(ILCorrectness, TrapKindAcceptsErrorOperandAndRejectsLegacyMnemonicOperand) {
    Module module = parseModule(R"(il 0.3.0
global const str @msg = "boom"
func @main(i32 %code) -> i64 {
entry:
  %msg = const_str @msg
  %err:error = trap.err %code, %msg
  %kind = trap.kind %err
  ret %kind
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());
    const std::string text = il::io::Serializer::toString(module);
    EXPECT_CONTAINS(text, "trap.kind %err");

    EXPECT_TRUE(parseFailsWith(R"(il 0.3.0
func @main() -> i64 {
entry:
  %kind = trap.kind DivideByZero
  ret %kind
}
)",
                               ""));
}

TEST(ILCorrectness, DominanceRejectsCrossComponentUses) {
    Module module;
    Function fn = makeMain();
    BasicBlock &entry = fn.blocks.front();
    Instr add;
    add.result = 0;
    add.op = Opcode::IAddOvf;
    add.type = Type(Type::Kind::I64);
    add.operands = {Value::constInt(1), Value::constInt(2)};
    entry.instructions.push_back(std::move(add));
    Instr retEntry;
    retEntry.op = Opcode::Ret;
    retEntry.type = Type(Type::Kind::Void);
    retEntry.operands = {Value::constInt(0)};
    entry.instructions.push_back(std::move(retEntry));
    entry.terminated = true;

    BasicBlock dead;
    dead.label = "dead";
    Instr retDead;
    retDead.op = Opcode::Ret;
    retDead.type = Type(Type::Kind::Void);
    retDead.operands = {Value::temp(0)};
    dead.instructions.push_back(std::move(retDead));
    dead.terminated = true;
    fn.blocks.push_back(std::move(dead));
    fn.valueNames.resize(1);
    module.functions.push_back(std::move(fn));

    EXPECT_TRUE(verifyFailsWith(module, "not dominated by definition"));
}

TEST(ILCorrectness, PureFunctionMayUsePrivateStack) {
    Module module;
    Function fn = makeMain();
    fn.attrs().pure = true;
    BasicBlock &entry = fn.blocks.front();

    Instr alloca;
    alloca.result = 0;
    alloca.op = Opcode::Alloca;
    alloca.type = Type(Type::Kind::Ptr);
    alloca.operands = {Value::constInt(8)};
    entry.instructions.push_back(std::move(alloca));

    Instr store;
    store.op = Opcode::Store;
    store.type = Type(Type::Kind::I64);
    store.operands = {Value::temp(0), Value::constInt(7)};
    entry.instructions.push_back(std::move(store));

    Instr load;
    load.result = 1;
    load.op = Opcode::Load;
    load.type = Type(Type::Kind::I64);
    load.operands = {Value::temp(0)};
    entry.instructions.push_back(std::move(load));

    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands = {Value::temp(1)};
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;
    fn.valueNames.resize(2);
    module.functions.push_back(std::move(fn));

    auto verified = il::verify::Verifier::verify(module);
    EXPECT_TRUE(verified.hasValue());
}

TEST(ILCorrectness, TrappingDivisionViolatesNothrow) {
    Module module;
    Function fn = makeMain();
    fn.attrs().nothrow = true;
    BasicBlock &entry = fn.blocks.front();

    Instr div;
    div.result = 0;
    div.op = Opcode::SDivChk0;
    div.type = Type(Type::Kind::I64);
    div.operands = {Value::constInt(4), Value::constInt(2)};
    entry.instructions.push_back(std::move(div));

    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands = {Value::temp(0)};
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;
    fn.valueNames.resize(1);
    module.functions.push_back(std::move(fn));

    EXPECT_TRUE(verifyFailsWith(module, "nothrow function contains trapping instruction"));
}

TEST(ILCorrectness, VerifierTableMarksPlainDivRemAsTrapping) {
    for (Opcode op : {Opcode::SDiv, Opcode::UDiv, Opcode::SRem, Opcode::URem}) {
        auto props = il::verify::lookup(op);
        ASSERT_TRUE(props.has_value());
        EXPECT_TRUE(props->canTrap);
    }
}

TEST(ILCorrectness, ExternCallAttributesRequireMetadata) {
    Module module = parseModule(R"(il 0.3.0
extern @host_value() -> i64
func @main() -> i64 {
entry:
  %v = call @host_value() [pure, nothrow]
  ret %v
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "call attributes require callee effect metadata"));
}

TEST(ILCorrectness, ConstNullAllowsPointerLikeTypesOnly) {
    Module module;
    Function fn = makeMain(Type(Type::Kind::Error));
    BasicBlock &entry = fn.blocks.front();
    Instr nullErr;
    nullErr.result = 0;
    nullErr.op = Opcode::ConstNull;
    nullErr.type = Type(Type::Kind::Error);
    entry.instructions.push_back(std::move(nullErr));
    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands = {Value::temp(0)};
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;
    fn.valueNames.resize(1);
    module.functions.push_back(std::move(fn));
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());

    module.functions.front().blocks.front().instructions.front().type = Type(Type::Kind::I64);
    EXPECT_TRUE(verifyFailsWith(module, "const.null result type must be"));
}

TEST(ILCorrectness, ConstFoldMasksShiftCounts) {
    Module module;
    Function fn = makeMain();
    BasicBlock &entry = fn.blocks.front();
    Instr shl;
    shl.result = 0;
    shl.op = Opcode::Shl;
    shl.type = Type(Type::Kind::I64);
    shl.operands = {Value::constInt(1), Value::constInt(65)};
    entry.instructions.push_back(std::move(shl));
    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands = {Value::temp(0)};
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;
    fn.valueNames.resize(1);
    module.functions.push_back(std::move(fn));

    il::transform::constFold(module);
    const auto &retInstr = module.functions.front().blocks.front().instructions.back();
    ASSERT_EQ(retInstr.operands.size(), 1u);
    ASSERT_EQ(retInstr.operands.front().kind, Value::Kind::ConstInt);
    EXPECT_EQ(retInstr.operands.front().i64, 2);
}

TEST(ILCorrectness, BranchFoldingAcceptsCanonicalIntegerBooleanConstants) {
    Module module;
    Function fn = makeMain();
    BasicBlock &entry = fn.blocks.front();

    Instr cbr;
    cbr.op = Opcode::CBr;
    cbr.type = Type(Type::Kind::Void);
    cbr.operands = {Value::constInt(1)};
    cbr.labels = {"yes", "no"};
    cbr.brArgs = {{}, {}};
    entry.instructions.push_back(std::move(cbr));
    entry.terminated = true;

    BasicBlock yes;
    yes.label = "yes";
    Instr yesRet;
    yesRet.op = Opcode::Ret;
    yesRet.type = Type(Type::Kind::Void);
    yesRet.operands = {Value::constInt(1)};
    yes.instructions.push_back(std::move(yesRet));
    yes.terminated = true;

    BasicBlock no;
    no.label = "no";
    Instr noRet;
    noRet.op = Opcode::Ret;
    noRet.type = Type(Type::Kind::Void);
    noRet.operands = {Value::constInt(0)};
    no.instructions.push_back(std::move(noRet));
    no.terminated = true;

    fn.blocks.push_back(std::move(yes));
    fn.blocks.push_back(std::move(no));
    module.functions.push_back(std::move(fn));

    il::transform::SimplifyCFG pass(/*aggressive=*/true);
    il::transform::SimplifyCFG::Stats stats;
    pass.run(module.functions.front(), &stats);

    const Instr &term = module.functions.front().blocks.front().instructions.back();
    ASSERT_TRUE(term.op == Opcode::Br || term.op == Opcode::Ret);
    if (term.op == Opcode::Br) {
        ASSERT_EQ(term.labels.size(), 1u);
        EXPECT_EQ(term.labels.front(), "yes");
    } else {
        ASSERT_EQ(term.operands.size(), 1u);
        ASSERT_EQ(term.operands.front().kind, Value::Kind::ConstInt);
        EXPECT_EQ(term.operands.front().i64, 1);
    }
}

TEST(ILCorrectness, VerifierRejectsPartialFixedBranchArgumentBundles) {
    Module module;
    Function fn = makeMain();
    BasicBlock &entry = fn.blocks.front();

    Instr cbr;
    cbr.op = Opcode::CBr;
    cbr.type = Type(Type::Kind::Void);
    cbr.operands = {Value::constBool(true)};
    cbr.labels = {"yes", "no"};
    cbr.brArgs = {{}}; // Partial bundle list is malformed for a fixed two-edge terminator.
    entry.instructions.push_back(std::move(cbr));
    entry.terminated = true;

    for (const char *label : {"yes", "no"}) {
        BasicBlock block;
        block.label = label;
        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        ret.operands = {Value::constInt(0)};
        block.instructions.push_back(std::move(ret));
        block.terminated = true;
        fn.blocks.push_back(std::move(block));
    }

    module.functions.push_back(std::move(fn));
    EXPECT_TRUE(verifyFailsWith(module, "expected 2 branch argument bundles, or none"));
}

TEST(ILCorrectness, FunctionAttributesRoundTripAndDriveCallMetadata) {
    Module module = parseModule(R"(il 0.3.0
func @helper() -> i64 [nothrow, readonly, pure] {
entry:
  ret 7
}
func @main() -> i64 {
entry:
  %v = call @helper() [nothrow, readonly, pure]
  ret %v
}
)");

    ASSERT_EQ(module.functions.size(), 2u);
    EXPECT_TRUE(module.functions[0].attrs().nothrow);
    EXPECT_TRUE(module.functions[0].attrs().readonly);
    EXPECT_TRUE(module.functions[0].attrs().pure);
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());

    const std::string text = il::io::Serializer::toString(module);
    EXPECT_CONTAINS(text, "func @helper() -> i64 [nothrow, readonly, pure] {");

    Module reparsed = parseModule(text.c_str());
    ASSERT_EQ(reparsed.functions.size(), 2u);
    EXPECT_TRUE(reparsed.functions[0].attrs().nothrow);
    EXPECT_TRUE(reparsed.functions[0].attrs().readonly);
    EXPECT_TRUE(reparsed.functions[0].attrs().pure);
}

TEST(ILCorrectness, ParserAcceptsCommaDelimitedOperandsWithoutWhitespace) {
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %p = alloca 8
  store i64,%p,3
  %v = load i64,%p
  ret %v
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());
}

TEST(ILCorrectness, VariadicCallsRejectUnsupportedExtraArgumentTypes) {
    Module module = parseModule(R"(il 0.3.0
func @sink(i64 %x, ...) -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  %err:error = const_null
  call @sink(1, %err)
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "call vararg type mismatch"));
}

TEST(ILCorrectness, RuntimeStringArrayRegistryUsesStringElements) {
    using Kind = il::runtime::signatures::SigParam::Kind;
    il::runtime::signatures::register_array_signatures();
    const auto &signatures = il::runtime::signatures::all_signatures();

    auto find = [&](const std::string &name) {
        return std::find_if(signatures.begin(), signatures.end(), [&](const auto &sig) {
            return sig.name == name;
        });
    };

    auto get = find("rt_arr_str_get");
    ASSERT_TRUE(get != signatures.end());
    ASSERT_EQ(get->rets.size(), 1u);
    EXPECT_EQ(get->rets[0].kind, Kind::Str);

    auto put = find("rt_arr_str_put");
    ASSERT_TRUE(put != signatures.end());
    ASSERT_EQ(put->params.size(), 3u);
    EXPECT_EQ(put->params[2].kind, Kind::Str);
}

TEST(ILCorrectness, NothrowRejectsPotentiallyTrappingMemoryOperations) {
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 [nothrow] {
entry:
  %p = alloca 8
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "nothrow function contains trapping instruction"));
}

TEST(ILCorrectness, ExplicitCallIndirectSignaturesAreCheckedAndSerialized) {
    Module ok = parseModule(R"(il 0.3.0
func @main(ptr %fn, i64 %x) -> i64 {
entry:
  %r = call.indirect [i64(i64)] %fn(%x)
  ret %r
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(ok).hasValue());
    const std::string text = il::io::Serializer::toString(ok);
    EXPECT_CONTAINS(text, "call.indirect [i64(i64)] %fn(%x)");

    Module bad = parseModule(R"(il 0.3.0
func @main(ptr %fn) -> i64 {
entry:
  %r = call.indirect [i64(i64)] %fn(null)
  ret %r
}
)");
    EXPECT_TRUE(verifyFailsWith(bad, "call.indirect arg type mismatch"));
}

TEST(ILCorrectness, PointerCallIndirectRequiresSignatureAndResultBinding) {
    Module noSignature = parseModule(R"(il 0.3.0
func @main(ptr %fn) -> i64 {
entry(%fn:ptr):
  %r = call.indirect %fn()
  ret %r
}
)");
    EXPECT_TRUE(verifyFailsWith(noSignature,
                                "call.indirect through pointer requires an explicit signature"));

    Module missingResult = parseModule(R"(il 0.3.0
func @main(ptr %fn) -> i64 {
entry(%fn:ptr):
  call.indirect [i64()] %fn()
  ret 0
}
)");
    EXPECT_TRUE(
        verifyFailsWith(missingResult, "call.indirect to non-void signature requires a result"));
}

TEST(ILCorrectness, VerifierKeepsPtrAndStrTypesDistinct) {
    Module module = parseModule(R"(il 0.3.0
global const str @s = "hello"
func @sink(ptr %p) -> void {
entry(%p:ptr):
  ret
}
func @main() -> i64 {
entry:
  %s = const_str @s
  call @sink(%s)
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "call arg type mismatch: @sink parameter 0 expects ptr"));
}

TEST(ILCorrectness, RuntimeObjectParametersAcceptStringHandles) {
    const auto *sig = il::runtime::findRuntimeSignature("Zanna.Collections.Map.Set");
    ASSERT_NE(sig, nullptr);
    ASSERT_EQ(sig->paramTypes.size(), 3u);
    EXPECT_EQ(sig->paramTypes[0].kind, Type::Kind::Ptr);
    EXPECT_EQ(sig->paramTypes[1].kind, Type::Kind::Str);
    EXPECT_EQ(sig->paramTypes[2].kind, Type::Kind::Ptr);
    EXPECT_NE(sig->objectParamMask & 0x1u, 0u);
    EXPECT_NE(sig->objectParamMask & 0x4u, 0u);

    Module module = parseModule(R"(il 0.3.0
global const str @k = "key"
global const str @v = "value"
func @main() -> i64 {
entry:
  %k = const_str @k
  %v = const_str @v
  call @Zanna.Collections.Map.Set(null, %k, %v)
  ret 0
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());
}

TEST(ILCorrectness, DiscardingOwnedRuntimeReturnsIsRejected) {
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  call @rt_str_empty()
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "discarding owned return from @rt_str_empty"));
}

TEST(ILCorrectness, RuntimeF64ArrayHelpersAreVerified) {
    Module ok = parseModule(R"(il 0.3.0
func @main() -> f64 {
entry:
  %a = call @rt_arr_f64_new(2)
  call @rt_arr_f64_set(%a, 0, 1.5)
  %v = call @rt_arr_f64_get(%a, 0)
  call @rt_arr_f64_release(%a)
  ret %v
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(ok).hasValue());

    Module bad = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %a = call @rt_arr_f64_new(2)
  call @rt_arr_f64_set(%a, 0, 1)
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(bad, "@rt_arr_f64_set value operand must be f64"));
}

TEST(ILCorrectness, GepAllowsSignedOffsetsButRejectsStaticOutOfBoundsOffsets) {
    Module negativeWithinBounds = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %p = alloca 32
  %mid = gep %p, 16
  %q = gep %mid, -8
  store i64, %q, 1
  ret 0
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(negativeWithinBounds).hasValue());

    Module negativeOutOfBounds = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %p = alloca 8
  %q = gep %p, -1
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(negativeOutOfBounds, "gep offset outside alloca"));

    Module onePast = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %p = alloca 8
  %q = gep %p, 8
  ret 0
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(onePast).hasValue());

    Module onePastLoad = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %p = alloca 8
  %q = gep %p, 8
  %v = load i64, %q
  ret %v
}
)");
    EXPECT_TRUE(verifyFailsWith(onePastLoad, "load exceeds alloca bounds"));
}

TEST(ILCorrectness, StackLoadStoreBoundsUseAccessWidth) {
    Module loadTooWide = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %p = alloca 8
  %q = gep %p, 4
  %v = load i64, %q
  ret %v
}
)");
    EXPECT_TRUE(verifyFailsWith(loadTooWide, "load exceeds alloca bounds"));

    Module storeTooWide = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %p = alloca 8
  %q = gep %p, 6
  store i32, %q, 1
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(storeTooWide, "store exceeds alloca bounds"));
}

TEST(ILCorrectness, I64RuntimeArrayReleaseLifetimeIsTracked) {
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %a = call @rt_arr_i64_new(4)
  call @rt_arr_i64_release(%a)
  call @rt_arr_i64_release(%a)
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "double release"));
}

TEST(ILCorrectness, ParserRejectsInstructionsAfterTerminators) {
    const char *text = R"(il 0.3.0
func @main() -> i64 {
entry:
  ret 0
  %late = iadd.ovf 1, 2
}
)";
    Module module;
    std::istringstream input{text};
    auto parsed = il::io::Parser::parse(input, module);
    ASSERT_FALSE(parsed.hasValue());
    EXPECT_CONTAINS(parsed.error().message, "instruction appears after block terminator");
}

TEST(ILCorrectness, ConstFoldPreservesSgnF64NaN) {
    Module module = parseModule(R"(il 0.3.0
func @main() -> f64 {
entry:
  %v = call @rt_sgn_f64(NaN)
  ret %v
}
)");

    il::transform::constFold(module);
    const Function &fn = module.functions.front();
    const Instr &ret = fn.blocks.front().instructions.back();
    ASSERT_EQ(ret.operands.size(), 1u);
    ASSERT_EQ(ret.operands.front().kind, Value::Kind::ConstFloat);
    EXPECT_TRUE(std::isnan(ret.operands.front().f64));
}

TEST(ILCorrectness, CSEIncludesDeterministicPlainArithmetic) {
    Instr add;
    add.result = 0;
    add.op = Opcode::Add;
    add.type = Type(Type::Kind::I64);
    add.operands = {Value::temp(1), Value::temp(2)};
    EXPECT_TRUE(il::transform::makeValueKey(add).has_value());

    Instr checked;
    checked.result = 3;
    checked.op = Opcode::IAddOvf;
    checked.type = Type(Type::Kind::I64);
    checked.operands = {Value::temp(1), Value::temp(2)};
    EXPECT_TRUE(il::transform::makeValueKey(checked).has_value());
}

TEST(ILCorrectness, BasicAAReportsOpaqueTypeSizes) {
    EXPECT_EQ(zanna::analysis::BasicAA::typeSizeBytes(Type(Type::Kind::Error)), 24u);
    EXPECT_EQ(zanna::analysis::BasicAA::typeSizeBytes(Type(Type::Kind::ResumeTok)), 8u);
}

TEST(ILCorrectness, AllocaIsClassifiedAsMemoryWriting) {
    EXPECT_EQ(memoryEffects(Opcode::Alloca), MemoryEffects::Write);
    EXPECT_TRUE(hasMemoryWrite(Opcode::Alloca));
}

TEST(ILCorrectness, SimplifyCFGKeepsBlocksReferencedByRetainedEHBlocks) {
    Function fn = makeMain();
    fn.blocks.front().instructions.clear();
    fn.blocks.front().terminated = true;
    Instr retEntry;
    retEntry.op = Opcode::Ret;
    retEntry.type = Type(Type::Kind::Void);
    retEntry.operands = {Value::constInt(0)};
    fn.blocks.front().instructions.push_back(std::move(retEntry));

    BasicBlock handler;
    handler.label = "handler";
    handler.params.push_back(Param{"err", Type(Type::Kind::Error), 0});
    handler.params.push_back(Param{"tok", Type(Type::Kind::ResumeTok), 1});
    Instr ehEntry;
    ehEntry.op = Opcode::EhEntry;
    handler.instructions.push_back(std::move(ehEntry));
    Instr brCleanup;
    brCleanup.op = Opcode::Br;
    brCleanup.labels = {"cleanup"};
    handler.instructions.push_back(std::move(brCleanup));
    handler.terminated = true;
    fn.blocks.push_back(std::move(handler));

    BasicBlock cleanup;
    cleanup.label = "cleanup";
    Instr retCleanup;
    retCleanup.op = Opcode::Ret;
    retCleanup.type = Type(Type::Kind::Void);
    retCleanup.operands = {Value::constInt(0)};
    cleanup.instructions.push_back(std::move(retCleanup));
    cleanup.terminated = true;
    fn.blocks.push_back(std::move(cleanup));

    BasicBlock dead;
    dead.label = "dead";
    Instr retDead;
    retDead.op = Opcode::Ret;
    retDead.type = Type(Type::Kind::Void);
    retDead.operands = {Value::constInt(0)};
    dead.instructions.push_back(std::move(retDead));
    dead.terminated = true;
    fn.blocks.push_back(std::move(dead));

    il::transform::SimplifyCFG::Stats stats;
    il::transform::SimplifyCFG::SimplifyCFGPassContext ctx(fn, nullptr, stats);
    EXPECT_TRUE(il::transform::simplify_cfg::removeUnreachableBlocks(ctx));
    EXPECT_TRUE(hasBlockLabel(fn, "handler"));
    EXPECT_TRUE(hasBlockLabel(fn, "cleanup"));
    EXPECT_FALSE(hasBlockLabel(fn, "dead"));
}

TEST(ILCorrectness, ParamCanonicalizationSkipsMalformedEdgesWithoutAsserting) {
    Function fn;
    fn.name = "param_canonical_malformed_edge";
    fn.retType = Type(Type::Kind::I64);

    BasicBlock entry;
    entry.label = "entry";
    Instr br;
    br.op = Opcode::Br;
    br.labels = {"target"};
    entry.instructions.push_back(std::move(br));
    entry.terminated = true;
    fn.blocks.push_back(std::move(entry));

    BasicBlock target;
    target.label = "target";
    target.params.push_back(Param{"unused", Type(Type::Kind::I64), 0});
    target.params.push_back(Param{"used", Type(Type::Kind::I64), 1});
    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands = {Value::temp(1)};
    target.instructions.push_back(std::move(ret));
    target.terminated = true;
    fn.blocks.push_back(std::move(target));

    il::transform::SimplifyCFG::Stats stats;
    il::transform::SimplifyCFG::SimplifyCFGPassContext ctx(fn, nullptr, stats);
    EXPECT_TRUE(il::transform::simplify_cfg::canonicalizeParamsAndArgs(ctx));
    ASSERT_EQ(fn.blocks[1].params.size(), 1u);
    EXPECT_EQ(fn.blocks[1].params[0].id, 1u);
}

TEST(ILCorrectness, IntegerLiteralParsingIsConsistentAcrossOperandsAndGlobals) {
    Module module = parseModule(R"(il 0.3.0
global i64 @hex = 0x10
global i64 @bin = 0b1010
func @main() -> i64 {
entry:
  ret -0xfeed
}
)");
    ASSERT_EQ(module.globals.size(), 2u);
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());
    EXPECT_EQ(module.functions.front().blocks.front().instructions.back().operands.front().i64,
              -0xfeedLL);

    long long minValue = 0;
    EXPECT_TRUE(il::io::parseIntegerLiteral(
        "-0b1000000000000000000000000000000000000000000000000000000000000000", minValue));
    EXPECT_EQ(minValue, std::numeric_limits<long long>::min());
}

TEST(ILCorrectness, ExternAndImportAttributesRoundTripAndValidateCalls) {
    Module module = parseModule(R"(il 0.3.0
extern @host_value() -> i64 [nothrow, readonly, pure]
func import @foreign(ptr %p) -> void [nothrow, readonly]
func @main(ptr %p) -> i64 {
entry(%p:ptr):
  %v = call @host_value() [nothrow, readonly, pure]
  call @foreign(%p) [nothrow, readonly]
  ret %v
}
)");
    ASSERT_EQ(module.externs.size(), 1u);
    EXPECT_TRUE(module.externs.front().attrs().pure);
    ASSERT_EQ(module.functions.size(), 2u);
    EXPECT_TRUE(module.functions.front().attrs().readonly);
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());

    const std::string text = il::io::Serializer::toString(module);
    EXPECT_CONTAINS(text, "extern @host_value() -> i64 [nothrow, readonly, pure]");
    EXPECT_CONTAINS(text, "func import @foreign(ptr %p) -> void [nothrow, readonly]");
    Module reparsed = parseModule(text.c_str());
    ASSERT_EQ(reparsed.externs.size(), 1u);
    EXPECT_TRUE(reparsed.externs.front().attrs().pure);
    EXPECT_TRUE(reparsed.functions.front().attrs().readonly);
}

TEST(ILCorrectness, ExternParametersRejectInternalOnlyTypes) {
    EXPECT_TRUE(parseFailsWith(R"(il 0.3.0
extern @bad(void) -> void
)",
                               "unsupported extern parameter type"));

    Module programmatic;
    programmatic.externs.push_back(
        Extern{"bad", Type(Type::Kind::Void), {Type(Type::Kind::ResumeTok)}});
    EXPECT_TRUE(verifyFailsWith(programmatic, "unsupported parameter type resume_tok"));
}

TEST(ILCorrectness, StringGlobalsRequireInitializersInVerifier) {
    Module module;
    module.globals.push_back(Global{"s", Type(Type::Kind::Str), ""});
    EXPECT_TRUE(verifyFailsWith(module, "string global @s requires an initializer"));
}

TEST(ILCorrectness, LocalAndExternEffectsDriveOptimizationAndAA) {
    Module module = parseModule(R"(il 0.3.0
extern @read_host(ptr) -> i64 [nothrow, readonly]
func @pure_local(i64 %x) -> i64 [nothrow, pure] {
entry(%x:i64):
  ret %x
}
func @main(ptr %p) -> i64 {
entry(%p:ptr):
  %dead = call @pure_local(7)
  %v = call @read_host(%p)
  ret %v
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());
    zanna::analysis::BasicAA aa(module, module.functions.back());
    const Instr &readCall = module.functions.back().blocks.front().instructions[1];
    EXPECT_EQ(aa.modRef(readCall), zanna::analysis::ModRefResult::Ref);

    il::transform::dce(module);
    EXPECT_EQ(countOpcode(module.functions.back(), Opcode::Call), 1u);
}

TEST(ILCorrectness, StackPointersCanBeBorrowedByDirectCallsButNotEscaped) {
    Module borrowed = parseModule(R"(il 0.3.0
func @sink(ptr %p) -> void {
entry(%p:ptr):
  ret
}
func @main() -> i64 {
entry:
  %p = alloca 8
  call @sink(%p)
  ret 0
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(borrowed).hasValue());

    Module returned = parseModule(R"(il 0.3.0
func @main() -> ptr {
entry:
  %p = alloca 8
  ret %p
}
)");
    EXPECT_TRUE(verifyFailsWith(returned, "returning alloca-derived pointer"));

    Module indirect = parseModule(R"(il 0.3.0
func @main(ptr %fn) -> i64 {
entry(%fn:ptr):
  %p = alloca 8
  call.indirect [void(ptr)] %fn(%p)
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(indirect, "passing alloca-derived pointer"));
}

TEST(ILCorrectness, ParserRejectsMissingCommasAndEmptyIndirectCallee) {
    EXPECT_TRUE(parseFailsWith(R"(il 0.3.0
func @main() -> i64 {
entry:
  %v = iadd.ovf 1 2
  ret %v
}
)",
                               "missing comma"));

    EXPECT_TRUE(parseFailsWith(R"(il 0.3.0
func @main(i64 %x) -> i64 {
entry(%x:i64):
  %r = call.indirect [i64(i64)] (%x)
  ret %r
}
)",
                               "call.indirect missing callee"));
}

TEST(ILCorrectness, DirectCallsDoNotCoerceIntegerLiteralsToF64) {
    Module module = parseModule(R"(il 0.3.0
func @takes_f64(f64 %x) -> void {
entry(%x:f64):
  ret
}
func @main() -> i64 {
entry:
  call @takes_f64(1)
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "call arg type mismatch"));
}

TEST(ILCorrectness, SwitchI32AcceptsFittingIntegerLiterals) {
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  switch.i32 1, ^default, 1 -> ^one
default:
  ret 0
one:
  ret 1
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());
}

TEST(ILCorrectness, SerializerMarksMalformedOperands) {
    Function fn = makeMain();
    BasicBlock &entry = fn.blocks.front();
    entry.instructions.clear();

    Instr br;
    br.op = Opcode::Br;
    br.type = Type(Type::Kind::Void);
    entry.instructions.push_back(std::move(br));

    Instr sw;
    sw.op = Opcode::SwitchI32;
    sw.type = Type(Type::Kind::Void);
    entry.instructions.push_back(std::move(sw));

    Instr indirect;
    indirect.op = Opcode::CallIndirect;
    indirect.type = Type(Type::Kind::Void);
    entry.instructions.push_back(std::move(indirect));
    entry.terminated = true;

    Module module;
    module.functions.push_back(std::move(fn));
    bool threw = false;
    try {
        (void)il::io::Serializer::toString(module);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    EXPECT_TRUE(threw);

    const std::string text = il::io::Serializer::toString(module, il::io::Serializer::Mode::Debug);
    EXPECT_CONTAINS(text, "br ; missing label");
    EXPECT_CONTAINS(text, "switch.i32 ; missing scrutinee");
    EXPECT_CONTAINS(text, "call.indirect ; missing callee");
}

TEST(ILCorrectness, RuntimeOwnershipMetadataDrivesReleaseChecks) {
    Module module = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %obj = call @rt_obj_new_i64(0, 8)
  call @rt_obj_release_check0(%obj)
  call @rt_obj_release_check0(%obj)
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(module, "double release"));
}

TEST(ILCorrectness, RuntimeOwnershipAllowsObjectFinalizationAfterReleaseCheck) {
    Module module = parseModule(R"(il 0.3.0
func @Klass.destroy(ptr %self) -> void {
entry(%self:ptr):
  ret
}
func @Klass.__dtor(ptr %self) -> void {
entry(%self:ptr):
  ret
}
func @main() -> i64 {
entry:
  %obj = call @rt_obj_new_i64(0, 8)
  call @rt_obj_release_check0(%obj)
  call @Klass.destroy(%obj)
  call @Klass.__dtor(%obj)
  call @rt_obj_free(%obj)
  ret 0
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(module).hasValue());
}

TEST(ILCorrectness, RuntimeOwnershipOnlyLinearizesExplicitReleases) {
    Module concat = parseModule(R"(il 0.3.0
global const str @a = "a"
global const str @b = "b"
func @main() -> str {
entry:
  %a = const_str @a
  %b = const_str @b
  %ab:str = call @rt_str_concat(%a, %b)
  call @rt_str_retain_maybe(%a)
  ret %ab
}
)");
    EXPECT_TRUE(il::verify::Verifier::verify(concat).hasValue());

    Module releaseTwice = parseModule(R"(il 0.3.0
func @main() -> i64 {
entry:
  %s:str = call @rt_str_empty()
  call @rt_str_release_maybe(%s)
  call @rt_str_release_maybe(%s)
  ret 0
}
)");
    EXPECT_TRUE(verifyFailsWith(releaseTwice, "double release"));
}

TEST(ILCorrectness, DceKeepsPotentiallyTrappingAllocas) {
    Module module = parseModule(R"(il 0.3.0
func @main(i64 %n) -> i64 {
entry(%n:i64):
  %p = alloca %n
  ret 0
}
)");
    il::transform::dce(module);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::Alloca), 1u);
}

TEST(ILCorrectness, DceRemovesDeadPureArithmeticChains) {
    Module module = parseModule(R"(il 0.3.0
func @main(i64 %n) -> i64 {
entry(%n:i64):
  %a = add %n, 1
  %b = mul %a, 2
  %c = fdiv 1.0, 0.0
  ret 0
}
)");
    il::transform::dce(module);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::Add), 0u);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::Mul), 0u);
    EXPECT_EQ(countOpcode(module.functions.front(), Opcode::FDiv), 0u);
}

TEST(ILCorrectness, DceCollapsesLongDeadChainWithWorklist) {
    Module module;
    Function fn = makeMain();
    fn.params.push_back({"seed", Type(Type::Kind::I64), 0});
    BasicBlock &entry = fn.blocks.front();
    unsigned previous = 0;
    constexpr unsigned kChainLength = 4096;
    for (unsigned id = 1; id <= kChainLength; ++id) {
        Instr add;
        add.op = Opcode::Add;
        add.result = id;
        add.type = Type(Type::Kind::I64);
        add.operands = {Value::temp(previous), Value::constInt(1)};
        entry.instructions.push_back(std::move(add));
        previous = id;
    }
    Instr ret;
    ret.op = Opcode::Ret;
    ret.operands = {Value::constInt(0)};
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;
    module.functions.push_back(std::move(fn));

    il::transform::dce(module);
    ASSERT_EQ(module.functions.front().blocks.front().instructions.size(), 1u);
    EXPECT_EQ(module.functions.front().blocks.front().instructions.front().op, Opcode::Ret);
}

// ZB-28: the standard IL VM (used by `zanna run --trace/--profile`) must accept
// loads and stores through rt_modvar_addr_* storage — the backing store of every
// Zia module-level variable — which lives outside the runtime heap registry.
TEST(ILCorrectness, StandardVmAcceptsModuleVariableStores) {
    Module module = parseModule(R"(il 0.3.0
extern @rt_modvar_addr_i64(str) -> ptr
global const str @.L0 = "zb28_g"
func @main() -> i64 {
entry:
  %t0 = const_str @.L0
  %t1:ptr = call @rt_modvar_addr_i64(%t0)
  store i64, %t1, 40
  %t2 = load i64, %t1
  %t3 = iadd.ovf %t2, 2
  store i64, %t1, %t3
  %t4 = load i64, %t1
  ret %t4
}
)");
    il::vm::RunConfig cfg;
    il::vm::Runner runner(module, cfg);
    const int64_t result = runner.run();
    const auto trap = runner.lastTrapMessage();
    if (trap && !trap->empty())
        std::cerr << "unexpected trap: " << *trap << "\n";
    EXPECT_FALSE(trap.has_value() && !trap->empty());
    EXPECT_EQ(result, 42);
}

// ZB-28 (second half): class descriptors and other runtime tables are plain
// rt_alloc blocks; the reference VM must accept stores into them as well.
TEST(ILCorrectness, StandardVmAcceptsRawAllocationStores) {
    Module module = parseModule(R"(il 0.3.0
extern @rt_alloc(i64) -> ptr
func @main() -> i64 {
entry:
  %t0:ptr = call @rt_alloc(16)
  %t1 = gep %t0, 8
  store i64, %t1, 7
  store i64, %t0, 35
  %t2 = load i64, %t0
  %t3 = load i64, %t1
  %t4 = iadd.ovf %t2, %t3
  ret %t4
}
)");
    il::vm::RunConfig cfg;
    il::vm::Runner runner(module, cfg);
    const int64_t result = runner.run();
    const auto trap = runner.lastTrapMessage();
    if (trap && !trap->empty())
        std::cerr << "unexpected trap: " << *trap << "\n";
    EXPECT_FALSE(trap.has_value() && !trap->empty());
    EXPECT_EQ(result, 42);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
