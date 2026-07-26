//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/ZiaSupport.hpp
// Purpose: Small always-available support utilities shared across the Zia
//          frontend: an index sentinel, a safe signed->index conversion, and an
//          always-on invariant assertion macro.
// Key invariants:
//   - kInvalidIndex is the single canonical "no index" sentinel for size_t.
//   - toIndex() never produces a wrapped (huge) index from a negative input;
//     it traps instead, converting a latent OOB into a controlled abort.
//   - ZANNA_ZIA_ASSERT fires in ALL build configurations (unlike <cassert>,
//     which is disabled under NDEBUG), so frontend invariants are enforced in
//     release builds too.
// Ownership/Lifetime:
//   - Header-only; no state.
// Links: frontends/zia/Lowerer.hpp, frontends/zia/Sema.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines allocation-conscious naming, safe index conversion, and
///        always-on invariant checks shared by the Zia frontend.
/// @details The header is stateless and keeps release builds from silently
///          accepting negative-index wrapping or violated lowering invariants.

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace il::frontends::zia {

/// @brief Build "<base>_<id>" with a single heap allocation.
/// @details Replaces `base + "_" + std::to_string(id)`, which allocates two or
///          three temporaries. Uses std::to_chars (no allocation) for the digits
///          and reserves the exact final size.
/// @param base Stem copied before the separator.
/// @param id Unsigned decimal suffix value.
/// @return A string containing @p base, an underscore, and @p id in decimal.
inline std::string makeSuffixedName(std::string_view base, unsigned long long id) {
    char digits[24];
    auto result = std::to_chars(digits, digits + sizeof(digits), id);
    std::string out;
    out.reserve(base.size() + 1 + static_cast<std::size_t>(result.ptr - digits));
    out.append(base.data(), base.size());
    out.push_back('_');
    out.append(digits, result.ptr);
    return out;
}

/// @brief Canonical "no index" sentinel for size_t-valued indices.
/// @details Use in place of ad-hoc `static_cast<size_t>(-1)` so the intent is
///          named and greppable.
inline constexpr std::size_t kInvalidIndex = static_cast<std::size_t>(-1);

/// @brief Report a failed invariant and abort.
/// @param cond Stringized failing condition.
/// @param msg  Human-readable description of the invariant.
/// @param file Source file of the failing check.
/// @param line Source line of the failing check.
/// @details Always active (not gated on NDEBUG). Keeping invariant checks live
///          in release builds turns silent corruption (e.g. a wrapped index)
///          into a deterministic, diagnosable failure.
[[noreturn]] inline void ziaAssertFail(const char *cond,
                                       const char *msg,
                                       const char *file,
                                       int line) {
    std::fprintf(stderr, "Zia invariant violated: %s (%s) at %s:%d\n",
                 msg ? msg : "", cond ? cond : "", file ? file : "?", line);
    std::abort();
}

/// @brief Convert a signed value to a size_t index, trapping on negatives.
/// @param value Signed index (e.g. an overload-binding source slot).
/// @param context Short label used in the abort message.
/// @return @p value as size_t when non-negative.
/// @details A negative input would otherwise wrap to a near-SIZE_MAX value and
///          index far out of bounds. Trapping converts that latent
///          memory-safety bug into a controlled abort.
inline std::size_t toIndex(long long value, const char *context = "index") {
    if (value < 0)
        ziaAssertFail("value >= 0", context, __FILE__, __LINE__);
    return static_cast<std::size_t>(value);
}

} // namespace il::frontends::zia

/// @brief Always-on invariant assertion for the Zia frontend.
/// @details Unlike `assert()` from <cassert>, this is NOT compiled out under
///          NDEBUG. Use for invariants that must hold in every build (scope
///          stack non-empty, index < size before a cast, etc.).
/// @param cond Condition that must evaluate to true.
/// @param msg Human-readable explanation reported when @p cond is false.
#define ZANNA_ZIA_ASSERT(cond, msg)                                                                \
    do {                                                                                           \
        if (!(cond))                                                                               \
            ::il::frontends::zia::ziaAssertFail(#cond, (msg), __FILE__, __LINE__);                 \
    } while (0)
