//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/codegen/linker/test_platform_import_planners.cpp
// Purpose: Targeted unit coverage for per-platform native-link import planners.
// Key invariants:
//   - Every accepted platform import maps to the DLL/framework that exports it.
//   - Platform-exclusive symbols are rejected for foreign targets.
//   - Planner failure never leaves a partial import plan.
//   - Diagnostics are stable when multiple unordered imports are invalid.
// Ownership/Lifetime:
//   - Test-owned plans and object files live for one test case.
// Links: src/codegen/common/linker/PlatformImportPlanner.hpp,
//        src/codegen/common/linker/DynamicSymbolPolicy.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/common/linker/DynamicSymbolPolicy.hpp"
#include "codegen/common/linker/PlatformImportPlanner.hpp"
#include "tests/TestHarness.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_set>

using namespace zanna::codegen::linker;

namespace {

template <typename T> bool contains(const std::vector<T> &items, const T &value) {
    return std::find(items.begin(), items.end(), value) != items.end();
}

bool importPlanHasDll(const WindowsImportPlan &plan, const std::string &dll) {
    return std::any_of(plan.imports.begin(), plan.imports.end(), [&](const DllImport &entry) {
        return entry.dllName == dll;
    });
}

bool importPlanDllHasFunction(const WindowsImportPlan &plan,
                              const std::string &dll,
                              const std::string &function) {
    for (const auto &entry : plan.imports) {
        if (entry.dllName == dll)
            return contains(entry.functions, function);
    }
    return false;
}

bool objHasSymbol(const ObjFile &obj, const std::string &name) {
    return std::any_of(obj.symbols.begin(), obj.symbols.end(), [&](const ObjSymbol &sym) {
        return sym.name == name;
    });
}

uint32_t dylibOrdinalForPath(const MacImportPlan &plan, const std::string &path) {
    for (size_t i = 0; i < plan.dylibs.size(); ++i) {
        if (plan.dylibs[i].path == path)
            return static_cast<uint32_t>(i + 1);
    }
    return 0;
}

} // namespace

TEST(PlatformImportPlanners, LinuxPlannerClassifiesNeededLibraries) {
    LinuxImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planLinuxImports({"cbrtf",
                                  "cos",
                                  "dlopen",
                                  "exp10",
                                  "ftruncate",
                                  "getpwuid_r",
                                  "pipe2",
                                  "pthread_create",
                                  "XOpenDisplay",
                                  "snd_pcm_open",
                                  "__once_proxy"},
                                 plan,
                                 err));
    EXPECT_EQ(std::vector<std::string>({"libc.so.6",
                                        "libm.so.6",
                                        "libdl.so.2",
                                        "libpthread.so.0",
                                        "libstdc++.so.6",
                                        "libX11.so.6",
                                        "libasound.so.2"}),
              plan.neededLibs);
}

TEST(PlatformImportPlanners, LinuxPlannerRejectsUnknownImportsWithoutPartialPlan) {
    LinuxImportPlan plan;
    plan.neededLibs = {"stale.so"};
    std::ostringstream err;
    EXPECT_FALSE(planLinuxImports({"malloc", "zanna_missing_linux_symbol"}, plan, err));
    EXPECT_TRUE(plan.neededLibs.empty());
    EXPECT_NE(err.str().find("unrecognized Linux dynamic import 'zanna_missing_linux_symbol'"),
              std::string::npos);
}

TEST(PlatformImportPlanners, LinuxPlannerReportsUnknownImportsDeterministically) {
    LinuxImportPlan plan;
    std::ostringstream err;
    EXPECT_FALSE(
        planLinuxImports({"zanna_unknown_zeta", "malloc", "zanna_unknown_alpha"}, plan, err));
    EXPECT_TRUE(plan.neededLibs.empty());
    EXPECT_EQ("error: unrecognized Linux dynamic import 'zanna_unknown_alpha'\n", err.str());
}

