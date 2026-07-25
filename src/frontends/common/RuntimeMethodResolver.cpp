//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/RuntimeMethodResolver.cpp
// Purpose: Implement shared runtime method lookup and overload resolution.
// Key invariants: Runtime method metadata is read from RuntimeRegistry only;
//                 no frontend keeps a parallel parsed-signature catalog.
// Ownership/Lifetime: Lookup results own converted strings and vectors, while
//                     raw catalog data remains owned by RuntimeRegistry.
// Links: src/frontends/common/RuntimeMethodResolver.hpp,
//        src/il/runtime/classes/RuntimeClasses.cpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements shared runtime method lookup and compatibility ranking.
//
//===----------------------------------------------------------------------===//

#include "frontends/common/RuntimeMethodResolver.hpp"

#include "frontends/common/StringUtils.hpp"
#include "il/runtime/RuntimeSignatures.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace il::frontends::common {
namespace {

/**
 * @brief Build a frontend-neutral method descriptor from a parsed runtime method.
 *
 * @details This function normalizes legacy target names by checking whether the
 * parsed target exists as-is and falling back to `class.method` when the
 * catalog stores a short target. It also derives receiver expectations from the
 * runtime descriptor's physical parameter count.
 *
 * @param classQName Runtime class that owns the method.
 * @param method Method name from the runtime catalog.
 * @param parsed Parsed method signature from RuntimeRegistry.
 * @return Fully populated method descriptor.
 */
RuntimeMethodInfo makeRuntimeMethodInfo(std::string_view classQName,
                                        std::string_view method,
                                        const il::runtime::ParsedMethod &parsed) {
    RuntimeMethodInfo info;

    info.target = parsed.target ? parsed.target : "";
    if (!info.target.empty() && !il::runtime::findRuntimeDescriptor(info.target)) {
        std::string qualifiedTarget = std::string(classQName) + "." + std::string(method);
        if (il::runtime::findRuntimeDescriptor(qualifiedTarget)) {
            info.target = std::move(qualifiedTarget);
        }
    }

    info.ret = parsed.signature.returnType;
    info.returnClassQName = il::runtime::concreteRuntimeReturnClassQName(parsed.signature);
    info.rawPointerReturn = parsed.signature.rawPointerReturn;
    info.rawPointerParams = parsed.signature.rawPointerParams;
    info.args = parsed.signature.params;

    if (const auto *descriptor = il::runtime::findRuntimeDescriptor(info.target)) {
        const auto runtimeParamCount = descriptor->signature.paramTypes.size();
        const auto explicitParamCount = parsed.signature.params.size();
        if (runtimeParamCount == explicitParamCount) {
            info.hasReceiver = false;
        } else if (runtimeParamCount == explicitParamCount + 1U) {
            info.hasReceiver = true;
        }
    }

    return info;
}

/**
 * @brief Score one actual argument type against one expected runtime parameter.
 *
 * @details Lower scores are better. A missing optional return indicates the
 * argument cannot be passed to that parameter without violating existing
 * frontend compatibility rules.
 *
 * @param expected Runtime parameter type.
 * @param actual Source argument type after frontend conversion to IL terms.
 * @return Compatibility score, or std::nullopt for an incompatible pair.
 */
std::optional<int> scoreArgMatch(il::runtime::ILScalarType expected,
                                 il::runtime::ILScalarType actual) {
    if (expected == actual) {
        return 0;
    }
    if (expected == il::runtime::ILScalarType::Object) {
        if (actual == il::runtime::ILScalarType::Void) {
            return std::nullopt;
        }
        return actual == il::runtime::ILScalarType::Unknown ? 2 : 1;
    }
    if (actual == il::runtime::ILScalarType::Unknown) {
        return 2;
    }
    if (expected == il::runtime::ILScalarType::F64 && actual == il::runtime::ILScalarType::I64) {
        return 1;
    }
    if (expected == il::runtime::ILScalarType::Bool && actual == il::runtime::ILScalarType::I64) {
        return 1;
    }
    return std::nullopt;
}

/**
 * @brief Score a complete runtime signature against actual argument types.
 *
 * @details The signature must have the same explicit arity as the call. The
 * total score is the sum of parameter compatibility scores.
 *
 * @param expected Runtime parameter types.
 * @param actual Source argument types converted to IL terms.
 * @return Total compatibility score, or std::nullopt when any argument fails.
 */
std::optional<int> scoreSignature(const std::vector<il::runtime::ILScalarType> &expected,
                                  const std::vector<il::runtime::ILScalarType> &actual) {
    if (expected.size() != actual.size()) {
        return std::nullopt;
    }

    int score = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        auto argScore = scoreArgMatch(expected[i], actual[i]);
        if (!argScore) {
            return std::nullopt;
        }
        score += *argScore;
    }
    return score;
}

