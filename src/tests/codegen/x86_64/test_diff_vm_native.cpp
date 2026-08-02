//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/codegen/x86_64/test_diff_vm_native.cpp
// Purpose: Ensure the ilc VM runner and x86-64 native backend produce identical
//          process results for representative IL programs.
// Key invariants:
//   - Each scenario runs the same IL through the VM and native CLI paths.
//   - Exit codes and stdout must match exactly unless a scenario opts into tolerance.
// Ownership/Lifetime:
//   - CodegenFixture owns all temporary files created for each scenario.
//   - Runtime objects allocated by scenario IL live until process teardown.
// Links: docs/internals/architecture.md, src/tests/common/CodegenFixture.hpp
//
//===----------------------------------------------------------------------===//

#include "common/CodegenFixture.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {
using zanna::tests::CodegenComparisonOptions;
using zanna::tests::CodegenComparisonResult;
using zanna::tests::CodegenFixture;
using zanna::tests::CodegenRunConfig;

struct CliScenario {
    const char *name;
    CodegenRunConfig config;
    CodegenComparisonOptions options;
};

const CliScenario kScenarios[] = {{"BranchPrint",
                                   {R"(il 0.3.0
extern @rt_print_i64(i64) -> void
extern @rt_print_f64(f64) -> void

func @main() -> i32 {
entry:
  %condition = scmp_gt 5, 3
  cbr %condition, greater, smaller
greater:
  call @rt_print_i64(42)
  call @rt_print_f64(3.5)
  br exit
smaller:
  call @rt_print_i64(0)
  call @rt_print_f64(0.0)
  br exit
exit:
  ret 7
}
)",
                                    "branch_print.il",
                                    {},
                                    {}},
                                   {false, std::nullopt}},
                                  {"BranchPrintSpecialChar",
                                   {R"(il 0.3.0
extern @rt_print_i64(i64) -> void
extern @rt_print_f64(f64) -> void

func @main() -> i32 {
entry:
  %condition = scmp_gt 5, 3
  cbr %condition, greater, smaller
greater:
  call @rt_print_i64(42)
  call @rt_print_f64(3.5)
  br exit
smaller:
  call @rt_print_i64(0)
  call @rt_print_f64(0.0)
  br exit
exit:
  ret 7
}
)",
                                    "branch_print$literal.il",
                                    {},
                                    {}},
                                   {false, std::nullopt}},
                                  {"Material3DAnisotropyRoundTrip",
                                   {R"(il 0.3.0
extern @Zanna.Graphics3D.Material3D.New() -> ptr
extern @Zanna.Graphics3D.Material3D.set_Anisotropy(ptr, i64) -> void
extern @Zanna.Graphics3D.Material3D.get_Anisotropy(ptr) -> i64

func @main() -> i64 {
entry:
  %mat = call @Zanna.Graphics3D.Material3D.New()
  %default = call @Zanna.Graphics3D.Material3D.get_Anisotropy(%mat)
  call @Zanna.Graphics3D.Material3D.set_Anisotropy(%mat, 0)
  %low = call @Zanna.Graphics3D.Material3D.get_Anisotropy(%mat)
  call @Zanna.Graphics3D.Material3D.set_Anisotropy(%mat, 64)
  %high = call @Zanna.Graphics3D.Material3D.get_Anisotropy(%mat)
  call @Zanna.Graphics3D.Material3D.set_Anisotropy(%mat, 8)
  %round = call @Zanna.Graphics3D.Material3D.get_Anisotropy(%mat)
  %a = iadd.ovf %default, %low
  %b = iadd.ovf %a, %high
  %sum = iadd.ovf %b, %round
  ret %sum
}
)",
                                    "material3d_anisotropy.il",
                                    {},
                                    {}},
                                   {false, std::nullopt}},
                                  {"SceneGraphHierarchyCounts",
                                   {R"(il 0.3.0
extern @Zanna.Graphics3D.SceneGraph.New() -> ptr
extern @Zanna.Graphics3D.SceneNode.New() -> ptr
extern @Zanna.Graphics3D.SceneGraph.Add(ptr, ptr) -> void
extern @Zanna.Graphics3D.SceneNode.AddChild(ptr, ptr) -> void
extern @Zanna.Graphics3D.SceneGraph.get_NodeCount(ptr) -> i64
extern @Zanna.Graphics3D.SceneGraph.get_UnresolvedPrefabCount(ptr) -> i64

func @main() -> i64 {
entry:
  %scene = call @Zanna.Graphics3D.SceneGraph.New()
  %parent = call @Zanna.Graphics3D.SceneNode.New()
  %child = call @Zanna.Graphics3D.SceneNode.New()
  call @Zanna.Graphics3D.SceneGraph.Add(%scene, %parent)
  call @Zanna.Graphics3D.SceneNode.AddChild(%parent, %child)
  %count = call @Zanna.Graphics3D.SceneGraph.get_NodeCount(%scene)
  %unresolved = call @Zanna.Graphics3D.SceneGraph.get_UnresolvedPrefabCount(%scene)
  %sum = iadd.ovf %count, %unresolved
  ret %sum
}
)",
                                    "scenegraph_hierarchy_counts.il",
                                    {},
                                    {}},
                                   {false, std::nullopt}},
                                  {"QuatEulerDecompositionParity",
                                   {R"(il 0.3.0
extern @Zanna.Math.Quat.FromEuler(f64, f64, f64) -> ptr
extern @Zanna.Math.Quat.ToEuler(ptr) -> ptr
extern @Zanna.Math.Vec3.get_X(ptr) -> f64
extern @Zanna.Math.Vec3.get_Y(ptr) -> f64
extern @Zanna.Math.Vec3.get_Z(ptr) -> f64
extern @rt_print_f64(f64) -> void

func @main() -> i64 {
entry:
  %quat = call @Zanna.Math.Quat.FromEuler(0.3, -0.7, 1.1)
  %euler = call @Zanna.Math.Quat.ToEuler(%quat)
  %pitch = call @Zanna.Math.Vec3.get_X(%euler)
  %yaw = call @Zanna.Math.Vec3.get_Y(%euler)
  %roll = call @Zanna.Math.Vec3.get_Z(%euler)
  call @rt_print_f64(%pitch)
  call @rt_print_f64(%yaw)
  call @rt_print_f64(%roll)
  ret 0
}
)",
                                    "quat_euler_decomposition.il",
                                    {},
                                    {}},
                                   {false, std::nullopt}}};

CodegenComparisonResult runScenario(CodegenFixture &fixture, const CliScenario &scenario) {
    return fixture.compareVmAndNative(scenario.config, scenario.options);
}

} // namespace

int main() {
    CodegenFixture fixture;
    if (!fixture.isReady()) {
        std::cerr << fixture.setupError();
        return EXIT_FAILURE;
    }

    for (const CliScenario &scenario : kScenarios) {
        const CodegenComparisonResult result = runScenario(fixture, scenario);
        if (!result.success) {
            std::cerr << result.message;
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