TEST(PlatformImportPlanners, MacPlannerMapsFrameworkAndFlatLookupSymbols) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planMacImports(
        {"CFStringCreateWithCString", "_OBJC_CLASS_$_NSApplication", "dispatch_async", "fsync"},
        plan,
        err));

    EXPECT_TRUE(std::any_of(plan.dylibs.begin(), plan.dylibs.end(), [](const DylibImport &import) {
        return import.path == "/usr/lib/libSystem.B.dylib";
    }));
    EXPECT_TRUE(std::any_of(plan.dylibs.begin(), plan.dylibs.end(), [](const DylibImport &import) {
        return import.path.find("CoreFoundation.framework") != std::string::npos;
    }));
    ASSERT_TRUE(plan.symOrdinals.count("_OBJC_CLASS_$_NSApplication") != 0);
    EXPECT_EQ(0u, plan.symOrdinals["_OBJC_CLASS_$_NSApplication"]);
    ASSERT_TRUE(plan.symOrdinals.count("dispatch_async") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["dispatch_async"]);
    ASSERT_TRUE(plan.symOrdinals.count("fsync") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["fsync"]);
}

/// @brief Verify AppKit accessibility constants and posting functions resolve to AppKit.
/// @details The GUI semantic bridge references exported NSAccessibility string constants in
///          addition to the posting functions. The native linker must assign all of them the
///          AppKit dylib ordinal instead of rejecting the final Zanna Studio link as unmapped.
TEST(PlatformImportPlanners, MacPlannerMapsAccessibilitySymbolsToAppKit) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planMacImports({"NSAccessibilityAnnouncementKey",
                                "NSAccessibilityPriorityKey",
                                "NSAccessibilityAnnouncementRequestedNotification",
                                "NSAccessibilityPostNotificationWithUserInfo",
                                "NSAccessibilityGroupRole"},
                               plan,
                               err));

    const uint32_t appKit =
        dylibOrdinalForPath(plan, "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit");
    ASSERT_TRUE(appKit != 0);
    EXPECT_EQ(appKit, plan.symOrdinals["NSAccessibilityAnnouncementKey"]);
    EXPECT_EQ(appKit, plan.symOrdinals["NSAccessibilityPriorityKey"]);
    EXPECT_EQ(appKit, plan.symOrdinals["NSAccessibilityAnnouncementRequestedNotification"]);
    EXPECT_EQ(appKit, plan.symOrdinals["NSAccessibilityPostNotificationWithUserInfo"]);
    EXPECT_EQ(appKit, plan.symOrdinals["NSAccessibilityGroupRole"]);
}

TEST(PlatformImportPlanners, MacPlannerMapsMachineAndHostSyscallsToLibSystem) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planMacImports({"gethostname", "sysctlbyname", "uname"}, plan, err));

    EXPECT_TRUE(std::any_of(plan.dylibs.begin(), plan.dylibs.end(), [](const DylibImport &import) {
        return import.path == "/usr/lib/libSystem.B.dylib";
    }));
    ASSERT_TRUE(plan.symOrdinals.count("gethostname") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["gethostname"]);
    ASSERT_TRUE(plan.symOrdinals.count("sysctlbyname") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["sysctlbyname"]);
    ASSERT_TRUE(plan.symOrdinals.count("uname") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["uname"]);
}

/// @brief Verify Darwin pattern-fill helpers synthesized by Clang resolve to libSystem.
/// @details Optimized C loops may acquire these imports even when source code only contains
///          ordinary scalar stores. Cover all three public pattern widths so compiler version and
///          optimization-level changes cannot silently make a runtime archive unlinkable.
TEST(PlatformImportPlanners, MacPlannerMapsCompilerPatternFillsToLibSystem) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(
        planMacImports({"memset_pattern4", "memset_pattern8", "__memset_pattern16"}, plan, err));

    ASSERT_TRUE(plan.symOrdinals.count("memset_pattern4") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["memset_pattern4"]);
    ASSERT_TRUE(plan.symOrdinals.count("memset_pattern8") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["memset_pattern8"]);
    ASSERT_TRUE(plan.symOrdinals.count("__memset_pattern16") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["__memset_pattern16"]);
}

TEST(PlatformImportPlanners, MacPlannerMapsDarwinArgvAccessorsToLibSystem) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planMacImports({"_NSGetArgc", "_NSGetArgv"}, plan, err));

    EXPECT_TRUE(std::any_of(plan.dylibs.begin(), plan.dylibs.end(), [](const DylibImport &import) {
        return import.path == "/usr/lib/libSystem.B.dylib";
    }));
    ASSERT_TRUE(plan.symOrdinals.count("_NSGetArgc") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["_NSGetArgc"]);
    ASSERT_TRUE(plan.symOrdinals.count("_NSGetArgv") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["_NSGetArgv"]);
}

