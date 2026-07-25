//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the registry that tracks BASIC procedure declarations, ensuring
// unique names and emitting diagnostics when conflicts occur.
//
//===----------------------------------------------------------------------===//
//
/// @file ProcRegistry.cpp
/// @brief Procedure registry implementation for the BASIC semantic analyser.
/// @details Maintains a hash table of function/subroutine signatures and exposes
///          helpers for registering new declarations, clearing state, and
///          performing lookups.

#include "frontends/basic/ProcRegistry.hpp"
#include "frontends/basic/Diag.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/types/TypeMapping.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <unordered_set>
#include <utility>

namespace il::frontends::basic {

/// @brief Construct a registry and seed its runtime-visible procedures.
/// @param d Diagnostic service borrowed for the registry's lifetime.
/// @pre @p d outlives this registry.
ProcRegistry::ProcRegistry(SemanticDiagnostics &d) : de(d) {
    // Seed built-in extern procedure signatures from runtime registry.
    seedRuntimeBuiltins();
}

/// @brief Remove all procedures registered so far.
/// @details Clears signature and qualified-entry tables, then immediately
///          repopulates supported dotted runtime builtins for the next unit.
void ProcRegistry::clear() {
    procs_.clear();
    byQualified_.clear();
    seedRuntimeBuiltins();
}

/// @brief Build a canonical signature from a descriptor collected during analysis.
///
/// The helper copies declaration metadata into a stable signature, performs
/// duplicate parameter checks, and validates array parameter types against the
/// BASIC specification.
///
/// @param descriptor Source-level procedure description.
/// @return Populated signature describing the procedure for later lookup.
ProcSignature ProcRegistry::buildSignature(const ProcDescriptor &descriptor) {
    ProcSignature sig;
    sig.kind = descriptor.kind;
    sig.retType = descriptor.retType;

    std::unordered_set<std::string> paramNames;
    for (const auto &p : descriptor.params) {
        if (!paramNames.insert(p.name).second) {
            de.emit(diag::BasicDiag::DuplicateParameter,
                    p.loc,
                    static_cast<uint32_t>(p.name.size()),
                    std::initializer_list<diag::Replacement>{diag::Replacement{"name", p.name}});
        }
        if (p.is_array && p.type != Type::I64 && p.type != Type::Str) {
            de.emit(diag::BasicDiag::ArrayParamType, p.loc, static_cast<uint32_t>(p.name.size()));
        }
        sig.params.push_back({p.type, p.is_array});
    }

    return sig;
}

/// @brief Remove one trailing BASIC type suffix from a procedure spelling.
/// @param name Identifier segment to normalize.
/// @return Owned spelling without a final `$`, `#`, `!`, `&`, or `%`.
static std::string stripSuffix(std::string_view name) {
    if (name.empty())
        return std::string{};
    char last = name.back();
    if (last == '$' || last == '#' || last == '!' || last == '&' || last == '%')
        name = name.substr(0, name.size() - 1);
    return std::string{name};
}

/// @brief Canonicalize every segment of a dotted procedure name.
/// @details Splits on dots, strips a BASIC type suffix only from the final
///          segment, canonicalizes non-empty segments, and rejoins them.
///          Empty segments are retained; an invalid non-empty segment makes the
///          entire result empty.
/// @param dotted Qualified or unqualified dotted spelling.
/// @return Canonical joined key, or an empty string on invalid input.
static std::string canonicalizeQualifiedFlat(std::string_view dotted) {
    // Split on '.' and canonicalize each segment (ASCII lowercase).
    // For the final segment only, strip BASIC type suffix before canonicalization.
    std::vector<std::string> parts;
    parts.reserve(4);
    std::string segment;
    std::vector<std::string> raw;
    for (size_t i = 0; i <= dotted.size(); ++i) {
        if (i == dotted.size() || dotted[i] == '.') {
            raw.emplace_back(std::move(segment));
            segment.clear();
        } else {
            segment.push_back(dotted[i]);
        }
    }

    parts.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        std::string_view seg = raw[i];
        if (seg.empty()) {
            parts.emplace_back(std::string{});
            continue;
        }
        // Strip type suffix from the final identifier segment, if present.
        if (i + 1 == raw.size()) {
            char last = seg.back();
            if (last == '$' || last == '#' || last == '!' || last == '&' || last == '%') {
                seg = seg.substr(0, seg.size() - 1);
            }
        }
        std::string canon = CanonicalizeIdent(seg);
        if (canon.empty() && !seg.empty()) {
            // Invalid character encountered; signal failure.
            return std::string{};
        }
        parts.emplace_back(std::move(canon));
    }
    return JoinQualified(parts);
}

