//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/zia/test_zia_binds.cpp
// Purpose: Verify file and namespace bind parsing, resolution, and diagnostics.
// Key invariants:
//   - Failed file imports make strict resolution fail after reporting diagnostics.
//   - Imported declarations retain their source identity and module qualification.
// Ownership/Lifetime:
//   - Tests own temporary source trees and compiler inputs.
//   - Compiler results own all emitted diagnostics and IL modules.
// Links: src/frontends/zia/ImportResolver.cpp, docs/languages/zia-reference.md
//
//===----------------------------------------------------------------------===//
//
// Unit tests for Zia bind resolution.
//
//===----------------------------------------------------------------------===//

#include "frontends/zia/Compiler.hpp"
#include "frontends/zia/ImportResolver.hpp"
#include "support/source_manager.hpp"
#include "tests/TestHarness.hpp"

#include "tests/common/PosixCompat.h"
#include <filesystem>
#include <fstream>
#include <string>

using namespace il::frontends::zia;
using namespace il::support;

namespace {

namespace fs = std::filesystem;

fs::path writeFile(const fs::path &dir, const std::string &name, const std::string &contents) {
    fs::create_directories(dir);
    fs::path path = dir / name;
    std::ofstream out(path);
    out << contents;
    out.close();
    return path;
}

TEST(ZiaBinds, BindStringLiteralWithExtension) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "bind_ok";

    const fs::path libPath = writeFile(dir,
                                       "lib.zia",
                                       R"(
module Lib;

func greet() {    Zanna.Terminal.Say("hi");
}
)");

    const std::string mainSource = R"(
module Main;
bind "lib.zia";

func start() {    greet();
}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();

    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for BindStringLiteralWithExtension:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    bool hasMain = false;
    bool hasGreet = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "main")
            hasMain = true;
        if (fn.name == "greet")
            hasGreet = true;
    }
    EXPECT_TRUE(hasMain);
    EXPECT_TRUE(hasGreet);

    (void)libPath;
}

TEST(ZiaBinds, MissingBindReportsAtBindSite) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "missing_bind";

    const std::string mainSource = R"(
module Main;
bind "missing.zia";

func start() {}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();

    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    EXPECT_FALSE(result.succeeded());

    bool foundError = false;
    for (const auto &d : result.diagnostics.diagnostics()) {
        if (d.message.find("Failed to open imported file") == std::string::npos)
            continue;
        foundError = true;
        EXPECT_EQ(d.code, "V1000");
        EXPECT_EQ(d.loc.file_id, result.fileId);
    }
    EXPECT_TRUE(foundError);
}

TEST(ZiaBinds, StrictResolverReturnsFalseForMissingImport) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path rootPath = tempRoot / "strict_missing" / "main.zia";

    DiagnosticEngine diag;
    SourceManager sm;
    const uint32_t rootId = sm.addFile(rootPath.string());
    ModuleDecl module(SourceLoc{rootId, 1, 1}, "Main");
    module.binds.emplace_back(SourceLoc{rootId, 2, 1}, "does-not-exist.zia");

    ImportResolver resolver(diag, sm);
    EXPECT_FALSE(resolver.resolve(module, rootPath.string()));
    EXPECT_GT(diag.errorCount(), 0u);
}

TEST(ZiaBinds, SupportsModularGraphsBeyondPrevious256FileLimit) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "wide_import_graph";

    std::string mainSource = "module Main;\n";
    for (int i = 0; i < 300; ++i) {
        const std::string moduleName = "WideImport" + std::to_string(i);
        const std::string fileName = "wide_import_" + std::to_string(i) + ".zia";
        writeFile(dir, fileName, "module " + moduleName + ";\n");
        mainSource += "bind \"" + fileName + "\";\n";
    }
    mainSource += "func start() {}\n";

    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();
    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    EXPECT_TRUE(result.succeeded());
}

TEST(ZiaBinds, LegacyAliasFirstNamespaceBindWorks) {
    const std::string source = R"(
module Main;
bind IO = Zanna.Terminal;

func start() {
    IO.Say("hi");
}
)";

    SourceManager sm;
    CompilerInput input{.source = source, .path = "bind_alias_first_runtime.zia"};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for LegacyAliasFirstNamespaceBindWorks:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    bool hasMain = false;
    bool hasSay = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name != "main")
            continue;
        hasMain = true;
        for (const auto &block : fn.blocks) {
            for (const auto &instr : block.instructions) {
                if (instr.op == il::core::Opcode::Call && instr.callee == "Zanna.Terminal.Say")
                    hasSay = true;
            }
        }
    }
    EXPECT_TRUE(hasMain);
    EXPECT_TRUE(hasSay);
}

