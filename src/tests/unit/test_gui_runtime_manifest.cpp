//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_gui_runtime_manifest.cpp
// Purpose: Guard the complete Zanna.GUI registry-to-C ABI manifest.
//
// Key invariants:
//   - Every public GUI function has a valid signature, handler, and C symbol.
//   - Every GUI class constructor, property, and method target resolves publicly.
//   - Counts and a deterministic fingerprint change for any GUI ABI drift.
//
// Ownership/Lifetime:
//   - Reads immutable process-lifetime runtime registries and retains no values.
//
// Links: docs/adr/0106-gui-runtime-lifetime-contract-and-coordinate-policy.md,
//        docs/adr/0150-gui-native-minimum-window-size.md,
//        docs/adr/0156-listbox-selected-row-data.md,
//        docs/adr/0163-stable-multiselect-and-row-aware-treeview-editing.md,
//        docs/adr/0165-scrollview-descendant-reveal.md,
//        docs/adr/0167-spinner-mixed-value-state.md,
//        docs/adr/0205-listbox-application-directed-reordering.md,
//        docs/adr/0258-undoable-state-preserving-full-document-replacement.md,
//        src/tools/zanna/main.cpp
//
//===----------------------------------------------------------------------===//

#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <unordered_set>

namespace {

constexpr std::size_t kExpectedFunctionCount = 1163;
constexpr std::size_t kExpectedClassCount = 79;
constexpr std::size_t kExpectedPropertyCount = 110;
constexpr std::size_t kExpectedMethodCount = 1054;

/// @brief Test whether a canonical runtime name belongs to the GUI boundary.
/// @param name Function or class name from the live runtime registry.
/// @return True only for names rooted at `Zanna.GUI.`.
bool isGuiName(std::string_view name) {
    return name.starts_with("Zanna.GUI.");
}

/// @brief Deterministic length-delimited FNV-1a accumulator for manifest fields.
/// @details Length-prefixing prevents adjacent fields from creating ambiguous byte streams. All
///          integer values are encoded little-endian so the expected hash is host-independent.
class ManifestHash {
  public:
    /// @brief Add one raw byte to the manifest hash.
    /// @param byte Byte to mix into the FNV-1a state.
    void addByte(std::uint8_t byte) {
        value_ ^= byte;
        value_ *= UINT64_C(1099511628211);
    }

    /// @brief Add one unsigned integer using a fixed eight-byte encoding.
    /// @param value Integer to encode and mix.
    void addUnsigned(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            addByte(static_cast<std::uint8_t>(value >> shift));
    }

    /// @brief Add a byte-exact string preceded by its length.
    /// @param value Registry text to mix.
    void addString(std::string_view value) {
        addUnsigned(value.size());
        for (unsigned char byte : value)
            addByte(byte);
    }

    /// @brief Add a nullable C string while distinguishing null from empty.
    /// @param value Optional registry field.
    void addNullable(const char *value) {
        addByte(value != nullptr ? 1U : 0U);
        if (value != nullptr)
            addString(value);
    }

    /// @brief Read the current manifest fingerprint.
    /// @return Platform-independent 64-bit hash value.
    [[nodiscard]] std::uint64_t value() const {
        return value_;
    }

