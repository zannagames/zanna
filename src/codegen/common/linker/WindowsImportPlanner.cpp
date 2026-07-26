//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/WindowsImportPlanner.cpp
// Purpose: Windows DLL import planning and thunk generation for the native
//          linker. Resolves each undefined dynamic symbol to a (DLL, function)
//          pair, builds the .idata$* import-directory sections, and synthesises
//          the corresponding ObjFile so SectionMerger can place them.
// Key invariants:
//   - Symbol table is fully baked in; no DLL probing on disk.
//   - When @p debugRuntime is set the planner picks ucrtbased.dll /
//     vcruntime140d.dll; otherwise the release pair is used.
//   - Generated thunks honour both the AArch64 and x64 calling conventions —
//     ABI-divergent routines (chkstk, security cookie) get arch-specific stubs.
// Ownership/Lifetime: stateless — caller owns the populated WindowsImportPlan.
// Links: codegen/common/linker/PlatformImportPlanner.hpp,
//        codegen/common/linker/PeExeWriter.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file WindowsImportPlanner.cpp
 * @brief Implements deterministic PE/COFF import mapping and thunk synthesis.
 *
 * The planner maps each permitted unresolved Windows symbol to a known DLL,
 * normalizes exported CRT spellings, and constructs a synthetic COFF object
 * containing IAT slots and architecture-specific jump thunks. No host DLL
 * probing is performed; the mapping is an explicit part of the linker policy.
 */

#include "codegen/common/linker/PlatformImportPlanner.hpp"

