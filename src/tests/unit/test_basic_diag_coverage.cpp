//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_basic_diag_coverage.cpp
// Purpose: Drift guard — assert every BASIC diagnostic code (Bxxxx) emitted in
//          the frontend source has a catalog entry backing `zanna explain`.
// Key invariants:
//   - The frontend cannot emit a code that `zanna explain` does not understand.
// Ownership/Lifetime: Standalone unit test; reads frontend sources at runtime.
// Links: src/support/diag_catalog.hpp, src/support/diag_catalog.def
//
//===----------------------------------------------------------------------===//

#include "support/diag_catalog.hpp"
#include "tests/TestHarness.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace il::support;

#ifndef ZANNA_BASIC_FRONTEND_DIR
#define ZANNA_BASIC_FRONTEND_DIR ""
#endif

namespace {

/// @brief Collect every distinct "Bxxxx" diagnostic-code literal emitted across
///        the BASIC frontend's .cpp sources.
std::set<std::string> collectEmittedBasicCodes() {
    namespace fs = std::filesystem;
    std::set<std::string> codes;
    const std::string dir = ZANNA_BASIC_FRONTEND_DIR;
    if (dir.empty() || !fs::exists(dir))
        return codes;

    // Match a four-digit B-code inside a string literal, e.g. "B2107".
    const std::regex codeRe("\"(B[0-9]{4})\"");
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h")
            continue;
        std::ifstream in(entry.path());
        if (!in)
            continue;
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        for (auto it = std::sregex_iterator(text.begin(), text.end(), codeRe);
             it != std::sregex_iterator();
             ++it) {
            codes.insert((*it)[1].str());
        }
    }
    return codes;
}

} // namespace

/// @brief Every B-code the BASIC frontend emits must be cataloged for `zanna explain`.
TEST(BasicDiagCoverage, EveryEmittedCodeIsCataloged) {
    const auto codes = collectEmittedBasicCodes();
    // The source directory must be wired and contain emit sites; a zero count
    // would silently pass and defeat the guard.
    EXPECT_GT(codes.size(), 30u);

    std::set<std::string> missing;
    for (const auto &code : codes) {
        if (findDiagCode(code) == nullptr)
            missing.insert(code);
    }
    if (!missing.empty()) {
        std::cerr
            << "BASIC frontend emits diagnostic codes with no diag_catalog.def entry "
               "(zanna explain would report 'unknown code'). Add a DIAG_CODE(...) for each:\n";
        for (const auto &code : missing)
            std::cerr << "  - " << code << "\n";
    }
    EXPECT_TRUE(missing.empty());
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
