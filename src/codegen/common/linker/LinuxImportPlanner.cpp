//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/LinuxImportPlanner.cpp
// Purpose: Linux ELF dynamic import classification for the native linker.
//          Maps each undefined dynamic symbol to one of a fixed set of libraries
//          (libc, libm, libdl, libpthread, libX11, libasound) and produces the
//          deduplicated DT_NEEDED list for the ELF executable writer.
// Key invariants:
//   - The lookup table is fully baked-in; no filesystem access.
//   - Symbol validation is lexicographically ordered so diagnostics never
//     depend on unordered-container iteration.
//   - DT_NEEDED order follows a stable ABI-conventional sequence: libc.so.6 is
//     always first (when present) so unresolved C runtime symbols satisfy.
//   - Returning false clears @p plan, so callers never observe a stale or
//     partially populated DT_NEEDED list after a fatal link error.
// Ownership/Lifetime: stateless — caller owns the populated LinuxImportPlan.
// Links: codegen/common/linker/PlatformImportPlanner.hpp,
//        codegen/common/linker/ElfExeWriter.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file LinuxImportPlanner.cpp
 * @brief Implements deterministic Linux symbol-to-`DT_NEEDED` classification.
 */

#include "codegen/common/linker/DynamicSymbolPolicy.hpp"
#include "codegen/common/linker/PlatformImportPlanner.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {
namespace {

/// @brief Closed set of shared objects the built-in Linux import policy may request.
enum class LinuxNeededLib : uint8_t {
    LibC,
    LibM,
    LibDL,
    LibPthread,
    LibStdCpp,
    LibGccS,
    LibGL,
    LibX11,
    LibASound,
};

/// @brief Maps an import-library category to its Linux SONAME.
/// @param lib Library category.
/// @return Process-lifetime SONAME string; an invalid enum conservatively maps
///         to `libc.so.6`.
const char *linuxNeededLibName(LinuxNeededLib lib) {
    switch (lib) {
        case LinuxNeededLib::LibC:
            return "libc.so.6";
        case LinuxNeededLib::LibM:
            return "libm.so.6";
        case LinuxNeededLib::LibDL:
            return "libdl.so.2";
        case LinuxNeededLib::LibPthread:
            return "libpthread.so.0";
        case LinuxNeededLib::LibStdCpp:
            return "libstdc++.so.6";
        case LinuxNeededLib::LibGccS:
            return "libgcc_s.so.1";
        case LinuxNeededLib::LibGL:
            return "libGL.so.1";
        case LinuxNeededLib::LibX11:
            return "libX11.so.6";
        case LinuxNeededLib::LibASound:
            return "libasound.so.2";
    }
    return "libc.so.6";
}

/// @brief Tests the baked-in libm function set.
/// @param name Normalized ELF symbol name.
/// @return `true` when libm is the expected provider.
bool isLinuxMathSymbol(const std::string &name) {
    static const std::unordered_set<std::string> kMath = {
        "acos",  "acosf",  "asin",   "asinf",  "atan",     "atan2",     "atan2f", "atanf",
        "cbrt",  "cbrtf",  "ceil",   "ceilf",  "copysign", "copysignf", "cos",    "cosf",
        "cosh",  "exp",    "exp10",  "expf",   "fabs",     "fabsf",     "floor",  "floorf",
        "fmax",  "fmaxf",  "fmin",   "fminf",  "fmod",     "fmodf",     "hypot",  "ldexp",
        "log",   "log10",  "log2",   "logf",   "nan",      "pow",       "powf",   "rint",
        "round", "roundf", "lrint",  "lrintf", "llrint",   "sin",       "sinf",   "sinh",
        "sqrt",  "sqrtf",  "tan",    "tanf",   "tanh",     "trunc",     "truncf", "atan2l",
        "ceill", "cosl",   "floorl", "fmaxl",  "fminl",    "fmodl",     "sinl",   "sqrtl",
    };
    return kMath.count(name) != 0;
}

/// @brief Tests the supported dynamic-loader API names.
/// @param name Normalized ELF symbol name.
/// @return `true` for `dlopen`, `dlsym`, `dlclose`, or `dlerror`.
bool isLinuxDlSymbol(const std::string &name) {
    return name == "dlopen" || name == "dlsym" || name == "dlclose" || name == "dlerror";
}

/// @brief Tests for known Itanium C++ ABI and libstdc++ symbols.
/// @param name ELF symbol name, including normal Itanium leading underscores.
/// @return `true` when libstdc++ is the expected provider.
bool isLinuxCppRuntimeSymbol(const std::string &name) {
    static const std::unordered_set<std::string> kExact = {
        "__cxa_begin_catch",
        "__cxa_end_catch",
        "__cxa_rethrow",
        "__gxx_personality_v0",
        "__once_proxy",
    };
    if (kExact.count(name) != 0)
        return true;

    static const char *const kPrefixes[] = {
        "_ZNSt",
        "_ZNKSt",
        "_ZNKRSt",
        "_ZNSi",
        "_ZNSo",
        "_ZTINSt",
        "_ZTSNSt",
        "_ZTVNSt",
        "_ZSt",
        "_ZTISt",
        "_ZTSSt",
        "_ZTVSt",
        "_Zda",
        "_Zdl",
        "_Zna",
        "_Znw",
    };
    for (const char *prefix : kPrefixes) {
        if (name.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

/// @brief Tests for known libgcc/compiler-runtime helper functions.
/// @param name ELF symbol name.
/// @return `true` when libgcc_s is the expected provider.
bool isLinuxCompilerRuntimeSymbol(const std::string &name) {
    static const std::unordered_set<std::string> kExact = {
        "__addtf3",      "__divtf3",     "__eqtf2",     "__extenddftf2", "__fixtfdi",
        "__fixtfsi",     "__fixunstfdi", "__floatditf", "__floatsitf",   "__floatunditf",
        "__floatunsitf", "__getf2",      "__gttf2",     "__letf2",       "__lttf2",
        "__multf3",      "__netf2",      "__subtf3",    "__trunctfdf2",
    };
    return kExact.count(name) != 0;
}

/// @brief Tests the constrained OpenGL/GLX `gl` plus uppercase symbol family.
/// @param name ELF symbol name.
/// @return `true` for names routed to libGL; lowercase libc names such as
///         `glob` deliberately do not match.
bool isLinuxGlSymbol(const std::string &name) {
    return name.size() > 2 && name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z';
}

/// @brief Tests constrained X11 symbol families.
/// @param name ELF symbol name.
/// @return `true` for `X` plus uppercase, `Xutf8*`, `Xkb*`, or `Xrm*`.
/// @details Matching narrowly prevents unrelated `X*` user symbols from
///          silently adding libX11 to non-GUI programs.
bool isLinuxX11Symbol(const std::string &name) {
    if (name.size() < 2 || name[0] != 'X')
        return false;
    if (name[1] >= 'A' && name[1] <= 'Z')
        return true;
    return name.rfind("Xutf8", 0) == 0 || name.rfind("Xkb", 0) == 0 ||
           name.rfind("Xrm", 0) == 0;
}

/// @brief Selects the expected shared-object category for a validated import.
/// @param name ELF dynamic symbol name.
/// @return Specialized library category when recognized, otherwise libc.
LinuxNeededLib classifyLinuxImportLibrary(const std::string &name) {
    if (name.rfind("snd_", 0) == 0)
        return LinuxNeededLib::LibASound;
    if (isLinuxGlSymbol(name))
        return LinuxNeededLib::LibGL;
    if (isLinuxX11Symbol(name))
        return LinuxNeededLib::LibX11;
    if (isLinuxDlSymbol(name))
        return LinuxNeededLib::LibDL;
    if (name.rfind("pthread_", 0) == 0)
        return LinuxNeededLib::LibPthread;
    if (name.rfind("_Unwind_", 0) == 0)
        return LinuxNeededLib::LibGccS;
    if (isLinuxCompilerRuntimeSymbol(name))
        return LinuxNeededLib::LibGccS;
    if (isLinuxCppRuntimeSymbol(name))
        return LinuxNeededLib::LibStdCpp;
    if (isLinuxMathSymbol(name))
        return LinuxNeededLib::LibM;
    return LinuxNeededLib::LibC;
}

} // namespace

/// @copydoc planLinuxImports(const std::unordered_set<std::string> &, LinuxImportPlan &, std::ostream &)
bool planLinuxImports(const std::unordered_set<std::string> &dynamicSyms,
                      LinuxImportPlan &plan,
                      std::ostream &err) {
    plan.neededLibs.clear();

    std::vector<std::string> orderedSymbols(dynamicSyms.begin(), dynamicSyms.end());
    std::sort(orderedSymbols.begin(), orderedSymbols.end());

    std::unordered_set<LinuxNeededLib> libs = {LinuxNeededLib::LibC};
    for (const auto &sym : orderedSymbols) {
        if (!isKnownDynamicSymbol(sym, LinkPlatform::Linux)) {
            err << "error: unrecognized Linux dynamic import '" << sym << "'\n";
            plan.neededLibs.clear();
            return false;
        }
        libs.insert(classifyLinuxImportLibrary(sym));
    }

    static constexpr LinuxNeededLib kOrder[] = {
        LinuxNeededLib::LibC,
        LinuxNeededLib::LibM,
        LinuxNeededLib::LibDL,
        LinuxNeededLib::LibPthread,
        LinuxNeededLib::LibStdCpp,
        LinuxNeededLib::LibGccS,
        LinuxNeededLib::LibGL,
        LinuxNeededLib::LibX11,
        LinuxNeededLib::LibASound,
    };
    for (LinuxNeededLib lib : kOrder) {
        if (libs.count(lib) != 0)
            plan.neededLibs.push_back(linuxNeededLibName(lib));
    }
    return true;
}

} // namespace zanna::codegen::linker
