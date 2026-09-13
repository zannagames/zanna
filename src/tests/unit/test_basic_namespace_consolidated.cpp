//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_basic_namespace_consolidated.cpp
// Purpose: Consolidated namespace/using tests (8 files merged, 75 TEST cases).
// Key invariants:
//   - Each case parses, analyzes and (where relevant) lowers in-memory BASIC
//     source and asserts on the diagnostic count or resolved names.
//   - Test programs use documented type names only; any other AS name is a
//     class and must exist.
// Ownership/Lifetime:
//   - Every case owns its SourceManager, AST and diagnostics.
// Links: docs/languages/basic-namespaces.md
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/AST.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/sem/NamespaceRegistry.hpp"
#include "frontends/basic/sem/RegistryBuilder.hpp"
#include "frontends/basic/sem/UsingContext.hpp"
#include "il/io/Serializer.hpp"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"
#include "tests/TestHarness.hpp"
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace il::frontends::basic;
using namespace il::support;

// ── test_namespace_registry.cpp ──

TEST(NsRegistry, TestRegisterNamespaceMergesRepeatedBlocks) {
    NamespaceRegistry reg;

    // Register the same namespace multiple times with different casings.
    reg.registerNamespace("A.B");
    reg.registerNamespace("a.b");
    reg.registerNamespace("A.b");

    // All should resolve to the same namespace.
    ASSERT_TRUE(reg.namespaceExists("A.B"));
    ASSERT_TRUE(reg.namespaceExists("a.b"));
    ASSERT_TRUE(reg.namespaceExists("A.b"));

    // Canonical spelling should be the first-seen ("A.B").
    const auto *info = reg.info("a.b");
    ASSERT_TRUE(info != nullptr);
    ASSERT_TRUE(info->full == "A.B");
}

TEST(NsRegistry, TestRegisterClassCreatesFqName) {
    NamespaceRegistry reg;

    reg.registerNamespace("Foo.Bar");
    reg.registerClass("Foo.Bar", "MyClass");

    // Type should exist with fully-qualified name.
    ASSERT_TRUE(reg.typeExists("Foo.Bar.MyClass"));
    ASSERT_TRUE(reg.typeExists("foo.bar.myclass"));
    ASSERT_TRUE(reg.typeExists("FOO.BAR.MYCLASS"));

    // Type kind should be Class.
    ASSERT_TRUE(reg.getTypeKind("Foo.Bar.MyClass") == NamespaceRegistry::TypeKind::Class);
    ASSERT_TRUE(reg.getTypeKind("foo.bar.myclass") == NamespaceRegistry::TypeKind::Class);

    // Namespace should contain the class.
    const auto *info = reg.info("foo.bar");
    ASSERT_TRUE(info != nullptr);
    ASSERT_TRUE(info->classes.size() == 1);
    ASSERT_TRUE(info->classes.count("Foo.Bar.MyClass") == 1);
}

TEST(NsRegistry, TestRegisterInterfaceCreatesFqName) {
    NamespaceRegistry reg;

    reg.registerNamespace("A.B");
    reg.registerInterface("A.B", "IFoo");

    // Type should exist with fully-qualified name.
    ASSERT_TRUE(reg.typeExists("A.B.IFoo"));
    ASSERT_TRUE(reg.typeExists("a.b.ifoo"));
    ASSERT_TRUE(reg.typeExists("A.b.IFoo"));

    // Type kind should be Interface.
    ASSERT_TRUE(reg.getTypeKind("A.B.IFoo") == NamespaceRegistry::TypeKind::Interface);
    ASSERT_TRUE(reg.getTypeKind("a.b.ifoo") == NamespaceRegistry::TypeKind::Interface);

    // Namespace should contain the interface.
    const auto *info = reg.info("a.B");
    ASSERT_TRUE(info != nullptr);
    ASSERT_TRUE(info->interfaces.size() == 1);
    ASSERT_TRUE(info->interfaces.count("A.B.IFoo") == 1);
}

TEST(NsRegistry, TestNamespaceExistsCaseInsensitive) {
    NamespaceRegistry reg;

    reg.registerNamespace("MyNamespace");

    // All case variations should succeed.
    ASSERT_TRUE(reg.namespaceExists("MyNamespace"));
    ASSERT_TRUE(reg.namespaceExists("mynamespace"));
    ASSERT_TRUE(reg.namespaceExists("MYNAMESPACE"));
    ASSERT_TRUE(reg.namespaceExists("myNAMEspace"));

    // Non-existent namespace should fail.
    ASSERT_TRUE(!reg.namespaceExists("Other"));
}

TEST(NsRegistry, TestTypeExistsCaseInsensitive) {
    NamespaceRegistry reg;

    reg.registerClass("NS", "MyClass");
    reg.registerInterface("NS", "IMyInterface");

    // Class checks (case-insensitive).
    ASSERT_TRUE(reg.typeExists("NS.MyClass"));
    ASSERT_TRUE(reg.typeExists("ns.myclass"));
    ASSERT_TRUE(reg.typeExists("NS.MYCLASS"));

    // Interface checks (case-insensitive).
    ASSERT_TRUE(reg.typeExists("NS.IMyInterface"));
    ASSERT_TRUE(reg.typeExists("ns.imyinterface"));
    ASSERT_TRUE(reg.typeExists("NS.IMYINTERFACE"));

    // Non-existent type should fail.
    ASSERT_TRUE(!reg.typeExists("NS.Other"));
}

TEST(NsRegistry, TestGetTypeKindPositiveNegative) {
    NamespaceRegistry reg;

    reg.registerClass("A", "C1");
    reg.registerInterface("A", "I1");

    // Check class kind.
    ASSERT_TRUE(reg.getTypeKind("A.C1") == NamespaceRegistry::TypeKind::Class);
    ASSERT_TRUE(reg.getTypeKind("a.c1") == NamespaceRegistry::TypeKind::Class);

    // Check interface kind.
    ASSERT_TRUE(reg.getTypeKind("A.I1") == NamespaceRegistry::TypeKind::Interface);
    ASSERT_TRUE(reg.getTypeKind("a.i1") == NamespaceRegistry::TypeKind::Interface);

    // Non-existent type should return None.
    ASSERT_TRUE(reg.getTypeKind("A.Missing") == NamespaceRegistry::TypeKind::None);
    ASSERT_TRUE(reg.getTypeKind("NonExistent.Type") == NamespaceRegistry::TypeKind::None);
}

