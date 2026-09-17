//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_mangle.cpp
// Purpose: Exercise common linker-symbol mangling collision and readability rules.
// Key invariants: Plain entry-point symbols stay readable, while qualified or
//                 escaped names use a reversible reserved-prefix encoding.
// Ownership/Lifetime: Standalone unit test binary.
// Links: src/common/Mangle.hpp
//
//===----------------------------------------------------------------------===//

#include "common/Mangle.hpp"

#include <cassert>
#include <string>

/// @brief Validate the common linker-symbol mangling contract.
/// @details The test intentionally checks both compatibility and collision
///          avoidance: simple symbols such as @c main remain plain for native
///          toolchain entry points, reserved-prefix user symbols are escaped,
///          dotted/underscore/special names no longer collapse to the same
///          linker symbol, and names spelled like C runtime functions get a
///          guarded spelling that keeps them out of the C symbol namespace.
/// @return Zero when all invariants hold.
int main() {
    using zanna::common::DemangleLink;
    using zanna::common::GuardReservedRuntimeName;
    using zanna::common::IsReservedRuntimeName;
    using zanna::common::MangleLink;

    assert(MangleLink("main") == "main");
    assert(DemangleLink("main") == "main");

    const std::string dotted = MangleLink("A.B");
    const std::string underscored = MangleLink("A_B");
    const std::string dashed = MangleLink("A-B");
    assert(dotted != underscored);
    assert(dotted != dashed);
    assert(underscored != dashed);

    assert(DemangleLink(dotted) == "a.b");
    assert(DemangleLink(underscored) == "a_b");
    assert(DemangleLink(dashed) == "a-b");

    const std::string reserved = MangleLink("vpr_user_symbol");
    assert(reserved != "vpr_user_symbol");
    assert(DemangleLink(reserved) == "vpr_user_symbol");

    assert(DemangleLink("@legacy_symbol") == "legacy.symbol");

    // A user function spelled like a C runtime symbol must not claim that
    // symbol: a natively linked program shares one namespace with libc/libm, so
    // a plain `floor` definition would be called by the runtime in place of
    // libm's own. Codegen renames such definitions before emission.
    for (const char *libcName : {"floor",
                                 "abs",
                                 "ceil",
                                 "sqrt",
                                 "poll",
                                 "random",
                                 "exit",
                                 "malloc",
                                 "free",
                                 "memcpy",
                                 "printf",
                                 "read",
                                 "write"}) {
        assert(IsReservedRuntimeName(libcName));
        const std::string guarded = GuardReservedRuntimeName(libcName);
        assert(guarded != libcName);
        // The guarded spelling must survive MangleLink untouched, so the text
        // and binary emitters agree on one symbol.
        assert(MangleLink(guarded) == guarded);
        // Guarding is idempotent.
        assert(GuardReservedRuntimeName(guarded) == guarded);
    }

    // Case folding happens before the reserved-name check.
    assert(IsReservedRuntimeName("Floor"));
    assert(GuardReservedRuntimeName("Floor") == GuardReservedRuntimeName("floor"));

    // Distinct reserved names stay distinct.
    assert(GuardReservedRuntimeName("floor") != GuardReservedRuntimeName("abs"));

    // Ordinary names, the native entry point, and lookalikes are untouched, so
    // symbols stay readable in profilers and debuggers.
    for (const char *plain :
         {"main", "generate", "shadow", "floors", "absolute", "rt_pixels_new"}) {
        assert(!IsReservedRuntimeName(plain));
        assert(GuardReservedRuntimeName(plain) == plain);
    }
    assert(MangleLink("main") == "main");
    assert(MangleLink("floor") == "floor");

    return 0;
}