/**
 * @brief Find a runtime class by qualified name without regard to case.
 *
 * @param classQName Qualified class name to search for.
 * @return Borrowed catalog entry, or nullptr when the class is absent.
 */
const il::runtime::RuntimeClass *findRuntimeClass(std::string_view classQName) {
    const auto &catalog = il::runtime::RuntimeRegistry::instance().rawCatalog();
    for (const auto &klass : catalog) {
        if (klass.qname && string_utils::iequals(klass.qname, classQName))
            return &klass;
    }
    return nullptr;
}

/**
 * @brief Outcome of type-directed lookup within one runtime class.
 * @details NotFound means no viable signature, Found identifies one uniquely
 * best signature, and Ambiguous records a tie at the best score.
 */
enum class TypedLookupStatus { NotFound, Found, Ambiguous };

/**
 * @brief Internal type-directed lookup result with explicit ambiguity state.
 */
struct TypedLookupResult {
    /// @brief Lookup outcome.
    TypedLookupStatus status{TypedLookupStatus::NotFound};
    /// @brief Selected method when status is Found.
    std::optional<RuntimeMethodInfo> method;
};

/**
 * @brief Select the best overload declared directly by one runtime class.
 *
 * @details Candidate signatures must have matching arity. Compatible arguments
 * are scored with scoreSignature(); equal best scores produce Ambiguous rather
 * than relying on catalog order.
 *
 * @param klass Runtime class whose direct methods are examined.
 * @param method Case-insensitive method name.
 * @param argTypes Explicit call argument types.
 * @return Found, NotFound, or Ambiguous result for this class only.
 */
TypedLookupResult findTypedInClass(
    const il::runtime::RuntimeClass &klass,
    std::string_view method,
    const std::vector<il::runtime::ILScalarType> &argTypes) {
    std::optional<RuntimeMethodInfo> best;
    int bestScore = std::numeric_limits<int>::max();
    bool ambiguous = false;

    for (const auto &candidate : klass.methods) {
        if (!candidate.name || !string_utils::iequals(candidate.name, method)) {
            continue;
        }

        auto sig =
            il::runtime::parseRuntimeSignature(candidate.signature ? candidate.signature : "");
        if (!sig.isValid() || sig.params.size() != argTypes.size()) {
            continue;
        }

        il::runtime::ParsedMethod parsed{candidate.name, candidate.target, sig};
        RuntimeMethodInfo info = makeRuntimeMethodInfo(klass.qname, candidate.name, parsed);
        auto score = scoreSignature(info.args, argTypes);
        if (!score) {
            continue;
        }

        if (*score < bestScore) {
            best = std::move(info);
            bestScore = *score;
            ambiguous = false;
        } else if (*score == bestScore) {
            ambiguous = true;
        }
    }

    if (ambiguous) {
        return {TypedLookupStatus::Ambiguous, std::nullopt};
    }
    if (!best)
        return {};
    return {TypedLookupStatus::Found, std::move(best)};
}

} // namespace

/**
 * @brief Determine whether a runtime class derives from Zanna.GUI.Widget.
 * @param classQName Qualified concrete runtime class name.
 * @return True when the catalog inheritance chain contains the Widget base.
 */
bool isGuiWidgetSubclass(std::string_view classQName) {
    const auto *klass = findRuntimeClass(classQName);
    while (klass && klass->baseQName && *klass->baseQName) {
        if (string_utils::iequals(klass->baseQName, "Zanna.GUI.Widget"))
            return true;
        klass = findRuntimeClass(klass->baseQName);
    }
    return false;
}

