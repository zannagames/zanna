//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Runtime.cpp
/// @brief Runtime function registration for the Zia semantic analyzer.
///
/// @details This file implements the initialization of runtime function bindings
/// for the Zia semantic analyzer. It bridges the IL-layer RuntimeRegistry with
/// the Zia type system, enabling full type-checking of runtime function calls.
///
/// ## Registration Process
///
/// The initRuntimeFunctions() method performs three phases of registration:
///
/// ### Phase 1: Runtime Class Types
///
/// Registers each runtime class (e.g., "Zanna.String", "Zanna.File") as a
/// type in the Zia type registry. This enables the semantic analyzer to
/// recognize expressions like `new Zanna.Graphics.Canvas(...)` and property
/// accesses like `canvas.Width`.
///
/// ### Phase 2: Runtime Function Fallbacks (ZiaRuntimeExterns.inc)
///
/// Reads the generated ZiaRuntimeExterns.inc metadata table and registers
/// ABI-shaped fallback extern signatures for every RT_FUNC entry.
/// These cover runtime calls that are not described by the runtime-class
/// catalog, such as `Zanna.Time.Clock.Sleep` or `Zanna.Game.LevelData.ObjectType`.
///
/// ### Phase 3: Methods and Properties from RuntimeRegistry
///
/// For each runtime class in the catalog:
///
/// 1. **Methods**: Parses the signature string (e.g., "str(i64,i64)") and
///    refines the function with full parameter type information. This enables
///    the semantic analyzer to validate argument types at compile time.
///
/// 2. **Properties**: Registers getter and setter functions. Getters are
///    zero-parameter functions returning the property type. Setters are
///    void functions taking the property type as a parameter.
///
/// ## Type Conversion
///
/// The RuntimeAdapter functions (toZiaType, toZiaParamTypes) convert IL-layer
/// type representations to Zia semantic types:
///
/// - ILScalarType::I64 → types::integer()
/// - ILScalarType::F64 → types::number()
/// - ILScalarType::Bool → types::boolean()
/// - ILScalarType::String → types::string()
/// - ILScalarType::Object → typed runtime classes or types::any()
/// - ILScalarType::Void → types::voidType()
///
/// ## Example Registration
///
/// For `Zanna.String.Substring` with signature "str(i64,i64)":
///
/// ```cpp
/// // Parsed signature: returnType=String, params=[I64, I64]
/// defineExternFunction(
///     "Zanna.String.Substring",  // extern target name
///     types::string(),           // return type
///     {types::integer(), types::integer()}  // parameter types
/// );
/// ```
///
/// This enables the semantic analyzer to verify that calls like:
/// ```zia
/// var s = "hello".Substring(0, 3)  // OK: Integer arguments
/// var s = "hello".Substring("a")   // ERROR: String argument, expected Integer
/// ```
///
/// ## Thread Safety
///
/// This function is called once during Sema initialization before any
/// concurrent access. The RuntimeRegistry itself is thread-safe and immutable.
///
/// @see RuntimeAdapter.hpp - Type conversion between IL and Zia types
/// @see il::runtime::RuntimeRegistry - Source of runtime signatures
/// @see Sema::defineExternFunction - Registers extern functions in symbol table
/// @see ZiaRuntimeExterns.inc - Generated fallback extern metadata
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/RuntimeAdapter.hpp"
#include "frontends/zia/Sema.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace il::frontends::zia {

