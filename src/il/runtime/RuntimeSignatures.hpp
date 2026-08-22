//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the runtime helper signature registry, which provides type
// and side effect information for all runtime library functions callable from IL
// programs. The registry enables the compiler to generate type-correct IL for
// runtime calls and provides optimization metadata for analysis passes.
//
// Zanna's runtime library provides essential services: memory management, string
// operations, array access, file I/O, and mathematical functions. Each runtime
// helper has a stable C ABI signature that IL programs must respect. This file
// defines the canonical registry mapping helper names to their signatures and
// side effect annotations.
//
// Registry Structure:
// - RtSig enumeration: Each runtime helper has a unique enumeration constant
//   generated from the RuntimeSigs.def macro file
// - RuntimeSignature: Structured representation of a helper's return type,
//   parameter types, and effect annotations (readonly, noalias, etc.)
// - Lookup functions: Query helpers by name or enumeration constant to retrieve
//   signature information
//
// Integration:
// The BASIC frontend uses this registry when lowering builtin functions to IL
// runtime calls. Optimization passes consult effect annotations to determine
// whether runtime calls can be reordered, eliminated, or moved.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the canonical runtime ABI descriptor and lookup registry.
/// @details The registry joins textual IL signatures, VM invocation adapters,
///          lowering requirements, trap behavior, and ownership/effect metadata
///          for runtime-call consumers.

#pragma once

#include "il/core/Type.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace il::runtime {

/// @brief Enumerates runtime signatures covered by the generated registry subset.
enum class RtSig : std::size_t {
#define SIG(name, type) name,
#include "il/runtime/RuntimeSigs.def"
#undef SIG
    Count,
};

/// @brief Enumerates optional runtime helpers that lowering may request.
enum class RuntimeFeature : std::size_t {
    Concat,
    CsvQuote,
    InputLine,
    SplitFields,
    ToInt,
    ToDouble,
    ParseInt64,
    ParseDouble,
    CintFromDouble,
    ClngFromDouble,
    CsngFromDouble,
    CdblFromAny,
    IntToStr,
    F64ToStr,
    StrFromI16,
    StrFromI32,
    StrFromSingle,
    StrFromDouble,
    Val,
    Alloc,
    StrEq,
    StrLt,
    StrLe,
    StrGt,
    StrGe,
    Left,
    Right,
    Mid2,
    Mid3,
    Instr2,
    Instr3,
    Ltrim,
    Rtrim,
    Trim,
    Ucase,
    Lcase,
    Chr,
    Asc,
    ByteAt,
    Sqrt,
    AbsI64,
    AbsF64,
    Floor,
    Ceil,
    Sin,
    Cos,
    Tan,
    Atan,
    Exp,
    Log,
    SgnI64,
    SgnF64,
    Pow,
    IntFloor,
    FixTrunc,
    RoundEven,
    RandomizeI64,
    Rnd,
    Timer,
    TermCls,
    TermBell,
    TermColor,
    TermLocate,
    TermCursor,
    TermAltScreen,
    TermBeginBatch,
    TermEndBatch,
    TermFlush,
    InKey,
    GetKey,
    GetKeyTimeout,
    KeyPressed,
    ArgsCount,
    ArgsGet,
    Cmdline,
    ObjNew,
    ObjRetainMaybe,
    ObjReleaseChk0,
    ObjFree,
    Count,
};

/// @brief Lowering requirement classification for runtime helpers.
enum class RuntimeLoweringKind {
    Always,
    BoundsChecked,
    Feature,
    Manual,
};

/// @brief Metadata describing when lowering should declare a runtime helper.
struct RuntimeLowering {
    RuntimeLoweringKind kind{RuntimeLoweringKind::Manual}; ///< Dispatch strategy.
    RuntimeFeature feature{
        RuntimeFeature::Count}; ///< Feature identifier when @ref kind is Feature.
    bool ordered{false};        ///< Preserve request order when true.
};

/// @brief Handler invocation adapter signature used by the VM bridge.
using RuntimeHandler = void (*)(void **args, void *result);

/// @brief Enumerates hidden bridge-argument categories.
enum class RuntimeHiddenParamKind {
    None,
    PowStatusPointer,
};