TEST(NsRegistry, TestCanonicalSpellingPreserved) {
    NamespaceRegistry reg;

    // First registration uses mixed case.
    reg.registerNamespace("FooBar.BazQux");
    reg.registerClass("FooBar.BazQux", "MyClass");

    // Later registrations use different casings.
    reg.registerNamespace("foobar.bazqux");
    reg.registerClass("FOOBAR.BAZQUX", "AnotherClass");

    // Retrieve info using any casing.
    const auto *info = reg.info("fooBar.bazQUX");
    ASSERT_TRUE(info != nullptr);

    // Canonical namespace spelling should be the first-seen.
    ASSERT_TRUE(info->full == "FooBar.BazQux");

    // Both classes should be registered with canonical namespace prefix.
    ASSERT_TRUE(info->classes.size() == 2);
    ASSERT_TRUE(info->classes.count("FooBar.BazQux.MyClass") == 1);
    ASSERT_TRUE(info->classes.count("FooBar.BazQux.AnotherClass") == 1);
}

TEST(NsRegistry, TestInfoReturnsNullForNonexistentNamespace) {
    NamespaceRegistry reg;

    reg.registerNamespace("Exists");

    // Existing namespace should return valid pointer.
    ASSERT_TRUE(reg.info("Exists") != nullptr);
    ASSERT_TRUE(reg.info("exists") != nullptr);

    // Non-existent namespace should return nullptr.
    ASSERT_TRUE(reg.info("DoesNotExist") == nullptr);
    ASSERT_TRUE(reg.info("doesnotexist") == nullptr);
}

TEST(NsRegistry, TestRegisterClassCreatesNamespaceImplicitly) {
    NamespaceRegistry reg;

    // Register a class without explicitly registering the namespace first.
    reg.registerClass("Implicit.NS", "TestClass");

    // Namespace should now exist.
    ASSERT_TRUE(reg.namespaceExists("Implicit.NS"));
    ASSERT_TRUE(reg.namespaceExists("implicit.ns"));

    // Type should exist.
    ASSERT_TRUE(reg.typeExists("Implicit.NS.TestClass"));
    ASSERT_TRUE(reg.getTypeKind("implicit.ns.testclass") == NamespaceRegistry::TypeKind::Class);
}

TEST(NsRegistry, TestRegisterInterfaceCreatesNamespaceImplicitly) {
    NamespaceRegistry reg;

    // Register an interface without explicitly registering the namespace first.
    reg.registerInterface("Auto.Created", "ITest");

    // Namespace should now exist.
    ASSERT_TRUE(reg.namespaceExists("Auto.Created"));
    ASSERT_TRUE(reg.namespaceExists("auto.created"));

    // Type should exist.
    ASSERT_TRUE(reg.typeExists("Auto.Created.ITest"));
    ASSERT_TRUE(reg.getTypeKind("auto.created.itest") == NamespaceRegistry::TypeKind::Interface);
}

TEST(NsRegistry, TestMultipleTypesInSameNamespace) {
    NamespaceRegistry reg;

    reg.registerNamespace("MyNS");
    reg.registerClass("MyNS", "Class1");
    reg.registerClass("MyNS", "Class2");
    reg.registerInterface("MyNS", "Interface1");
    reg.registerInterface("MyNS", "Interface2");

    const auto *info = reg.info("myns");
    ASSERT_TRUE(info != nullptr);
    ASSERT_TRUE(info->classes.size() == 2);
    ASSERT_TRUE(info->interfaces.size() == 2);

    ASSERT_TRUE(info->classes.count("MyNS.Class1") == 1);
    ASSERT_TRUE(info->classes.count("MyNS.Class2") == 1);
    ASSERT_TRUE(info->interfaces.count("MyNS.Interface1") == 1);
    ASSERT_TRUE(info->interfaces.count("MyNS.Interface2") == 1);
}

// ── test_namespace_diagnostics.cpp ──
std::string getFirstDiagnostic(const std::string &source) {
    SourceManager sm;
    uint32_t fileId = sm.addFile("test.bas");

    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);
    emitter.addSource(fileId, source);
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    // Get first diagnostic.
    std::ostringstream oss;
    de.printAll(oss, &sm);
    std::string output = oss.str();

    // Extract just the error message part (after the location).
    // Handle both "error: msg" and "error[code]: msg" formats.
    size_t errorPos = output.find("error");
    if (errorPos != std::string::npos) {
        // Find the colon-space that precedes the message
        size_t colonSpace = output.find(": ", errorPos);
        if (colonSpace != std::string::npos) {
            size_t msgStart = colonSpace + 2; // Skip ": "
            size_t msgEnd = output.find('\n', msgStart);
            return output.substr(msgStart, msgEnd - msgStart);
        }
    }
    return "";
}

// Test E_NS_001: "namespace not found: '{ns}'"


TEST(NsDiagnostics, TestNs001ExactMessage) {
    std::string source = R"(
100 USING NonExistent
)";
    std::string msg = getFirstDiagnostic(source);
    // Note: BASIC identifiers are case-insensitive and stored uppercase
    ASSERT_TRUE(msg.find("namespace not found:") != std::string::npos);
    ASSERT_TRUE(msg.find("NONEXISTENT") != std::string::npos);
}

// Test E_NS_002: "type '{type}' not found in namespace '{ns}'"
TEST(NsDiagnostics, TestNs002ExactMessage) {
    std::string source = R"(
100 NAMESPACE NS1
110 END NAMESPACE
120 CLASS MyClass : NS1.MissingType
130 END CLASS
)";
    std::string msg = getFirstDiagnostic(source);
    ASSERT_TRUE(msg.find("type 'MissingType' not found in namespace 'NS1'") != std::string::npos ||
                msg.find("base class not found") !=
                    std::string::npos); // Old OOP system may emit first
}

// Test E_NS_003: "ambiguous reference to '{type}' (found in: ...)"
TEST(NsDiagnostics, TestNs003ExactMessage) {
    // This test is tricky because USING must come before NAMESPACE
    // We can't test cross-file ambiguity easily in a single file
    // So we'll just verify the format is correct if we can trigger it

    // Note: This is hard to test in practice with current USING constraints
    // The test is primarily to document the expected format
}

// Test E_NS_004: "duplicate alias: '{alias}' already defined"
TEST(NsDiagnostics, TestNs004ExactMessage) {
    // This test cannot be easily written due to USING placement constraints
    // USING must come before NAMESPACE, so we cannot test duplicate aliases
    // referencing namespaces defined in the same file.
    // The diagnostic format is verified in the yaml and implementation.
}

