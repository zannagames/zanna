//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_graphics3d_runtime_manifest.cpp
// Purpose: Guard the complete Graphics3D/Game3D registry-to-C ABI manifest.
// Key invariants:
//   - Every public 3D function has a valid signature, handler, and C symbol.
//   - Every 3D class binding resolves to a public runtime descriptor.
//   - The deterministic manifest fingerprint changes for any ABI surface drift.
// Ownership/Lifetime:
//   - Reads immutable process-lifetime runtime registries without retaining data.
// Links: docs/adr/0102-graphics3d-runtime-boundary-and-contract-manifest.md,
//        docs/adr/0157-material-texture-pixel-inspection.md,
//        docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md,
//        docs/adr/0161-stable-scenenode-sibling-reordering.md,
//        docs/adr/0162-exact-preserve-world-scenenode-reparenting.md,
//        docs/adr/0166-exact-scenenode-world-matrix-assignment.md,
//        docs/adr/0168-windowless-canvas3d-rendering.md,
//        docs/adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md,
//        docs/adr/0210-read-only-mesh-vertex-positions.md
//
//===----------------------------------------------------------------------===//

#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <unordered_set>

namespace {

/* Animation3D.StripRootMotion (2026-08-05) and Animation3D.ExtractRange
 * (2026-08-06, broadcast action-core trimming): +1 function/+1 method each.
 * Canvas3D.SetCaptureAfterPresent + get_CaptureAfterPresent (2026-08-06,
 * E3): +2 functions / +1 method / +1 property.
 * Canvas3D.DrawText2DTtf + MeasureText2DTtf (2026-08-07, E1 font bridge):
 * +2 functions / +2 methods.
 * Animation3D.Mirror (2026-08-07, ADR 0243 L/R clip mirroring):
 * +1 function / +1 method.
 * World3D.ClearFog (2026-08-13, ADR 0248 fog argument-order alignment):
 * +1 function / +1 method.
 * Mesh3D.RasterizeUvMaskY (2026-08-15, body-zone texture masking for
 * per-region character recolors): +1 function / +1 method.
 * Mesh3D.BoundsMin/BoundsMax/BoundsCenter/BoundsSize/BoundsRadius
 * (2026-08-16, ADR 0252 bounds readback — the runtime already maintained
 * this AABB for culling but never exposed it, so every app re-scanned the
 * vertex buffer by hand): +5 functions / +5 properties.
 * Mesh3D.Append (2026-08-16, ADR 0252 geometry merge — the runtime could
 * build and transform a mesh but not put one mesh's triangles into another,
 * so batching parts into a single draw call meant re-emitting every vertex
 * by hand): +1 function / +1 method. */
constexpr std::size_t kExpectedFunctionCount = 2249;
constexpr std::size_t kExpectedClassCount = 131;
constexpr std::size_t kExpectedPropertyCount = 826;
constexpr std::size_t kExpectedMethodCount = 1210;

bool is3DName(std::string_view name) {
    return name.starts_with("Zanna.Graphics3D.") || name.starts_with("Zanna.Game3D.");
}

class ManifestHash {
  public:
    void addByte(std::uint8_t byte) {
        value_ ^= byte;
        value_ *= UINT64_C(1099511628211);
    }

    void addUnsigned(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            addByte(static_cast<std::uint8_t>(value >> shift));
    }

    void addString(std::string_view value) {
        addUnsigned(value.size());
        for (unsigned char byte : value)
            addByte(byte);
    }

    void addNullable(const char *value) {
        addByte(value != nullptr ? 1U : 0U);
        if (value != nullptr)
            addString(value);
    }

    [[nodiscard]] std::uint64_t value() const {
        return value_;
    }

