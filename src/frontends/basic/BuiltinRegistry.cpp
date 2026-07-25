//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BuiltinRegistry.cpp
// Purpose: Own dense BASIC builtin descriptors, semantic and lowering rules,
//          name indexes, and dynamically registered lowering handlers.
// Ownership/Lifetime: Static tables and maps live for the process lifetime.
// Links: src/frontends/basic/BuiltinRegistry.hpp,
//        src/frontends/basic/builtin_registry.inc,
//        src/frontends/basic/builtins/MathBuiltins.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declarative metadata and lookup utilities for BASIC builtins.
/// @details The registry centralises builtin descriptors so semantic analysis
///          and the lowering stages can agree on signatures, runtime feature
///          requirements, and lowering strategies.  Helper functions in this
///          unit perform lazy initialisation of lookup tables and expose a
///          stable API for both static and dynamically registered builtins.

#include "frontends/basic/BuiltinRegistry.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/builtins/MathBuiltins.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace il::frontends::basic {
namespace {
using B = BuiltinCallExpr::Builtin;
using LowerRule = BuiltinLoweringRule;
using ResultSpec = LowerRule::ResultSpec;
using Variant = LowerRule::Variant;
using Condition = Variant::Condition;
using VariantKind = Variant::Kind;
using Argument = LowerRule::Argument;
using Transform = LowerRule::ArgTransform;
using TransformKind = LowerRule::ArgTransform::Kind;
using Feature = LowerRule::Feature;
using FeatureAction = LowerRule::Feature::Action;

/// Number of dense builtin enumerators, including the final Err builtin.
constexpr std::size_t kBuiltinCount = static_cast<std::size_t>(B::Err) + 1;

/// @brief Convert a builtin enumerator into a dense array index.
/// @details All tables in this file store entries densely in declaration order
///          so the enumerator's numeric value doubles as the correct array
///          index.  Using a helper keeps the casting logic centralised.
/// @param b Builtin enumerator being indexed.
/// @return Zero-based array index corresponding to @p b.
constexpr std::size_t idx(B b) noexcept {
    return static_cast<std::size_t>(b);
}

/// @brief Heterogeneous hash enabling string_view/`const char*` lookups into a
///        std::string-keyed map without constructing a temporary std::string.
struct TransparentStringHash {
    /// Marker enabling heterogeneous unordered-map lookup.
    using is_transparent = void;

    /// @brief Hash a borrowed string view.
    /// @param sv Key bytes.
    /// @return Standard string-view hash.
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }

    /// @brief Hash a NUL-terminated string without allocating.
    /// @param sv Key string.
    /// @return Standard string-view hash.
    std::size_t operator()(const char *sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }

    /// @brief Hash an owning string through its view.
    /// @param s Key string.
    /// @return Standard string-view hash.
    std::size_t operator()(const std::string &s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

/// @brief Heterogeneous equality companion to @ref TransparentStringHash.
struct TransparentStringEqual {
    /// Marker enabling heterogeneous unordered-map lookup.
    using is_transparent = void;

    /// @brief Compare two key views byte-for-byte.
    /// @param lhs Left key.
    /// @param rhs Right key.
    /// @return Whether both views contain identical bytes.
    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

/// Exact-name map owning keys and storing non-owning function pointers.
using HandlerMap =
    std::unordered_map<std::string, BuiltinHandler, TransparentStringHash, TransparentStringEqual>;

/// @brief Access the singleton map storing dynamically registered builtins.
/// @details The registry persists for the lifetime of the process, allowing
///          tools or tests to inject custom builtin implementations at runtime.
///          It is initialised on first use and reused thereafter; inserts may
///          allocate or rehash its owned key storage.
/// @return Reference to the mutable handler registry.
/// @warning Access is not internally synchronized.
HandlerMap &builtinHandlerRegistry() {
    static HandlerMap registry{};
    return registry;
}

/// @brief Describes broad type categories produced by a builtin.
enum class TypeMask : std::uint8_t {
    /// No known result category.
    None = 0,
    /// Integer result category.
    I64 = 1U << 0U,
    /// Floating-point result category.
    F64 = 1U << 1U,
    /// String result category.
    Str = 1U << 2U,
};

/// @brief Combine two @ref TypeMask values.
/// @details Enables ergonomic composition of descriptor masks without verbose
///          static casts at every call site.
/// @param lhs First mask operand.
/// @param rhs Second mask operand.
/// @return Bitwise OR of the supplied masks.
constexpr TypeMask operator|(TypeMask lhs, TypeMask rhs) noexcept {
    return static_cast<TypeMask>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

/// @brief Inclusive argument-count bounds for a builtin.
struct Arity {
    std::uint8_t minArgs{0}; ///< Minimum required arguments.
    std::uint8_t maxArgs{0}; ///< Maximum accepted arguments.
};

/// @brief Single compile-time row of the densely indexed builtin table.
/// @details One entry exists per @ref BuiltinCallExpr::Builtin enumerator, in
///          declaration order, so the enumerator value doubles as the index.
struct BuiltinDescriptor {
    const char *name{nullptr};                 ///< Canonical uppercase BASIC spelling.
    B builtin{B::Len};                         ///< Enumerator this row describes.
    Arity arity{};                             ///< Accepted argument-count range.
    TypeMask typeMask{TypeMask::None};         ///< Possible result categories (see @ref TypeMask).
    SemanticAnalyzer::BuiltinAnalyzer analyze{}; ///< Optional semantic-analysis hook.
};

/// @brief Build the compile-time descriptor table for each builtin.
/// @details Names remain uppercase because @ref lookupBuiltin performs exact,
///          case-sensitive matches on normalized BASIC keywords.  The arity
///          bounds mirror @ref SemanticAnalyzer::builtinSignature so
///          diagnostics continue to enforce the same argument requirements, and
///          the type masks communicate the possible result categories to
///          inference helpers.  The helper remains constexpr so the resulting
///          table can populate other constant data structures without runtime
///          overhead.
/// @return Fully populated descriptor table.
constexpr std::array<BuiltinDescriptor, kBuiltinCount> makeBuiltinDescriptors() {
    std::array<BuiltinDescriptor, kBuiltinCount> descriptors{};
#define BUILTIN(NAME, DESCRIPTOR_EXPR, LOWER_EXPR, SCAN_EXPR)                                      \
    descriptors[idx(B::NAME)] = DESCRIPTOR_EXPR;
#include "frontends/basic/builtin_registry.inc"
#undef BUILTIN
    return descriptors;
}

/// Dense compile-time descriptors indexed by BuiltinCallExpr::Builtin.
constexpr auto kBuiltinDescriptors = makeBuiltinDescriptors();

/// @brief Create the lightweight info table consumed by semantic analysis.
/// @details Extracts the name and analyser callback from the descriptor table
///          so the semantic analyser can perform lookups without depending on
///          lowering metadata.
/// @return Array mapping builtins to @ref BuiltinInfo records.
constexpr std::array<BuiltinInfo, kBuiltinCount> makeBuiltinInfos() {
    std::array<BuiltinInfo, kBuiltinCount> infos{};
    for (const auto &desc : kBuiltinDescriptors)
        infos[idx(desc.builtin)] = BuiltinInfo{desc.name, desc.analyze};
    return infos;
}

/// Dense semantic info records derived from the descriptors.
constexpr auto kBuiltins = makeBuiltinInfos();

/// @brief Generate the lowering rule table referenced by lowering passes.
/// @details Uses an immediately-invoked lambda so additional registration steps
///          (such as math builtins) can run after the table is initialised while
///          still keeping the storage static-duration.
static const std::array<LowerRule, kBuiltinCount> kBuiltinLoweringRules = [] {
    std::array<LowerRule, kBuiltinCount> rules{};
#define BUILTIN(NAME, DESCRIPTOR_EXPR, LOWER_EXPR, SCAN_EXPR) rules[idx(B::NAME)] = LOWER_EXPR;
#include "frontends/basic/builtin_registry.inc"
#undef BUILTIN
    builtins::registerMathBuiltinLoweringRules(rules);
    return rules;
}();

/// @brief Populate the scan rule table for builtin argument traversal.
/// @details Similar to the lowering table, this lambda initialises the scan
///          metadata at static initialisation time while still allowing the math
///          builtin helpers to inject additional patterns.
static const std::array<BuiltinScanRule, kBuiltinCount> kBuiltinScanRules = [] {
    std::array<BuiltinScanRule, kBuiltinCount> rules{};
#define BUILTIN(NAME, DESCRIPTOR_EXPR, LOWER_EXPR, SCAN_EXPR) rules[idx(B::NAME)] = SCAN_EXPR;
#include "frontends/basic/builtin_registry.inc"
#undef BUILTIN
    builtins::registerMathBuiltinScanRules(rules);
    return rules;
}();

/// @brief Access the lazily constructed map from builtin names to enumerators.
/// @details Entries are stored with canonical uppercase spellings so callers
///          should normalize BASIC identifiers before lookup.  The map is built
///          on first use using descriptor data and cached for subsequent
///          lookups.
/// @return Reference to the immutable name-to-enum map.
const std::unordered_map<std::string_view, B> &builtinNameIndex() {
    /// Build the immutable exact-spelling index on first access.
    static const auto index = [] {
        std::unordered_map<std::string_view, B> map;
        map.reserve(kBuiltinDescriptors.size());
        for (const auto &desc : kBuiltinDescriptors)
            map.emplace(desc.name, desc.builtin);
        return map;
    }();
    return index;
}
} // namespace

/// @brief Fetch metadata for a BASIC built-in represented by its enum value.
/// @param b Builtin enumerator to inspect.
/// @return Reference to the metadata describing the semantic analysis hook.
/// @pre @p b must be a valid BuiltinCallExpr::Builtin enumerator; passing an
///      out-of-range value results in undefined behavior due to direct array
///      indexing.
const BuiltinInfo &getBuiltinInfo(BuiltinCallExpr::Builtin b) {
    return kBuiltins[static_cast<std::size_t>(b)];
}

/// @brief Retrieve the argument arity constraints for a builtin.
/// @param b Builtin enumerator to inspect.
/// @return Structure containing minimum and maximum argument counts, or
///         `{0, 0}` for an out-of-range value.
BuiltinArity getBuiltinArity(BuiltinCallExpr::Builtin b) {
    const auto idx = static_cast<std::size_t>(b);
    if (idx >= kBuiltinCount)
        return BuiltinArity{0, 0};
    const auto &desc = kBuiltinDescriptors[idx];
    return BuiltinArity{desc.arity.minArgs, desc.arity.maxArgs};
}

/// @brief Resolve a BASIC built-in enum from its source spelling.
/// @param name BASIC source spelling to look up. Exact canonical spelling is
///        attempted first, followed by case folding and the CHR alias.
/// @return Built-in enumerator on success; std::nullopt when the name is not
///         registered.
std::optional<BuiltinCallExpr::Builtin> lookupBuiltin(std::string_view name) {
    const auto &index = builtinNameIndex();
    auto it = index.find(name);
    if (it != index.end())
        return it->second;

    // Fall back to case-insensitive lookup.
    std::string upper;
    upper.reserve(name.size());
    for (unsigned char c : name)
        upper.push_back(static_cast<char>(std::toupper(c)));
    if (auto it2 = index.find(upper); it2 != index.end())
        return it2->second;

    // Builtin aliasing: accept CHR without suffix as CHR$.
    if (upper == "CHR")
        return BuiltinCallExpr::Builtin::Chr;

    return std::nullopt;
}

/// @brief Access the declarative scan rule for a BASIC builtin.
/// @param b Builtin enumerator whose rule is requested.
/// @return Reference to the rule describing argument traversal and runtime features.
const BuiltinScanRule &getBuiltinScanRule(BuiltinCallExpr::Builtin b) {
    return kBuiltinScanRules[static_cast<std::size_t>(b)];
}

/// @brief Access the lowering rule for a BASIC builtin.
/// @param b Builtin enumerator identifying the builtin.
/// @return Reference to the declarative lowering description.
const BuiltinLoweringRule &getBuiltinLoweringRule(BuiltinCallExpr::Builtin b) {
    return kBuiltinLoweringRules[static_cast<std::size_t>(b)];
}

/// @brief Register or remove a dynamically provided builtin handler.
/// @details Installing a non-null handler either adds a new entry or replaces
///          an existing one keyed by @p name.  Passing @c nullptr removes the
///          handler, allowing tests to restore the default behaviour.
/// @param name Canonical builtin name in uppercase form.
/// @param fn Handler function to associate with @p name, or nullptr to remove.
void register_builtin(std::string_view name, BuiltinHandler fn) {
    auto &registry = builtinHandlerRegistry();
    if (fn)
        registry.insert_or_assign(std::string{name}, fn);
    else
        registry.erase(std::string{name});
}

/// @brief Locate a dynamically registered builtin handler.
/// @details Performs an exact lookup against the canonical uppercase spelling.
///          When no handler exists the function returns @c nullptr so callers
///          can fall back to the static registry.
/// @param name Canonical builtin name to query.
/// @return Installed handler or nullptr when none is registered.
BuiltinHandler find_builtin(std::string_view name) {
    auto &registry = builtinHandlerRegistry();
    if (auto it = registry.find(name); it != registry.end())
        return it->second;
    return nullptr;
}

/// @brief Collapse a result-category mask to a single fixed result kind.
/// @details A builtin has a *fixed* result kind only when exactly one category
///          bit is set; masks with zero or multiple bits (result depends on
///          arguments) deliberately yield @ref BuiltinResultKind::Unknown.
/// @param m Result-category mask from the builtin descriptor.
/// @return The sole category as a @ref BuiltinResultKind, else Unknown.
static il::frontends::basic::BuiltinResultKind resultKindFromMask(TypeMask m) {
    using RK = il::frontends::basic::BuiltinResultKind;
    const bool i = (static_cast<std::uint8_t>(m) & static_cast<std::uint8_t>(TypeMask::I64)) != 0;
    const bool f = (static_cast<std::uint8_t>(m) & static_cast<std::uint8_t>(TypeMask::F64)) != 0;
    const bool s = (static_cast<std::uint8_t>(m) & static_cast<std::uint8_t>(TypeMask::Str)) != 0;
    const int count = (i ? 1 : 0) + (f ? 1 : 0) + (s ? 1 : 0);
    if (count != 1)
        return RK::Unknown;
    if (i)
        return RK::Int;
    if (f)
        return RK::Float;
    if (s)
        return RK::String;
    return RK::Unknown;
}

/// @copydoc getBuiltinFixedResult()
BuiltinResultKind getBuiltinFixedResult(BuiltinCallExpr::Builtin b) {
    const auto idxv = static_cast<std::size_t>(b);
    if (idxv >= kBuiltinCount)
        return BuiltinResultKind::Unknown;
    const auto &desc = kBuiltinDescriptors[idxv];
    return resultKindFromMask(desc.typeMask);
}

// Minimal registry-driven semantic signatures for critical builtins ----------
/// ARGGET's one required integer argument specification.
static const SemanticArgSpecView kArgGetArgs[] = {
    SemanticArgSpecView{false, BuiltinArgTypeMask::Int},
};

/// ARGC semantic signature: no arguments, integer result.
static const SemanticSignatureView kArgcSemSig{/*min*/ 0,
                                               /*max*/ 0,
                                               /*args*/ nullptr,
                                               /*count*/ 0,
                                               /*result*/ BuiltinResultKind::Int};
/// ARGGET semantic signature: one integer index, string result.
static const SemanticSignatureView kArgGetSemSig{/*min*/ 1,
                                                 /*max*/ 1,
                                                 /*args*/ kArgGetArgs,
                                                 /*count*/ 1,
                                                 /*result*/ BuiltinResultKind::String};
/// COMMAND semantic signature: no arguments, string result.
static const SemanticSignatureView kCommandSemSig{/*min*/ 0,
                                                  /*max*/ 0,
                                                  /*args*/ nullptr,
                                                  /*count*/ 0,
                                                  /*result*/ BuiltinResultKind::String};

/// @copydoc getBuiltinSemanticSignature()
const SemanticSignatureView *getBuiltinSemanticSignature(BuiltinCallExpr::Builtin b) {
    using E = BuiltinCallExpr::Builtin;
    switch (b) {
        case E::Argc:
            return &kArgcSemSig;
        case E::ArgGet:
            return &kArgGetSemSig;
        case E::Command:
            return &kCommandSemSig;
        default:
            return nullptr;
    }
}

} // namespace il::frontends::basic