TEST(PlatformImportPlanners, MacPlannerMapsDarwinMathHelpersToLibSystem) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planMacImports({"cbrtf", "___exp10", "__sincosf_stret"}, plan, err));

    EXPECT_TRUE(std::any_of(plan.dylibs.begin(), plan.dylibs.end(), [](const DylibImport &import) {
        return import.path == "/usr/lib/libSystem.B.dylib";
    }));
    ASSERT_TRUE(plan.symOrdinals.count("cbrtf") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["cbrtf"]);
    ASSERT_TRUE(plan.symOrdinals.count("___exp10") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["___exp10"]);
    ASSERT_TRUE(plan.symOrdinals.count("__sincosf_stret") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["__sincosf_stret"]);
}

TEST(PlatformImportPlanners, MacPlannerMapsLibcxxRuntimeSymbolsToLibcxxDylib) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planMacImports(
        {"__ZNSt3__118condition_variable10notify_allEv", "__cxa_throw", "_Unwind_Resume"},
        plan,
        err));

    const uint32_t libcxxOrdinal = dylibOrdinalForPath(plan, "/usr/lib/libc++.1.dylib");
    ASSERT_NE(0u, libcxxOrdinal);
    ASSERT_TRUE(plan.symOrdinals.count("__ZNSt3__118condition_variable10notify_allEv") != 0);
    EXPECT_EQ(libcxxOrdinal, plan.symOrdinals["__ZNSt3__118condition_variable10notify_allEv"]);
    ASSERT_TRUE(plan.symOrdinals.count("__cxa_throw") != 0);
    EXPECT_EQ(libcxxOrdinal, plan.symOrdinals["__cxa_throw"]);
    ASSERT_TRUE(plan.symOrdinals.count("_Unwind_Resume") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["_Unwind_Resume"]);
}

TEST(PlatformImportPlanners, MacPlannerMapsCxaAtexitSymbolsToLibSystem) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(
        planMacImports({"___cxa_atexit", "___cxa_finalize", "___cxa_thread_atexit"}, plan, err));

    EXPECT_EQ(1u, plan.symOrdinals["___cxa_atexit"]);
    EXPECT_EQ(1u, plan.symOrdinals["___cxa_finalize"]);
    EXPECT_EQ(1u, plan.symOrdinals["___cxa_thread_atexit"]);
}

TEST(PlatformImportPlanners, MacPlannerMapsSecurityConstantsToSecurityFramework) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planMacImports({"kSecKeyAlgorithmECDSASignatureDigestX962SHA256"}, plan, err));

    const uint32_t securityOrdinal = dylibOrdinalForPath(
        plan, "/System/Library/Frameworks/Security.framework/Versions/A/Security");
    ASSERT_NE(0u, securityOrdinal);
    ASSERT_TRUE(plan.symOrdinals.count("kSecKeyAlgorithmECDSASignatureDigestX962SHA256") != 0);
    EXPECT_EQ(securityOrdinal, plan.symOrdinals["kSecKeyAlgorithmECDSASignatureDigestX962SHA256"]);
}

TEST(PlatformImportPlanners, MacPlannerMapsImageIOSymbolsBeforeCoreGraphics) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(
        planMacImports({"_CGImageSourceCreateImageAtIndex", "CGContextDrawImage"}, plan, err));

    const uint32_t imageIOOrdinal = dylibOrdinalForPath(
        plan, "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO");
    const uint32_t coreGraphicsOrdinal = dylibOrdinalForPath(
        plan, "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics");
    ASSERT_NE(0u, imageIOOrdinal);
    ASSERT_NE(0u, coreGraphicsOrdinal);
    ASSERT_TRUE(plan.symOrdinals.count("_CGImageSourceCreateImageAtIndex") != 0);
    ASSERT_TRUE(plan.symOrdinals.count("CGContextDrawImage") != 0);
    EXPECT_EQ(imageIOOrdinal, plan.symOrdinals["_CGImageSourceCreateImageAtIndex"]);
    EXPECT_EQ(coreGraphicsOrdinal, plan.symOrdinals["CGContextDrawImage"]);
}

TEST(PlatformImportPlanners, MacPlannerMapsCommonCryptoSymbolsToLibSystem) {
    MacImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(planMacImports({"CC_SHA512"}, plan, err));

    ASSERT_TRUE(plan.symOrdinals.count("CC_SHA512") != 0);
    EXPECT_EQ(1u, plan.symOrdinals["CC_SHA512"]);
}