/// @brief Hidden runtime argument injected by the VM bridge.
struct RuntimeHiddenParam {
    RuntimeHiddenParamKind kind{
        RuntimeHiddenParamKind::None}; ///< Classification for bridge marshalling.
};

/// @brief Classification guiding runtime trap handling for helper invocations.
enum class RuntimeTrapClass {
    None,
    PowDomainOverflow,
};

/// @brief Describes the complete IL-visible and bridge-visible runtime ABI.
/// @details Parameter order matches the runtime C ABI. Hidden parameters are
///          appended by the bridge and are not included in ownership bitmasks.
struct RuntimeSignature {
    il::core::Type retType;                       ///< Return type of the helper.
    std::vector<il::core::Type> paramTypes;       ///< Parameter types in declaration order.
    std::vector<RuntimeHiddenParam> hiddenParams; ///< Hidden arguments appended by the bridge.
    RuntimeTrapClass trapClass{RuntimeTrapClass::None}; ///< Trap semantics for the helper.
    bool nothrow{false};                                ///< Helper is guaranteed not to throw.
    bool readonly{false};             ///< Helper may read from memory without writes.
    bool pure{false};                 ///< Helper has no observable side effects.
    bool isVarArg{false};             ///< Helper uses C-style variadic arguments.
    std::uint64_t objectParamMask{0}; ///< Parameters spelled `obj` in the runtime surface.
    std::uint64_t consumedArgMask{0}; ///< IL-visible args whose ownership is consumed.
    std::uint64_t retainedArgMask{0}; ///< IL-visible args whose reference count is retained.
    std::uint64_t ownedOutArgMask{0}; ///< Pointer args that receive an owned reference.
    bool returnsOwned{false};         ///< Helper returns an owned reference/string handle.
    bool mayAllocate{false};          ///< Helper may allocate runtime-managed storage.
    bool returnsKnownObject{false};   ///< Result supports object-specific RC helpers.
    bool valid{true};                 ///< Parsed signature text was well-formed and supported.
};

/// @brief Aggregated descriptor covering signature, handler, and lowering metadata.
struct RuntimeDescriptor {
    std::string_view name;           ///< Canonical name exported through the runtime registry.
    std::string_view cSymbol;        ///< Backing C symbol, empty for hand-authored internal rows.
    std::string_view signatureText;  ///< Public runtime.def signature spelling.
    RuntimeSignature signature;      ///< Canonical IL signature for the helper.
    RuntimeHandler handler{nullptr}; ///< Adapter that invokes the C implementation.
    RuntimeLowering lowering;        ///< Lowering metadata controlling declaration.
    RuntimeTrapClass trapClass{RuntimeTrapClass::None}; ///< Trap classification for VM bridge.
    bool publicSurface{true}; ///< Visible as a standalone frontend runtime function.
};

/// @brief Access the ordered registry of runtime descriptors.
/// @return Stable list of descriptors mirroring the runtime ABI.
const std::vector<RuntimeDescriptor> &runtimeRegistry();

/// @brief Lookup runtime descriptor by exported symbol name.
/// @param name Canonical or native exported runtime symbol.
/// @return Descriptor pointer when registered, nullptr otherwise.
const RuntimeDescriptor *findRuntimeDescriptor(std::string_view name);

/// @brief Lookup runtime descriptor by lowering feature identifier.
/// @param feature Lowering feature whose providing helper is requested.
/// @return Descriptor pointer when @p feature is published, nullptr otherwise.
const RuntimeDescriptor *findRuntimeDescriptor(RuntimeFeature feature);

/// @brief Access the registry of runtime signatures keyed by symbol name.
/// @return Mapping from runtime symbol to its IL signature metadata.
const std::unordered_map<std::string_view, RuntimeSignature> &runtimeSignatures();

/// @brief Look up the signature for a runtime helper if it exists.
/// @param name Runtime symbol name, e.g., "rt_print_str".
/// @return Pointer to the signature when registered; nullptr otherwise.
const RuntimeSignature *findRuntimeSignature(std::string_view name);

/// @brief Look up the generated runtime signature identifier for a symbol.
/// @param name Runtime symbol name to query.
/// @return Runtime signature identifier when tracked, std::nullopt otherwise.
std::optional<RtSig> findRuntimeSignatureId(std::string_view name);

