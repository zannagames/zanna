//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/DynamicSymbolPolicy.hpp
// Purpose: Shared policy helpers for symbols that are allowed to resolve
//          dynamically through system libraries or platform frameworks. The
//          native linker uses these to decide whether an undefined symbol is a
//          legitimate dyld/dlopen reference (libc, ObjC, Win32, pthreads, etc.)
//          versus a real linker error.
// Key invariants:
//   - The exact-match list and prefix lists are sorted by use frequency, not
//     alphabetically; do not reorder without measuring impact on link cost.
//   - Symbols beginning with leading underscores are stripped before matching
//     to handle the Mach-O "_main"/"main" convention transparently.
// Ownership/Lifetime: Stateless inline helpers; no allocation beyond returned
//                     strings.
// Links: src/codegen/common/linker/SymbolResolver.cpp,
//        src/codegen/common/linker/NativeLinker.cpp,
//        src/codegen/common/linker/MachOExeWriter.cpp,
//        src/codegen/common/linker/PeExeWriter.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file DynamicSymbolPolicy.hpp
 * @brief Defines the cross-platform allowlist and rejection policy for
 *        loader-resolved native symbols.
 *
 * The policy normalizes object-format decoration, rejects names known to
 * belong exclusively to a different target, and recognizes tightly scoped
 * system, C++, compiler-runtime, framework, and ABI symbol families.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <string>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Normalise a symbol name to its "bare" form for prefix matching.
/// @details Drops any leading underscores (Mach-O mangles "main" to "_main")
///          and trims the trailing "$DARWIN_EXTSN" Darwin-extension marker so
///          variants like `select` and `select$DARWIN_EXTSN` compare equal.
/// @param name Raw object-format symbol name.
/// @return The stripped name, or @p name unchanged when no transform applied.
inline std::string stripDynamicSymbolLeadingUnderscores(const std::string &name) {
    size_t i = 0;
    while (i < name.size() && name[i] == '_')
        ++i;

    size_t end = name.size();
    static constexpr const char *kDarwinExtSuffix = "$DARWIN_EXTSN";
    static constexpr size_t kDarwinExtSuffixLen = 13;
    if (end >= i + kDarwinExtSuffixLen &&
        name.compare(end - kDarwinExtSuffixLen, kDarwinExtSuffixLen, kDarwinExtSuffix) == 0) {
        end -= kDarwinExtSuffixLen;
    }

    return (i == 0 && end == name.size()) ? name : name.substr(i, end - i);
}