TEST(PlatformImportPlanners, WindowsPlannerCreatesGroupedImportsAndThunks) {
    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64,
                                       {"ExitProcess",
                                        "GetModuleFileNameW",
                                        "InitializeCriticalSectionAndSpinCount",
                                        "InitializeCriticalSectionEx",
                                        "BringWindowToTop",
                                        "CreateWindowExW",
                                        "LoadIconW",
                                        "LoadImageW",
                                        "CreateWaitableTimerExW",
                                        "ClipCursor",
                                        "SetWaitableTimer",
                                        "GetDiskFreeSpaceExW",
                                        "CopyFile2",
                                        "FormatMessageA",
                                        "GetLocaleInfoEx",
                                        "CreateDirectoryExW",
                                        "DeviceIoControl",
                                        "TryAcquireSRWLockShared",
                                        "SetFileInformationByHandle",
                                        "CreateHardLinkW",
                                        "AreFileApisANSI",
                                        "SetFileAttributesW",
                                        "FindFirstFileExW",
                                        "GetFinalPathNameByHandleW",
                                        "GetRawInputData",
                                        "SetFileTime",
                                        "CreateFile2",
                                        "GetFileInformationByHandleEx",
                                        "ReplaceFileW",
                                        "CreateSymbolicLinkW",
                                        "SetConsoleCtrlHandler",
                                        "LockFileEx",
                                        "UnlockFileEx",
                                        "SleepConditionVariableSRW",
                                        "TryAcquireSRWLockExclusive",
                                        "SetFocus",
                                        "GetCapture",
                                        "GetClassInfoExW",
                                        "ReleaseCapture",
                                        "SetCapture",
                                        "MsgWaitForMultipleObjectsEx",
                                        "RegisterRawInputDevices",
                                        "D3D11CreateDevice",
                                        "cbrtf",
                                        "cos",
                                        "exp2f",
                                        "log2f",
                                        "remainder",
                                        "remainderf",
                                        "_stat64",
                                        "_fstat64",
                                        "_wstat64",
                                        "__RTDynamicCast",
                                        "_Init_thread_header",
                                        "_Smtx_lock_exclusive",
                                        "__std_type_info_name",
                                        "__std_smf_beta",
                                        "__std_atomic_wait_direct",
                                        "fsetpos",
                                        "_invalid_parameter",
                                        "_callnewh",
                                        "_initialize_onexit_table",
                                        "_cexit",
                                        "_configure_narrow_argv",
                                        "___lc_codepage_func",
                                        "_register_onexit_function",
                                        "_seh_filter_dll",
                                        "_crt_atexit",
                                        "_initialize_narrow_environment",
                                        "_execute_onexit_table",
                                        "_crt_at_quick_exit",
                                        "__stdio_common_vsprintf_s",
                                        "__stdio_common_vswprintf",
                                        "_get_osfhandle",
                                        "_chmod",
                                        "_wgetenv",
                                        "_fdclass",
                                        "_fdtest",
                                        "_rotl",
                                        "_beginthreadex",
                                        "__intrinsic_setjmp",
                                        "terminate",
                                        "__imp_ExitProcess"},
                                       false,
                                       plan,
                                       err));

    EXPECT_TRUE(importPlanHasDll(plan, "kernel32.dll"));
    EXPECT_TRUE(importPlanHasDll(plan, "user32.dll"));
    EXPECT_TRUE(importPlanHasDll(plan, "d3d11.dll"));
    EXPECT_TRUE(importPlanHasDll(plan, "ucrtbase.dll"));
    EXPECT_TRUE(importPlanHasDll(plan, "VCRUNTIME140.dll"));
    EXPECT_TRUE(importPlanHasDll(plan, "MSVCP140.dll"));
    EXPECT_TRUE(importPlanHasDll(plan, "MSVCP140_2.dll"));
    EXPECT_TRUE(importPlanHasDll(plan, "MSVCP140_ATOMIC_WAIT.dll"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "RegisterRawInputDevices"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "MsgWaitForMultipleObjectsEx"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "BringWindowToTop"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "ClipCursor"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "GetRawInputData"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "SetFocus"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "GetCapture"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "GetClassInfoExW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "ReleaseCapture"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "SetCapture"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "LoadIconW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "LoadImageW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", "GetModuleFileNameW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", "InitializeCriticalSectionEx"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", "LockFileEx"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", "ReplaceFileW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", "SetConsoleCtrlHandler"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", "UnlockFileEx"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "log2f"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "remainder"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "remainderf"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_stat64"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_fstat64"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_wstat64"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "__stdio_common_vswprintf"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_get_osfhandle"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_chmod"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_wgetenv"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_fdclass"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_fdtest"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "terminate"));
    EXPECT_FALSE(importPlanDllHasFunction(plan, "VCRUNTIME140.dll", "terminate"));
    EXPECT_TRUE(objHasSymbol(plan.obj, "__imp_ExitProcess"));
    EXPECT_TRUE(objHasSymbol(plan.obj, "ExitProcess"));
    EXPECT_TRUE(plan.obj.sections.size() >= 2);
}

TEST(PlatformImportPlanners, Windows64BitStatSymbolsStayWindowsOnly) {
    for (const char *symbol : {"_stat64", "_fstat64", "_wstat64"}) {
        EXPECT_TRUE(isKnownDynamicSymbol(symbol, LinkPlatform::Windows));
        EXPECT_FALSE(isKnownDynamicSymbol(symbol, LinkPlatform::Linux));
        EXPECT_FALSE(isKnownDynamicSymbol(symbol, LinkPlatform::macOS));
    }
}

TEST(PlatformImportPlanners, WindowsFloatClassificationHelpersStayWindowsOnly) {
    for (const char *symbol : {"_fdclass", "_fdtest"}) {
        EXPECT_TRUE(isKnownDynamicSymbol(symbol, LinkPlatform::Windows));
        EXPECT_FALSE(isKnownDynamicSymbol(symbol, LinkPlatform::Linux));
        EXPECT_FALSE(isKnownDynamicSymbol(symbol, LinkPlatform::macOS));
    }
}

TEST(PlatformImportPlanners, WindowsPlannerMapsDebugOnlyUcrtImports) {
    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(
        generateWindowsImports(LinkArch::X86_64, {"_CrtDbgReport", "_free_dbg"}, true, plan, err));

    EXPECT_TRUE(importPlanHasDll(plan, "ucrtbased.dll"));
    EXPECT_TRUE(objHasSymbol(plan.obj, "__imp__CrtDbgReport"));
    EXPECT_TRUE(objHasSymbol(plan.obj, "_CrtDbgReport"));
    EXPECT_TRUE(objHasSymbol(plan.obj, "__imp__free_dbg"));
    EXPECT_TRUE(objHasSymbol(plan.obj, "_free_dbg"));
}

TEST(PlatformImportPlanners, WindowsPlannerMapsCertificateKeyImportToCrypt32) {
    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(
        generateWindowsImports(LinkArch::X86_64, {"CryptImportPublicKeyInfo"}, false, plan, err));

    EXPECT_TRUE(importPlanDllHasFunction(plan, "crypt32.dll", "CryptImportPublicKeyInfo"));
    EXPECT_FALSE(importPlanDllHasFunction(plan, "advapi32.dll", "CryptImportPublicKeyInfo"));
}

TEST(PlatformImportPlanners, WindowsPlannerRejectsUnavailableMsvcRuntimeImports) {
    for (const char *symbol : {"__std_find_trivial_1", "_Avx2WmemEnabled"}) {
        WindowsImportPlan plan;
        std::ostringstream err;
        EXPECT_FALSE(generateWindowsImports(LinkArch::X86_64, {symbol}, false, plan, err));
        EXPECT_NE(std::string::npos, err.str().find(symbol));
        EXPECT_NE(std::string::npos, err.str().find("no DLL mapping"));
    }
    EXPECT_FALSE(isKnownDynamicSymbol("_Avx2WmemEnabled", LinkPlatform::Windows));
}

// F10: the dynamic-symbol allow-list is platform-scoped for names exclusive to
// one platform's system libraries, so a foreign/typo'd API is a link error
// elsewhere instead of a dynamic import that never resolves at load time.
TEST(DynamicSymbolPolicy, ForeignPlatformSymbolsRejectedNativeAccepted) {
    // Win32 API: accepted on Windows, rejected on Linux/macOS.
    EXPECT_TRUE(isKnownDynamicSymbol("GetProcAddress", LinkPlatform::Windows));
    EXPECT_FALSE(isKnownDynamicSymbol("GetProcAddress", LinkPlatform::Linux));
    EXPECT_FALSE(isKnownDynamicSymbol("GetProcAddress", LinkPlatform::macOS));
    EXPECT_TRUE(isKnownDynamicSymbol("InitializeCriticalSectionEx", LinkPlatform::Windows));
    EXPECT_FALSE(isKnownDynamicSymbol("InitializeCriticalSectionEx", LinkPlatform::Linux));
    EXPECT_TRUE(isKnownDynamicSymbol("ReplaceFileW", LinkPlatform::Windows));
    EXPECT_FALSE(isKnownDynamicSymbol("ReplaceFileW", LinkPlatform::macOS));
    EXPECT_TRUE(isKnownDynamicSymbol("_chmod", LinkPlatform::Windows));
    EXPECT_FALSE(isKnownDynamicSymbol("_chmod", LinkPlatform::Linux));

    // Darwin/Mach: accepted on macOS, rejected on Linux/Windows.
    EXPECT_TRUE(isKnownDynamicSymbol("mach_absolute_time", LinkPlatform::macOS));
    EXPECT_FALSE(isKnownDynamicSymbol("mach_absolute_time", LinkPlatform::Linux));
    EXPECT_FALSE(isKnownDynamicSymbol("mach_absolute_time", LinkPlatform::Windows));
    EXPECT_TRUE(isKnownDynamicSymbol("memset_pattern16", LinkPlatform::macOS));
    EXPECT_FALSE(isKnownDynamicSymbol("memset_pattern16", LinkPlatform::Linux));
    EXPECT_FALSE(isKnownDynamicSymbol("memset_pattern16", LinkPlatform::Windows));

    // glibc-internal: accepted on Linux, rejected on macOS/Windows.
    EXPECT_TRUE(isKnownDynamicSymbol("__errno_location", LinkPlatform::Linux));
    EXPECT_FALSE(isKnownDynamicSymbol("__errno_location", LinkPlatform::macOS));
    EXPECT_FALSE(isKnownDynamicSymbol("__errno_location", LinkPlatform::Windows));
    EXPECT_TRUE(isKnownDynamicSymbol("__ctype_tolower_loc", LinkPlatform::Linux));
    EXPECT_FALSE(isKnownDynamicSymbol("__ctype_tolower_loc", LinkPlatform::macOS));
    EXPECT_FALSE(isKnownDynamicSymbol("__ctype_tolower_loc", LinkPlatform::Windows));

    // Genuinely cross-platform libc stays accepted on every platform.
    EXPECT_TRUE(isKnownDynamicSymbol("malloc", LinkPlatform::Windows));
    EXPECT_TRUE(isKnownDynamicSymbol("malloc", LinkPlatform::Linux));
    EXPECT_TRUE(isKnownDynamicSymbol("malloc", LinkPlatform::macOS));
    EXPECT_TRUE(isKnownDynamicSymbol("bcmp", LinkPlatform::Linux));
    EXPECT_FALSE(isKnownDynamicSymbol("bcmp", LinkPlatform::macOS));
    EXPECT_FALSE(isKnownDynamicSymbol("bcmp", LinkPlatform::Windows));
}

// Context state uses a recursive POSIX mutex. Native applications link the
// runtime archive directly, so the in-tree linker must permit the complete
// mutex-attribute setup sequence instead of failing late at symbol resolution.
TEST(DynamicSymbolPolicy, PosixRecursiveMutexAttributeSymbolsAccepted) {
    static constexpr const char *kSymbols[] = {
        "pthread_mutexattr_init",
        "pthread_mutexattr_settype",
        "pthread_mutexattr_destroy",
    };
    for (const char *symbol : kSymbols) {
        EXPECT_TRUE(isKnownDynamicSymbol(symbol, LinkPlatform::Linux));
        EXPECT_TRUE(isKnownDynamicSymbol(symbol, LinkPlatform::macOS));
    }
}

// Workspace transaction durability and process-tree ownership use these
// descriptor-relative libc entry points. Keep the native linker's policy in
// lockstep with the statically archived runtime so failures are caught here,
// not when a large application reaches its final native link.
TEST(DynamicSymbolPolicy, RuntimeWorkspaceAndProcessSymbolsAccepted) {
    static constexpr const char *kPosixSymbols[] = {
        "fchown",
        "fgetxattr",
        "flistxattr",
        "flock",
        "fsetxattr",
        "posix_spawnattr_destroy",
        "posix_spawnattr_getflags",
        "posix_spawnattr_init",
        "posix_spawnattr_setflags",
        "posix_spawnattr_setpgroup",
        "pread",
    };
    for (const char *symbol : kPosixSymbols) {
        EXPECT_TRUE(isKnownDynamicSymbol(symbol, LinkPlatform::Linux));
        EXPECT_TRUE(isKnownDynamicSymbol(symbol, LinkPlatform::macOS));
    }

    EXPECT_TRUE(isKnownDynamicSymbol("fchflags", LinkPlatform::macOS));
    EXPECT_FALSE(isKnownDynamicSymbol("fchflags", LinkPlatform::Linux));
    EXPECT_FALSE(isKnownDynamicSymbol("fchflags", LinkPlatform::Windows));
}

// F24/F25: OpenGL (gl + CamelCase, incl. glX) resolves to libGL and X11 (X +
// CamelCase) to libX11, while libc glob / stray uppercase-X names are NOT treated
// as GL/X11 dynamic imports.
TEST(PlatformImportPlanners, LinuxClassifiesGlAndX11Precisely) {
    std::unordered_set<std::string> syms = {"glClear", "glXCreateContext", "XOpenDisplay"};
    LinuxImportPlan plan;
    std::ostringstream err;
    EXPECT_TRUE(planLinuxImports(syms, plan, err));
    EXPECT_TRUE(contains(plan.neededLibs, std::string("libGL.so.1")));
    EXPECT_TRUE(contains(plan.neededLibs, std::string("libX11.so.6")));

    EXPECT_TRUE(isKnownDynamicSymbol("glClear", LinkPlatform::Linux));
    EXPECT_TRUE(isKnownDynamicSymbol("glXCreateContext", LinkPlatform::Linux));
    EXPECT_TRUE(isKnownDynamicSymbol("XOpenDisplay", LinkPlatform::Linux));
    EXPECT_FALSE(isKnownDynamicSymbol("Xtypo", LinkPlatform::Linux));     // X + lowercase
    EXPECT_FALSE(isKnownDynamicSymbol("globmatch", LinkPlatform::Linux)); // gl + lowercase
}

// F22: the WASAPI/COM entry points used by the Windows audio backend are both
// accepted as dynamic imports and mapped to ole32.dll (previously the planner
// listed them but isKnownDynamicSymbol rejected them, so audio failed to link).
TEST(PlatformImportPlanners, WindowsComAudioSymbolsResolveToOle32) {
    for (const char *sym : {"CoCreateInstance", "CoInitializeEx", "CoUninitialize"})
        EXPECT_TRUE(isKnownDynamicSymbol(sym, LinkPlatform::Windows));

    std::unordered_set<std::string> syms = {"CoCreateInstance", "CoInitializeEx", "CoUninitialize"};
    WindowsImportPlan plan;
    std::ostringstream err;
    EXPECT_TRUE(generateWindowsImports(LinkArch::X86_64, syms, false, plan, err));
    EXPECT_TRUE(importPlanHasDll(plan, "ole32.dll"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ole32.dll", "CoCreateInstance"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ole32.dll", "CoUninitialize"));
}

TEST(PlatformImportPlanners, WindowsGuiAutomationSymbolsResolveToSystemDlls) {
    const std::unordered_set<std::string> syms = {
        "LoadLibraryW",
        "SafeArrayCreateVector",
        "SafeArrayPutElement",
        "SHCreateItemFromParsingName",
        "SysAllocString",
        "SysAllocStringLen",
        "SysFreeString",
        "VariantInit",
    };

    for (const auto &sym : syms) {
        EXPECT_TRUE(isKnownDynamicSymbol(sym, LinkPlatform::Windows));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::Linux));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::macOS));
    }

    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64, syms, false, plan, err));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", "LoadLibraryW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "shell32.dll", "SHCreateItemFromParsingName"));
    for (const char *sym : {"SafeArrayCreateVector",
                            "SafeArrayPutElement",
                            "SysAllocString",
                            "SysAllocStringLen",
                            "SysFreeString",
                            "VariantInit"}) {
        EXPECT_TRUE(importPlanDllHasFunction(plan, "oleaut32.dll", sym));
    }
}