/// @brief Register one descriptor under display and canonical lookup keys.
/// @details Canonicalizes qualified segments and the final type suffix, rejects
///          invalid names, checks the qualified-entry table for user/builtin
///          conflicts, validates the descriptor, and inserts signature copies
///          under both the supplied spelling and canonical key.
/// @param name Display or dotted procedure name.
/// @param descriptor Kind, return type, and borrowed parameter declarations.
/// @param loc Declaration location used for invalid/duplicate diagnostics and storage.
void ProcRegistry::registerProcImpl(std::string_view name,
                                    const ProcDescriptor &descriptor,
                                    il::support::SourceLoc loc) {
    // Derive canonical qualified key. Lowercase all segments and strip suffix
    // from the final segment for unqualified or dotted names alike.
    std::string key;
    if (name.find('.') != std::string_view::npos)
        key = canonicalizeQualifiedFlat(name);
    else
        key = CanonicalizeIdent(stripSuffix(name));

    if (key.empty()) {
        std::string nameStr{name};
        de.emitter().emit(il::support::Severity::Error,
                          "B0005",
                          loc,
                          static_cast<uint32_t>(nameStr.size()),
                          "invalid procedure name '" + nameStr + "'");
        return;
    }

    auto it = byQualified_.find(key);
    if (it != byQualified_.end()) {
        // Duplicate name: if the existing entry is a builtin extern, report the
        // dedicated shadowing error; otherwise emit the standard duplicate proc.
        std::string display = key;
        if (it->second.kind == ProcKind::BuiltinExtern) {
            diagx::ErrorBuiltinShadow(de.emitter(), display, loc);
        } else {
            diagx::ErrorDuplicateProc(de.emitter(), display, it->second.loc, loc);
        }
        return;
    }

    byQualified_.emplace(key, ProcEntry{nullptr, loc});
    // Build signature once, then insert under both original and canonical keys for lookup.
    ProcSignature sig = buildSignature(descriptor);
    std::string nameStr{name};
    procs_.emplace(std::move(nameStr), sig);
    procs_.emplace(key, sig);
}

/// @brief Register a FUNCTION declaration with its return type and parameters.
/// @details Constructs a @ref ProcDescriptor capturing the declaration metadata
///          and chooses `qualifiedName`, namespace path plus name, or the
///          unqualified name before delegating to @ref registerProcImpl.
/// @param f Function declaration to index; no AST pointer is retained.
void ProcRegistry::registerProc(const FunctionDecl &f) {
    const ProcDescriptor descriptor{
        ProcSignature::Kind::Function, f.ret, std::span<const Param>{f.params}, f.loc};
    std::string nameBuf;
    std::string_view nm;
    if (!f.qualifiedName.empty()) {
        nm = std::string_view{f.qualifiedName};
    } else if (!f.namespacePath.empty()) {
        // Build a dotted name from namespacePath + name so shadowing checks can fire.
        nameBuf = JoinQualified(f.namespacePath);
        if (!nameBuf.empty()) {
            nameBuf.push_back('.');
            nameBuf += f.name;
            nm = std::string_view{nameBuf};
        } else {
            nm = std::string_view{f.name};
        }
    } else {
        nm = std::string_view{f.name};
    }
    registerProcImpl(nm, descriptor, f.loc);
}

/// @brief Register a SUB declaration with its parameter list.
/// @details Functions similarly to @ref registerProc for functions but records a
///          void return type and the best available qualified spelling.
/// @param s Subroutine declaration to index; no AST pointer is retained.
void ProcRegistry::registerProc(const SubDecl &s) {
    const ProcDescriptor descriptor{
        ProcSignature::Kind::Sub, std::nullopt, std::span<const Param>{s.params}, s.loc};
    std::string nameBuf;
    std::string_view nm;
    if (!s.qualifiedName.empty()) {
        nm = std::string_view{s.qualifiedName};
    } else if (!s.namespacePath.empty()) {
        nameBuf = JoinQualified(s.namespacePath);
        if (!nameBuf.empty()) {
            nameBuf.push_back('.');
            nameBuf += s.name;
            nm = std::string_view{nameBuf};
        } else {
            nm = std::string_view{s.name};
        }
    } else {
        nm = std::string_view{s.name};
    }
    registerProcImpl(nm, descriptor, s.loc);
}

/// @brief Access the internal procedure table for iteration.
/// @return Const reference to the registry-owned display/canonical signature map.
const ProcTable &ProcRegistry::procs() const {
    return procs_;
}

/// @brief Look up a registered procedure by name.
///
/// @param name Identifier to search for.
/// @return Pointer to the stored signature when found; otherwise nullptr.
const ProcSignature *ProcRegistry::lookup(std::string_view name) const {
    // First try exact lookup for performance (heterogeneous lookup, no allocation)
    auto it = procs_.find(name);
    if (it != procs_.end())
        return &it->second;

    // Canonicalize qualified names (case-insensitive, strip suffix from final segment)
    std::string key;
    if (name.find('.') != std::string_view::npos)
        key = canonicalizeQualifiedFlat(name);
    else
        key = CanonicalizeIdent(stripSuffix(name));

    if (key.empty())
        return nullptr;

    it = procs_.find(key);
    return it == procs_.end() ? nullptr : &it->second;
}

