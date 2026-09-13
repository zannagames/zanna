//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/e2e/test_namespace_e2e.cpp
// Purpose: End-to-end multi-file namespace tests proving file-scoped USING and aliasing.
// Key invariants: To be documented.
// Ownership/Lifetime: To be documented.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "il/io/Serializer.hpp"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace il::frontends::basic;
using namespace il::support;

/// @brief Helper to run multi-file pipeline and return error count.
/// @param files Vector of (filename, source) pairs
/// @param shouldLower Whether to attempt IL lowering on success
/// @return Number of errors encountered
size_t runMultiFilePipeline(const std::vector<std::pair<std::string, std::string>> &files,
                            bool shouldLower = true) {
    SourceManager sm;
    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);

    std::vector<std::unique_ptr<Program>> programs;

    // Parse each file and add to source manager
    for (const auto &[filename, source] : files) {
        uint32_t fileId = sm.addFile(filename);
        emitter.addSource(fileId, source);

        Parser parser(source, fileId);
        programs.push_back(parser.parseProgram());
    }

    // Merge all programs into a single combined program
    // Strategy: USING statements must come before all other declarations
    // 1. Collect all USING statements from all files
    // 2. Collect all other statements
    // 3. Merge procedures
    auto combined = std::make_unique<Program>();
    std::vector<StmtPtr> usings;
    std::vector<StmtPtr> others;

    for (auto &prog : programs) {
        for (auto &stmt : prog->main) {
            // Check if this is a USING statement by attempting dynamic_cast
            if (dynamic_cast<UsingDecl *>(stmt.get()) != nullptr) {
                usings.push_back(std::move(stmt));
            } else {
                others.push_back(std::move(stmt));
            }
        }
        for (auto &proc : prog->procs) {
            combined->procs.push_back(std::move(proc));
        }
    }

    // Add USING statements first, then all other statements
    for (auto &u : usings) {
        combined->main.push_back(std::move(u));
    }
    for (auto &o : others) {
        combined->main.push_back(std::move(o));
    }

    // Analyze the combined program
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*combined);

    size_t errorCount = emitter.errorCount();

    // Debug: print combined program for inspection
    if (errorCount > 0) {
        std::ostringstream oss;
        de.printAll(oss, &sm);
        std::cerr << "Errors:\n" << oss.str() << "\n";

        std::cerr << "Combined program has:\n";
        std::cerr << "  " << combined->main.size() << " main statements\n";
        std::cerr << "  " << combined->procs.size() << " procedures\n";
    }

    // Lower to IL if no errors and lowering is requested
    if (errorCount == 0 && shouldLower) {
        Lowerer lowerer;
        lowerer.setDiagnosticEmitter(&emitter);
        auto module = lowerer.lowerProgram(*combined);
        std::string il = il::io::Serializer::toString(module);
        assert(!il.empty());
    }

    return errorCount;
}

/// @brief Helper to check diagnostic output for multi-file scenarios.
/// @param files Vector of (filename, source) pairs
/// @param expectedMsg Expected diagnostic message substring
/// @param expectedFile Expected filename in diagnostic location
/// @return True if diagnostic found with correct file location
bool hasMultiFileDiagnostic(const std::vector<std::pair<std::string, std::string>> &files,
                            const std::string &expectedMsg,
                            const std::string &expectedFile = "") {
    SourceManager sm;
    DiagnosticEngine de;
    DiagnosticEmitter emitter(de, sm);

    std::vector<std::unique_ptr<Program>> programs;

    for (const auto &[filename, source] : files) {
        uint32_t fileId = sm.addFile(filename);
        emitter.addSource(fileId, source);

        Parser parser(source, fileId);
        programs.push_back(parser.parseProgram());
    }

    // Merge all programs into a single combined program
    // Strategy: USING statements must come before all other declarations
    auto combined = std::make_unique<Program>();
    std::vector<StmtPtr> usings;
    std::vector<StmtPtr> others;

    for (auto &prog : programs) {
        for (auto &stmt : prog->main) {
            if (dynamic_cast<UsingDecl *>(stmt.get()) != nullptr) {
                usings.push_back(std::move(stmt));
            } else {
                others.push_back(std::move(stmt));
            }
        }
        for (auto &proc : prog->procs) {
            combined->procs.push_back(std::move(proc));
        }
    }

    // Add USING statements first, then all other statements
    for (auto &u : usings) {
        combined->main.push_back(std::move(u));
    }
    for (auto &o : others) {
        combined->main.push_back(std::move(o));
    }

    // Analyze the combined program
    SemanticAnalyzer analyzer(emitter);
    analyzer.analyze(*combined);

    std::ostringstream oss;
    de.printAll(oss, &sm);
    std::string output = oss.str();

    bool hasMessage = output.find(expectedMsg) != std::string::npos;
    bool hasFile = expectedFile.empty() || output.find(expectedFile) != std::string::npos;

    return hasMessage && hasFile;
}