TEST(ZiaBinds, LegacyAliasFirstFileBindWorks) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "bind_alias_first_file";

    writeFile(dir,
              "utils.zia",
              R"(
module Utils;

expose func greet() {
    Zanna.Terminal.Say("hi");
}
)");

    const std::string mainSource = R"(
module Main;
bind U = "./utils";

func start() {
    U.greet();
}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();

    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for LegacyAliasFirstFileBindWorks:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    bool hasMain = false;
    bool hasGreet = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "main")
            hasMain = true;
        if (fn.name == "greet")
            hasGreet = true;
    }
    EXPECT_TRUE(hasMain);
    EXPECT_TRUE(hasGreet);
}

TEST(ZiaBinds, RuntimeCompatibilityAliasesDoNotPolluteBroadImports) {
    const std::string source = R"(
module Main;

bind Zanna.Graphics;
bind Zanna.Game;
bind Zanna.Text;
bind Zanna.Localization;
bind Zanna.GUI;
bind Zanna.System;
bind Zanna.IO;
bind Zanna.Workspace;
bind Zanna.Game.UI;
bind Zanna.Game3D;
bind Zanna.System.Process;
bind Zanna.Zia.SemanticJob;
bind Zanna.Zia.ProjectIndex;

func start() {
}
)";

    SourceManager sm;
    CompilerInput input{.source = source, .path = "runtime_alias_import_conflicts.zia"};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    bool hasImportConflict = false;
    for (const auto &d : result.diagnostics.diagnostics()) {
        if (d.message.find("conflicts with existing import") != std::string::npos)
            hasImportConflict = true;
    }
    if (!result.succeeded() || hasImportConflict) {
        std::cerr << "Diagnostics for RuntimeCompatibilityAliasesDoNotPolluteBroadImports:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }

    EXPECT_TRUE(result.succeeded());
    EXPECT_FALSE(hasImportConflict);
}

TEST(ZiaBinds, DuplicateClassNamesAreModuleScoped) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "module_scoped_classes";

    writeFile(dir,
              "alpha.zia",
              R"(
module Alpha;

expose class WishDup {
    expose Integer value;

    expose func init(v: Integer) {
        value = v;
    }

    expose func score() -> Integer {
        return value;
    }
}
)");

    writeFile(dir,
              "beta.zia",
              R"(
module Beta;

expose class WishDup {
    expose Integer value;

    expose func init(v: Integer) {
        value = v;
    }

    expose func score() -> Integer {
        return value * 2;
    }
}
)");

    const std::string mainSource = R"(
module Main;
bind "./alpha" as A;
bind "./beta";

func start() {
    var a: A.WishDup = new A.WishDup(7);
    var b: Beta.WishDup = new Beta.WishDup(11);
    Zanna.Terminal.SayInt(a.score() + b.score());
}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();

    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for DuplicateClassNamesAreModuleScoped:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    bool hasAlphaInit = false;
    bool hasBetaInit = false;
    bool hasAlphaScore = false;
    bool hasBetaScore = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "Alpha.WishDup.init")
            hasAlphaInit = true;
        if (fn.name == "Beta.WishDup.init")
            hasBetaInit = true;
        if (fn.name == "Alpha.WishDup.score")
            hasAlphaScore = true;
        if (fn.name == "Beta.WishDup.score")
            hasBetaScore = true;
    }
    EXPECT_TRUE(hasAlphaInit);
    EXPECT_TRUE(hasBetaInit);
    EXPECT_TRUE(hasAlphaScore);
    EXPECT_TRUE(hasBetaScore);
}

TEST(ZiaBinds, DuplicateFunctionsAndGlobalsAreModuleScoped) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "module_scoped_functions_globals";

    writeFile(dir,
              "alpha.zia",
              R"(
module Alpha;

expose final VALUE = 10;

expose func make() -> Integer {
    return VALUE;
}
)");

    writeFile(dir,
              "beta.zia",
              R"(
module Beta;

expose final VALUE = 20;

expose func make() -> Integer {
    return VALUE;
}
)");

    const std::string mainSource = R"(
module Main;
bind "./alpha" as A;
bind "./beta";

