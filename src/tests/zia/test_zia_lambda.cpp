//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Unit tests for Zia lambda expressions.
//
//===----------------------------------------------------------------------===//

#include "frontends/zia/Compiler.hpp"
#include "il/core/Opcode.hpp"
#include "support/source_manager.hpp"
#include "tests/TestHarness.hpp"
#include <string>

using namespace il::frontends::zia;
using namespace il::support;

namespace {

const il::core::Function *findLambdaFunction(const il::core::Module &module) {
    for (const auto &fn : module.functions) {
        if (fn.name.find("__lambda_") != std::string::npos)
            return &fn;
    }
    return nullptr;
}

bool hasCallWithConstIntArg(const il::core::Function &fn,
                            const std::string &callee,
                            int64_t value) {
    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.op != il::core::Opcode::Call || instr.callee != callee ||
                instr.operands.empty()) {
                continue;
            }
            for (const auto &operand : instr.operands) {
                if (operand.kind == il::core::Value::Kind::ConstInt && operand.i64 == value)
                    return true;
            }
        }
    }
    return false;
}

/// @brief Test that lambda with block body compiles.
TEST(ZiaLambda, LambdaWithBlockBody) {
    SourceManager sm;
    const std::string source = R"(
module Test;

func start() {    var greet = () => {
        Zanna.Terminal.Say("Hello");
    };
}
)";
    CompilerInput input{.source = source, .path = "lambda_block.zia"};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for LambdaWithBlockBody:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());

    bool foundLambdaFunc = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name.find("lambda") != std::string::npos) {
            foundLambdaFunc = true;
            break;
        }
    }
    EXPECT_TRUE(foundLambdaFunc);
}

/// @brief Test that block-body lambdas capture outer locals and lay out envs with alignment.
TEST(ZiaLambda, BlockBodyLambdaCapturesAndAlignsEnvironment) {
    SourceManager sm;
    const std::string source = R"(
module Test;

func start() {    var b: Byte = 1;
    var i: Integer = 2;
    var ready: Boolean = true;
    var f = () => {
        if ready {
            Zanna.Terminal.SayInt(i);
        }
        var copy = b;
    };
}
)";
    CompilerInput input{.source = source, .path = "lambda_capture_block.zia"};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);

    ASSERT_TRUE(result.succeeded());
    const il::core::Function *mainFn = nullptr;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "main") {
            mainFn = &fn;
            break;
        }
    }
    ASSERT_TRUE(mainFn != nullptr);
    // One managed closure record: [code, env] (16) + inline env
    // {Boolean @0, Integer @8, Byte @16} (24; a Byte is carried in i64).
    EXPECT_TRUE(hasCallWithConstIntArg(*mainFn, "rt_obj_new_i64", 40));
    ASSERT_TRUE(findLambdaFunction(result.module) != nullptr);
}

/// @brief Test typed multi-parameter lambdas.
TEST(ZiaLambda, MultiParameterTypedLambda) {
    SourceManager sm;
    const std::string source = R"(
module Test;

func start() {
    var add = (a: Integer, b: Integer) => a + b;
    var value = add(10, 32);
    Zanna.Terminal.SayInt(value);
}
)";
    CompilerInput input{.source = source, .path = "lambda_multi_typed.zia"};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);

    EXPECT_TRUE(result.succeeded());
    ASSERT_TRUE(findLambdaFunction(result.module) != nullptr);
}

} // namespace

int main() {
    return zanna_test::run_all_tests();
}