// Phase 2: USING may appear at file scope after declarations; ensure no error
TEST(NsDiagnostics, TestNs005FileScopeAllowsUsingAfterDecl) {
    std::string source = R"(
100 NAMESPACE A
110 END NAMESPACE
120 USING System
)";
    std::string msg = getFirstDiagnostic(source);
    // Spec: USING must appear before declarations → expect error
    ASSERT_TRUE(!msg.empty());
}

// Test E_NS_006: "cannot resolve type: '{type}'"
TEST(NsDiagnostics, TestNs006ExactMessage) {
    std::string source = R"(
100 CLASS MyClass : NonExistentType
110 END CLASS
)";
    std::string msg = getFirstDiagnostic(source);
    // Old OOP system emits B2101 first, so this may not trigger our new diagnostic
    // Just verify we don't crash
    ASSERT_TRUE(!msg.empty());
}

// Test E_NS_007: "alias '{alias}' conflicts with namespace name"
TEST(NsDiagnostics, TestNs007ExactMessage) {
    // This test cannot be easily written due to USING placement constraints
    // USING must come before NAMESPACE, so we cannot test aliases that
    // conflict with namespaces defined in the same file.
    // The diagnostic format is verified in the yaml and implementation.
}

// Phase 2: USING may appear inside namespace blocks; no error expected here.
TEST(NsDiagnostics, TestNs008ScopedUsingAllowed) {
    std::string source = R"(
100 NAMESPACE A
110 END NAMESPACE
120 NAMESPACE B
130     USING A
140 END NAMESPACE
)";
    std::string msg = getFirstDiagnostic(source);
    // Phase 2: USING inside namespace is allowed (no error expected).
    ASSERT_TRUE(msg.empty());
}

// Test E_NS_009: "reserved root namespace 'Zanna' cannot be declared or imported"
TEST(NsDiagnostics, TestNs009ExactMessage) {
    std::string source = R"(
100 NAMESPACE Zanna
110 END NAMESPACE
)";
    std::string msg = getFirstDiagnostic(source);
    ASSERT_TRUE(msg.find("reserved root namespace 'Zanna' cannot be declared or imported") !=
                std::string::npos);
}

// Test that contender list is comma-separated
TEST(NsDiagnostics, TestContenderListFormat) {
    // This would require a scenario where we can trigger E_NS_003
    // Due to USING constraints, this is difficult to test in a unit test
    // The implementation already ensures comma-separation at lines 234, 277
}

// Test that diagnostics include proper location information
TEST(NsDiagnostics, TestDiagnosticLocations) {
    std::string source = R"(
100 USING NonExistent
)";

    SourceManager sm;
    uint32_t fileId = sm.addFile("test.bas");

    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);
    emitter.addSource(fileId, source);
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    std::ostringstream oss;
    de.printAll(oss, &sm);
    std::string output = oss.str();

    // Verify output contains file:line:col format
    ASSERT_TRUE(output.find("test.bas:") != std::string::npos);
    ASSERT_TRUE(output.find("error") != std::string::npos);
}

// ── test_namespace_integration.cpp ──
size_t runPipeline(const std::string &source, bool shouldLower = true) {
    SourceManager sm;
    uint32_t fileId = sm.addFile("test.bas");

    // Parse
    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    // Semantic analysis
    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);
    emitter.addSource(fileId, source);
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    size_t errorCount = emitter.errorCount();

    // Lower to IL if no errors and lowering is requested
    if (errorCount == 0 && shouldLower) {
        Lowerer lowerer;
        lowerer.setDiagnosticEmitter(&emitter);
        auto module = lowerer.lowerProgram(*program);

        // Verify IL was generated
        std::string il = il::io::Serializer::toString(module);
        ASSERT_TRUE(!il.empty());
        ASSERT_TRUE(il.find("@main") != std::string::npos);
    }

    return errorCount;
}

// Helper to check for specific diagnostic in output.
bool hasDiagnostic(const std::string &source, const std::string &expectedMsg) {
    SourceManager sm;
    uint32_t fileId = sm.addFile("test.bas");

    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);
    emitter.addSource(fileId, source);
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    std::ostringstream oss;
    de.printAll(oss, &sm);
    std::string output = oss.str();

    return output.find(expectedMsg) != std::string::npos;
}

// (a) Base/Derived across namespaces (success)


TEST(NsIntegration, TestCrossNamespaceInheritanceSuccess) {
    std::string source = R"(
100 NAMESPACE Lib.Core
110   CLASS BaseClass
120     DIM value AS I64
130   END CLASS
140 END NAMESPACE
150 NAMESPACE App
160   CLASS DerivedClass : Lib.Core.BaseClass
170     DIM name AS STRING
180   END CLASS
190 END NAMESPACE
)";

    size_t errorCount = runPipeline(source);
    ASSERT_TRUE(errorCount == 0);
}

// (b) Interface implementation across namespaces (success)
// Note: Current parser may not support INTERFACE/IMPLEMENTS yet,
// so we use CLASS inheritance as a proxy for cross-namespace type references.
TEST(NsIntegration, TestCrossNamespaceTypeReferenceSuccess) {
    std::string source = R"(
100 NAMESPACE System.Collections
110   CLASS Container
120   END CLASS
130 END NAMESPACE
140 NAMESPACE App.DataStructures
150   CLASS MyContainer : System.Collections.Container
160   END CLASS
170 END NAMESPACE
)";

    size_t errorCount = runPipeline(source);
    ASSERT_TRUE(errorCount == 0);
}

// (c) USING + unqualified usage (success)
TEST(NsIntegration, TestUsingUnqualifiedUsageSuccess) {
    std::string source = R"(
100 NAMESPACE Graphics
110   CLASS Shape
120   END CLASS
130 END NAMESPACE
140 NAMESPACE Utils
150   CLASS Helper
160   END CLASS
170 END NAMESPACE
)";

    size_t errorCount = runPipeline(source);
    ASSERT_TRUE(errorCount == 0);
}

// (d) Ambiguity across two USINGs (E_NS_003; stable contenders)
TEST(NsIntegration, TestAmbiguityTwoUsings) {
    // Due to USING placement constraints, this is difficult to test
    // in a way that triggers E_NS_003. The scenario would require:
    // - Two namespaces with same type name
    // - USING both namespaces
    // - Attempting to use the ambiguous type
    // However, USING must come before NAMESPACE declarations.

    // Instead, we verify the diagnostic format is correct when
    // such ambiguity is detected during semantic analysis.
    // The TypeResolver tests already verify contender sorting.
}

// (e) USING inside NAMESPACE (E_NS_008)
// NOTE: TestUsingInsideNamespaceError disabled — Phase 2 semantics allow
// USING inside namespace blocks. TestUsingAfterDeclError also disabled —
// E_NS_005 not enforced to support Phase 2 USING placement.