func start() {
    Zanna.Terminal.SayInt(A.make() + Beta.make() + A.VALUE + Beta.VALUE);
}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();

    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for DuplicateFunctionsAndGlobalsAreModuleScoped:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    bool hasAlphaMake = false;
    bool hasBetaMake = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "Alpha.make")
            hasAlphaMake = true;
        if (fn.name == "Beta.make")
            hasBetaMake = true;
    }
    EXPECT_TRUE(hasAlphaMake);
    EXPECT_TRUE(hasBetaMake);
}

TEST(ZiaBinds, QualifiedExtendsImplementsAndStructLiteralCompile) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "qualified_type_positions";

    writeFile(dir,
              "base.zia",
              R"(
module Base;

expose interface Named {
    func name() -> String;
}

expose class Parent {
    expose func base() -> Integer {
        return 3;
    }
}

expose struct Point {
    expose Integer x;
    expose Integer y;
}
)");

    const std::string mainSource = R"(
module Main;
bind "./base";

class Child extends Base.Parent implements Base.Named {
    expose func name() -> String {
        return "child";
    }
}

func start() {
    var child = new Child();
    var point: Base.Point = Base.Point { x = 4, y = 5 };
    Zanna.Terminal.SayInt(child.base() + point.x + point.y);
}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();

    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for QualifiedExtendsImplementsAndStructLiteralCompile:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());
}

TEST(ZiaBinds, CircularBindAllowed) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "cycle";

    const std::string aSource = R"(
module A;
bind "b.zia";

func a() {}

func start() {    a();
    b();
}
)";
    const fs::path aPath = writeFile(dir, "a.zia", aSource);
    const std::string aPathStr = aPath.string();

    const std::string bSource = R"(
module B;
bind "a.zia";

func b() {}
)";
    writeFile(dir, "b.zia", bSource);

    SourceManager sm;
    CompilerInput input{.source = aSource, .path = aPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for CircularBindAllowed:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    bool hasMain = false;
    bool hasA = false;
    bool hasB = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "main")
            hasMain = true;
        if (fn.name == "a")
            hasA = true;
        if (fn.name == "b")
            hasB = true;
    }
    EXPECT_TRUE(hasMain);
    EXPECT_TRUE(hasA);
    EXPECT_TRUE(hasB);

    (void)aPath;
}

TEST(ZiaBinds, CircularBindCrossReference) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "cycle_cross";

    // File A defines class Foo and uses class Bar from B
    const std::string aSource = R"(
module A;
bind "b.zia";

class Foo {
    expose Integer x;
    expose func init(val: Integer) { x = val; }}

func useFoo() -> Integer {    var f: Foo = new Foo(10);
    return f.x;
}

func start() {    var a: Integer = useFoo();
    var b: Integer = useBar();
    Zanna.Terminal.SayInt(a);
    Zanna.Terminal.SayInt(b);
}
)";
    const fs::path aPath = writeFile(dir, "a.zia", aSource);
    const std::string aPathStr = aPath.string();

    // File B defines class Bar and uses class Foo from A
    const std::string bSource = R"(
module B;
bind "a.zia";

class Bar {
    expose Integer y;
    expose func init(val: Integer) { y = val; }}

func useBar() -> Integer {    var b: Bar = new Bar(20);
    return b.y;
}
)";
    writeFile(dir, "b.zia", bSource);

    SourceManager sm;
    CompilerInput input{.source = aSource, .path = aPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for CircularBindCrossReference:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    // Verify all four key symbols are present
    bool hasFooInit = false;
    bool hasBarInit = false;
    bool hasUseFoo = false;
    bool hasUseBar = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "Foo.init")
            hasFooInit = true;
        if (fn.name == "Bar.init")
            hasBarInit = true;
        if (fn.name == "useFoo")
            hasUseFoo = true;
        if (fn.name == "useBar")
            hasUseBar = true;
    }
    EXPECT_TRUE(hasFooInit);
    EXPECT_TRUE(hasBarInit);
    EXPECT_TRUE(hasUseFoo);
    EXPECT_TRUE(hasUseBar);

    (void)aPath;
}

TEST(ZiaBinds, CircularBindSelfImport) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "self_import";

    // File binds itself - should not infinite loop
    const std::string aSource = R"(
module A;
bind "./a";

func start() {    Zanna.Terminal.Say("self");
}
)";
    const fs::path aPath = writeFile(dir, "a.zia", aSource);
    const std::string aPathStr = aPath.string();

    SourceManager sm;
    CompilerInput input{.source = aSource, .path = aPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for CircularBindSelfImport:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    bool hasMain = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "main")
            hasMain = true;
    }
    EXPECT_TRUE(hasMain);

    (void)aPath;
}

