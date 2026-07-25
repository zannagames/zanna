//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/common/RuntimeMethodResolver.hpp
// Purpose: Frontend-neutral runtime method lookup and overload resolution.
// Key invariants: Method signatures are sourced only from the IL runtime
//                 registry; frontends adapt the returned ILScalarType values to
//                 their own type systems rather than maintaining parallel
//                 hardcoded method indexes.
// Ownership/Lifetime: Returned descriptors own their string/vector data. The
//                     singleton resolver borrows immutable registry metadata.
// Links: src/il/runtime/classes/RuntimeClasses.hpp,
//        src/frontends/basic/sem/RuntimeMethodIndex.hpp,
//        src/frontends/zia/Sema_Expr_Call.cpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares frontend-neutral runtime method lookup and overload ranking.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/runtime/classes/RuntimeClasses.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace il::frontends::common {

/**
 * @brief Frontend-neutral descriptor for a resolved runtime method.
 *
 * @details The descriptor keeps runtime signature data in IL-layer terms so
 * each language frontend can map the result to its own semantic type system.
 * The explicit argument vectors exclude an implicit receiver; @ref hasReceiver
 * tells lowerers whether the target extern still expects the receiver at call
 * time.
 */
struct RuntimeMethodInfo {
    /// @brief Return type expressed in shared IL scalar terms.
    il::runtime::ILScalarType ret{il::runtime::ILScalarType::Unknown};
    /// @brief Concrete class name for object returns, or empty when unknown.
    std::string returnClassQName;
    /// @brief Explicit parameter types, excluding the implicit receiver.
    std::vector<il::runtime::ILScalarType> args;
    /// @brief True when the runtime signature returns an untyped pointer.
    bool rawPointerReturn{false};
    /// @brief Raw-pointer flags corresponding to @ref args.
    std::vector<bool> rawPointerParams;
    /// @brief True when the physical extern signature expects a receiver argument.
    bool hasReceiver{true};
    /// @brief Canonical extern target used by IL call emission.
    std::string target;
};

/**
 * @brief Return true for concrete GUI runtime classes that inherit Widget operations.
 *
 * @details The runtime class catalog keeps base widget methods only on
 * `Zanna.GUI.Widget`; frontends use this predicate to offer and resolve those
 * methods for concrete widget handles without copying the methods into every
 * public class surface.
 *
 * @param classQName Qualified runtime class name to inspect.
 * @return True when a base-class chain reaches `Zanna.GUI.Widget`.
 */
[[nodiscard]] bool isGuiWidgetSubclass(std::string_view classQName);

/**
 * @brief Shared resolver for runtime class methods.
 *
 * @details This class centralizes the behavior that had drifted across
 * frontends: case-insensitive catalog lookup, signature parsing, overload
 * compatibility scoring, receiver detection, and diagnostic candidate listing.
 * It delegates all catalog data to @ref il::runtime::RuntimeRegistry.
 */
class RuntimeMethodResolver {
  public:
    /**
     * @brief Return the process-wide resolver instance.
     *
     * @details The resolver itself owns no mutable indexes; the singleton is a
     * stable access point over the immutable runtime registry.
     *
     * @return Shared resolver instance.
     */
    [[nodiscard]] static const RuntimeMethodResolver &instance();

    /**
     * @brief Find a runtime method by exact explicit arity.
     *
     * @details The lookup is case-insensitive and excludes the implicit
     * receiver from @p arity, matching runtime signature conventions.
     *
     * @param classQName Fully-qualified runtime class name.
     * @param method Runtime method name.
     * @param arity Number of explicit arguments at the source call site.
     * @return Resolved method information, or std::nullopt when no method
     *         matches.
     */
    [[nodiscard]] std::optional<RuntimeMethodInfo> find(std::string_view classQName,
                                                        std::string_view method,
                                                        std::size_t arity) const;

    /**
     * @brief Find the best overload for concrete IL argument types.
     *
     * @details Compatibility scoring accepts exact matches first, then safe
     * frontend conveniences that existing frontends already allowed: object
     * parameters accept non-void values, unknown arguments are weak matches,
     * and integer arguments may feed float or boolean runtime parameters. If
     * two overloads tie for best score, resolution fails as ambiguous.
     *
     * @param classQName Fully-qualified runtime class name.
     * @param method Runtime method name.
     * @param argTypes Explicit argument types, excluding the receiver.
     * @return Best resolved method, or std::nullopt for no match or ambiguity.
     */
    [[nodiscard]] std::optional<RuntimeMethodInfo> find(
        std::string_view classQName,
        std::string_view method,
        const std::vector<il::runtime::ILScalarType> &argTypes) const;

    /**
     * @brief List available overload candidates for diagnostics.
     *
     * @param classQName Fully-qualified runtime class name.
     * @param method Runtime method name.
     * @return Candidate strings such as `Substring/2`.
     */
    [[nodiscard]] std::vector<std::string> candidates(std::string_view classQName,
                                                      std::string_view method) const;
};

} // namespace il::frontends::common