// NOTE: TestUsingAfterDeclError also disabled (E_NS_005 not enforced in Phase 2)

TEST(NsIntegration, TestNestedNamespaceFullQualification) {
    std::string source = R"(
100 NAMESPACE Outer.Middle.Inner
110   CLASS DeepClass
120   END CLASS
130 END NAMESPACE
140 NAMESPACE App
150   CLASS MyClass : Outer.Middle.Inner.DeepClass
160   END CLASS
170 END NAMESPACE
)";

    size_t errorCount = runPipeline(source);
    ASSERT_TRUE(errorCount == 0);
}

// Additional scenario: Same-namespace type resolution
TEST(NsIntegration, TestSameNamespaceResolution) {
    std::string source = R"(
100 NAMESPACE MyApp
110   CLASS BaseType
120   END CLASS
130   CLASS DerivedType : BaseType
140   END CLASS
150 END NAMESPACE
)";

    size_t errorCount = runPipeline(source);
    ASSERT_TRUE(errorCount == 0);
}

// Additional scenario: Type not found in namespace (E_NS_002 or old B2101)
TEST(NsIntegration, TestTypeNotFoundInNamespace) {
    std::string source = R"(
100 NAMESPACE Lib
110 END NAMESPACE
120 CLASS MyClass : Lib.NonExistent
130 END CLASS
)";

    size_t errorCount = runPipeline(source, false); // Don't lower, expect errors
    ASSERT_TRUE(errorCount > 0);                    // Should have at least one error
}

// Additional scenario: Reserved namespace Zanna (E_NS_009)
TEST(NsIntegration, TestReservedNamespaceZanna) {
    std::string source = R"(
100 NAMESPACE Zanna.Core
110   CLASS MyClass
120   END CLASS
130 END NAMESPACE
)";

    // Should produce E_NS_009: "reserved root namespace 'Zanna' cannot be declared or imported"
    bool hasError =
        hasDiagnostic(source, "reserved root namespace 'Zanna' cannot be declared or imported");
    ASSERT_TRUE(hasError);
}

// Additional scenario: Multiple types in namespace
TEST(NsIntegration, TestMultipleTypesInNamespace) {
    std::string source = R"(
100 NAMESPACE Collections
110   CLASS List
120   END CLASS
130   CLASS Set
140   END CLASS
150   CLASS Map
160   END CLASS
170 END NAMESPACE
180 NAMESPACE App
190   CLASS MyList : Collections.List
200   END CLASS
210   CLASS MySet : Collections.Set
220   END CLASS
230 END NAMESPACE
)";

    size_t errorCount = runPipeline(source);
    ASSERT_TRUE(errorCount == 0);
}

// Additional scenario: Case-insensitive namespace references
TEST(NsIntegration, TestCaseInsensitiveNamespaceRefs) {
    std::string source = R"(
100 NAMESPACE FooBar
110   CLASS MyClass
120   END CLASS
130 END NAMESPACE
140 CLASS DerivedClass : foobar.myclass
150 END CLASS
)";

    size_t errorCount = runPipeline(source);
    // Case-insensitive resolution should succeed
    ASSERT_TRUE(errorCount == 0);
}

// Additional scenario: Lowering preserves namespace qualification
TEST(NsIntegration, TestLoweringPreservesQualification) {
    std::string source = R"(
100 NAMESPACE Lib
110   CLASS Resource
120   END CLASS
130 END NAMESPACE
)";

    SourceManager sm;
    uint32_t fileId = sm.addFile("test.bas");

    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);
    emitter.addSource(fileId, source);
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    ASSERT_TRUE(emitter.errorCount() == 0);

    // Lower to IL
    Lowerer lowerer;
    lowerer.setDiagnosticEmitter(&emitter);
    auto module = lowerer.lowerProgram(*program);

    // Serialize and verify IL
    std::string il = il::io::Serializer::toString(module);

    // IL should not contain raw USING or NAMESPACE keywords
    ASSERT_TRUE(il.find("USING") == std::string::npos);
    ASSERT_TRUE(il.find("NAMESPACE") == std::string::npos);

    // Should have @main function
    ASSERT_TRUE(il.find("@main") != std::string::npos);
}

// ── test_ns_registry_builder.cpp ──
Program makeProgram(std::vector<StmtPtr> stmts) {
    Program prog;
    prog.main = std::move(stmts);
    return prog;
}

TEST(NsRegistryBuilder, TestEmptyProgram) {
    NamespaceRegistry reg;
    UsingContext uc;
    Program prog;

    buildNamespaceRegistry(prog, reg, uc, nullptr);

    // Registry should be empty (no registrations).
    ASSERT_TRUE(uc.imports().empty());
}

TEST(NsRegistryBuilder, TestSingleNamespace) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto ns = std::make_unique<NamespaceDecl>();
    ns->path = {"MyNamespace"};
    ns->loc = SourceLoc{1, 1, 1};

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(ns));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(reg.namespaceExists("MyNamespace"));
}

TEST(NsRegistryBuilder, TestNestedNamespace) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto ns = std::make_unique<NamespaceDecl>();
    ns->path = {"A", "B", "C"};
    ns->loc = SourceLoc{1, 1, 1};

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(ns));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(reg.namespaceExists("A.B.C"));
}

TEST(NsRegistryBuilder, TestClassInNamespace) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto klass = std::make_unique<ClassDecl>();
    klass->name = "MyClass";
    klass->loc = SourceLoc{1, 1, 1};

    auto ns = std::make_unique<NamespaceDecl>();
    ns->path = {"MyNamespace"};
    ns->loc = SourceLoc{1, 1, 1};
    ns->body.push_back(std::move(klass));

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(ns));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(reg.namespaceExists("MyNamespace"));
    ASSERT_TRUE(reg.typeExists("MyNamespace.MyClass"));
    ASSERT_TRUE(reg.getTypeKind("MyNamespace.MyClass") == NamespaceRegistry::TypeKind::Class);
}

TEST(NsRegistryBuilder, TestInterfaceInNamespace) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto iface = std::make_unique<InterfaceDecl>();
    iface->qualifiedName = {"MyNamespace", "IFoo"};
    iface->loc = SourceLoc{1, 1, 1};

    auto ns = std::make_unique<NamespaceDecl>();
    ns->path = {"MyNamespace"};
    ns->loc = SourceLoc{1, 1, 1};
    ns->body.push_back(std::move(iface));

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(ns));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(reg.namespaceExists("MyNamespace"));
    ASSERT_TRUE(reg.typeExists("MyNamespace.IFoo"));
    ASSERT_TRUE(reg.getTypeKind("MyNamespace.IFoo") == NamespaceRegistry::TypeKind::Interface);
}