TEST(PlatformImportPlanners, WindowsImeSymbolsResolveToImm32) {
    const std::unordered_set<std::string> syms = {
        "ImmGetCompositionStringW", "ImmGetContext", "ImmReleaseContext"};

    for (const auto &sym : syms) {
        EXPECT_TRUE(isKnownDynamicSymbol(sym, LinkPlatform::Windows));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::Linux));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::macOS));
    }

    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64, syms, false, plan, err));
    EXPECT_TRUE(importPlanHasDll(plan, "imm32.dll"));
    for (const auto &sym : syms)
        EXPECT_TRUE(importPlanDllHasFunction(plan, "imm32.dll", sym));
}

TEST(PlatformImportPlanners, WindowsGuiRuntimeSymbolsResolveToSystemDlls) {
    const std::unordered_set<std::string> syms = {
        "SystemParametersInfoW", "RegGetValueW", "strpbrk", "lround"};

    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64, syms, false, plan, err));
    for (const char *sym : {"SystemParametersInfoW", "RegGetValueW"}) {
        EXPECT_TRUE(isKnownDynamicSymbol(sym, LinkPlatform::Windows));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::Linux));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::macOS));
    }
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "SystemParametersInfoW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "advapi32.dll", "RegGetValueW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "strpbrk"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "lround"));
}