/// @brief Test whether @p name (or its stripped form) starts with any of the
///        null-terminated prefix list @p prefixes (terminated by a nullptr entry).
/// @param name Raw symbol name to test.
/// @param prefixes Pointer to a null-terminated array of C-string prefixes.
/// @return `true` when the raw or normalized name starts with any prefix.
inline bool dynamicSymbolHasPrefix(const std::string &name, const char *const *prefixes) {
    const std::string stripped = stripDynamicSymbolLeadingUnderscores(name);
    for (const char *const *p = prefixes; p != nullptr && *p != nullptr; ++p) {
        const std::string prefix(*p);
        if (name.rfind(prefix, 0) == 0 || stripped.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}

/// @brief Test whether @p name (or its de-underscored form) exactly matches any
///        entry in @p arr (length @p count), mirroring the exact-list comparison.
/// @param name Raw symbol name.
/// @param stripped Precomputed normalized form of @p name.
/// @param arr Array of exact C-string candidates.
/// @param count Number of entries in @p arr.
/// @return `true` when either form equals an entry.
inline bool matchesExactName(const std::string &name,
                             const std::string &stripped,
                             const char *const *arr,
                             size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (name == arr[i] || stripped == arr[i])
            return true;
    return false;
}

/// @brief Symbols that are exclusive to one platform's system libraries.
/// @details These are consulted as a negative filter: an unresolved symbol that
///          belongs to a platform OTHER than the one being linked is rejected
///          (not treated as a dynamic import), so a typo'd or mis-targeted
///          foreign API — GetProcAddress on Linux, __errno_location on Windows,
///          mach_absolute_time on Linux — fails at link time instead of being
///          emitted as a dynamic import that can never resolve at load time.
///          Only unambiguously OS-exclusive names appear here; anything that can
///          legitimately be a cross-platform libc/libm/POSIX reference is left in
///          the shared exact list. Missing an entry is safe (stays permissive as
///          before); it can never wrongly reject a real cross-platform symbol.
/// @return Stable process-lifetime list of Windows-only import names.
inline const std::vector<const char *> &windowsExclusiveDynamicSymbols() {
    static const std::vector<const char *> kSyms = {
        "ExitProcess",
        "GetModuleHandleA",
        "GetProcAddress",
        "VirtualAlloc",
        "VirtualFree",
        "GetLastError",
        "GetFullPathNameA",
        "GetComputerNameA",
        "GetComputerNameW",
        "GetActiveProcessorCount",
        "GetUserNameA",
        "GetUserNameW",
        "GetWindowsDirectoryW",
        "GetFileSizeEx",
        "GetClientRect",
        "GlobalMemoryStatusEx",
        "GlobalSize",
        "GetAdaptersAddresses",
        "ResetEvent",
        "SetEvent",
        "D3D11CreateDevice",
        "D3D11CreateDeviceAndSwapChain",
        "D3DCompile",
        "D3DCompile2",
        "D3DCompileFromFile",
        "D3DReflect",
        "CertAddEncodedCertificateToStore",
        "CertCloseStore",
        "CertCreateCertificateContext",
        "CertCreateCertificateChainEngine",
        "CertFreeCertificateChain",
        "CertFreeCertificateChainEngine",
        "CertFreeCertificateContext",
        "CertGetCertificateChain",
        "CertOpenStore",
        "CertVerifyCertificateChainPolicy",
        "CryptAcquireCertificatePrivateKey",
        "CryptStringToBinaryA",
        "BCryptGenRandom",
        "BCryptDestroyKey",
        "BCryptVerifySignature",
        "XInputGetState",
        "XInputSetState",
        "WSAStartup",
        "WSACleanup",
        "WSAGetLastError",
        "closesocket",
        "ioctlsocket",
        "__acrt_iob_func",
        "__local_stdio_printf_options",
        "__local_stdio_scanf_options",
        "__stdio_common_vfprintf",
        "__stdio_common_vsprintf",
        "__stdio_common_vsprintf_s",
        "_calloc_dbg",
        "_free_dbg",
        "_malloc_dbg",
        "_realloc_dbg",
        "_vfprintf_l",
        "_vsscanf_l",
        "__C_specific_handler",
        "__C_specific_handler_noexcept",
        "__current_exception",
        "__current_exception_context",
        "_CxxThrowException",
        "__CxxFrameHandler3",
        "__CxxFrameHandler4",
        "__RTDynamicCast",
        "__intrinsic_setjmp",
        "__intrinsic_setjmpex",
        "__std_exception_copy",
        "__std_exception_destroy",
        "__std_type_info_compare",
        "__security_check_cookie",
        "__security_init_cookie",
        "__security_pop_cookie",
        "__security_push_cookie",
        "__GSHandlerCheck",
        "__GSHandlerCheck_EH4",
        "__chkstk",
        "_callnewh",
        "callnewh",
        "_purecall",
        "__RTC_memset",
        "_setjmpex",
        "_byteswap_uint64",
        "_rotl",
        "_rotl64",
        "_rotr",
        "_rotr64",
        "_InterlockedCompareExchange",
        "_InterlockedCompareExchange64",
        "_InterlockedCompareExchangePointer",
        "_InterlockedDecrement",
        "_InterlockedExchange",
        "_InterlockedExchange64",
        "_InterlockedExchange8",
        "_InterlockedExchangeAdd",
        "_InterlockedExchangeAdd64",
        "_InterlockedIncrement64",
        "_InterlockedOr",
        "InitializeCriticalSectionEx",
        "ReplaceFileW",
        "TryEnterCriticalSection",
        "CommandLineToArgvW",
        "LoadLibraryW",
        "CoCreateInstance",
        "CoInitializeEx",
        "CoUninitialize",
        "CoTaskMemFree",
        "SafeArrayCreateVector",
        "SafeArrayPutElement",
        "SysAllocString",
        "SysAllocStringLen",
        "SysFreeString",
        "VariantInit",
        "SHCreateItemFromParsingName",
        "DragQueryFileW",
        "ImmGetCompositionStringW",
        "ImmGetContext",
        "ImmReleaseContext",
        "RegGetValueW",
        "SystemParametersInfoW",
        "_open_osfhandle",
        "_cexit",
        "_configure_narrow_argv",
        "_crt_at_quick_exit",
        "_crt_atexit",
        "_execute_onexit_table",
        "_initialize_narrow_environment",
        "_initialize_onexit_table",
        "_register_onexit_function",
        "_seh_filter_dll",
        "_CrtDbgReport",
        "_CrtDbgReportW",
        "rand_s",
        "_fdclass",
        "_fdtest",
        "strcpy_s",
        "strcat_s",
        "wcscpy_s",
        "_wcsnicmp",
        "_chmod",
        "_wchmod",
        "_wsplitpath_s",
        "_wmakepath_s",
        "_stat64",
        "_fstat64",
        "_wstat64",
        "?_OptionsStorage@?1??__local_stdio_printf_options@@9@9",
        "?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9"};
    return kSyms;
}

/// @brief Returns symbols supplied exclusively by macOS system libraries and ABIs.
/// @return Stable process-lifetime list of macOS-only import names.
inline const std::vector<const char *> &macExclusiveDynamicSymbols() {
    static const std::vector<const char *> kSyms = {"sincosf_stret",
                                                    "memset_pattern4",
                                                    "memset_pattern8",
                                                    "memset_pattern16",
                                                    "_NSGetExecutablePath",
                                                    "_NSGetArgc",
                                                    "_NSGetArgv",
                                                    "_NSConcreteStackBlock",
                                                    "_NSConcreteGlobalBlock",
                                                    "_NSConcreteMallocBlock",
                                                    "_Block_copy",
                                                    "_Block_release",
                                                    "_Block_object_assign",
                                                    "_Block_object_dispose",
                                                    "dyld_stub_binder",
                                                    "_tlv_atexit",
                                                    "_tlv_bootstrap",
                                                    "__assert_rtn",
                                                    "__chkstk_darwin",
                                                    "__stderrp",
                                                    "__stdinp",
                                                    "__stdoutp",
                                                    "__darwin_check_fd_set_overflow",
                                                    "select$DARWIN_EXTSN",
                                                    "mach_timebase_info",
                                                    "mach_absolute_time",
                                                    "mach_task_self_",
                                                    "mach_host_self",
                                                    "sysctlbyname",
                                                    "task_info",
                                                    "host_page_size",
                                                    "_os_unfair_lock_lock",
                                                    "_os_unfair_lock_unlock",
                                                    "os_unfair_lock_lock",
                                                    "os_unfair_lock_unlock",
                                                    "sel_registerName",
                                                    "sel_getName",
                                                    "_objc_empty_cache",
                                                    "_objc_empty_vtable"};
    return kSyms;
}

/// @brief Returns symbols supplied exclusively by Linux system libraries and ABIs.
/// @return Stable process-lifetime list of Linux-only import names.
inline const std::vector<const char *> &linuxExclusiveDynamicSymbols() {
    static const std::vector<const char *> kSyms = {"__errno_location",
                                                    "__assert_fail",
                                                    "__ctype_b_loc",
                                                    "__ctype_tolower_loc",
                                                    "bcmp",
                                                    "__isoc23_strtol",
                                                    "__isoc23_strtoll",
                                                    "__isoc99_sscanf",
                                                    "fopen64",
                                                    "fseeko64",
                                                    "ftello64",
                                                    "getrandom",
                                                    "sysinfo"};
    return kSyms;
}

/// @brief Recognise Itanium-mangled C++ runtime/library symbols (`std::*`, RTTI, etc.).
/// @details These symbols are supplied by the platform C++ runtime rather than
///          Zanna's own archives. The matcher stays narrow to known `std::*`,
///          RTTI/vtable, operator new/delete, and exception-runtime prefixes so
///          arbitrary user-defined C++ mangled names are not treated as system imports.
/// @param name Raw object-format symbol name.
/// @return `true` for a recognized Itanium C++ ABI/runtime symbol.
inline bool isKnownCppRuntimeDynamicSymbol(const std::string &name) {
    const std::string stripped = stripDynamicSymbolLeadingUnderscores(name);
    static const char *const kCppRuntimeExact[] = {
        "once_proxy",
        nullptr,
    };
    for (const char *const *p = kCppRuntimeExact; p && *p; ++p) {
        if (stripped == *p)
            return true;
    }

    static const char *const kCppRuntimePrefixes[] = {
        "ZNSt",
        "ZNKSt",
        "ZNKRSt",
        "ZNSi",
        "ZNSo",
        "ZTINSt",
        "ZTSNSt",
        "ZTVNSt",
        "ZTTNSt",
        "ZSt",
        "ZTISt",
        "ZTSSt",
        "ZTVSt",
        "ZTTSt",
        // libc++abi base RTTI vtables / type-infos (__cxxabiv1::*). Pulled in
        // by embedded C++ editor services; supplied by libc++abi, which
        // libc++.1.dylib re-exports on macOS.
        "ZTVN10__cxxabiv",
        "ZTIN10__cxxabiv",
        "ZTSN10__cxxabiv",
        // Fundamental-type RTTI (typeinfo for void/int/double/...) emitted by
        // C++ editor-service code; supplied by libc++abi. The single trailing
        // builtin-code letter keeps these from matching class type-infos
        // (`ZTIN...` / `ZTI<len>...`), which the editor-service closure defines
        // itself.
        "ZTIv",
        "ZTIb",
        "ZTIc",
        "ZTIa",
        "ZTIh",
        "ZTIs",
        "ZTIt",
        "ZTIi",
        "ZTIj",
        "ZTIl",
        "ZTIm",
        "ZTIx",
        "ZTIy",
        "ZTIf",
        "ZTId",
        "ZTIe",
        "ZTIw",
        "ZTIn",
        "ZTIo",
        "ZTIDn",
        "ZTIPv",
        "ZTIPKc",
        "ZTIPc",
        "Zda",
        "Zdl",
        "Zna",
        "Znw",
        "cxa_",
        "dynamic_cast",
        "gxx_personality_",
        nullptr,
    };
    return dynamicSymbolHasPrefix(name, kCppRuntimePrefixes);
}

/// @brief Recognises compiler runtime helper symbols supplied by libgcc_s/compiler-rt.
/// @param name Raw object-format symbol name.
/// @param platform Target platform, reserved for platform-specific compiler
///                 helper policy.
/// @return `true` when @p name is a recognized compiler support routine.
inline bool isKnownCompilerRuntimeDynamicSymbol(const std::string &name, LinkPlatform platform) {
    const std::string stripped = stripDynamicSymbolLeadingUnderscores(name);

    static const char *const kCompilerRuntimeExact[] = {
        "addtf3",      "divtf3",     "eqtf2",     "extenddftf2", "fixtfdi",
        "fixtfsi",     "fixunstfdi", "floatditf", "floatsitf",   "floatunditf",
        "floatunsitf", "getf2",      "gttf2",     "letf2",       "lttf2",
        "multf3",      "netf2",      "subtf3",    "trunctfdf2",  nullptr,
    };
    for (const char *const *p = kCompilerRuntimeExact; p && *p; ++p) {
        if (stripped == *p)
            return true;
    }
    return false;
}

/// @brief Determines whether an unresolved name may be delegated to the target loader.
/// @details Applies foreign-platform negative filters before shared and
///          platform-specific exact/prefix allowlists.  Linux additionally
///          recognizes constrained X11/OpenGL families; Windows recognizes
///          MSVC-mangled library names while explicitly excluding thread-safe
///          static guard symbols that must resolve internally.
/// @param name Raw object-format unresolved symbol name.
/// @param platform Platform for which the output is being linked.
/// @return `true` only when target system libraries or ABI runtimes are
///         expected to supply the symbol.
inline bool isKnownDynamicSymbol(const std::string &name, LinkPlatform platform) {
    const std::string stripped = stripDynamicSymbolLeadingUnderscores(name);

    if (isKnownCompilerRuntimeDynamicSymbol(name, platform))
        return true;

    // Reject exact-match symbols exclusive to a DIFFERENT platform's system
    // libraries so a mis-targeted or typo'd foreign API becomes a link error
    // instead of a dynamic import that cannot resolve at load time. The shared
    // exact list below still accepts these on their own platform.
    {
        const auto &win = windowsExclusiveDynamicSymbols();
        const auto &mac = macExclusiveDynamicSymbols();
        const auto &lin = linuxExclusiveDynamicSymbols();
        if (platform != LinkPlatform::Windows &&
            matchesExactName(name, stripped, win.data(), win.size()))
            return false;
        if (platform != LinkPlatform::macOS &&
            matchesExactName(name, stripped, mac.data(), mac.size()))
            return false;
        if (platform != LinkPlatform::Linux &&
            matchesExactName(name, stripped, lin.data(), lin.size()))
            return false;
    }

    if ((platform == LinkPlatform::macOS || platform == LinkPlatform::Linux) && stripped == "exp10")
        return true;

    static const char *const kDynSymExact[] = {
        // libSystem ctype data/helpers referenced by libc++ <locale>/<sstream>
        // (pulled in by the embedded C++ frontend). Both raw and de-underscored
        // forms are listed so the match holds regardless of Mach-O mangling.
        "__maskrune",
        "maskrune",
        "_DefaultRuneLocale",
        "DefaultRuneLocale",
        // libm (<cmath>/<charconv>) referenced by the embedded C++ frontend's
        // libc++ instantiations. All are libSystem/libm exports.
        "modf",
        "frexp",
        "ldexp",
        "scalbn",
        "copysign",
        "fmod",
        "hypot",
        "fma",
        "nearbyint",
        "rint",
        "trunc",
        "round",
        "lround",
        "llround",
        "lrint",
        "lrintf",
        "llrint",
        "fmin",
        "fmax",
        "fdim",
        "remainder",
        "remquo",
        "cbrt",
        "expm1",
        "log1p",
        "exp2",
        "log2",
        "tgamma",
        "lgamma",
        "sinh",
        "cosh",
        "tanh",
        "asinh",
        "acosh",
        "atanh",
        "asin",
        "acos",
        "atan",
        "atan2",
        "sin",
        "cos",
        "tan",
        "exp",
        "log",
        "log10",
        "pow",
        "sqrt",
        "ceil",
        "floor",
        "fabs",
        "wmemchr",
        "wmemcmp",
        "wmemcpy",
        "wmemmove",
        "wmemset",
        "wcslen",
        "wcscmp",
        "wcscpy",
        "wcsncmp",
        "printf",
        "fprintf",
        "sprintf",
        "snprintf",
        "vsnprintf",
        "puts",
        "fputs",
        "fopen",
        "fclose",
        "fread",
        "fwrite",
        "fseek",
        "fseeko",
        "ftell",
        "ftello",
        "fflush",
        "fgets",
        "malloc",
        "calloc",
        "realloc",
        "free",
        "memcpy",
        "memmove",
        "memset",
        // Darwin libSystem helpers that Clang may synthesize when vectorizing
        // repeated fixed-width stores. They are intentionally macOS-only via
        // macExclusiveDynamicSymbols() above.
        "memset_pattern4",
        "memset_pattern8",
        "memset_pattern16",
        "memcmp",
        "bcmp",
        "memchr",
        "ctype_tolower_loc",
        "strlen",
        "strnlen",
        "strcmp",
        "strcpy",
        "strncpy",
        "strdup",
        "strndup",
        "strcat",
        "strncat",
        "strstr",
        "strchr",
        "strrchr",
        "atoi",
        "atol",
        "atoll",
        "atof",
        "strtol",
        "strtod",
        "strtoul",
        "strtoull",
        "strspn",
        "strcspn",
        "strpbrk",
        "strsep",
        "getline",
        "getdelim",
        "mbstowcs",
        "wcstombs",
        "vprintf",
        "vsprintf",
        "vscanf",
        "vsscanf",
        "exit",
        "_exit",
        "abort",
        "atexit",
        "getenv",
        "setenv",
        "system",
        "time",
        "clock",
        "difftime",
        "gettimeofday",
        "gmtime_r",
        "localtime_r",
        "mktime",
        "strftime",
        "nanosleep",
        "usleep",
        "sleep",
        "open",
        "openat",
        "close",
        "read",
        "write",
        "lseek",
        "ftruncate",
        "stat",
        "fstat",
        "fstatat",
        "lstat",
        "chmod",
        "fchmod",
        "mkdir",
        "mkdirat",
        "rmdir",
        "link",
        "unlink",
        "unlinkat",
        "remove",
        "rename",
        "renameat",
        "getcwd",
        "chdir",
        "socket",
        "bind",
        "listen",
        "accept",
        "connect",
        "send",
        "recv",
        "recvmsg",
        "select",
        "poll",
        "setsockopt",
        "getsockopt",
        "getsockname",
        "closesocket",
        "ioctlsocket",
        "recvfrom",
        "sendto",
        "shutdown",
        "WSAStartup",
        "WSACleanup",
        "WSAGetLastError",
        "htons",
        "htonl",
        "ntohs",
        "ntohl",
        "inet_ntop",
        "inet_pton",
        "getaddrinfo",
        "freeaddrinfo",
        "getifaddrs",
        "freeifaddrs",
        "getnameinfo",
        "GetAdaptersAddresses",
        "GetComputerNameA",
        "GetComputerNameW",
        "GetFileSizeEx",
        "GetClientRect",
        "GlobalMemoryStatusEx",
        "GetUserNameA",
        "GetUserNameW",
        "ResetEvent",
        "SetEvent",
        "D3D11CreateDevice",
        "D3D11CreateDeviceAndSwapChain",
        "D3DCompile",
        "D3DCompile2",
        "D3DCompileFromFile",
        "D3DReflect",
        "CertAddEncodedCertificateToStore",
        "CertCloseStore",
        "CertCreateCertificateContext",
        "CertCreateCertificateChainEngine",
        "CertFreeCertificateChain",
        "CertFreeCertificateChainEngine",
        "CertFreeCertificateContext",
        "CertGetCertificateChain",
        "CertOpenStore",
        "CertVerifyCertificateChainPolicy",
        "CryptAcquireCertificatePrivateKey",
        "CryptStringToBinaryA",
        "pthread_create",
        "pthread_join",
        "pthread_detach",
        "pthread_mutex_init",
        "pthread_mutexattr_init",
        "pthread_mutexattr_settype",
        "pthread_mutexattr_destroy",
        "pthread_mutex_lock",
        "pthread_mutex_trylock",
        "pthread_mutex_unlock",
        "pthread_mutex_destroy",
        "pthread_cond_init",
        "pthread_cond_wait",
        "pthread_cond_signal",
        "pthread_cond_broadcast",
        "pthread_cond_destroy",
        "pthread_cond_timedwait",
        "pthread_cond_timedwait_relative_np",
        "pthread_condattr_init",
        "pthread_condattr_setclock",
        "pthread_condattr_destroy",
        "pthread_key_create",
        "pthread_getspecific",
        "pthread_setspecific",
        "pthread_once",
        "pthread_rwlock_init",
        "pthread_rwlock_rdlock",
        "pthread_rwlock_wrlock",
        "pthread_rwlock_unlock",
        "pthread_rwlock_destroy",
        "pthread_self",
        "pthread_equal",
        "pthread_attr_init",
        "pthread_attr_destroy",
        "pthread_attr_setdetachstate",
        "dlopen",
        "dlsym",
        "dlclose",
        "dlerror",
        "mmap",
        "munmap",
        "mprotect",
        "popen",
        "pclose",
        "posix_spawn",
        "posix_spawnp",
        "posix_spawn_file_actions_addchdir",
        "posix_spawn_file_actions_addchdir_np",
        "posix_spawn_file_actions_addclose",
        "posix_spawn_file_actions_adddup2",
        "posix_spawn_file_actions_destroy",
        "posix_spawn_file_actions_init",
        "sysconf",
        "getrlimit",
        "setrlimit",
        "getpid",
        "geteuid",
        "getgid",
        "getegid",
        "getuid",
        "kill",
        "signal",
        "sigaction",
        "sigaltstack",
        "sigaddset",
        "sigemptyset",
        "sigismember",
        "sigpending",
        "sigprocmask",
        "sigwait",
        "raise",
        "setjmp",
        "longjmp",
        "_setjmp",
        "_longjmp",
        "qsort",
        "bsearch",
        "isalpha",
        "isalnum",
        "isdigit",
        "isxdigit",
        "islower",
        "isspace",
        "isupper",
        "toupper",
        "tolower",
        "localeconv",
        "strerror",
        "strcasecmp",
        "strncasecmp",
        "perror",
        "sscanf",
        "strtoll",
        "strncmp",
        "strtok_r",
        "fnmatch",
        "regcomp",
        "regexec",
        "regfree",
        "regerror",
        "vfprintf",
        "setvbuf",
        "setbuf",
        "freopen",
        "tmpfile",
        "tmpnam",
        "mkstemp",
        "mkdtemp",
        "fdopen",
        "getc",
        "putc",
        "fgetc",
        "fputc",
        "ungetc",
        "ferror",
        "feof",
        "clearerr",
        "rewind",
        "_Exit",
        "posix_memalign",
        "aligned_alloc",
        "reallocf",
        "bzero",
        "access",
        "dup",
        "dup2",
        "pipe",
        "pipe2",
        "fork",
        "execv",
        "execve",
        "execvp",
        "posix_openpt",
        "grantpt",
        "unlockpt",
        "ptsname",
        "ptsname_r",
        "setsid",
        "waitpid",
        "wait",
        "fcntl",
        "fsync",
        "ioctl",
        "realpath",
        "uname",
        "kqueue",
        "kevent",
        "tcgetattr",
        "tcsetattr",
        "fileno",
        "isatty",
        "ttyname",
        "gethostname",
        "sched_yield",
        "clock_gettime",
        "readlink",
        "strtof",
        "rint",
        "sin",
        "cos",
        "sinh",
        "cosh",
        "tan",
        "tanh",
        "asin",
        "acos",
        "atan",
        "atan2",
        "atan2l",
        "sinf",
        "cosf",
        "tanf",
        "asinf",
        "acosf",
        "atanf",
        "atan2f",
        // Darwin libSystem helper emitted by Clang when neighbouring sinf/cosf
        // calls are combined, for example circular progress rendering.
        "sincosf_stret",
        "cbrt",
        "cbrtf",
        "sqrt",
        "sqrtl",
        "sqrtf",
        "pow",
        "powf",
        "hypot",
        "ldexp",
        "exp",
        "expf",
        "exp2f",
        "log",
        "logf",
        "log2",
        "log2f",
        "log10",
        "ceil",
        "ceill",
        "ceilf",
        "floor",
        "floorl",
        "floorf",
        "round",
        "roundf",
        "fmod",
        "fmodl",
        "fmodf",
        "fabs",
        "fabsf",
        "fmin",
        "fminf",
        "fmax",
        "fmaxl",
        "fmaxf",
        "cosl",
        "sinl",
        "copysign",
        "copysignf",
        "trunc",
        "truncf",
        "_NSGetExecutablePath",
        "_NSGetArgc",
        "_NSGetArgv",
        "_NSConcreteStackBlock",
        "_NSConcreteGlobalBlock",
        "_NSConcreteMallocBlock",
        "_Block_copy",
        "_Block_release",
        "_Block_object_assign",
        "_Block_object_dispose",
        "dyld_stub_binder",
        "_tlv_atexit",
        "_tlv_bootstrap",
        "__assert_rtn",
        "__chkstk_darwin",
        "__error",
        "__stderrp",
        "__stdinp",
        "__stdoutp",
        "__memcpy_chk",
        "__memmove_chk",
        "__memset_chk",
        "__snprintf_chk",
        "__strcat_chk",
        "__strncat_chk",
        "__strcpy_chk",
        "__strncpy_chk",
        "__vsnprintf_chk",
        "__darwin_check_fd_set_overflow",
        "select$DARWIN_EXTSN",
        "mach_timebase_info",
        "mach_absolute_time",
        "mach_task_self_",
        "mach_host_self",
        "sysctlbyname",
        "task_info",
        "host_page_size",
        "_os_unfair_lock_lock",
        "_os_unfair_lock_unlock",
        "os_unfair_lock_lock",
        "os_unfair_lock_unlock",
        "newlocale",
        "freelocale",
        "uselocale",
        "utime",
        "arc4random_buf",
        "backtrace",
        "backtrace_symbols_fd",
        "closedir",
        "opendir",
        "fdopendir",
        "dirfd",
        "readdir",
        "environ",
        "stdin",
        "stdout",
        "stderr",
        "__errno_location",
        "__assert_fail",
        "__ctype_b_loc",
        "__isoc23_strtol",
        "__isoc23_strtoll",
        "__isoc99_sscanf",
        "fopen64",
        "fseeko64",
        "ftello64",
        "getrandom",
        "getpwuid",
        "getpwuid_r",
        "nan",
        "sel_registerName",
        "sel_getName",
        "_objc_empty_cache",
        "_objc_empty_vtable",
        "ExitProcess",
        "GetModuleHandleA",
        "GetProcAddress",
        "VirtualAlloc",
        "VirtualFree",
        "GetLastError",
        "GetFullPathNameA",
        "GetActiveProcessorCount",
        "GetWindowsDirectoryW",
        "GlobalSize",
        "BCryptGenRandom",
        "BCryptDestroyKey",
        "BCryptVerifySignature",
        "XInputGetState",
        "XInputSetState",
        "__acrt_iob_func",
        "__local_stdio_printf_options",
        "__local_stdio_scanf_options",
        "__stdio_common_vfprintf",
        "__stdio_common_vsprintf",
        "__stdio_common_vsprintf_s",
        "_calloc_dbg",
        "_free_dbg",
        "_malloc_dbg",
        "_realloc_dbg",
        "_vfprintf_l",
        "_vsscanf_l",
        "__C_specific_handler",
        "__C_specific_handler_noexcept",
        "__current_exception",
        "__current_exception_context",
        "_CxxThrowException",
        "__CxxFrameHandler3",
        "__CxxFrameHandler4",
        "__RTDynamicCast",
        "__intrinsic_setjmp",
        "__intrinsic_setjmpex",
        "__std_exception_copy",
        "__std_exception_destroy",
        "__std_type_info_compare",
        "__security_check_cookie",
        "__security_init_cookie",
        "__security_pop_cookie",
        "__security_push_cookie",
        "__GSHandlerCheck",
        "__GSHandlerCheck_EH4",
        "__chkstk",
        "_callnewh",
        "callnewh",
        "dclass",
        "_purecall",
        "__RTC_memset",
        "_setjmpex",
        "_byteswap_uint64",
        "_rotl",
        "_rotl64",
        "_rotr",
        "_rotr64",
        "_InterlockedCompareExchange",
        "_InterlockedCompareExchange64",
        "_InterlockedCompareExchangePointer",
        "_InterlockedDecrement",
        "_InterlockedExchange",
        "_InterlockedExchange64",
        "_InterlockedExchange8",
        "_InterlockedExchangeAdd",
        "_InterlockedExchangeAdd64",
        "_InterlockedIncrement64",
        "_InterlockedOr",
        "InitializeCriticalSectionEx",
        "ReplaceFileW",
        "TryEnterCriticalSection",
        "CommandLineToArgvW",
        "LoadLibraryW",
        "CoCreateInstance",
        "CoInitializeEx",
        "CoUninitialize",
        "CoTaskMemFree",
        "SafeArrayCreateVector",
        "SafeArrayPutElement",
        "SysAllocString",
        "SysAllocStringLen",
        "SysFreeString",
        "VariantInit",
        "SHCreateItemFromParsingName",
        "DragQueryFileW",
        "ImmGetCompositionStringW",
        "ImmGetContext",
        "ImmReleaseContext",
        "RegGetValueW",
        "SystemParametersInfoW",
        "_open_osfhandle",
        "_cexit",
        "_configure_narrow_argv",
        "_crt_at_quick_exit",
        "_crt_atexit",
        "_execute_onexit_table",
        "_initialize_narrow_environment",
        "_initialize_onexit_table",
        "_register_onexit_function",
        "_seh_filter_dll",
        "terminate",
        "_CrtDbgReport",
        "_CrtDbgReportW",
        "_fdclass",
        "_fdtest",
        "abs",
        "fmaxl",
        "fminl",
        "rand_s",
        "strcpy_s",
        "strcat_s",
        "sysinfo",
        "_chmod",
        "_wcsnicmp",
        "_wchmod",
        "_wsplitpath_s",
        "_wmakepath_s",
        "_stat64",
        "_fstat64",
        "_wstat64",
        "wcscpy_s",
        "?_OptionsStorage@?1??__local_stdio_printf_options@@9@9",
        "?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9",
    };

    for (const char *sym : kDynSymExact) {
        if (name == sym || stripped == sym)
            return true;
    }

    static const char *const kCommonDynPrefixes[] = {
        "__libc_",
        "__stack_chk_",
    };
    static const char *const kMacDynPrefixes[] = {
        "CF",
        "kCF",
        "CG",
        "kCG",
        "NS",
        "IOKit",
        "IOHID",
        "IOService",
        "IORegistryEntry",
        "objc_",
        "OBJC_",
        "_objc_",
        "UTType",
        "UTCopy",
        "AudioQueue",
        "AudioServices",
        "AudioComponent",
        "Sec",
        "kSec",
        "CC_",
        "mach_",
        "task_",
        "host_",
        "vm_",
        "kern_",
        "Unwind_",
        "dispatch_",
        "MTL",
        "AudioObject",
        "AudioDevice",
    };
    static const char *const kLinuxDynPrefixes[] = {
        "Unwind_",
        "__isoc23_",
        "__isoc99_",
        "inotify_",
        "pthread_",
        "snd_",
    };
    static const char *const kWindowsDynPrefixes[] = {
        "__imp_",
        "__vcrt_",
        "_Cnd_",
        "_Init_thread_",
        "_Mtx_",
        "_Query_perf_",
        "_Smtx_",
        "_Thrd_",
        "__std_",
    };

    for (const char *prefix : kCommonDynPrefixes) {
        size_t plen = 0;
        while (prefix[plen] != '\0')
            ++plen;
        if ((name.size() >= plen && name.compare(0, plen, prefix) == 0) ||
            (stripped.size() >= plen && stripped.compare(0, plen, prefix) == 0)) {
            return true;
        }
    }

    const char *const *platformPrefixes = nullptr;
    size_t prefixCount = 0;
    switch (platform) {
        case LinkPlatform::Linux:
            platformPrefixes = kLinuxDynPrefixes;
            prefixCount = sizeof(kLinuxDynPrefixes) / sizeof(kLinuxDynPrefixes[0]);
            break;
        case LinkPlatform::macOS:
            platformPrefixes = kMacDynPrefixes;
            prefixCount = sizeof(kMacDynPrefixes) / sizeof(kMacDynPrefixes[0]);
            break;
        case LinkPlatform::Windows:
            platformPrefixes = kWindowsDynPrefixes;
            prefixCount = sizeof(kWindowsDynPrefixes) / sizeof(kWindowsDynPrefixes[0]);
            break;
        default:
            break;
    }

    for (size_t i = 0; i < prefixCount; ++i) {
        const char *prefix = platformPrefixes[i];
        size_t plen = 0;
        while (prefix[plen] != '\0')
            ++plen;
        if ((name.size() >= plen && name.compare(0, plen, prefix) == 0) ||
            (stripped.size() >= plen && stripped.compare(0, plen, prefix) == 0)) {
            return true;
        }
    }

    if ((platform == LinkPlatform::macOS || platform == LinkPlatform::Linux) &&
        isKnownCppRuntimeDynamicSymbol(name))
        return true;

    if (platform == LinkPlatform::Linux) {
        // X11 (X + CamelCase, plus the Xutf8/Xkb/Xrm families) and OpenGL (gl +
        // CamelCase, including glX) come from libX11/libGL. Match them precisely
        // so a stray uppercase-X symbol or a gl-lowercase libc name (e.g. glob)
        // stays a hard link error rather than an unresolvable dynamic import.
        if ((stripped.size() >= 2 && stripped[0] == 'X' && stripped[1] >= 'A' &&
             stripped[1] <= 'Z') ||
            stripped.rfind("Xutf8", 0) == 0 || stripped.rfind("Xkb", 0) == 0 ||
            stripped.rfind("Xrm", 0) == 0)
            return true;
        if (stripped.size() > 2 && stripped[0] == 'g' && stripped[1] == 'l' && stripped[2] >= 'A' &&
            stripped[2] <= 'Z')
            return true;
    }

    if (platform == LinkPlatform::Windows) {
        if (isMsvcThreadSafeStaticGuardSymbol(name) || isMsvcThreadSafeStaticGuardSymbol(stripped))
            return false;
        if (name.find("@std@@") != std::string::npos ||
            stripped.find("@std@@") != std::string::npos || name.rfind("??", 0) == 0 ||
            stripped.rfind("??", 0) == 0 || name.rfind("?_", 0) == 0 ||
            stripped.rfind("?_", 0) == 0)
            return true;
    }

    return false;
}

} // namespace zanna::codegen::linker