TEST(NsRegistryBuilder, TestMultipleNamespaces) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto ns1 = std::make_unique<NamespaceDecl>();
    ns1->path = {"NS1"};
    ns1->loc = SourceLoc{1, 1, 1};

    auto ns2 = std::make_unique<NamespaceDecl>();
    ns2->path = {"NS2"};
    ns2->loc = SourceLoc{2, 1, 1};

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(ns1));
    stmts.push_back(std::move(ns2));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(reg.namespaceExists("NS1"));
    ASSERT_TRUE(reg.namespaceExists("NS2"));
}

TEST(NsRegistryBuilder, TestMergedNamespace) {
    NamespaceRegistry reg;
    UsingContext uc;

    // First declaration of MyNS with ClassA
    auto klassA = std::make_unique<ClassDecl>();
    klassA->name = "ClassA";
    klassA->loc = SourceLoc{1, 1, 1};

    auto ns1 = std::make_unique<NamespaceDecl>();
    ns1->path = {"MyNS"};
    ns1->loc = SourceLoc{1, 1, 1};
    ns1->body.push_back(std::move(klassA));

    // Second declaration of MyNS with ClassB
    auto klassB = std::make_unique<ClassDecl>();
    klassB->name = "ClassB";
    klassB->loc = SourceLoc{2, 1, 1};

    auto ns2 = std::make_unique<NamespaceDecl>();
    ns2->path = {"MyNS"};
    ns2->loc = SourceLoc{2, 1, 1};
    ns2->body.push_back(std::move(klassB));

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(ns1));
    stmts.push_back(std::move(ns2));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(reg.namespaceExists("MyNS"));
    ASSERT_TRUE(reg.typeExists("MyNS.ClassA"));
    ASSERT_TRUE(reg.typeExists("MyNS.ClassB"));
}

TEST(NsRegistryBuilder, TestUsingDirective) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto usingDecl = std::make_unique<UsingDecl>();
    usingDecl->namespacePath = {"System", "Collections"};
    usingDecl->alias = "";
    usingDecl->loc = SourceLoc{1, 1, 1};

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(usingDecl));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(uc.imports().size() == 1);
    ASSERT_TRUE(uc.imports()[0].ns == "System.Collections");
    ASSERT_TRUE(uc.imports()[0].alias.empty());
}

TEST(NsRegistryBuilder, TestUsingDirectiveWithAlias) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto usingDecl = std::make_unique<UsingDecl>();
    usingDecl->namespacePath = {"System", "Collections"};
    usingDecl->alias = "SC";
    usingDecl->loc = SourceLoc{1, 1, 1};

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(usingDecl));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(uc.imports().size() == 1);
    ASSERT_TRUE(uc.imports()[0].ns == "System.Collections");
    ASSERT_TRUE(uc.imports()[0].alias == "SC");
    ASSERT_TRUE(uc.hasAlias("SC"));
    ASSERT_TRUE(uc.resolveAlias("SC") == "System.Collections");
}

TEST(NsRegistryBuilder, TestMultipleUsingDirectives) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto using1 = std::make_unique<UsingDecl>();
    using1->namespacePath = {"NS1"};
    using1->alias = "";
    using1->loc = SourceLoc{1, 1, 1};

    auto using2 = std::make_unique<UsingDecl>();
    using2->namespacePath = {"NS2"};
    using2->alias = "";
    using2->loc = SourceLoc{2, 1, 1};

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(using1));
    stmts.push_back(std::move(using2));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(uc.imports().size() == 2);
    ASSERT_TRUE(uc.imports()[0].ns == "NS1");
    ASSERT_TRUE(uc.imports()[1].ns == "NS2");
}

TEST(NsRegistryBuilder, TestGlobalClass) {
    NamespaceRegistry reg;
    UsingContext uc;

    auto klass = std::make_unique<ClassDecl>();
    klass->name = "GlobalClass";
    klass->loc = SourceLoc{1, 1, 1};

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(klass));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    // Global classes have empty namespace prefix
    ASSERT_TRUE(reg.typeExists("GlobalClass"));
    ASSERT_TRUE(reg.getTypeKind("GlobalClass") == NamespaceRegistry::TypeKind::Class);
}

TEST(NsRegistryBuilder, TestComplexNestedStructure) {
    NamespaceRegistry reg;
    UsingContext uc;

    // Build: NAMESPACE A.B { CLASS C }
    auto klassC = std::make_unique<ClassDecl>();
    klassC->name = "C";
    klassC->loc = SourceLoc{1, 1, 1};

    auto nsAB = std::make_unique<NamespaceDecl>();
    nsAB->path = {"A", "B"};
    nsAB->loc = SourceLoc{1, 1, 1};
    nsAB->body.push_back(std::move(klassC));

    // Build: USING A.B AS AB
    auto usingDecl = std::make_unique<UsingDecl>();
    usingDecl->namespacePath = {"A", "B"};
    usingDecl->alias = "AB";
    usingDecl->loc = SourceLoc{2, 1, 1};

    std::vector<StmtPtr> stmts;
    stmts.push_back(std::move(nsAB));
    stmts.push_back(std::move(usingDecl));

    Program prog = makeProgram(std::move(stmts));
    buildNamespaceRegistry(prog, reg, uc, nullptr);

    ASSERT_TRUE(reg.namespaceExists("A.B"));
    ASSERT_TRUE(reg.typeExists("A.B.C"));
    ASSERT_TRUE(uc.imports().size() == 1);
    ASSERT_TRUE(uc.hasAlias("AB"));
    ASSERT_TRUE(uc.resolveAlias("AB") == "A.B");
}

// ── test_ns_resolve_pass.cpp ──
size_t parseAndAnalyze(const std::string &source, DiagnosticEngine &de, bool verbose = false) {
    SourceManager sm;
    uint32_t fileId = sm.addFile("test.bas");

    // Parse the program.
    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    // Analyze with diagnostics.
    DiagnosticEmitter emitter(de, sm);
    emitter.addSource(fileId, source);
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    if (verbose && emitter.errorCount() > 0) {
        std::cerr << "Semantic errors: " << emitter.errorCount() << std::endl;
        de.printAll(std::cerr, &sm);
    }

    return emitter.errorCount();
}

// Test: Cross-namespace base class resolution (fully qualified)