/// @brief Retrieve signature metadata by generated runtime signature identifier.
/// @param sig Enumerated runtime signature identifier.
/// @return Pointer to the signature metadata when defined; nullptr otherwise.
const RuntimeSignature *findRuntimeSignature(RtSig sig);

/// @brief Check whether a callee uses C-style variadic arguments.
/// @details Consults the runtime registry first; falls back to a hardcoded list
///          of known C library functions (e.g., rt_snprintf, rt_sb_printf) that
///          are not registered but require vararg calling convention.
/// @param name Symbol name of the callee to query.
/// @return True when the callee is known to be variadic.
[[nodiscard]] bool isVarArgCallee(std::string_view name);

/// @brief Perform a lightweight self-check on runtime descriptors.
/// @details Validates that no duplicate descriptor names exist and that each
///          descriptor's parameter count matches its signature. This check runs
///          once per process and is lightweight enough for release builds.
///          In debug builds, also runs the full validation against the whitelist.
///
///          The check is idempotent and thread-safe; multiple calls return the
///          cached result from the first invocation.
///
/// @note **For embedders**: Call this function early in your application's
///       startup sequence to verify runtime integrity before executing IL code.
///       The VM calls this automatically during initialization, but embedders
///       using the runtime library directly should call it explicitly.
///
/// @note The VM startup path in VMInit.cpp calls this function and aborts if
///       it returns false, ensuring mismatches are caught before any IL execution.
///
/// @return True if all checks pass, false otherwise. In debug builds, asserts
///         on failure. In release builds, logs errors to stderr and returns false.
[[nodiscard]] bool selfCheckRuntimeDescriptors();

/// @brief Mode controlling how runtime signature invariant violations are reported.
enum class InvariantViolationMode {
    Abort, ///< Default: call std::abort() immediately.
    Trap,  ///< Use registered trap handler when available, otherwise abort.
};

/// @brief Set the invariant violation mode for runtime signature checks.
/// @details When set to `Trap`, violations of runtime signature invariants
///          will be reported through the registered trap handler, allowing
///          the VM's exception handling to process them. If no handler is
///          registered or it returns, falls back to abort behavior.
///          Default mode is `Abort` for maximum strictness.
/// @param mode The desired violation reporting mode.
void setInvariantViolationMode(InvariantViolationMode mode);

/// @brief Query the current invariant violation mode.
/// @return The active mode for reporting signature invariant violations.
[[nodiscard]] InvariantViolationMode getInvariantViolationMode();

/// @brief Type signature for invariant violation trap handlers.
/// @details A trap handler receives the error message and should either:
///          1. Raise a trap through the VM (which may be caught by exception handlers)
///          2. Return false to indicate the trap was not handled (caller will abort)
/// @param message Human-readable description of the invariant violation.
/// @return True if the trap was handled (execution may continue via exception handler),
///         false if the handler could not process the trap (caller will abort).
using InvariantTrapHandler = bool (*)(const char *message);

/// @brief Register a trap handler for invariant violations.
/// @details The VM layer calls this during initialization to register its trap
///          mechanism. When InvariantViolationMode is Trap and a violation occurs,
///          the registered handler is invoked. If the handler returns false or
///          no handler is registered, the violation falls back to abort behavior.
/// @param handler Function pointer to the trap handler, or nullptr to unregister.
void setInvariantTrapHandler(InvariantTrapHandler handler);

/// @brief Query the currently registered trap handler.
/// @return The registered handler, or nullptr if none is registered.
[[nodiscard]] InvariantTrapHandler getInvariantTrapHandler();

/// @brief Report a runtime signature invariant violation.
/// @details Behavior depends on the current InvariantViolationMode:
///          - Abort: Logs the error and calls std::abort().
///          - Trap: Invokes the registered trap handler. If no handler is
///            registered or it returns false, falls back to abort.
/// @param message Human-readable description of the invariant violation.
/// @note This function does not return when mode is Abort, no handler is
///       registered, or the handler returns false.
[[noreturn]] void reportInvariantViolation(const char *message);

} // namespace il::runtime