TEST(PlatformImportPlanners, WindowsReliabilityApisResolveToKernel32) {
    const std::unordered_set<std::string> syms = {
        "GetActiveProcessorCount", "GetWindowsDirectoryW", "GlobalSize"};

    for (const auto &sym : syms) {
        EXPECT_TRUE(isKnownDynamicSymbol(sym, LinkPlatform::Windows));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::Linux));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::macOS));
    }

    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64, syms, false, plan, err));
    for (const auto &sym : syms)
        EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", sym));
}

TEST(PlatformImportPlanners, WindowsHardenedRuntimeSymbolsResolveToSystemDlls) {
    const std::unordered_set<std::string> syms = {
        "_wsopen_s", "CreateEventW", "GetWindowLongW", "SetWindowLongW"};

    for (const auto &sym : syms) {
        EXPECT_TRUE(isKnownDynamicSymbol(sym, LinkPlatform::Windows));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::Linux));
        EXPECT_FALSE(isKnownDynamicSymbol(sym, LinkPlatform::macOS));
    }

    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64, syms, false, plan, err));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", "_wsopen_s"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", "CreateEventW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "GetWindowLongW"));
    EXPECT_TRUE(importPlanDllHasFunction(plan, "user32.dll", "SetWindowLongW"));

    WindowsImportPlan debugPlan;
    std::ostringstream debugErr;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64, {"_wsopen_s"}, true, debugPlan, debugErr));
    EXPECT_TRUE(importPlanDllHasFunction(debugPlan, "ucrtbased.dll", "_wsopen_s"));
}