  private:
    std::uint64_t value_{UINT64_C(14695981039346656037)};
};

bool require(bool condition, std::string_view message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool checkTarget(std::string_view target) {
    const il::runtime::RuntimeDescriptor *descriptor = il::runtime::findRuntimeDescriptor(target);
    bool ok = require(descriptor != nullptr, "3D class binding target is not registered");
    if (descriptor == nullptr)
        return false;
    ok = require(descriptor->publicSurface, "3D class binding target is not public") && ok;
    ok = require(!descriptor->cSymbol.empty(), "3D class binding target has no C symbol") && ok;
    return ok;
}

} // namespace

int main() {
    ManifestHash hash;
    hash.addString("zanna-graphics3d-abi-manifest-v1");

    std::size_t functionCount = 0;
    std::unordered_set<std::string_view> functionNames;
    bool ok = true;
    for (const il::runtime::RuntimeDescriptor &descriptor : il::runtime::runtimeRegistry()) {
        if (!descriptor.publicSurface || !is3DName(descriptor.name))
            continue;

        ++functionCount;
        ok = require(functionNames.insert(descriptor.name).second,
                     "duplicate public 3D runtime function") &&
             ok;
        ok = require(!descriptor.cSymbol.empty(), "public 3D function has no C symbol") && ok;
        ok =
            require(!descriptor.signatureText.empty(), "public 3D function has no signature") && ok;
        ok = require(descriptor.signature.valid, "public 3D function has an invalid signature") &&
             ok;
        ok = require(descriptor.handler != nullptr, "public 3D function has no VM handler") && ok;

        hash.addString(descriptor.name);
        hash.addString(descriptor.signatureText);
        hash.addString(descriptor.cSymbol);
    }
    for (std::string_view textureGetter :
         {"Zanna.Graphics3D.Material3D.get_TexturePixels",
          "Zanna.Graphics3D.Material3D.get_NormalMapPixels",
          "Zanna.Graphics3D.Material3D.get_SpecularMapPixels",
          "Zanna.Graphics3D.Material3D.get_EmissiveMapPixels",
          "Zanna.Graphics3D.Material3D.get_MetallicRoughnessMapPixels",
          "Zanna.Graphics3D.Material3D.get_AmbientOcclusionMapPixels",
          "Zanna.Graphics3D.Material3D.get_LightmapPixels"}) {
        ok = require(functionNames.contains(textureGetter),
                     "reviewed material texture inspection getter is missing") &&
             ok;
    }
    ok = require(functionNames.contains("Zanna.Graphics3D.SceneNode.TrySetWorldMatrix"),
                 "reviewed exact SceneNode world-matrix assignment is missing") &&
         ok;
    ok = require(functionNames.contains("Zanna.Graphics3D.Canvas3D.NewOffscreen"),
                 "reviewed windowless Canvas3D constructor is missing") &&
         ok;
    ok = require(functionNames.contains("Zanna.Graphics3D.Canvas3D.get_IsOffscreen"),
                 "reviewed Canvas3D offscreen query is missing") &&
         ok;
    ok = require(functionNames.contains("Zanna.Graphics3D.SceneGraph.SaveToText"),
                 "reviewed in-memory scene serialization (ADR 0190) is missing") &&
         ok;
    ok = require(functionNames.contains("Zanna.Graphics3D.SceneAsset.LoadTextResult"),
                 "reviewed in-memory scene text loading (ADR 0190) is missing") &&
         ok;
    ok = require(functionNames.contains("Zanna.Graphics3D.Canvas3D.NewOffscreenAccelerated"),
                 "reviewed accelerated offscreen constructor (ADR 0191) is missing") &&
         ok;
    ok = require(functionNames.contains("Zanna.Graphics3D.Mesh3D.VertexPosition"),
                 "reviewed read-only mesh vertex query (ADR 0210) is missing") &&
         ok;

    std::size_t classCount = 0;
    std::size_t propertyCount = 0;
    std::size_t methodCount = 0;
    for (const il::runtime::RuntimeClass &runtimeClass : il::runtime::runtimeClassCatalog()) {
        if (runtimeClass.qname == nullptr || !is3DName(runtimeClass.qname))
            continue;

        ++classCount;
        hash.addString(runtimeClass.qname);
        hash.addNullable(runtimeClass.layout);
        hash.addNullable(runtimeClass.ctor);
        if (runtimeClass.ctor != nullptr && *runtimeClass.ctor != '\0')
            ok = checkTarget(runtimeClass.ctor) && ok;

        for (const il::runtime::RuntimeProperty &property : runtimeClass.properties) {
            ++propertyCount;
            hash.addNullable(property.name);
            hash.addNullable(property.type);
            hash.addByte(property.readonly ? 1U : 0U);
            hash.addNullable(property.getter);
            hash.addNullable(property.setter);
            const bool hasGetter = property.getter != nullptr && *property.getter != '\0';
            const bool hasSetter = property.setter != nullptr && *property.setter != '\0';
            ok = require(hasGetter || hasSetter, "3D property has no accessor") && ok;
            if (hasGetter)
                ok = checkTarget(property.getter) && ok;
            if (hasSetter)
                ok = checkTarget(property.setter) && ok;
        }

        for (const il::runtime::RuntimeMethod &method : runtimeClass.methods) {
            ++methodCount;
            hash.addNullable(method.name);
            hash.addNullable(method.signature);
            hash.addNullable(method.target);
            ok = require(method.target != nullptr, "3D method has no target") && ok;
            if (method.target != nullptr && *method.target != '\0')
                ok = checkTarget(method.target) && ok;
        }
    }

    /* On mismatch, print actual vs expected so the deliberate-review update is
     * a copy-paste instead of a guessing game. */
    if (functionCount != kExpectedFunctionCount || classCount != kExpectedClassCount ||
        propertyCount != kExpectedPropertyCount || methodCount != kExpectedMethodCount) {
        std::cerr << "MANIFEST ACTUALS: functions=" << functionCount << " classes=" << classCount
                  << " properties=" << propertyCount << " methods=" << methodCount << '\n';
    }
    ok = require(functionCount == kExpectedFunctionCount,
                 "public 3D function count changed; review and update the ABI manifest") &&
         ok;
    ok = require(classCount == kExpectedClassCount,
                 "public 3D class count changed; review and update the ABI manifest") &&
         ok;
    ok = require(propertyCount == kExpectedPropertyCount,
                 "public 3D property count changed; review and update the ABI manifest") &&
         ok;
    ok = require(methodCount == kExpectedMethodCount,
                 "public 3D method count changed; review and update the ABI manifest") &&
         ok;

    // Filled from the canonical registry after deliberate ABI review. This one value
    // covers every function name/signature/C symbol and every class member binding.
    /* Rehashed 2026-08-06: Animation3D.ExtractRange, then the
     * SceneTemplate.Instantiate(SceneAt) typed method returns
     * (obj -> obj<Entity3D>; the untyped method-table form made Zia type
     * instantiated entities as SceneTemplate — the ZB-1 dual-registry
     * drift pattern), then Canvas3D.SetCaptureAfterPresent /
     * get_CaptureAfterPresent (E3 capture hardening: opt-in pre-present
     * blit so post-Present readback sees the shown frame on GPU
     * direct-present paths), then Animation3D.Mirror (ADR 0243 L/R clip
     * mirroring).
     * Rehashed 2026-08-13: World3D.ClearFog added and World3D.SetFog's
     * parameter meaning realigned to Canvas3D's (near, far, r, g, b) —
     * ADR 0248 / ZB-22.
     * Rehashed 2026-08-15: Mesh3D.RasterizeUvMaskY added (bind-pose
     * Y-band UV coverage into a Pixels mask — body-zone texture masking
     * for per-region character recolors, plan 55 uniforms). */
    constexpr std::uint64_t kExpectedManifestHash = UINT64_C(0x13e1a31028da0a8f);
    if (hash.value() != kExpectedManifestHash) {
        std::cerr << "FAIL: 3D ABI manifest changed; reviewed hash is 0x" << std::hex
                  << hash.value() << '\n';
        ok = false;
    }

    return ok ? 0 : 1;
}