/**
 * @brief Access the process-wide stateless runtime method resolver.
 * @return Reference to the function-local singleton.
 */
const RuntimeMethodResolver &RuntimeMethodResolver::instance() {
    static const RuntimeMethodResolver resolver;
    return resolver;
}

/**
 * @brief Find a runtime method by name and explicit argument count.
 * @details Tries the requested class first, then walks its runtime base-class
 *          chain until a matching arity is found.
 * @param classQName Qualified receiver class name.
 * @param method Case-insensitive method name.
 * @param arity Number of explicit arguments, excluding any receiver.
 * @return Adapted runtime method metadata, or std::nullopt when absent.
 */
std::optional<RuntimeMethodInfo> RuntimeMethodResolver::find(std::string_view classQName,
                                                             std::string_view method,
                                                             std::size_t arity) const {
    const auto &registry = il::runtime::RuntimeRegistry::instance();
    auto parsed = registry.findMethod(classQName, method, arity);
    if (parsed) {
        return makeRuntimeMethodInfo(classQName, method, *parsed);
    }

    const auto *klass = findRuntimeClass(classQName);
    while (!parsed && klass && klass->baseQName && *klass->baseQName) {
        parsed = registry.findMethod(klass->baseQName, method, arity);
        if (parsed)
            return makeRuntimeMethodInfo(klass->baseQName, method, *parsed);
        klass = findRuntimeClass(klass->baseQName);
    }
    return std::nullopt;
}

/**
 * @brief Find the uniquely best runtime overload for concrete argument types.
 * @details Resolves direct overloads before searching base classes. Ambiguity
 *          within any searched class terminates lookup with no result.
 * @param classQName Qualified receiver class name.
 * @param method Case-insensitive method name.
 * @param argTypes Explicit IL scalar argument types.
 * @return Selected method metadata, or std::nullopt for no match or ambiguity.
 */
std::optional<RuntimeMethodInfo> RuntimeMethodResolver::find(
    std::string_view classQName,
    std::string_view method,
    const std::vector<il::runtime::ILScalarType> &argTypes) const {
    const auto &registry = il::runtime::RuntimeRegistry::instance();
    for (const auto &klass : registry.rawCatalog()) {
        if (!klass.qname || !string_utils::iequals(klass.qname, classQName)) {
            continue;
        }

        auto direct = findTypedInClass(klass, method, argTypes);
        if (direct.status == TypedLookupStatus::Found)
            return std::move(direct.method);
        if (direct.status == TypedLookupStatus::Ambiguous)
            return std::nullopt;
        break;
    }

    const auto *klass = findRuntimeClass(classQName);
    while (klass && klass->baseQName && *klass->baseQName) {
        klass = findRuntimeClass(klass->baseQName);
        if (!klass)
            break;
        auto inherited = findTypedInClass(*klass, method, argTypes);
        if (inherited.status == TypedLookupStatus::Found)
            return std::move(inherited.method);
        if (inherited.status == TypedLookupStatus::Ambiguous)
            return std::nullopt;
    }
    return std::nullopt;
}

/**
 * @brief Collect direct and inherited overload descriptions for diagnostics.
 * @details Preserves registry order while removing duplicate inherited strings.
 * @param classQName Qualified receiver class name.
 * @param method Case-insensitive method name.
 * @return Candidate descriptions from the class and its base chain.
 */
std::vector<std::string> RuntimeMethodResolver::candidates(std::string_view classQName,
                                                           std::string_view method) const {
    const auto &registry = il::runtime::RuntimeRegistry::instance();
    std::vector<std::string> result = registry.methodCandidates(classQName, method);
    const auto *klass = findRuntimeClass(classQName);
    while (klass && klass->baseQName && *klass->baseQName) {
        auto inherited = registry.methodCandidates(klass->baseQName, method);
        for (auto &candidate : inherited) {
            if (std::find(result.begin(), result.end(), candidate) == result.end())
                result.push_back(std::move(candidate));
        }
        klass = findRuntimeClass(klass->baseQName);
    }
    return result;
}

} // namespace il::frontends::common