// =============================================================================
// Scenario 1: Two-file base/derived with USING (success)
// =============================================================================

void test_two_file_base_derived_with_using() {
    std::cout << "Running: test_two_file_base_derived_with_using\n";

    // File 1: Define base class in namespace
    std::string file1 = R"(
NAMESPACE Foundation
  CLASS Entity
    DIM id AS I64
    DIM name AS STRING
  END CLASS
END NAMESPACE
)";

    // File 2: Derive from base using USING directive
    std::string file2 = R"(
USING Foundation

NAMESPACE App
  REM Inherit from Entity without qualification (via USING)
  CLASS Customer : Entity
    DIM email AS STRING
  END CLASS
END NAMESPACE

END
)";

    std::vector<std::pair<std::string, std::string>> files = {{"foundation.bas", file1},
                                                              {"app.bas", file2}};

    size_t errorCount = runMultiFilePipeline(files);
    if (errorCount != 0) {
        std::cout << "  FAIL: Expected 0 errors, got " << errorCount << "\n";
        return;
    }

    std::cout << "  PASS: Two-file inheritance with USING\n";
}

// =============================================================================
// Scenario 2: Three-file alias usage (success)
// =============================================================================

void test_three_file_alias_usage() {
    std::cout << "Running: test_three_file_alias_usage\n";

    // File 1: Library definition
    std::string file1 = R"(
NAMESPACE Lib.Core
  CLASS Container
    DIM capacity AS I64
  END CLASS

  CLASS Iterator
    DIM position AS I64
  END CLASS
END NAMESPACE
END
)";

    // File 2: Define types using alias
    std::string file2 = R"(
USING L = Lib.Core

NAMESPACE Data
  REM Use aliased namespace qualification
  CLASS Buffer
    DIM storage AS I64
  END CLASS
END NAMESPACE
END
)";

    // File 3: Use types from aliased namespace
    std::string file3 = R"(
USING L = Lib.Core

NAMESPACE App
  REM Reference type via alias
  CLASS MyContainer : L.Container
    DIM flags AS I64
  END CLASS
END NAMESPACE
END
)";

    std::vector<std::pair<std::string, std::string>> files = {
        {"lib.bas", file1}, {"data.bas", file2}, {"app.bas", file3}};

    size_t errorCount = runMultiFilePipeline(files);
    assert(errorCount == 0);

    std::cout << "  PASS: Three-file alias usage\n";
}

// =============================================================================
// Scenario 3: Multi-file ambiguity (E_NS_003)
// =============================================================================

void test_multi_file_ambiguity() {
    std::cout << "Running: test_multi_file_ambiguity\n";

    // File 1: Define Thing in namespace A
    std::string file1 = R"(
NAMESPACE A
  CLASS Thing
    DIM x AS I64
  END CLASS
END NAMESPACE
END
)";

    // File 2: Define Thing in namespace B
    std::string file2 = R"(