/// @brief Test that transitive binds maintain correct declaration order (Bug #26).
/// When main binds both inner and outer, where outer also binds inner,
/// the entities must be lowered in dependency order (Inner before Outer).
TEST(ZiaBinds, TransitiveBindDeclarationOrder) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "transitive_order";

    // Inner class with a method
    const fs::path innerPath = writeFile(dir, "inner.zia", R"(
module Inner;

class Inner {
    expose Integer myValue;

    expose func init(v: Integer) {        myValue = v;
    }

    expose func getValue() -> Integer {        return myValue;
    }
}
)");

    // Outer class that has Inner field and calls its method
    const fs::path outerPath = writeFile(dir, "outer.zia", R"(
module Outer;

bind "./inner";

class Outer {
    expose Inner inner;

    expose func test() -> Integer {        return inner.getValue();
    }
}
)");

    // Main binds both inner AND outer (outer also binds inner)
    const std::string mainSource = R"(
module Main;

bind "./inner";
bind "./outer";

func start() {    var o: Outer = new Outer();
    o.inner = new Inner(42);
    var result: Integer = o.test();
    Zanna.Terminal.SayInt(result);
}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();

    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);

    if (!result.succeeded()) {
        std::cerr << "Diagnostics for TransitiveBindDeclarationOrder:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());

    // Verify Outer.test calls Inner.getValue directly (not via lambda/closure)
    bool foundOuterTest = false;
    bool foundDirectCall = false;
    for (const auto &fn : result.module.functions) {
        if (fn.name == "Outer.test") {
            foundOuterTest = true;
            for (const auto &block : fn.blocks) {
                for (const auto &instr : block.instructions) {
                    if (instr.op == il::core::Opcode::Call) {
                        // Check if callee is Inner.getValue (direct call)
                        if (instr.callee == "Inner.getValue") {
                            foundDirectCall = true;
                        }
                    }
                }
            }
        }
    }
    EXPECT_TRUE(foundOuterTest);
    EXPECT_TRUE(foundDirectCall);

    (void)innerPath;
    (void)outerPath;
}

/// @brief A malformed bind (an incomplete "bind Zanna.X." captured mid-edit by
/// live diagnostics) must not abort resolution of a valid relative bind beside
/// it, nor fabricate a "<dir>/.zia" import. Regression for the 55-error cascade.
TEST(ZiaBinds, MalformedBindDoesNotCascade) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "malformed_no_cascade";

    writeFile(dir, "cfg.zia", R"(
module cfg;
var WIDTH: Integer = 100;
)");

    const std::string mainSource = R"(
module Main;
bind "./cfg";
bind Zanna.Game3D.
func start() {    Zanna.Terminal.SayInt(cfg.WIDTH);
}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();

    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};

    auto result = compile(input, opts, sm);

    int v1000 = 0;
    int errorCount = 0;
    bool cfgUnresolved = false;
    for (const auto &d : result.diagnostics.diagnostics()) {
        if (d.severity != Severity::Error)
            continue;
        errorCount++;
        if (d.message.find("Failed to open imported file") != std::string::npos)
            v1000++;
        // The valid "./cfg" bind must still resolve (no unresolved-cfg cascade).
        if (d.message.find("cfg") != std::string::npos)
            cfgUnresolved = true;
    }
    if (v1000 != 0 || cfgUnresolved || errorCount > 2) {
        std::cerr << "Diagnostics for MalformedBindDoesNotCascade (errors=" << errorCount << "):\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.code << " " << d.message << "\n";
        }
    }
    EXPECT_EQ(v1000, 0);         // no fabricated "<dir>/.zia" import
    EXPECT_FALSE(cfgUnresolved); // the valid relative bind still resolved
    EXPECT_LE(errorCount, 2);    // only the local incomplete-bind error, not a cascade
}