/// @brief Register a function pointer through the legacy phase-one API.
/// @details A null pointer is ignored. The explicit @p loc supplies stored and
///          diagnostic location data; `qualifiedName` is preferred over the
///          unqualified name, and namespacePath is not synthesized by this API.
/// @param fn Function declaration to register, or null for no operation.
/// @param loc Location associated with this registration.
void ProcRegistry::AddProc(const FunctionDecl *fn, il::support::SourceLoc loc) {
    if (!fn)
        return;
    const ProcDescriptor descriptor{
        ProcSignature::Kind::Function, fn->ret, std::span<const Param>{fn->params}, loc};
    std::string_view nm = fn->qualifiedName.empty() ? std::string_view{fn->name}
                                                    : std::string_view{fn->qualifiedName};
    registerProcImpl(nm, descriptor, loc);
}

/// @brief Look up an entry by its exact qualified-table key.
/// @details Performs heterogeneous lookup without case folding, suffix stripping,
///          or dotted-name canonicalization.
/// @param qualified Exact key, normally already canonicalized.
/// @return Pointer to the registry-owned entry, or null when absent.
const ProcRegistry::ProcEntry *ProcRegistry::LookupExact(std::string_view qualified) const {
    // Heterogeneous lookup, no allocation
    auto it = byQualified_.find(qualified);
    return it == byQualified_.end() ? nullptr : &it->second;
}

/// @brief Seed the procedure registry with builtin externs from the runtime registry.
/// @details Iterates runtime descriptors, selects canonical dotted names (e.g.,
///          "Zanna.*"), skips signatures whose non-void types cannot map to
///          BASIC, and records runtime IDs plus raw-pointer/object masks. Each
///          accepted descriptor is inserted under its display and canonical
///          keys unless that canonical key already exists.
void ProcRegistry::seedRuntimeBuiltins() {
    using namespace il::runtime;
    const auto &registry = runtimeRegistry();
    for (const auto &desc : registry) {
        // Only publish canonical dotted names; skip legacy flat aliases.
        if (desc.name.find('.') == std::string_view::npos)
            continue;

        // Prefer helpers with a generated signature id (back-pointer for lowering),
        // but also seed descriptors without one so dotted runtime names like
        // Zanna.IO.File.* resolve during semantic analysis.
        auto sigIdOpt = findRuntimeSignatureId(desc.name);

        // Map return type; Void -> SUB (no return), others -> FUNCTION.
        std::optional<Type> retTy;
        if (auto mappedRet = types::mapIlToBasic(desc.signature.retType))
            retTy = *mappedRet;
        else if (desc.signature.retType.kind != il::core::Type::Kind::Void)
            continue; // Unsupported return type; skip

        // Map parameter list; fail if any unsupported type present.
        std::vector<Param> params;
        params.reserve(desc.signature.paramTypes.size());
        bool ok = true;
        for (const auto &p : desc.signature.paramTypes) {
            auto mapped = types::mapIlToBasic(p);
            if (!mapped) {
                ok = false;
                break;
            }
            Param param{};
            param.name = "p"; // name not used for builtins; placeholder
            param.type = *mapped;
            param.is_array = false;
            params.push_back(std::move(param));
        }
        if (!ok)
            continue;

        // Build ProcSignature directly
        ProcSignature sig;
        sig.kind = retTy ? ProcSignature::Kind::Function : ProcSignature::Kind::Sub;
        sig.retType = retTy;
        for (const auto &p : params)
            sig.params.push_back({p.type, p.is_array});
        sig.isRuntimeBuiltin = true;
        sig.runtimeTarget = std::string(desc.name);
        sig.rawPointerParams.reserve(desc.signature.paramTypes.size());
        sig.objectParams.reserve(desc.signature.paramTypes.size());
        for (std::size_t i = 0; i < desc.signature.paramTypes.size(); ++i) {
            const bool objectParam =
                (desc.signature.objectParamMask & (std::uint64_t{1} << i)) != 0;
            sig.rawPointerParams.push_back(
                desc.signature.paramTypes[i].kind == il::core::Type::Kind::Ptr && !objectParam);
            sig.objectParams.push_back(objectParam);
        }
        if (auto parsed = RuntimeRegistry::instance().findFunction(desc.name)) {
            sig.rawPointerReturn = parsed->rawPointerReturn;
            sig.objectReturn =
                parsed->returnType == ILScalarType::Object && !parsed->rawPointerReturn;
        }

        // Canonical qualified key
        std::string key = canonicalizeQualifiedFlat(desc.name);
        if (key.empty())
            continue;
        // Avoid duplicate insertions.
        if (byQualified_.find(key) != byQualified_.end())
            continue;

        byQualified_.emplace(key, ProcEntry{nullptr, {}, ProcKind::BuiltinExtern, sigIdOpt});
        // Insert under both display and canonical keys for lookup();
        procs_.emplace(std::string(desc.name), sig);
        procs_.emplace(key, sig);
    }
}

} // namespace il::frontends::basic
