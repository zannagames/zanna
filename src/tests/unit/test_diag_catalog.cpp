//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_diag_catalog.cpp
// Purpose: Unit tests for the diagnostic-code catalog backing `zanna explain`.
// Key invariants:
//   - Catalog entries are unique, non-empty, and cover the core code families.
//   - Prefix-family fallback resolves every code family used in the tree.
//   - BASIC semantic codes are summarized by what they actually report.
// Ownership/Lifetime:
//   - Test-only file
// Links: support/diag_catalog.hpp
//
//===----------------------------------------------------------------------===//

#include "support/diag_catalog.hpp"
#include "tests/TestHarness.hpp"

#include <set>
#include <string>

using namespace il::support;

TEST(DiagCatalog, EntriesAreUniqueAndWellFormed) {
    const auto &entries = diagCatalog();
    EXPECT_GT(entries.size(), 100u);
    EXPECT_EQ(diagCatalogEntries().size(), entries.size());

    std::set<std::string> seen;
    for (const auto &e : entries) {
        EXPECT_TRUE(!e.code.empty());
        EXPECT_TRUE(!e.subsystem.empty());
        EXPECT_TRUE(!e.summary.empty());
        auto inserted = seen.insert(std::string(e.code));
        EXPECT_TRUE(inserted.second); // no duplicate codes
    }
}

TEST(DiagCatalog, FindsCoreCodes) {
    const auto *undef = findDiagCode("V-ZIA-UNDEFINED");
    EXPECT_TRUE(undef != nullptr);
    if (undef)
        EXPECT_EQ(std::string(undef->subsystem), std::string("zia-sema"));

    EXPECT_TRUE(findDiagCode("V-ZIA-TYPE-MISMATCH") != nullptr);
    EXPECT_TRUE(findDiagCode("W001") != nullptr);
    EXPECT_TRUE(findDiagCode("W019") != nullptr);
    EXPECT_TRUE(findDiagCode("B2001") != nullptr);
    EXPECT_TRUE(findDiagCode("V-IL-VERIFY") != nullptr);
    EXPECT_TRUE(findDiagCode("V-BC-UNSUPPORTED-OP") != nullptr);
}

TEST(DiagCatalog, BasicSemaCodesDescribeWhatTheyReport) {
    /// Summary text for @p code, or empty when the code is not cataloged.
    auto summary = [](const char *code) {
        const auto *entry = findDiagCode(code);
        return entry ? std::string(entry->summary) : std::string{};
    };
    EXPECT_EQ(summary("B1003"), std::string("GOTO, GOSUB, or RESUME names an unknown label"));
    EXPECT_EQ(summary("B1006"), std::string("Call to an unknown procedure"));
    EXPECT_EQ(summary("B1013"), std::string("Local name is already declared in this scope"));
}

TEST(DiagCatalog, UnknownCodeReturnsNull) {
    EXPECT_TRUE(findDiagCode("TOTALLY-BOGUS") == nullptr);
    EXPECT_TRUE(findDiagCode("") == nullptr);
}

TEST(DiagCatalog, FamilyFallbackCoversKnownPrefixes) {
    EXPECT_TRUE(diagCodeFamily("V-ZIA-SOMETHING-NEW").has_value());
    EXPECT_TRUE(diagCodeFamily("V-ZIA-PARSE-FUTURE").has_value());
    EXPECT_TRUE(diagCodeFamily("V-BC-FUTURE").has_value());
    EXPECT_TRUE(diagCodeFamily("V-CG-FUTURE").has_value());
    EXPECT_TRUE(diagCodeFamily("V-CLI-WARNING").has_value());
    EXPECT_TRUE(diagCodeFamily("V-IL-IO").has_value());
    EXPECT_TRUE(diagCodeFamily("IL001").has_value());
    EXPECT_TRUE(diagCodeFamily("B2113").has_value());
    EXPECT_TRUE(diagCodeFamily("B9007").has_value());
    EXPECT_TRUE(diagCodeFamily("W042").has_value());
    EXPECT_TRUE(diagCodeFamily("E1009").has_value());
    EXPECT_FALSE(diagCodeFamily("XYZZY").has_value());
    EXPECT_FALSE(diagCodeFamily("B12X4").has_value());
    EXPECT_FALSE(diagCodeFamily("ILLEGAL").has_value());
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