namespace {

/// @brief Generated fallback metadata for one runtime extern.
struct ZiaRuntimeExternSpec {
    /// Canonical dotted runtime name.
    std::string_view canonical;
    /// Generated runtime signature spelling.
    std::string_view signature;
    /// Newline-delimited parameter names.
    std::string_view paramNames;
    /// Per-parameter pointer bridge role codes.
    std::string_view bridgeRoles;
};

#include "il/runtime/ZiaRuntimeExterns.inc"

/// @brief Trim ASCII whitespace from both ends of a runtime signature token.
/// @param value Token view to trim.
/// @return Subview containing the non-whitespace token.
static std::string_view trimRuntimeToken(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
        value.remove_suffix(1);
    return value;
}

/// @brief Extract a runtime token's base name before any generic argument.
/// @param token Runtime token such as `seq<str>`.
/// @return Trimmed base token such as `seq`.
static std::string_view runtimeTokenBase(std::string_view token) {
    token = trimRuntimeToken(token);
    size_t genericStart = token.find('<');
    if (genericStart == std::string_view::npos)
        return token;
    return trimRuntimeToken(token.substr(0, genericStart));
}

/// @brief Extract the text inside a runtime token's outer angle brackets.
/// @param token Runtime token such as `seq<str>`.
/// @return Trimmed inner token, or an empty view when no valid argument exists.
static std::string_view runtimeTokenTypeArg(std::string_view token) {
    token = trimRuntimeToken(token);
    size_t genericStart = token.find('<');
    size_t genericEnd = token.rfind('>');
    if (genericStart == std::string_view::npos || genericEnd == std::string_view::npos ||
        genericEnd <= genericStart)
        return {};
    return trimRuntimeToken(token.substr(genericStart + 1, genericEnd - genericStart - 1));
}

/// @brief Convert a generated runtime parameter token to its Zia surface type.
/// @param token Runtime token, including optional or container syntax.
/// @return Corresponding semantic type; unsupported tokens conservatively become Any.
static TypeRef ziaParamTypeForGeneratedToken(std::string_view token) {
    token = trimRuntimeToken(token);
    if (!token.empty() && token.back() == '?') {
        token.remove_suffix(1);
        return types::optional(ziaParamTypeForGeneratedToken(token));
    }

    std::string_view base = runtimeTokenBase(token);
    if (base == "str")
        return types::string();
    if (base == "i64" || base == "i32" || base == "i16")
        return types::integer();
    if (base == "f64")
        return types::number();
    if (base == "i1" || base == "bool")
        return types::boolean();
    if (base == "void")
        return types::voidType();
    if (base == "seq") {
        std::string_view elem = runtimeTokenTypeArg(token);
        if (elem.empty())
            return types::ptr();
        return types::seqOf(ziaParamTypeForGeneratedToken(elem));
    }
    if (base == "list") {
        std::string_view elem = runtimeTokenTypeArg(token);
        if (elem.empty())
            return types::ptr();
        return types::list(ziaParamTypeForGeneratedToken(elem));
    }
    if (base == "ptr")
        return types::ptr();
    return types::any();
}

/// @brief Split the parameter portion of a generated runtime signature.
/// @param signature Signature spelling containing parentheses.
/// @return Trimmed parameter token views, respecting commas nested in angle brackets.
static std::vector<std::string_view> generatedSignatureParamTokens(std::string_view signature) {
    std::vector<std::string_view> tokens;
    size_t open = signature.find('(');
    size_t close = signature.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
        return tokens;

    std::string_view args = signature.substr(open + 1, close - open - 1);
    size_t start = 0;
    int angleDepth = 0;
    for (size_t i = 0; i <= args.size(); ++i) {
        if (i < args.size()) {
            if (args[i] == '<')
                ++angleDepth;
            else if (args[i] == '>' && angleDepth > 0)
                --angleDepth;
        }

        if (i == args.size() || (args[i] == ',' && angleDepth == 0)) {
            std::string_view token = trimRuntimeToken(args.substr(start, i - start));
            if (!token.empty())
                tokens.push_back(token);
            start = i + 1;
        }
    }
    return tokens;
}

/// @brief Convert every generated extern parameter token to a Zia semantic type.
/// @param signature Generated runtime signature.
/// @return Parameter types in ABI order.
static std::vector<TypeRef> ziaParamTypesForGeneratedExtern(std::string_view signature) {
    std::vector<TypeRef> paramTypes;
    auto tokens = generatedSignatureParamTokens(signature);
    paramTypes.reserve(tokens.size());
    for (std::string_view token : tokens)
        paramTypes.push_back(ziaParamTypeForGeneratedToken(token));
    return paramTypes;
}

/// @brief Translate a generated extern's parsed return type for the Zia surface.
/// @details The runtime row is the only source of a returned object's class (ADR 0356): an
///          `obj<Class>` return becomes that class, `seq<T>`/`list<T>` a typed container, and a
///          bare `obj` stays Any. Nothing is inferred from the owning class or the method name.
/// @param sig Parsed runtime signature.
/// @return Declared class or container, scalar translation for scalar returns, and Any for an
///         undeclared object.
static TypeRef ziaReturnTypeForGeneratedExtern(const il::runtime::ParsedSignature &sig) {
    if (!sig.objectTypeName.empty())
        return runtimeObjectType(sig.objectTypeName);

    if (!sig.elementTypeName.empty()) {
        TypeRef elemType = ziaParamTypeForGeneratedToken(sig.elementTypeName);
        if (sig.containerTypeName == "list")
            return types::list(elemType);
        return types::seqOf(elemType);
    }

    return toZiaType(sig.returnType);
}

/// @brief Decode newline-delimited generated parameter names.
/// @param encoded Generated name payload.
/// @return Parameter names in signature order.
static std::vector<std::string> splitGeneratedParamNames(std::string_view encoded) {
    std::vector<std::string> names;
    if (encoded.empty())
        return names;

    size_t start = 0;
    while (true) {
        size_t end = encoded.find('\n', start);
        if (end == std::string_view::npos) {
            names.emplace_back(encoded.substr(start));
            break;
        }
        names.emplace_back(encoded.substr(start, end - start));
        start = end + 1;
    }
    return names;
}

/// @brief Decode one generated pointer-bridge role code.
/// @param code `c` for callback, `p` for payload, or any other code for none.
/// @return Semantic bridge role.
static Sema::RuntimePointerBridgeRole generatedBridgeRole(char code) {
    if (code == 'c')
        return Sema::RuntimePointerBridgeRole::Callback;
    if (code == 'p')
        return Sema::RuntimePointerBridgeRole::Payload;
    return Sema::RuntimePointerBridgeRole::None;
}

/// @brief Decode per-parameter pointer bridge roles from generated metadata.
/// @param encoded Compact role-code sequence.
/// @param paramCount Number of parameters requiring role entries.
/// @return Role vector padded with None, or an empty vector when no metadata was encoded.
static std::vector<Sema::RuntimePointerBridgeRole> decodeGeneratedBridgeRoles(
    std::string_view encoded, std::size_t paramCount) {
    if (encoded.empty())
        return {};

    std::vector<Sema::RuntimePointerBridgeRole> roles;
    roles.reserve(paramCount);
    for (std::size_t i = 0; i < paramCount; ++i) {
        char code = i < encoded.size() ? encoded[i] : 'n';
        roles.push_back(generatedBridgeRole(code));
    }
    return roles;
}

} // namespace