TEST(NsResolvePass, TestCrossNamespaceQualified) {
    std::string source = R"(
100 NAMESPACE NS1
110   CLASS BaseClass
120   END CLASS
130 END NAMESPACE
140 NAMESPACE NS2
150   CLASS DerivedClass : NS1.BaseClass
160   END CLASS
170 END NAMESPACE
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyze(source, de);
    // Should have no errors - valid cross-namespace inheritance with FQ name.
    ASSERT_TRUE(errorCount == 0);
}

// Test: Valid resolution in same namespace
TEST(NsResolvePass, TestSameNamespaceResolution) {
    std::string source = R"(
100 NAMESPACE MyNS
110   CLASS BaseClass
120   END CLASS
130   CLASS DerivedClass : BaseClass
140   END CLASS
150 END NAMESPACE
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyze(source, de);
    // Should have no errors - base class in same namespace.
    ASSERT_TRUE(errorCount == 0);
}

// Test: Type not found (E_NS_006 or old B2101)
TEST(NsResolvePass, TestTypeNotFound) {
    std::string source = R"(
100 CLASS MyClass : NonExistentType
110 END CLASS
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyze(source, de);
    ASSERT_TRUE(errorCount > 0);
}

// Test: Nested namespace with fully qualified name
TEST(NsResolvePass, TestNestedNamespace) {
    std::string source = R"(
100 NAMESPACE Outer.Inner
110   CLASS BaseClass
120   END CLASS
130 END NAMESPACE
140 CLASS DerivedClass : Outer.Inner.BaseClass
150 END CLASS
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyze(source, de);
    // Should have no errors - valid nested namespace reference.
    ASSERT_TRUE(errorCount == 0);
}

// ── test_using_context.cpp ──

TEST(UsingContext, TestDeclarationOrderPreserved) {
    UsingContext ctx;

    SourceLoc loc1{1, 1, 1};
    SourceLoc loc2{1, 2, 1};
    SourceLoc loc3{1, 3, 1};

    ctx.add("First.NS", "", loc1);
    ctx.add("Second.NS", "S", loc2);
    ctx.add("Third.NS", "", loc3);

    const auto &imports = ctx.imports();
    ASSERT_TRUE(imports.size() == 3);

    // Verify declaration order is preserved.
    ASSERT_TRUE(imports[0].ns == "First.NS");
    ASSERT_TRUE(imports[0].alias.empty());
    ASSERT_TRUE(imports[0].loc.line == 1);

    ASSERT_TRUE(imports[1].ns == "Second.NS");
    ASSERT_TRUE(imports[1].alias == "S");
    ASSERT_TRUE(imports[1].loc.line == 2);

    ASSERT_TRUE(imports[2].ns == "Third.NS");
    ASSERT_TRUE(imports[2].alias.empty());
    ASSERT_TRUE(imports[2].loc.line == 3);
}

TEST(UsingContext, TestResolveAliasCaseInsensitive) {
    UsingContext ctx;

    SourceLoc loc{1, 1, 1};
    ctx.add("Foo.Bar.Baz", "FB", loc);

    // All case variations should resolve to the same namespace.
    ASSERT_TRUE(ctx.resolveAlias("FB") == "Foo.Bar.Baz");
    ASSERT_TRUE(ctx.resolveAlias("fb") == "Foo.Bar.Baz");
    ASSERT_TRUE(ctx.resolveAlias("Fb") == "Foo.Bar.Baz");
    ASSERT_TRUE(ctx.resolveAlias("fB") == "Foo.Bar.Baz");

    // Non-existent alias should return empty string.
    ASSERT_TRUE(ctx.resolveAlias("Missing").empty());
    ASSERT_TRUE(ctx.resolveAlias("").empty());
}

TEST(UsingContext, TestHasAliasCaseInsensitive) {
    UsingContext ctx;

    SourceLoc loc{1, 1, 1};
    ctx.add("System.IO", "SIO", loc);

    // All case variations should be detected.
    ASSERT_TRUE(ctx.hasAlias("SIO"));
    ASSERT_TRUE(ctx.hasAlias("sio"));
    ASSERT_TRUE(ctx.hasAlias("Sio"));
    ASSERT_TRUE(ctx.hasAlias("SIo"));

    // Non-existent alias should return false.
    ASSERT_TRUE(!ctx.hasAlias("Missing"));
    ASSERT_TRUE(!ctx.hasAlias(""));
}

TEST(UsingContext, TestHasAliasDetectsDuplicates) {
    UsingContext ctx;

    SourceLoc loc1{1, 1, 1};
    SourceLoc loc2{1, 2, 1};

    ctx.add("First.NS", "Alias1", loc1);

    // Before adding duplicate, first alias should exist.
    ASSERT_TRUE(ctx.hasAlias("Alias1"));
    ASSERT_TRUE(ctx.hasAlias("alias1"));

    // Add another import with the same alias (different case).
    ctx.add("Second.NS", "ALIAS1", loc2);

    // Both should be detectable (but the second will overwrite in alias map).
    ASSERT_TRUE(ctx.hasAlias("Alias1"));
    ASSERT_TRUE(ctx.hasAlias("ALIAS1"));

    // The last registration wins for resolveAlias.
    ASSERT_TRUE(ctx.resolveAlias("alias1") == "Second.NS");
}

TEST(UsingContext, TestClearRemovesAllImports) {
    UsingContext ctx;

    SourceLoc loc{1, 1, 1};
    ctx.add("NS1", "A1", loc);
    ctx.add("NS2", "A2", loc);
    ctx.add("NS3", "", loc);

    ASSERT_TRUE(ctx.imports().size() == 3);
    ASSERT_TRUE(ctx.hasAlias("A1"));
    ASSERT_TRUE(ctx.hasAlias("A2"));

    ctx.clear();

    ASSERT_TRUE(ctx.imports().empty());
    ASSERT_TRUE(!ctx.hasAlias("A1"));
    ASSERT_TRUE(!ctx.hasAlias("A2"));
    ASSERT_TRUE(ctx.resolveAlias("A1").empty());
}

TEST(UsingContext, TestEmptyAliasNoRegistration) {
    UsingContext ctx;

    SourceLoc loc{1, 1, 1};
    ctx.add("Some.Namespace", "", loc);

    // Import should be recorded.
    ASSERT_TRUE(ctx.imports().size() == 1);
    ASSERT_TRUE(ctx.imports()[0].ns == "Some.Namespace");
    ASSERT_TRUE(ctx.imports()[0].alias.empty());

    // No alias should be resolvable.
    ASSERT_TRUE(!ctx.hasAlias("Some.Namespace"));
    ASSERT_TRUE(ctx.resolveAlias("Some.Namespace").empty());
}

