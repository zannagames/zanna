//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the verifier responsible for tracking module-level global
// declarations.  The translation unit builds and maintains a map from global
// names to their definitions so duplicate declarations can be diagnosed quickly
// while giving other passes a cheap lookup structure.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Verification helpers for enforcing global declaration uniqueness.
/// @details The @ref GlobalVerifier caches pointers to module-owned globals in a
///          dense lookup table.  Subsequent queries avoid repeated scans of the
///          module vector while ensuring every symbol is unique within the
///          translation unit.

#include "il/verify/GlobalVerifier.hpp"

#include "il/core/Global.hpp"
#include "il/core/Module.hpp"
#include "il/internal/io/ParserUtil.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>

using namespace il::core;

namespace il::verify {
namespace {
using il::support::Expected;
using il::support::makeError;

/// @brief Parse an IL integer initializer through the shared literal parser.
/// @param text Initializer text.
/// @param out Output value set on successful parsing.
/// @return `true` when the complete literal is a valid signed integer.
bool parseInteger(std::string_view text, long long &out) {
    return il::io::parseIntegerLiteral(std::string{text}, out);
}

/// @brief Parse an IL floating-point initializer through the shared literal parser.
/// @param text Initializer text.
/// @param out Output value set on successful parsing.
/// @return `true` when the complete literal is a valid double.
bool parseFloat(std::string_view text, double &out) {
    return il::io::parseFloatLiteral(std::string{text}, out);
}

/// @brief Validate one global declaration independently of name uniqueness.
/// @details Checks identifier syntax, excludes verifier-only types, enforces
///          import and string-initializer rules, and parses scalar initializers
///          according to the declared type and range.
/// @param global Declaration to inspect.
/// @return Success, or the first name, type, linkage, or initializer diagnostic.
Expected<void> validateGlobal(const Global &global) {
    if (global.name.empty())
        return Expected<void>{makeError({}, "global has empty name")};
    if (!il::io::isValidILIdentifier(global.name))
        return Expected<void>{makeError({}, "global has malformed name @" + global.name)};

    if (global.type.kind == Type::Kind::Void || global.type.kind == Type::Kind::Error ||
        global.type.kind == Type::Kind::ResumeTok) {
        return Expected<void>{makeError(
            {}, "global @" + global.name + " has unsupported type " + global.type.toString())};
    }

    if (global.linkage == Linkage::Import && !global.init.empty()) {
        return Expected<void>{
            makeError({}, "import global @" + global.name + " must not have an initializer")};
    }

    if (global.type.kind == Type::Kind::Str) {
        if (global.linkage != Linkage::Import && !global.hasInitializer)
            return Expected<void>{
                makeError({}, "string global @" + global.name + " requires an initializer")};
        return {};
    }

    if (global.init.empty())
        return {};

    long long intValue = 0;
    switch (global.type.kind) {
        case Type::Kind::I1:
            if (!parseInteger(global.init, intValue) || (intValue != 0 && intValue != 1)) {
                return Expected<void>{
                    makeError({}, "global @" + global.name + " initializer must be i1 0 or 1")};
            }
            return {};
        case Type::Kind::I16:
            if (!parseInteger(global.init, intValue) ||
                intValue < std::numeric_limits<int16_t>::min() ||
                intValue > std::numeric_limits<int16_t>::max()) {
                return Expected<void>{
                    makeError({}, "global @" + global.name + " initializer out of range for i16")};
            }
            return {};
        case Type::Kind::I32:
            if (!parseInteger(global.init, intValue) ||
                intValue < std::numeric_limits<int32_t>::min() ||
                intValue > std::numeric_limits<int32_t>::max()) {
                return Expected<void>{
                    makeError({}, "global @" + global.name + " initializer out of range for i32")};
            }
            return {};
        case Type::Kind::I64:
            if (!parseInteger(global.init, intValue)) {
                return Expected<void>{
                    makeError({}, "global @" + global.name + " initializer must be an integer")};
            }
            return {};
        case Type::Kind::F64: {
            double floatValue = 0.0;
            if (!parseFloat(global.init, floatValue)) {
                return Expected<void>{
                    makeError({}, "global @" + global.name + " initializer must be f64")};
            }
            return {};
        }
        case Type::Kind::Ptr:
            if (global.init != "null") {
                return Expected<void>{
                    makeError({}, "global @" + global.name + " ptr initializer must be null")};
            }
            return {};
        case Type::Kind::Void:
        case Type::Kind::Str:
        case Type::Kind::Error:
        case Type::Kind::ResumeTok:
            break;
    }
    return {};
}
} // namespace

/// @brief Expose the cached map from global names to module-owned definitions.
/// @details The verifier records raw pointers to the immutable @ref il::core::Global
///          instances stored inside the module so downstream passes can perform
///          O(1) lookups without rebuilding the index.  The returned reference is
///          valid until the verifier mutates its map; the pointed-to globals
///          additionally require the source module to remain alive and unmoved.
/// @return Const reference to the current name index.
[[nodiscard]] const GlobalVerifier::GlobalMap &GlobalVerifier::globals() const {
    return globals_;
}

/// @brief Populate the lookup map and detect duplicate declarations.
/// @details Clears any previous state, iterates over every global declared in the
///          module, and inserts its address into @ref globals_.  Duplicate names
///          trigger an error result. The sink parameter is reserved for
///          interface consistency and is not currently written.
/// @param module Module whose globals should be indexed.
/// @param sink Reserved diagnostic sink.
/// @return Empty result on success or a diagnostic for the first invalid global.
Expected<void> GlobalVerifier::run(const Module &module, [[maybe_unused]] DiagSink &sink) {
    globals_.clear();

    for (const auto &global : module.globals) {
        if (auto result = validateGlobal(global); !result)
            return result;
        if (!globals_.emplace(global.name, &global).second)
            return Expected<void>{makeError({}, "duplicate global @" + global.name)};
    }

    return {};
}

} // namespace il::verify
