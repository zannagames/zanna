//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_runtime_result_ownership.cpp
// Purpose: Enforce ADR 0314: every registered runtime descriptor that returns
//          a managed reference (obj, obj<...>, seq<...>, str) declares whether
//          the caller owns the result, and the declaration reaches the parsed
//          signature the lowerer consults.
// Key invariants:
//   - No reference-returning descriptor is left Unspecified.
//   - Declared `borrowed` clears returnsOwned even when a name pattern in the
//     ownership catalog would have claimed the result.
// Ownership/Lifetime:
//   - Read-only walk over the process-wide runtime registry.
// Links: src/il/runtime/RuntimeSignatures.hpp, docs/adr/0314-declared-runtime-result-ownership.md
//
//===----------------------------------------------------------------------===//

#include "il/runtime/RuntimeSignatures.hpp"
#include "tests/TestHarness.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using il::runtime::RuntimeResultOwnership;

bool returnsManagedReference(std::string_view spec) {
    return spec.rfind("obj", 0) == 0 || spec.rfind("seq", 0) == 0 || spec.rfind("str", 0) == 0;
}

TEST(RuntimeResultOwnership, EveryReferenceResultIsDeclared) {
    std::vector<std::string> undeclared;
    std::size_t declared = 0;
    for (const auto &descriptor : il::runtime::runtimeRegistry()) {
        if (!descriptor.publicSurface || descriptor.cSymbol.empty())
            continue; // hand-authored internal rows carry no runtime.def declaration
        if (!returnsManagedReference(descriptor.signatureText))
            continue;
        if (descriptor.signature.resultOwnership == RuntimeResultOwnership::Unspecified)
            undeclared.emplace_back(descriptor.name);
        else
            ++declared;
    }
    EXPECT_TRUE(declared > 2000);
    for (const auto &name : undeclared)
        EXPECT_TRUE(false && name.c_str());
    EXPECT_EQ(undeclared.size(), static_cast<std::size_t>(0));
}

TEST(RuntimeResultOwnership, DeclarationReachesTheParsedSignature) {
    const auto *box = il::runtime::findRuntimeDescriptor("Zanna.Graphics3D.Mesh3D.Box");
    ASSERT_TRUE(box != nullptr);
    EXPECT_TRUE(box->signature.resultOwnership == RuntimeResultOwnership::Owned);
    EXPECT_TRUE(box->signature.returnsOwned);

    const auto *getMesh = il::runtime::findRuntimeDescriptor("Zanna.Graphics3D.SceneAsset.GetMesh");
    ASSERT_TRUE(getMesh != nullptr);
    EXPECT_TRUE(getMesh->signature.resultOwnership == RuntimeResultOwnership::Borrowed);
    EXPECT_FALSE(getMesh->signature.returnsOwned);

    // The entity getters document "the validated retained pointer" but return
    // the entity's slot without a retain of their own.
    const auto *entityMesh = il::runtime::findRuntimeDescriptor("Zanna.Game3D.Entity3D.get_Mesh");
    ASSERT_TRUE(entityMesh != nullptr);
    EXPECT_FALSE(entityMesh->signature.returnsOwned);

    // `.From` in the name used to make this receiver-returning method "owned".
    const auto *detach = il::runtime::findRuntimeDescriptor("Zanna.Game3D.Entity3D.DetachFromBone");
    ASSERT_TRUE(detach != nullptr);
    EXPECT_FALSE(detach->signature.returnsOwned);

    const auto *unwrapStr = il::runtime::findRuntimeDescriptor("Zanna.Result.UnwrapStr");
    ASSERT_TRUE(unwrapStr != nullptr);
    EXPECT_TRUE(unwrapStr->signature.resultOwnership == RuntimeResultOwnership::Borrowed);

    const auto *split = il::runtime::findRuntimeDescriptor("Zanna.String.Split");
    ASSERT_TRUE(split != nullptr);
    EXPECT_TRUE(split->signature.resultOwnership == RuntimeResultOwnership::Owned);
    EXPECT_TRUE(split->signature.returnsOwned);
}

} // namespace

int main() {
    return zanna_test::run_all_tests();
}