TEST(PlatformImportPlanners, WindowsEmbeddedPreviewAndPhysicsSymbolsResolveToSystemDlls) {
    const std::unordered_set<std::string> mappingSymbols = {
        "CreateFileMappingA", "OpenFileMappingA", "MapViewOfFile", "UnmapViewOfFile"};
    const std::unordered_set<std::string> mathSymbols = {
        "expm1", "fma", "frexp", "log1p", "scalbn"};
    std::unordered_set<std::string> symbols = mappingSymbols;
    symbols.insert(mathSymbols.begin(), mathSymbols.end());

    for (const auto &symbol : mappingSymbols) {
        EXPECT_TRUE(isKnownDynamicSymbol(symbol, LinkPlatform::Windows));
        EXPECT_FALSE(isKnownDynamicSymbol(symbol, LinkPlatform::Linux));
        EXPECT_FALSE(isKnownDynamicSymbol(symbol, LinkPlatform::macOS));
    }
    for (const auto &symbol : mathSymbols)
        EXPECT_TRUE(isKnownDynamicSymbol(symbol, LinkPlatform::Windows));

    WindowsImportPlan plan;
    std::ostringstream err;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64, symbols, false, plan, err));
    for (const auto &symbol : mappingSymbols)
        EXPECT_TRUE(importPlanDllHasFunction(plan, "kernel32.dll", symbol));
    for (const auto &symbol : mathSymbols)
        EXPECT_TRUE(importPlanDllHasFunction(plan, "ucrtbase.dll", symbol));

    WindowsImportPlan debugPlan;
    std::ostringstream debugErr;
    ASSERT_TRUE(generateWindowsImports(LinkArch::X86_64, symbols, true, debugPlan, debugErr));
    for (const auto &symbol : mappingSymbols)
        EXPECT_TRUE(importPlanDllHasFunction(debugPlan, "kernel32.dll", symbol));
    for (const auto &symbol : mathSymbols)
        EXPECT_TRUE(importPlanDllHasFunction(debugPlan, "ucrtbased.dll", symbol));
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