  private:
    std::uint64_t value_{UINT64_C(14695981039346656037)}; ///< Current FNV-1a state.
};

/// @brief Report one failed manifest predicate without aborting later diagnostics.
/// @param condition Predicate result.
/// @param message Human-readable failure context.
/// @return The original predicate result.
bool require(bool condition, std::string_view message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

/// @brief Validate that one class binding target resolves to a callable public descriptor.
/// @param target Canonical target name stored in a constructor, property, or method binding.
/// @return True when the target exists, is public, and has a C symbol.
bool checkTarget(std::string_view target) {
    const il::runtime::RuntimeDescriptor *descriptor = il::runtime::findRuntimeDescriptor(target);
    bool ok = require(descriptor != nullptr, "GUI class binding target is not registered");
    if (descriptor == nullptr)
        return false;
    ok = require(descriptor->publicSurface, "GUI class binding target is not public") && ok;
    ok = require(!descriptor->cSymbol.empty(), "GUI class binding target has no C symbol") && ok;
    return ok;
}

/// @brief Check one deliberately reviewed public GUI function contract.
/// @param name Canonical registry name.
/// @param signature Expected registry signature.
/// @param symbol Expected native C symbol.
/// @return True when the complete contract matches.
bool checkFunctionContract(std::string_view name,
                           std::string_view signature,
                           std::string_view symbol) {
    const il::runtime::RuntimeDescriptor *descriptor = il::runtime::findRuntimeDescriptor(name);
    bool ok = require(descriptor != nullptr, "reviewed GUI function is not registered");
    if (descriptor == nullptr)
        return false;
    ok = require(descriptor->publicSurface, "reviewed GUI function is not public") && ok;
    ok = require(descriptor->signatureText == signature,
                 "reviewed GUI function signature changed") &&
         ok;
    ok = require(descriptor->cSymbol == symbol, "reviewed GUI C symbol changed") && ok;
    return ok;
}

} // namespace

/// @brief Validate and fingerprint the complete public GUI registry surface.
/// @return Zero when counts, targets, uniqueness, and the reviewed hash match; otherwise one.
int main() {
    ManifestHash hash;
    hash.addString("zanna-gui-abi-manifest-v1");

    std::size_t functionCount = 0;
    std::unordered_set<std::string_view> functionNames;
    bool ok = true;
    for (const il::runtime::RuntimeDescriptor &descriptor : il::runtime::runtimeRegistry()) {
        if (!descriptor.publicSurface || !isGuiName(descriptor.name))
            continue;

        ++functionCount;
        ok = require(functionNames.insert(descriptor.name).second,
                     "duplicate public GUI runtime function") &&
             ok;
        ok = require(!descriptor.cSymbol.empty(), "public GUI function has no C symbol") && ok;
        ok = require(!descriptor.signatureText.empty(), "public GUI function has no signature") &&
             ok;
        ok = require(descriptor.signature.valid, "public GUI function has an invalid signature") &&
             ok;
        ok = require(descriptor.handler != nullptr, "public GUI function has no VM handler") && ok;

        hash.addString(descriptor.name);
        hash.addString(descriptor.signatureText);
        hash.addString(descriptor.cSymbol);
    }

    std::size_t classCount = 0;
    std::size_t propertyCount = 0;
    std::size_t methodCount = 0;
    std::unordered_set<std::string_view> classNames;
    for (const il::runtime::RuntimeClass &runtimeClass : il::runtime::runtimeClassCatalog()) {
        if (runtimeClass.qname == nullptr || !isGuiName(runtimeClass.qname))
            continue;

        ++classCount;
        ok = require(classNames.insert(runtimeClass.qname).second,
                     "duplicate public GUI runtime class") &&
             ok;
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
            ok = require(hasGetter || hasSetter, "GUI property has no accessor") && ok;
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
            ok = require(method.target != nullptr, "GUI method has no target") && ok;
            if (method.target != nullptr && *method.target != '\0')
                ok = checkTarget(method.target) && ok;
        }
    }

    if (functionCount != kExpectedFunctionCount || classCount != kExpectedClassCount ||
        propertyCount != kExpectedPropertyCount || methodCount != kExpectedMethodCount) {
        std::cerr << "MANIFEST ACTUALS: functions=" << functionCount << " classes=" << classCount
                  << " properties=" << propertyCount << " methods=" << methodCount << '\n';
    }
    ok = require(functionCount == kExpectedFunctionCount,
                 "public GUI function count changed; review and update the ABI manifest") &&
         ok;
    ok = require(classCount == kExpectedClassCount,
                 "public GUI class count changed; review and update the ABI manifest") &&
         ok;
    ok = require(propertyCount == kExpectedPropertyCount,
                 "public GUI property count changed; review and update the ABI manifest") &&
         ok;
    ok = require(methodCount == kExpectedMethodCount,
                 "public GUI method count changed; review and update the ABI manifest") &&
         ok;

    ok = checkFunctionContract("Zanna.GUI.CodeEditor.ReplaceAllText",
                               "i1(obj, string)",
                               "rt_codeeditor_replace_all_text") &&
         ok;
    ok = checkFunctionContract("Zanna.GUI.EditorBuffer.ReplaceAllText",
                               "i1(obj, string)",
                               "rt_editorbuffer_replace_all_text") &&
         ok;

    // Set after deliberate review of every registry row. Any future mismatch prints the new value
    // and requires an explicit count/signature/class-binding review before this constant changes.
    constexpr std::uint64_t kExpectedManifestHash = UINT64_C(0x127601512fcaf7dc);
    if (hash.value() != kExpectedManifestHash) {
        std::cerr << "FAIL: GUI ABI manifest changed; reviewed hash is 0x" << std::hex
                  << hash.value() << '\n';
        ok = false;
    }

    return ok ? 0 : 1;
}