// ZB-27: an inherited field must shadow a same-named exported global of a
// bound module, exactly as a declared field does. The base class lives in one
// module, a bound helper module exports `var vec_: Any`, and the derived class
// (in a third module) reads and assigns `vec_` unqualified.
TEST(ZiaBinds, InheritedFieldShadowsBoundModuleGlobal) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "inherited_field_shadow";
    writeFile(dir,
              "other.zia",
              R"(
module other;
var vec_: Any = null;
var thing_: Any = null;
class Thing {
    expose Integer v;
    expose func init() { v = 5; }
    expose func twice() -> Integer { return v * 2; }
}
)");
    writeFile(dir,
              "base.zia",
              R"(
module base;
bind Zanna.Math as Math;
bind "./other";
class Base {
    expose Math.Vec3 vec_;
    expose other.Thing thing_;
    expose func init() { vec_ = Math.Vec3.New(1.0, 2.0, 3.0); thing_ = new other.Thing(); }
}
)");
    writeFile(dir,
              "mid.zia",
              R"(
module mid;
bind Zanna.Math as Math;
bind "./base";
bind "./other";
class Mid extends base.Base {
    expose func midVal() -> Float { return vec_.X + (thing_.twice() + 0.0); }
    expose func reset() { vec_ = Math.Vec3.New(7.0, 8.0, 9.0); }
}
)");
    const std::string mainSource = R"(
module main;
bind Zanna.Math as Math;
bind "./base";
bind "./mid";
bind "./other";
class Top extends mid.Mid {
    expose func init() { super.init(); }
    expose func topVal() -> Float { self.reset(); return vec_.Z + (thing_.twice() + 0.0); }
}
func start() {
    var t = new Top();
    Zanna.Terminal.Say("v=" + toString(t.midVal()) + " top=" + toString(t.topVal()));
}
)";
    const fs::path mainPath = writeFile(dir, "main.zia", mainSource);
    const std::string mainPathStr = mainPath.string();
    SourceManager sm;
    CompilerInput input{.source = mainSource, .path = mainPathStr};
    CompilerOptions opts{};
    auto result = compile(input, opts, sm);
    if (!result.succeeded()) {
        std::cerr << "Diagnostics for InheritedFieldShadowsBoundModuleGlobal:\n";
        for (const auto &d : result.diagnostics.diagnostics()) {
            std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                      << d.code << " " << d.message << "\n";
        }
    }
    EXPECT_TRUE(result.succeeded());
    fs::remove_all(tempRoot);
}

/// @brief A name exported by two bound modules is reported as ambiguous, and qualifying it works.
TEST(ZiaBinds, NameExportedByTwoBoundModulesIsAmbiguous) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "ambiguous_export";
    writeFile(
        dir, "moda.zia", "module ModA;\n\nfunc helper(x: Integer) -> Integer { return x + 1; }\n");
    writeFile(
        dir, "modb.zia", "module ModB;\n\nfunc helper(x: Integer) -> Integer { return x + 2; }\n");

    const std::string ambiguousSource = R"(
module Main;
bind "./moda";
bind "./modb";

func start() {
    Zanna.Terminal.SayInt(helper(1));
}
)";
    const fs::path ambiguousPath = writeFile(dir, "ambiguous.zia", ambiguousSource);
    SourceManager sm;
    const std::string ambiguousPathStr = ambiguousPath.string();
    CompilerInput ambiguousInput{.source = ambiguousSource, .path = ambiguousPathStr};
    CompilerOptions opts{};
    auto ambiguous = compile(ambiguousInput, opts, sm);
    EXPECT_FALSE(ambiguous.succeeded());
    bool sawAmbiguity = false;
    for (const auto &d : ambiguous.diagnostics.diagnostics()) {
        if (d.message.find("Ambiguous identifier 'helper': exported by 'ModA' and 'ModB'") !=
            std::string::npos)
            sawAmbiguity = true;
        EXPECT_EQ(d.message.find("Undefined identifier"), std::string::npos);
    }
    EXPECT_TRUE(sawAmbiguity);

    const std::string qualifiedSource = R"(
module Main;
bind "./moda";
bind "./modb";

func start() {
    Zanna.Terminal.SayInt(ModA.helper(1) + ModB.helper(1));
}
)";
    const fs::path qualifiedPath = writeFile(dir, "qualified.zia", qualifiedSource);
    SourceManager sm2;
    const std::string qualifiedPathStr = qualifiedPath.string();
    CompilerInput qualifiedInput{.source = qualifiedSource, .path = qualifiedPathStr};
    auto qualified = compile(qualifiedInput, opts, sm2);
    EXPECT_TRUE(qualified.succeeded());

    fs::remove_all(tempRoot);
}

