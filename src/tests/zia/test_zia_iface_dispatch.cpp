//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/zia/test_zia_iface_dispatch.cpp
// Purpose: Verify Zia interface dispatch and generated runtime type initialization.
// Key invariants:
//   - Interface calls lower through runtime itable lookup and call.indirect.
//   - Classes and interfaces register before their itables are bound.
//   - Large class registries are partitioned into ordered, bounded helpers.
// Ownership/Lifetime:
//   - Every compiler result and synthesized source buffer is test-owned.
// Cross-platform touchpoints:
//   - Tests inspect target-neutral IL so batching remains valid for every native backend.
// Links: src/frontends/zia/Lowerer_Decl_Types.cpp,
//        src/codegen/aarch64/LoweringContext.hpp
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

/// @brief Check if a function contains a Call to a specific callee.
static bool hasCall(const il::core::Module &mod,
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

/// @brief Check if a function contains a specific opcode.
static bool hasOpcode(const il::core::Module &mod, const std::string &fnName, il::core::Opcode op) {
    for (const auto &fn : mod.functions) {
        if (fn.name == fnName) {
            for (const auto &block : fn.blocks) {
                for (const auto &instr : block.instructions) {
                    if (instr.op == op)
                        return true;
                }
            }
        }
    }
    return false;
}

/// @brief Check if a named function exists in the module.
static bool hasFunction(const il::core::Module &mod, const std::string &fnName) {
    for (const auto &fn : mod.functions) {
        if (fn.name == fnName)
            return true;
    }
    return false;
}

/// @brief Helper to dump diagnostics for debugging.
static void dumpDiags(const CompilerResult &result) {
    for (const auto &d : result.diagnostics.diagnostics()) {
        fprintf(stderr, "  [%s] %s\n", d.code.c_str(), d.message.c_str());
    }
}

// ============================================================================
// Interface itable dispatch tests
// ============================================================================

/// @brief Basic interface dispatch: verify __zia_iface_init is emitted and
///        start() calls it.
TEST(ZiaIfaceDispatch, EmitsItableInit) {
    SourceManager sm;
    const std::string source = R"(
module Test;

interface IShape {
    func area() -> Number;}

class Circle implements IShape {
    expose Number radius;
    expose func area() -> Number { return 3.14 * self.radius * self.radius; }}

func start() {    var c = new Circle();
}
)";
    CompilerInput input{.source = source, .path = "iface.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    if (!result.succeeded())
        dumpDiags(result);
    EXPECT_TRUE(result.succeeded());

    // __zia_iface_init function should exist
    EXPECT_TRUE(hasFunction(result.module, "__zia_iface_init"));

    // main (mangled from start) should call __zia_iface_init
    EXPECT_TRUE(hasCall(result.module, "main", "__zia_iface_init"));

    // __zia_iface_init should call the runtime-string registration bridge
    EXPECT_TRUE(hasCall(result.module, "__zia_iface_init", "rt_register_interface_direct_rs"));

    // __zia_iface_init should register class metadata before binding interfaces.
    EXPECT_TRUE(hasCall(result.module, "__zia_iface_init", "rt_register_class_with_base_rs"));

    // __zia_iface_init should call rt_bind_interface
    EXPECT_TRUE(hasCall(result.module, "__zia_iface_init", "rt_bind_interface"));
}

/// @brief Verify that interface method calls emit rt_get_interface_impl + call.indirect.
TEST(ZiaIfaceDispatch, ItableLookupAndCallIndirect) {
    SourceManager sm;
    const std::string source = R"(
module Test;

interface IGreeter {
    func greet() -> String;}

class HelloGreeter implements IGreeter {
    expose func greet() -> String { return "Hello"; }}

func greetWith(g: IGreeter) -> String {    return g.greet();
}

func start() {    var h = new HelloGreeter();
    var msg = greetWith(h);
}
)";
    CompilerInput input{.source = source, .path = "iface.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    if (!result.succeeded())
        dumpDiags(result);
    EXPECT_TRUE(result.succeeded());

    // greetWith should call rt_get_interface_impl for dispatch
    EXPECT_TRUE(hasCall(result.module, "greetWith", "rt_get_interface_impl"));

    // greetWith should use call.indirect for the dispatched call
    EXPECT_TRUE(hasOpcode(result.module, "greetWith", il::core::Opcode::CallIndirect));
}

/// @brief Multiple implementors of the same interface all get itable entries.
TEST(ZiaIfaceDispatch, MultipleImplementors) {
    SourceManager sm;
    const std::string source = R"(
module Test;

interface IAnimal {
    func speak() -> String;}

class Dog implements IAnimal {
    expose func speak() -> String { return "Woof"; }}

class Cat implements IAnimal {
    expose func speak() -> String { return "Meow"; }}

func animalSpeak(a: IAnimal) -> String {    return a.speak();
}

func start() {    var d = new Dog();
    var c = new Cat();
    var s1 = animalSpeak(d);
    var s2 = animalSpeak(c);
}
)";
    CompilerInput input{.source = source, .path = "iface.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    if (!result.succeeded())
        dumpDiags(result);
    EXPECT_TRUE(result.succeeded());

    // rt_alloc should be called in init to allocate itables
    EXPECT_TRUE(hasCall(result.module, "__zia_iface_init", "rt_alloc"));

    // animalSpeak should use itable dispatch
    EXPECT_TRUE(hasCall(result.module, "animalSpeak", "rt_get_interface_impl"));
    EXPECT_TRUE(hasOpcode(result.module, "animalSpeak", il::core::Opcode::CallIndirect));
}

/// @brief Interface with multiple methods — verify slot-based dispatch.
TEST(ZiaIfaceDispatch, MultipleSlots) {
    SourceManager sm;
    const std::string source = R"(
module Test;

interface IShape {
    func area() -> Number;    func perimeter() -> Number;}

class Rect implements IShape {
    expose Number w;
    expose Number h;
    expose func area() -> Number { return self.w * self.h; }    expose func perimeter() -> Number { return 2.0 * (self.w + self.h); }}

func computeArea(s: IShape) -> Number {    return s.area();
}

func computePerimeter(s: IShape) -> Number {    return s.perimeter();
}

func start() {    var r = new Rect();
    var a = computeArea(r);
    var p = computePerimeter(r);
}
)";
    CompilerInput input{.source = source, .path = "iface.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    if (!result.succeeded())
        dumpDiags(result);
    EXPECT_TRUE(result.succeeded());

    // Both dispatch functions should use itable lookup
    EXPECT_TRUE(hasCall(result.module, "computeArea", "rt_get_interface_impl"));
    EXPECT_TRUE(hasCall(result.module, "computePerimeter", "rt_get_interface_impl"));
}

TEST(ZiaIfaceDispatch, DefaultInterfaceMethodFillsMissingSlot) {
    SourceManager sm;
    const std::string source = R"(
module Test;

interface INamed {
    func name() -> String;
    func label() -> String {
        return "name: " + self.name();
    }
}

class Thing implements INamed {
    expose func name() -> String {
        return "thing";
    }
}

func render(item: INamed) -> String {
    return item.label();
}

func start() {
    var t = new Thing();
    var item: INamed = t;
    Zanna.Terminal.Say(render(item));
}
)";
    CompilerInput input{.source = source, .path = "iface_default.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    if (!result.succeeded())
        dumpDiags(result);
    EXPECT_TRUE(result.succeeded());

    EXPECT_TRUE(hasFunction(result.module, "INamed.label"));
    EXPECT_TRUE(hasCall(result.module, "INamed.label", "rt_get_interface_impl"));
    EXPECT_TRUE(hasCall(result.module, "__zia_iface_init", "rt_bind_interface"));
}

/// @brief A large application's class registry is emitted through bounded helpers instead of
///        one native-backend-hostile initializer with tens of thousands of virtual registers.
TEST(ZiaIfaceDispatch, LargeClassRegistryUsesBoundedInitializationHelpers) {
    SourceManager sm;
    constexpr int kClassCount = 700;
    std::string source = R"(
module LargeRegistry;

interface IMarker {
    func marker() -> Integer;
}
)";
    source.reserve(160000);
    for (int i = 0; i < kClassCount; ++i) {
        const std::string suffix = std::to_string(i);
        source += "class RegistryType" + suffix + " {\n";
        source += "    expose func first() -> Integer { return " + suffix + "; }\n";
        source += "    expose func second() -> Integer { return " + suffix + "; }\n";
        source += "}\n";
    }
    source += "func start() {}\n";

    CompilerInput input{.source = source, .path = "large_iface_registry.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    if (!result.succeeded())
        dumpDiags(result);
    ASSERT_TRUE(result.succeeded());

    size_t helperCount = 0;
    size_t registrationCount = 0;
    bool initializerCallsHelper = false;
    for (const auto &fn : result.module.functions) {
        const bool isHelper = fn.name.rfind("__zia_class_init_", 0) == 0;
        if (isHelper)
            ++helperCount;
        for (const auto &block : fn.blocks) {
            for (const auto &instr : block.instructions) {
                if (instr.op != il::core::Opcode::Call)
                    continue;
                if (isHelper && instr.callee == "rt_register_class_with_base_rs")
                    ++registrationCount;
                if (fn.name == "__zia_iface_init" &&
                    instr.callee.rfind("__zia_class_init_", 0) == 0) {
                    initializerCallsHelper = true;
                }
            }
        }
    }

    EXPECT_TRUE(helperCount > 1);
    EXPECT_TRUE(initializerCallsHelper);
    EXPECT_EQ(static_cast<size_t>(kClassCount), registrationCount);
}

/// @brief No interfaces defined — no __zia_iface_init emitted.
TEST(ZiaIfaceDispatch, NoInterfacesNoInit) {
    SourceManager sm;
    const std::string source = R"(
module Test;

class Foo {
    expose Integer x;
}

func start() {    var f = new Foo();
}
)";
    CompilerInput input{.source = source, .path = "iface.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    if (!result.succeeded())
        dumpDiags(result);
    EXPECT_TRUE(result.succeeded());

    // No interfaces, so no init function
    EXPECT_FALSE(hasFunction(result.module, "__zia_iface_init"));
}

} // namespace

int main() {
    return zanna_test::run_all_tests();
}