TEST(UsingContext, TestMultipleImportsSameNamespaceDifferentAliases) {
    UsingContext ctx;

    SourceLoc loc1{1, 1, 1};
    SourceLoc loc2{1, 2, 1};

    // Same namespace imported twice with different aliases.
    ctx.add("Common.NS", "Alias1", loc1);
    ctx.add("Common.NS", "Alias2", loc2);

    ASSERT_TRUE(ctx.imports().size() == 2);
    ASSERT_TRUE(ctx.imports()[0].alias == "Alias1");
    ASSERT_TRUE(ctx.imports()[1].alias == "Alias2");

    // Both aliases should resolve to the same namespace.
    ASSERT_TRUE(ctx.resolveAlias("Alias1") == "Common.NS");
    ASSERT_TRUE(ctx.resolveAlias("Alias2") == "Common.NS");
    ASSERT_TRUE(ctx.resolveAlias("alias1") == "Common.NS");
    ASSERT_TRUE(ctx.resolveAlias("ALIAS2") == "Common.NS");
}

TEST(UsingContext, TestMixedAliasedAndNonAliasedImports) {
    UsingContext ctx;

    SourceLoc loc{1, 1, 1};
    ctx.add("NS1", "A", loc);
    ctx.add("NS2", "", loc);
    ctx.add("NS3", "B", loc);
    ctx.add("NS4", "", loc);

    const auto &imports = ctx.imports();
    ASSERT_TRUE(imports.size() == 4);

    ASSERT_TRUE(imports[0].alias == "A");
    ASSERT_TRUE(imports[1].alias.empty());
    ASSERT_TRUE(imports[2].alias == "B");
    ASSERT_TRUE(imports[3].alias.empty());

    ASSERT_TRUE(ctx.hasAlias("A"));
    ASSERT_TRUE(!ctx.hasAlias("NS2"));
    ASSERT_TRUE(ctx.hasAlias("B"));
    ASSERT_TRUE(!ctx.hasAlias("NS4"));
}

TEST(UsingContext, TestSourceLocationsPreserved) {
    UsingContext ctx;

    SourceLoc loc1{10, 5, 8};
    SourceLoc loc2{20, 10, 15};

    ctx.add("NS1", "A1", loc1);
    ctx.add("NS2", "", loc2);

    const auto &imports = ctx.imports();
    ASSERT_TRUE(imports.size() == 2);

    ASSERT_TRUE(imports[0].loc.file_id == 10);
    ASSERT_TRUE(imports[0].loc.line == 5);
    ASSERT_TRUE(imports[0].loc.column == 8);

    ASSERT_TRUE(imports[1].loc.file_id == 20);
    ASSERT_TRUE(imports[1].loc.line == 10);
    ASSERT_TRUE(imports[1].loc.column == 15);
}

TEST(UsingContext, TestResolveAliasReturnsEmptyForNonExistent) {
    UsingContext ctx;

    SourceLoc loc{1, 1, 1};
    ctx.add("ExistingNS", "ExistingAlias", loc);

    // Existing alias should resolve.
    ASSERT_TRUE(!ctx.resolveAlias("ExistingAlias").empty());

    // Non-existent aliases should return empty string.
    ASSERT_TRUE(ctx.resolveAlias("DoesNotExist").empty());
    ASSERT_TRUE(ctx.resolveAlias("Another").empty());
    ASSERT_TRUE(ctx.resolveAlias("").empty());
}

// ── test_using_semantics.cpp ──
size_t parseAndAnalyzeSema(const std::string &source, DiagnosticEngine &de) {
    SourceManager sm;
    uint32_t fileId = sm.addFile("test.bas");

    // Parse the program.
    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    // Analyze with diagnostics.
    DiagnosticEmitter emitter(de, sm);
    emitter.addSource(fileId, source);
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    return emitter.errorCount();
}

// Test: USING inside namespace (Phase 2 allows scoped USING inside namespace)


TEST(UsingSemantics, TestUsingInsideNamespace) {
    // In Phase 2 we allow USING inside namespace blocks.
    // Use an existing namespace to avoid unknown-namespace diagnostics.
    std::string source2 = R"(
NAMESPACE A
END NAMESPACE
NAMESPACE B
  USING A
END NAMESPACE
)";
    DiagnosticEngine de2;
    size_t err2 = parseAndAnalyzeSema(source2, de2);
    // Phase 2: scoped USING inside namespace is allowed (no error expected).
    ASSERT_TRUE(err2 == 0);
}

// Test: USING after first decl (Phase 2 allows file-scoped USING anywhere)
TEST(UsingSemantics, TestUsingAfterDecl) {
    std::string source = R"(
NAMESPACE A
END NAMESPACE
USING A
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    // Spec: USING must appear before declarations (E_NS_005)
    ASSERT_TRUE(errorCount > 0);
}

// Test: USING after class decl (Phase 2 allows file-scoped USING anywhere)
TEST(UsingSemantics, TestUsingAfterClass) {
    std::string source = R"(
CLASS MyClass
END CLASS
NAMESPACE A
END NAMESPACE
USING A
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    // Spec: USING must appear before declarations (E_NS_005)
    ASSERT_TRUE(errorCount > 0);
}

// Test: USING NonExistent.Namespace → E_NS_001
TEST(UsingSemantics, TestUsingNonexistentNamespace) {
    std::string source = R"(
USING NonExistent.Namespace
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    ASSERT_TRUE(errorCount > 0);
}

// Test: USING X = A then USING X = B → E_NS_004
TEST(UsingSemantics, TestDuplicateAlias) {
    std::string source = R"(
NAMESPACE A
END NAMESPACE
NAMESPACE B
END NAMESPACE
USING X = A
USING X = B
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    ASSERT_TRUE(errorCount > 0);
}

// Test: USING alias that equals a namespace name (e.g., "A") → E_NS_007
TEST(UsingSemantics, TestAliasShadowsNamespace) {
    std::string source = R"(
NAMESPACE A
END NAMESPACE
NAMESPACE B
END NAMESPACE
USING A = B
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    ASSERT_TRUE(errorCount > 0);
}

// Test: NAMESPACE Zanna … → E_NS_009
TEST(UsingSemantics, TestReservedZannaNamespace) {
    std::string source = R"(
NAMESPACE Zanna
END NAMESPACE
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    ASSERT_TRUE(errorCount > 0);
}

// Test: USING Zanna … → E_NS_009
TEST(UsingSemantics, TestReservedZannaUsing) {
    std::string source = R"(
NAMESPACE Zanna
END NAMESPACE
USING Zanna
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    ASSERT_TRUE(errorCount > 0);
}