/// @brief A bind is not inherited (ADR 0376): a file sees only the modules it binds
///        itself, never those bound by the files it binds. Every way of naming a
///        declaration of an unbound module reports the module to bind, once per file
///        and without follow-on errors, and binding it directly makes the same code
///        compile. (Legacy Baseball ledger ZB-54.)
TEST(ZiaBinds, BindIsNotInheritedFromBoundFiles) {
    const fs::path tempRoot = fs::temp_directory_path() / "zia_bind_tests" /
                              std::to_string(static_cast<unsigned long long>(::getpid()));
    const fs::path dir = tempRoot / "bind_not_inherited";
    writeFile(dir, "a.zia", R"(
module a;

enum Color { Red, Green }

class Page {
    expose Integer n;
    expose func init() { n = 1; }
}

interface Greeter {
    func greet() -> String;
}

class Base {
    expose func hi() -> String { return "hi"; }
}

class Box[T] {
    expose T v;
    expose func init(x: T) { v = x; }
}

struct Pt {
    expose Integer x;
}

type Alias = Integer;

final LIMIT = 5;

func helper() -> Integer { return 7; }
)");
    writeFile(dir, "b.zia", R"(
module b;

bind "./a";

func viaB() -> Integer { return helper(); }
)");

    struct Case {
        const char *name;
        const char *decls;
    };

    const Case cases[] = {
        {"bare_function", "func use() -> Integer { return helper(); }"},
        {"qualified_function", "func use() -> Integer { return a.helper(); }"},
        {"bare_constant", "func use() -> Integer { return LIMIT; }"},
        {"type_annotation", "func use(p: Page) -> Integer { return p.n; }"},
        {"new_expression", "func use() -> Integer { var p = new Page(); return p.n; }"},
        {"enum_variant", "func use() -> Integer { var c = Color.Red; return 1; }"},
        {"match_on_enum",
         "func use(k: Color) -> Integer { return match k { Color.Red => 1, Color.Green => 2 }; }"},
        {"implements",
         "class G implements Greeter { expose func greet() -> String { return \"g\"; } }"},
        {"extends",
         "class D extends Base { }\nfunc use() -> String { var d = new D(); return d.hi(); }"},
        {"generic_class", "func use() -> Integer { var bx = new Box[Integer](2); return bx.v; }"},
        {"struct_literal", "func use() -> Integer { var q = Pt { x: 1 }; return q.x; }"},
        {"type_alias", "func use(v: Alias) -> Integer { return v; }"},
        {"type_test", "func use(o: Any) -> Boolean { return o is Page; }"},
        {"element_type", "func use() -> Integer { var xs: List[Page] = []; return xs.count(); }"},
        {"many_uses",
         "func use(p: Page, q: Page?) -> Integer { var m: Map[String, Page] = new Map[String, "
         "Page](); return helper() + LIMIT; }"},
    };

    for (const auto &c : cases) {
        for (bool bindA : {false, true}) {
            std::string source = "module Main;\n\nbind \"./b\";\n";
            if (bindA)
                source += "bind \"./a\";\n";
            source += "\n";
            source += c.decls;
            source += "\n\nfunc start() {\n    Zanna.Terminal.Say(\"x\");\n}\n";
            const fs::path mainPath = writeFile(
                dir, std::string(c.name) + (bindA ? "_bound.zia" : "_unbound.zia"), source);
            const std::string mainPathStr = mainPath.string();
            SourceManager sm;
            CompilerInput input{.source = source, .path = mainPathStr};
            CompilerOptions opts{};
            auto result = compile(input, opts, sm);

            int errors = 0;
            int unbound = 0;
            for (const auto &d : result.diagnostics.diagnostics()) {
                if (d.severity != Severity::Error)
                    continue;
                ++errors;
                if (d.code == "V-ZIA-UNBOUND-MODULE" &&
                    (d.message.find("module 'a', which this file does not bind") !=
                         std::string::npos ||
                     d.message.find("Module 'a' is not bound in this file") != std::string::npos))
                    ++unbound;
            }
            const bool ok = bindA ? (result.succeeded() && errors == 0)
                                  : (!result.succeeded() && errors == 1 && unbound == 1);
            if (!ok) {
                std::cerr << "BindIsNotInheritedFromBoundFiles/" << c.name
                          << (bindA ? " (bound)" : " (unbound)") << ":\n";
                for (const auto &d : result.diagnostics.diagnostics()) {
                    std::cerr << "  [" << (d.severity == Severity::Error ? "ERROR" : "WARN") << "] "
                              << d.code << " " << d.message << "\n";
                }
            }
            EXPECT_TRUE(ok);
        }
    }

    fs::remove_all(tempRoot);
}

} // namespace

int main() {
    return zanna_test::run_all_tests();
}