#include "codegen/common/linker/RelocConstants.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace zanna::codegen::linker {
namespace {

/// @brief Strip the COFF __imp_ prefix used for Windows IAT-thunk references.
/// @details Windows compilers prefix indirect imports with __imp_; the planner
///          looks up the underlying function by its bare name.
/// @param name Symbol spelling to normalize.
/// @return @p name without a leading `__imp_`, or unchanged when not indirect.
std::string stripImpPrefix(const std::string &name) {
    if (name.rfind("__imp_", 0) == 0)
        return name.substr(6);
    return name;
}

/// @brief Remove COFF symbol-decoration underscores for import classification.
/// @details Some object producers preserve one or more leading underscores on C
///          runtime and Windows API references. Import planning compares both the
///          raw and stripped spellings so decorated input still maps to the right
///          DLL or static-runtime bucket.
/// @param name Symbol spelling to normalize.
/// @return The suffix following all leading underscores.
std::string stripLeadingUnderscores(const std::string &name) {
    size_t i = 0;
    while (i < name.size() && name[i] == '_')
        ++i;
    return (i > 0) ? name.substr(i) : name;
}

/// @brief Identify libm-style symbols exported by the Windows Universal CRT.
/// @details The same names resolve through libm on Unix, but Windows native
///          binaries must place them in the UCRT import table.
/// @param name Bare or decorated function name to classify.
/// @return `true` when @p name belongs to the maintained UCRT math set.
bool isUcrtMathSymbol(const std::string &name) {
    static const std::unordered_set<std::string> kMath = {
        "acos",      "acosf",      "asin", "asinf", "atan",     "atan2",     "atan2f", "atanf",
        "cbrt",      "cbrtf",      "ceil", "ceilf", "copysign", "copysignf", "cos",    "cosf",
        "cosh",      "exp",        "expf", "exp2f", "fabs",     "fabsf",     "floor",  "floorf",
        "fmax",      "fmaxf",      "fmin", "fmaxl", "fminf",    "fminl",     "fmod",   "fmodf",
        "hypot",     "ldexp",      "log",  "log10", "log2",     "logf",      "lrint",  "lrintf",
        "nan",       "pow",        "powf", "round", "roundf",   "sin",       "sinf",   "sinh",
        "remainder", "remainderf", "sqrt", "sqrtf", "tan",      "tanf",      "tanh",   "trunc",
        "truncf",
    };
    return kMath.count(name) != 0;
}

/// @brief Check a prefix against both raw and underscore-stripped symbol spellings.
/// @param name     Original symbol spelling.
/// @param stripped Same symbol after leading underscore decoration is removed.
/// @param prefix   Prefix to test.
/// @return `true` when either spelling begins with @p prefix.
bool hasPrefixEither(const std::string &name, const std::string &stripped, const char *prefix) {
    return name.rfind(prefix, 0) == 0 || stripped.rfind(prefix, 0) == 0;
}

/// @brief Decide whether a symbol belongs to the statically supplied Windows CRT/compiler set.
/// @details These helper names are satisfied by generated linker support objects
///          or local runtime archives. Treating them as DLL imports would emit an
///          invalid import table entry and leave the actual static helper unused.
/// @param name Original symbol spelling.
/// @param stripped Same symbol with leading underscores removed.
/// @return `true` when the symbol must be resolved from static support code.
bool isWindowsStaticCompilerRuntimeSymbol(const std::string &name, const std::string &stripped) {
    return name == "__RTC_memset" || stripped == "RTC_memset" || name == "__security_pop_cookie" ||
           stripped == "security_pop_cookie" || name == "__security_push_cookie" ||
           stripped == "security_push_cookie" || hasPrefixEither(name, stripped, "_Interlocked") ||
           hasPrefixEither(name, stripped, "Interlocked");
}

/// @brief Map a Windows import symbol to the DLL that exports it.
/// @details Checks explicit Win32 API sets, compiler/CRT helper families, C++
///          decorated names, UCRT functions, and legacy MSVCRT exports. Debug
///          mode selects the corresponding debug CRT DLL where one exists.
/// @param name Function name after removing any `__imp_` indirection prefix.
/// @param debugRuntime Whether debug CRT variants should be selected.
/// @param dllName Receives the canonical DLL filename on success.
/// @return `true` when @p name has a maintained DLL mapping.
bool dllForImport(const std::string &name, bool debugRuntime, std::string &dllName) {
    const std::string stripped = stripLeadingUnderscores(name);

    static const std::unordered_set<std::string> kernel32 = {
        "ExitProcess",
        "FreeLibrary",
        "LoadLibraryW",
        "GetCurrentProcessId",
        "GetCurrentThreadId",
        "GetEnvironmentVariableA",
        "GetEnvironmentVariableW",
        "GetActiveProcessorCount",
        "GetLastError",
        "GetCommandLineW",
        "GetComputerNameA",
        "GetComputerNameW",
        "GetCurrentDirectoryW",
        "GetFullPathNameA",
        "GetModuleHandleW",
        "GetModuleFileNameA",
        "GetModuleFileNameW",
        "GetProcAddress",
        "GetProcessHeap",
        "GetStartupInfoW",
        "GetStdHandle",
        "GetSystemInfo",
        "GetSystemTimeAsFileTime",
        "GetTempPathA",
        "GetWindowsDirectoryW",
        "HeapAlloc",
        "HeapFree",
        "IsDebuggerPresent",
        "InitOnceExecuteOnce",
        "InitializeCriticalSection",
        "InitializeCriticalSectionAndSpinCount",
        "InitializeCriticalSectionEx",
        "InitializeSListHead",
        "LeaveCriticalSection",
        "DeleteCriticalSection",
        "EnterCriticalSection",
        "TryEnterCriticalSection",
        "MultiByteToWideChar",
        "OutputDebugStringA",
        "PeekNamedPipe",
        "RaiseException",
        "SetEnvironmentVariableA",
        "SetEnvironmentVariableW",
        "SetLastError",
        "SetUnhandledExceptionFilter",
        "SetErrorMode",
        "SetCurrentDirectoryW",
        "SwitchToThread",
        "TerminateProcess",
        "WriteFile",
        "GetTickCount",
        "GetTickCount64",
        "AddVectoredExceptionHandler",
        "GlobalAlloc",
        "GlobalFree",
        "GlobalLock",
        "GlobalSize",
        "GlobalUnlock",
        "InitializeSRWLock",
        "AcquireSRWLockExclusive",
        "AcquireSRWLockShared",
        "ReleaseSRWLockExclusive",
        "ReleaseSRWLockShared",
        "AreFileApisANSI",
        "QueryPerformanceCounter",
        "VirtualQuery",
        "WideCharToMultiByte",
        "Beep",
        "CloseHandle",
        "CancelIo",
        "CopyFile2",
        "CreateEventA",
        "CreateEventW",
        "CreateDirectoryExW",
        "CreateDirectoryW",
        "CreateFileA",
        "CreateFile2",
        "CreateFileW",
        "CreateHardLinkW",
        "CreatePipe",
        "CreateProcessA",
        "CreateProcessW",
        "CreateSymbolicLinkW",
        "CreateThread",
        "CreateWaitableTimerExW",
        "DeleteProcThreadAttributeList",
        "DeleteFileW",
        "DeviceIoControl",
        "FindClose",
        "FindFirstFileA",
        "FindFirstFileExW",
        "FindFirstFileW",
        "FindNextFileA",
        "FindNextFileW",
        "FormatMessageA",
        "GetFileAttributesA",
        "GetFileAttributesW",
        "GetFileAttributesExW",
        "GetDiskFreeSpaceExW",
        "GetFileSizeEx",
        "GetConsoleMode",
        "GetExitCodeProcess",
        "GetFileInformationByHandle",
        "GetFileInformationByHandleEx",
        "GetFinalPathNameByHandleW",
        "GetFullPathNameW",
        "GetLocaleInfoEx",
        "GetLogicalDrives",
        "GetOverlappedResult",
        "GetThreadId",
        "GetUserDefaultLocaleName",
        "GetVersionExA",
        "GetVersionExW",
        "GlobalMemoryStatusEx",
        "FlushFileBuffers",
        "LocalFree",
        "LockFileEx",
        "InitializeConditionVariable",
        "InitializeProcThreadAttributeList",
        "QueryPerformanceFrequency",
        "ReadDirectoryChangesW",
        "ReadFile",
        "ReplaceFileW",
        "MoveFileExA",
        "MoveFileExW",
        "RemoveDirectoryW",
        "SetConsoleCP",
        "SetConsoleCtrlHandler",
        "SetConsoleMode",
        "SetConsoleOutputCP",
        "SetFileAttributesW",
        "SetFileInformationByHandle",
        "SetFileTime",
        "ResetEvent",
        "SetEvent",
        "SetHandleInformation",
        "SetWaitableTimer",
        "Sleep",
        "SleepConditionVariableCS",
        "SleepConditionVariableSRW",
        "TryAcquireSRWLockExclusive",
        "TryAcquireSRWLockShared",
        "UnlockFileEx",
        "UpdateProcThreadAttribute",
        "WaitForMultipleObjects",
        "WaitForSingleObject",
        "WakeAllConditionVariable",
        "WakeConditionVariable",
        "GetTempPathW",
    };
    static const std::unordered_set<std::string> user32 = {
        "AdjustWindowRect",
        "AdjustWindowRectEx",
        "BeginPaint",
        "BringWindowToTop",
        "ClientToScreen",
        "ClipCursor",
        "CloseClipboard",
        "CreateWindowExW",
        "DefWindowProcW",
        "DestroyWindow",
        "DispatchMessageW",
        "EmptyClipboard",
        "EndPaint",
        "GetCapture",
        "GetClassInfoExW",
        "GetClipboardData",
        "GetClientRect",
        "GetDC",
        "GetKeyState",
        "GetMonitorInfoA",
        "GetRawInputData",
        "GetSystemMetrics",
        "GetWindowLongA",
        "GetWindowLongW",
        "GetWindowLongPtrA",
        "GetWindowRect",
        "IsClipboardFormatAvailable",
        "IsIconic",
        "IsWindow",
        "IsZoomed",
        "LoadCursorA",
        "LoadIconW",
        "LoadImageW",
        "MonitorFromPoint",
        "MonitorFromWindow",
        "OpenClipboard",
        "PeekMessageW",
        "MsgWaitForMultipleObjectsEx",
        "RegisterClassExW",
        "RegisterClipboardFormatW",
        "RegisterRawInputDevices",
        "ReleaseCapture",
        "ReleaseDC",
        "SetClipboardData",
        "SetCapture",
        "SetCursor",
        "SetCursorPos",
        "SetFocus",
        "ScreenToClient",
        "SetForegroundWindow",
        "SetWindowLongA",
        "SetWindowLongW",
        "SetWindowLongPtrW",
        "SetWindowPos",
        "SetWindowTextW",
        "ShowCursor",
        "ShowWindow",
        "SystemParametersInfoW",
        "TranslateMessage",
        "UpdateWindow",
    };
    static const std::unordered_set<std::string> gdi32 = {
        "CreateCompatibleDC",
        "CreateDIBSection",
        "DeleteDC",
        "DeleteObject",
        "GetDeviceCaps",
        "GetStockObject",
        "SelectObject",
        "StretchBlt",
    };
    static const std::unordered_set<std::string> imm32 = {
        "ImmGetCompositionStringW", "ImmGetContext", "ImmReleaseContext"};
    static const std::unordered_set<std::string> shell32 = {
        "CommandLineToArgvW",
        "DragAcceptFiles",
        "DragFinish",
        "DragQueryFileA",
        "DragQueryFileW",
        "SHCreateItemFromParsingName",
    };
    static const std::unordered_set<std::string> ole32 = {
        "CoCreateInstance", "CoInitializeEx", "CoTaskMemFree", "CoUninitialize"};
    static const std::unordered_set<std::string> oleaut32 = {
        "SafeArrayCreateVector",
        "SafeArrayPutElement",
        "SysAllocString",
        "SysAllocStringLen",
        "SysFreeString",
        "VariantInit",
    };
    static const std::unordered_set<std::string> xinput = {"XInputGetState", "XInputSetState"};
    static const std::unordered_set<std::string> advapi32 = {
        "CryptAcquireContextA",
        "CryptAcquireContextW",
        "CryptAcquireContext",
        "CryptGenRandom",
        "CryptCreateHash",
        "CryptDestroyHash",
        "CryptDestroyKey",
        "CryptReleaseContext",
        "CryptSetHashParam",
        "CryptVerifySignature",
        "CryptVerifySignatureA",
        "GetUserNameA",
        "GetUserNameW",
        "RegGetValueW",
    };
    static const std::unordered_set<std::string> bcrypt = {
        "BCryptDestroyKey",
        "BCryptGenRandom",
        "BCryptVerifySignature",
    };
    static const std::unordered_set<std::string> ws2_32 = {
        "WSACleanup",  "WSAStartup",  "WSAGetLastError", "accept",      "bind",
        "closesocket", "connect",     "freeaddrinfo",    "getaddrinfo", "gethostname",
        "getnameinfo", "getsockname", "htonl",           "htons",       "inet_ntop",
        "inet_pton",   "ioctlsocket", "getsockopt",      "listen",      "ntohl",
        "ntohs",       "recv",        "recvfrom",        "select",      "send",
        "sendto",      "setsockopt",  "shutdown",        "socket",
    };
    static const std::unordered_set<std::string> iphlpapi = {"GetAdaptersAddresses"};
    static const std::unordered_set<std::string> crypt32 = {
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
        "CryptImportPublicKeyInfo",
        "CryptImportPublicKeyInfoEx2",
        "CryptStringToBinaryA",
    };
    static const std::unordered_set<std::string> d3d11 = {"D3D11CreateDevice",
                                                          "D3D11CreateDeviceAndSwapChain"};
    static const std::unordered_set<std::string> d3dcompiler = {
        "D3DCompile", "D3DCompile2", "D3DCompileFromFile", "D3DReflect"};
    static const std::unordered_set<std::string> ucrt = {
        "_Exit",
        "_exit",
        "__acrt_iob_func",
        "___lc_codepage_func",
        "acrt_iob_func",
        "__local_stdio_printf_options",
        "__local_stdio_scanf_options",
        "__stdio_common_vfprintf",
        "stdio_common_vfprintf",
        "__stdio_common_vsprintf",
        "__stdio_common_vsprintf_s",
        "__stdio_common_vswprintf",
        "stdio_common_vsprintf",
        "stdio_common_vsprintf_s",
        "stdio_common_vswprintf",
        "__stdio_common_vsscanf",
        "stdio_common_vsscanf",
        "_vfprintf_l",
        "_vsscanf_l",
        "abort",
        "access",
        "abs",
        "aligned_free",
        "aligned_malloc",
        "aligned_alloc",
        "atexit",
        "atof",
        "atoi",
        "atol",
        "atoll",
        "beginthreadex",
        "_beginthreadex",
        "bsearch",
        "_callnewh",
        "calloc_dbg",
        "calloc",
        "callnewh",
        "cbrt",
        "cbrtf",
        "ceil",
        "ceilf",
        "clearerr",
        "clock",
        "close",
        "commit",
        "_commit",
        "cos",
        "cosf",
        "create_locale",
        "_cexit",
        "_configure_narrow_argv",
        "_crt_atexit",
        "_crt_at_quick_exit",
        "dclass",
        "difftime64",
        "_difftime64",
        "_invalid_parameter",
        "invalid_parameter",
        "dsign",
        "dtest",
        "_dtest",
        "fdsign",
        "_initialize_narrow_environment",
        "_initialize_onexit_table",
        "ldsign",
        "dup",
        "dup2",
        "errno",
        "_execute_onexit_table",
        "exp2f",
        "exit",
        "fabs",
        "fabsf",
        "fdclass",
        "_fdtest",
        "fclose",
        "fcntl",
        "ferror",
        "feof",
        "fflush",
        "fgetc",
        "fgets",
        "fileno",
        "fmax",
        "fdopen",
        "_fdopen",
        "fgetpos",
        "fmaxf",
        "fmaxl",
        "fmin",
        "fminf",
        "fminl",
        "floor",
        "floorf",
        "fmod",
        "fmodf",
        "fopen",
        "fprintf",
        "fputc",
        "fputs",
        "fread",
        "free",
        "free_locale",
        "freopen",
        "fseek",
        "fsetpos",
        "fstat64i32",
        "_fstat64",
        "ftell",
        "fwrite",
        "getc",
        "getcwd",
        "getenv",
        "_get_osfhandle",
        "_wgetenv",
        "getch",
        "get_stream_buffer_pointers",
        "_get_stream_buffer_pointers",
        "getpid",
        "hypot",
        "isalnum",
        "isalpha",
        "isdigit",
        "islower",
        "isspace",
        "isupper",
        "isxdigit",
        "isatty",
        "kbhit",
        "lround",
        "llround",
        "localeconv",
        "lock_file",
        "_lock_file",
        "log",
        "log10",
        "log2",
        "log2f",
        "logf",
        "lrint",
        "lrintf",
        "longjmp",
        "lseek",
        "malloc",
        "memchr",
        "memcmp",
        "memcpy",
        "memmove",
        "memset",
        "modf",
        "nan",
        "nearbyint",
        "open",
        "perror",
        "pclose",
        "posix_memalign",
        "popen",
        "pow",
        "powf",
        "printf",
        "putc",
        "puts",
        "qsort",
        "raise",
        "read",
        "realloc",
        "remove",
        "_register_onexit_function",
        "rename",
        "rewind",
        "rint",
        "round",
        "roundf",
        "setbuf",
        "setenv",
        "setjmp",
        "setvbuf",
        "sin",
        "sinf",
        "snprintf",
        "sprintf",
        "sqrt",
        "sqrtf",
        "sscanf",
        "_seh_filter_dll",
        "strcat",
        "strcat_s",
        "strchr",
        "strcmp",
        "strcpy",
        "strcpy_s",
        "strdup",
        "strerror",
        "stricmp",
        "strlen",
        "strnlen",
        "strncat",
        "strnicmp",
        "strncmp",
        "strncpy",
        "strpbrk",
        "strstr",
        "strtod",
        "strtod_l",
        "strtol",
        "strtoll",
        "strtok_s",
        "strtoul",
        "strrchr",
        "strftime",
        "system",
        "tan",
        "tanf",
        "time",
        "time64",
        "terminate",
        "tmpfile",
        "tmpnam",
        "tolower",
        "toupper",
        "trunc",
        "truncf",
        "ungetc",
        "unlink",
        "unlock_file",
        "_unlock_file",
        "utime64",
        "_wutime64",
        "vfprintf",
        "wcscmp",
        "wcscpy",
        "wcslen",
        "wcsncmp",
        "wcscpy_s",
        "write",
        "_wfopen",
        "wfopen",
        "_wsopen_s",
        "_wmakepath_s",
        "_wmkdir",
        "wmkdir",
        "_open_osfhandle",
        "open_osfhandle",
        "_wopen",
        "wopen",
        "_wremove",
        "wremove",
        "_wsplitpath_s",
        "_wunlink",
        "wunlink",
        "fseeki64",
        "ftelli64",
        "gmtime64_s",
        "localtime64_s",
        "lseeki64",
        "mktime64",
        "rand_s",
        "set_abort_behavior",
        "stat64i32",
        "_stat64i32",
        "_stat64",
        "wstat64i32",
        "_wstat64i32",
        "_wstat64",
        "wassert",
        "_byteswap_uint64",
        "byteswap_uint64",
        "_rotl",
        "_rotl64",
        "_rotr",
        "_rotr64",
        "_wcsnicmp",
        "wcsnicmp",
        "_chmod",
        "chmod",
        "_wchmod",
        "wchmod",
    };
    static const std::unordered_set<std::string> debugOnlyUcrt = {"_CrtDbgReport",
                                                                  "_CrtDbgReportW",
                                                                  "CrtDbgReport",
                                                                  "_calloc_dbg",
                                                                  "calloc_dbg",
                                                                  "_free_dbg",
                                                                  "free_dbg",
                                                                  "_malloc_dbg",
                                                                  "malloc_dbg",
                                                                  "_realloc_dbg",
                                                                  "realloc_dbg"};
    static const std::unordered_set<std::string> msvcrt = {"_setjmpex", "setjmpex"};

    if (kernel32.count(name) || kernel32.count(stripped)) {
        dllName = "kernel32.dll";
        return true;
    }
    if (user32.count(name) || user32.count(stripped)) {
        dllName = "user32.dll";
        return true;
    }
    if (gdi32.count(name) || gdi32.count(stripped)) {
        dllName = "gdi32.dll";
        return true;
    }
    if (imm32.count(name) || imm32.count(stripped)) {
        dllName = "imm32.dll";
        return true;
    }
    if (shell32.count(name) || shell32.count(stripped)) {
        dllName = "shell32.dll";
        return true;
    }
    if (ole32.count(name) || ole32.count(stripped)) {
        dllName = "ole32.dll";
        return true;
    }
    if (oleaut32.count(name) || oleaut32.count(stripped)) {
        dllName = "oleaut32.dll";
        return true;
    }
    if (xinput.count(name) || xinput.count(stripped)) {
        dllName = "xinput1_4.dll";
        return true;
    }
    if (advapi32.count(name) || advapi32.count(stripped)) {
        dllName = "advapi32.dll";
        return true;
    }
    if (bcrypt.count(name) || bcrypt.count(stripped)) {
        dllName = "bcrypt.dll";
        return true;
    }
    if (ws2_32.count(name) || ws2_32.count(stripped)) {
        dllName = "ws2_32.dll";
        return true;
    }
    if (iphlpapi.count(name) || iphlpapi.count(stripped)) {
        dllName = "iphlpapi.dll";
        return true;
    }
    if (crypt32.count(name) || crypt32.count(stripped)) {
        dllName = "crypt32.dll";
        return true;
    }
    if (d3d11.count(name) || d3d11.count(stripped)) {
        dllName = "d3d11.dll";
        return true;
    }
    if (d3dcompiler.count(name) || d3dcompiler.count(stripped)) {
        dllName = "d3dcompiler_47.dll";
        return true;
    }

    if (name == "__C_specific_handler" || name == "__C_specific_handler_noexcept" ||
        name == "__current_exception" || name == "__current_exception_context" ||
        name == "__RTDynamicCast" || name == "_CxxThrowException" || name == "__CxxFrameHandler3" ||
        name == "__CxxFrameHandler4" || name == "__std_exception_copy" ||
        name == "__std_exception_destroy" || name == "__std_type_info_compare" ||
        name == "__std_type_info_destroy_list" || name == "__std_type_info_hash" ||
        name == "__std_type_info_name" || name == "__std_terminate" ||
        name == "__intrinsic_setjmp" || name == "__intrinsic_setjmpex" || name == "_purecall" ||
        stripped == "__C_specific_handler" || stripped == "__C_specific_handler_noexcept" ||
        stripped == "__current_exception" || stripped == "__current_exception_context" ||
        stripped == "RTDynamicCast" || stripped == "CxxThrowException" ||
        stripped == "CxxFrameHandler3" || stripped == "CxxFrameHandler4" ||
        stripped == "std_exception_copy" || stripped == "std_exception_destroy" ||
        stripped == "std_type_info_compare" || stripped == "std_type_info_destroy_list" ||
        stripped == "std_type_info_hash" || stripped == "std_type_info_name" ||
        stripped == "std_terminate" || stripped == "intrinsic_setjmp" ||
        stripped == "intrinsic_setjmpex" || stripped == "purecall" ||
        hasPrefixEither(name, stripped, "_Init_thread_") ||
        hasPrefixEither(name, stripped, "Init_thread_") || name.rfind("__vcrt_", 0) == 0 ||
        stripped.rfind("__vcrt_", 0) == 0) {
        dllName = debugRuntime ? "VCRUNTIME140D.dll" : "VCRUNTIME140.dll";
        return true;
    }

    if (hasPrefixEither(name, stripped, "_Cnd_") || hasPrefixEither(name, stripped, "Cnd_") ||
        hasPrefixEither(name, stripped, "_Mtx_") || hasPrefixEither(name, stripped, "Mtx_") ||
        hasPrefixEither(name, stripped, "_Query_perf_") ||
        hasPrefixEither(name, stripped, "Query_perf_") ||
        hasPrefixEither(name, stripped, "_Smtx_") || hasPrefixEither(name, stripped, "Smtx_") ||
        hasPrefixEither(name, stripped, "_Thrd_") || hasPrefixEither(name, stripped, "Thrd_")) {
        dllName = debugRuntime ? "MSVCP140D.dll" : "MSVCP140.dll";
        return true;
    }

    if (hasPrefixEither(name, stripped, "__std_smf_") ||
        hasPrefixEither(name, stripped, "std_smf_")) {
        dllName = debugRuntime ? "MSVCP140_2D.dll" : "MSVCP140_2.dll";
        return true;
    }

    if (hasPrefixEither(name, stripped, "__std_atomic_") ||
        hasPrefixEither(name, stripped, "std_atomic_") ||
        hasPrefixEither(name, stripped, "__std_tzdb_") ||
        hasPrefixEither(name, stripped, "std_tzdb_") ||
        name == "__std_acquire_shared_mutex_for_instance" ||
        stripped == "std_acquire_shared_mutex_for_instance" ||
        name == "__std_release_shared_mutex_for_instance" ||
        stripped == "std_release_shared_mutex_for_instance" ||
        name == "__std_parallel_algorithms_hw_threads" ||
        stripped == "std_parallel_algorithms_hw_threads" ||
        hasPrefixEither(name, stripped, "__std_execution_") ||
        hasPrefixEither(name, stripped, "std_execution_") ||
        name == "__std_wait_for_threadpool_work_callbacks" ||
        stripped == "std_wait_for_threadpool_work_callbacks") {
        dllName = debugRuntime ? "MSVCP140D_ATOMIC_WAIT.dll" : "MSVCP140_ATOMIC_WAIT.dll";
        return true;
    }

    if (name.find("@std@@") != std::string::npos || stripped.find("@std@@") != std::string::npos ||
        name.rfind("??", 0) == 0 || stripped.rfind("??", 0) == 0 || name.rfind("?_", 0) == 0 ||
        stripped.rfind("?_", 0) == 0) {
        dllName = debugRuntime ? "MSVCP140D.dll" : "MSVCP140.dll";
        return true;
    }

    if (isUcrtMathSymbol(name) || isUcrtMathSymbol(stripped) || ucrt.count(name) ||
        ucrt.count(stripped)) {
        dllName = debugRuntime ? "ucrtbased.dll" : "ucrtbase.dll";
        return true;
    }
    if (debugRuntime && (debugOnlyUcrt.count(name) || debugOnlyUcrt.count(stripped))) {
        dllName = "ucrtbased.dll";
        return true;
    }
    if (msvcrt.count(name) || msvcrt.count(stripped)) {
        dllName = "msvcrt.dll";
        return true;
    }
    return false;
}

/// @brief Translate a source-level CRT spelling to its exported import name.
/// @param name Resolved symbol spelling used by input objects and linker thunks.
/// @return The DLL export spelling, or @p name when no remapping is required.
std::string importNameForSymbol(const std::string &name) {
    static const std::unordered_map<std::string, std::string> remap = {
        {"atexit", "_crt_atexit"},
        {"close", "_close"},
        {"lseek", "_lseek"},
        {"_longjmp", "longjmp"},
        {"open", "_open"},
        {"read", "_read"},
        {"_setjmp", "setjmp"},
        {"strdup", "_strdup"},
        {"unlink", "_unlink"},
        {"write", "_write"},
    };
    auto it = remap.find(name);
    if (it != remap.end())
        return it->second;
    return name;
}

/// @brief Checked addition for synthetic import-section byte offsets.
/// @details The Windows import planner emits an in-memory COFF object. Its
///          section offsets are later consumed as relocation patch sites, so
///          offset arithmetic is validated before resizing the backing vectors.
/// @param lhs Current byte offset or section size.
/// @param rhs Number of bytes to append.
/// @param out Receives the sum on success.
/// @return `true` when the addition fits in `size_t`.
bool checkedImportSizeAdd(size_t lhs, size_t rhs, size_t &out) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs)
        return false;
    out = lhs + rhs;
    return true;
}