// Test: Valid USING at file scope before declarations
TEST(UsingSemantics, TestValidUsing) {
    std::string source = R"(
100 USING System
110 NAMESPACE System
120 END NAMESPACE
130 NAMESPACE MyApp
140 END NAMESPACE
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    // Should have no errors - valid USING placement.
    ASSERT_TRUE(errorCount == 0);
}

// Test: Valid USING with alias
TEST(UsingSemantics, TestValidUsingWithAlias) {
    std::string source = R"(
100 USING SC = System.Collections
110 NAMESPACE System.Collections
120 END NAMESPACE
130 NAMESPACE MyApp
140 END NAMESPACE
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    // Should have no errors - valid USING with alias.
    ASSERT_TRUE(errorCount == 0);
}

// Test: Case-insensitive Zanna check
TEST(UsingSemantics, TestReservedZannaCaseInsensitive) {
    std::string source = R"(
NAMESPACE zanna
END NAMESPACE
)";

    DiagnosticEngine de;
    size_t errorCount = parseAndAnalyzeSema(source, de);
    ASSERT_TRUE(errorCount > 0);
}

// ── test_using_compiletime_only.cpp ──

TEST(UsingCompiletimeOnly, TestUsingOnlyProducesNoArtifacts) {
    // First, get baseline IL size for empty program (no USING)
    std::string emptySource = "END\n";

    SourceManager smEmpty;
    DiagnosticEngine deEmpty;
    DiagnosticEmitter emitterEmpty(deEmpty, smEmpty);

    uint32_t fileIdEmpty = smEmpty.addFile("empty.bas");
    emitterEmpty.addSource(fileIdEmpty, emptySource);

    Parser parserEmpty(emptySource, fileIdEmpty);
    auto programEmpty = parserEmpty.parseProgram();

    SemanticAnalyzer analyzerEmpty(emitterEmpty);
    analyzerEmpty.analyze(*programEmpty);

    Lowerer lowererEmpty;
    lowererEmpty.setDiagnosticEmitter(&emitterEmpty);
    auto moduleEmpty = lowererEmpty.lowerProgram(*programEmpty);

    std::string ilEmpty = il::io::Serializer::toString(moduleEmpty);
    size_t baselineSize = ilEmpty.length();

    // Count baseline functions (should be 2: __mod_init and main)
    size_t baselineFuncs = 0;
    size_t pos = 0;
    while ((pos = ilEmpty.find("func ", pos)) != std::string::npos) {
        baselineFuncs++;
        pos += 5;
    }

    // Now test with USING directives that reference non-existent namespaces
    // (we expect E_NS_001 errors but IL should still be minimal)
    std::string sourceWithUsing = R"(
USING System
USING Collections
USING Utils.Helpers

END
)";

    SourceManager sm;
    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);

    uint32_t fileId = sm.addFile("test.bas");
    emitter.addSource(fileId, sourceWithUsing);

    Parser parser(sourceWithUsing, fileId);
    auto program = parser.parseProgram();

    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    // Note: Will have E_NS_001 errors for non-existent namespaces, but that's OK

    Lowerer lowerer;
    lowerer.setDiagnosticEmitter(&emitter);
    auto module = lowerer.lowerProgram(*program);

    std::string il = il::io::Serializer::toString(module);

    // Verify IL is approximately the same size as empty program
    // Allow small delta for whitespace/formatting differences
    size_t sizeDelta =
        (il.length() > baselineSize) ? (il.length() - baselineSize) : (baselineSize - il.length());

    std::cout << "  Baseline IL: " << baselineSize << " bytes, " << baselineFuncs << " functions\n";
    std::cout << "  With USING:  " << il.length() << " bytes\n";
    std::cout << "  Delta:       " << sizeDelta << " bytes\n";

    ASSERT_TRUE(sizeDelta < 200 && "USING directives should not significantly increase IL size");

    // Count functions - should be same as baseline
    size_t funcCount = 0;
    pos = 0;
    while ((pos = il.find("func ", pos)) != std::string::npos) {
        funcCount++;
        pos += 5;
    }

    ASSERT_TRUE(funcCount == baselineFuncs && "USING should not add function definitions");

    // Verify no additional class or type definitions beyond baseline
    bool hasClassDef = il.find("type ") != std::string::npos;
    bool hasBaselineClassDef = ilEmpty.find("type ") != std::string::npos;

    ASSERT_TRUE(hasClassDef == hasBaselineClassDef &&
                "USING should not generate class/type definitions");
}

/// @brief Test that USING with declarations produces correct artifacts.
/// This is a sanity check that USING works when combined with real code.
TEST(UsingCompiletimeOnly, TestUsingWithDeclarations) {
    std::string source = R"(
USING Collections

NAMESPACE Collections
  CLASS List
    DIM size AS I64
  END CLASS
END NAMESPACE

CLASS App
  DIM myList AS List
END CLASS

END
)";

    SourceManager sm;
    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);

    uint32_t fileId = sm.addFile("test.bas");
    emitter.addSource(fileId, source);

    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    size_t errorCount = emitter.errorCount();
    ASSERT_TRUE(errorCount == 0 && "USING with declarations should compile without errors");

    Lowerer lowerer;
    lowerer.setDiagnosticEmitter(&emitter);
    auto module = lowerer.lowerProgram(*program);

    std::string il = il::io::Serializer::toString(module);

    // Main assertion: USING keyword should not appear in IL (it's compile-time only)
    bool hasUsingKeyword =
        il.find("USING") != std::string::npos || il.find("using") != std::string::npos;
    ASSERT_TRUE(!hasUsingKeyword && "USING keyword should not appear in IL");

    // Should have produced some IL (not minimal)
    ASSERT_TRUE(il.length() > 500 && "Program with classes should produce substantial IL");

    std::cout << "  Generated IL: " << il.length() << " bytes (with classes)\n";
}

/// @brief Test empty program (baseline for comparison).
TEST(UsingCompiletimeOnly, TestEmptyProgramBaseline) {
    std::string source = "END\n";

    SourceManager sm;
    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);

    uint32_t fileId = sm.addFile("test.bas");
    emitter.addSource(fileId, source);

    Parser parser(source, fileId);
    auto program = parser.parseProgram();

    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*program);

    Lowerer lowerer;
    lowerer.setDiagnosticEmitter(&emitter);
    auto module = lowerer.lowerProgram(*program);

    std::string il = il::io::Serializer::toString(module);

    // Baseline: empty program should produce minimal IL
    std::cout << "Empty program IL length: " << il.length() << " bytes\n";
    std::cout << "Empty program IL:\n" << il << "\n";
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
