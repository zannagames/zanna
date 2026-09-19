//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Unit tests for Zia properties (get/set) and static members.
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

// ============================================================================
// Helpers
// ============================================================================

static bool hasFunction(const il::core::Module &mod, const std::string &fnName) {
    for (const auto &fn : mod.functions) {
        if (fn.name == fnName)
            return true;
    }
    return false;
}

static bool hasCallee(const il::core::Module &mod,
                      const std::string &fnName,
                      const std::string &callee) {
    for (const auto &fn : mod.functions) {
        if (fn.name == fnName) {
            for (const auto &block : fn.blocks) {
                for (const auto &instr : block.instructions) {
                    if (instr.op == il::core::Opcode::Call && instr.callee == callee)
                        return true;
                }
            }
        }
    }
    return false;
}

static size_t countCallsTo(const il::core::Module &mod,
                           const std::string &fnName,
                           const std::string &callee) {
    size_t count = 0;
    for (const auto &fn : mod.functions) {
        if (fn.name != fnName)
            continue;
        for (const auto &block : fn.blocks) {
            for (const auto &instr : block.instructions) {
                if (instr.op == il::core::Opcode::Call && instr.callee == callee)
                    ++count;
            }
        }
    }
    return count;
}

/// @brief Check if a function has a "self" parameter.
static bool hasSelfParam(const il::core::Module &mod, const std::string &fnName) {
    for (const auto &fn : mod.functions) {
        if (fn.name == fnName) {
            for (const auto &param : fn.params) {
                if (param.name == "self")
                    return true;
            }
            return false;
        }
    }
    return false;
}

/// @brief Check if a string global carries the requested module-storage key.
static bool hasStringGlobalValue(const il::core::Module &mod, const std::string &value) {
    for (const auto &g : mod.globals) {
        if (g.init == value)
            return true;
    }
    return false;
}

static bool hasErrorContaining(const CompilerResult &result, const std::string &needle) {
    for (const auto &d : result.diagnostics.diagnostics()) {
        if (d.severity == Severity::Error && d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// ============================================================================
// Property tests
// ============================================================================

/// @brief Test that a property with a getter synthesizes get_PropertyName.
TEST(ZiaProperties, GetterSynthesized) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Circle {
    expose Number radius;

    property area: Number {
        get {
            return self.radius * self.radius;
        }
    }
}

func start() {    var c = new Circle();
}
)";

    CompilerInput input{.source = source, .path = "test_prop_get.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());

    // Should have synthesized get_area method
    EXPECT_TRUE(hasFunction(result.module, "Circle.get_area"));
    // Getter should have self parameter
    EXPECT_TRUE(hasSelfParam(result.module, "Circle.get_area"));
}

/// @brief Test property with getter and setter.
TEST(ZiaProperties, GetterAndSetter) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Temperature {
    expose Number celsius;

    property fahrenheit: Number {
        get {
            return self.celsius * 1.8 + 32.0;
        }
        set(f) {
            self.celsius = (f - 32.0) / 1.8;
        }
    }
}

func start() {    var t = new Temperature();
}
)";

    CompilerInput input{.source = source, .path = "test_prop_getset.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());

    // Should have synthesized both get_ and set_ methods
    EXPECT_TRUE(hasFunction(result.module, "Temperature.get_fahrenheit"));
    EXPECT_TRUE(hasFunction(result.module, "Temperature.set_fahrenheit"));
}

/// @brief Property access syntax should lower through synthesized getter/setter methods.
TEST(ZiaProperties, MemberAccessUsesSynthesizedAccessors) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Counter {
    hide Integer _count;

    expose func init() {        _count = 0;
    }

    expose property count: Integer {
        get {
            return _count;
        }
        set(v) {
            _count = v;
        }
    }
}

func start() {    var c = new Counter();
    c.count = 42;
    Zanna.Terminal.SayInt(c.count);
}
)";

    CompilerInput input{.source = source, .path = "test_prop_member_access.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "Counter.get_count"));
    EXPECT_TRUE(hasFunction(result.module, "Counter.set_count"));
    EXPECT_TRUE(hasCallee(result.module, "main", "Counter.get_count"));
    EXPECT_TRUE(hasCallee(result.module, "main", "Counter.set_count"));
}

/// @brief Setter-only properties must remain valid assignment targets.
TEST(ZiaProperties, WriteOnlyPropertyAssignmentUsesSetter) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Sink {
    hide Integer _value;

    expose property value: Integer {
        set(v) {
            _value = v;
        }
    }
}

func start() {    var s = new Sink();
    s.value = 42;
}
)";

    CompilerInput input{.source = source, .path = "test_prop_write_only.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "Sink.set_value"));
    EXPECT_TRUE(hasCallee(result.module, "main", "Sink.set_value"));
    EXPECT_FALSE(hasFunction(result.module, "Sink.get_value"));
}

/// @brief Reading a setter-only property should still fail with a write-only diagnostic.
TEST(ZiaProperties, WriteOnlyPropertyReadFails) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Sink {
    expose property value: Integer {
        set(v) { }
    }
}