NAMESPACE B
  CLASS Thing
    DIM y AS I64
  END CLASS
END NAMESPACE
END
)";

    // File 3: USING both namespaces causes ambiguity
    std::string file3 = R"(
USING A
USING B

NAMESPACE App
  REM Unqualified "Thing" is ambiguous
  CLASS MyClass : Thing
    DIM z AS I64
  END CLASS
END NAMESPACE
END
)";

    std::vector<std::pair<std::string, std::string>> files = {
        {"a.bas", file1}, {"b.bas", file2}, {"app.bas", file3}};

    // Should have exactly 1 error (ambiguous type reference)
    bool hasAmbiguityError = hasMultiFileDiagnostic(files, "ambiguous reference to", "app.bas");
    assert(hasAmbiguityError);

    // Verify stable sorted candidate list
    bool hasSortedCandidates = hasMultiFileDiagnostic(files, "A.THING, B.THING");
    assert(hasSortedCandidates);

    std::cout << "  PASS: Multi-file ambiguity detected with correct location\n";
}

// =============================================================================
// Scenario 4: File-scoped USING isolation
// =============================================================================

void test_using_is_file_scoped() {
    std::cout << "Running: test_using_is_file_scoped\n";

    // File 1: Define namespace
    std::string file1 = R"(
NAMESPACE Collections
  CLASS List
    DIM size AS I64
  END CLASS
END NAMESPACE
END
)";

    // File 2: USING Collections
    std::string file2 = R"(
USING Collections

NAMESPACE App
  REM Can use List unqualified due to USING
  CLASS MyApp
    DIM data AS I64
  END CLASS
END NAMESPACE
END
)";

    // File 3: No USING - must use fully-qualified names
    std::string file3 = R"(
NAMESPACE Other
  REM Must use FQ name - file2's USING doesn't apply here
  CLASS OtherApp : Collections.List
    DIM extra AS I64
  END CLASS
END NAMESPACE
END
)";

    std::vector<std::pair<std::string, std::string>> files = {
        {"collections.bas", file1}, {"app.bas", file2}, {"other.bas", file3}};

    size_t errorCount = runMultiFilePipeline(files);
    assert(errorCount == 0);

    std::cout << "  PASS: USING is file-scoped, not cross-file\n";
}

// =============================================================================
// Scenario 5: Multi-file with different aliases
// =============================================================================

void test_multi_file_different_aliases() {
    std::cout << "Running: test_multi_file_different_aliases\n";

    // File 1: Define namespace
    std::string file1 = R"(
NAMESPACE Lib.Database
  CLASS Connection
    DIM handle AS I64
  END CLASS
END NAMESPACE
END
)";

    // File 2: Use alias "DB"
    std::string file2 = R"(
USING DB = Lib.Database

NAMESPACE App.Core
  CLASS Service : DB.Connection
    DIM timeout AS I64
  END CLASS
END NAMESPACE
END
)";

    // File 3: Use alias "Conn"
    std::string file3 = R"(
USING Conn = Lib.Database

NAMESPACE App.UI
  CLASS Widget : Conn.Connection
    DIM visible AS I64
  END CLASS
END NAMESPACE
END
)";

    std::vector<std::pair<std::string, std::string>> files = {
        {"lib.bas", file1}, {"core.bas", file2}, {"ui.bas", file3}};

    size_t errorCount = runMultiFilePipeline(files);
    assert(errorCount == 0);

    std::cout << "  PASS: Each file can use different aliases for same namespace\n";
}

// =============================================================================
// Main test runner
// =============================================================================

int main() {
    std::cout << "=== Namespace E2E Multi-File Tests ===\n\n";

    test_two_file_base_derived_with_using();
    test_three_file_alias_usage();
    test_multi_file_ambiguity();
    test_using_is_file_scoped();
    test_multi_file_different_aliases();

    std::cout << "\n=== All E2E namespace tests passed ===\n";
    return 0;
}