/// @brief Narrow a synthetic import symbol index to COFF's 32-bit field.
/// @details Returns false with a diagnostic instead of truncating if an
///          unexpectedly large import plan would exceed the object format's
///          symbol-index capacity.
/// @param index Native-size index to narrow.
/// @param name Symbol being added, used in any diagnostic.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the COFF-width index on success.
/// @return `true` when @p index is representable by `uint32_t`.
bool checkedImportSymbolIndex(size_t index,
                              const std::string &name,
                              std::ostream &err,
                              uint32_t &out) {
    if (index > std::numeric_limits<uint32_t>::max()) {
        err << "error: Windows import symbol table index overflow while adding '" << name << "'\n";
        return false;
    }
    out = static_cast<uint32_t>(index);
    return true;
}

} // namespace

/// @copydoc zanna::codegen::linker::generateWindowsImports
bool generateWindowsImports(LinkArch arch,
                            const std::unordered_set<std::string> &dynamicSyms,
                            bool debugRuntime,
                            WindowsImportPlan &plan,
                            std::ostream &err) {
    plan = WindowsImportPlan{};
    plan.obj.name = (arch == LinkArch::AArch64) ? "<winarm64-imports>" : "<win64-imports>";
    plan.obj.synthetic = true;
    plan.obj.format = ObjFileFormat::COFF;
    plan.obj.is64bit = true;
    plan.obj.isLittleEndian = true;
    plan.obj.machine = (arch == LinkArch::AArch64) ? 0xAA64 : 0x8664;
    plan.obj.sections.push_back(ObjSection{});
    plan.obj.symbols.push_back(ObjSymbol{});

    ObjSection textSec;
    textSec.name = ".text";
    textSec.executable = true;
    textSec.alloc = true;
    textSec.alignment = 16;

    ObjSection dataSec;
    dataSec.name = ".data";
    dataSec.writable = true;
    dataSec.alloc = true;
    dataSec.alignment = 8;

    std::unordered_map<std::string, std::vector<std::string>> dllToFuncs;
    std::unordered_set<std::string> seenFuncs;
    bool mappedAllImports = true;
    for (const auto &sym : dynamicSyms) {
        if (sym == "__ImageBase")
            continue;
        const std::string base = stripImpPrefix(sym);
        if (isWindowsLinkerHelperSymbol(sym) || isWindowsLinkerHelperSymbol(base))
            continue;
        const std::string strippedBase = stripLeadingUnderscores(base);
        if (isWindowsStaticCompilerRuntimeSymbol(base, strippedBase))
            continue;
        if (base.rfind("rt_", 0) == 0)
            continue;
        if (!seenFuncs.insert(base).second)
            continue;
        std::string dllName;
        if (!dllForImport(base, debugRuntime, dllName)) {
            err << "error: Windows import symbol '" << base
                << "' is unresolved but has no DLL mapping\n";
            mappedAllImports = false;
            continue;
        }
        dllToFuncs[dllName].push_back(base);
    }
    if (!mappedAllImports)
        return false;

    std::vector<std::string> dllNames;
    dllNames.reserve(dllToFuncs.size());
    for (const auto &[dll, _] : dllToFuncs)
        dllNames.push_back(dll);
    std::sort(dllNames.begin(), dllNames.end());

    for (const auto &dll : dllNames) {
        auto funcs = dllToFuncs[dll];
        std::sort(funcs.begin(), funcs.end());
        DllImport dllImport;
        dllImport.dllName = dll;
        dllImport.functions = funcs;
        for (const auto &fn : funcs) {
            const std::string importName = importNameForSymbol(fn);
            if (importName != fn)
                dllImport.importNames.emplace(fn, importName);
        }
        plan.imports.push_back(std::move(dllImport));

        for (const auto &fn : funcs) {
            const size_t slotOff = dataSec.data.size();
            size_t slotEnd = 0;
            if (!checkedImportSizeAdd(slotOff, 8, slotEnd)) {
                err << "error: Windows import slot for '" << fn << "' overflows addressable size\n";
                return false;
            }
            dataSec.data.resize(slotEnd, 0);

            ObjSymbol slotSym;
            slotSym.name = "__imp_" + fn;
            slotSym.binding = ObjSymbol::Global;
            slotSym.sectionIndex = 2;
            slotSym.offset = slotOff;
            uint32_t slotSymIdx = 0;
            if (!checkedImportSymbolIndex(plan.obj.symbols.size(), slotSym.name, err, slotSymIdx))
                return false;
            plan.obj.symbols.push_back(std::move(slotSym));

            const size_t stubOff = textSec.data.size();
            if (arch == LinkArch::AArch64) {
                textSec.data.insert(
                    textSec.data.end(),
                    {0x10, 0x00, 0x00, 0x90, 0x10, 0x02, 0x40, 0xF9, 0x00, 0x02, 0x1F, 0xD6});
            } else {
                textSec.data.insert(textSec.data.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
            }

            ObjSymbol stubSym;
            stubSym.name = fn;
            stubSym.binding = ObjSymbol::Global;
            stubSym.sectionIndex = 1;
            stubSym.offset = stubOff;
            plan.obj.symbols.push_back(std::move(stubSym));

            if (arch == LinkArch::AArch64) {
                ObjReloc pageReloc;
                pageReloc.offset = stubOff + 0;
                pageReloc.type = coff_a64::kPageRel21;
                pageReloc.symIndex = slotSymIdx;
                pageReloc.addend = 0;
                textSec.relocs.push_back(pageReloc);

                ObjReloc loadReloc;
                if (!checkedImportSizeAdd(stubOff, 4, loadReloc.offset)) {
                    err << "error: Windows ARM64 import load relocation for '" << fn
                        << "' overflows addressable size\n";
                    return false;
                }
                loadReloc.type = coff_a64::kPageOff12L;
                loadReloc.symIndex = slotSymIdx;
                loadReloc.addend = 0;
                textSec.relocs.push_back(loadReloc);
            } else {
                ObjReloc reloc;
                if (!checkedImportSizeAdd(stubOff, 2, reloc.offset)) {
                    err << "error: Windows x64 import jump relocation for '" << fn
                        << "' overflows addressable size\n";
                    return false;
                }
                reloc.type = coff_x64::kRel32;
                reloc.symIndex = slotSymIdx;
                reloc.addend = 0;
                textSec.relocs.push_back(reloc);
            }
        }

        size_t terminatorEnd = 0;
        if (!checkedImportSizeAdd(dataSec.data.size(), 8, terminatorEnd)) {
            err << "error: Windows import descriptor terminator overflows addressable size\n";
            return false;
        }
        dataSec.data.resize(terminatorEnd, 0);
    }

    if (!textSec.data.empty())
        plan.obj.sections.push_back(std::move(textSec));
    if (!dataSec.data.empty()) {
        if (plan.obj.sections.size() == 1)
            plan.obj.sections.push_back(ObjSection{});
        plan.obj.sections.push_back(std::move(dataSec));
    }

    return true;
}

} // namespace zanna::codegen::linker
