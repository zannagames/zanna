//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/zia/test_zia_optional_narrowing.cpp
// Purpose: Test optional type narrowing after null checks, alone and combined with &&, || and !.
// Key invariants:
//   - After a null check, the checked value narrows in the branch where it is non-null.
//   - A combination narrows exactly the values its outcome proves non-null.
// Ownership/Lifetime:
//   - Each test owns its source manager and compilation result.
// Links: src/frontends/zia/Sema_Scope.cpp, docs/languages/zia-reference.md
//
//===----------------------------------------------------------------------===//

#include "frontends/zia/Compiler.hpp"
#include "il/core/Function.hpp"
#include "il/core/Module.hpp"
#include "il/verify/Verifier.hpp"
#include "support/source_manager.hpp"
#include "tests/TestHarness.hpp"

#include <string>

using namespace il::frontends::zia;
using namespace il::support;

namespace {

/// @brief Test that type narrowing works after "x != null" check.
TEST(ZiaOptionalNarrowing, NarrowingAfterNotNullCheck) {
    const std::string src = R"(
module Test;

class Person {
    expose String name;

    expose func init(n: String) {        name = n;
    }
}

func start() {    var maybePerson: Person? = new Person("Alice");

    if (maybePerson != null) {
        // Inside this branch, maybePerson should be narrowed to Person
        var name: String = maybePerson.name;
        Zanna.Terminal.Say(name);
    }
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for NarrowingAfterNotNullCheck:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief Test that type narrowing works in else branch after "x == null" check.
TEST(ZiaOptionalNarrowing, NarrowingInElseBranchAfterNullCheck) {
    const std::string src = R"(
module Test;

class Person {
    expose String name;

    expose func init(n: String) {        name = n;
    }
}

func start() {    var maybePerson: Person? = new Person("Bob");

    if (maybePerson == null) {
        Zanna.Terminal.Say("No person");
    } else {
        // Inside else branch, maybePerson should be narrowed to Person
        var name: String = maybePerson.name;
        Zanna.Terminal.Say(name);
    }
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for NarrowingInElseBranchAfterNullCheck:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief Test that type narrowing works with reversed null check (null != x).
TEST(ZiaOptionalNarrowing, NarrowingWithReversedNullCheck) {
    const std::string src = R"(
module Test;

class Person {
    expose String name;

    expose func init(n: String) {        name = n;
    }
}

func start() {    var maybePerson: Person? = new Person("Charlie");

    if (null != maybePerson) {
        // Inside this branch, maybePerson should be narrowed to Person
        var name: String = maybePerson.name;
        Zanna.Terminal.Say(name);
    }
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for NarrowingWithReversedNullCheck:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief Test that narrowed type allows method calls.
TEST(ZiaOptionalNarrowing, NarrowedTypeAllowsMethodCalls) {
    const std::string src = R"(
module Test;

class Person {
    expose String name;

    expose func init(n: String) {        name = n;
    }

    expose func greet() -> String {        return "Hello, " + self.name;
    }
}

func start() {    var maybePerson: Person? = new Person("Eve");

    if (maybePerson != null) {
        // Inside this branch, can call methods on the narrowed type
        var greeting: String = maybePerson.greet();
        Zanna.Terminal.Say(greeting);
    }
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for NarrowedTypeAllowsMethodCalls:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief A non-null initializer should narrow Optional[T] to T until reassignment.
TEST(ZiaOptionalNarrowing, InitializerNarrowsOptionalToInnerType) {
    const std::string src = R"(
module Test;

func start() {
    var x: Integer? = 42;
    Zanna.Terminal.SayInt(x);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for InitializerNarrowsOptionalToInnerType:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief Reassigning null must clear initializer-based narrowing.
TEST(ZiaOptionalNarrowing, NullAssignmentClearsInitializerNarrowing) {
    const std::string src = R"(
module Test;

func start() {
    var x: Integer? = 42;
    x = null;
    Zanna.Terminal.SayInt(x);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    EXPECT_FALSE(result.succeeded());
}

/// @brief Optional chaining should still use the declared Optional[T] surface type.
TEST(ZiaOptionalNarrowing, InitializerNarrowingPreservesOptionalChainingSurface) {
    const std::string src = R"(
module Test;

class Node {
    expose Integer value;

    expose func init(v: Integer) {        value = v;
    }
}

func start() {
    var node: Node? = new Node(5);
    var value = node?.value ?? 0;
    Zanna.Terminal.SayInt(value);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for InitializerNarrowingPreservesOptionalChainingSurface:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief Matching on null should still work after initializer-based narrowing.
TEST(ZiaOptionalNarrowing, InitializerNarrowingPreservesOptionalMatchSurface) {
    const std::string src = R"(
module Test;

func start() {
    var x: Integer? = 42;
    var matched = 0;
    match x {
        null => { matched = -1; }
        _ => { matched = 1; }
    }
    Zanna.Terminal.SayInt(matched);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for InitializerNarrowingPreservesOptionalMatchSurface:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

//=============================================================================
// Force-Unwrap Operator Tests
//=============================================================================

/// @brief Test that force-unwrap converts Optional[Entity] to Entity.
TEST(ZiaForceUnwrap, ForceUnwrapEntity) {
    const std::string src = R"(
module Test;

class Person {
    expose String name;

    expose func init(n: String) {        name = n;
    }
}

func start() {    var maybePerson: Person? = new Person("Alice");
    var person: Person = maybePerson!;
    Zanna.Terminal.Say(person.name);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for ForceUnwrapEntity:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief Test that force-unwrap works in function call arguments.
TEST(ZiaForceUnwrap, ForceUnwrapInCallArg) {
    const std::string src = R"(
module Test;

class Item {
    expose String label;

    expose func init(l: String) {        label = l;
    }
}

func useItem(item: Item) {    Zanna.Terminal.Say(item.label);
}

func start() {    var maybeItem: Item? = new Item("sword");
    useItem(maybeItem!);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for ForceUnwrapInCallArg:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief Initializer-based narrowing should not block force-unwrap on Optional[T].
TEST(ZiaForceUnwrap, ForceUnwrapInitializerNarrowedPrimitiveOptional) {
    const std::string src = R"(
module Test;

func start() {
    var x: Integer? = 42;
    var y: Integer = x!;
    Zanna.Terminal.SayInt(y);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for ForceUnwrapInitializerNarrowedPrimitiveOptional:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief String-like optionals must lower to verifier-clean IL for `!`, `?.`, and `??`.
TEST(ZiaForceUnwrap, StringOptionalLoweringPassesVerifier) {
    const std::string src = R"(
module Test;

class Person {
    expose String name;

    expose func init(n: String) {        name = n;
    }
}

func start() {
    var maybeText: String? = "hello";
    var text = maybeText!;
    Zanna.Terminal.Say(text);

    var maybePerson: Person? = new Person("Ada");
    var name = maybePerson?.name ?? "n/a";
    Zanna.Terminal.Say(name);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for StringOptionalLoweringPassesVerifier:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());
    auto verified = il::verify::Verifier::verify(result.module);
    EXPECT_TRUE(verified.hasValue());
}

/// @brief Test that force-unwrap on non-optional produces an error.
TEST(ZiaForceUnwrap, ForceUnwrapNonOptionalError) {
    const std::string src = R"(
module Test;

func start() {    var x: Integer = 42;
    var y = x!;
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    // Should fail — force-unwrapping a non-optional is an error
    EXPECT_FALSE(result.succeeded());
}

/// @brief Test force-unwrap chains with field access.
TEST(ZiaForceUnwrap, ForceUnwrapThenFieldAccess) {
    const std::string src = R"(
module Test;

class Node {
    expose String value;

    expose func init(v: String) {        value = v;
    }
}

func start() {    var maybeNode: Node? = new Node("hello");
    var val: String = maybeNode!.value;
    Zanna.Terminal.Say(val);
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "test.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for ForceUnwrapThenFieldAccess:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

TEST(ZiaOptionalNarrowing, FieldNullCheckNarrowsFieldAccess) {
    const std::string src = R"(
module Test;

class Person {
    expose String name;

    expose func init(n: String) {        name = n;
    }
}

class Holder {
    expose Person? child;

    expose func init() {        child = new Person("Ada");
    }

    expose func printChild() {
        if self.child != null {
            var childName: String = self.child.name;
            Zanna.Terminal.Say(childName);
        }
    }
}

func start() {
    var holder: Holder = new Holder();
    holder.printChild();
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "field_null_narrow.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for FieldNullCheckNarrowsFieldAccess:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
}

/// @brief Null checks combined with `&&`, `||` and `!` narrow every value they prove non-null.
/// @details `a != null && b != null` narrows both in the then-branch; `a == null || b == null`
///          narrows both in the else-branch and after an early return; a negated disjunction
///          narrows like the conjunction; and a later `&&` operand sees the earlier checks.
TEST(ZiaOptionalNarrowing, LogicalCombinationsOfNullChecksNarrowEachValue) {
    const std::string src = R"(
module Test;

class Row {
    expose Integer v;
    expose func init(x: Integer) { v = x; }
    expose func getValue() -> Integer { return v; }
}

func pick(i: Integer) -> Row? {
    if i > 0 { return new Row(i); }
    return null;
}

func sum(a: Row?, b: Row?) -> Integer {
    if a == null || b == null {
        return -1;
    }
    return a.getValue() + b.getValue();
}

func start() {
    var a = pick(1);
    var b = pick(2);
    if a != null && b != null {
        var copy = a;
        Zanna.Terminal.SayInt(copy.getValue() + b.getValue());
    }
    if !(a == null || b == null) {
        Zanna.Terminal.SayInt(b.getValue());
    }
    if a == null || b == null {
        Zanna.Terminal.Say("missing");
    } else {
        Zanna.Terminal.SayInt(a.getValue() * b.getValue());
    }
    if a != null && a.getValue() > 0 && b != null && b.getValue() > 1 {
        Zanna.Terminal.SayInt(sum(a, b));
    }
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "logical_null_narrow.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for LogicalCombinationsOfNullChecksNarrowEachValue:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    ASSERT_TRUE(result.succeeded());
    auto verified = il::verify::Verifier::verify(result.module);
    EXPECT_TRUE(verified.hasValue());
}

/// @brief A disjunction that holds proves nothing, so neither value narrows in its then-branch.
TEST(ZiaOptionalNarrowing, DisjunctionThenBranchDoesNotNarrow) {
    const std::string src = R"(
module Test;

class Row {
    expose Integer v;
    expose func init(x: Integer) { v = x; }
}

func start() {
    var a: Row? = null;
    var b: Row? = new Row(2);
    if a != null || b != null {
        Zanna.Terminal.SayInt(a.v);
    }
}
)";

    SourceManager sm;
    CompilerInput input{.source = src, .path = "disjunction_no_narrow.zia"};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    EXPECT_FALSE(result.succeeded());
}

} // namespace

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