/// @brief Initializes all runtime function bindings for semantic analysis.
///
/// @details This method populates the Zia semantic analyzer's symbol table
/// with extern declarations for all runtime functions. It uses the unified
/// RuntimeRegistry to ensure signature information is consistent with other
/// frontends.
///
/// The registration happens in three phases:
///
/// 1. **Type Registration**: Each runtime class is registered as a named type,
///    enabling `new ClassName()` expressions and type annotations.
///
/// 2. **Fallback Registration**: Each generated RT_FUNC row is
///    registered with ABI-shaped parameter types and pointer-safety metadata.
///
/// 3. **Method/Property Registration**: For each class, all methods and
///    properties refine the fallback signatures with catalog-level type
///    information. Methods get their signature from parseRuntimeSignature();
///    properties get separate getter and setter registrations.
///
/// ## Error Handling
///
/// Methods with unparseable signatures (isValid() returns false) are silently
/// skipped. This is acceptable because:
/// - The signature format is well-defined and generated by rtgen
/// - Invalid signatures indicate a bug in runtime.def, not user code
/// - The method simply won't be available for use
///
/// ## Performance
///
/// This function is called once during Sema construction. Work is linear in the
/// generated extern table plus the runtime catalog's methods and properties.
///
void Sema::initRuntimeFunctions() {
    // Access the singleton RuntimeRegistry which contains all parsed signatures
    const auto &registry = il::runtime::RuntimeRegistry::instance();
    const auto &catalog = registry.rawCatalog();
    /// @brief Registers a runtime extern or refines an existing generated declaration.
    /// @param name Canonical runtime symbol name.
    /// @param returnType Refined semantic return type.
    /// @param fallbackParamTypes Parameter types used when no extern exists.
    /// @param pointerSafety Optional raw-pointer safety metadata.
    /// @param paramNames Source-visible parameter names.
    auto registerOrRefineExtern = [&](const std::string &name,
                                      TypeRef returnType,
                                      const std::vector<TypeRef> &fallbackParamTypes,
                                      std::optional<RuntimePointerSafety> pointerSafety =
                                          std::nullopt,
                                      const std::vector<std::string> &paramNames = {}) {
        if (name.empty())
            return;

        if (Symbol *existing = currentScope_->lookupLocal(name);
            existing && existing->isExtern && existing->kind == Symbol::Kind::Function &&
            existing->type && existing->type->kind == TypeKindSem::Function) {
            std::optional<RuntimePointerSafety> refinedSafety = std::nullopt;
            if (runtimePointerSafety_.find(name) == runtimePointerSafety_.end())
                refinedSafety = std::move(pointerSafety);
            std::vector<std::string> effectiveParamNames =
                paramNames.empty() ? existing->paramNames : paramNames;
            defineExternFunction(
                name, returnType, existing->type->paramTypes(), effectiveParamNames, refinedSafety);
            return;
        }

        defineExternFunction(
            name, returnType, fallbackParamTypes, paramNames, std::move(pointerSafety));
    };

    //==========================================================================
    // Phase 1: Register runtime class types
    //==========================================================================
    // Each runtime class becomes a named type in the Zia type registry.
    // This enables type checking for:
    // - Variable declarations: `var f: Zanna.File`
    // - Constructor calls: `new Zanna.File("path.txt")`
    // - Type comparisons and casts
    for (const auto &cls : catalog) {
        typeRegistry_[cls.qname] = types::runtimeClass(cls.qname);
    }

    //==========================================================================
    // Phase 2: Register RT_FUNC fallback externs from runtime.def
    //==========================================================================
    // The ZiaRuntimeExterns.inc table is generated by rtgen from runtime.def.
    // It provides ABI-shaped fallback signatures for all RT_FUNC entries without
    // forcing clang to optimize thousands of generated registration statements.
    // Phase 3 below overrides any entries that have richer runtime-class
    // metadata (e.g. receiver-less method signatures or typed seq<T> returns).
    for (const auto &entry : kZiaRuntimeExterns) {
        auto sig = il::runtime::parseRuntimeSignature(entry.signature);
        if (!sig.isValid())
            continue;

        TypeRef returnType = ziaReturnTypeForGeneratedExtern(sig);
        if (sig.isOptionalReturn)
            returnType = types::optional(returnType);

        RuntimePointerSafety pointerSafety{
            sig.rawPointerReturn,
            sig.rawPointerParams,
            decodeGeneratedBridgeRoles(entry.bridgeRoles, sig.params.size()),
        };

        registerOrRefineExtern(std::string(entry.canonical),
                               returnType,
                               ziaParamTypesForGeneratedExtern(entry.signature),
                               std::move(pointerSafety),
                               splitGeneratedParamNames(entry.paramNames));
    }

    //==========================================================================
    // Phase 3: Register methods and properties with full signatures (fine)
    //==========================================================================
    // This phase runs AFTER Phase 2 so that typed returns (e.g. seqOf(string)
    // for seq<str>-annotated methods) override the coarse ptr() from Phase 2.
    for (const auto &cls : catalog) {
        //----------------------------------------------------------------------
        // Register all methods for this class
        //----------------------------------------------------------------------
        for (const auto &m : cls.methods) {
            // Parse the signature string (e.g., "str(i64,i64)") into structured form
            auto sig = il::runtime::parseRuntimeSignature(m.signature ? m.signature : "");
            if (!sig.isValid())
                continue; // Skip methods with unparseable signatures

            // Convert IL types to Zia types, honouring element type hints from seq<T>. An object
            // result has exactly the class its row declares; a bare `obj` stays Any (ADR 0356).
            TypeRef returnType = toZiaReturnType(sig);
            if (sig.isOptionalReturn)
                returnType = types::optional(returnType);
            std::vector<TypeRef> paramTypes = toZiaParamTypes(sig);
            RuntimePointerSafety pointerSafety{sig.rawPointerReturn, sig.rawPointerParams, {}};

            // Preserve ABI-shaped explicit receiver signatures from Phase 2 while
            // refining the return type for method-style semantic analysis.
            registerOrRefineExtern(m.target ? m.target : "", returnType, paramTypes, pointerSafety);
        }

        //----------------------------------------------------------------------
        // Register property getters and setters
        //----------------------------------------------------------------------
        for (const auto &p : cls.properties) {
            // Convert the property's IL type to a Zia type. Property type strings
            // may use the same typed object annotation as function signatures
            // (for example obj<Zanna.GUI.Widget>).
            TypeRef propType = nullptr;
            std::string propSigText = std::string(p.type ? p.type : "") + "()";
            auto propSig = il::runtime::parseRuntimeSignature(propSigText);
            if (propSig.isValid()) {
                propType = toZiaReturnType(propSig);
                if (propSig.isOptionalReturn)
                    propType = types::optional(propType);
            } else {
                propType = toZiaType(il::runtime::mapILToken(p.type ? p.type : ""));
            }

            // Register getter: no parameters, returns property type
            // Example: Zanna.String.get_Length() -> Integer
            if (p.getter) {
                registerOrRefineExtern(p.getter, propType, {});
            }

            // Register setter: takes property type, returns void
            // Example: Zanna.GUI.Widget.set_Visible(Boolean) -> void
            if (p.setter) {
                registerOrRefineExtern(p.setter, types::voidType(), {propType});
            }
        }
    }
}

} // namespace il::frontends::zia