func start() {    var s = new Sink();
    var x = s.value;
}
)";

    CompilerInput input{.source = source, .path = "test_prop_write_only_read.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    EXPECT_FALSE(result.succeeded());

    bool sawWriteOnly = false;
    for (const auto &d : result.diagnostics.diagnostics()) {
        if (d.message.find("write-only") != std::string::npos) {
            sawWriteOnly = true;
            break;
        }
    }
    EXPECT_TRUE(sawWriteOnly);
}

// ============================================================================
// Static member tests
// ============================================================================

/// @brief Test that static methods don't have self parameter.
TEST(ZiaStatic, StaticMethodNoSelf) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Counter {
    expose Integer value;

    static func create() -> Integer {        return 42;
    }
}

func start() {    var c = new Counter();
}
)";

    CompilerInput input{.source = source, .path = "test_static_method.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());

    // Static method should exist
    EXPECT_TRUE(hasFunction(result.module, "Counter.create"));
    // Static method should NOT have self parameter
    EXPECT_FALSE(hasSelfParam(result.module, "Counter.create"));
}

/// @brief Test that static fields are excluded from instance layout.
/// @details Static fields don't contribute to the class's instance size,
/// and are stored at module level. We verify the class compiles successfully
/// with a static field declaration.
TEST(ZiaStatic, StaticFieldCompiles) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Config {
    expose Integer value;
    static Integer count = 0;
}

func start() {    var c = new Config();
}
)";

    CompilerInput input{.source = source, .path = "test_static_field.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());
}

TEST(ZiaStatic, StaticFieldReadWriteThroughType) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Config {
    expose static Integer count = 0;
}

func start() {
    Config.count = Config.count + 1;
    Zanna.Terminal.SayInt(Config.count);
}
)";

    CompilerInput input{.source = source, .path = "test_static_field_access.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());
    EXPECT_GE(countCallsTo(result.module, "main", "rt_modvar_addr_i64"), static_cast<size_t>(3));
    EXPECT_TRUE(hasCallee(result.module, "main", "Zanna.Terminal.SayInt"));
    EXPECT_TRUE(hasStringGlobalValue(result.module, "Config.count"));
}

/// @brief ZB-47: `Type.staticMethod()` calls the method directly — the type name is never
///        lowered as a value and no self argument is passed.
TEST(ZiaStatic, StaticMethodCallThroughTypeNameTakesNoSelf) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Counter {
    expose static func create(seed: Integer) -> Integer {
        return seed + 41;
    }
}

func start() {
    Zanna.Terminal.SayInt(Counter.create(1));
}
)";

    CompilerInput input{.source = source, .path = "test_static_method_call.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    ASSERT_TRUE(result.succeeded());

    size_t argCount = 99;
    for (const auto &fn : result.module.functions) {
        if (fn.name != "main")
            continue;
        for (const auto &block : fn.blocks) {
            for (const auto &instr : block.instructions) {
                if (instr.op == il::core::Opcode::Call && instr.callee == "Counter.create")
                    argCount = instr.operands.size();
            }
        }
    }
    EXPECT_EQ(argCount, static_cast<size_t>(1)); // the seed only; no self
    EXPECT_FALSE(hasSelfParam(result.module, "Counter.create"));
}

/// @brief ZB-48: a bare static field inside its class reads and writes the module global —
///        the read no longer reaches lowering unresolved, and the write no longer defines a
///        local that silently drops the store.
TEST(ZiaStatic, BareStaticFieldInsideClassUsesModuleGlobal) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Counter {
    expose static Integer bumps;

    expose static func bump() -> Integer {
        bumps = bumps + 1;
        return bumps;
    }
}

func start() {
    Zanna.Terminal.SayInt(Counter.bump());
}
)";

    CompilerInput input{.source = source, .path = "test_static_field_bare.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    ASSERT_TRUE(result.succeeded());
    // Read, write and the returned read all address the global.
    EXPECT_GE(countCallsTo(result.module, "Counter.bump", "rt_modvar_addr_i64"),
              static_cast<size_t>(3));
    EXPECT_TRUE(hasStringGlobalValue(result.module, "Counter.bumps"));
}

TEST(ZiaStatic, InheritedPrivateFieldStaysPrivate) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Base {
    hide Integer secret;
}

class Child extends Base {
}

func start() {
    var child = new Child();
    var leaked: Integer = child.secret;
}
)";

    CompilerInput input{.source = source, .path = "test_inherited_private_field.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(hasErrorContaining(result, "Cannot access private member"));
}

/// @brief Test that non-static methods still have self.
TEST(ZiaStatic, NonStaticMethodHasSelf) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Box {
    expose Integer width;

    func getWidth() -> Integer {        return self.width;
    }
}

func start() {    var b = new Box();
}
)";

    CompilerInput input{.source = source, .path = "test_nonstatic.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    ASSERT_TRUE(result.succeeded());

    // Non-static method should have self parameter
    EXPECT_TRUE(hasSelfParam(result.module, "Box.getWidth"));
}

} // anonymous namespace

int main() {
    return zanna_test::run_all_tests();
}
